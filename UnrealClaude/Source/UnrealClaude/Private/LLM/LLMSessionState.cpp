// Copyright Natali Caggiano. All Rights Reserved.

#include "LLMSessionState.h"

FLLMSessionState::FLLMSessionState(const FString& InSystemPrompt)
	: SystemPrompt(InSystemPrompt)
{
}

void FLLMSessionState::AppendContext(const FString& ContextText)
{
	if (!ContextText.IsEmpty())
	{
		ContextBlocks.Add(ContextText);
	}
}

void FLLMSessionState::AddUserMessage(const FString& Content, const TArray<FString>& ImagePaths)
{
	FLLMMessage Msg;
	Msg.Role = TEXT("user");
	Msg.Content = Content;
	Msg.ImagePaths = ImagePaths;
	Messages.Add(MoveTemp(Msg));
}

void FLLMSessionState::AddAssistantMessage(const FString& Content)
{
	FLLMMessage Msg;
	Msg.Role = TEXT("assistant");
	Msg.Content = Content;
	Messages.Add(MoveTemp(Msg));
}

FString FLLMSessionState::GetFullSystemPrompt() const
{
	if (ContextBlocks.IsEmpty())
	{
		return SystemPrompt;
	}

	FString Full = SystemPrompt;
	for (const FString& Block : ContextBlocks)
	{
		Full += TEXT("\n\n") + Block;
	}
	return Full;
}

int32 FLLMSessionState::EstimateTokenCount() const
{
	int32 CharCount = GetFullSystemPrompt().Len();
	for (const FLLMMessage& Msg : Messages)
	{
		CharCount += Msg.Content.Len();
	}
	// Rough estimate: 1 token ~= 4 characters for English text
	return CharCount / 4;
}

void FLLMSessionState::TrimToTokenBudget(int32 MaxTokens, int32 MinExchangesToKeep)
{
	// An "exchange" is a user+assistant pair = 2 messages
	int32 MinMessagesToKeep = MinExchangesToKeep * 2;

	while (Messages.Num() > MinMessagesToKeep && EstimateTokenCount() > MaxTokens)
	{
		// Remove the oldest message
		Messages.RemoveAt(0);
	}
}

void FLLMSessionState::ClearMessages()
{
	Messages.Empty();
}
