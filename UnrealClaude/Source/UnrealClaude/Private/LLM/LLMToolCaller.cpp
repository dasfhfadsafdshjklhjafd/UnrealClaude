// Copyright Natali Caggiano. All Rights Reserved.

#include "LLMToolCaller.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/UnrealClaudeMCPServer.h"
#include "UnrealClaudeModule.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// Tool Registry Access
// ============================================================================

FMCPToolRegistry* FLLMToolCaller::GetToolRegistry()
{
	FUnrealClaudeModule& Module = FModuleManager::GetModuleChecked<FUnrealClaudeModule>(TEXT("UnrealClaude"));
	TSharedPtr<FUnrealClaudeMCPServer> Server = Module.GetMCPServer();
	if (Server.IsValid())
	{
		return Server->GetToolRegistry().Get();
	}
	return nullptr;
}

TArray<FMCPToolInfo> FLLMToolCaller::GetFilteredTools(EToolFilter Filter)
{
	FMCPToolRegistry* Registry = GetToolRegistry();
	if (!Registry)
	{
		return {};
	}

	TArray<FMCPToolInfo> AllTools = Registry->GetAllTools();

	if (Filter == EToolFilter::All)
	{
		return AllTools;
	}

	// ReadOnly filter
	TArray<FMCPToolInfo> Filtered;
	for (const FMCPToolInfo& Tool : AllTools)
	{
		if (Tool.Annotations.bReadOnlyHint)
		{
			Filtered.Add(Tool);
		}
	}
	return Filtered;
}

// ============================================================================
// Schema Conversion
// ============================================================================

FString FLLMToolCaller::ToJsonSchemaType(const FString& MCPType)
{
	// MCP types map directly to JSON Schema types
	if (MCPType == TEXT("number") || MCPType == TEXT("integer"))
	{
		return MCPType;
	}
	if (MCPType == TEXT("boolean"))
	{
		return TEXT("boolean");
	}
	if (MCPType == TEXT("array"))
	{
		return TEXT("array");
	}
	if (MCPType == TEXT("object"))
	{
		return TEXT("object");
	}
	return TEXT("string");
}

TSharedPtr<FJsonObject> FLLMToolCaller::BuildInputSchema(const FMCPToolInfo& Tool)
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Required;

	for (const FMCPToolParameter& Param : Tool.Parameters)
	{
		TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("type"), ToJsonSchemaType(Param.Type));
		PropObj->SetStringField(TEXT("description"), Param.Description);

		Properties->SetObjectField(Param.Name, PropObj);

		if (Param.bRequired)
		{
			Required.Add(MakeShared<FJsonValueString>(Param.Name));
		}
	}

	Schema->SetObjectField(TEXT("properties"), Properties);
	if (Required.Num() > 0)
	{
		Schema->SetArrayField(TEXT("required"), Required);
	}

	return Schema;
}

// ============================================================================
// Tool Definition Conversion — Anthropic Format
// ============================================================================

TArray<TSharedPtr<FJsonValue>> FLLMToolCaller::GetToolsForAnthropic(EToolFilter Filter)
{
	TArray<FMCPToolInfo> Tools = GetFilteredTools(Filter);
	TArray<TSharedPtr<FJsonValue>> Result;

	for (const FMCPToolInfo& Tool : Tools)
	{
		TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		ToolObj->SetStringField(TEXT("name"), Tool.Name);
		ToolObj->SetStringField(TEXT("description"), Tool.Description);
		ToolObj->SetObjectField(TEXT("input_schema"), BuildInputSchema(Tool));

		Result.Add(MakeShared<FJsonValueObject>(ToolObj));
	}

	return Result;
}

// ============================================================================
// Tool Definition Conversion — OpenAI Format
// ============================================================================

TArray<TSharedPtr<FJsonValue>> FLLMToolCaller::GetToolsForOpenAI(EToolFilter Filter)
{
	TArray<FMCPToolInfo> Tools = GetFilteredTools(Filter);
	TArray<TSharedPtr<FJsonValue>> Result;

	for (const FMCPToolInfo& Tool : Tools)
	{
		TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
		FuncObj->SetStringField(TEXT("name"), Tool.Name);
		FuncObj->SetStringField(TEXT("description"), Tool.Description);
		FuncObj->SetObjectField(TEXT("parameters"), BuildInputSchema(Tool));

		TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		ToolObj->SetStringField(TEXT("type"), TEXT("function"));
		ToolObj->SetObjectField(TEXT("function"), FuncObj);

		Result.Add(MakeShared<FJsonValueObject>(ToolObj));
	}

	return Result;
}

// ============================================================================
// Tool Execution
// ============================================================================

TPair<bool, FString> FLLMToolCaller::ExecuteToolCall(const FString& ToolName, const FString& InputJson)
{
	FMCPToolRegistry* Registry = GetToolRegistry();
	if (!Registry)
	{
		return TPair<bool, FString>(false, TEXT("MCP tool registry not available"));
	}

	// Parse input JSON
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	if (!InputJson.IsEmpty() && InputJson != TEXT("{}"))
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InputJson);
		if (!FJsonSerializer::Deserialize(Reader, Params) || !Params.IsValid())
		{
			return TPair<bool, FString>(false, FString::Printf(TEXT("Failed to parse tool input JSON for '%s'"), *ToolName));
		}
	}

	// Execute tool
	FMCPToolResult ToolResult = Registry->ExecuteTool(ToolName, Params.ToSharedRef());

	// Format result as text
	FString ResultText = ToolResult.Message;
	if (ToolResult.Data.IsValid())
	{
		FString DataJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DataJson);
		FJsonSerializer::Serialize(ToolResult.Data.ToSharedRef(), Writer);
		if (!ResultText.IsEmpty())
		{
			ResultText += TEXT("\n");
		}
		ResultText += DataJson;
	}

	return TPair<bool, FString>(ToolResult.bSuccess, ResultText);
}

// ============================================================================
// Response Parsing — Anthropic
// ============================================================================

bool FLLMToolCaller::AnthropicResponseHasToolUse(const TSharedPtr<FJsonObject>& ResponseRoot)
{
	FString StopReason;
	if (ResponseRoot->TryGetStringField(TEXT("stop_reason"), StopReason))
	{
		return StopReason == TEXT("tool_use");
	}
	return false;
}

TArray<FLLMToolCaller::FToolCall> FLLMToolCaller::ExtractAnthropicToolCalls(const TSharedPtr<FJsonObject>& ResponseRoot)
{
	TArray<FToolCall> Calls;

	const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
	if (!ResponseRoot->TryGetArrayField(TEXT("content"), ContentArray))
	{
		return Calls;
	}

	for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
	{
		const TSharedPtr<FJsonObject>* ContentObj = nullptr;
		if (!ContentValue->TryGetObject(ContentObj) || !ContentObj)
		{
			continue;
		}

		FString Type;
		if (!(*ContentObj)->TryGetStringField(TEXT("type"), Type) || Type != TEXT("tool_use"))
		{
			continue;
		}

		FToolCall Call;
		(*ContentObj)->TryGetStringField(TEXT("id"), Call.Id);
		(*ContentObj)->TryGetStringField(TEXT("name"), Call.Name);

		// Input is a JSON object — serialize it to string
		const TSharedPtr<FJsonObject>* InputObj = nullptr;
		if ((*ContentObj)->TryGetObjectField(TEXT("input"), InputObj) && InputObj)
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Call.InputJson);
			FJsonSerializer::Serialize((*InputObj).ToSharedRef(), Writer);
		}
		else
		{
			Call.InputJson = TEXT("{}");
		}

		Calls.Add(MoveTemp(Call));
	}

	return Calls;
}

// ============================================================================
// Response Parsing — OpenAI
// ============================================================================

TArray<FLLMToolCaller::FToolCall> FLLMToolCaller::ExtractOpenAIToolCalls(const TSharedPtr<FJsonObject>& ResponseRoot)
{
	TArray<FToolCall> Calls;

	const TArray<TSharedPtr<FJsonValue>>* ChoicesArray = nullptr;
	if (!ResponseRoot->TryGetArrayField(TEXT("choices"), ChoicesArray) || ChoicesArray->Num() == 0)
	{
		return Calls;
	}

	const TSharedPtr<FJsonObject>* ChoiceObj = nullptr;
	if (!(*ChoicesArray)[0]->TryGetObject(ChoiceObj) || !ChoiceObj)
	{
		return Calls;
	}

	const TSharedPtr<FJsonObject>* MessageObj = nullptr;
	if (!(*ChoiceObj)->TryGetObjectField(TEXT("message"), MessageObj) || !MessageObj)
	{
		return Calls;
	}

	const TArray<TSharedPtr<FJsonValue>>* ToolCallsArray = nullptr;
	if (!(*MessageObj)->TryGetArrayField(TEXT("tool_calls"), ToolCallsArray))
	{
		return Calls;
	}

	for (const TSharedPtr<FJsonValue>& ToolCallValue : *ToolCallsArray)
	{
		const TSharedPtr<FJsonObject>* ToolCallObj = nullptr;
		if (!ToolCallValue->TryGetObject(ToolCallObj) || !ToolCallObj)
		{
			continue;
		}

		FToolCall Call;
		(*ToolCallObj)->TryGetStringField(TEXT("id"), Call.Id);

		const TSharedPtr<FJsonObject>* FuncObj = nullptr;
		if ((*ToolCallObj)->TryGetObjectField(TEXT("function"), FuncObj) && FuncObj)
		{
			(*FuncObj)->TryGetStringField(TEXT("name"), Call.Name);
			(*FuncObj)->TryGetStringField(TEXT("arguments"), Call.InputJson);
		}

		Calls.Add(MoveTemp(Call));
	}

	return Calls;
}

// ============================================================================
// Message Building — Anthropic
// ============================================================================

TSharedPtr<FJsonObject> FLLMToolCaller::BuildAnthropicAssistantToolMessage(
	const TArray<TSharedPtr<FJsonValue>>& OriginalContent)
{
	// Echo back the assistant message with its content blocks (text + tool_use)
	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), TEXT("assistant"));
	Msg->SetArrayField(TEXT("content"), OriginalContent);
	return Msg;
}

TSharedPtr<FJsonObject> FLLMToolCaller::BuildAnthropicToolResultMessage(
	const TArray<FToolCall>& Calls,
	const TArray<TPair<bool, FString>>& Results)
{
	// Build a user message with tool_result content blocks
	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), TEXT("user"));

	TArray<TSharedPtr<FJsonValue>> ContentArray;

	for (int32 i = 0; i < Calls.Num(); ++i)
	{
		TSharedPtr<FJsonObject> ResultBlock = MakeShared<FJsonObject>();
		ResultBlock->SetStringField(TEXT("type"), TEXT("tool_result"));
		ResultBlock->SetStringField(TEXT("tool_use_id"), Calls[i].Id);
		ResultBlock->SetStringField(TEXT("content"), Results[i].Value);

		if (!Results[i].Key)
		{
			ResultBlock->SetBoolField(TEXT("is_error"), true);
		}

		ContentArray.Add(MakeShared<FJsonValueObject>(ResultBlock));
	}

	Msg->SetArrayField(TEXT("content"), ContentArray);
	return Msg;
}

// ============================================================================
// Message Building — OpenAI
// ============================================================================

TSharedPtr<FJsonObject> FLLMToolCaller::BuildOpenAIAssistantToolMessage(
	const FString& TextContent,
	const TSharedPtr<FJsonObject>& ResponseRoot)
{
	// Echo the full assistant message including tool_calls
	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), TEXT("assistant"));

	if (!TextContent.IsEmpty())
	{
		Msg->SetStringField(TEXT("content"), TextContent);
	}
	else
	{
		Msg->SetField(TEXT("content"), MakeShared<FJsonValueNull>());
	}

	// Copy tool_calls array from response
	const TArray<TSharedPtr<FJsonValue>>* ChoicesArray = nullptr;
	if (ResponseRoot->TryGetArrayField(TEXT("choices"), ChoicesArray) && ChoicesArray->Num() > 0)
	{
		const TSharedPtr<FJsonObject>* ChoiceObj = nullptr;
		if ((*ChoicesArray)[0]->TryGetObject(ChoiceObj) && ChoiceObj)
		{
			const TSharedPtr<FJsonObject>* MessageObj = nullptr;
			if ((*ChoiceObj)->TryGetObjectField(TEXT("message"), MessageObj) && MessageObj)
			{
				const TArray<TSharedPtr<FJsonValue>>* ToolCallsArray = nullptr;
				if ((*MessageObj)->TryGetArrayField(TEXT("tool_calls"), ToolCallsArray))
				{
					Msg->SetArrayField(TEXT("tool_calls"), *ToolCallsArray);
				}
			}
		}
	}

	return Msg;
}

TArray<TSharedPtr<FJsonObject>> FLLMToolCaller::BuildOpenAIToolResultMessages(
	const TArray<FToolCall>& Calls,
	const TArray<TPair<bool, FString>>& Results)
{
	// OpenAI uses one message per tool result
	TArray<TSharedPtr<FJsonObject>> Messages;

	for (int32 i = 0; i < Calls.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("tool"));
		Msg->SetStringField(TEXT("tool_call_id"), Calls[i].Id);
		Msg->SetStringField(TEXT("content"), Results[i].Value);

		Messages.Add(Msg);
	}

	return Messages;
}
