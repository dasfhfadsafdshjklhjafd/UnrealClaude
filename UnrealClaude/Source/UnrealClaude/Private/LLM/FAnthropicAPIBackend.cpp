// Copyright Natali Caggiano. All Rights Reserved.

#include "FAnthropicAPIBackend.h"
#include "LLM/LLMBackendRegistry.h"
#include "UnrealClaudeModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Async/Async.h"

const FString FAnthropicAPIBackend::APIEndpoint = TEXT("https://api.anthropic.com/v1/messages");
const FString FAnthropicAPIBackend::APIVersion = TEXT("2023-06-01");

FAnthropicAPIBackend::FAnthropicAPIBackend()
{
}

FAnthropicAPIBackend::~FAnthropicAPIBackend()
{
	Cancel();
	Sessions.Empty();
}

// ============================================================================
// Capabilities
// ============================================================================

ELLMBackendCapability FAnthropicAPIBackend::GetCapabilities() const
{
	return ELLMBackendCapability::ToolCalling
		| ELLMBackendCapability::ImageInput
		| ELLMBackendCapability::Streaming
		| ELLMBackendCapability::CostReporting;
}

bool FAnthropicAPIBackend::IsAvailable() const
{
	return !GetAPIKey().IsEmpty();
}

FString FAnthropicAPIBackend::GetUnavailableReason() const
{
	if (IsAvailable())
	{
		return FString();
	}
	return TEXT("Anthropic API key not configured. Set it in the backend settings.");
}

FString FAnthropicAPIBackend::GetAPIKey() const
{
	return FLLMBackendRegistry::LoadAPIKey(TEXT("anthropic-api"));
}

// ============================================================================
// Session Management
// ============================================================================

FLLMSessionHandle FAnthropicAPIBackend::CreateTaskSession(const FString& SystemPrompt)
{
	FLLMSessionHandle Handle = FLLMSessionHandle::Create();
	Sessions.Add(Handle.SessionId, MakeShared<FLLMSessionState>(SystemPrompt));

	UE_LOG(LogUnrealClaude, Log, TEXT("FAnthropicAPIBackend: Created session %s"), *Handle.SessionId.ToString());
	return Handle;
}

void FAnthropicAPIBackend::AppendContext(const FLLMSessionHandle& Session, const FString& ContextText)
{
	TSharedPtr<FLLMSessionState>* Found = Sessions.Find(Session.SessionId);
	if (Found && Found->IsValid())
	{
		(*Found)->AppendContext(ContextText);
	}
}

void FAnthropicAPIBackend::DestroySession(const FLLMSessionHandle& Session)
{
	Sessions.Remove(Session.SessionId);
}

// ============================================================================
// Execution
// ============================================================================

bool FAnthropicAPIBackend::SubmitTurn(
	const FLLMSessionHandle& Session,
	const FString& UserMessage,
	const TArray<FString>& ImagePaths,
	const FString& ModelId,
	FOnLLMTurnComplete OnComplete,
	FOnLLMStreamProgress OnProgress)
{
	TSharedPtr<FLLMSessionState>* Found = Sessions.Find(Session.SessionId);
	if (!Found || !Found->IsValid())
	{
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(FLLMTurnResult::Error(TEXT("Invalid session handle")));
		}
		return false;
	}

	if (IsBusy())
	{
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(FLLMTurnResult::Error(TEXT("Backend is busy")));
		}
		return false;
	}

	FString APIKey = GetAPIKey();
	if (APIKey.IsEmpty())
	{
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(FLLMTurnResult::Error(TEXT("Anthropic API key not configured")));
		}
		return false;
	}

	FLLMSessionState& State = **Found;

	// Add user message to session history
	State.AddUserMessage(UserMessage, ImagePaths);

	// Accumulate response data across tool-call iterations
	TSharedPtr<FLLMTokenUsage> AccumulatedUsage = MakeShared<FLLMTokenUsage>();
	AccumulatedUsage->ModelId = ModelId;
	AccumulatedUsage->ProviderId = TEXT("anthropic-api");

	TSharedPtr<FString> AccumulatedText = MakeShared<FString>();

	// Start the request (may loop for tool calls)
	SendRequest(Session.SessionId, ModelId, OnComplete, OnProgress, AccumulatedUsage, AccumulatedText, 0);

	return true;
}

// ============================================================================
// SSE Streaming
// ============================================================================

namespace
{
	/** Per-request SSE parse state, shared between OnRequestProgress and OnProcessRequestComplete */
	struct FAnthropicSSEState
	{
		FString LineBuffer;
		int32 BytesProcessed = 0;

		// Current content block being streamed
		FString CurrentBlockType;   // "text" or "tool_use"
		FString CurrentBlockId;
		FString CurrentBlockName;
		FString CurrentBlockText;   // accumulated text_delta chunks
		FString CurrentBlockInput;  // accumulated input_json_delta chunks

		// Completed content for building the assistant tool message
		TArray<TSharedPtr<FJsonValue>> ContentBlocks;
		TArray<FLLMToolCaller::FToolCall> CompletedToolCalls;

		// Stream end state (set by message_delta / message_stop)
		FString StopReason;
		bool bStreamComplete = false;
	};
}

void FAnthropicAPIBackend::SendRequest(
	const FGuid& SessionId,
	const FString& ModelId,
	FOnLLMTurnComplete OnComplete,
	FOnLLMStreamProgress OnProgress,
	TSharedPtr<FLLMTokenUsage> AccumulatedUsage,
	TSharedPtr<FString> AccumulatedText,
	int32 ToolLoopCount)
{
	TSharedPtr<FLLMSessionState>* Found = Sessions.Find(SessionId);
	if (!Found || !Found->IsValid())
	{
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(FLLMTurnResult::Error(TEXT("Session lost during tool loop")));
		}
		return;
	}

	FString APIKey = GetAPIKey();
	FString RequestBody = BuildRequestBody(**Found, ModelId);

	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(APIEndpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("x-api-key"), APIKey);
	Request->SetHeader(TEXT("anthropic-version"), APIVersion);
	Request->SetContentAsString(RequestBody);

	TSharedPtr<FAnthropicSSEState> SSEState = MakeShared<FAnthropicSSEState>();

	// Parse SSE chunks as they arrive and emit stream events
	Request->OnRequestProgress64().BindLambda(
		[this, OnProgress, AccumulatedUsage, AccumulatedText, SSEState]
		(FHttpRequestPtr Req, uint64 /*BytesSent*/, uint64 /*BytesReceived*/)
	{
		if (!Req->GetResponse().IsValid()) return;

		FString FullContent = Req->GetResponse()->GetContentAsString();
		if (FullContent.Len() <= SSEState->BytesProcessed) return;

		SSEState->LineBuffer += FullContent.Mid(SSEState->BytesProcessed);
		SSEState->BytesProcessed = FullContent.Len();

		// Process all complete lines from the buffer
		int32 NewlineIdx;
		while (SSEState->LineBuffer.FindChar(TEXT('\n'), NewlineIdx))
		{
			FString Line = SSEState->LineBuffer.Left(NewlineIdx).TrimEnd();
			SSEState->LineBuffer = SSEState->LineBuffer.Mid(NewlineIdx + 1);

			// SSE lines are "data: {json}" — skip blank lines and non-data lines
			if (Line.IsEmpty() || !Line.StartsWith(TEXT("data:"))) continue;

			FString Data = Line.Mid(5).TrimStart();

			TSharedPtr<FJsonObject> EventJson;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Data);
			if (!FJsonSerializer::Deserialize(Reader, EventJson) || !EventJson.IsValid()) continue;

			FString EventType;
			EventJson->TryGetStringField(TEXT("type"), EventType);

			if (EventType == TEXT("message_start"))
			{
				// Extract input token count from initial usage
				const TSharedPtr<FJsonObject>* MsgObj = nullptr;
				if (EventJson->TryGetObjectField(TEXT("message"), MsgObj) && MsgObj)
				{
					const TSharedPtr<FJsonObject>* UsageObj = nullptr;
					if ((*MsgObj)->TryGetObjectField(TEXT("usage"), UsageObj) && UsageObj)
					{
						int32 InTokens = 0;
						(*UsageObj)->TryGetNumberField(TEXT("input_tokens"), InTokens);
						AccumulatedUsage->InputTokens += InTokens;
					}
				}
			}
			else if (EventType == TEXT("content_block_start"))
			{
				// Reset current block state
				SSEState->CurrentBlockType.Empty();
				SSEState->CurrentBlockId.Empty();
				SSEState->CurrentBlockName.Empty();
				SSEState->CurrentBlockText.Empty();
				SSEState->CurrentBlockInput.Empty();

				const TSharedPtr<FJsonObject>* BlockObj = nullptr;
				if (EventJson->TryGetObjectField(TEXT("content_block"), BlockObj) && BlockObj)
				{
					(*BlockObj)->TryGetStringField(TEXT("type"), SSEState->CurrentBlockType);
					(*BlockObj)->TryGetStringField(TEXT("id"), SSEState->CurrentBlockId);
					(*BlockObj)->TryGetStringField(TEXT("name"), SSEState->CurrentBlockName);
				}
			}
			else if (EventType == TEXT("content_block_delta"))
			{
				const TSharedPtr<FJsonObject>* DeltaObj = nullptr;
				if (EventJson->TryGetObjectField(TEXT("delta"), DeltaObj) && DeltaObj)
				{
					FString DeltaType;
					(*DeltaObj)->TryGetStringField(TEXT("type"), DeltaType);

					if (DeltaType == TEXT("text_delta"))
					{
						FString Text;
						(*DeltaObj)->TryGetStringField(TEXT("text"), Text);
						if (!Text.IsEmpty())
						{
							SSEState->CurrentBlockText += Text;
							*AccumulatedText += Text;

							// Emit immediately for real-time text display
							if (OnProgress.IsBound())
							{
								FClaudeStreamEvent Event;
								Event.Type = EClaudeStreamEventType::TextContent;
								Event.Text = Text;
								OnProgress.Execute(Event);
							}
						}
					}
					else if (DeltaType == TEXT("input_json_delta"))
					{
						FString PartialJson;
						(*DeltaObj)->TryGetStringField(TEXT("partial_json"), PartialJson);
						SSEState->CurrentBlockInput += PartialJson;
					}
				}
			}
			else if (EventType == TEXT("content_block_stop"))
			{
				if (SSEState->CurrentBlockType == TEXT("text"))
				{
					// Build text content block for assistant message
					TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
					TextBlock->SetStringField(TEXT("type"), TEXT("text"));
					TextBlock->SetStringField(TEXT("text"), SSEState->CurrentBlockText);
					SSEState->ContentBlocks.Add(MakeShared<FJsonValueObject>(TextBlock));
				}
				else if (SSEState->CurrentBlockType == TEXT("tool_use"))
				{
					// Parse accumulated input JSON
					TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
					if (!SSEState->CurrentBlockInput.IsEmpty())
					{
						TSharedRef<TJsonReader<>> InputReader = TJsonReaderFactory<>::Create(SSEState->CurrentBlockInput);
						TSharedPtr<FJsonObject> ParsedInput;
						if (FJsonSerializer::Deserialize(InputReader, ParsedInput) && ParsedInput.IsValid())
						{
							InputObj = ParsedInput;
						}
					}

					// Build tool_use content block for assistant message echo
					TSharedPtr<FJsonObject> ToolBlock = MakeShared<FJsonObject>();
					ToolBlock->SetStringField(TEXT("type"), TEXT("tool_use"));
					ToolBlock->SetStringField(TEXT("id"), SSEState->CurrentBlockId);
					ToolBlock->SetStringField(TEXT("name"), SSEState->CurrentBlockName);
					ToolBlock->SetObjectField(TEXT("input"), InputObj);
					SSEState->ContentBlocks.Add(MakeShared<FJsonValueObject>(ToolBlock));

					// Record completed tool call
					FLLMToolCaller::FToolCall ToolCall;
					ToolCall.Id = SSEState->CurrentBlockId;
					ToolCall.Name = SSEState->CurrentBlockName;
					ToolCall.InputJson = SSEState->CurrentBlockInput;
					SSEState->CompletedToolCalls.Add(ToolCall);

					// Emit ToolUse event — full input now available
					if (OnProgress.IsBound())
					{
						FClaudeStreamEvent Event;
						Event.Type = EClaudeStreamEventType::ToolUse;
						Event.ToolName = ToolCall.Name;
						Event.ToolInput = ToolCall.InputJson;
						Event.ToolCallId = ToolCall.Id;
						OnProgress.Execute(Event);
					}
				}
			}
			else if (EventType == TEXT("message_delta"))
			{
				// Stop reason and output token count
				const TSharedPtr<FJsonObject>* DeltaObj = nullptr;
				if (EventJson->TryGetObjectField(TEXT("delta"), DeltaObj) && DeltaObj)
				{
					(*DeltaObj)->TryGetStringField(TEXT("stop_reason"), SSEState->StopReason);
				}
				const TSharedPtr<FJsonObject>* UsageObj = nullptr;
				if (EventJson->TryGetObjectField(TEXT("usage"), UsageObj) && UsageObj)
				{
					int32 OutTokens = 0, CachedTokens = 0;
					(*UsageObj)->TryGetNumberField(TEXT("output_tokens"), OutTokens);
					(*UsageObj)->TryGetNumberField(TEXT("cache_read_input_tokens"), CachedTokens);
					AccumulatedUsage->OutputTokens += OutTokens;
					AccumulatedUsage->CachedInputTokens += CachedTokens;
				}
			}
			else if (EventType == TEXT("message_stop"))
			{
				SSEState->bStreamComplete = true;
			}
		}
	});

	// Handle completion: execute tools and loop, or finalize
	Request->OnProcessRequestComplete().BindLambda(
		[this, OnComplete, OnProgress, AccumulatedUsage, AccumulatedText, ModelId, SessionId, ToolLoopCount, SSEState]
		(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnectedSuccessfully)
	{
		CurrentRequest.Reset();

		if (!bConnectedSuccessfully || !Resp.IsValid())
		{
			if (OnComplete.IsBound())
			{
				OnComplete.Execute(FLLMTurnResult::Error(TEXT("Connection to Anthropic API failed")));
			}
			return;
		}

		int32 ResponseCode = Resp->GetResponseCode();
		if (ResponseCode != 200)
		{
			FString ErrorMsg = FString::Printf(TEXT("Anthropic API error (HTTP %d)"), ResponseCode);
			FString ResponseBody = Resp->GetContentAsString();
			TSharedPtr<FJsonObject> ErrorJson;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
			if (FJsonSerializer::Deserialize(Reader, ErrorJson) && ErrorJson.IsValid())
			{
				const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
				if (ErrorJson->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj)
				{
					FString Message;
					if ((*ErrorObj)->TryGetStringField(TEXT("message"), Message))
					{
						ErrorMsg = FString::Printf(TEXT("Anthropic API error (HTTP %d): %s"), ResponseCode, *Message);
					}
				}
			}
			if (OnComplete.IsBound())
			{
				OnComplete.Execute(FLLMTurnResult::Error(ErrorMsg));
			}
			return;
		}

		if (!SSEState->bStreamComplete)
		{
			if (OnComplete.IsBound())
			{
				OnComplete.Execute(FLLMTurnResult::Error(TEXT("Anthropic API stream ended unexpectedly")));
			}
			return;
		}

		// Tool calls — execute and loop
		if (SSEState->StopReason == TEXT("tool_use") && SSEState->CompletedToolCalls.Num() > 0
			&& ToolLoopCount < MaxToolLoopIterations)
		{
			TArray<TPair<bool, FString>> Results;
			for (const FLLMToolCaller::FToolCall& Call : SSEState->CompletedToolCalls)
			{
				UE_LOG(LogUnrealClaude, Log, TEXT("FAnthropicAPIBackend: Executing tool '%s' (id: %s)"),
					*Call.Name, *Call.Id);
				TPair<bool, FString> Result = FLLMToolCaller::ExecuteToolCall(Call.Name, Call.InputJson);
				Results.Add(Result);

				if (OnProgress.IsBound())
				{
					FClaudeStreamEvent Event;
					Event.Type = EClaudeStreamEventType::ToolResult;
					Event.ToolCallId = Call.Id;
					Event.ToolName = Call.Name;
					Event.ToolResultContent = Result.Value;
					Event.bIsError = !Result.Key;
					OnProgress.Execute(Event);
				}
			}

			TSharedPtr<FLLMSessionState>* SessionPtr = Sessions.Find(SessionId);
			if (SessionPtr && SessionPtr->IsValid())
			{
				(*SessionPtr)->AddRawJsonMessage(
					FLLMToolCaller::BuildAnthropicAssistantToolMessage(SSEState->ContentBlocks));
				(*SessionPtr)->AddRawJsonMessage(
					FLLMToolCaller::BuildAnthropicToolResultMessage(SSEState->CompletedToolCalls, Results));
			}

			SendRequest(SessionId, ModelId, OnComplete, OnProgress, AccumulatedUsage, AccumulatedText, ToolLoopCount + 1);
			return;
		}

		// No tool calls — finalize
		TSharedPtr<FLLMSessionState>* SessionPtr = Sessions.Find(SessionId);
		if (SessionPtr && SessionPtr->IsValid())
		{
			(*SessionPtr)->AddAssistantMessage(*AccumulatedText);
			(*SessionPtr)->ClearRawJsonMessages();
		}

		FLLMTurnResult Result = FLLMTurnResult::Success(*AccumulatedText, *AccumulatedUsage);

		if (OnProgress.IsBound())
		{
			FClaudeStreamEvent ResultEvent;
			ResultEvent.Type = EClaudeStreamEventType::Result;
			ResultEvent.ResultText = *AccumulatedText;
			ResultEvent.InputTokens = AccumulatedUsage->InputTokens;
			ResultEvent.OutputTokens = AccumulatedUsage->OutputTokens;
			ResultEvent.TotalCostUsd = AccumulatedUsage->EstimatedCostUsd;
			ResultEvent.NumTurns = ToolLoopCount + 1;
			OnProgress.Execute(ResultEvent);
		}

		if (OnComplete.IsBound())
		{
			OnComplete.Execute(Result);
		}
	});

	CurrentRequest = Request;
	Request->ProcessRequest();

	UE_LOG(LogUnrealClaude, Log, TEXT("FAnthropicAPIBackend: Sending SSE request (model: %s, loop: %d)"),
		*ModelId, ToolLoopCount);
}

void FAnthropicAPIBackend::Cancel()
{
	if (CurrentRequest.IsValid())
	{
		CurrentRequest->CancelRequest();
		CurrentRequest.Reset();
	}
}

bool FAnthropicAPIBackend::IsBusy() const
{
	return CurrentRequest.IsValid() &&
		CurrentRequest->GetStatus() == EHttpRequestStatus::Processing;
}

// ============================================================================
// Model Discovery
// ============================================================================

TArray<FString> FAnthropicAPIBackend::GetSupportedModels() const
{
	return {
		TEXT("claude-sonnet-4-6"),
		TEXT("claude-opus-4-6"),
		TEXT("claude-haiku-4-5-20251001")
	};
}

FString FAnthropicAPIBackend::GetModelDisplayName(const FString& ModelId) const
{
	if (ModelId == TEXT("claude-sonnet-4-6"))       return TEXT("Claude Sonnet 4.6");
	if (ModelId == TEXT("claude-opus-4-6"))         return TEXT("Claude Opus 4.6");
	if (ModelId == TEXT("claude-haiku-4-5-20251001")) return TEXT("Claude Haiku 4.5");
	return ModelId;
}

float FAnthropicAPIBackend::EstimateCost(const FString& PromptText, const FString& ModelId) const
{
	// Rough estimate: 1 token ~= 4 chars
	int32 EstimatedInputTokens = PromptText.Len() / 4;
	// Assume ~1000 output tokens for estimation
	int32 EstimatedOutputTokens = 1000;

	// Use pricing from the known rates
	float InputRate = 3.0f;   // Default to Sonnet pricing
	float OutputRate = 15.0f;

	if (ModelId.Contains(TEXT("opus")))
	{
		InputRate = 15.0f;
		OutputRate = 75.0f;
	}
	else if (ModelId.Contains(TEXT("haiku")))
	{
		InputRate = 0.8f;
		OutputRate = 4.0f;
	}

	return (static_cast<float>(EstimatedInputTokens) / 1000000.0f) * InputRate
		+ (static_cast<float>(EstimatedOutputTokens) / 1000000.0f) * OutputRate;
}

// ============================================================================
// Request Building
// ============================================================================

FString FAnthropicAPIBackend::BuildRequestBody(FLLMSessionState& Session, const FString& ModelId, int32 MaxTokens) const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(TEXT("model"), ModelId);
	Root->SetNumberField(TEXT("max_tokens"), MaxTokens);
	Root->SetBoolField(TEXT("stream"), true);

	// System prompt
	FString FullSystemPrompt = Session.GetFullSystemPrompt();
	if (!FullSystemPrompt.IsEmpty())
	{
		Root->SetStringField(TEXT("system"), FullSystemPrompt);
	}

	// Tools — include all MCP tools for Anthropic
	TArray<TSharedPtr<FJsonValue>> ToolDefs = FLLMToolCaller::GetToolsForAnthropic(EToolFilter::ReadOnly);
	if (ToolDefs.Num() > 0)
	{
		Root->SetArrayField(TEXT("tools"), ToolDefs);
	}

	// Messages array
	TArray<TSharedPtr<FJsonValue>> MessagesArray;

	for (const FLLMMessage& Msg : Session.GetMessages())
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		MsgObj->SetStringField(TEXT("role"), Msg.Role);

		if (Msg.ImagePaths.Num() > 0)
		{
			// Multi-part content with images
			TArray<TSharedPtr<FJsonValue>> ContentArray;

			for (const FString& ImagePath : Msg.ImagePaths)
			{
				TArray<uint8> ImageData;
				if (FFileHelper::LoadFileToArray(ImageData, *ImagePath))
				{
					FString Base64 = FBase64::Encode(ImageData);

					TSharedPtr<FJsonObject> ImageBlock = MakeShared<FJsonObject>();
					ImageBlock->SetStringField(TEXT("type"), TEXT("image"));

					TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
					Source->SetStringField(TEXT("type"), TEXT("base64"));
					Source->SetStringField(TEXT("media_type"), TEXT("image/png"));
					Source->SetStringField(TEXT("data"), Base64);
					ImageBlock->SetObjectField(TEXT("source"), Source);

					ContentArray.Add(MakeShared<FJsonValueObject>(ImageBlock));
				}
			}

			TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
			TextBlock->SetStringField(TEXT("type"), TEXT("text"));
			TextBlock->SetStringField(TEXT("text"), Msg.Content);
			ContentArray.Add(MakeShared<FJsonValueObject>(TextBlock));

			MsgObj->SetArrayField(TEXT("content"), ContentArray);
		}
		else
		{
			MsgObj->SetStringField(TEXT("content"), Msg.Content);
		}

		MessagesArray.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	// Append any raw JSON messages (tool_use echoes + tool_result messages from the loop)
	for (const TSharedPtr<FJsonObject>& RawMsg : Session.GetRawJsonMessages())
	{
		MessagesArray.Add(MakeShared<FJsonValueObject>(RawMsg));
	}

	Root->SetArrayField(TEXT("messages"), MessagesArray);

	// Serialize
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Output;
}

// ============================================================================
// Response Parsing (legacy — kept for compatibility)
// ============================================================================

FLLMTurnResult FAnthropicAPIBackend::ParseResponse(const FString& ResponseBody, const FString& ModelId) const
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);

	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FLLMTurnResult::Error(TEXT("Failed to parse Anthropic API response"));
	}

	FString ResponseText;
	const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
	if (Root->TryGetArrayField(TEXT("content"), ContentArray))
	{
		for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
		{
			const TSharedPtr<FJsonObject>* ContentObj = nullptr;
			if (ContentValue->TryGetObject(ContentObj) && ContentObj)
			{
				FString Type;
				if ((*ContentObj)->TryGetStringField(TEXT("type"), Type) && Type == TEXT("text"))
				{
					FString Text;
					if ((*ContentObj)->TryGetStringField(TEXT("text"), Text))
					{
						if (!ResponseText.IsEmpty()) ResponseText += TEXT("\n");
						ResponseText += Text;
					}
				}
			}
		}
	}

	FLLMTokenUsage Usage;
	Usage.ModelId = ModelId;
	Usage.ProviderId = TEXT("anthropic-api");

	const TSharedPtr<FJsonObject>* UsageObj = nullptr;
	if (Root->TryGetObjectField(TEXT("usage"), UsageObj) && UsageObj)
	{
		(*UsageObj)->TryGetNumberField(TEXT("input_tokens"), Usage.InputTokens);
		(*UsageObj)->TryGetNumberField(TEXT("output_tokens"), Usage.OutputTokens);
		(*UsageObj)->TryGetNumberField(TEXT("cache_read_input_tokens"), Usage.CachedInputTokens);
	}

	return FLLMTurnResult::Success(ResponseText, Usage);
}
