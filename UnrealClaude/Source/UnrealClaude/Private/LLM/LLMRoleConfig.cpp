// Copyright Natali Caggiano. All Rights Reserved.

#include "LLM/LLMRoleConfig.h"
#include "UnrealClaudeModule.h"
#include "Misc/ConfigCacheIni.h"

FLLMRoleManager::FLLMRoleManager()
{
	SetDefaults();
}

void FLLMRoleManager::SetDefaults()
{
	Assignments.Empty();

	{
		FModelRoleAssignment A;
		A.Role = EModelRole::Worker;
		A.ProviderId = TEXT("claude-code");
		A.ModelId = TEXT("claude-sonnet-4-6");
		Assignments.Add(EModelRole::Worker, A);
	}
	{
		FModelRoleAssignment A;
		A.Role = EModelRole::Critic;
		A.ProviderId = TEXT("anthropic-api");
		A.ModelId = TEXT("claude-sonnet-4-6");
		Assignments.Add(EModelRole::Critic, A);
	}
	{
		FModelRoleAssignment A;
		A.Role = EModelRole::Architect;
		A.ProviderId = TEXT("anthropic-api");
		A.ModelId = TEXT("claude-opus-4-6");
		Assignments.Add(EModelRole::Architect, A);
	}
	{
		FModelRoleAssignment A;
		A.Role = EModelRole::Escalation;
		A.ProviderId = TEXT("claude-code");
		A.ModelId = TEXT("claude-opus-4-6");
		Assignments.Add(EModelRole::Escalation, A);
	}
	{
		FModelRoleAssignment A;
		A.Role = EModelRole::DocsAgent;
		A.ProviderId = TEXT("anthropic-api");
		A.ModelId = TEXT("claude-sonnet-4-6");
		Assignments.Add(EModelRole::DocsAgent, A);
	}
}

FModelRoleAssignment FLLMRoleManager::GetAssignment(EModelRole Role) const
{
	const FModelRoleAssignment* Found = Assignments.Find(Role);
	if (Found)
	{
		return *Found;
	}

	// Fallback: return Worker defaults
	FModelRoleAssignment Fallback;
	Fallback.Role = Role;
	Fallback.ProviderId = TEXT("claude-code");
	Fallback.ModelId = TEXT("claude-sonnet-4-6");
	return Fallback;
}

void FLLMRoleManager::SetAssignment(EModelRole Role, const FString& ProviderId, const FString& ModelId)
{
	FModelRoleAssignment A;
	A.Role = Role;
	A.ProviderId = ProviderId;
	A.ModelId = ModelId;
	Assignments.Add(Role, A);
}

void FLLMRoleManager::SaveToConfig()
{
	const TCHAR* Section = GetConfigSection();

	for (const auto& Pair : Assignments)
	{
		FString RoleName = GetModelRoleDisplayName(Pair.Key);
		FString ProviderKey = FString::Printf(TEXT("%s.Provider"), *RoleName);
		FString ModelKey = FString::Printf(TEXT("%s.Model"), *RoleName);

		GConfig->SetString(Section, *ProviderKey, *Pair.Value.ProviderId, GEditorPerProjectIni);
		GConfig->SetString(Section, *ModelKey, *Pair.Value.ModelId, GEditorPerProjectIni);
	}

	GConfig->Flush(false, GEditorPerProjectIni);

	UE_LOG(LogUnrealClaude, Log, TEXT("FLLMRoleManager: Saved %d role assignments to config"), Assignments.Num());
}

void FLLMRoleManager::LoadFromConfig()
{
	const TCHAR* Section = GetConfigSection();

	for (EModelRole Role : GetAllModelRoles())
	{
		FString RoleName = GetModelRoleDisplayName(Role);
		FString ProviderKey = FString::Printf(TEXT("%s.Provider"), *RoleName);
		FString ModelKey = FString::Printf(TEXT("%s.Model"), *RoleName);

		FString ProviderId, ModelId;
		bool bHasProvider = GConfig->GetString(Section, *ProviderKey, ProviderId, GEditorPerProjectIni);
		bool bHasModel = GConfig->GetString(Section, *ModelKey, ModelId, GEditorPerProjectIni);

		if (bHasProvider && bHasModel && !ProviderId.IsEmpty() && !ModelId.IsEmpty())
		{
			SetAssignment(Role, ProviderId, ModelId);
		}
		// Otherwise keep the default
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("FLLMRoleManager: Loaded role assignments from config"));
}

void FLLMRoleManager::ResetToDefaults()
{
	SetDefaults();
	SaveToConfig();
}
