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
- think — plan your investigation upfront; call FIRST for any multi-step task, then execute all steps in one response
- blueprint_search — keyword search over the Blueprint index; returns matching blueprints and graphs by variable/function/node name. Use for open-ended discovery.
- blueprint_find_variable — find every read/write of a named variable across all graphs. Cheapest way to trace data flow.
- blueprint_query — inspect Blueprint variables, functions, components
- blueprint_read_graph — read node graphs; use summary_only=true first for large graphs to locate relevant nodes cheaply; lists ALL graphs including AnimBP state machines/transitions/conduits
- asset_search / asset_dependencies / asset_referencers — find and trace assets
- get_level_actors — list actors in the current level
- get_output_log — read editor output log (supports filtering, cursor-based incremental reads)
- capture_viewport — screenshot the viewport
- open_level — list available map templates (open/new/save disabled)
- widget_editor — inspect UMG Widget Blueprint trees and properties (inspect_tree, get_properties only)
- blend_space — inspect BlendSpace and AimOffset assets (inspect, list only)
- montage_modify — inspect AnimMontage structure (get_info, get_curves only)
- anim_edit — inspect animation tracks and skeletal meshes (inspect_track, inspect_mesh only)

TOOL ROUTING — choose the cheapest path:
- Open-ended question ("where is X handled?", "what calls Y?", "how does Z work?") → blueprint_search first
- Known exact location ("show me BP_PlayerBase EventGraph") → blueprint_read_graph directly
- Tracing a variable ("where is HealthState written?") → blueprint_find_variable
- Large graph, unknown which nodes matter → blueprint_read_graph with summary_only=true, then read targeted range

HOW TO READ A GRAPH:
1. blueprint_search(query="...") if you don't know which BP/graph yet
2. blueprint_query(operation="inspect", blueprint_path="...") for variable/function overview
3. blueprint_read_graph(blueprint_path="...") to list all graphs
4. blueprint_read_graph(blueprint_path="...", graph_name="...", summary_only=true) for large graphs
5. blueprint_read_graph(blueprint_path="...", graph_name="...", start_node=N, max_nodes=30) for targeted reads

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
	EModelRole HistoryRole = Options.Role;
	FOnClaudeResponse WrappedComplete;
	WrappedComplete.BindLambda([this, HistoryKey, HistoryRole, OnComplete](const FString& Response, bool bSuccess)
	{
		if (bSuccess && SessionManager.IsValid())
		{
			// Prefix non-Worker responses with role label so other roles can tell who said what
			FString StoredResponse = Response;
			if (HistoryRole != EModelRole::Worker)
			{
				StoredResponse = FString::Printf(TEXT("[%s]: %s"), *GetModelRoleDisplayName(HistoryRole), *Response);
			}
			SessionManager->AddExchange(HistoryKey, StoredResponse);
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
- For any multi-step investigation, call think() FIRST with your goal and every tool call you plan to make. Then execute ALL steps in one response without checking back in.
- ALWAYS use tools to verify before answering. Do not guess or assume.
- Chain multiple tool calls in one response — do not wait for user acknowledgement between steps.
- Never say "I would need to check..." — just check. You have the tools.
- Never ask "should I inspect X?" — just do it as part of your plan.

TOOL ROUTING — pick the cheapest path first:
- Open-ended question ("where is equip handled?", "what controls jump height?") → blueprint_search(query="...") first, costs ~200 tokens
- Already know the exact asset and graph → blueprint_read_graph directly, skip search
- Tracing data flow ("where is CurrentAmmo written?") → blueprint_find_variable, costs ~50 tokens
- Large graph, unknown which nodes matter → blueprint_read_graph with summary_only=true, then read only the relevant range
- If you know the answer from ARCHITECTURE.md or context already → don't call any tools

The worst case with blueprint_search is paying 200 tokens before doing what you'd do anyway.
The worst case without it is blindly reading 20k tokens from the wrong Blueprint.
Always search first for open-ended questions.

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
	}
	// Destroy all role sessions — history has been wiped, stale context must not persist
	TArray<EModelRole> Roles;
	RoleSessions.GetKeys(Roles);
	for (EModelRole Role : Roles)
	{
		DestroyRoleSession(Role);
	}
}

void FClaudeCodeSubsystem::DestroyRoleSession(EModelRole Role)
{
	FLLMSessionHandle* Session = RoleSessions.Find(Role);
	if (Session && Session->IsValid())
	{
		FString* BackendId = RoleSessionBackendIds.Find(Role);
		if (BackendId)
		{
			ILLMBackend* Backend = BackendRegistry->GetBackend(*BackendId);
			if (Backend) Backend->DestroySession(*Session);
		}
	}
	RoleSessions.Remove(Role);
	RoleSessionSyncedHistoryCounts.Remove(Role);
	RoleSessionBackendIds.Remove(Role);
}

FString FClaudeCodeSubsystem::BuildAPISystemPrompt(const FClaudePromptOptions& Options) const
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
	SystemPrompt += GetProjectInstructionsPrompt();
	SystemPrompt += GetArchitectureContextPrompt();
	SystemPrompt += GetKanbanContextPrompt();
	SystemPrompt += GetAPIBehaviorPrompt();
	if (!CustomSystemPrompt.IsEmpty())
	{
		SystemPrompt += TEXT("\n\n") + CustomSystemPrompt;
	}
	return SystemPrompt;
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
	// Cancel all API backends — with per-role sessions any backend could be in-flight
	for (ILLMBackend* Backend : BackendRegistry->GetAllBackends())
	{
		if (Backend) Backend->Cancel();
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
	if (!BackendRegistry->HasBackend(ProviderId))
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("Unknown backend provider: %s"), *ProviderId);
		return;
	}

	if (ActiveBackendId != ProviderId)
	{
		// Invalidate the Worker session — its backend is changing.
		// Other role sessions are unaffected; they each track their own backend.
		DestroyRoleSession(EModelRole::Worker);
		ActiveBackendId = ProviderId;
		UE_LOG(LogUnrealClaude, Log, TEXT("Active backend set to: %s"), *ProviderId);
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
	// Resolve model and backend for this role.
	// Worker uses ActiveBackendId (controlled by the model selector).
	// Non-Worker roles use their own assignment's ProviderId so they never
	// touch the Worker's session or backend selection.
	FModelRoleAssignment Assignment = RoleManager->GetAssignment(Role);
	FString ModelId = Assignment.ModelId.IsEmpty() ? SelectedModel : Assignment.ModelId;
	FString BackendId = (Role == EModelRole::Worker || Assignment.ProviderId.IsEmpty())
		? ActiveBackendId
		: Assignment.ProviderId;

	// ---- CLI path: stateless, no session management needed ----
	if (BackendId == TEXT("claude-code"))
	{
		FOnClaudeResponse LegacyComplete;
		LegacyComplete.BindLambda([this, OnComplete, Role, ModelId](const FString& Response, bool bSuccess)
		{
			FLLMTurnResult Result;
			Result.ResponseText = Response;
			Result.bSuccess = bSuccess;
			if (!bSuccess) Result.ErrorMessage = Response;
			Result.TokenUsage.ProviderId = TEXT("claude-code");
			Result.TokenUsage.ModelId = ModelId;
			Result.TokenUsage.Role = Role;
			if (OnComplete.IsBound()) OnComplete.Execute(Result);
		});

		FClaudePromptOptions CLIOptions = Options;
		CLIOptions.Role = Role;
		CLIOptions.ModelOverride = ModelId;
		SendPrompt(Prompt, LegacyComplete, CLIOptions);
		return;
	}

	// ---- API path: use per-role session ----
	ILLMBackend* Backend = BackendRegistry->GetBackend(BackendId);
	if (!Backend)
	{
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(FLLMTurnResult::Error(
				FString::Printf(TEXT("Backend '%s' not found"), *BackendId)));
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

	// Get or create this role's dedicated session.
	// If the role's assigned backend has changed since the session was created, invalidate it.
	FLLMSessionHandle& Session    = RoleSessions.FindOrAdd(Role);
	int32&             SyncCount  = RoleSessionSyncedHistoryCounts.FindOrAdd(Role, 0);
	FString&           SessionBId = RoleSessionBackendIds.FindOrAdd(Role);

	if (Session.IsValid() && SessionBId != BackendId)
	{
		// Backend changed for this role — destroy stale session
		ILLMBackend* OldBackend = BackendRegistry->GetBackend(SessionBId);
		if (OldBackend) OldBackend->DestroySession(Session);
		Session    = FLLMSessionHandle::Invalid();
		SyncCount  = 0;
		SessionBId = FString();
	}

	if (!Session.IsValid())
	{
		FString SystemPrompt = BuildAPISystemPrompt(Options);
		UE_LOG(LogUnrealClaude, Log, TEXT("Creating API session for role %d on '%s' (%d chars)"),
			(int32)Role, *BackendId, SystemPrompt.Len());
		Session    = Backend->CreateTaskSession(SystemPrompt);
		SessionBId = BackendId;
		SyncCount  = 0;
	}

	// Sync any new SessionManager exchanges not yet in this role's session
	// (picks up exchanges from CLI roles or other roles that happened since last sync)
	if (SessionManager.IsValid())
	{
		const TArray<TPair<FString, FString>>& History = SessionManager->GetHistory();
		if (History.Num() > SyncCount)
		{
			TArray<TPair<FString, FString>> NewEntries(
				History.GetData() + SyncCount,
				History.Num() - SyncCount);
			Backend->SeedHistory(Session, NewEntries);
			SyncCount = History.Num();
		}
	}

	// Use HistoryPrompt if set (clean user message without role prefix)
	FString HistoryKey = Options.HistoryPrompt.IsEmpty() ? Prompt : Options.HistoryPrompt;

	FOnLLMTurnComplete WrappedComplete;
	WrappedComplete.BindLambda([this, HistoryKey, OnComplete, Role, ModelId](const FLLMTurnResult& Result)
	{
		if (Result.bSuccess && SessionManager.IsValid())
		{
			FString StoredResponse = Result.ResponseText;
			if (Role != EModelRole::Worker)
			{
				StoredResponse = FString::Printf(TEXT("[%s]: %s"), *GetModelRoleDisplayName(Role), *Result.ResponseText);
			}
			SessionManager->AddExchange(HistoryKey, StoredResponse);
			SessionManager->SaveSession();
		}

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

		if (OnComplete.IsBound()) OnComplete.Execute(Result);
	});

	FOnLLMStreamProgress StreamProgress;
	if (Options.OnStreamEvent.IsBound())
	{
		FOnClaudeStreamEvent StreamDelegate = Options.OnStreamEvent;
		StreamProgress.BindLambda([StreamDelegate](const FClaudeStreamEvent& Event)
		{
			StreamDelegate.ExecuteIfBound(Event);
		});
	}

	Backend->SubmitTurn(Session, Prompt, Options.AttachedImagePaths, ModelId, WrappedComplete, StreamProgress);
}
