// Copyright Natali Caggiano. All Rights Reserved.

#include "UnrealClaudeModule.h"
#include "UnrealClaudeCommands.h"
#include "ClaudeEditorWidget.h"
#include "ClaudeCodeRunner.h"
#include "ClaudeSubsystem.h"
#include "ScriptExecutionManager.h"
#include "MCP/UnrealClaudeMCPServer.h"
#include "ProjectContext.h"
#include "RAG/FBlueprintSearchIndex.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Application/SlateApplication.h"
#include "HttpServerModule.h"

DEFINE_LOG_CATEGORY(LogUnrealClaude);

#define LOCTEXT_NAMESPACE "FUnrealClaudeModule"

static const FName ClaudeTabName("ClaudeAssistant");

void FUnrealClaudeModule::StartupModule()
{
	UE_LOG(LogUnrealClaude, Warning, TEXT("=== UnrealClaude BUILD 20260107-1450 THREAD_TESTS_DISABLED ==="));
	
	// Register commands
	FUnrealClaudeCommands::Register();
	
	PluginCommands = MakeShared<FUICommandList>();
	
	// Map commands to actions
	PluginCommands->MapAction(
		FUnrealClaudeCommands::Get().OpenClaudePanel,
		FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(ClaudeTabName);
		}),
		FCanExecuteAction()
	);

	// Map QuickAsk command - shows a popup for quick questions
	PluginCommands->MapAction(
		FUnrealClaudeCommands::Get().QuickAsk,
		FExecuteAction::CreateLambda([]()
		{
			// Create a simple input dialog
			TSharedRef<SWindow> QuickAskWindow = SNew(SWindow)
				.Title(LOCTEXT("QuickAskTitle", "Quick Ask Claude"))
				.ClientSize(FVector2D(500, 100))
				.SupportsMinimize(false)
				.SupportsMaximize(false);

			TSharedPtr<SEditableTextBox> InputBox;

			QuickAskWindow->SetContent(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.Padding(10)
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("QuickAskLabel", "Ask Claude a quick question:"))
				]
				+ SVerticalBox::Slot()
				.Padding(10, 0, 10, 10)
				.FillHeight(1.0f)
				[
					SAssignNew(InputBox, SEditableTextBox)
					.HintText(LOCTEXT("QuickAskHint", "Type your question here..."))
					.OnTextCommitted_Lambda([QuickAskWindow](const FText& Text, ETextCommit::Type CommitType)
					{
						if (CommitType == ETextCommit::OnEnter && !Text.IsEmpty())
						{
							// Close the window
							QuickAskWindow->RequestDestroyWindow();

							// Send prompt to Claude
							FString Prompt = Text.ToString();
							FClaudePromptOptions Options;
							Options.bIncludeEngineContext = true;
							Options.bIncludeProjectContext = true;
							FClaudeCodeSubsystem::Get().SendPrompt(
								Prompt,
								FOnClaudeResponse::CreateLambda([](const FString& Response, bool bSuccess)
								{
									// Show response in notification
									FNotificationInfo Info(FText::FromString(
										bSuccess
											? (Response.Len() > 300 ? Response.Left(300) + TEXT("...") : Response)
											: TEXT("Error: ") + Response));
									Info.ExpireDuration = bSuccess ? 15.0f : 5.0f;
									Info.bUseLargeFont = false;
									Info.bUseSuccessFailIcons = true;
									FSlateNotificationManager::Get().AddNotification(Info);
								}),
								Options
							);
						}
					})
				]
			);

			FSlateApplication::Get().AddWindow(QuickAskWindow);

			// Focus the input box
			if (InputBox.IsValid())
			{
				FSlateApplication::Get().SetKeyboardFocus(InputBox);
			}
		}),
		FCanExecuteAction::CreateLambda([]()
		{
			return FClaudeCodeRunner::IsClaudeAvailable();
		})
	);

	// Register the tab spawner
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		ClaudeTabName,
		FOnSpawnTab::CreateLambda([](const FSpawnTabArgs& Args) -> TSharedRef<SDockTab>
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				.Label(LOCTEXT("ClaudeTabTitle", "Claude Assistant"))
				[
					SNew(SClaudeEditorWidget)
				];
		}))
		.SetDisplayName(LOCTEXT("ClaudeTabTitle", "Claude Assistant"))
		.SetTooltipText(LOCTEXT("ClaudeTabTooltip", "Open the Claude AI Assistant for UE5.7 development help"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Help"));
	
	// Register menus after engine init
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUnrealClaudeModule::RegisterMenus));

	// Bind keyboard shortcuts to the Level Editor
	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	LevelEditorModule.GetGlobalLevelEditorActions()->Append(PluginCommands.ToSharedRef());

	// Check Claude availability
	if (FClaudeCodeRunner::IsClaudeAvailable())
	{
		UE_LOG(LogUnrealClaude, Log, TEXT("Claude CLI found at: %s"), *FClaudeCodeRunner::GetClaudePath());
	}
	else
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("Claude CLI not found. Please install with: npm install -g @anthropic-ai/claude-code"));
	}

	// Start MCP Server
	StartMCPServer();

	// Initialize project context (async, will gather in background)
	FProjectContextManager::Get().RefreshContext();

	// Initialize script execution manager (creates script directories)
	FScriptExecutionManager::Get();

	// Initialize Blueprint search index (loads from disk if available)
	InitializeBlueprintIndex();
}

void FUnrealClaudeModule::InitializeBlueprintIndex()
{
	FBlueprintSearchIndex& Index = FBlueprintSearchIndex::Get();
	Index.Initialize(); // loads from disk if file exists — fast path for subsequent launches

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Wire incremental re-index on asset save/remove
	OnAssetAddedHandle   = AssetRegistry.OnAssetAdded().AddRaw(this, &FUnrealClaudeModule::OnAssetAdded);
	OnAssetUpdatedHandle = AssetRegistry.OnAssetUpdated().AddRaw(this, &FUnrealClaudeModule::OnAssetUpdated);
	OnAssetRemovedHandle = AssetRegistry.OnAssetRemoved().AddRaw(this, &FUnrealClaudeModule::OnAssetRemoved);

	if (Index.IsBuilt())
	{
		// Loaded from disk — no rebuild needed
		UE_LOG(LogUnrealClaude, Log, TEXT("BlueprintSearchIndex: loaded %d chunks from disk"), Index.GetChunkCount());
	}
	else if (AssetRegistry.IsLoadingAssets())
	{
		// AssetRegistry is still scanning — defer rebuild until it finishes
		UE_LOG(LogUnrealClaude, Log, TEXT("BlueprintSearchIndex: AssetRegistry still loading, deferring RebuildAll..."));
		OnFilesLoadedHandle = AssetRegistry.OnFilesLoaded().AddRaw(this, &FUnrealClaudeModule::OnAssetRegistryFilesLoaded);
	}
	else
	{
		// AssetRegistry is already ready (rare at plugin startup, but handle it)
		UE_LOG(LogUnrealClaude, Log, TEXT("BlueprintSearchIndex: building index..."));
		Index.RebuildAll();
	}
}

void FUnrealClaudeModule::OnAssetRegistryFilesLoaded()
{
	UE_LOG(LogUnrealClaude, Log, TEXT("BlueprintSearchIndex: AssetRegistry ready — building index now"));
	FBlueprintSearchIndex::Get().RebuildAll();
	// One-shot delegate — unbind after firing
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get()
		.OnFilesLoaded().Remove(OnFilesLoadedHandle);
	OnFilesLoadedHandle.Reset();
}

void FUnrealClaudeModule::OnAssetAdded(const FAssetData& AssetData)
{
	if (!AssetData.AssetClassPath.ToString().Contains(TEXT("Blueprint"))) return;
	if (!FBlueprintSearchIndex::Get().IsBuilt()) return;
	FBlueprintSearchIndex::Get().IndexBlueprint(AssetData.GetObjectPathString());
}

void FUnrealClaudeModule::OnAssetUpdated(const FAssetData& AssetData)
{
	if (!AssetData.AssetClassPath.ToString().Contains(TEXT("Blueprint"))) return;
	if (!FBlueprintSearchIndex::Get().IsBuilt()) return;
	FBlueprintSearchIndex::Get().IndexBlueprint(AssetData.GetObjectPathString());
}

void FUnrealClaudeModule::OnAssetRemoved(const FAssetData& AssetData)
{
	if (!AssetData.AssetClassPath.ToString().Contains(TEXT("Blueprint"))) return;
	if (!FBlueprintSearchIndex::Get().IsBuilt()) return;
	FBlueprintSearchIndex::Get().RemoveBlueprint(AssetData.GetObjectPathString());
}

void FUnrealClaudeModule::ShutdownModule()
{
	UE_LOG(LogUnrealClaude, Log, TEXT("UnrealClaude module shutting down"));

	// Unhook asset registry delegates
	if (FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
	{
		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		AssetRegistry.OnAssetAdded().Remove(OnAssetAddedHandle);
		AssetRegistry.OnAssetUpdated().Remove(OnAssetUpdatedHandle);
		AssetRegistry.OnAssetRemoved().Remove(OnAssetRemovedHandle);
		AssetRegistry.OnFilesLoaded().Remove(OnFilesLoadedHandle);
	}

	// Stop MCP Server
	StopMCPServer();

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	FUnrealClaudeCommands::Unregister();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ClaudeTabName);
}

FUnrealClaudeModule& FUnrealClaudeModule::Get()
{
	return FModuleManager::LoadModuleChecked<FUnrealClaudeModule>("UnrealClaude");
}

bool FUnrealClaudeModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded("UnrealClaude");
}

void FUnrealClaudeModule::RegisterMenus()
{
	// Owner will be used for cleanup in call to UToolMenus::UnregisterOwner
	FToolMenuOwnerScoped OwnerScoped(this);
	
	// Add to the main menu bar under Tools
	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("UnrealClaude");

		Section.AddMenuEntryWithCommandList(
			FUnrealClaudeCommands::Get().OpenClaudePanel,
			PluginCommands,
			LOCTEXT("OpenClaudeMenuItem", "Claude Assistant"),
			LOCTEXT("OpenClaudeMenuItemTooltip", "Open the Claude AI Assistant for UE5.7 help (Ctrl+Shift+C)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Help")
		);

		Section.AddMenuEntryWithCommandList(
			FUnrealClaudeCommands::Get().QuickAsk,
			PluginCommands,
			LOCTEXT("QuickAskMenuItem", "Quick Ask Claude"),
			LOCTEXT("QuickAskMenuItemTooltip", "Quickly ask Claude a question (Ctrl+Alt+C)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Help")
		);
	}
	
	// Add to the toolbar
	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("UnrealClaude");
		
		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			FUnrealClaudeCommands::Get().OpenClaudePanel,
			LOCTEXT("ClaudeToolbarButton", "Claude"),
			LOCTEXT("ClaudeToolbarTooltip", "Open Claude Assistant (Ctrl+Shift+C)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Help")
		));
	}
}

void FUnrealClaudeModule::UnregisterMenus()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FUnrealClaudeModule::StartMCPServer()
{
	if (MCPServer.IsValid())
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("MCP Server already exists"));
		return;
	}

	MCPServer = MakeShared<FUnrealClaudeMCPServer>();

	if (!MCPServer->Start(GetMCPServerPort()))
	{
		UE_LOG(LogUnrealClaude, Error, TEXT("Failed to start MCP Server on port %d"), GetMCPServerPort());
		MCPServer.Reset();
	}
}

void FUnrealClaudeModule::StopMCPServer()
{
	if (MCPServer.IsValid())
	{
		MCPServer->Stop();
		MCPServer.Reset();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUnrealClaudeModule, UnrealClaude)
