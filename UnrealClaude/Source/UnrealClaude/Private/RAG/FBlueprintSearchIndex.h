// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A single indexed chunk from a Blueprint.
 * Either a metadata entry (variables/functions/components) or a window of graph nodes.
 */
struct FBlueprintChunk
{
	FString BpPath;      // Full asset path: "/Game/Game/Characters/Player/Blueprints/BP_PlayerBase"
	FString BpName;      // Short name: "BP_PlayerBase"
	FString ChunkType;   // "metadata" or "graph"
	FString GraphName;   // Graph name if chunk_type=="graph", else empty
	int32   StartNode = 0;
	int32   NodeCount = 0;
	FString Summary;     // Human-readable text returned to the LLM
	TArray<FString> Keywords; // Lowercase tokens used for the inverted index
};

/**
 * In-memory inverted-index search over Blueprint chunks.
 *
 * Lifecycle:
 *   Initialize()   — set file path, load from disk if present
 *   RebuildAll()   — scan all BPs under /Game/Game/ and build fresh index
 *   IndexBlueprint()  — incremental update for one BP (called on save)
 *   Search(query)  — BM25-style keyword search, returns top N chunks
 *   SaveToDisk()   — persist to Saved/UnrealClaude/BlueprintSearchIndex.json
 */
class FBlueprintSearchIndex
{
public:
	static FBlueprintSearchIndex& Get();

	/** Set file path and try to load existing index from disk. */
	void Initialize();

	/** Rebuild entire index from /Game/Game/ (or a custom path). Expensive but async-safe on game thread. */
	void RebuildAll(const FString& SearchPath = TEXT("/Game/Game/"));

	/** Incrementally re-index one blueprint (e.g. after a save). */
	void IndexBlueprint(const FString& BpPath);

	/** Remove all chunks for a blueprint (e.g. on asset delete). */
	void RemoveBlueprint(const FString& BpPath);

	/** Keyword search. Returns up to MaxResults chunks sorted by match score. */
	TArray<FBlueprintChunk> Search(const FString& Query, int32 MaxResults = 8) const;

	int32 GetChunkCount() const { return Chunks.Num(); }
	bool  IsBuilt()       const { return bBuilt; }

	void SaveToDisk();
	void LoadFromDisk();

private:
	/** Replace all existing chunks for BpPath with NewChunks, then rebuild index. */
	void SetBlueprintChunks(const FString& BpPath, TArray<FBlueprintChunk>&& NewChunks);

	/** Rebuild InvertedIndex from scratch after Chunks changes. */
	void RebuildInvertedIndex();

	/** Split text into lowercase alpha-numeric tokens (≥2 chars). */
	static TArray<FString> Tokenize(const FString& Text);

	TArray<FBlueprintChunk>        Chunks;
	TMap<FString, TArray<int32>>   InvertedIndex; // keyword → chunk indices
	FString                        IndexFilePath;
	bool                           bBuilt = false;
};
