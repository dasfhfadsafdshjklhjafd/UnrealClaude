// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_BlueprintVarSearch.h"
#include "BlueprintUtils.h"
#include "UnrealClaudeModule.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_Variable.h"

FMCPToolResult FMCPTool_BlueprintVarSearch::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString BpPath;
	if (!Params->TryGetStringField(TEXT("blueprint_path"), BpPath) || BpPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("'blueprint_path' is required"));
	}

	FString VarName;
	if (!Params->TryGetStringField(TEXT("variable_name"), VarName) || VarName.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("'variable_name' is required"));
	}

	FString Operation = TEXT("any");
	Params->TryGetStringField(TEXT("operation"), Operation);
	Operation = Operation.ToLower();
	if (Operation != TEXT("read") && Operation != TEXT("write") && Operation != TEXT("any"))
	{
		return FMCPToolResult::Error(TEXT("'operation' must be 'read', 'write', or 'any'"));
	}

	const bool bWantReads  = (Operation == TEXT("read")  || Operation == TEXT("any"));
	const bool bWantWrites = (Operation == TEXT("write") || Operation == TEXT("any"));

	FString LoadError;
	UBlueprint* BP = FBlueprintUtils::LoadBlueprint(BpPath, LoadError);
	if (!BP)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load blueprint '%s': %s"), *BpPath, *LoadError));
	}

	const FName SearchName(*VarName);
	const FName SearchNameLower(*VarName.ToLower());

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	TArray<TSharedPtr<FJsonValue>> MatchesJson;

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		FString GraphName = Graph->GetName();

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node);
			if (!VarNode) continue;

			// Case-insensitive name match
			FName NodeVarName = VarNode->GetVarName();
			if (!NodeVarName.IsEqual(SearchName, ENameCase::IgnoreCase)) continue;

			// Determine if this is a read or write
			const FName NodeClass = Node->GetClass()->GetFName();
			const bool bIsWrite = (NodeClass == TEXT("K2Node_VariableSet"));
			const bool bIsRead  = !bIsWrite; // K2Node_VariableGet and others

			if (bIsWrite && !bWantWrites) continue;
			if (bIsRead  && !bWantReads)  continue;

			FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			FString OpStr = bIsWrite ? TEXT("write") : TEXT("read");

			TSharedPtr<FJsonObject> MatchObj = MakeShared<FJsonObject>();
			MatchObj->SetStringField(TEXT("graph"),     GraphName);
			MatchObj->SetStringField(TEXT("operation"), OpStr);
			MatchObj->SetStringField(TEXT("node"),      NodeTitle);
			MatchesJson.Add(MakeShared<FJsonValueObject>(MatchObj));
		}
	}

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("blueprint"), BP->GetName());
	Response->SetStringField(TEXT("variable"),  VarName);
	Response->SetNumberField(TEXT("count"),     MatchesJson.Num());
	Response->SetArrayField(TEXT("usages"),     MatchesJson);

	if (MatchesJson.IsEmpty())
	{
		Response->SetStringField(TEXT("message"),
			FString::Printf(TEXT("No %s usages of '%s' found. Check spelling or use blueprint_search for partial matches."),
				*Operation, *VarName));
	}

	FString Summary = FString::Printf(TEXT("Found %d %s usage(s) of '%s' in %s"),
		MatchesJson.Num(), *Operation, *VarName, *BP->GetName());

	return FMCPToolResult::Success(Summary, Response);
}
