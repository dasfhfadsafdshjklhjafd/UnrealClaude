// Copyright Natali Caggiano. All Rights Reserved.

#include "SClaudeToolbar.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "UnrealClaude"

FString SClaudeToolbar::GetModelDisplayName(const FString& ModelId)
{
	if (ModelId == TEXT("claude-opus-4-6"))       return TEXT("Opus 4.6");
	if (ModelId == TEXT("claude-sonnet-4-6"))     return TEXT("Sonnet 4.6");
	if (ModelId == TEXT("claude-haiku-4-5-20251001")) return TEXT("Haiku 4.5");
	return ModelId;
}

void SClaudeToolbar::Construct(const FArguments& InArgs)
{
	bUE57ContextEnabled = InArgs._bUE57ContextEnabled;
	bProjectContextEnabled = InArgs._bProjectContextEnabled;
	bRestoreEnabled = InArgs._bRestoreEnabled;
	SelectedModel = InArgs._SelectedModel;
	TokenCountText = InArgs._TokenCountText;
	TokenCountColor = InArgs._TokenCountColor;
	OnUE57ContextChanged = InArgs._OnUE57ContextChanged;
	OnProjectContextChanged = InArgs._OnProjectContextChanged;
	OnRefreshContext = InArgs._OnRefreshContext;
	OnRestoreSession = InArgs._OnRestoreSession;
	OnNewSession = InArgs._OnNewSession;
	OnClear = InArgs._OnClear;
	OnCopyLast = InArgs._OnCopyLast;
	OnCopyChat = InArgs._OnCopyChat;
	OnCompact = InArgs._OnCompact;
	OnModelChanged = InArgs._OnModelChanged;

	ModelOptions = {
		MakeShared<FString>(TEXT("claude-sonnet-4-6")),
		MakeShared<FString>(TEXT("claude-opus-4-6")),
		MakeShared<FString>(TEXT("claude-haiku-4-5-20251001")),
	};

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(8.0f, 2.0f))
		[
			SNew(SVerticalBox)

			// ── Row 1: title | model | [spacer] | context toggles | refresh | restore ──
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

				// Model selector
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f)
				[
					SNew(SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&ModelOptions)
					.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
					{
						if (Item.IsValid()) OnModelChanged.ExecuteIfBound(*Item);
					})
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
					{
						return SNew(STextBlock).Text(FText::FromString(SClaudeToolbar::GetModelDisplayName(Item.IsValid() ? *Item : FString())));
					})
					.ToolTipText(LOCTEXT("ModelSelectorTip", "Select Claude model"))
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return FText::FromString(SClaudeToolbar::GetModelDisplayName(SelectedModel.Get())); })
					]
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
					.ToolTipText(LOCTEXT("RestoreContextTip", "Restore previous session context from disk"))
					.IsEnabled_Lambda([this]() { return bRestoreEnabled.Get(); })
				]
			]

			// ── Row 2: session actions | copy | compact | [spacer] | token count ──
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(SHorizontalBox)

				// New Session
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

				// Clear
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

				// Copy Last
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

				// Copy Chat
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

				// Compact
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

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[ SNullWidget::NullWidget ]

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
