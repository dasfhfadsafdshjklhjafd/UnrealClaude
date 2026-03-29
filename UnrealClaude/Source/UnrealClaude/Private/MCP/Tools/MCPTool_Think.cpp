// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_Think.h"
#include "UnrealClaudeModule.h"

FMCPToolResult FMCPTool_Think::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Goal;
	Params->TryGetStringField(TEXT("goal"), Goal);

	const TArray<TSharedPtr<FJsonValue>>* StepsArray = nullptr;
	TArray<FString> Steps;
	if (Params->TryGetArrayField(TEXT("steps"), StepsArray))
	{
		for (const TSharedPtr<FJsonValue>& StepVal : *StepsArray)
		{
			FString Step;
			if (StepVal.IsValid() && StepVal->TryGetString(Step))
			{
				Steps.Add(Step);
			}
		}
	}

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("status"), TEXT("plan_recorded"));
	if (!Goal.IsEmpty())
	{
		Response->SetStringField(TEXT("goal"), Goal);
	}

	TArray<TSharedPtr<FJsonValue>> StepsJson;
	for (const FString& Step : Steps)
	{
		StepsJson.Add(MakeShared<FJsonValueString>(Step));
	}
	Response->SetArrayField(TEXT("steps"), StepsJson);
	Response->SetStringField(TEXT("instruction"), TEXT("Execute all steps above in this same response. Do not ask the user for confirmation between steps."));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Plan recorded (%d steps) — now execute all steps without further check-ins"), Steps.Num()),
		Response
	);
}
