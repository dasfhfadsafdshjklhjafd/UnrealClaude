// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Think / plan before acting
 *
 * Forces the model to articulate a step-by-step execution plan before
 * calling any Blueprint or asset tools. The tool returns the plan unchanged —
 * its value is in requiring structured reasoning upfront, which dramatically
 * reduces the number of round-trips needed for multi-step investigations.
 *
 * Usage pattern:
 *   1. Call think() with a numbered list of steps you intend to take
 *   2. Execute ALL steps in the same response without checking back in
 *   3. Only return to the user when you have a complete answer
 */
class FMCPTool_Think : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("think");
		Info.Description = TEXT(
			"Plan your investigation before using any other tools.\n\n"
			"Call this FIRST with a numbered list of every tool call you intend to make "
			"to answer the user's question. Then execute ALL steps in the same response "
			"without asking for permission between steps.\n\n"
			"This keeps conversations short and avoids repeated round-trips.\n\n"
			"Example:\n"
			"think({\n"
			"  \"steps\": [\n"
			"    \"1. blueprint_query inspect BP_PlayerBase to get function list\",\n"
			"    \"2. blueprint_read_graph BP_PlayerBase summary_only=true for EventGraph\",\n"
			"    \"3. blueprint_read_graph BP_PlayerBase graph=f_FireOneShot for full detail\"\n"
			"  ],\n"
			"  \"goal\": \"Find where fire montage is called\"\n"
			"})"
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("steps"), TEXT("array"),
				TEXT("Ordered list of tool calls you plan to make"), true),
			FMCPToolParameter(TEXT("goal"), TEXT("string"),
				TEXT("What you are trying to find out"), true),
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
