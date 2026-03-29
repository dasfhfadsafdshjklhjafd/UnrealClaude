// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IClaudeRunner.h"

// ============================================================================
// Capability Flags
// ============================================================================

/** Capability flags for backend feature detection */
enum class ELLMBackendCapability : uint8
{
	None            = 0,
	NativeSession   = 1 << 0,  // Backend maintains conversation state internally (e.g. CLI)
	ToolCalling     = 1 << 1,  // Backend supports native tool_use / function_calling
	MCP             = 1 << 2,  // Backend supports MCP server connections
	ImageInput      = 1 << 3,  // Backend accepts image content blocks
	Streaming       = 1 << 4,  // Backend supports streaming responses
	CostReporting   = 1 << 5,  // Backend reports token usage per request
};
ENUM_CLASS_FLAGS(ELLMBackendCapability);

// ============================================================================
// Model Roles
// ============================================================================

/** Model role for multi-model routing */
enum class EModelRole : uint8
{
	Worker,      // Primary task execution — daily driver
	Critic,      // Review/validation — second opinion
	Architect,   // Governance, design decisions — on-demand only
	Escalation,  // Fallback when worker is stuck — on-demand only
	DocsAgent    // Documentation/analysis agent — reads and summarizes
};

/** Get display name for a role */
inline FString GetModelRoleDisplayName(EModelRole Role)
{
	switch (Role)
	{
	case EModelRole::Worker:     return TEXT("Worker");
	case EModelRole::Critic:     return TEXT("Critic");
	case EModelRole::Architect:  return TEXT("Architect");
	case EModelRole::Escalation: return TEXT("Escalation");
	case EModelRole::DocsAgent:  return TEXT("DocsAgent");
	default:                     return TEXT("Unknown");
	}
}

/** Get short description for a role */
inline FString GetModelRoleDescription(EModelRole Role)
{
	switch (Role)
	{
	case EModelRole::Worker:     return TEXT("Main driver for daily tasks");
	case EModelRole::Critic:     return TEXT("Review and second opinion");
	case EModelRole::Architect:  return TEXT("Design governance (on-demand)");
	case EModelRole::Escalation: return TEXT("Fallback when stuck (on-demand)");
	case EModelRole::DocsAgent:  return TEXT("Documentation and analysis");
	default:                     return TEXT("");
	}
}

/** All roles for iteration */
inline TArray<EModelRole> GetAllModelRoles()
{
	return { EModelRole::Worker, EModelRole::Critic, EModelRole::Architect, EModelRole::Escalation, EModelRole::DocsAgent };
}

// ============================================================================
// Backend Type
// ============================================================================

/** Backend provider type — determines session semantics */
enum class ELLMBackendType : uint8
{
	/** Session-oriented: backend manages conversation state (CLI subprocess) */
	SessionOriented,
	/** Request/response: plugin manages conversation history, sends full context each turn */
	RequestResponse
};

// ============================================================================
// Token Usage
// ============================================================================

/** Token usage report returned from each turn */
struct UNREALCLAUDE_API FLLMTokenUsage
{
	int32 InputTokens = 0;
	int32 OutputTokens = 0;
	int32 CachedInputTokens = 0;
	FString ModelId;
	FString ProviderId;  // "claude-code", "anthropic-api", "openai-api"
	EModelRole Role = EModelRole::Worker;

	/** Estimated cost in USD (computed from pricing config) */
	float EstimatedCostUsd = 0.0f;
};

// ============================================================================
// Turn Result
// ============================================================================

/** Result from a single LLM turn */
struct UNREALCLAUDE_API FLLMTurnResult
{
	/** Full response text */
	FString ResponseText;

	/** Whether the request succeeded */
	bool bSuccess = false;

	/** Error message if bSuccess is false */
	FString ErrorMessage;

	/** Token usage data for this turn */
	FLLMTokenUsage TokenUsage;

	/** Tool call events (reuses existing stream event type) */
	TArray<FClaudeStreamEvent> ToolCalls;

	static FLLMTurnResult Success(const FString& Response, const FLLMTokenUsage& Usage)
	{
		FLLMTurnResult R;
		R.ResponseText = Response;
		R.bSuccess = true;
		R.TokenUsage = Usage;
		return R;
	}

	static FLLMTurnResult Error(const FString& Message)
	{
		FLLMTurnResult R;
		R.bSuccess = false;
		R.ErrorMessage = Message;
		return R;
	}
};

// ============================================================================
// Delegates
// ============================================================================

DECLARE_DELEGATE_OneParam(FOnLLMTurnComplete, const FLLMTurnResult& /*Result*/);
DECLARE_DELEGATE_OneParam(FOnLLMStreamProgress, const FClaudeStreamEvent& /*Event*/);

// ============================================================================
// Session Handle
// ============================================================================

/** Opaque session handle returned by CreateTaskSession */
struct UNREALCLAUDE_API FLLMSessionHandle
{
	FGuid SessionId;

	bool IsValid() const { return SessionId.IsValid(); }

	static FLLMSessionHandle Create()
	{
		FLLMSessionHandle H;
		H.SessionId = FGuid::NewGuid();
		return H;
	}

	static FLLMSessionHandle Invalid()
	{
		return FLLMSessionHandle();
	}

	bool operator==(const FLLMSessionHandle& Other) const { return SessionId == Other.SessionId; }
	friend uint32 GetTypeHash(const FLLMSessionHandle& H) { return GetTypeHash(H.SessionId); }
};

// ============================================================================
// ILLMBackend Interface
// ============================================================================

/**
 * Abstract interface for LLM backends (Claude Code CLI, Anthropic API, OpenAI API).
 *
 * Two backend types exist:
 *   SessionOriented — backend manages its own conversation state (CLI subprocess).
 *   RequestResponse — plugin manages conversation history and sends full context each turn.
 *
 * All callbacks (OnComplete, OnProgress) are guaranteed to fire on the game thread.
 */
class UNREALCLAUDE_API ILLMBackend
{
public:
	virtual ~ILLMBackend() = default;

	// ---- Identity ----

	/** Human-readable backend name (e.g. "Claude Code CLI", "Anthropic API") */
	virtual FString GetBackendName() const = 0;

	/** Provider identifier for cost tracking and config (e.g. "claude-code", "anthropic-api") */
	virtual FString GetProviderId() const = 0;

	/** Whether this is session-oriented or request/response */
	virtual ELLMBackendType GetBackendType() const = 0;

	// ---- Capabilities ----

	/** Query capability flags */
	virtual ELLMBackendCapability GetCapabilities() const = 0;

	/** Check for a specific capability */
	bool HasCapability(ELLMBackendCapability Cap) const
	{
		return EnumHasAllFlags(GetCapabilities(), Cap);
	}

	// ---- Availability ----

	/** Check if backend is configured and ready (API key set, CLI available, etc.) */
	virtual bool IsAvailable() const = 0;

	/** Get human-readable reason if not available (e.g. "API key not set") */
	virtual FString GetUnavailableReason() const = 0;

	// ---- Session Management ----

	/**
	 * Create a new task session.
	 * For session-oriented backends: may start a subprocess or allocate internal state.
	 * For request/response backends: creates an in-memory conversation buffer.
	 */
	virtual FLLMSessionHandle CreateTaskSession(const FString& SystemPrompt) = 0;

	/**
	 * Append additional context to a session (e.g. injected file contents, UE context).
	 * For session-oriented backends this may be a no-op.
	 * For request/response backends this adds a system message to the history.
	 */
	virtual void AppendContext(const FLLMSessionHandle& Session, const FString& ContextText) = 0;

	/**
	 * Inject prior conversation exchanges into a session as proper user/assistant turns.
	 * Used to seed an API session with cross-role history from the shared SessionManager.
	 * No-op for session-oriented backends (CLI manages its own history).
	 */
	virtual void SeedHistory(const FLLMSessionHandle& Session, const TArray<TPair<FString, FString>>& Exchanges) {}

	/** Destroy a session and free associated resources */
	virtual void DestroySession(const FLLMSessionHandle& Session) = 0;

	// ---- Execution ----

	/**
	 * Submit a user turn and get a response asynchronously.
	 * @param Session       Session handle from CreateTaskSession
	 * @param UserMessage   The user's message text
	 * @param ImagePaths    Optional paths to attached images
	 * @param ModelId       Model to use for this turn (e.g. "claude-sonnet-4-6", "gpt-4o")
	 * @param OnComplete    Called on game thread when turn completes
	 * @param OnProgress    Optional streaming progress callback (game thread)
	 * @return true if the request was submitted successfully
	 */
	virtual bool SubmitTurn(
		const FLLMSessionHandle& Session,
		const FString& UserMessage,
		const TArray<FString>& ImagePaths,
		const FString& ModelId,
		FOnLLMTurnComplete OnComplete,
		FOnLLMStreamProgress OnProgress = FOnLLMStreamProgress()
	) = 0;

	/** Cancel the current in-flight request */
	virtual void Cancel() = 0;

	/** Check if a request is currently in flight */
	virtual bool IsBusy() const = 0;

	// ---- Model Discovery ----

	/** Get list of model IDs this backend supports */
	virtual TArray<FString> GetSupportedModels() const = 0;

	/** Get display name for a model ID (e.g. "claude-sonnet-4-6" -> "Claude Sonnet 4.6") */
	virtual FString GetModelDisplayName(const FString& ModelId) const = 0;

	// ---- Cost Estimation ----

	/**
	 * Estimate cost for a prompt before sending (rough token count * pricing).
	 * Returns estimated USD cost. Returns -1.0 if estimation is not supported.
	 */
	virtual float EstimateCost(const FString& PromptText, const FString& ModelId) const = 0;
};
