// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Widget Blueprint editor operations
 *
 * Read operations:
 *   - inspect_tree:      Get the full widget hierarchy
 *   - get_properties:    Get widget visual and slot properties
 *
 * Write operations:
 *   - create:            Create a new Widget Blueprint
 *   - add_widget:        Add a widget to the tree
 *   - remove_widget:     Remove a widget from the tree
 *   - set_property:      Set visual properties on a widget
 *   - set_slot_property: Set slot (layout) properties on a widget
 *   - save:              Save the Widget Blueprint
 *   - batch:             Execute multiple operations in one call
 */
class FMCPTool_Widget : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("widget_editor");
		Info.Description = TEXT(
			"Read and modify Widget Blueprints (UMG).\n\n"
			"Read operations:\n"
			"- 'inspect_tree': Get the full widget hierarchy with slot/visual properties\n"
			"- 'get_properties': Get detailed properties of a specific widget\n\n"
			"Write operations:\n"
			"- 'create': Create a new Widget Blueprint asset\n"
			"- 'add_widget': Add a widget to the tree under a parent\n"
			"- 'remove_widget': Remove a widget by name\n"
			"- 'set_property': Set visual properties (text, color, opacity, etc.)\n"
			"- 'set_slot_property': Set slot/layout properties (position, size, anchors)\n"
			"- 'save': Save the Widget Blueprint to disk\n"
			"- 'batch': Execute multiple operations atomically\n\n"
			"Example paths:\n"
			"- '/Game/UI/WBP_MainMenu'\n"
			"- '/Game/UI/WBP_HUD'\n\n"
			"Returns: Widget tree hierarchy, properties, or operation results."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("Operation: inspect_tree, get_properties, create, add_widget, remove_widget, set_property, set_slot_property, save, batch"), true),
			FMCPToolParameter(TEXT("asset_path"), TEXT("string"),
				TEXT("Full asset path to the Widget Blueprint (e.g. '/Game/UI/WBP_MainMenu')"), false),
			FMCPToolParameter(TEXT("widget_name"), TEXT("string"),
				TEXT("Name of the widget to target (for get_properties, add_widget, remove_widget, set_property, set_slot_property)"), false),
			FMCPToolParameter(TEXT("widget_class"), TEXT("string"),
				TEXT("Widget class to create (e.g. 'TextBlock', 'Button', 'Image', 'ProgressBar', 'CanvasPanel')"), false),
			FMCPToolParameter(TEXT("parent_name"), TEXT("string"),
				TEXT("Name of the parent widget (for add_widget; omit for root)"), false),
			FMCPToolParameter(TEXT("properties"), TEXT("object"),
				TEXT("Key-value map of visual properties to set (for set_property)"), false),
			FMCPToolParameter(TEXT("slot_properties"), TEXT("object"),
				TEXT("Key-value map of slot properties to set (for set_slot_property)"), false),
			FMCPToolParameter(TEXT("name"), TEXT("string"),
				TEXT("Asset name (for create)"), false),
			FMCPToolParameter(TEXT("package_path"), TEXT("string"),
				TEXT("Package path for new asset (for create, e.g. '/Game/UI')"), false),
			FMCPToolParameter(TEXT("parent_class"), TEXT("string"),
				TEXT("Parent class for new Widget Blueprint (for create, default: 'UserWidget')"), false),
			FMCPToolParameter(TEXT("root_widget_class"), TEXT("string"),
				TEXT("Root widget class for new Widget Blueprint (for create, default: 'CanvasPanel')"), false),
			FMCPToolParameter(TEXT("operations"), TEXT("array"),
				TEXT("Array of operation objects for batch execution"), false)
		};
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
