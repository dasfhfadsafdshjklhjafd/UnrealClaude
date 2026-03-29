// Copyright Natali Caggiano. All Rights Reserved.

#include "ClaudeSubsystem.h"
#include "ClaudeCodeRunner.h"
#include "ClaudeSessionManager.h"
#include "ProjectContext.h"
#include "ScriptExecutionManager.h"
#include "UnrealClaudeModule.h"
#include "UnrealClaudeConstants.h"
#include "LLM/LLMBackendRegistry.h"
#include "LLM/LLMRoleConfig.h"
#include "LLM/LLMTokenTracker.h"
#include "LLM/LLMPricingConfig.h"
#include "LLM/FClaudeCodeBackend.h"
#include "LLM/FAnthropicAPIBackend.h"
#include "LLM/FOpenAIAPIBackend.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"

// Cached system prompt - static to avoid recreation on each call
static const FString CachedUE57SystemPrompt = TEXT(R"(You are an expert Unreal Engine 5.7 assistant working inside the UE Editor on a Blueprint-only multiplayer FPS project.

ALL TOOLS ARE READ-ONLY. You cannot modify Blueprints, assets, levels, or source files.

AVAILABLE MCP TOOLS (all read-only):
- blueprint_query — inspect Blueprint variables, functions, components
- blueprint_read_graph — read node graphs; lists ALL graphs including AnimBP state machines/transitions/conduits; extracts PropertyAccess bindings on AnimGraph nodes
- asset_search / asset_dependencies / asset_referencers — find and trace assets
- get_level_actors — list actors in the current level
- get_output_log — read editor output log (supports filtering, cursor-based incremental reads)
- capture_viewport — screenshot the viewport
- open_level — list available map templates (open/new/save disabled)
- widget_editor — inspect UMG Widget Blueprint trees and properties (inspect_tree, get_properties only)
- blend_space — inspect BlendSpace and AimOffset assets (inspect, list only)
- montage_modify — inspect AnimMontage structure (get_info, get_curves only)
- anim_edit — inspect animation tracks and skeletal meshes (inspect_track, inspect_mesh only)

HOW TO READ A GRAPH:
1. blueprint_query(operation="inspect", blueprint_path="...", include_functions=true)
2. blueprint_read_graph(blueprint_path="...")                        <- lists all graphs
3. blueprint_read_graph(blueprint_path="...", graph_name="MyGraph") <- reads nodes
Use start_node + max_nodes to page large graphs (EventGraph can have 200+ nodes).

RULES:
- Read ARCHITECTURE.md before proposing any solution
- Be concise; avoid restating what you just read)");

FClaudeCodeSubsystem& FClaudeCodeSubsystem::Get()
{
	static FClaudeCodeSubsystem Instance;
	return Instance;
}

FClaudeCodeSubsystem::FClaudeCodeSubsystem()
{
	Runner = MakeUnique<FClaudeCodeRunner>();
	SessionManager = MakeUnique<FClaudeSessionManager>();
	InitializeLLMSystem();
}

FClaudeCodeSubsystem::~FClaudeCodeSubsystem()
{
	// Destructor defined here where full types are available
	// TUniquePtr will properly destroy the objects
}

IClaudeRunner* FClaudeCodeSubsystem::GetRunner() const
{
	return Runner.Get();
}

void FClaudeCodeSubsystem::SendPrompt(
	const FString& Prompt,
	FOnClaudeResponse OnComplete,
	const FClaudePromptOptions& Options)
{
	FClaudeRequestConfig Config;

	// Build prompt with conversation history context
	Config.Prompt = BuildPromptWithHistory(Prompt);
	Config.WorkingDirectory = FPaths::ProjectDir();
	Config.bSkipPermissions = true;
	Config.AllowedTools = { TEXT("Read"), TEXT("Grep"), TEXT("Glob") };

	// Only DocsAgent role gets write access to documentation files
	if (Options.Role == EModelRole::DocsAgent)
	{
		Config.AllowedTools.Append({
			TEXT("Edit(Docs/**)"), TEXT("Edit(ARCHITECTURE.md)"), TEXT("Edit(CLAUDE.md)"),
			TEXT("Write(Docs/**)"), TEXT("Write(ARCHITECTURE.md)"),
		});
	}
	Config.Model = Options.ModelOverride.IsEmpty() ? SelectedModel : Options.ModelOverride;

	Config.AttachedImagePaths = Options.AttachedImagePaths;

	if (Options.bIncludeEngineContext)
	{
		Config.SystemPrompt = GetUE57SystemPrompt();
	}

	if (Options.bIncludeProjectContext)
	{
		Config.SystemPrompt += GetProjectContextPrompt();
	}

	if (!CustomSystemPrompt.IsEmpty())
	{
		Config.SystemPrompt += TEXT("\n\n") + CustomSystemPrompt;
	}

	// Pass structured event delegate through to runner config
	Config.OnStreamEvent = Options.OnStreamEvent;

	// Wrap completion to store history and save session
	// Use HistoryPrompt if set (clean user message without role prefix), else fall back to Prompt
	FString HistoryKey = Options.HistoryPrompt.IsEmpty() ? Prompt : Options.HistoryPrompt;
	FOnClaudeResponse WrappedComplete;
	WrappedComplete.BindLambda([this, HistoryKey, OnComplete](const FString& Response, bool bSuccess)
	{
		if (bSuccess && SessionManager.IsValid())
		{
			SessionManager->AddExchange(HistoryKey, Response);
			SessionManager->SaveSession();
		}
		OnComplete.ExecuteIfBound(Response, bSuccess);
	});

	Runner->ExecuteAsync(Config, WrappedComplete, Options.OnProgress);
}

void FClaudeCodeSubsystem::SendPrompt(
	const FString& Prompt,
	FOnClaudeResponse OnComplete,
	bool bIncludeUE57Context,
	FOnClaudeProgress OnProgress,
	bool bIncludeProjectContext)
{
	// Legacy API - delegate to new API
	FClaudePromptOptions Options;
	Options.bIncludeEngineContext = bIncludeUE57Context;
	Options.bIncludeProjectContext = bIncludeProjectContext;
	Options.OnProgress = OnProgress;
	SendPrompt(Prompt, OnComplete, Options);
}

FString FClaudeCodeSubsystem::GetUE57SystemPrompt() const
{
	// Return cached static prompt to avoid string recreation
	return CachedUE57SystemPrompt;
}

FString FClaudeCodeSubsystem::GetProjectContextPrompt() const
{
	FString Context = FProjectContextManager::Get().FormatContextForPrompt();

	// Add script execution history (last 10 scripts)
	FString ScriptHistory = FScriptExecutionManager::Get().FormatHistoryForContext(10);
	if (!ScriptHistory.IsEmpty())
	{
		Context += TEXT("\n\n") + ScriptHistory;
	}

	return Context;
}

FString FClaudeCodeSubsystem::GetProjectInstructionsPrompt() const
{
	// Read CLAUDE.md from project root — this gives API backends the same project instructions
	// that Claude Code CLI discovers automatically
	FString ClaudeMdPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("CLAUDE.md"));
	FString Contents;
	if (FFileHelper::LoadFileToString(Contents, *ClaudeMdPath))
	{
		return FString::Printf(TEXT("\n\n--- Project Instructions (CLAUDE.md) ---\n%s"), *Contents);
	}
	return FString();
}

FString FClaudeCodeSubsystem::GetArchitectureContextPrompt() const
{
	FString ArchPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("ARCHITECTURE.md"));
	FString Contents;
	if (FFileHelper::LoadFileToString(Contents, *ArchPath))
	{
		UE_LOG(LogUnrealClaude, Log, TEXT("GetArchitectureContextPrompt: Loaded ARCHITECTURE.md (%d chars)"), Contents.Len());
		return FString::Printf(TEXT("\n\n--- ARCHITECTURE.md (ground truth — already injected, do NOT attempt to read from disk) ---\n%s"), *Contents);
	}
	UE_LOG(LogUnrealClaude, Warning, TEXT("GetArchitectureContextPrompt: ARCHITECTURE.md not found at: %s"), *ArchPath);
	return FString();
}

FString FClaudeCodeSubsystem::GetKanbanContextPrompt() const
{
	// Extract kanban file path from CLAUDE.md (look for backtick-enclosed path containing "kanban")
	// This avoids hardcoding an absolute path in C++ — the project's CLAUDE.md is the source of truth
	FString ClaudeMdPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("CLAUDE.md"));
	FString ClaudeMd;
	if (!FFileHelper::LoadFileToString(ClaudeMd, *ClaudeMdPath))
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("GetKanbanContextPrompt: Could not read CLAUDE.md at: %s"), *ClaudeMdPath);
		return FString();
	}

	// Find pattern: `<path>/kanban.md` in CLAUDE.md
	FString KanbanPath;
	int32 Idx = ClaudeMd.Find(TEXT("kanban.md"), ESearchCase::IgnoreCase);
	if (Idx == INDEX_NONE)
	{
		UE_LOG(LogUnrealClaude, Log, TEXT("GetKanbanContextPrompt: No 'kanban.md' reference found in CLAUDE.md"));
		return FString();
	}

	// Walk backward to find opening backtick
	int32 BacktickStart = INDEX_NONE;
	for (int32 i = Idx - 1; i >= 0; --i)
	{
		if (ClaudeMd[i] == TEXT('`'))
		{
			BacktickStart = i + 1;
			break;
		}
		if (ClaudeMd[i] == TEXT('\n'))
		{
			UE_LOG(LogUnrealClaude, Warning, TEXT("GetKanbanContextPrompt: Hit newline before finding opening backtick"));
			break;
		}
	}
	// Walk forward to find closing backtick
	int32 BacktickEnd = INDEX_NONE;
	for (int32 i = Idx; i < ClaudeMd.Len(); ++i)
	{
		if (ClaudeMd[i] == TEXT('`'))
		{
			BacktickEnd = i;
			break;
		}
		if (ClaudeMd[i] == TEXT('\n'))
		{
			UE_LOG(LogUnrealClaude, Warning, TEXT("GetKanbanContextPrompt: Hit newline before finding closing backtick"));
			break;
		}
	}

	if (BacktickStart == INDEX_NONE || BacktickEnd == INDEX_NONE)
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("GetKanbanContextPrompt: Could not extract path from backticks (start=%d, end=%d)"), BacktickStart, BacktickEnd);
		return FString();
	}

	KanbanPath = ClaudeMd.Mid(BacktickStart, BacktickEnd - BacktickStart);
	UE_LOG(LogUnrealClaude, Log, TEXT("GetKanbanContextPrompt: Extracted kanban path: '%s'"), *KanbanPath);

	// Normalize path separators
	KanbanPath.ReplaceInline(TEXT("\\"), TEXT("/"));

	FString KanbanContents;
	if (FFileHelper::LoadFileToString(KanbanContents, *KanbanPath))
	{
		UE_LOG(LogUnrealClaude, Log, TEXT("GetKanbanContextPrompt: Successfully loaded kanban (%d chars)"), KanbanContents.Len());
		return FString::Printf(TEXT("\n\n--- Task Board (kanban.md — already injected into your context, do NOT attempt to read this file from disk) ---\n%s"), *KanbanContents);
	}

	UE_LOG(LogUnrealClaude, Warning, TEXT("GetKanbanContextPrompt: Kanban file not found at: %s"), *KanbanPath);
	return FString();
}

FString FClaudeCodeSubsystem::GetAPIBehaviorPrompt() const
{
	return TEXT(R"(

--- API Backend Behavior Instructions ---
You are running as a direct API backend (not Claude CLI). Adapt your behavior:

TOOL USAGE — BE AGENTIC:
- ALWAYS use tools to verify before answering. Do not guess or assume.
- Chain multiple tool calls: inspect → read → grep → compare → answer.
- If asked about a Blueprint, use blueprint_query first, THEN read the relevant graph.
- If asked about a bug, read the output log, inspect the Blueprint, cross-reference ARCHITECTURE.md — THEN diagnose.
- Never say "I would need to check..." — just check. You have the tools.

RESPONSE STYLE:
- Be concise and direct. Lead with the answer, not the reasoning.
- Do not hedge or ask permission to use tools. Just use them.
- Do not summarize what you just read back to the user unless they asked.
- Do not repeat the user's question back to them.

CONTEXT ALREADY IN YOUR SYSTEM PROMPT:
- ARCHITECTURE.md (ground truth decisions) — already injected, do NOT read from disk
- Task board (kanban.md) — already injected, do NOT read from disk
- CLAUDE.md (project instructions) — already injected, do NOT read from disk
- Reference these documents directly from your context. Only use tools for Blueprints, assets, and the output log.)");
}

void FClaudeCodeSubsystem::SetCustomSystemPrompt(const FString& InCustomPrompt)
{
	CustomSystemPrompt = InCustomPrompt;
}

const TArray<TPair<FString, FString>>& FClaudeCodeSubsystem::GetHistory() const
{
	static TArray<TPair<FString, FString>> EmptyHistory;
	if (SessionManager.IsValid())
	{
		return SessionManager->GetHistory();
	}
	return EmptyHistory;
}

void FClaudeCodeSubsystem::ClearHistory()
{
	if (SessionManager.IsValid())
	{
		SessionManager->ClearHistory();
		ActiveSessionSyncedHistoryCount = 0;
	}
}

void FClaudeCodeSubsystem::AddExchange(const FString& Prompt, const FString& Response)
{
	if (SessionManager.IsValid())
	{
		SessionManager->AddExchange(Prompt, Response);
	}
}

void FClaudeCodeSubsystem::CancelCurrentRequest()
{
	if (Runner.IsValid())
	{
		Runner->Cancel();
	}
}

bool FClaudeCodeSubsystem::SaveSession()
{
	if (SessionManager.IsValid())
	{
		return SessionManager->SaveSession();
	}
	return false;
}

bool FClaudeCodeSubsystem::LoadSession()
{
	if (SessionManager.IsValid())
	{
		return SessionManager->LoadSession();
	}
	return false;
}

bool FClaudeCodeSubsystem::HasSavedSession() const
{
	if (SessionManager.IsValid())
	{
		return SessionManager->HasSavedSession();
	}
	return false;
}

FString FClaudeCodeSubsystem::GetSessionFilePath() const
{
	if (SessionManager.IsValid())
	{
		return SessionManager->GetSessionFilePath();
	}
	return FString();
}

bool FClaudeCodeSubsystem::LoadSessionFromFile(const FString& FilePath)
{
	if (SessionManager.IsValid())
	{
		return SessionManager->LoadSessionFromFile(FilePath);
	}
	return false;
}

FString FClaudeCodeSubsystem::GetSessionDir() const
{
	if (SessionManager.IsValid())
	{
		return SessionManager->GetSessionDir();
	}
	return FString();
}

FString FClaudeCodeSubsystem::BuildPromptWithHistory(const FString& NewPrompt) const
{
	if (!SessionManager.IsValid())
	{
		return NewPrompt;
	}

	const TArray<TPair<FString, FString>>& History = SessionManager->GetHistory();
	if (History.Num() == 0)
	{
		return NewPrompt;
	}

	FString PromptWithHistory;

	// Include recent history (limit to last N exchanges)
	int32 StartIndex = FMath::Max(0, History.Num() - UnrealClaudeConstants::Session::MaxHistoryInPrompt);

	for (int32 i = StartIndex; i < History.Num(); ++i)
	{
		const TPair<FString, FString>& Exchange = History[i];
		PromptWithHistory += FString::Printf(TEXT("Human: %s\n\nAssistant: %s\n\n"), *Exchange.Key, *Exchange.Value);
	}

	PromptWithHistory += FString::Printf(TEXT("Human: %s"), *NewPrompt);

	return PromptWithHistory;
}

// ============================================================================
// Multi-Backend System
// ============================================================================

void FClaudeCodeSubsystem::InitializeLLMSystem()
{
	// Create registry and register all backends
	BackendRegistry = MakeUnique<FLLMBackendRegistry>();
	BackendRegistry->RegisterBackend(MakeShared<FClaudeCodeBackend>());
	BackendRegistry->RegisterBackend(MakeShared<FAnthropicAPIBackend>());
	BackendRegistry->RegisterBackend(MakeShared<FOpenAIAPIBackend>());

	// Load role config
	RoleManager = MakeUnique<FLLMRoleManager>();
	RoleManager->LoadFromConfig();

	// Initialize token tracker
	TokenTracker = MakeUnique<FLLMTokenTracker>();
	FString SaveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealClaude"));
	TokenTracker->Initialize(SaveDir);

	// Load pricing config
	PricingConfig = MakeUnique<FLLMPricingConfig>();
	// Try plugin Resources/ directory
	FString PricingPath;
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealClaude"));
	if (Plugin.IsValid())
	{
		PricingPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("llm-pricing.json"));
	}
	if (PricingPath.IsEmpty() || !FPaths::FileExists(PricingPath))
	{
		// Fallback: check relative to plugin content
		PricingPath = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UnrealClaude"), TEXT("Resources"), TEXT("llm-pricing.json"));
	}
	if (FPaths::FileExists(PricingPath))
	{
		PricingConfig->LoadFromFile(PricingPath);
	}
	else
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("LLM pricing config not found at: %s"), *PricingPath);
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("LLM system initialized: %d backends, %d roles configured"),
		BackendRegistry->GetAllBackends().Num(), RoleManager->GetAllAssignments().Num());
}

FLLMBackendRegistry& FClaudeCodeSubsystem::GetBackendRegistry()
{
	return *BackendRegistry;
}

FLLMRoleManager& FClaudeCodeSubsystem::GetRoleManager()
{
	return *RoleManager;
}

FLLMTokenTracker& FClaudeCodeSubsystem::GetTokenTracker()
{
	return *TokenTracker;
}

FLLMPricingConfig& FClaudeCodeSubsystem::GetPricingConfig()
{
	return *PricingConfig;
}

void FClaudeCodeSubsystem::SetActiveBackendId(const FString& ProviderId)
{
	if (BackendRegistry->HasBackend(ProviderId))
	{
		// Only destroy session if backend actually changes
		if (ActiveBackendId != ProviderId)
		{
			// Destroy session on the OLD backend before switching
			ILLMBackend* OldBackend = GetActiveBackend();
			if (OldBackend && ActiveSession.IsValid())
			{
				OldBackend->DestroySession(ActiveSession);
				ActiveSession = FLLMSessionHandle::Invalid();
				ActiveSessionSyncedHistoryCount = 0;
			}

			ActiveBackendId = ProviderId;
			UE_LOG(LogUnrealClaude, Log, TEXT("Active backend set to: %s"), *ProviderId);
		}
	}
	else
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("Unknown backend provider: %s"), *ProviderId);
	}
}

ILLMBackend* FClaudeCodeSubsystem::GetActiveBackend() const
{
	return BackendRegistry->GetBackend(ActiveBackendId);
}

void FClaudeCodeSubsystem::SendPromptViaBackend(
	const FString& Prompt,
	FOnLLMTurnComplete OnComplete,
	const FClaudePromptOptions& Options,
	EModelRole Role)
{
	// Resolve model: use role assignment if it targets this backend, else use selected model
	FModelRoleAssignment Assignment = RoleManager->GetAssignment(Role);
	FString ModelId = SelectedModel;
	FString AssignmentBackend = BackendRegistry->ResolveBackendForModel(Assignment.ModelId);
	if (AssignmentBackend == ActiveBackendId)
	{
		ModelId = Assignment.ModelId;
	}

	// If using Claude Code CLI backend, delegate to existing SendPrompt path
	if (ActiveBackendId == TEXT("claude-code"))
	{
		// Wrap the LLM callback into the legacy callback format
		FOnClaudeResponse LegacyComplete;
		LegacyComplete.BindLambda([this, OnComplete, Role, ModelId](const FString& Response, bool bSuccess)
		{
			FLLMTurnResult Result;
			Result.ResponseText = Response;
			Result.bSuccess = bSuccess;
			if (!bSuccess)
			{
				Result.ErrorMessage = Response;
			}
			// Token usage will be populated from stream events
			Result.TokenUsage.ProviderId = TEXT("claude-code");
			Result.TokenUsage.ModelId = ModelId;
			Result.TokenUsage.Role = Role;

			if (OnComplete.IsBound())
			{
				OnComplete.Execute(Result);
			}
		});

		// Pass role and resolved model through so SendPrompt uses the right model
		FClaudePromptOptions CLIOptions = Options;
		CLIOptions.Role = Role;
		CLIOptions.ModelOverride = ModelId;
		SendPrompt(Prompt, LegacyComplete, CLIOptions);
		return;
	}

	// Direct API backend path
	ILLMBackend* Backend = GetActiveBackend();
	if (!Backend)
	{
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(FLLMTurnResult::Error(
				FString::Printf(TEXT("Backend '%s' not found"), *ActiveBackendId)));
		}
		return;
	}

	if (!Backend->IsAvailable())
	{
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(FLLMTurnResult::Error(Backend->GetUnavailableReason()));
		}
		return;
	}

	// Ensure session exists
	if (!ActiveSession.IsValid())
	{
		FString SystemPrompt;
		if (Options.bIncludeEngineContext)
		{
			SystemPrompt = GetUE57SystemPrompt();
		}
		if (Options.bIncludeProjectContext)
		{
			SystemPrompt += GetProjectContextPrompt();
		}
		// API backends don't auto-discover CLAUDE.md — inject project instructions
		SystemPrompt += GetProjectInstructionsPrompt();
		// Inject key project files that MCP tools can't read (they only handle Blueprints/assets)
		FString ArchitecturePrompt = GetArchitectureContextPrompt();
		SystemPrompt += ArchitecturePrompt;
		FString KanbanPrompt = GetKanbanContextPrompt();
		SystemPrompt += KanbanPrompt;
		// Inject agentic behavior instructions for API models
		SystemPrompt += GetAPIBehaviorPrompt();
		if (!CustomSystemPrompt.IsEmpty())
		{
			SystemPrompt += TEXT("\n\n") + CustomSystemPrompt;
		}

		UE_LOG(LogUnrealClaude, Log, TEXT("API session created — system prompt: %d chars, architecture: %s, kanban: %s"),
			SystemPrompt.Len(),
			ArchitecturePrompt.IsEmpty() ? TEXT("NO") : TEXT("YES"),
			KanbanPrompt.IsEmpty() ? TEXT("NO") : TEXT("YES"));

		ActiveSession = Backend->CreateTaskSession(SystemPrompt);
	}

	// Submit turn (ModelId already resolved above, before the CLI early-return)
	// Use HistoryPrompt if set (clean user message without role prefix), else fall back to Prompt
	FString HistoryKey = Options.HistoryPrompt.IsEmpty() ? Prompt : Options.HistoryPrompt;
	FOnLLMTurnComplete WrappedComplete;
	WrappedComplete.BindLambda([this, HistoryKey, OnComplete, Role, ModelId](const FLLMTurnResult& Result)
	{
		// Record in session history
		if (Result.bSuccess && SessionManager.IsValid())
		{
			SessionManager->AddExchange(HistoryKey, Result.ResponseText);
			SessionManager->SaveSession();
		}

		// Track token usage
		if (TokenTracker.IsValid() && PricingConfig.IsValid())
		{
			FLLMTokenUsage Usage = Result.TokenUsage;
			Usage.Role = Role;
			if (Usage.EstimatedCostUsd <= 0.0f)
			{
				Usage.EstimatedCostUsd = PricingConfig->CalculateCost(
					Usage.ModelId, Usage.InputTokens, Usage.OutputTokens, Usage.CachedInputTokens);
			}
			TokenTracker->RecordUsage(Usage);
		}

		if (OnComplete.IsBound())
		{
			OnComplete.Execute(Result);
		}
	});

	// Bridge FOnClaudeStreamEvent → FOnLLMStreamProgress (same signature, different delegate types)
	FOnLLMStreamProgress StreamProgress;
	if (Options.OnStreamEvent.IsBound())
	{
		FOnClaudeStreamEvent StreamDelegate = Options.OnStreamEvent;
		StreamProgress.BindLambda([StreamDelegate](const FClaudeStreamEvent& Event)
		{
			StreamDelegate.ExecuteIfBound(Event);
		});
	}

	// Sync any SessionManager entries not yet in ActiveSession (cross-role exchanges from CLI roles)
	if (SessionManager.IsValid())
	{
		const TArray<TPair<FString, FString>>& History = SessionManager->GetHistory();
		if (History.Num() > ActiveSessionSyncedHistoryCount)
		{
			TArray<TPair<FString, FString>> NewEntries(
				History.GetData() + ActiveSessionSyncedHistoryCount,
				History.Num() - ActiveSessionSyncedHistoryCount);
			Backend->SeedHistory(ActiveSession, NewEntries);
			ActiveSessionSyncedHistoryCount = History.Num();
		}
	}

	Backend->SubmitTurn(
		ActiveSession,
		Prompt,
		Options.AttachedImagePaths,
		ModelId,
		WrappedComplete,
		StreamProgress
	);
}
