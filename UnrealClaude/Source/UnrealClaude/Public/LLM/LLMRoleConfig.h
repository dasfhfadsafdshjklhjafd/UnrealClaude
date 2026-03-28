// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LLM/ILLMBackend.h"

/**
 * Assignment of a model role to a specific provider + model.
 */
struct UNREALCLAUDE_API FModelRoleAssignment
{
	EModelRole Role = EModelRole::Worker;
	FString ProviderId;  // "claude-code", "anthropic-api", "openai-api"
	FString ModelId;     // "claude-sonnet-4-6", "gpt-4o", etc.

	bool IsValid() const { return !ProviderId.IsEmpty() && !ModelId.IsEmpty(); }
};

/**
 * Manages per-role model assignments.
 *
 * Each role (Worker, Critic, Architect, Escalation) maps to a provider + model.
 * Assignments are persisted in the editor per-project INI.
 *
 * Defaults:
 *   Worker     -> claude-code / claude-sonnet-4-6
 *   Critic     -> anthropic-api / claude-sonnet-4-6
 *   Architect  -> anthropic-api / claude-opus-4-6
 *   Escalation -> claude-code / claude-opus-4-6
 */
class UNREALCLAUDE_API FLLMRoleManager
{
public:
	FLLMRoleManager();

	/** Get the assignment for a role */
	FModelRoleAssignment GetAssignment(EModelRole Role) const;

	/** Set the assignment for a role */
	void SetAssignment(EModelRole Role, const FString& ProviderId, const FString& ModelId);

	/** Get all assignments */
	const TMap<EModelRole, FModelRoleAssignment>& GetAllAssignments() const { return Assignments; }

	/** Save to editor preferences */
	void SaveToConfig();

	/** Load from editor preferences */
	void LoadFromConfig();

	/** Reset all roles to defaults */
	void ResetToDefaults();

private:
	/** Set up default assignments */
	void SetDefaults();

	/** Config section name */
	static const TCHAR* GetConfigSection() { return TEXT("UnrealClaude.Roles"); }

	TMap<EModelRole, FModelRoleAssignment> Assignments;
};
