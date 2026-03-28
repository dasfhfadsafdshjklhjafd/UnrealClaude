// Copyright Natali Caggiano. All Rights Reserved.

#include "LLM/LLMPricingConfig.h"
#include "UnrealClaudeModule.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

bool FLLMPricingConfig::LoadFromFile(const FString& FilePath)
{
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("FLLMPricingConfig: Could not load pricing file: %s"), *FilePath);
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogUnrealClaude, Error, TEXT("FLLMPricingConfig: Failed to parse pricing JSON from: %s"), *FilePath);
		return false;
	}

	// Read version date
	RootObject->TryGetStringField(TEXT("version_date"), VersionDate);

	// Read models
	const TSharedPtr<FJsonObject>* ModelsObject = nullptr;
	if (!RootObject->TryGetObjectField(TEXT("models"), ModelsObject) || !ModelsObject)
	{
		UE_LOG(LogUnrealClaude, Error, TEXT("FLLMPricingConfig: No 'models' object in pricing JSON"));
		return false;
	}

	Models.Empty();

	for (const auto& Pair : (*ModelsObject)->Values)
	{
		const FString& ModelId = Pair.Key;
		const TSharedPtr<FJsonObject>* EntryObject = nullptr;

		if (!Pair.Value->TryGetObject(EntryObject) || !EntryObject)
		{
			continue;
		}

		FLLMModelPricing Pricing;
		Pricing.ModelId = ModelId;

		FString Provider;
		if ((*EntryObject)->TryGetStringField(TEXT("provider"), Provider))
		{
			Pricing.ProviderId = Provider;
		}

		(*EntryObject)->TryGetNumberField(TEXT("input_per_1m"), Pricing.InputPer1M);
		(*EntryObject)->TryGetNumberField(TEXT("output_per_1m"), Pricing.OutputPer1M);
		(*EntryObject)->TryGetNumberField(TEXT("cached_input_per_1m"), Pricing.CachedInputPer1M);

		Models.Add(ModelId, MoveTemp(Pricing));
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("FLLMPricingConfig: Loaded pricing for %d models (version: %s)"),
		Models.Num(), *VersionDate);

	return true;
}

const FLLMModelPricing* FLLMPricingConfig::GetPricing(const FString& ModelId) const
{
	return Models.Find(ModelId);
}

float FLLMPricingConfig::CalculateCost(const FString& ModelId, int32 InputTokens, int32 OutputTokens, int32 CachedInputTokens) const
{
	const FLLMModelPricing* Pricing = GetPricing(ModelId);
	if (!Pricing)
	{
		return 0.0f;
	}
	return Pricing->CalculateCost(InputTokens, OutputTokens, CachedInputTokens);
}

TArray<FString> FLLMPricingConfig::GetAllModelIds() const
{
	TArray<FString> Result;
	Models.GetKeys(Result);
	return Result;
}
