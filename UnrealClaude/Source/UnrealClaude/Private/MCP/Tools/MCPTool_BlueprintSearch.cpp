// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_BlueprintSearch.h"
#include "RAG/FBlueprintSearchIndex.h"
#include "UnrealClaudeModule.h"

FMCPToolResult FMCPTool_BlueprintSearch::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Query;
	if (!Params->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("'query' parameter is required and must be non-empty"));
	}

	int32 MaxResults = 8;
	double MaxResultsDouble = 8.0;
	if (Params->TryGetNumberField(TEXT("max_results"), MaxResultsDouble))
	{
		MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsDouble), 1, 50);
	}

	FBlueprintSearchIndex& Index = FBlueprintSearchIndex::Get();

	// Auto-build on first use if needed
	if (!Index.IsBuilt())
	{
		UE_LOG(LogUnrealClaude, Log, TEXT("BlueprintSearch: index not built — running RebuildAll now"));
		Index.RebuildAll();
	}

	if (Index.GetChunkCount() == 0)
	{
		return FMCPToolResult::Error(TEXT("Blueprint index is empty. No blueprints found under /Game/Game/. Check that RebuildAll completed successfully."));
	}

	TArray<FBlueprintChunk> Results = Index.Search(Query, MaxResults);

	if (Results.IsEmpty())
	{
		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("query"), Query);
		Response->SetNumberField(TEXT("total_chunks"), Index.GetChunkCount());
		Response->SetStringField(TEXT("message"), TEXT("No chunks matched the query. Try different keywords or more general terms."));
		return FMCPToolResult::Success(
			FString::Printf(TEXT("No results for '%s' (index has %d chunks)"), *Query, Index.GetChunkCount()),
			Response
		);
	}

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("query"), Query);
	Response->SetNumberField(TEXT("result_count"), Results.Num());
	Response->SetNumberField(TEXT("total_chunks"), Index.GetChunkCount());

	TArray<TSharedPtr<FJsonValue>> ResultsJson;
	for (const FBlueprintChunk& Chunk : Results)
	{
		TSharedPtr<FJsonObject> ChunkObj = MakeShared<FJsonObject>();
		ChunkObj->SetStringField(TEXT("bp_path"),    Chunk.BpPath);
		ChunkObj->SetStringField(TEXT("bp_name"),    Chunk.BpName);
		ChunkObj->SetStringField(TEXT("chunk_type"), Chunk.ChunkType);
		if (!Chunk.GraphName.IsEmpty())
		{
			ChunkObj->SetStringField(TEXT("graph_name"), Chunk.GraphName);
		}
		ChunkObj->SetStringField(TEXT("summary"), Chunk.Summary);
		ResultsJson.Add(MakeShared<FJsonValueObject>(ChunkObj));
	}
	Response->SetArrayField(TEXT("results"), ResultsJson);

	FString SummaryLine = FString::Printf(TEXT("Found %d chunks for '%s'"), Results.Num(), *Query);
	return FMCPToolResult::Success(SummaryLine, Response);
}
