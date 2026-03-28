// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SComboBox.h"
#include "LLM/ILLMBackend.h"

/**
 * Flyout panel for configuring per-role model assignments.
 * Shows a dropdown per role (Worker, Critic, Architect, Escalation, DocsAgent)
 * with all available models from all backends.
 */
class SRoleConfigPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRoleConfigPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	/** Build one row: label + model dropdown */
	TSharedRef<SWidget> BuildRoleRow(EModelRole Role);

	/** Called when a model is selected for a role */
	void OnRoleModelSelected(TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo, EModelRole Role);

	/** Save all assignments */
	FReply OnSave();

	/** Reset to defaults */
	FReply OnReset();

	/** Rebuild all dropdowns from current assignments */
	void RefreshFromAssignments();

	/** Get display name for a model ID */
	static FString GetModelDisplay(const FString& ModelId);

	/** All available model IDs (shared across dropdowns) */
	TArray<TSharedPtr<FString>> ModelOptions;

	/** Per-role combo boxes (for refreshing selection) */
	TMap<EModelRole, TSharedPtr<SComboBox<TSharedPtr<FString>>>> RoleComboBoxes;

	/** Per-role currently selected model ID */
	TMap<EModelRole, FString> SelectedModels;
};
