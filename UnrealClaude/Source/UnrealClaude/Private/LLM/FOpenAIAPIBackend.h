// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LLM/ILLMBackend.h"
#include "LLMSessionState.h"
#include "Interfaces/IHttpRequest.h"

/**
 * LLM backend that talks directly to the OpenAI Chat Completions API.
 *
 * This is a request/response backend — the plugin manages conversation
 * history and sends the full context with every request.
 *
 * IMPORTANT: This backend is READ-ONLY. It has no tool calling or MCP
 * capabilities. OpenAI models can only analyze and respond — they cannot
 * modify editor state, blueprints, assets, or project files.
 *
 * Endpoint: POST https://api.openai.com/v1/chat/completions
 * Auth: Bearer token
 * Streaming: SSE with choices[0].delta.content
 */
class FOpenAIAPIBackend : public ILLMBackend
{
public:
	FOpenAIAPIBackend();
	virtual ~FOpenAIAPIBackend();

	// ---- ILLMBackend ----

	virtual FString GetBackendName() const override { return TEXT("OpenAI API"); }
	virtual FString GetProviderId() const override { return TEXT("openai-api"); }
	virtual ELLMBackendType GetBackendType() const override { return ELLMBackendType::RequestResponse; }

	virtual ELLMBackendCapability GetCapabilities() const override;
	virtual bool IsAvailable() const override;
	virtual FString GetUnavailableReason() const override;

	virtual FLLMSessionHandle CreateTaskSession(const FString& SystemPrompt) override;
	virtual void AppendContext(const FLLMSessionHandle& Session, const FString& ContextText) override;
	virtual void DestroySession(const FLLMSessionHandle& Session) override;

	virtual bool SubmitTurn(
		const FLLMSessionHandle& Session,
		const FString& UserMessage,
		const TArray<FString>& ImagePaths,
		const FString& ModelId,
		FOnLLMTurnComplete OnComplete,
		FOnLLMStreamProgress OnProgress = FOnLLMStreamProgress()
	) override;

	virtual void Cancel() override;
	virtual bool IsBusy() const override;

	virtual TArray<FString> GetSupportedModels() const override;
	virtual FString GetModelDisplayName(const FString& ModelId) const override;
	virtual float EstimateCost(const FString& PromptText, const FString& ModelId) const override;

private:
	/** Build the JSON request body for Chat Completions */
	FString BuildRequestBody(const FLLMSessionState& Session, const FString& ModelId, int32 MaxTokens = 8192) const;

	/** Parse a non-streaming response */
	FLLMTurnResult ParseResponse(const FString& ResponseBody, const FString& ModelId) const;

	/** Get the API key */
	FString GetAPIKey() const;

	/** API endpoint */
	static const FString APIEndpoint;

	/** Active sessions */
	TMap<FGuid, TSharedPtr<FLLMSessionState>> Sessions;

	/** Current in-flight request */
	TSharedPtr<IHttpRequest> CurrentRequest;
};
