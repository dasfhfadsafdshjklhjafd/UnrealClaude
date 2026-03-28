// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Pricing entry for a single model.
 * All costs are in USD per 1 million tokens.
 */
struct UNREALCLAUDE_API FLLMModelPricing
{
	/** Model ID (e.g. "claude-sonnet-4-6", "gpt-4o") */
	FString ModelId;

	/** Provider ID (e.g. "anthropic-api", "openai-api") */
	FString ProviderId;

	/** Cost per 1M input tokens (USD) */
	float InputPer1M = 0.0f;

	/** Cost per 1M output tokens (USD) */
	float OutputPer1M = 0.0f;

	/** Cost per 1M cached input tokens (USD). 0 if caching not supported. */
	float CachedInputPer1M = 0.0f;

	/** Calculate cost for a given token count */
	float CalculateCost(int32 InputTokens, int32 OutputTokens, int32 CachedInputTokens = 0) const
	{
		float Cost = 0.0f;
		Cost += (static_cast<float>(InputTokens) / 1000000.0f) * InputPer1M;
		Cost += (static_cast<float>(OutputTokens) / 1000000.0f) * OutputPer1M;
		if (CachedInputTokens > 0 && CachedInputPer1M > 0.0f)
		{
			// Cached tokens replace some input tokens at a lower rate
			Cost += (static_cast<float>(CachedInputTokens) / 1000000.0f) * (CachedInputPer1M - InputPer1M);
		}
		return Cost;
	}
};

/**
 * Manages the LLM pricing table loaded from a JSON config file.
 * File location: Resources/llm-pricing.json
 *
 * JSON format:
 * {
 *   "version_date": "2026-03-01",
 *   "models": {
 *     "claude-sonnet-4-6": { "provider": "anthropic", "input_per_1m": 3.0, "output_per_1m": 15.0, "cached_input_per_1m": 0.3 },
 *     ...
 *   }
 * }
 */
class UNREALCLAUDE_API FLLMPricingConfig
{
public:
	/** Load pricing from the plugin's Resources/llm-pricing.json */
	bool LoadFromFile(const FString& FilePath);

	/** Get pricing for a model. Returns nullptr if model not found. */
	const FLLMModelPricing* GetPricing(const FString& ModelId) const;

	/** Calculate cost for a specific model + token counts. Returns 0 if model not found. */
	float CalculateCost(const FString& ModelId, int32 InputTokens, int32 OutputTokens, int32 CachedInputTokens = 0) const;

	/** Get the version date of the loaded pricing data */
	const FString& GetVersionDate() const { return VersionDate; }

	/** Check if any pricing data is loaded */
	bool IsLoaded() const { return Models.Num() > 0; }

	/** Get all known model IDs */
	TArray<FString> GetAllModelIds() const;

private:
	/** Version date of pricing data (for staleness tracking) */
	FString VersionDate;

	/** Model ID -> pricing data */
	TMap<FString, FLLMModelPricing> Models;
};
