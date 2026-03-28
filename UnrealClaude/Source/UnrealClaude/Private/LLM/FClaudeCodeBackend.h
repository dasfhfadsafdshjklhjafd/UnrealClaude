// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LLM/ILLMBackend.h"

class FClaudeCodeRunner;

/**
 * LLM backend that wraps the existing Claude Code CLI subprocess.
 *
 * This is a session-oriented backend — the CLI process manages its own
 * conversation state, tool calling, MCP connections, and context compaction.
 *
 * The wrapper delegates to FClaudeCodeRunner without modifying it.
 * Token usage is extracted from the FClaudeStreamEvent::Result events
 * that the runner already produces.
 */
class FClaudeCodeBackend : public ILLMBackend
{
public:
	FClaudeCodeBackend();
	virtual ~FClaudeCodeBackend();

	// ---- ILLMBackend ----

	virtual FString GetBackendName() const override { return TEXT("Claude Code CLI"); }
	virtual FString GetProviderId() const override { return TEXT("claude-code"); }
	virtual ELLMBackendType GetBackendType() const override { return ELLMBackendType::SessionOriented; }

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

	/** Get the underlying CLI runner (for backward compatibility with GetRunner()) */
	FClaudeCodeRunner* GetClaudeRunner() const { return Runner.Get(); }

private:
	/** Internal session data */
	struct FClaudeCodeSession
	{
		FLLMSessionHandle Handle;
		FString SystemPrompt;
		TArray<FString> ContextBlocks;
		TArray<TPair<FString, FString>> History;  // user/assistant pairs
	};

	/** Build the full prompt with history for the CLI */
	FString BuildPromptWithHistory(const FClaudeCodeSession& Session, const FString& NewMessage) const;

	TUniquePtr<FClaudeCodeRunner> Runner;
	TMap<FGuid, FClaudeCodeSession> Sessions;
};
