// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Blueprint search (RAG)
 *
 * Keyword search over the Blueprint inverted index. Returns the top matching
 * chunks (metadata + graph windows) so the LLM knows which blueprints and
 * graphs are relevant before paying the cost of a full graph read.
 *
 * Usage:
 *   1. blueprint_search(query="fire weapon montage")  — find relevant BPs
 *   2. blueprint_read_graph(...)                       — read only those graphs
 */
class FMCPTool_BlueprintSearch : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("blueprint_search");
		Info.Description = TEXT(
			"Search the Blueprint index for blueprints and graphs relevant to a query.\n\n"
			"Returns the top matching chunks — each chunk includes the blueprint path, "
			"graph name, and a summary of the nodes or variables it contains.\n\n"
			"Use this BEFORE blueprint_read_graph to avoid reading full graphs blindly. "
			"A good search query uses specific identifiers: variable names, function names, "
			"node titles, component names, or class names.\n\n"
			"If the index is empty (first run), it is built automatically — this may take "
			"a few seconds the first time.\n\n"
			"Examples:\n"
			"- blueprint_search({\"query\": \"fire weapon montage\"})\n"
			"- blueprint_search({\"query\": \"health damage death\"})\n"
			"- blueprint_search({\"query\": \"replication server RPC\", \"max_results\": 12})"
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("query"), TEXT("string"),
				TEXT("Space-separated keywords to search for (variable names, function names, node titles, etc.)"), true),
			FMCPToolParameter(TEXT("max_results"), TEXT("number"),
				TEXT("Maximum number of chunks to return (default: 8)"), false, TEXT("8")),
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
