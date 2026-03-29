// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RAG/FBlueprintSearchIndex.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

/**
 * Builds FBlueprintChunk arrays from UBlueprint objects.
 * Produces two chunk types per blueprint:
 *   - One "metadata" chunk: variable names/types, function names, component names, parent class
 *   - N "graph" chunks: one per event/custom-event/function-entry execution path (BFS via exec pins)
 *
 * Graphs with no detectable entry nodes fall back to fixed-window chunking.
 */
class FBlueprintIndexBuilder
{
public:
	/** Load the blueprint at BpPath and build all its chunks. Returns empty on failure. */
	static TArray<FBlueprintChunk> BuildChunksForBlueprint(const FString& BpPath);

	/** Return all blueprint asset paths under SearchPath (via AssetRegistry, no loading). */
	static TArray<FString> GetAllBlueprintPaths(const FString& SearchPath);

private:
	static void BuildMetadataChunk(UBlueprint* BP, const FString& BpPath, TArray<FBlueprintChunk>& Out);
	static void BuildGraphChunks(UBlueprint* BP, const FString& BpPath, TArray<FBlueprintChunk>& Out);

	/** Fallback: fixed-window chunks when no entry nodes are found in a graph. */
	static void BuildFixedWindowChunks(
		UEdGraph* Graph,
		const FString& GraphName,
		const FString& BpName,
		const FString& BpPath,
		TArray<FBlueprintChunk>& Out);

	/** Append lowercase alpha-numeric tokens (>= 2 chars) from Text into Keywords. */
	static void TokenizeInto(const FString& Text, TArray<FString>& Keywords);

	/** Max nodes per entry-path chunk (BFS stops here; larger events get one big chunk). */
	static constexpr int32 MaxChunkSize = 60;

	/** Window size for fixed-window fallback. */
	static constexpr int32 WindowSize = 20;
};
