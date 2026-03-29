// Copyright Natali Caggiano. All Rights Reserved.

#include "FOpenAIAPIBackend.h"
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

const FString FOpenAIAPIBackend::APIEndpoint = TEXT("https://api.openai.com/v1/chat/completions");

FOpenAIAPIBackend::FOpenAIAPIBackend()
{
}

FOpenAIAPIBackend::~FOpenAIAPIBackend()
{
	Cancel();
	Sessions.Empty();
}

// ============================================================================
// Capabilities
// ============================================================================

ELLMBackendCapability FOpenAIAPIBackend::GetCapabilities() const
{
	// READ-ONLY tool calling: only read-only MCP tools are exposed
	return ELLMBackendCapability::ToolCalling
		| ELLMBackendCapability::ImageInput
		| ELLMBackendCapability::Streaming
		| ELLMBackendCapability::CostReporting;
}

bool FOpenAIAPIBackend::IsAvailable() const
{
	return !GetAPIKey().IsEmpty();
}

FString FOpenAIAPIBackend::GetUnavailableReason() const
{
	if (IsAvailable())
	{
		return FString();
	}
	return TEXT("OpenAI API key not configured. Set it in the backend settings.");
}

FString FOpenAIAPIBackend::GetAPIKey() const
{
	return FLLMBackendRegistry::LoadAPIKey(TEXT("openai-api"));
}

// ============================================================================
// Session Management
// ============================================================================

FLLMSessionHandle FOpenAIAPIBackend::CreateTaskSession(const FString& SystemPrompt)
{
	FLLMSessionHandle Handle = FLLMSessionHandle::Create();
	Sessions.Add(Handle.SessionId, MakeShared<FLLMSessionState>(SystemPrompt));

	UE_LOG(LogUnrealClaude, Log, TEXT("FOpenAIAPIBackend: Created session %s"), *Handle.SessionId.ToString());
	return Handle;
}

void FOpenAIAPIBackend::AppendContext(const FLLMSessionHandle& Session, const FString& ContextText)
{
	TSharedPtr<FLLMSessionState>* Found = Sessions.Find(Session.SessionId);
	if (Found && Found->IsValid())
	{
		(*Found)->AppendContext(ContextText);
	}
}

void FOpenAIAPIBackend::SeedHistory(const FLLMSessionHandle& Session, const TArray<TPair<FString, FString>>& Exchanges)
{
	TSharedPtr<FLLMSessionState>* Found = Sessions.Find(Session.SessionId);
	if (!Found || !Found->IsValid())
	{
		return;
	}
	FLLMSessionState& State = **Found;
	for (const TPair<FString, FString>& Exchange : Exchanges)
	{
		State.AddUserMessage(Exchange.Key);
		State.AddAssistantMessage(Exchange.Value);
	}
}

void FOpenAIAPIBackend::DestroySession(const FLLMSessionHandle& Session)
{
	Sessions.Remove(Session.SessionId);
}

// ============================================================================
// Execution
// ============================================================================

bool FOpenAIAPIBackend::SubmitTurn(
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
			OnComplete.Execute(FLLMTurnResult::Error(TEXT("OpenAI API key not configured")));
		}
		return false;
	}

	FLLMSessionState& State = **Found;
	State.AddUserMessage(UserMessage, ImagePaths);

	TSharedPtr<FLLMTokenUsage> AccumulatedUsage = MakeShared<FLLMTokenUsage>();
	AccumulatedUsage->ModelId = ModelId;
	AccumulatedUsage->ProviderId = TEXT("openai-api");

	TSharedPtr<FString> AccumulatedText = MakeShared<FString>();

	SendRequest(Session.SessionId, ModelId, OnComplete, OnProgress, AccumulatedUsage, AccumulatedText, 0);
	return true;
}

// ============================================================================
// SSE Streaming
// ============================================================================

namespace
{
	/** Per-tool-call accumulation state (OpenAI sends tool calls in delta chunks by index) */
	struct FOpenAIPendingToolCall
	{
		FString Id;
		FString Name;
		FString Arguments; // accumulated function.arguments chunks
	};

	/** Per-request SSE parse state, shared between OnRequestProgress and OnProcessRequestComplete */
	struct FOpenAISSEState
	{
		FString LineBuffer;
		int32 BytesProcessed = 0;

		// Text content from this turn (may appear before tool calls)
		FString ResponseText;

		// Tool calls accumulated by index
		TMap<int32, FOpenAIPendingToolCall> PendingToolCalls;

		// Stream end state
		FString FinishReason;
		bool bStreamComplete = false;
	};
}

void FOpenAIAPIBackend::SendRequest(
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
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *APIKey));
	Request->SetContentAsString(RequestBody);

	TSharedPtr<FOpenAISSEState> SSEState = MakeShared<FOpenAISSEState>();

	// Parse SSE chunks as they arrive and emit stream events
	Request->OnRequestProgress64().BindLambda(
		[this, OnProgress, AccumulatedUsage, SSEState]
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

			if (Line.IsEmpty() || !Line.StartsWith(TEXT("data:"))) continue;

			FString Data = Line.Mid(5).TrimStart();

			if (Data == TEXT("[DONE]"))
			{
				SSEState->bStreamComplete = true;
				continue;
			}

			TSharedPtr<FJsonObject> EventJson;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Data);
			if (!FJsonSerializer::Deserialize(Reader, EventJson) || !EventJson.IsValid()) continue;

			// Extract usage if present (stream_options: {include_usage: true} sends it last)
			const TSharedPtr<FJsonObject>* UsageObj = nullptr;
			if (EventJson->TryGetObjectField(TEXT("usage"), UsageObj) && UsageObj)
			{
				int32 InTokens = 0, OutTokens = 0, CachedTokens = 0;
				(*UsageObj)->TryGetNumberField(TEXT("prompt_tokens"), InTokens);
				(*UsageObj)->TryGetNumberField(TEXT("completion_tokens"), OutTokens);
				const TSharedPtr<FJsonObject>* PromptDetails = nullptr;
				if ((*UsageObj)->TryGetObjectField(TEXT("prompt_tokens_details"), PromptDetails) && PromptDetails)
				{
					(*PromptDetails)->TryGetNumberField(TEXT("cached_tokens"), CachedTokens);
				}
				AccumulatedUsage->InputTokens += InTokens;
				AccumulatedUsage->OutputTokens += OutTokens;
				AccumulatedUsage->CachedInputTokens += CachedTokens;
			}

			// Process choices delta
			const TArray<TSharedPtr<FJsonValue>>* ChoicesArray = nullptr;
			if (!EventJson->TryGetArrayField(TEXT("choices"), ChoicesArray) || ChoicesArray->Num() == 0) continue;

			const TSharedPtr<FJsonObject>* ChoiceObj = nullptr;
			if (!(*ChoicesArray)[0]->TryGetObject(ChoiceObj) || !ChoiceObj) continue;

			// Finish reason
			FString FinishReason;
			if ((*ChoiceObj)->TryGetStringField(TEXT("finish_reason"), FinishReason) && !FinishReason.IsEmpty())
			{
				SSEState->FinishReason = FinishReason;
			}

			const TSharedPtr<FJsonObject>* DeltaObj = nullptr;
			if (!(*ChoiceObj)->TryGetObjectField(TEXT("delta"), DeltaObj) || !DeltaObj) continue;

			// Text content chunk
			FString Content;
			if ((*DeltaObj)->TryGetStringField(TEXT("content"), Content) && !Content.IsEmpty())
			{
				SSEState->ResponseText += Content;

				// Emit immediately for real-time text display
				if (OnProgress.IsBound())
				{
					FClaudeStreamEvent Event;
					Event.Type = EClaudeStreamEventType::TextContent;
					Event.Text = Content;
					OnProgress.Execute(Event);
				}
			}

			// Tool call deltas — accumulate by index
			const TArray<TSharedPtr<FJsonValue>>* ToolCallsArray = nullptr;
			if ((*DeltaObj)->TryGetArrayField(TEXT("tool_calls"), ToolCallsArray))
			{
				for (const TSharedPtr<FJsonValue>& TCValue : *ToolCallsArray)
				{
					const TSharedPtr<FJsonObject>* TCObj = nullptr;
					if (!TCValue->TryGetObject(TCObj) || !TCObj) continue;

					int32 ToolIndex = 0;
					(*TCObj)->TryGetNumberField(TEXT("index"), ToolIndex);

					FOpenAIPendingToolCall& PTC = SSEState->PendingToolCalls.FindOrAdd(ToolIndex);

					FString Id;
					if ((*TCObj)->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
					{
						PTC.Id = Id;
					}

					const TSharedPtr<FJsonObject>* FuncObj = nullptr;
					if ((*TCObj)->TryGetObjectField(TEXT("function"), FuncObj) && FuncObj)
					{
						FString Name;
						if ((*FuncObj)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
						{
							PTC.Name = Name;
						}
						FString Args;
						if ((*FuncObj)->TryGetStringField(TEXT("arguments"), Args))
						{
							PTC.Arguments += Args;
						}
					}
				}
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
				OnComplete.Execute(FLLMTurnResult::Error(TEXT("Connection to OpenAI API failed")));
			}
			return;
		}

		int32 ResponseCode = Resp->GetResponseCode();
		if (ResponseCode != 200)
		{
			FString ErrorMsg = FString::Printf(TEXT("OpenAI API error (HTTP %d)"), ResponseCode);
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
						ErrorMsg = FString::Printf(TEXT("OpenAI API error (HTTP %d): %s"), ResponseCode, *Message);
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
				OnComplete.Execute(FLLMTurnResult::Error(TEXT("OpenAI API stream ended unexpectedly")));
			}
			return;
		}

		// Tool calls — execute and loop
		if (SSEState->FinishReason == TEXT("tool_calls") && SSEState->PendingToolCalls.Num() > 0
			&& ToolLoopCount < MaxToolLoopIterations)
		{
			// Sort by index and convert to FToolCall
			TArray<int32> Indices;
			SSEState->PendingToolCalls.GetKeys(Indices);
			Indices.Sort();

			TArray<FLLMToolCaller::FToolCall> ToolCalls;
			for (int32 Idx : Indices)
			{
				const FOpenAIPendingToolCall& PTC = SSEState->PendingToolCalls[Idx];
				FLLMToolCaller::FToolCall TC;
				TC.Id = PTC.Id;
				TC.Name = PTC.Name;
				TC.InputJson = PTC.Arguments;
				ToolCalls.Add(TC);

				// Emit ToolUse event (full input now available)
				if (OnProgress.IsBound())
				{
					FClaudeStreamEvent Event;
					Event.Type = EClaudeStreamEventType::ToolUse;
					Event.ToolName = TC.Name;
					Event.ToolInput = TC.InputJson;
					Event.ToolCallId = TC.Id;
					OnProgress.Execute(Event);
				}
			}

			// Execute tools (READ-ONLY only — enforced by GetToolsForOpenAI filter)
			TArray<TPair<bool, FString>> Results;
			for (const FLLMToolCaller::FToolCall& Call : ToolCalls)
			{
				UE_LOG(LogUnrealClaude, Log, TEXT("FOpenAIAPIBackend: Executing read-only tool '%s'"), *Call.Name);
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

			// Build assistant message with tool_calls for session history
			TSharedPtr<FJsonObject> AssistantMsg = MakeShared<FJsonObject>();
			AssistantMsg->SetStringField(TEXT("role"), TEXT("assistant"));
			if (!SSEState->ResponseText.IsEmpty())
			{
				AssistantMsg->SetStringField(TEXT("content"), SSEState->ResponseText);
			}
			else
			{
				AssistantMsg->SetField(TEXT("content"), MakeShared<FJsonValueNull>());
			}

			TArray<TSharedPtr<FJsonValue>> ToolCallsJson;
			for (const FLLMToolCaller::FToolCall& TC : ToolCalls)
			{
				TSharedPtr<FJsonObject> TCObj = MakeShared<FJsonObject>();
				TCObj->SetStringField(TEXT("id"), TC.Id);
				TCObj->SetStringField(TEXT("type"), TEXT("function"));
				TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
				FuncObj->SetStringField(TEXT("name"), TC.Name);
				FuncObj->SetStringField(TEXT("arguments"), TC.InputJson);
				TCObj->SetObjectField(TEXT("function"), FuncObj);
				ToolCallsJson.Add(MakeShared<FJsonValueObject>(TCObj));
			}
			AssistantMsg->SetArrayField(TEXT("tool_calls"), ToolCallsJson);

			// Append text to accumulated text if any
			if (!SSEState->ResponseText.IsEmpty())
			{
				if (!AccumulatedText->IsEmpty()) *AccumulatedText += TEXT("\n");
				*AccumulatedText += SSEState->ResponseText;
			}

			TSharedPtr<FLLMSessionState>* SessionPtr = Sessions.Find(SessionId);
			if (SessionPtr && SessionPtr->IsValid())
			{
				(*SessionPtr)->AddRawJsonMessage(AssistantMsg);
				TArray<TSharedPtr<FJsonObject>> ResultMsgs =
					FLLMToolCaller::BuildOpenAIToolResultMessages(ToolCalls, Results);
				for (const TSharedPtr<FJsonObject>& Msg : ResultMsgs)
				{
					(*SessionPtr)->AddRawJsonMessage(Msg);
				}
			}

			SendRequest(SessionId, ModelId, OnComplete, OnProgress, AccumulatedUsage, AccumulatedText, ToolLoopCount + 1);
			return;
		}

		// No tool calls — finalize
		if (!SSEState->ResponseText.IsEmpty())
		{
			if (!AccumulatedText->IsEmpty()) *AccumulatedText += TEXT("\n");
			*AccumulatedText += SSEState->ResponseText;
		}

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

	UE_LOG(LogUnrealClaude, Log, TEXT("FOpenAIAPIBackend: Sending SSE request (model: %s, loop: %d)"),
		*ModelId, ToolLoopCount);
}

void FOpenAIAPIBackend::Cancel()
{
	if (CurrentRequest.IsValid())
	{
		CurrentRequest->CancelRequest();
		CurrentRequest.Reset();
	}
}

bool FOpenAIAPIBackend::IsBusy() const
{
	return CurrentRequest.IsValid() &&
		CurrentRequest->GetStatus() == EHttpRequestStatus::Processing;
}

// ============================================================================
// Model Discovery
// ============================================================================

TArray<FString> FOpenAIAPIBackend::GetSupportedModels() const
{
	return {
		TEXT("gpt-4o"),
		TEXT("gpt-4o-mini"),
		TEXT("gpt-5.2-codex"),
		TEXT("gpt-5.4"),
		TEXT("gpt-5.4-mini"),
		TEXT("o3"),
		TEXT("o4-mini")
	};
}

FString FOpenAIAPIBackend::GetModelDisplayName(const FString& ModelId) const
{
	if (ModelId == TEXT("gpt-4o"))         return TEXT("GPT-4o");
	if (ModelId == TEXT("gpt-4o-mini"))    return TEXT("GPT-4o Mini");
	if (ModelId == TEXT("gpt-5.2-codex"))  return TEXT("GPT-5.2 Codex");
	if (ModelId == TEXT("gpt-5.4"))        return TEXT("GPT-5.4");
	if (ModelId == TEXT("gpt-5.4-mini"))   return TEXT("GPT-5.4 Mini");
	if (ModelId == TEXT("o3"))             return TEXT("o3");
	if (ModelId == TEXT("o4-mini"))        return TEXT("o4-mini");
	return ModelId;
}

float FOpenAIAPIBackend::EstimateCost(const FString& PromptText, const FString& ModelId) const
{
	int32 EstimatedInputTokens = PromptText.Len() / 4;
	int32 EstimatedOutputTokens = 1000;

	// Default to GPT-4o pricing
	float InputRate = 2.5f;
	float OutputRate = 10.0f;

	if (ModelId.Contains(TEXT("mini")))
	{
		InputRate = 0.15f;
		OutputRate = 0.60f;
	}
	else if (ModelId.Contains(TEXT("5.4")) && !ModelId.Contains(TEXT("mini")))
	{
		InputRate = 5.0f;
		OutputRate = 15.0f;
	}
	else if (ModelId.Contains(TEXT("codex")))
	{
		InputRate = 2.0f;
		OutputRate = 8.0f;
	}
	else if (ModelId == TEXT("o3"))
	{
		InputRate = 10.0f;
		OutputRate = 40.0f;
	}

	return (static_cast<float>(EstimatedInputTokens) / 1000000.0f) * InputRate
		+ (static_cast<float>(EstimatedOutputTokens) / 1000000.0f) * OutputRate;
}

// ============================================================================
// Request Building
// ============================================================================

FString FOpenAIAPIBackend::BuildRequestBody(FLLMSessionState& Session, const FString& ModelId, int32 MaxTokens) const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(TEXT("model"), ModelId);
	Root->SetNumberField(TEXT("max_completion_tokens"), MaxTokens);
	Root->SetBoolField(TEXT("stream"), true);

	// Request token usage in the final stream chunk
	TSharedPtr<FJsonObject> StreamOptions = MakeShared<FJsonObject>();
	StreamOptions->SetBoolField(TEXT("include_usage"), true);
	Root->SetObjectField(TEXT("stream_options"), StreamOptions);

	// Tools — READ-ONLY only for OpenAI
	TArray<TSharedPtr<FJsonValue>> ToolDefs = FLLMToolCaller::GetToolsForOpenAI(EToolFilter::ReadOnly);
	if (ToolDefs.Num() > 0)
	{
		Root->SetArrayField(TEXT("tools"), ToolDefs);
	}

	// Messages array
	TArray<TSharedPtr<FJsonValue>> MessagesArray;

	// System message
	FString FullSystemPrompt = Session.GetFullSystemPrompt();
	if (!FullSystemPrompt.IsEmpty())
	{
		TSharedPtr<FJsonObject> SystemMsg = MakeShared<FJsonObject>();
		SystemMsg->SetStringField(TEXT("role"), TEXT("system"));
		SystemMsg->SetStringField(TEXT("content"), FullSystemPrompt);
		MessagesArray.Add(MakeShared<FJsonValueObject>(SystemMsg));
	}

	// Conversation messages
	for (const FLLMMessage& Msg : Session.GetMessages())
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		MsgObj->SetStringField(TEXT("role"), Msg.Role);

		if (Msg.ImagePaths.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ContentArray;

			for (const FString& ImagePath : Msg.ImagePaths)
			{
				TArray<uint8> ImageData;
				if (FFileHelper::LoadFileToArray(ImageData, *ImagePath))
				{
					FString Base64 = FBase64::Encode(ImageData);

					TSharedPtr<FJsonObject> ImageBlock = MakeShared<FJsonObject>();
					ImageBlock->SetStringField(TEXT("type"), TEXT("image_url"));

					TSharedPtr<FJsonObject> ImageUrl = MakeShared<FJsonObject>();
					ImageUrl->SetStringField(TEXT("url"),
						FString::Printf(TEXT("data:image/png;base64,%s"), *Base64));
					ImageBlock->SetObjectField(TEXT("image_url"), ImageUrl);

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

	// Append raw JSON messages (tool call echoes + tool results from the loop)
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

FLLMTurnResult FOpenAIAPIBackend::ParseResponse(const FString& ResponseBody, const FString& ModelId) const
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);

	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FLLMTurnResult::Error(TEXT("Failed to parse OpenAI API response"));
	}

	// Extract text from choices[0].message.content
	FString ResponseText;
	const TArray<TSharedPtr<FJsonValue>>* ChoicesArray = nullptr;
	if (Root->TryGetArrayField(TEXT("choices"), ChoicesArray) && ChoicesArray->Num() > 0)
	{
		const TSharedPtr<FJsonObject>* ChoiceObj = nullptr;
		if ((*ChoicesArray)[0]->TryGetObject(ChoiceObj) && ChoiceObj)
		{
			const TSharedPtr<FJsonObject>* MessageObj = nullptr;
			if ((*ChoiceObj)->TryGetObjectField(TEXT("message"), MessageObj) && MessageObj)
			{
				(*MessageObj)->TryGetStringField(TEXT("content"), ResponseText);
			}
		}
	}

	// Extract token usage
	FLLMTokenUsage Usage;
	Usage.ModelId = ModelId;
	Usage.ProviderId = TEXT("openai-api");

	const TSharedPtr<FJsonObject>* UsageObj = nullptr;
	if (Root->TryGetObjectField(TEXT("usage"), UsageObj) && UsageObj)
	{
		(*UsageObj)->TryGetNumberField(TEXT("prompt_tokens"), Usage.InputTokens);
		(*UsageObj)->TryGetNumberField(TEXT("completion_tokens"), Usage.OutputTokens);

		// OpenAI reports cached tokens under prompt_tokens_details
		const TSharedPtr<FJsonObject>* PromptDetails = nullptr;
		if ((*UsageObj)->TryGetObjectField(TEXT("prompt_tokens_details"), PromptDetails) && PromptDetails)
		{
			(*PromptDetails)->TryGetNumberField(TEXT("cached_tokens"), Usage.CachedInputTokens);
		}
	}

	return FLLMTurnResult::Success(ResponseText, Usage);
}
