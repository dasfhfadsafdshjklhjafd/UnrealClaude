// Copyright Natali Caggiano. All Rights Reserved.

#include "SRoleConfigPanel.h"
#include "ClaudeSubsystem.h"
#include "UnrealClaudeModule.h"
#include "LLM/LLMBackendRegistry.h"
#include "LLM/LLMRoleConfig.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "UnrealClaude"

FString SRoleConfigPanel::GetModelDisplay(const FString& ModelId)
{
	if (ModelId == TEXT("claude-opus-4-6"))           return TEXT("Opus 4.6");
	if (ModelId == TEXT("claude-sonnet-4-6"))         return TEXT("Sonnet 4.6");
	if (ModelId == TEXT("claude-haiku-4-5-20251001")) return TEXT("Haiku 4.5");
	if (ModelId == TEXT("gpt-4o"))                    return TEXT("GPT-4o");
	if (ModelId == TEXT("gpt-4o-mini"))               return TEXT("GPT-4o Mini");
	if (ModelId == TEXT("gpt-5.2-codex"))             return TEXT("GPT-5.2 Codex");
	if (ModelId == TEXT("gpt-5.4"))                   return TEXT("GPT-5.4");
	if (ModelId == TEXT("gpt-5.4-mini"))              return TEXT("GPT-5.4 Mini");
	if (ModelId == TEXT("o3"))                        return TEXT("o3");
	if (ModelId == TEXT("o4-mini"))                   return TEXT("o4-mini");
	return ModelId;
}

void SRoleConfigPanel::Construct(const FArguments& InArgs)
{
	// Build model options from all backends (not filtered by Anthropic mode — roles can target any backend)
	FLLMBackendRegistry& Registry = FClaudeCodeSubsystem::Get().GetBackendRegistry();
	for (ILLMBackend* Backend : Registry.GetAllBackends())
	{
		if (!Backend) continue;
		for (const FString& Model : Backend->GetSupportedModels())
		{
			// Avoid duplicates (Claude models appear in both CLI and API backends)
			bool bAlreadyExists = false;
			for (const TSharedPtr<FString>& Existing : ModelOptions)
			{
				if (*Existing == Model) { bAlreadyExists = true; break; }
			}
			if (!bAlreadyExists)
			{
				ModelOptions.Add(MakeShared<FString>(Model));
			}
		}
	}

	// Load current assignments
	FLLMRoleManager& RoleManager = FClaudeCodeSubsystem::Get().GetRoleManager();
	for (EModelRole Role : GetAllModelRoles())
	{
		FModelRoleAssignment A = RoleManager.GetAssignment(Role);
		SelectedModels.Add(Role, A.ModelId);
	}

	// Build UI
	TSharedPtr<SVerticalBox> RoleRows = SNew(SVerticalBox);

	for (EModelRole Role : GetAllModelRoles())
	{
		RoleRows->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				BuildRoleRow(Role)
			];
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(12.0f, 8.0f))
		[
			SNew(SVerticalBox)

			// Title
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("RoleConfigTitle", "Role Model Assignments"))
				.TextStyle(FAppStyle::Get(), "LargeText")
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("RoleConfigDesc", "Assign which model handles each role. Backend is auto-resolved from model."))
				.TextStyle(FAppStyle::Get(), "SmallText")
				.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.55f)))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(SSeparator)
			]

			// Role rows
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				RoleRows.ToSharedRef()
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SSeparator)
			]

			// Buttons
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("SaveRoles", "Save"))
					.OnClicked(this, &SRoleConfigPanel::OnSave)
					.ToolTipText(LOCTEXT("SaveRolesTip", "Save role assignments to project config"))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("ResetRoles", "Reset Defaults"))
					.OnClicked(this, &SRoleConfigPanel::OnReset)
					.ToolTipText(LOCTEXT("ResetRolesTip", "Reset all roles to default model assignments"))
				]
			]
		]
	];
}

TSharedRef<SWidget> SRoleConfigPanel::BuildRoleRow(EModelRole Role)
{
	FString RoleName = GetModelRoleDisplayName(Role);
	FString RoleDesc = GetModelRoleDescription(Role);

	TSharedPtr<SComboBox<TSharedPtr<FString>>> ComboBox;

	TSharedRef<SWidget> Row = SNew(SHorizontalBox)

		// Role name + description
		+ SHorizontalBox::Slot()
		.FillWidth(0.35f)
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(RoleName))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(RoleDesc))
				.TextStyle(FAppStyle::Get(), "SmallText")
				.ColorAndOpacity(FSlateColor(FLinearColor(0.45f, 0.45f, 0.5f)))
			]
		]

		// Model dropdown
		+ SHorizontalBox::Slot()
		.FillWidth(0.65f)
		.VAlign(VAlign_Center)
		[
			SAssignNew(ComboBox, SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&ModelOptions)
			.OnSelectionChanged_Lambda([this, Role](TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo)
			{
				OnRoleModelSelected(Item, SelectInfo, Role);
			})
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
			{
				return SNew(STextBlock)
					.Text(FText::FromString(SRoleConfigPanel::GetModelDisplay(Item.IsValid() ? *Item : FString())));
			})
			[
				SNew(STextBlock)
				.Text_Lambda([this, Role]()
				{
					const FString* Found = SelectedModels.Find(Role);
					return FText::FromString(Found ? GetModelDisplay(*Found) : TEXT("(none)"));
				})
			]
		];

	RoleComboBoxes.Add(Role, ComboBox);

	// Set initial selection
	FString CurrentModel = SelectedModels.FindRef(Role);
	for (const TSharedPtr<FString>& Option : ModelOptions)
	{
		if (*Option == CurrentModel)
		{
			ComboBox->SetSelectedItem(Option);
			break;
		}
	}

	return Row;
}

void SRoleConfigPanel::OnRoleModelSelected(TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo, EModelRole Role)
{
	if (Item.IsValid())
	{
		SelectedModels.Add(Role, *Item);
	}
}

FReply SRoleConfigPanel::OnSave()
{
	FLLMRoleManager& RoleManager = FClaudeCodeSubsystem::Get().GetRoleManager();
	FLLMBackendRegistry& Registry = FClaudeCodeSubsystem::Get().GetBackendRegistry();

	for (const auto& Pair : SelectedModels)
	{
		FString ProviderId = Registry.ResolveBackendForModel(Pair.Value);
		RoleManager.SetAssignment(Pair.Key, ProviderId, Pair.Value);
	}

	RoleManager.SaveToConfig();

	UE_LOG(LogUnrealClaude, Log, TEXT("Role assignments saved"));
	return FReply::Handled();
}

FReply SRoleConfigPanel::OnReset()
{
	FLLMRoleManager& RoleManager = FClaudeCodeSubsystem::Get().GetRoleManager();
	RoleManager.ResetToDefaults();
	RoleManager.SaveToConfig();

	// Refresh selections from defaults
	for (EModelRole Role : GetAllModelRoles())
	{
		FModelRoleAssignment A = RoleManager.GetAssignment(Role);
		SelectedModels.Add(Role, A.ModelId);

		// Update combo box selection
		TSharedPtr<SComboBox<TSharedPtr<FString>>>* ComboPtr = RoleComboBoxes.Find(Role);
		if (ComboPtr && ComboPtr->IsValid())
		{
			for (const TSharedPtr<FString>& Option : ModelOptions)
			{
				if (*Option == A.ModelId)
				{
					(*ComboPtr)->SetSelectedItem(Option);
					break;
				}
			}
		}
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Role assignments reset to defaults"));
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
