// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Forward declarations
class FMCPToolRegistry;
struct FMCPToolInfo;
struct FMCPToolResult;

/**
 * Filtering mode for which tools to expose to an LLM backend.
 */
enum class EToolFilter : uint8
{
	/** All registered tools (read-only + modifying + destructive) */
	All,
	/** Only tools with bReadOnlyHint = true */
	ReadOnly
};

/**
 * Shared helper for converting MCP tools to API tool-calling formats
 * and executing the tool-call loop.
 *
 * Used by FAnthropicAPIBackend and FOpenAIAPIBackend.
 * This is NOT a backend — it's a stateless utility.
 */
class FLLMToolCaller
{
public:
	// ---- Tool Definition Conversion ----

	/**
	 * Get MCP tool definitions as Anthropic API format.
	 * Returns a JSON array suitable for the "tools" field.
	 *
	 * Format per tool:
	 *   { "name": "...", "description": "...", "input_schema": { "type":"object", "properties":{...}, "required":[...] } }
	 */
	static TArray<TSharedPtr<FJsonValue>> GetToolsForAnthropic(EToolFilter Filter = EToolFilter::All);

	/**
	 * Get MCP tool definitions as OpenAI API format.
	 * Returns a JSON array suitable for the "tools" field.
	 *
	 * Format per tool:
	 *   { "type": "function", "function": { "name": "...", "description": "...", "parameters": { "type":"object", ... } } }
	 */
	static TArray<TSharedPtr<FJsonValue>> GetToolsForOpenAI(EToolFilter Filter = EToolFilter::ReadOnly);

	// ---- Tool Execution ----

	/**
	 * Execute a single tool call by name and return the result.
	 * Runs on game thread (FMCPToolRegistry handles thread marshalling).
	 *
	 * @param ToolName MCP tool name
	 * @param InputJson JSON string of tool input parameters
	 * @return Pair of (success, result_text)
	 */
	static TPair<bool, FString> ExecuteToolCall(const FString& ToolName, const FString& InputJson);

	// ---- Response Parsing ----

	/** Anthropic: check if response has stop_reason "tool_use" */
	static bool AnthropicResponseHasToolUse(const TSharedPtr<FJsonObject>& ResponseRoot);

	/**
	 * Anthropic: extract tool_use blocks from content array.
	 * Returns array of { id, name, input (as JSON string) }
	 */
	struct FToolCall
	{
		FString Id;
		FString Name;
		FString InputJson;
	};
	static TArray<FToolCall> ExtractAnthropicToolCalls(const TSharedPtr<FJsonObject>& ResponseRoot);

	/**
	 * OpenAI: extract tool calls from choices[0].message.tool_calls.
	 * Returns array of { id, name, input (as JSON string) }
	 */
	static TArray<FToolCall> ExtractOpenAIToolCalls(const TSharedPtr<FJsonObject>& ResponseRoot);

	// ---- Message Building ----

	/**
	 * Anthropic: build the assistant message with tool_use blocks (to echo back)
	 * and the user message with tool_result blocks.
	 */
	static TSharedPtr<FJsonObject> BuildAnthropicAssistantToolMessage(
		const TArray<TSharedPtr<FJsonValue>>& OriginalContent);

	static TSharedPtr<FJsonObject> BuildAnthropicToolResultMessage(
		const TArray<FToolCall>& Calls,
		const TArray<TPair<bool, FString>>& Results);

	/**
	 * OpenAI: build the assistant message echoing tool calls,
	 * and individual tool result messages.
	 */
	static TSharedPtr<FJsonObject> BuildOpenAIAssistantToolMessage(
		const FString& TextContent,
		const TSharedPtr<FJsonObject>& ResponseRoot);

	static TArray<TSharedPtr<FJsonObject>> BuildOpenAIToolResultMessages(
		const TArray<FToolCall>& Calls,
		const TArray<TPair<bool, FString>>& Results);

private:
	/** Get the tool registry from the module's MCP server */
	static FMCPToolRegistry* GetToolRegistry();

	/** Filter tools based on filter mode */
	static TArray<FMCPToolInfo> GetFilteredTools(EToolFilter Filter);

	/** Convert MCP parameter type to JSON Schema type */
	static FString ToJsonSchemaType(const FString& MCPType);

	/** Build JSON Schema properties object from tool parameters */
	static TSharedPtr<FJsonObject> BuildInputSchema(const FMCPToolInfo& Tool);
};
