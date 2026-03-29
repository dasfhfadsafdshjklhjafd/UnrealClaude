// Copyright Natali Caggiano. All Rights Reserved.

#include "SClaudeToolbar.h"
#include "ClaudeSubsystem.h"
#include "LLM/LLMBackendRegistry.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Styling/AppStyle.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "UnrealClaude"

FString SClaudeToolbar::GetModelDisplayName(const FString& ModelId)
{
	// Claude models
	if (ModelId == TEXT("claude-opus-4-6"))              return TEXT("Opus 4.6");
	if (ModelId == TEXT("claude-sonnet-4-6"))            return TEXT("Sonnet 4.6");
	if (ModelId == TEXT("claude-haiku-4-5-20251001"))    return TEXT("Haiku 4.5");
	// OpenAI models
	if (ModelId == TEXT("gpt-4o"))          return TEXT("GPT-4o");
	if (ModelId == TEXT("gpt-4o-mini"))     return TEXT("GPT-4o Mini");
	if (ModelId == TEXT("gpt-5.2-codex"))   return TEXT("GPT-5.2 Codex");
	if (ModelId == TEXT("gpt-5.4"))         return TEXT("GPT-5.4");
	if (ModelId == TEXT("gpt-5.4-mini"))    return TEXT("GPT-5.4 Mini");
	if (ModelId == TEXT("o3"))              return TEXT("o3");
	if (ModelId == TEXT("o4-mini"))         return TEXT("o4-mini");
	return ModelId;
}

void SClaudeToolbar::RefreshModelOptions()
{
	ModelOptions.Empty();
	TArray<TPair<FString, FString>> AllModels =
		FClaudeCodeSubsystem::Get().GetBackendRegistry().GetAllModelsWithProviders();
	for (const auto& Pair : AllModels)
	{
		ModelOptions.Add(MakeShared<FString>(Pair.Key));
	}
	if (ModelComboBox.IsValid())
	{
		ModelComboBox->RefreshOptions();
	}
}

void SClaudeToolbar::Construct(const FArguments& InArgs)
{
	bUE57ContextEnabled = InArgs._bUE57ContextEnabled;
	bProjectContextEnabled = InArgs._bProjectContextEnabled;
	bRestoreEnabled = InArgs._bRestoreEnabled;
	SelectedModel = InArgs._SelectedModel;
	bAnthropicUseCLI = InArgs._bAnthropicUseCLI;
	TokenCountText = InArgs._TokenCountText;
	TokenCountColor = InArgs._TokenCountColor;
	CostText = InArgs._CostText;
	OnUE57ContextChanged = InArgs._OnUE57ContextChanged;
	OnProjectContextChanged = InArgs._OnProjectContextChanged;
	OnRefreshContext = InArgs._OnRefreshContext;
	OnRestoreSession = InArgs._OnRestoreSession;
	OnNewSession = InArgs._OnNewSession;
	OnClear = InArgs._OnClear;
	OnCopyLast = InArgs._OnCopyLast;
	OnCopyChat = InArgs._OnCopyChat;
	OnCompact = InArgs._OnCompact;
	OnRoleConfig = InArgs._OnRoleConfig;
	OnModelChanged = InArgs._OnModelChanged;
	OnAnthropicModeChanged = InArgs._OnAnthropicModeChanged;
	OnSendRoleChanged = InArgs._OnSendRoleChanged;
	OnRestoreArchive = InArgs._OnRestoreArchive;
	ActiveSendRole = InArgs._ActiveSendRole;

	// Initialize model options from all backends
	RefreshModelOptions();

	// Initialize role options
	for (EModelRole Role : GetAllModelRoles())
	{
		RoleOptions.Add(MakeShared<EModelRole>(Role));
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(8.0f, 2.0f))
		[
			SNew(SVerticalBox)

			// ── Row 1: title | model | CLI/API toggle | [spacer] | context toggles | refresh | restore ──
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Title", "Claude Assistant"))
					.TextStyle(FAppStyle::Get(), "LargeText")
				]

				// Active worker model label (configured via Roles)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(SClaudeToolbar::GetModelDisplayName(SelectedModel.Get())); })
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.7f, 0.9f)))
					.ToolTipText(LOCTEXT("WorkerModelTip", "Active worker model (change via Roles button)"))
				]

				// Anthropic: CLI / API toggle
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bAnthropicUseCLI.Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { OnAnthropicModeChanged.ExecuteIfBound(NewState == ECheckBoxState::Checked); })
					.ToolTipText(LOCTEXT("CLIModeTip", "Checked = Claude CLI (subscription)\nUnchecked = Anthropic API (pay-per-token)\nOnly affects Claude models"))
					[
						SNew(STextBlock).Text(LOCTEXT("CLIMode", "CLI"))
					]
				]

				// Roles config button
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Roles", "Roles"))
					.OnClicked_Lambda([this]() { OnRoleConfig.ExecuteIfBound(); return FReply::Handled(); })
					.ToolTipText(LOCTEXT("RolesTip", "Configure model assignments per role (Worker, Critic, Architect, etc.)"))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[ SNullWidget::NullWidget ]

				// UE5.7 Context checkbox
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bUE57ContextEnabled.Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { OnUE57ContextChanged.ExecuteIfBound(NewState == ECheckBoxState::Checked); })
					.ToolTipText(LOCTEXT("UE57ContextTip", "Include Unreal Engine 5.7 context in prompts"))
					[
						SNew(STextBlock).Text(LOCTEXT("UE57Context", "UE5.7"))
					]
				]

				// Project Context checkbox
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bProjectContextEnabled.Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { OnProjectContextChanged.ExecuteIfBound(NewState == ECheckBoxState::Checked); })
					.ToolTipText(LOCTEXT("ProjectContextTip", "Include project source files and level actors in prompts"))
					[
						SNew(STextBlock).Text(LOCTEXT("ProjectContext", "Project"))
					]
				]

				// Refresh button
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Refresh", "Refresh"))
					.OnClicked_Lambda([this]() { OnRefreshContext.ExecuteIfBound(); return FReply::Handled(); })
					.ToolTipText(LOCTEXT("RefreshContextTip", "Refresh project context (source files, classes, level actors)"))
				]

				// Restore button
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Restore", "Restore"))
					.OnClicked_Lambda([this]() { OnRestoreSession.ExecuteIfBound(); return FReply::Handled(); })
					.ToolTipText(LOCTEXT("RestoreContextTip", "Restore current session.json into chat view"))
					.IsEnabled_Lambda([this]() { return bRestoreEnabled.Get(); })
				]

				// Archives combobutton — pick a specific previous session
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(2.0f, 0.0f)
				[
					SNew(SComboButton)
					.ButtonContent()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Archives", "Archives"))
						.TextStyle(FAppStyle::Get(), "SmallText")
					]
					.OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
					{
						FString SessionDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealClaude"));
						TArray<FString> ArchiveFiles;
						IFileManager::Get().FindFiles(ArchiveFiles,
							*FPaths::Combine(SessionDir, TEXT("session_*.json")), true, false);
						ArchiveFiles.Sort([](const FString& A, const FString& B) { return A > B; }); // newest first

						TSharedRef<SScrollBox> List = SNew(SScrollBox);

						if (ArchiveFiles.IsEmpty())
						{
							List->AddSlot()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("NoArchives", "No archived sessions found"))
								.Margin(FMargin(8.0f, 4.0f))
							];
						}

						for (const FString& File : ArchiveFiles)
						{
							FString FullPath = FPaths::Combine(SessionDir, File);
							FString Label = File;
							Label.RemoveFromStart(TEXT("session_"));
							Label.RemoveFromEnd(TEXT(".json"));
							// "2026-03-29T14-05" → "2026-03-29  14:05"
							Label.ReplaceInline(TEXT("T"), TEXT("  "));
							Label.ReplaceInline(TEXT("-"), TEXT(":"), ESearchCase::CaseSensitive);
							// Restore date dashes (first 10 chars are date)
							if (Label.Len() >= 10)
							{
								Label[4] = TEXT('-');
								Label[7] = TEXT('-');
							}

							List->AddSlot()
							[
								SNew(SButton)
								.Text(FText::FromString(Label))
								.TextStyle(FAppStyle::Get(), "SmallText")
								.OnClicked_Lambda([this, FullPath]()
								{
									OnRestoreArchive.ExecuteIfBound(FullPath);
									return FReply::Handled();
								})
							];
						}
						return SNew(SBox).MaxDesiredHeight(220.0f) [ List ];
					})
					.ToolTipText(LOCTEXT("ArchivesTip", "Pick and restore a specific archived session"))
				]
			]

			// ── Row 2: session actions | copy | compact | [spacer] | cost | token count ──
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("NewSession", "New Session"))
					.OnClicked_Lambda([this]() { OnNewSession.ExecuteIfBound(); return FReply::Handled(); })
					.ToolTipText(LOCTEXT("NewSessionTip", "Start a new session (clears history)"))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Clear", "Clear"))
					.OnClicked_Lambda([this]() { OnClear.ExecuteIfBound(); return FReply::Handled(); })
					.ToolTipText(LOCTEXT("ClearTip", "Clear chat display"))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyLast", "Copy Last"))
					.OnClicked_Lambda([this]() { OnCopyLast.ExecuteIfBound(); return FReply::Handled(); })
					.ToolTipText(LOCTEXT("CopyTip", "Copy last response to clipboard"))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyChat", "Copy Chat"))
					.OnClicked_Lambda([this]() { OnCopyChat.ExecuteIfBound(); return FReply::Handled(); })
					.ToolTipText(LOCTEXT("CopyChatTip", "Copy entire chat history to clipboard"))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Compact", "Compact"))
					.OnClicked_Lambda([this]() { OnCompact.ExecuteIfBound(); return FReply::Handled(); })
					.ToolTipText(LOCTEXT("CompactTip", "Summarize conversation to free up context window"))
				]

				// Role toggle buttons — lit green when active, click again to return to Worker
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AskCritic", "Critic"))
					.OnClicked_Lambda([this]()
					{
						EModelRole NewRole = (ActiveSendRole.Get() == EModelRole::Critic) ? EModelRole::Worker : EModelRole::Critic;
						OnSendRoleChanged.ExecuteIfBound(NewRole);
						return FReply::Handled();
					})
					.ButtonColorAndOpacity_Lambda([this]()
					{
						return ActiveSendRole.Get() == EModelRole::Critic
							? FLinearColor(0.85f, 0.42f, 0.08f)
							: FLinearColor(0.15f, 0.15f, 0.17f);
					})
					.ToolTipText(LOCTEXT("CriticTip", "Toggle Critic mode — stays active for all sends until toggled off"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(2.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AskArchitect", "Architect"))
					.OnClicked_Lambda([this]()
					{
						EModelRole NewRole = (ActiveSendRole.Get() == EModelRole::Architect) ? EModelRole::Worker : EModelRole::Architect;
						OnSendRoleChanged.ExecuteIfBound(NewRole);
						return FReply::Handled();
					})
					.ButtonColorAndOpacity_Lambda([this]()
					{
						return ActiveSendRole.Get() == EModelRole::Architect
							? FLinearColor(0.85f, 0.42f, 0.08f)
							: FLinearColor(0.15f, 0.15f, 0.17f);
					})
					.ToolTipText(LOCTEXT("ArchitectTip", "Toggle Architect mode — stays active for all sends until toggled off"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(2.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Escalate", "Escalate"))
					.OnClicked_Lambda([this]()
					{
						EModelRole NewRole = (ActiveSendRole.Get() == EModelRole::Escalation) ? EModelRole::Worker : EModelRole::Escalation;
						OnSendRoleChanged.ExecuteIfBound(NewRole);
						return FReply::Handled();
					})
					.ButtonColorAndOpacity_Lambda([this]()
					{
						return ActiveSendRole.Get() == EModelRole::Escalation
							? FLinearColor(0.85f, 0.42f, 0.08f)
							: FLinearColor(0.15f, 0.15f, 0.17f);
					})
					.ToolTipText(LOCTEXT("EscalateTip", "Toggle Escalation mode — uses a stronger model for all sends until toggled off"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(2.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("WriteDocs", "Docs"))
					.OnClicked_Lambda([this]()
					{
						EModelRole NewRole = (ActiveSendRole.Get() == EModelRole::DocsAgent) ? EModelRole::Worker : EModelRole::DocsAgent;
						OnSendRoleChanged.ExecuteIfBound(NewRole);
						return FReply::Handled();
					})
					.ButtonColorAndOpacity_Lambda([this]()
					{
						return ActiveSendRole.Get() == EModelRole::DocsAgent
							? FLinearColor(0.85f, 0.42f, 0.08f)
							: FLinearColor(0.15f, 0.15f, 0.17f);
					})
					.ToolTipText(LOCTEXT("DocsTip", "Toggle DocsAgent mode — routes all sends to DocsAgent until toggled off"))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[ SNullWidget::NullWidget ]

				// Cost display
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return CostText.Get(); })
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.7f, 0.4f)))
					.ToolTipText(LOCTEXT("CostTip", "Estimated API cost for today"))
				]

				// Token counter
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return TokenCountText.Get(); })
					.ColorAndOpacity_Lambda([this]() { return TokenCountColor.Get(); })
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ToolTipText(LOCTEXT("TokenCountTip", "Session context usage\n80k = orange warning\n130k = red (approaching limit)"))
				]
			]
		]
	];
}

#undef LOCTEXT_NAMESPACE
