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
	return ELLMBackendCapability::ImageInput
		| ELLMBackendCapability::Streaming
		| ELLMBackendCapability::CostReporting;
	// No NativeSession, no ToolCalling, no MCP
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

	// Build the request body
	FString RequestBody = BuildRequestBody(State, ModelId);

	// Create HTTP request
	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(APIEndpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("x-api-key"), APIKey);
	Request->SetHeader(TEXT("anthropic-version"), APIVersion);
	Request->SetContentAsString(RequestBody);

	// Accumulate response data
	TSharedPtr<FLLMTokenUsage> AccumulatedUsage = MakeShared<FLLMTokenUsage>();
	AccumulatedUsage->ModelId = ModelId;
	AccumulatedUsage->ProviderId = TEXT("anthropic-api");

	TSharedPtr<FString> AccumulatedText = MakeShared<FString>();
	FGuid SessionId = Session.SessionId;

	// Handle completion
	Request->OnProcessRequestComplete().BindLambda(
		[this, OnComplete, AccumulatedUsage, AccumulatedText, ModelId, SessionId]
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
		FString ResponseBody = Resp->GetContentAsString();

		if (ResponseCode != 200)
		{
			// Parse error message from response
			FString ErrorMsg = FString::Printf(TEXT("Anthropic API error (HTTP %d)"), ResponseCode);

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

		// Parse the response
		FLLMTurnResult Result = ParseResponse(ResponseBody, ModelId);

		// Record assistant response in session
		TSharedPtr<FLLMSessionState>* SessionPtr = Sessions.Find(SessionId);
		if (SessionPtr && SessionPtr->IsValid() && Result.bSuccess)
		{
			(*SessionPtr)->AddAssistantMessage(Result.ResponseText);
		}

		if (OnComplete.IsBound())
		{
			OnComplete.Execute(Result);
		}
	});

	CurrentRequest = Request;
	Request->ProcessRequest();

	UE_LOG(LogUnrealClaude, Log, TEXT("FAnthropicAPIBackend: Submitted turn (model: %s, session messages: %d)"),
		*ModelId, (*Found)->GetMessageCount());

	return true;
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

FString FAnthropicAPIBackend::BuildRequestBody(const FLLMSessionState& Session, const FString& ModelId, int32 MaxTokens) const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(TEXT("model"), ModelId);
	Root->SetNumberField(TEXT("max_tokens"), MaxTokens);

	// System prompt
	FString FullSystemPrompt = Session.GetFullSystemPrompt();
	if (!FullSystemPrompt.IsEmpty())
	{
		Root->SetStringField(TEXT("system"), FullSystemPrompt);
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

			// Add images first
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

			// Add text content
			TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
			TextBlock->SetStringField(TEXT("type"), TEXT("text"));
			TextBlock->SetStringField(TEXT("text"), Msg.Content);
			ContentArray.Add(MakeShared<FJsonValueObject>(TextBlock));

			MsgObj->SetArrayField(TEXT("content"), ContentArray);
		}
		else
		{
			// Simple text content
			MsgObj->SetStringField(TEXT("content"), Msg.Content);
		}

		MessagesArray.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	Root->SetArrayField(TEXT("messages"), MessagesArray);

	// Serialize
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Output;
}

// ============================================================================
// Response Parsing
// ============================================================================

FLLMTurnResult FAnthropicAPIBackend::ParseResponse(const FString& ResponseBody, const FString& ModelId) const
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);

	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FLLMTurnResult::Error(TEXT("Failed to parse Anthropic API response"));
	}

	// Extract text from content blocks
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
						if (!ResponseText.IsEmpty())
						{
							ResponseText += TEXT("\n");
						}
						ResponseText += Text;
					}
				}
			}
		}
	}

	// Extract token usage
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

void FAnthropicAPIBackend::ProcessSSEChunk(
	const FString& Chunk,
	const FString& ModelId,
	FOnLLMStreamProgress OnProgress,
	TSharedPtr<FLLMTokenUsage> AccumulatedUsage,
	TSharedPtr<FString> AccumulatedText) const
{
	// SSE format: "event: <type>\ndata: <json>\n\n"
	// This is called for future streaming support
	// For now, we use non-streaming requests

	// Parse SSE lines
	TArray<FString> Lines;
	Chunk.ParseIntoArrayLines(Lines);

	FString EventType;
	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(TEXT("event: ")))
		{
			EventType = Line.Mid(7).TrimStartAndEnd();
		}
		else if (Line.StartsWith(TEXT("data: ")))
		{
			FString Data = Line.Mid(6);

			if (EventType == TEXT("content_block_delta"))
			{
				TSharedPtr<FJsonObject> DeltaJson;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Data);
				if (FJsonSerializer::Deserialize(Reader, DeltaJson) && DeltaJson.IsValid())
				{
					const TSharedPtr<FJsonObject>* Delta = nullptr;
					if (DeltaJson->TryGetObjectField(TEXT("delta"), Delta) && Delta)
					{
						FString Text;
						if ((*Delta)->TryGetStringField(TEXT("text"), Text))
						{
							*AccumulatedText += Text;

							// Emit stream event
							if (OnProgress.IsBound())
							{
								FClaudeStreamEvent Event;
								Event.Type = EClaudeStreamEventType::TextContent;
								Event.Text = Text;
								OnProgress.Execute(Event);
							}
						}
					}
				}
			}
			else if (EventType == TEXT("message_delta"))
			{
				TSharedPtr<FJsonObject> DeltaJson;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Data);
				if (FJsonSerializer::Deserialize(Reader, DeltaJson) && DeltaJson.IsValid())
				{
					const TSharedPtr<FJsonObject>* UsageObj = nullptr;
					if (DeltaJson->TryGetObjectField(TEXT("usage"), UsageObj) && UsageObj)
					{
						(*UsageObj)->TryGetNumberField(TEXT("output_tokens"), AccumulatedUsage->OutputTokens);
					}
				}
			}
			else if (EventType == TEXT("message_start"))
			{
				TSharedPtr<FJsonObject> StartJson;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Data);
				if (FJsonSerializer::Deserialize(Reader, StartJson) && StartJson.IsValid())
				{
					const TSharedPtr<FJsonObject>* MessageObj = nullptr;
					if (StartJson->TryGetObjectField(TEXT("message"), MessageObj) && MessageObj)
					{
						const TSharedPtr<FJsonObject>* UsageObj = nullptr;
						if ((*MessageObj)->TryGetObjectField(TEXT("usage"), UsageObj) && UsageObj)
						{
							(*UsageObj)->TryGetNumberField(TEXT("input_tokens"), AccumulatedUsage->InputTokens);
							(*UsageObj)->TryGetNumberField(TEXT("cache_read_input_tokens"), AccumulatedUsage->CachedInputTokens);
						}
					}
				}
			}

			EventType.Empty();
		}
	}
}
