// Copyright Natali Caggiano. All Rights Reserved.

#include "LLM/LLMTokenTracker.h"
#include "UnrealClaudeModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"

// ============================================================================
// FLLMDailyUsage
// ============================================================================

FLLMDailyUsageEntry FLLMDailyUsage::GetDayTotal() const
{
	FLLMDailyUsageEntry Total;
	for (const auto& ModelPair : Entries)
	{
		for (const auto& ProviderPair : ModelPair.Value)
		{
			for (const auto& RolePair : ProviderPair.Value)
			{
				Total.InputTokens += RolePair.Value.InputTokens;
				Total.OutputTokens += RolePair.Value.OutputTokens;
				Total.CachedInputTokens += RolePair.Value.CachedInputTokens;
				Total.CostUsd += RolePair.Value.CostUsd;
				Total.RequestCount += RolePair.Value.RequestCount;
			}
		}
	}
	return Total;
}

FLLMDailyUsageEntry FLLMDailyUsage::GetProviderTotal(const FString& ProviderId) const
{
	FLLMDailyUsageEntry Total;
	for (const auto& ModelPair : Entries)
	{
		const auto* ProviderMap = ModelPair.Value.Find(ProviderId);
		if (ProviderMap)
		{
			for (const auto& RolePair : *ProviderMap)
			{
				Total.InputTokens += RolePair.Value.InputTokens;
				Total.OutputTokens += RolePair.Value.OutputTokens;
				Total.CachedInputTokens += RolePair.Value.CachedInputTokens;
				Total.CostUsd += RolePair.Value.CostUsd;
				Total.RequestCount += RolePair.Value.RequestCount;
			}
		}
	}
	return Total;
}

// ============================================================================
// FLLMTokenTracker
// ============================================================================

FLLMTokenTracker::FLLMTokenTracker()
{
}

void FLLMTokenTracker::Initialize(const FString& SaveDirectory)
{
	if (!IFileManager::Get().DirectoryExists(*SaveDirectory))
	{
		IFileManager::Get().MakeDirectory(*SaveDirectory, true);
	}

	SaveFilePath = FPaths::Combine(SaveDirectory, TEXT("token-usage.json"));
	LoadFromDisk();

	UE_LOG(LogUnrealClaude, Log, TEXT("FLLMTokenTracker: Initialized. %d days of history loaded from: %s"),
		DailyData.Num(), *SaveFilePath);
}

void FLLMTokenTracker::RecordUsage(const FLLMTokenUsage& Usage)
{
	FString Today = GetTodayDateString();
	FLLMDailyUsage& Day = EnsureDayEntry(Today);

	FString RoleName = GetModelRoleDisplayName(Usage.Role).ToLower();

	// Navigate to the right entry: Model -> Provider -> Role
	auto& ProviderMap = Day.Entries.FindOrAdd(Usage.ModelId);
	auto& RoleMap = ProviderMap.FindOrAdd(Usage.ProviderId);
	FLLMDailyUsageEntry& Entry = RoleMap.FindOrAdd(RoleName);

	Entry.InputTokens += Usage.InputTokens;
	Entry.OutputTokens += Usage.OutputTokens;
	Entry.CachedInputTokens += Usage.CachedInputTokens;
	Entry.CostUsd += Usage.EstimatedCostUsd;
	Entry.RequestCount++;

	// Update session total
	SessionTotal.InputTokens += Usage.InputTokens;
	SessionTotal.OutputTokens += Usage.OutputTokens;
	SessionTotal.CachedInputTokens += Usage.CachedInputTokens;
	SessionTotal.CostUsd += Usage.EstimatedCostUsd;
	SessionTotal.RequestCount++;

	// Auto-flush periodically
	RecordsSinceLastFlush++;
	if (RecordsSinceLastFlush >= AutoFlushThreshold)
	{
		SaveToDisk();
		RecordsSinceLastFlush = 0;
	}
}

FLLMDailyUsage FLLMTokenTracker::GetTodayUsage() const
{
	return GetUsageForDate(GetTodayDateString());
}

FLLMDailyUsage FLLMTokenTracker::GetUsageForDate(const FString& Date) const
{
	const FLLMDailyUsage* Found = DailyData.Find(Date);
	if (Found)
	{
		return *Found;
	}

	FLLMDailyUsage Empty;
	Empty.Date = Date;
	return Empty;
}

FString FLLMTokenTracker::GetTodayDateString()
{
	return FDateTime::UtcNow().ToString(TEXT("%Y-%m-%d"));
}

FLLMDailyUsage& FLLMTokenTracker::EnsureDayEntry(const FString& Date)
{
	FLLMDailyUsage* Existing = DailyData.Find(Date);
	if (Existing)
	{
		return *Existing;
	}

	FLLMDailyUsage& New = DailyData.Add(Date);
	New.Date = Date;
	return New;
}

// ============================================================================
// Persistence
// ============================================================================

bool FLLMTokenTracker::SaveToDisk()
{
	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetNumberField(TEXT("version"), 1);

	TSharedPtr<FJsonObject> DaysObject = MakeShared<FJsonObject>();

	for (const auto& DayPair : DailyData)
	{
		const FLLMDailyUsage& Day = DayPair.Value;
		TSharedPtr<FJsonObject> DayObject = MakeShared<FJsonObject>();

		for (const auto& ModelPair : Day.Entries)
		{
			TSharedPtr<FJsonObject> ModelObject = MakeShared<FJsonObject>();

			for (const auto& ProviderPair : ModelPair.Value)
			{
				TSharedPtr<FJsonObject> ProviderObject = MakeShared<FJsonObject>();

				for (const auto& RolePair : ProviderPair.Value)
				{
					TSharedPtr<FJsonObject> EntryObject = MakeShared<FJsonObject>();
					const FLLMDailyUsageEntry& E = RolePair.Value;

					EntryObject->SetNumberField(TEXT("input"), E.InputTokens);
					EntryObject->SetNumberField(TEXT("output"), E.OutputTokens);
					EntryObject->SetNumberField(TEXT("cached_input"), E.CachedInputTokens);
					EntryObject->SetNumberField(TEXT("cost_usd"), E.CostUsd);
					EntryObject->SetNumberField(TEXT("requests"), E.RequestCount);

					ProviderObject->SetObjectField(RolePair.Key, EntryObject);
				}

				ModelObject->SetObjectField(ProviderPair.Key, ProviderObject);
			}

			DayObject->SetObjectField(ModelPair.Key, ModelObject);
		}

		DaysObject->SetObjectField(DayPair.Key, DayObject);
	}

	RootObject->SetObjectField(TEXT("days"), DaysObject);

	// Serialize
	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	if (!FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer))
	{
		UE_LOG(LogUnrealClaude, Error, TEXT("FLLMTokenTracker: Failed to serialize usage JSON"));
		return false;
	}

	if (!FFileHelper::SaveStringToFile(JsonString, *SaveFilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogUnrealClaude, Error, TEXT("FLLMTokenTracker: Failed to save to: %s"), *SaveFilePath);
		return false;
	}

	return true;
}

bool FLLMTokenTracker::LoadFromDisk()
{
	if (!IFileManager::Get().FileExists(*SaveFilePath))
	{
		return false;
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *SaveFilePath))
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("FLLMTokenTracker: Could not read: %s"), *SaveFilePath);
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("FLLMTokenTracker: Failed to parse usage JSON"));
		return false;
	}

	const TSharedPtr<FJsonObject>* DaysObject = nullptr;
	if (!RootObject->TryGetObjectField(TEXT("days"), DaysObject) || !DaysObject)
	{
		return false;
	}

	DailyData.Empty();

	for (const auto& DayPair : (*DaysObject)->Values)
	{
		const FString& DateStr = DayPair.Key;
		const TSharedPtr<FJsonObject>* DayObject = nullptr;
		if (!DayPair.Value->TryGetObject(DayObject) || !DayObject)
		{
			continue;
		}

		FLLMDailyUsage& Day = EnsureDayEntry(DateStr);

		for (const auto& ModelPair : (*DayObject)->Values)
		{
			const TSharedPtr<FJsonObject>* ModelObject = nullptr;
			if (!ModelPair.Value->TryGetObject(ModelObject) || !ModelObject)
			{
				continue;
			}

			for (const auto& ProviderPair : (*ModelObject)->Values)
			{
				const TSharedPtr<FJsonObject>* ProviderObject = nullptr;
				if (!ProviderPair.Value->TryGetObject(ProviderObject) || !ProviderObject)
				{
					continue;
				}

				for (const auto& RolePair : (*ProviderObject)->Values)
				{
					const TSharedPtr<FJsonObject>* EntryObject = nullptr;
					if (!RolePair.Value->TryGetObject(EntryObject) || !EntryObject)
					{
						continue;
					}

					FLLMDailyUsageEntry Entry;
					(*EntryObject)->TryGetNumberField(TEXT("input"), Entry.InputTokens);
					(*EntryObject)->TryGetNumberField(TEXT("output"), Entry.OutputTokens);
					(*EntryObject)->TryGetNumberField(TEXT("cached_input"), Entry.CachedInputTokens);
					(*EntryObject)->TryGetNumberField(TEXT("cost_usd"), Entry.CostUsd);
					(*EntryObject)->TryGetNumberField(TEXT("requests"), Entry.RequestCount);

					Day.Entries.FindOrAdd(ModelPair.Key)
						.FindOrAdd(ProviderPair.Key)
						.Add(RolePair.Key, Entry);
				}
			}
		}
	}

	return true;
}
