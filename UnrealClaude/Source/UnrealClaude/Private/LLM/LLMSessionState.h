// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LLM/ILLMBackend.h"

/**
 * A single message in a conversation.
 */
struct FLLMMessage
{
	/** Role: "system", "user", or "assistant" */
	FString Role;

	/** Text content */
	FString Content;

	/** Optional image paths (for user messages with vision) */
	TArray<FString> ImagePaths;
};

/**
 * Manages conversation history for request/response (stateless) backends.
 *
 * Session-oriented backends (CLI) manage their own state — this class is
 * only used by FAnthropicAPIBackend and FOpenAIAPIBackend.
 *
 * Each session has a system prompt and a rolling message buffer. The buffer
 * can be trimmed to stay within token budgets.
 */
class FLLMSessionState
{
public:
	FLLMSessionState() = default;
	explicit FLLMSessionState(const FString& SystemPrompt);

	/** Get the system prompt for this session */
	const FString& GetSystemPrompt() const { return SystemPrompt; }

	/** Append additional context as a system-level message */
	void AppendContext(const FString& ContextText);

	/** Add a user message */
	void AddUserMessage(const FString& Content, const TArray<FString>& ImagePaths = {});

	/** Add an assistant response */
	void AddAssistantMessage(const FString& Content);

	/** Get all messages in order (system prompt is NOT included — handle separately per API) */
	const TArray<FLLMMessage>& GetMessages() const { return Messages; }

	/** Get all context blocks appended via AppendContext */
	const TArray<FString>& GetContextBlocks() const { return ContextBlocks; }

	/** Get the full system prompt including appended context */
	FString GetFullSystemPrompt() const;

	/** Get total message count (excluding system prompt) */
	int32 GetMessageCount() const { return Messages.Num(); }

	/** Rough token estimate for the full conversation (chars / 4) */
	int32 EstimateTokenCount() const;

	/** Trim oldest messages to stay under a target token count. Keeps at least the last N exchanges. */
	void TrimToTokenBudget(int32 MaxTokens, int32 MinExchangesToKeep = 2);

	/** Clear all messages (keeps system prompt) */
	void ClearMessages();

private:
	FString SystemPrompt;
	TArray<FString> ContextBlocks;
	TArray<FLLMMessage> Messages;
};
