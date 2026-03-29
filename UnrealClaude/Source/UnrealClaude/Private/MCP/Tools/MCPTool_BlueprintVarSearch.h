// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Find all reads/writes of a Blueprint variable across its graphs.
 *
 * Answers "where is HealthState written?" without reading any full graph.
 * Returns a flat list of {graph_name, node_title, operation} entries — typically
 * under 100 tokens for a variable that appears in 3-5 places.
 */
class FMCPTool_BlueprintVarSearch : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("blueprint_find_variable");
		Info.Description = TEXT(
			"Find every read (Get) and write (Set) of a named variable across all graphs in a Blueprint.\n\n"
			"Much cheaper than reading full graphs when you need to know where a variable is used. "
			"Returns the graph name and node title for each occurrence.\n\n"
			"Use 'operation' to filter:\n"
			"- 'write' — only Set nodes (where the variable is assigned)\n"
			"- 'read'  — only Get nodes (where the variable is read)\n"
			"- 'any'   — both (default)\n\n"
			"The variable_name match is case-insensitive and matches the exact variable name "
			"(not a substring). For partial matches, use blueprint_search instead.\n\n"
			"Examples:\n"
			"- blueprint_find_variable({\"blueprint_path\": \"/Game/Game/Blueprints/BP_PlayerBase\", "
			"\"variable_name\": \"HealthState\"})\n"
			"- blueprint_find_variable({\"blueprint_path\": \"/Game/Game/Blueprints/BP_PlayerBase\", "
			"\"variable_name\": \"CurrentAmmo\", \"operation\": \"write\"})"
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("blueprint_path"), TEXT("string"),
				TEXT("Full Blueprint asset path e.g. '/Game/Game/Blueprints/BP_PlayerBase'"), true),
			FMCPToolParameter(TEXT("variable_name"), TEXT("string"),
				TEXT("Exact variable name to search for (case-insensitive)"), true),
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("'read', 'write', or 'any' (default: 'any')"), false, TEXT("any")),
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
