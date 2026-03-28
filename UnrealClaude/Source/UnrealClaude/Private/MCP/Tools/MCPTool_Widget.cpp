// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_Widget.h"
#include "WidgetEditor.h"

FMCPToolResult FMCPTool_Widget::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	if (!Params->TryGetStringField(TEXT("operation"), Operation))
		return FMCPToolResult::Error(TEXT("Missing required parameter: operation"));

	// ── Read operations (no WBP load needed first) ──────────────────────────

	if (Operation == TEXT("inspect_tree"))
	{
		FString AssetPath;
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return FMCPToolResult::Error(TEXT("inspect_tree requires 'asset_path'"));

		TSharedPtr<FJsonObject> Result = FWidgetEditor::InspectWidgetTree(AssetPath);
		if (!Result.IsValid())
			return FMCPToolResult::Error(TEXT("Failed to inspect widget tree"));

		bool bSuccess = false;
		Result->TryGetBoolField(TEXT("success"), bSuccess);
		FString Message;
		Result->TryGetStringField(TEXT("message"), Message);

		return bSuccess
			? FMCPToolResult::Success(Message, Result)
			: FMCPToolResult::Error(Message);
	}

	if (Operation == TEXT("get_properties"))
	{
		FString AssetPath, WidgetName;
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
			return FMCPToolResult::Error(TEXT("get_properties requires 'asset_path'"));
		if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMCPToolResult::Error(TEXT("get_properties requires 'widget_name'"));

		TSharedPtr<FJsonObject> Result = FWidgetEditor::GetWidgetProperties(AssetPath, WidgetName);
		if (!Result.IsValid())
			return FMCPToolResult::Error(TEXT("Failed to get widget properties"));

		bool bSuccess = false;
		Result->TryGetBoolField(TEXT("success"), bSuccess);
		FString Message;
		Result->TryGetStringField(TEXT("message"), Message);

		return bSuccess
			? FMCPToolResult::Success(Message, Result)
			: FMCPToolResult::Error(Message);
	}

	// ── Create (no pre-load needed) ──────────────────────────────────────────

	if (Operation == TEXT("create"))
	{
		FString Name, PackagePath, ParentClass, RootWidgetClass;
		if (!Params->TryGetStringField(TEXT("name"), Name))
			return FMCPToolResult::Error(TEXT("create requires 'name'"));
		if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
			return FMCPToolResult::Error(TEXT("create requires 'package_path'"));

		Params->TryGetStringField(TEXT("parent_class"), ParentClass);
		Params->TryGetStringField(TEXT("root_widget_class"), RootWidgetClass);

		TSharedPtr<FJsonObject> Result = FWidgetEditor::CreateWidgetBlueprint(Name, PackagePath, ParentClass, RootWidgetClass);
		if (!Result.IsValid())
			return FMCPToolResult::Error(TEXT("Failed to create Widget Blueprint"));

		bool bSuccess = false;
		Result->TryGetBoolField(TEXT("success"), bSuccess);
		FString Message;
		Result->TryGetStringField(TEXT("message"), Message);

		return bSuccess
			? FMCPToolResult::Success(Message, Result)
			: FMCPToolResult::Error(Message);
	}

	// ── Write operations (need WBP loaded) ──────────────────────────────────

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		return FMCPToolResult::Error(TEXT("This operation requires 'asset_path'"));

	FString LoadError;
	UWidgetBlueprint* WBP = FWidgetEditor::LoadWidgetBlueprint(AssetPath, LoadError);
	if (!WBP)
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load Widget Blueprint '%s': %s"), *AssetPath, *LoadError));

	TSharedPtr<FJsonObject> Result;

	if (Operation == TEXT("add_widget"))
	{
		FString WidgetClass, WidgetName, ParentName;
		if (!Params->TryGetStringField(TEXT("widget_class"), WidgetClass))
			return FMCPToolResult::Error(TEXT("add_widget requires 'widget_class'"));
		if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMCPToolResult::Error(TEXT("add_widget requires 'widget_name'"));
		Params->TryGetStringField(TEXT("parent_name"), ParentName);

		Result = FWidgetEditor::AddWidget(WBP, WidgetClass, WidgetName, ParentName);
	}
	else if (Operation == TEXT("remove_widget"))
	{
		FString WidgetName;
		if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMCPToolResult::Error(TEXT("remove_widget requires 'widget_name'"));

		Result = FWidgetEditor::RemoveWidget(WBP, WidgetName);
	}
	else if (Operation == TEXT("set_property"))
	{
		FString WidgetName;
		if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMCPToolResult::Error(TEXT("set_property requires 'widget_name'"));

		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj)
			return FMCPToolResult::Error(TEXT("set_property requires 'properties' object"));

		Result = FWidgetEditor::SetWidgetProperty(WBP, WidgetName, *PropsObj);
	}
	else if (Operation == TEXT("set_slot_property"))
	{
		FString WidgetName;
		if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMCPToolResult::Error(TEXT("set_slot_property requires 'widget_name'"));

		const TSharedPtr<FJsonObject>* SlotObj = nullptr;
		if (!Params->TryGetObjectField(TEXT("slot_properties"), SlotObj) || !SlotObj)
			return FMCPToolResult::Error(TEXT("set_slot_property requires 'slot_properties' object"));

		Result = FWidgetEditor::SetSlotProperty(WBP, WidgetName, *SlotObj);
	}
	else if (Operation == TEXT("save"))
	{
		Result = FWidgetEditor::SaveWidgetBlueprint(WBP);
	}
	else if (Operation == TEXT("batch"))
	{
		const TArray<TSharedPtr<FJsonValue>>* OpsArray = nullptr;
		if (!Params->TryGetArrayField(TEXT("operations"), OpsArray) || !OpsArray)
			return FMCPToolResult::Error(TEXT("batch requires 'operations' array"));

		Result = FWidgetEditor::ExecuteBatch(WBP, *OpsArray);
	}
	else
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Unknown operation: '%s'"), *Operation));
	}

	if (!Result.IsValid())
		return FMCPToolResult::Error(TEXT("Operation returned no result"));

	bool bSuccess = false;
	Result->TryGetBoolField(TEXT("success"), bSuccess);
	FString Message;
	Result->TryGetStringField(TEXT("message"), Message);

	return bSuccess
		? FMCPToolResult::Success(Message, Result)
		: FMCPToolResult::Error(Message);
}
