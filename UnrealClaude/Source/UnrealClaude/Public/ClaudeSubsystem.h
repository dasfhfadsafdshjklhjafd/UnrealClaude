// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IClaudeRunner.h"
#include "LLM/ILLMBackend.h"

// Forward declarations
class FClaudeSessionManager;
class FClaudeCodeRunner;
class FLLMBackendRegistry;
class FLLMRoleManager;
class FLLMTokenTracker;
class FLLMPricingConfig;

/**
 * Options for sending a prompt to Claude
 * Reduces parameter count in SendPrompt method
 */
struct UNREALCLAUDE_API FClaudePromptOptions
{
	/** Include UE5.7 engine context in system prompt */
	bool bIncludeEngineContext = true;

	/** Include project-specific context in system prompt */
	bool bIncludeProjectContext = true;

	/** Optional callback for streaming output progress */
	FOnClaudeProgress OnProgress;

	/** Optional callback for structured NDJSON stream events */
	FOnClaudeStreamEvent OnStreamEvent;

	/** Optional paths to attached clipboard images (PNG) */
	TArray<FString> AttachedImagePaths;

	/** Role for this request (controls write permissions — only DocsAgent can write) */
	EModelRole Role = EModelRole::Worker;

	/** Model override for CLI path (set by SendPromptViaBackend when routing role requests) */
	FString ModelOverride;

	/** Original user message to store in session history (if empty, the actual prompt is stored).
	 *  Set when sending an augmented prompt (role prefix + user message) so history stays clean. */
	FString HistoryPrompt;

	/** Default constructor with sensible defaults */
	FClaudePromptOptions() = default;

	/** Convenience constructor for common case */
	FClaudePromptOptions(bool bEngineContext, bool bProjectContext)
		: bIncludeEngineContext(bEngineContext)
		, bIncludeProjectContext(bProjectContext)
	{}
};

/**
 * Subsystem for managing Claude Code interactions
 * Orchestrates runner, session management, and prompt building
 */
class UNREALCLAUDE_API FClaudeCodeSubsystem
{
public:
	static FClaudeCodeSubsystem& Get();

	/** Destructor - must be defined in cpp where full types are available */
	~FClaudeCodeSubsystem();

	/** Send a prompt to Claude with optional context (new API with options struct) */
	void SendPrompt(
		const FString& Prompt,
		FOnClaudeResponse OnComplete,
		const FClaudePromptOptions& Options = FClaudePromptOptions()
	);

	/** Send a prompt to Claude with optional context (legacy API for backward compatibility) */
	void SendPrompt(
		const FString& Prompt,
		FOnClaudeResponse OnComplete,
		bool bIncludeUE57Context,
		FOnClaudeProgress OnProgress,
		bool bIncludeProjectContext = true
	);

	/** Get the default UE5.7 system prompt */
	FString GetUE57SystemPrompt() const;

	/** Get the project context prompt */
	FString GetProjectContextPrompt() const;

	/** Get project instructions from CLAUDE.md (for API backends that don't auto-discover it) */
	FString GetProjectInstructionsPrompt() const;

	/** Get ARCHITECTURE.md content for injection into API system prompt */
	FString GetArchitectureContextPrompt() const;

	/** Get kanban/task board content for injection into API system prompt */
	FString GetKanbanContextPrompt() const;

	/** Get agentic behavior instructions for API backends */
	FString GetAPIBehaviorPrompt() const;

	/** Check if the active backend is an API (not CLI) */
	bool IsAPIBackend() const { return ActiveBackendId != TEXT("claude-code"); }

	/** Set custom system prompt additions */
	void SetCustomSystemPrompt(const FString& InCustomPrompt);

	/** Get/Set the active Claude model (e.g. "claude-sonnet-4-6") */
	void SetModel(const FString& InModel) { SelectedModel = InModel; }
	const FString& GetModel() const { return SelectedModel; }

	/** Get conversation history (delegates to session manager) */
	const TArray<TPair<FString, FString>>& GetHistory() const;

	/** Clear conversation history */
	void ClearHistory();

	/** Inject a synthetic exchange into history (used after compact to seed summary as context) */
	void AddExchange(const FString& Prompt, const FString& Response);

	/** Cancel current request */
	void CancelCurrentRequest();

	/** Save current session to disk */
	bool SaveSession();

	/** Load previous session from disk */
	bool LoadSession();

	/** Load a specific archived session file */
	bool LoadSessionFromFile(const FString& FilePath);

	/** Get the session save directory */
	FString GetSessionDir() const;

	/** Check if a previous session exists */
	bool HasSavedSession() const;

	/** Get session file path */
	FString GetSessionFilePath() const;

	/** Get the runner interface (for testing/mocking) */
	IClaudeRunner* GetRunner() const;

	// ---- Multi-Backend API ----

	/** Get the backend registry */
	FLLMBackendRegistry& GetBackendRegistry();

	/** Get the role manager */
	FLLMRoleManager& GetRoleManager();

	/** Get the token tracker */
	FLLMTokenTracker& GetTokenTracker();

	/** Get the pricing config */
	FLLMPricingConfig& GetPricingConfig();

	/** Get/Set the active backend provider ID (e.g. "claude-code", "anthropic-api", "openai-api") */
	void SetActiveBackendId(const FString& ProviderId);
	const FString& GetActiveBackendId() const { return ActiveBackendId; }

	/** Get the active backend (resolved from ActiveBackendId). May return nullptr. */
	ILLMBackend* GetActiveBackend() const;

	/**
	 * Send a prompt through the active LLM backend.
	 * Routes through the ILLMBackend interface for API backends,
	 * or falls back to the CLI runner for "claude-code".
	 */
	void SendPromptViaBackend(
		const FString& Prompt,
		FOnLLMTurnComplete OnComplete,
		const FClaudePromptOptions& Options = FClaudePromptOptions(),
		EModelRole Role = EModelRole::Worker
	);

private:
	FClaudeCodeSubsystem();

	/** Build prompt with conversation history context */
	FString BuildPromptWithHistory(const FString& NewPrompt) const;

	/** Initialize the LLM backend system */
	void InitializeLLMSystem();

	TUniquePtr<FClaudeCodeRunner> Runner;
	TUniquePtr<FClaudeSessionManager> SessionManager;
	FString CustomSystemPrompt;
	FString SelectedModel = TEXT("claude-sonnet-4-6");

	// ---- Multi-Backend Members ----
	TUniquePtr<FLLMBackendRegistry> BackendRegistry;
	TUniquePtr<FLLMRoleManager> RoleManager;
	TUniquePtr<FLLMTokenTracker> TokenTracker;
	TUniquePtr<FLLMPricingConfig> PricingConfig;
	FString ActiveBackendId = TEXT("claude-code");
	FLLMSessionHandle ActiveSession;
};
