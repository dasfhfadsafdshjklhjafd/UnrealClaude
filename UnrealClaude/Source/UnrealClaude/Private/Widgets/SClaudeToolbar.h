// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SComboBox.h"

DECLARE_DELEGATE(FOnToolbarAction)
DECLARE_DELEGATE_OneParam(FOnCheckboxChanged, bool)
DECLARE_DELEGATE_OneParam(FOnModelChanged, const FString& /*ModelId*/)

/**
 * Toolbar widget for Claude Editor
 * Handles model selection (all providers), Anthropic CLI/API toggle,
 * role config, UE context toggles, and session management
 */
class SClaudeToolbar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SClaudeToolbar)
		: _bUE57ContextEnabled(true)
		, _bProjectContextEnabled(true)
		, _bRestoreEnabled(false)
		, _SelectedModel(TEXT("claude-sonnet-4-6"))
		, _bAnthropicUseCLI(true)
	{}
		SLATE_ATTRIBUTE(bool, bUE57ContextEnabled)
		SLATE_ATTRIBUTE(bool, bProjectContextEnabled)
		SLATE_ATTRIBUTE(bool, bRestoreEnabled)
		SLATE_ATTRIBUTE(FString, SelectedModel)
		SLATE_ATTRIBUTE(bool, bAnthropicUseCLI)
		SLATE_ATTRIBUTE(FText, TokenCountText)
		SLATE_ATTRIBUTE(FSlateColor, TokenCountColor)
		SLATE_ATTRIBUTE(FText, CostText)
		SLATE_EVENT(FOnCheckboxChanged, OnUE57ContextChanged)
		SLATE_EVENT(FOnCheckboxChanged, OnProjectContextChanged)
		SLATE_EVENT(FOnToolbarAction, OnRefreshContext)
		SLATE_EVENT(FOnToolbarAction, OnRestoreSession)
		SLATE_EVENT(FOnToolbarAction, OnNewSession)
		SLATE_EVENT(FOnToolbarAction, OnClear)
		SLATE_EVENT(FOnToolbarAction, OnCopyLast)
		SLATE_EVENT(FOnToolbarAction, OnCopyChat)
		SLATE_EVENT(FOnToolbarAction, OnCompact)
		SLATE_EVENT(FOnToolbarAction, OnRoleConfig)
		SLATE_EVENT(FOnModelChanged, OnModelChanged)
		SLATE_EVENT(FOnCheckboxChanged, OnAnthropicModeChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Rebuild model options from all backends */
	void RefreshModelOptions();

private:
	TAttribute<bool> bUE57ContextEnabled;
	TAttribute<bool> bProjectContextEnabled;
	TAttribute<bool> bRestoreEnabled;
	TAttribute<FString> SelectedModel;
	TAttribute<bool> bAnthropicUseCLI;
	TAttribute<FText> TokenCountText;
	TAttribute<FSlateColor> TokenCountColor;
	TAttribute<FText> CostText;

	FOnCheckboxChanged OnUE57ContextChanged;
	FOnCheckboxChanged OnProjectContextChanged;
	FOnToolbarAction OnRefreshContext;
	FOnToolbarAction OnRestoreSession;
	FOnToolbarAction OnNewSession;
	FOnToolbarAction OnClear;
	FOnToolbarAction OnCopyLast;
	FOnToolbarAction OnCopyChat;
	FOnToolbarAction OnCompact;
	FOnToolbarAction OnRoleConfig;
	FOnModelChanged OnModelChanged;
	FOnCheckboxChanged OnAnthropicModeChanged;

	TArray<TSharedPtr<FString>> ModelOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ModelComboBox;

	/** Get display name for a model, including provider hint */
	static FString GetModelDisplayName(const FString& ModelId);
};
