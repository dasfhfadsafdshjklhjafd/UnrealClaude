// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

/**
 * MCP Tool: Read Blueprint graph node structure (read-only)
 *
 * Serializes Blueprint graph nodes and their pin connections into
 * a compact JSON format suited for LLM consumption. Strips GUIDs
 * and positions; expresses connections by node index + pin name.
 *
 * Operations via parameters:
 *   - Omit graph_name  -> list all graphs with node counts
 *   - Provide graph_name -> serialize that graph's nodes and connections
 */
class FMCPTool_BlueprintGraphRead : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("blueprint_read_graph");
		Info.Description = TEXT(
			"Read Blueprint node graph structure (read-only).\n\n"
			"Usage:\n"
			"- Omit 'graph_name' to list all graphs in the Blueprint with their node counts.\n"
			"- Provide 'graph_name' to get the full node/pin/connection structure of that graph.\n\n"
			"Output format: nodes are indexed 0..N. Pin connections reference [targetNodeIndex, pinName].\n"
			"Pin 'kind' values: 'exec' (execution flow), 'bool', 'float', 'int', 'object', 'struct', etc.\n\n"
			"For large graphs use 'max_nodes' to page through in chunks (start_node + max_nodes).\n\n"
			"Example paths:\n"
			"- '/Game/Core/Characters/BP_PlayerBase'\n"
			"- '/Game/Weapons/BP_RifleBase'\n\n"
			"Always call without graph_name first to discover available graphs."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("blueprint_path"), TEXT("string"),
				TEXT("Full Blueprint asset path e.g. '/Game/Core/Characters/BP_PlayerBase'"), true),
			FMCPToolParameter(TEXT("graph_name"), TEXT("string"),
				TEXT("Graph to read. Omit to list all graphs."), false),
			FMCPToolParameter(TEXT("max_nodes"), TEXT("number"),
				TEXT("Max nodes to return (default: 150). Increase for large graphs."), false, TEXT("150")),
			FMCPToolParameter(TEXT("start_node"), TEXT("number"),
				TEXT("First node index to return (default: 0). Use with max_nodes for paging."), false, TEXT("0")),
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecuteListGraphs(UBlueprint* Blueprint);
	FMCPToolResult ExecuteReadGraph(UBlueprint* Blueprint, const FString& GraphName, int32 MaxNodes, int32 StartNode);

	static UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName, FString& OutType);
	static TSharedPtr<FJsonObject> SerializeGraph(UEdGraph* Graph, int32 StartNode, int32 MaxNodes, bool& bOutTruncated);
	static TSharedPtr<FJsonObject> SerializeNode(UEdGraphNode* Node, const TMap<UEdGraphNode*, int32>& IndexMap);
};
