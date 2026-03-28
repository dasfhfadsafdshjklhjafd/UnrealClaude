// Copyright Natali Caggiano. All Rights Reserved.

#include "LLM/LLMBackendRegistry.h"
#include "UnrealClaudeModule.h"
#include "Misc/ConfigCacheIni.h"

static const TCHAR* APIKeyConfigSection = TEXT("UnrealClaude.APIKeys");

void FLLMBackendRegistry::RegisterBackend(TSharedPtr<ILLMBackend> Backend)
{
	if (!Backend.IsValid())
	{
		return;
	}

	// On first registration, load saved Anthropic mode
	if (Backends.Num() == 0)
	{
		FString SavedMode;
		if (GConfig->GetString(APIKeyConfigSection, TEXT("AnthropicMode"), SavedMode, GEditorPerProjectIni)
			&& !SavedMode.IsEmpty())
		{
			AnthropicMode = SavedMode;
		}
	}

	FString ProviderId = Backend->GetProviderId();
	if (Backends.Contains(ProviderId))
	{
		UE_LOG(LogUnrealClaude, Warning,
			TEXT("FLLMBackendRegistry: Replacing existing backend for provider '%s'"), *ProviderId);
	}

	UE_LOG(LogUnrealClaude, Log,
		TEXT("FLLMBackendRegistry: Registered backend '%s' (provider: %s, available: %s)"),
		*Backend->GetBackendName(), *ProviderId,
		Backend->IsAvailable() ? TEXT("yes") : TEXT("no"));

	Backends.Add(ProviderId, MoveTemp(Backend));
}

ILLMBackend* FLLMBackendRegistry::GetBackend(const FString& ProviderId) const
{
	const TSharedPtr<ILLMBackend>* Found = Backends.Find(ProviderId);
	if (Found && Found->IsValid())
	{
		return Found->Get();
	}
	return nullptr;
}

TArray<ILLMBackend*> FLLMBackendRegistry::GetAllBackends() const
{
	TArray<ILLMBackend*> Result;
	for (const auto& Pair : Backends)
	{
		if (Pair.Value.IsValid())
		{
			Result.Add(Pair.Value.Get());
		}
	}
	return Result;
}

TArray<FString> FLLMBackendRegistry::GetProviderIds() const
{
	TArray<FString> Result;
	Backends.GetKeys(Result);
	return Result;
}

TArray<ILLMBackend*> FLLMBackendRegistry::GetAvailableBackends() const
{
	TArray<ILLMBackend*> Result;
	for (const auto& Pair : Backends)
	{
		if (Pair.Value.IsValid() && Pair.Value->IsAvailable())
		{
			Result.Add(Pair.Value.Get());
		}
	}
	return Result;
}

bool FLLMBackendRegistry::HasBackend(const FString& ProviderId) const
{
	return Backends.Contains(ProviderId);
}

// ============================================================================
// API Key Storage
// ============================================================================

void FLLMBackendRegistry::SaveAPIKey(const FString& ProviderId, const FString& Key)
{
	GConfig->SetString(APIKeyConfigSection, *ProviderId, *Key, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);

	UE_LOG(LogUnrealClaude, Log, TEXT("FLLMBackendRegistry: Saved API key for provider '%s'"), *ProviderId);
}

FString FLLMBackendRegistry::LoadAPIKey(const FString& ProviderId)
{
	// Prefer environment variables (most secure — never touches disk)
	FString EnvVarName;
	if (ProviderId == TEXT("anthropic-api"))
	{
		EnvVarName = TEXT("ANTHROPIC_API_KEY");
	}
	else if (ProviderId == TEXT("openai-api"))
	{
		EnvVarName = TEXT("OPENAI_API_KEY");
	}

	if (!EnvVarName.IsEmpty())
	{
		FString EnvKey = FPlatformMisc::GetEnvironmentVariable(*EnvVarName);
		if (!EnvKey.IsEmpty())
		{
			return EnvKey;
		}
	}

	// Fall back to INI storage
	FString Key;
	GConfig->GetString(APIKeyConfigSection, *ProviderId, Key, GEditorPerProjectIni);
	return Key;
}

bool FLLMBackendRegistry::HasAPIKey(const FString& ProviderId)
{
	return !LoadAPIKey(ProviderId).IsEmpty();
}

// ============================================================================
// Model-to-Backend Resolution
// ============================================================================

static bool IsOpenAIModel(const FString& ModelId)
{
	return ModelId.StartsWith(TEXT("gpt-"))
		|| ModelId.StartsWith(TEXT("o3"))
		|| ModelId.StartsWith(TEXT("o4"));
}

FString FLLMBackendRegistry::ResolveBackendForModel(const FString& ModelId) const
{
	if (IsOpenAIModel(ModelId))
	{
		return TEXT("openai-api");
	}
	// Claude models — use the Anthropic mode preference
	return AnthropicMode;
}

void FLLMBackendRegistry::SetAnthropicMode(const FString& ProviderId)
{
	if (ProviderId == TEXT("claude-code") || ProviderId == TEXT("anthropic-api"))
	{
		AnthropicMode = ProviderId;

		GConfig->SetString(APIKeyConfigSection, TEXT("AnthropicMode"), *AnthropicMode, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);

		UE_LOG(LogUnrealClaude, Log, TEXT("FLLMBackendRegistry: Anthropic mode set to '%s'"), *AnthropicMode);
	}
}

TArray<TPair<FString, FString>> FLLMBackendRegistry::GetAllModelsWithProviders() const
{
	TArray<TPair<FString, FString>> Result;

	for (const auto& Pair : Backends)
	{
		if (!Pair.Value.IsValid()) continue;

		// Skip "anthropic-api" if mode is CLI (avoid duplicate Claude models)
		if (Pair.Key == TEXT("anthropic-api") && AnthropicMode == TEXT("claude-code")) continue;
		// Skip "claude-code" if mode is API
		if (Pair.Key == TEXT("claude-code") && AnthropicMode == TEXT("anthropic-api")) continue;

		for (const FString& Model : Pair.Value->GetSupportedModels())
		{
			Result.Add(TPair<FString, FString>(Model, Pair.Key));
		}
	}

	return Result;
}
