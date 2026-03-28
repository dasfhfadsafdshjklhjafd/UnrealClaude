// Copyright Natali Caggiano. All Rights Reserved.

#include "FClaudeCodeBackend.h"
#include "ClaudeCodeRunner.h"
#include "UnrealClaudeModule.h"
#include "UnrealClaudeConstants.h"
#include "Misc/Paths.h"

FClaudeCodeBackend::FClaudeCodeBackend()
{
	Runner = MakeUnique<FClaudeCodeRunner>();
}

FClaudeCodeBackend::~FClaudeCodeBackend()
{
	Sessions.Empty();
}

// ============================================================================
// Capabilities
// ============================================================================

ELLMBackendCapability FClaudeCodeBackend::GetCapabilities() const
{
	return ELLMBackendCapability::NativeSession
		| ELLMBackendCapability::ToolCalling
		| ELLMBackendCapability::MCP
		| ELLMBackendCapability::ImageInput
		| ELLMBackendCapability::Streaming
		| ELLMBackendCapability::CostReporting;
}

bool FClaudeCodeBackend::IsAvailable() const
{
	return FClaudeCodeRunner::IsClaudeAvailable();
}

FString FClaudeCodeBackend::GetUnavailableReason() const
{
	if (IsAvailable())
	{
		return FString();
	}
	return TEXT("Claude Code CLI not found. Install it via: npm install -g @anthropic-ai/claude-code");
}

// ============================================================================
// Session Management
// ============================================================================

FLLMSessionHandle FClaudeCodeBackend::CreateTaskSession(const FString& SystemPrompt)
{
	FLLMSessionHandle Handle = FLLMSessionHandle::Create();

	FClaudeCodeSession Session;
	Session.Handle = Handle;
	Session.SystemPrompt = SystemPrompt;

	Sessions.Add(Handle.SessionId, MoveTemp(Session));

	UE_LOG(LogUnrealClaude, Log, TEXT("FClaudeCodeBackend: Created session %s"), *Handle.SessionId.ToString());
	return Handle;
}

void FClaudeCodeBackend::AppendContext(const FLLMSessionHandle& Session, const FString& ContextText)
{
	FClaudeCodeSession* S = Sessions.Find(Session.SessionId);
	if (S && !ContextText.IsEmpty())
	{
		S->ContextBlocks.Add(ContextText);
	}
}

void FClaudeCodeBackend::DestroySession(const FLLMSessionHandle& Session)
{
	Sessions.Remove(Session.SessionId);
}

// ============================================================================
// Execution
// ============================================================================

bool FClaudeCodeBackend::SubmitTurn(
	const FLLMSessionHandle& Session,
	const FString& UserMessage,
	const TArray<FString>& ImagePaths,
	const FString& ModelId,
	FOnLLMTurnComplete OnComplete,
	FOnLLMStreamProgress OnProgress)
{
	FClaudeCodeSession* S = Sessions.Find(Session.SessionId);
	if (!S)
	{
		UE_LOG(LogUnrealClaude, Error, TEXT("FClaudeCodeBackend: Invalid session handle"));
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(FLLMTurnResult::Error(TEXT("Invalid session handle")));
		}
		return false;
	}

	if (IsBusy())
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("FClaudeCodeBackend: Already executing a request"));
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(FLLMTurnResult::Error(TEXT("Backend is busy")));
		}
		return false;
	}

	// Build the CLI request config
	FClaudeRequestConfig Config;
	Config.Prompt = BuildPromptWithHistory(*S, UserMessage);
	Config.WorkingDirectory = FPaths::ProjectDir();
	Config.bSkipPermissions = true;
	Config.AllowedTools = {
		TEXT("Read"), TEXT("Grep"), TEXT("Glob"),
		TEXT("Edit(Docs/**)"), TEXT("Edit(ARCHITECTURE.md)"), TEXT("Edit(CLAUDE.md)"),
		TEXT("Write(Docs/**)"), TEXT("Write(ARCHITECTURE.md)"),
	};
	Config.Model = ModelId;
	Config.AttachedImagePaths = ImagePaths;

	// Build system prompt from session data
	Config.SystemPrompt = S->SystemPrompt;
	for (const FString& Block : S->ContextBlocks)
	{
		Config.SystemPrompt += TEXT("\n\n") + Block;
	}

	// Set up stream event forwarding — capture token usage from Result events
	FGuid SessionId = Session.SessionId;
	FString CapturedModelId = ModelId;

	// Accumulated token data from stream events
	TSharedPtr<FLLMTokenUsage> AccumulatedUsage = MakeShared<FLLMTokenUsage>();
	AccumulatedUsage->ModelId = ModelId;
	AccumulatedUsage->ProviderId = TEXT("claude-code");

	Config.OnStreamEvent.BindLambda([OnProgress, AccumulatedUsage](const FClaudeStreamEvent& Event)
	{
		// Capture token usage from Result event
		if (Event.Type == EClaudeStreamEventType::Result)
		{
			AccumulatedUsage->InputTokens = Event.InputTokens;
			AccumulatedUsage->OutputTokens = Event.OutputTokens;
			AccumulatedUsage->EstimatedCostUsd = Event.TotalCostUsd;
		}

		// Forward to caller
		if (OnProgress.IsBound())
		{
			OnProgress.Execute(Event);
		}
	});

	// Wrap the completion callback
	FOnClaudeResponse WrappedComplete;
	WrappedComplete.BindLambda([this, SessionId, UserMessage, OnComplete, AccumulatedUsage]
		(const FString& Response, bool bSuccess)
	{
		// Record exchange in session history
		FClaudeCodeSession* Sess = Sessions.Find(SessionId);
		if (Sess && bSuccess)
		{
			Sess->History.Add(TPair<FString, FString>(UserMessage, Response));
		}

		// Build turn result
		FLLMTurnResult Result;
		Result.ResponseText = Response;
		Result.bSuccess = bSuccess;
		Result.TokenUsage = *AccumulatedUsage;

		if (!bSuccess)
		{
			Result.ErrorMessage = Response;
		}

		if (OnComplete.IsBound())
		{
			OnComplete.Execute(Result);
		}
	});

	return Runner->ExecuteAsync(Config, WrappedComplete);
}

void FClaudeCodeBackend::Cancel()
{
	if (Runner.IsValid())
	{
		Runner->Cancel();
	}
}

bool FClaudeCodeBackend::IsBusy() const
{
	return Runner.IsValid() && Runner->IsExecuting();
}

// ============================================================================
// Model Discovery
// ============================================================================

TArray<FString> FClaudeCodeBackend::GetSupportedModels() const
{
	return {
		TEXT("claude-sonnet-4-6"),
		TEXT("claude-opus-4-6"),
		TEXT("claude-haiku-4-5-20251001")
	};
}

FString FClaudeCodeBackend::GetModelDisplayName(const FString& ModelId) const
{
	if (ModelId == TEXT("claude-sonnet-4-6"))       return TEXT("Claude Sonnet 4.6");
	if (ModelId == TEXT("claude-opus-4-6"))         return TEXT("Claude Opus 4.6");
	if (ModelId == TEXT("claude-haiku-4-5-20251001")) return TEXT("Claude Haiku 4.5");
	return ModelId;
}

float FClaudeCodeBackend::EstimateCost(const FString& PromptText, const FString& ModelId) const
{
	// CLI cost is opaque — reported via subscription, not per-token
	// Return -1 to indicate estimation not supported for this backend
	return -1.0f;
}

// ============================================================================
// History Building
// ============================================================================

FString FClaudeCodeBackend::BuildPromptWithHistory(const FClaudeCodeSession& Session, const FString& NewMessage) const
{
	if (Session.History.Num() == 0)
	{
		return NewMessage;
	}

	FString PromptWithHistory;

	int32 StartIndex = FMath::Max(0, Session.History.Num() - UnrealClaudeConstants::Session::MaxHistoryInPrompt);
	for (int32 i = StartIndex; i < Session.History.Num(); ++i)
	{
		const TPair<FString, FString>& Exchange = Session.History[i];
		PromptWithHistory += FString::Printf(TEXT("Human: %s\n\nAssistant: %s\n\n"), *Exchange.Key, *Exchange.Value);
	}

	PromptWithHistory += FString::Printf(TEXT("Human: %s"), *NewMessage);
	return PromptWithHistory;
}
