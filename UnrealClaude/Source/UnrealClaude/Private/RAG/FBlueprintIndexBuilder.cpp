// Copyright Natali Caggiano. All Rights Reserved.

#include "RAG/FBlueprintIndexBuilder.h"
#include "BlueprintUtils.h"
#include "UnrealClaudeModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

// ============================================================================
// Public
// ============================================================================

TArray<FString> FBlueprintIndexBuilder::GetAllBlueprintPaths(const FString& SearchPath)
{
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*SearchPath));
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	TArray<FString> Paths;
	for (const FAssetData& Asset : AssetList)
	{
		Paths.Add(Asset.GetObjectPathString());
	}
	return Paths;
}

TArray<FBlueprintChunk> FBlueprintIndexBuilder::BuildChunksForBlueprint(const FString& BpPath)
{
	TArray<FBlueprintChunk> Out;

	FString LoadError;
	UBlueprint* BP = FBlueprintUtils::LoadBlueprint(BpPath, LoadError);
	if (!BP)
	{
		UE_LOG(LogUnrealClaude, Verbose, TEXT("BlueprintIndexBuilder: could not load '%s': %s"), *BpPath, *LoadError);
		return Out;
	}

	BuildMetadataChunk(BP, BpPath, Out);
	BuildGraphChunks(BP, BpPath, Out);
	return Out;
}

// ============================================================================
// Private — helpers
// ============================================================================

void FBlueprintIndexBuilder::TokenizeInto(const FString& Text, TArray<FString>& Keywords)
{
	FString Token;
	for (TCHAR C : Text)
	{
		if (FChar::IsAlpha(C) || FChar::IsDigit(C))
		{
			Token += FChar::ToLower(C);
		}
		else if (!Token.IsEmpty())
		{
			if (Token.Len() >= 2) Keywords.AddUnique(Token);
			Token.Empty();
		}
	}
	if (Token.Len() >= 2) Keywords.AddUnique(Token);
}

// ============================================================================
// Private — metadata
// ============================================================================

void FBlueprintIndexBuilder::BuildMetadataChunk(UBlueprint* BP, const FString& BpPath, TArray<FBlueprintChunk>& Out)
{
	FString BpName = BP->GetName();
	TArray<FString> Keywords;
	FString Summary;

	// Blueprint name itself
	Keywords.AddUnique(BpName.ToLower());
	// Strip common prefixes for better searchability
	FString StrippedName = BpName;
	for (const FString& Prefix : {FString(TEXT("BP_")), FString(TEXT("ABP_")), FString(TEXT("WBP_"))})
	{
		if (StrippedName.StartsWith(Prefix))
		{
			StrippedName = StrippedName.Mid(Prefix.Len());
			Keywords.AddUnique(StrippedName.ToLower());
			break;
		}
	}

	// Parent class
	FString ParentName;
	if (BP->ParentClass)
	{
		ParentName = BP->ParentClass->GetName();
		Keywords.AddUnique(ParentName.ToLower());
	}

	// Variables
	TArray<FString> VarLines;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		FString VarName = Var.VarName.ToString();
		FString VarType = Var.VarType.PinCategory.ToString();
		if (Var.VarType.PinSubCategoryObject.IsValid())
		{
			VarType += TEXT("(") + Var.VarType.PinSubCategoryObject->GetName() + TEXT(")");
		}
		VarLines.Add(FString::Printf(TEXT("%s:%s"), *VarName, *VarType));

		// Whole name + CamelCase tokens
		Keywords.AddUnique(VarName.ToLower());
		FString Token;
		for (TCHAR C : VarName)
		{
			if (FChar::IsUpper(C) && !Token.IsEmpty())
			{
				if (Token.Len() >= 2) Keywords.AddUnique(Token.ToLower());
				Token.Empty();
			}
			Token += C;
		}
		if (Token.Len() >= 2) Keywords.AddUnique(Token.ToLower());
	}

	// Functions
	TArray<FString> FuncNames;
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (G)
		{
			FString FuncName = G->GetName();
			FuncNames.Add(FuncName);
			Keywords.AddUnique(FuncName.ToLower());
			if (FuncName.StartsWith(TEXT("f_")))
			{
				Keywords.AddUnique(FuncName.Mid(2).ToLower());
			}
		}
	}

	// Event graphs
	for (UEdGraph* G : BP->EventGraphs)
	{
		if (G) Keywords.AddUnique(G->GetName().ToLower());
	}

	// Components from SimpleConstructionScript
	TArray<FString> CompNames;
	if (BP->SimpleConstructionScript)
	{
		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node)
			{
				FString CompName = Node->GetVariableName().ToString();
				if (!CompName.IsEmpty())
				{
					CompNames.Add(CompName);
					Keywords.AddUnique(CompName.ToLower());
				}
			}
		}
	}

	// Build summary
	Summary = FString::Printf(TEXT("Blueprint: %s"), *BpName);
	if (!ParentName.IsEmpty())
	{
		Summary += FString::Printf(TEXT(" (extends %s)"), *ParentName);
	}
	if (!VarLines.IsEmpty())
	{
		Summary += TEXT("\nVariables: ") + FString::Join(VarLines, TEXT(", "));
	}
	if (!FuncNames.IsEmpty())
	{
		Summary += TEXT("\nFunctions: ") + FString::Join(FuncNames, TEXT(", "));
	}
	if (!CompNames.IsEmpty())
	{
		Summary += TEXT("\nComponents: ") + FString::Join(CompNames, TEXT(", "));
	}

	FBlueprintChunk Chunk;
	Chunk.BpPath    = BpPath;
	Chunk.BpName    = BpName;
	Chunk.ChunkType = TEXT("metadata");
	Chunk.Summary   = Summary;
	Chunk.Keywords  = Keywords;
	Out.Add(MoveTemp(Chunk));
}

// ============================================================================
// Private — graph chunks
// ============================================================================

void FBlueprintIndexBuilder::BuildGraphChunks(UBlueprint* BP, const FString& BpPath, TArray<FBlueprintChunk>& Out)
{
	FString BpName = BP->GetName();

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	static const FName ExecCategory(TEXT("exec"));

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph || Graph->Nodes.IsEmpty()) continue;

		// Skip AnimBP graph types — their structure is navigated via blueprint_read_graph,
		// and fixed-window chunks of state machine / blend tree nodes are pure noise in search.
		const FName GraphClass = Graph->GetClass()->GetFName();
		if (GraphClass == TEXT("AnimationTransitionGraph") ||
			GraphClass == TEXT("AnimationConduitGraph") ||
			GraphClass == TEXT("AnimationGraph") ||
			GraphClass == TEXT("AnimationStateGraph"))
		{
			continue;
		}

		FString GraphName = Graph->GetName();

		// --- Collect entry nodes (event / custom event / function entry) ---
		TArray<UEdGraphNode*> EntryNodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			const FName NodeClass = Node->GetClass()->GetFName();
			if (NodeClass == TEXT("K2Node_Event") ||
				NodeClass == TEXT("K2Node_CustomEvent") ||
				NodeClass == TEXT("K2Node_FunctionEntry"))
			{
				EntryNodes.Add(Node);
			}
		}

		// No entry nodes — fall back to fixed-window chunking
		if (EntryNodes.IsEmpty())
		{
			BuildFixedWindowChunks(Graph, GraphName, BpName, BpPath, Out);
			continue;
		}

		// --- BFS from each entry node via exec output pins ---
		TSet<UEdGraphNode*> GlobalVisited;

		for (UEdGraphNode* EntryNode : EntryNodes)
		{
			if (GlobalVisited.Contains(EntryNode)) continue;

			FString EntryTitle = EntryNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			TArray<FString> TitleLines;
			TArray<FString> Keywords;

			Keywords.AddUnique(GraphName.ToLower());
			if (GraphName.StartsWith(TEXT("f_")))
			{
				Keywords.AddUnique(GraphName.Mid(2).ToLower());
			}

			// BFS
			TArray<UEdGraphNode*> ChunkNodes;
			TQueue<UEdGraphNode*> Queue;
			TSet<UEdGraphNode*> LocalVisited;

			Queue.Enqueue(EntryNode);
			LocalVisited.Add(EntryNode);

			while (!Queue.IsEmpty() && ChunkNodes.Num() < MaxChunkSize)
			{
				UEdGraphNode* Current;
				Queue.Dequeue(Current);

				// Skip reroute nodes
				if (Current->GetClass()->GetFName() == TEXT("K2Node_Knot")) continue;

				ChunkNodes.Add(Current);
				GlobalVisited.Add(Current);

				FString Title = Current->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				if (!Title.IsEmpty())
				{
					TitleLines.Add(Title);
					TokenizeInto(Title, Keywords);
				}
				if (!Current->NodeComment.IsEmpty())
				{
					TokenizeInto(Current->NodeComment, Keywords);
				}

				// Follow exec output pins to next nodes
				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output) continue;
					if (Pin->PinType.PinCategory != ExecCategory) continue;

					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (!LinkedPin) continue;
						UEdGraphNode* Next = LinkedPin->GetOwningNode();
						if (!Next || LocalVisited.Contains(Next)) continue;
						LocalVisited.Add(Next);
						Queue.Enqueue(Next);
					}
				}
			}

			if (ChunkNodes.IsEmpty()) continue;

			const bool bTruncated = ChunkNodes.Num() >= MaxChunkSize;
			FString Summary = FString::Printf(TEXT("Graph '%s' in %s — entry '%s' (%d nodes%s):\n%s"),
				*GraphName, *BpName, *EntryTitle, ChunkNodes.Num(),
				bTruncated ? TEXT(", truncated") : TEXT(""),
				*FString::Join(TitleLines, TEXT(", ")));

			// Store the entry node's index in the graph node array for blueprint_read_graph hints
			int32 EntryIndex = Graph->Nodes.IndexOfByKey(EntryNode);

			FBlueprintChunk Chunk;
			Chunk.BpPath    = BpPath;
			Chunk.BpName    = BpName;
			Chunk.ChunkType = TEXT("graph");
			Chunk.GraphName = GraphName;
			Chunk.StartNode = EntryIndex >= 0 ? EntryIndex : 0;
			Chunk.NodeCount = ChunkNodes.Num();
			Chunk.Summary   = Summary;
			Chunk.Keywords  = Keywords;
			Out.Add(MoveTemp(Chunk));
		}
	}
}

void FBlueprintIndexBuilder::BuildFixedWindowChunks(
	UEdGraph* Graph,
	const FString& GraphName,
	const FString& BpName,
	const FString& BpPath,
	TArray<FBlueprintChunk>& Out)
{
	for (int32 Start = 0; Start < Graph->Nodes.Num(); Start += WindowSize)
	{
		int32 End = FMath::Min(Start + WindowSize, Graph->Nodes.Num());
		int32 ActualCount = 0;
		TArray<FString> TitleLines;
		TArray<FString> Keywords;

		Keywords.AddUnique(GraphName.ToLower());
		if (GraphName.StartsWith(TEXT("f_")))
		{
			Keywords.AddUnique(GraphName.Mid(2).ToLower());
		}

		for (int32 i = Start; i < End; i++)
		{
			UEdGraphNode* Node = Graph->Nodes[i];
			if (!Node) continue;
			if (Node->GetClass()->GetFName() == TEXT("K2Node_Knot")) continue;

			FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			if (!Title.IsEmpty())
			{
				TitleLines.Add(Title);
				TokenizeInto(Title, Keywords);
			}
			if (!Node->NodeComment.IsEmpty())
			{
				TokenizeInto(Node->NodeComment, Keywords);
			}
			ActualCount++;
		}

		if (ActualCount == 0) continue;

		FString Summary = FString::Printf(TEXT("Graph '%s' in %s — nodes %d-%d:\n%s"),
			*GraphName, *BpName, Start, End - 1,
			*FString::Join(TitleLines, TEXT(", ")));

		FBlueprintChunk Chunk;
		Chunk.BpPath    = BpPath;
		Chunk.BpName    = BpName;
		Chunk.ChunkType = TEXT("graph");
		Chunk.GraphName = GraphName;
		Chunk.StartNode = Start;
		Chunk.NodeCount = ActualCount;
		Chunk.Summary   = Summary;
		Chunk.Keywords  = Keywords;
		Out.Add(MoveTemp(Chunk));
	}
}
