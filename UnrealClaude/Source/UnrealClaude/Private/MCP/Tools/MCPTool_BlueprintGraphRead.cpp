// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_BlueprintGraphRead.h"
#include "BlueprintUtils.h"
#include "MCP/MCPParamValidator.h"
#include "UnrealClaudeModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node.h"
#include "AnimGraphNode_Base.h"

FMCPToolResult FMCPTool_BlueprintGraphRead::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintPath(BlueprintPath, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FMCPToolResult::Error(LoadError);
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"));
	if (GraphName.IsEmpty())
	{
		return ExecuteListGraphs(Blueprint);
	}

	int32 MaxNodes = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("max_nodes"), 150), 1, 1000);
	int32 StartNode = FMath::Max(ExtractOptionalNumber<int32>(Params, TEXT("start_node"), 0), 0);
	return ExecuteReadGraph(Blueprint, GraphName, MaxNodes, StartNode);
}

static FString ClassifyGraph(const UEdGraph* Graph, const UBlueprint* BP)
{
	const FName CN = Graph->GetClass()->GetFName();
	if (CN == TEXT("AnimationGraph"))             return TEXT("anim");
	if (CN == TEXT("AnimationStateMachineGraph")) return TEXT("state_machine");
	if (CN == TEXT("AnimationTransitionGraph"))   return TEXT("transition");
	if (CN == TEXT("AnimationStateGraph"))        return TEXT("state");
	if (CN == TEXT("AnimationConduitGraph"))      return TEXT("conduit");
	for (UEdGraph* G : BP->EventGraphs)   if (G == Graph) return TEXT("event");
	for (UEdGraph* G : BP->MacroGraphs)   if (G == Graph) return TEXT("macro");
	for (UEdGraph* G : BP->FunctionGraphs) if (G == Graph) return TEXT("function");
	return TEXT("graph");
}

FMCPToolResult FMCPTool_BlueprintGraphRead::ExecuteListGraphs(UBlueprint* Blueprint)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("blueprint"), Blueprint->GetName());

	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		TSharedPtr<FJsonObject> G = MakeShared<FJsonObject>();
		G->SetStringField(TEXT("name"), Graph->GetName());
		G->SetStringField(TEXT("type"), ClassifyGraph(Graph, Blueprint));
		G->SetNumberField(TEXT("nodes"), Graph->Nodes.Num());
		GraphsArray.Add(MakeShared<FJsonValueObject>(G));
	}

	Response->SetArrayField(TEXT("graphs"), GraphsArray);
	Response->SetStringField(TEXT("hint"), TEXT("Pass graph_name to read a specific graph's nodes and connections"));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("%d graphs in %s — pass graph_name to read one"), GraphsArray.Num(), *Blueprint->GetName()),
		Response
	);
}

FMCPToolResult FMCPTool_BlueprintGraphRead::ExecuteReadGraph(
	UBlueprint* Blueprint,
	const FString& GraphName,
	int32 MaxNodes,
	int32 StartNode)
{
	FString GraphType;
	UEdGraph* Graph = FindGraphByName(Blueprint, GraphName, GraphType);
	if (!Graph)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Graph '%s' not found in '%s'. Call without graph_name to list available graphs."),
			*GraphName, *Blueprint->GetName()));
	}

	bool bTruncated = false;
	TSharedPtr<FJsonObject> GraphData = SerializeGraph(Graph, StartNode, MaxNodes, bTruncated);
	GraphData->SetStringField(TEXT("blueprint"), Blueprint->GetName());
	GraphData->SetStringField(TEXT("graph_type"), GraphType);

	if (bTruncated)
	{
		int32 NextStart = StartNode + MaxNodes;
		GraphData->SetStringField(TEXT("truncated_hint"),
			FString::Printf(TEXT("Use start_node=%d to read the next %d nodes"), NextStart, MaxNodes));
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Graph '%s' in '%s' (%s) — %d total nodes"),
			*GraphName, *Blueprint->GetName(), *GraphType, Graph->Nodes.Num()),
		GraphData
	);
}

UEdGraph* FMCPTool_BlueprintGraphRead::FindGraphByName(
	UBlueprint* Blueprint,
	const FString& GraphName,
	FString& OutType)
{
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* G : AllGraphs)
	{
		if (G && G->GetName() == GraphName)
		{
			OutType = ClassifyGraph(G, Blueprint);
			return G;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FMCPTool_BlueprintGraphRead::SerializeGraph(
	UEdGraph* Graph,
	int32 StartNode,
	int32 MaxNodes,
	bool& bOutTruncated)
{
	bOutTruncated = false;

	// Build index map across all nodes (indices are stable across paging calls)
	TMap<UEdGraphNode*, int32> IndexMap;
	for (int32 i = 0; i < Graph->Nodes.Num(); i++)
	{
		if (Graph->Nodes[i])
		{
			IndexMap.Add(Graph->Nodes[i], i);
		}
	}

	TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
	GraphObj->SetStringField(TEXT("graph"), Graph->GetName());
	GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
	GraphObj->SetNumberField(TEXT("start_node"), StartNode);

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	int32 OutputCount = 0;

	for (int32 i = StartNode; i < Graph->Nodes.Num(); i++)
	{
		UEdGraphNode* Node = Graph->Nodes[i];
		if (!Node) continue;

		if (OutputCount >= MaxNodes)
		{
			bOutTruncated = true;
			break;
		}

		TSharedPtr<FJsonObject> NodeObj = SerializeNode(Node, IndexMap);
		NodeObj->SetNumberField(TEXT("n"), i);
		NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		OutputCount++;
	}

	GraphObj->SetArrayField(TEXT("nodes"), NodesArray);
	return GraphObj;
}

TSharedPtr<FJsonObject> FMCPTool_BlueprintGraphRead::SerializeNode(
	UEdGraphNode* Node,
	const TMap<UEdGraphNode*, int32>& IndexMap)
{
	TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();

	// Node type (class name without module prefix noise)
	NodeObj->SetStringField(TEXT("type"), Node->GetClass()->GetFName().ToString());

	// Human-readable title
	FText Title = Node->GetNodeTitle(ENodeTitleType::FullTitle);
	NodeObj->SetStringField(TEXT("title"), Title.ToString());

	// Comment (often carries important designer intent)
	if (!Node->NodeComment.IsEmpty())
	{
		NodeObj->SetStringField(TEXT("comment"), Node->NodeComment);
	}

	// Pure flag (no exec pins, no side effects)
	if (UK2Node* K2Node = Cast<UK2Node>(Node))
	{
		if (K2Node->IsNodePure())
		{
			NodeObj->SetBoolField(TEXT("pure"), true);
		}
	}

	// Pins
	TArray<TSharedPtr<FJsonValue>> PinsArray;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;

		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("dir"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
		PinObj->SetStringField(TEXT("kind"), Pin->PinType.PinCategory.ToString());

		// Sub-type for object/struct/class pins (e.g. "BP_PlayerBase", "FHitResult")
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			PinObj->SetStringField(TEXT("sub"), Pin->PinType.PinSubCategoryObject->GetName());
		}

		// Container type
		if (Pin->PinType.ContainerType == EPinContainerType::Array)
			PinObj->SetStringField(TEXT("container"), TEXT("array"));
		else if (Pin->PinType.ContainerType == EPinContainerType::Set)
			PinObj->SetStringField(TEXT("container"), TEXT("set"));
		else if (Pin->PinType.ContainerType == EPinContainerType::Map)
			PinObj->SetStringField(TEXT("container"), TEXT("map"));

		// Default value (only for unconnected data pins — skip exec)
		if (Pin->LinkedTo.Num() == 0
			&& !Pin->DefaultValue.IsEmpty()
			&& Pin->PinType.PinCategory != TEXT("exec"))
		{
			PinObj->SetStringField(TEXT("default"), Pin->DefaultValue);
		}

		// Connections: [[targetNodeIndex, targetPinName], ...]
		if (Pin->LinkedTo.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> LinkedArray;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin) continue;
				UEdGraphNode* TargetNode = LinkedPin->GetOwningNodeUnchecked();
				if (!TargetNode) continue;
				const int32* TargetIdx = IndexMap.Find(TargetNode);
				if (!TargetIdx) continue;

				TArray<TSharedPtr<FJsonValue>> LinkPair;
				LinkPair.Add(MakeShared<FJsonValueNumber>(*TargetIdx));
				LinkPair.Add(MakeShared<FJsonValueString>(LinkedPin->PinName.ToString()));
				LinkedArray.Add(MakeShared<FJsonValueArray>(LinkPair));
			}
			if (LinkedArray.Num() > 0)
			{
				PinObj->SetArrayField(TEXT("linked"), LinkedArray);
			}
		}

		PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
	}

	NodeObj->SetArrayField(TEXT("pins"), PinsArray);

	// AnimGraph PropertyAccess bindings — show what drives each pin (e.g. "CharacterMovement.Velocity.X")
	if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
	{
		TSharedPtr<FJsonObject> BindingsObj = MakeShared<FJsonObject>();

		// Legacy (pre-UE5.1) bindings
		for (const TPair<FName, FAnimGraphNodePropertyBinding>& Pair : AnimNode->PropertyBindings_DEPRECATED)
		{
			if (!Pair.Value.PathAsText.IsEmpty())
			{
				BindingsObj->SetStringField(Pair.Key.ToString(), Pair.Value.PathAsText.ToString());
			}
		}

		// Current binding system — get the Binding subobject as UObject* via reflection
		// (avoids including the internal AnimGraphNodeBinding.h header)
		UObject* BindingUObj = nullptr;
		if (FObjectProperty* BindingProp = CastField<FObjectProperty>(
			UAnimGraphNode_Base::StaticClass()->FindPropertyByName(TEXT("Binding"))))
		{
			BindingUObj = BindingProp->GetObjectPropertyValue_InContainer(AnimNode);
		}
		if (BindingUObj)
		{
			if (FProperty* Prop = BindingUObj->GetClass()->FindPropertyByName(TEXT("PropertyBindings")))
			{
				if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
				{
					FScriptMapHelper MapHelper(MapProp, MapProp->ContainerPtrToValuePtr<void>(BindingUObj));
					for (int32 MapIdx = 0; MapIdx < MapHelper.GetMaxIndex(); ++MapIdx)
					{
						if (!MapHelper.IsValidIndex(MapIdx)) continue;
						const FName* Key = reinterpret_cast<const FName*>(MapHelper.GetKeyPtr(MapIdx));
						const FAnimGraphNodePropertyBinding* Val = reinterpret_cast<const FAnimGraphNodePropertyBinding*>(MapHelper.GetValuePtr(MapIdx));
						if (Key && Val && !Val->PathAsText.IsEmpty())
						{
							BindingsObj->SetStringField(Key->ToString(), Val->PathAsText.ToString());
						}
					}
				}
			}
		}

		if (BindingsObj->Values.Num() > 0)
		{
			NodeObj->SetObjectField(TEXT("property_access"), BindingsObj);
		}
	}

	return NodeObj;
}
