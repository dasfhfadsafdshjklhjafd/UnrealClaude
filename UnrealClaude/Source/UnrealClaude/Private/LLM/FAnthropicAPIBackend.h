// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LLM/ILLMBackend.h"
#include "LLMSessionState.h"
#include "LLMToolCaller.h"
#include "Interfaces/IHttpRequest.h"

/**
 * LLM backend that talks directly to the Anthropic Messages API.
 *
 * This is a request/response backend — the plugin manages conversation
 * history and sends the full context with every request.
 *
 * Endpoint: POST https://api.anthropic.com/v1/messages
 * Auth: x-api-key header
 * Streaming: SSE with content_block_delta events
 *
 * Token usage is extracted from the message_stop event's usage object.
 * Supports prompt caching — cached_input_tokens are tracked for cost accuracy.
 */
class FAnthropicAPIBackend : public ILLMBackend
{
public:
	FAnthropicAPIBackend();
	virtual ~FAnthropicAPIBackend();

	// ---- ILLMBackend ----

	virtual FString GetBackendName() const override { return TEXT("Anthropic API"); }
	virtual FString GetProviderId() const override { return TEXT("anthropic-api"); }
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
	/** Build the JSON request body for the Messages API (includes tools) */
	FString BuildRequestBody(FLLMSessionState& Session, const FString& ModelId, int32 MaxTokens = 8192) const;

	/** Parse a non-streaming response, handling tool_use stop_reason */
	FLLMTurnResult ParseResponse(const FString& ResponseBody, const FString& ModelId) const;

	/** Parse SSE stream chunks and emit events */
	void ProcessSSEChunk(const FString& Chunk, const FString& ModelId,
		FOnLLMStreamProgress OnProgress, TSharedPtr<FLLMTokenUsage> AccumulatedUsage,
		TSharedPtr<FString> AccumulatedText) const;

	/**
	 * Handle a single HTTP response. If tools were called, executes them and
	 * fires the next request. Otherwise completes the turn.
	 */
	void HandleResponse(
		const FString& ResponseBody,
		const FString& ModelId,
		const FGuid& SessionId,
		FOnLLMTurnComplete OnComplete,
		FOnLLMStreamProgress OnProgress,
		TSharedPtr<FLLMTokenUsage> AccumulatedUsage,
		TSharedPtr<FString> AccumulatedText,
		int32 ToolLoopCount);

	/** Send a follow-up request (for tool-call loop continuation) */
	void SendRequest(
		const FGuid& SessionId,
		const FString& ModelId,
		FOnLLMTurnComplete OnComplete,
		FOnLLMStreamProgress OnProgress,
		TSharedPtr<FLLMTokenUsage> AccumulatedUsage,
		TSharedPtr<FString> AccumulatedText,
		int32 ToolLoopCount);

	/** Get the API key */
	FString GetAPIKey() const;

	/** API endpoint */
	static const FString APIEndpoint;
	static const FString APIVersion;

	/** Max tool-call loop iterations to prevent runaway */
	static const int32 MaxToolLoopIterations = 20;

	/** Active sessions */
	TMap<FGuid, TSharedPtr<FLLMSessionState>> Sessions;

	/** Current in-flight request */
	TSharedPtr<IHttpRequest> CurrentRequest;

	/** SSE line buffer for partial chunks */
	FString SSEBuffer;
};
