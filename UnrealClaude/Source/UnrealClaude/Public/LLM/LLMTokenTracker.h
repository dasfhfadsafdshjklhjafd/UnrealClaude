// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LLM/ILLMBackend.h"

/**
 * Usage record for a single model+provider+role combination within a day.
 */
struct UNREALCLAUDE_API FLLMDailyUsageEntry
{
	int32 InputTokens = 0;
	int32 OutputTokens = 0;
	int32 CachedInputTokens = 0;
	float CostUsd = 0.0f;
	int32 RequestCount = 0;
};

/**
 * Full daily usage breakdown.
 */
struct UNREALCLAUDE_API FLLMDailyUsage
{
	/** Date string "YYYY-MM-DD" */
	FString Date;

	/** Model -> Provider -> Role -> usage entry */
	TMap<FString, TMap<FString, TMap<FString, FLLMDailyUsageEntry>>> Entries;

	/** Get aggregated totals for the day */
	FLLMDailyUsageEntry GetDayTotal() const;

	/** Get totals for a specific provider */
	FLLMDailyUsageEntry GetProviderTotal(const FString& ProviderId) const;
};

/**
 * Tracks LLM token usage and estimated costs, persisted to disk.
 *
 * Storage: Saved/UnrealClaude/token-usage.json
 *
 * Records every turn's token usage broken down by:
 *   - Date (daily aggregation)
 *   - Model ID
 *   - Provider ID
 *   - Role (Worker/Critic/Architect/Escalation)
 *
 * Usage is flushed to disk periodically and on shutdown.
 */
class UNREALCLAUDE_API FLLMTokenTracker
{
public:
	FLLMTokenTracker();

	/** Initialize and load existing data from disk */
	void Initialize(const FString& SaveDirectory);

	/** Record token usage from a completed turn */
	void RecordUsage(const FLLMTokenUsage& Usage);

	/** Get today's usage */
	FLLMDailyUsage GetTodayUsage() const;

	/** Get usage for a specific date ("YYYY-MM-DD") */
	FLLMDailyUsage GetUsageForDate(const FString& Date) const;

	/** Get session totals (since Initialize was called) */
	FLLMDailyUsageEntry GetSessionTotal() const { return SessionTotal; }

	/** Flush to disk */
	bool SaveToDisk();

	/** Get the save file path */
	const FString& GetSaveFilePath() const { return SaveFilePath; }

private:
	/** Get today's date as "YYYY-MM-DD" */
	static FString GetTodayDateString();

	/** Load existing usage data from file */
	bool LoadFromDisk();

	/** Ensure the day entry exists in the map */
	FLLMDailyUsage& EnsureDayEntry(const FString& Date);

	/** Path to token-usage.json */
	FString SaveFilePath;

	/** All tracked days */
	TMap<FString, FLLMDailyUsage> DailyData;

	/** Running session total (since Initialize) */
	FLLMDailyUsageEntry SessionTotal;

	/** Number of records since last flush */
	int32 RecordsSinceLastFlush = 0;

	/** Auto-flush after this many records */
	static constexpr int32 AutoFlushThreshold = 5;
};
