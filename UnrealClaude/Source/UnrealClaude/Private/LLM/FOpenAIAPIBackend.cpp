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
	// READ-ONLY: No ToolCalling, no MCP. ImageInput for vision models.
	return ELLMBackendCapability::ImageInput
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

	// Add user message to session history
	State.AddUserMessage(UserMessage, ImagePaths);

	// Build the request body
	FString RequestBody = BuildRequestBody(State, ModelId);

	// Create HTTP request
	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(APIEndpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *APIKey));
	Request->SetContentAsString(RequestBody);

	FGuid SessionId = Session.SessionId;

	// Handle completion
	Request->OnProcessRequestComplete().BindLambda(
		[this, OnComplete, ModelId, SessionId]
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
		FString ResponseBody = Resp->GetContentAsString();

		if (ResponseCode != 200)
		{
			FString ErrorMsg = FString::Printf(TEXT("OpenAI API error (HTTP %d)"), ResponseCode);

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

	UE_LOG(LogUnrealClaude, Log, TEXT("FOpenAIAPIBackend: Submitted turn (model: %s, session messages: %d)"),
		*ModelId, (*Found)->GetMessageCount());

	return true;
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

FString FOpenAIAPIBackend::BuildRequestBody(const FLLMSessionState& Session, const FString& ModelId, int32 MaxTokens) const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(TEXT("model"), ModelId);
	Root->SetNumberField(TEXT("max_tokens"), MaxTokens);

	// Messages array — OpenAI uses system message as first message
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
			// Multi-part content with images (OpenAI vision format)
			TArray<TSharedPtr<FJsonValue>> ContentArray;

			// Add images
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

			// Add text
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
