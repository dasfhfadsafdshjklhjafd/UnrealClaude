// Copyright Natali Caggiano. All Rights Reserved.

#include "RAG/FBlueprintSearchIndex.h"
#include "RAG/FBlueprintIndexBuilder.h"
#include "UnrealClaudeModule.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ============================================================================

FBlueprintSearchIndex& FBlueprintSearchIndex::Get()
{
	static FBlueprintSearchIndex Instance;
	return Instance;
}

void FBlueprintSearchIndex::Initialize()
{
	if (bInitialized) return;
	bInitialized = true;

	IndexFilePath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UnrealClaude"),
		TEXT("BlueprintSearchIndex.json"));

	LoadFromDisk();
}

void FBlueprintSearchIndex::RebuildAll(const FString& SearchPath)
{
	UE_LOG(LogUnrealClaude, Log, TEXT("BlueprintSearchIndex: rebuilding from %s"), *SearchPath);

	Chunks.Empty();
	InvertedIndex.Empty();

	TArray<FString> Paths = FBlueprintIndexBuilder::GetAllBlueprintPaths(SearchPath);
	for (const FString& Path : Paths)
	{
		TArray<FBlueprintChunk> New = FBlueprintIndexBuilder::BuildChunksForBlueprint(Path);
		Chunks.Append(MoveTemp(New));
	}

	RebuildInvertedIndex();
	bBuilt = true;
	SaveToDisk();

	UE_LOG(LogUnrealClaude, Log, TEXT("BlueprintSearchIndex: %d chunks from %d blueprints"),
		Chunks.Num(), Paths.Num());
}

void FBlueprintSearchIndex::IndexBlueprint(const FString& BpPath)
{
	TArray<FBlueprintChunk> NewChunks = FBlueprintIndexBuilder::BuildChunksForBlueprint(BpPath);
	SetBlueprintChunks(BpPath, MoveTemp(NewChunks));
	SaveToDisk();
}

void FBlueprintSearchIndex::RemoveBlueprint(const FString& BpPath)
{
	SetBlueprintChunks(BpPath, TArray<FBlueprintChunk>());
	SaveToDisk();
}

void FBlueprintSearchIndex::SetBlueprintChunks(const FString& BpPath, TArray<FBlueprintChunk>&& NewChunks)
{
	// Remove existing chunks for this blueprint
	Chunks.RemoveAll([&BpPath](const FBlueprintChunk& C) { return C.BpPath == BpPath; });

	// Add new chunks
	Chunks.Append(MoveTemp(NewChunks));

	RebuildInvertedIndex();
	bBuilt = true;
}

void FBlueprintSearchIndex::RebuildInvertedIndex()
{
	InvertedIndex.Empty();
	for (int32 i = 0; i < Chunks.Num(); i++)
	{
		for (const FString& Keyword : Chunks[i].Keywords)
		{
			InvertedIndex.FindOrAdd(Keyword).Add(i);
		}
	}
}

TArray<FBlueprintChunk> FBlueprintSearchIndex::Search(const FString& Query, int32 MaxResults) const
{
	TArray<FString> QueryTokens = Tokenize(Query);
	if (QueryTokens.IsEmpty())
	{
		return {};
	}

	TMap<int32, int32> ChunkScores; // chunk index → number of matching tokens

	for (const FString& Token : QueryTokens)
	{
		if (const TArray<int32>* Indices = InvertedIndex.Find(Token))
		{
			for (int32 Idx : *Indices)
			{
				ChunkScores.FindOrAdd(Idx)++;
			}
		}
	}

	// Sort by score descending
	TArray<TPair<int32, int32>> Sorted; // (score, chunkIdx)
	for (const auto& Pair : ChunkScores)
	{
		Sorted.Add({Pair.Value, Pair.Key});
	}
	Sorted.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B)
	{
		return A.Key > B.Key;
	});

	TArray<FBlueprintChunk> Results;
	for (int32 i = 0; i < FMath::Min(MaxResults, Sorted.Num()); i++)
	{
		Results.Add(Chunks[Sorted[i].Value]);
	}
	return Results;
}

TArray<FString> FBlueprintSearchIndex::Tokenize(const FString& Text)
{
	TArray<FString> Tokens;
	FString Current;
	for (TCHAR C : Text)
	{
		if (FChar::IsAlpha(C) || FChar::IsDigit(C))
		{
			Current += FChar::ToLower(C);
		}
		else if (!Current.IsEmpty())
		{
			if (Current.Len() >= 2)
			{
				Tokens.AddUnique(Current);
			}
			Current.Empty();
		}
	}
	if (Current.Len() >= 2)
	{
		Tokens.AddUnique(Current);
	}
	return Tokens;
}

// ============================================================================
// Persistence
// ============================================================================

void FBlueprintSearchIndex::SaveToDisk()
{
	if (IndexFilePath.IsEmpty()) return;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);
	Root->SetNumberField(TEXT("chunk_count"), Chunks.Num());

	TArray<TSharedPtr<FJsonValue>> ChunksJson;
	for (const FBlueprintChunk& C : Chunks)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("bp_path"),    C.BpPath);
		Obj->SetStringField(TEXT("bp_name"),    C.BpName);
		Obj->SetStringField(TEXT("chunk_type"), C.ChunkType);
		Obj->SetStringField(TEXT("graph_name"), C.GraphName);
		Obj->SetNumberField(TEXT("start_node"), C.StartNode);
		Obj->SetNumberField(TEXT("node_count"), C.NodeCount);
		Obj->SetStringField(TEXT("summary"),    C.Summary);

		TArray<TSharedPtr<FJsonValue>> KwJson;
		for (const FString& Kw : C.Keywords)
		{
			KwJson.Add(MakeShared<FJsonValueString>(Kw));
		}
		Obj->SetArrayField(TEXT("keywords"), KwJson);
		ChunksJson.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Root->SetArrayField(TEXT("chunks"), ChunksJson);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(IndexFilePath), true);
	FFileHelper::SaveStringToFile(Output, *IndexFilePath);
}

void FBlueprintSearchIndex::LoadFromDisk()
{
	if (IndexFilePath.IsEmpty() || !FPaths::FileExists(IndexFilePath)) return;

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *IndexFilePath)) return;

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return;

	const TArray<TSharedPtr<FJsonValue>>* ChunksJson = nullptr;
	if (!Root->TryGetArrayField(TEXT("chunks"), ChunksJson)) return;

	Chunks.Empty();
	for (const TSharedPtr<FJsonValue>& Val : *ChunksJson)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Val->TryGetObject(Obj)) continue;

		FBlueprintChunk C;
		(*Obj)->TryGetStringField(TEXT("bp_path"),    C.BpPath);
		(*Obj)->TryGetStringField(TEXT("bp_name"),    C.BpName);
		(*Obj)->TryGetStringField(TEXT("chunk_type"), C.ChunkType);
		(*Obj)->TryGetStringField(TEXT("graph_name"), C.GraphName);
		(*Obj)->TryGetNumberField(TEXT("start_node"), C.StartNode);
		(*Obj)->TryGetNumberField(TEXT("node_count"), C.NodeCount);
		(*Obj)->TryGetStringField(TEXT("summary"),    C.Summary);

		const TArray<TSharedPtr<FJsonValue>>* KwJson = nullptr;
		if ((*Obj)->TryGetArrayField(TEXT("keywords"), KwJson))
		{
			for (const TSharedPtr<FJsonValue>& Kw : *KwJson)
			{
				FString KwStr;
				if (Kw->TryGetString(KwStr))
				{
					C.Keywords.Add(KwStr);
				}
			}
		}
		Chunks.Add(MoveTemp(C));
	}

	RebuildInvertedIndex();
	bBuilt = true;
	UE_LOG(LogUnrealClaude, Log, TEXT("BlueprintSearchIndex: loaded %d chunks from disk"), Chunks.Num());
}
