// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LLM/ILLMBackend.h"

/**
 * Registry of all available LLM backends.
 *
 * Owns backend instances and provides lookup by provider ID.
 * Backends are registered during subsystem initialization.
 */
class UNREALCLAUDE_API FLLMBackendRegistry
{
public:
	FLLMBackendRegistry() = default;
	~FLLMBackendRegistry() = default;

	// Non-copyable
	FLLMBackendRegistry(const FLLMBackendRegistry&) = delete;
	FLLMBackendRegistry& operator=(const FLLMBackendRegistry&) = delete;

	/** Register a backend. Registry takes ownership. */
	void RegisterBackend(TSharedPtr<ILLMBackend> Backend);

	/** Get a backend by provider ID. Returns nullptr if not found. */
	ILLMBackend* GetBackend(const FString& ProviderId) const;

	/** Get all registered backends */
	TArray<ILLMBackend*> GetAllBackends() const;

	/** Get all registered provider IDs */
	TArray<FString> GetProviderIds() const;

	/** Get only backends that are currently available (configured + ready) */
	TArray<ILLMBackend*> GetAvailableBackends() const;

	/** Check if a provider is registered */
	bool HasBackend(const FString& ProviderId) const;

	/** Store an API key for a provider. Persisted in editor per-project INI. */
	static void SaveAPIKey(const FString& ProviderId, const FString& Key);

	/** Load an API key for a provider. */
	static FString LoadAPIKey(const FString& ProviderId);

	/** Check if an API key exists for a provider */
	static bool HasAPIKey(const FString& ProviderId);

	/**
	 * Resolve which backend to use for a given model ID.
	 * - OpenAI models (gpt-*, o3, o4-*) → "openai-api"
	 * - Claude models → uses AnthropicMode preference ("claude-code" or "anthropic-api")
	 */
	FString ResolveBackendForModel(const FString& ModelId) const;

	/** Get/Set whether Claude models should use CLI or direct API */
	void SetAnthropicMode(const FString& ProviderId);  // "claude-code" or "anthropic-api"
	const FString& GetAnthropicMode() const { return AnthropicMode; }

	/** Get all models from all registered backends as a unified list */
	TArray<TPair<FString, FString>> GetAllModelsWithProviders() const;

private:
	/** Whether Claude models use CLI or API. Default: "claude-code" */
	FString AnthropicMode = TEXT("claude-code");
	/** Provider ID -> backend instance */
	TMap<FString, TSharedPtr<ILLMBackend>> Backends;
};
