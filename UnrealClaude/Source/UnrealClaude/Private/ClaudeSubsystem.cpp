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
#include "Interfaces/IPluginManager.h"

// Cached system prompt - static to avoid recreation on each call
static const FString CachedUE57SystemPrompt = TEXT(R"(You are an expert Unreal Engine 5.7 assistant working inside the UE Editor on a Blueprint-only multiplayer FPS project.

TOOL USAGE:

Read-only tools:
- blueprint_query — inspect Blueprint variables, functions, components
- blueprint_read_graph — read node graphs; lists ALL graphs including AnimBP state machines/transitions/conduits; extracts PropertyAccess bindings on AnimGraph nodes
- asset_search / asset_dependencies / asset_referencers — find and trace assets
- get_level_actors — list actors in the current level
- get_output_log — read editor output log
- capture_viewport — screenshot the viewport
- open_level — switch levels

Modifying tools:
- widget_editor — create/inspect/modify UMG Widget Blueprints (add_widget, remove_widget, set_property, set_slot_property, batch)
- blend_space — create/inspect/modify BlendSpace and AimOffset assets (add_sample, remove_sample, move_sample, set_axis, batch)
- montage_modify — create/modify AnimMontages (sections, segments, slots, notifies, blend settings, curves)
- anim_edit — batch-adjust bone tracks, resample, replace skeleton, extract frame range, inspect/transform skeletal mesh, set additive type

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
	Config.AllowedTools = {
		TEXT("Read"), TEXT("Grep"), TEXT("Glob"),
		// Allow edits only to docs and architecture files — all other writes remain blocked
		TEXT("Edit(Docs/**)"), TEXT("Edit(ARCHITECTURE.md)"), TEXT("Edit(CLAUDE.md)"),
		TEXT("Write(Docs/**)"), TEXT("Write(ARCHITECTURE.md)"),
	};
	Config.Model = SelectedModel;

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
	FOnClaudeResponse WrappedComplete;
	WrappedComplete.BindLambda([this, Prompt, OnComplete](const FString& Response, bool bSuccess)
	{
		if (bSuccess && SessionManager.IsValid())
		{
			SessionManager->AddExchange(Prompt, Response);
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
		ActiveBackendId = ProviderId;

		// Destroy old session if backend changed
		ILLMBackend* Backend = GetActiveBackend();
		if (Backend && ActiveSession.IsValid())
		{
			Backend->DestroySession(ActiveSession);
			ActiveSession = FLLMSessionHandle::Invalid();
		}

		UE_LOG(LogUnrealClaude, Log, TEXT("Active backend set to: %s"), *ProviderId);
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
	// If using Claude Code CLI backend, delegate to existing SendPrompt path
	if (ActiveBackendId == TEXT("claude-code"))
	{
		// Wrap the LLM callback into the legacy callback format
		FOnClaudeResponse LegacyComplete;
		LegacyComplete.BindLambda([this, OnComplete, Role](const FString& Response, bool bSuccess)
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
			Result.TokenUsage.ModelId = SelectedModel;
			Result.TokenUsage.Role = Role;

			if (OnComplete.IsBound())
			{
				OnComplete.Execute(Result);
			}
		});

		SendPrompt(Prompt, LegacyComplete, Options);
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
		if (!CustomSystemPrompt.IsEmpty())
		{
			SystemPrompt += TEXT("\n\n") + CustomSystemPrompt;
		}

		ActiveSession = Backend->CreateTaskSession(SystemPrompt);
	}

	// Resolve model: use role assignment if it targets this backend, else use selected model
	FModelRoleAssignment Assignment = RoleManager->GetAssignment(Role);
	FString ModelId = SelectedModel;
	FString AssignmentBackend = BackendRegistry->ResolveBackendForModel(Assignment.ModelId);
	if (AssignmentBackend == ActiveBackendId)
	{
		ModelId = Assignment.ModelId;
	}

	// Submit turn
	FOnLLMTurnComplete WrappedComplete;
	WrappedComplete.BindLambda([this, Prompt, OnComplete, Role, ModelId](const FLLMTurnResult& Result)
	{
		// Record in session history
		if (Result.bSuccess && SessionManager.IsValid())
		{
			SessionManager->AddExchange(Prompt, Result.ResponseText);
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

	Backend->SubmitTurn(
		ActiveSession,
		Prompt,
		Options.AttachedImagePaths,
		ModelId,
		WrappedComplete
	);
}
