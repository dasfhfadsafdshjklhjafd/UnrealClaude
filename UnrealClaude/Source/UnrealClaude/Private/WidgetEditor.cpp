// Copyright Natali Caggiano. All Rights Reserved.

#include "WidgetEditor.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Border.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Spacer.h"
#include "Components/Button.h"
#include "Components/ScaleBox.h"
#include "Components/GridPanel.h"
#include "Components/WrapBox.h"
#include "Components/PanelWidget.h"
#include "EditorAssetLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "WidgetBlueprintFactory.h"

// ============================================================================
// Private Helpers
// ============================================================================

TSharedPtr<FJsonObject> FWidgetEditor::SuccessResult(const FString& Message)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("message"), Message);
	return Result;
}

TSharedPtr<FJsonObject> FWidgetEditor::ErrorResult(const FString& Message)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false);
	Result->SetStringField(TEXT("error"), Message);
	return Result;
}

// ============================================================================
// Widget Class Resolution
// ============================================================================

UClass* FWidgetEditor::ResolveWidgetClass(const FString& ShortName)
{
	static TMap<FString, UClass*> ClassMap;
	if (ClassMap.Num() == 0)
	{
		ClassMap.Add(TEXT("CanvasPanel"), UCanvasPanel::StaticClass());
		ClassMap.Add(TEXT("Overlay"), UOverlay::StaticClass());
		ClassMap.Add(TEXT("VerticalBox"), UVerticalBox::StaticClass());
		ClassMap.Add(TEXT("HorizontalBox"), UHorizontalBox::StaticClass());
		ClassMap.Add(TEXT("SizeBox"), USizeBox::StaticClass());
		ClassMap.Add(TEXT("Border"), UBorder::StaticClass());
		ClassMap.Add(TEXT("ProgressBar"), UProgressBar::StaticClass());
		ClassMap.Add(TEXT("TextBlock"), UTextBlock::StaticClass());
		ClassMap.Add(TEXT("Image"), UImage::StaticClass());
		ClassMap.Add(TEXT("Spacer"), USpacer::StaticClass());
		ClassMap.Add(TEXT("Button"), UButton::StaticClass());
		ClassMap.Add(TEXT("ScaleBox"), UScaleBox::StaticClass());
		ClassMap.Add(TEXT("GridPanel"), UGridPanel::StaticClass());
		ClassMap.Add(TEXT("WrapBox"), UWrapBox::StaticClass());
	}

	if (UClass** Found = ClassMap.Find(ShortName))
		return *Found;

	return FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/UMG.%s"), *ShortName));
}

// ============================================================================
// Color Parsing
// ============================================================================

FLinearColor FWidgetEditor::ParseColor(const FString& ColorStr)
{
	FString Str = ColorStr.TrimStartAndEnd();

	if (Str.Equals(TEXT("red"),         ESearchCase::IgnoreCase)) return FLinearColor::Red;
	if (Str.Equals(TEXT("green"),       ESearchCase::IgnoreCase)) return FLinearColor::Green;
	if (Str.Equals(TEXT("blue"),        ESearchCase::IgnoreCase)) return FLinearColor::Blue;
	if (Str.Equals(TEXT("white"),       ESearchCase::IgnoreCase)) return FLinearColor::White;
	if (Str.Equals(TEXT("black"),       ESearchCase::IgnoreCase)) return FLinearColor::Black;
	if (Str.Equals(TEXT("yellow"),      ESearchCase::IgnoreCase)) return FLinearColor::Yellow;
	if (Str.Equals(TEXT("gray"),        ESearchCase::IgnoreCase) ||
	    Str.Equals(TEXT("grey"),        ESearchCase::IgnoreCase)) return FLinearColor::Gray;
	if (Str.Equals(TEXT("transparent"), ESearchCase::IgnoreCase)) return FLinearColor::Transparent;

	if (Str.StartsWith(TEXT("#"))) Str = Str.Mid(1);

	FColor ParsedColor = FColor::White;
	if      (Str.Len() == 6) ParsedColor = FColor::FromHex(Str + TEXT("FF"));
	else if (Str.Len() == 8) ParsedColor = FColor::FromHex(Str);

	return ParsedColor.ReinterpretAsLinear();
}

// ============================================================================
// Asset Loading
// ============================================================================

UWidgetBlueprint* FWidgetEditor::LoadWidgetBlueprint(const FString& Path, FString& OutError)
{
	UObject* Loaded = StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *Path);
	if (!Loaded)
	{
		OutError = FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *Path);
		return nullptr;
	}
	UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
	if (!WBP)
	{
		OutError = FString::Printf(TEXT("Asset is not a Widget Blueprint: %s (is %s)"),
			*Path, *Loaded->GetClass()->GetName());
		return nullptr;
	}
	return WBP;
}

// ============================================================================
// Serialization Helpers
// ============================================================================

static FString GetSlotTypeName(UPanelSlot* Slot)
{
	if (!Slot) return TEXT("None");
	if (Cast<UCanvasPanelSlot>(Slot))    return TEXT("CanvasPanelSlot");
	if (Cast<UOverlaySlot>(Slot))        return TEXT("OverlaySlot");
	if (Cast<UVerticalBoxSlot>(Slot))    return TEXT("VerticalBoxSlot");
	if (Cast<UHorizontalBoxSlot>(Slot))  return TEXT("HorizontalBoxSlot");
	return Slot->GetClass()->GetName();
}

static FString AlignToString(EHorizontalAlignment A)
{
	switch (A) { case HAlign_Left: return TEXT("Left"); case HAlign_Center: return TEXT("Center");
	             case HAlign_Right: return TEXT("Right"); default: return TEXT("Fill"); }
}
static FString VAlignToString(EVerticalAlignment A)
{
	switch (A) { case VAlign_Top: return TEXT("Top"); case VAlign_Center: return TEXT("Center");
	             case VAlign_Bottom: return TEXT("Bottom"); default: return TEXT("Fill"); }
}
static EHorizontalAlignment StringToHAlign(const FString& S)
{
	if (S.Equals(TEXT("Left"),   ESearchCase::IgnoreCase)) return HAlign_Left;
	if (S.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return HAlign_Center;
	if (S.Equals(TEXT("Right"),  ESearchCase::IgnoreCase)) return HAlign_Right;
	return HAlign_Fill;
}
static EVerticalAlignment StringToVAlign(const FString& S)
{
	if (S.Equals(TEXT("Top"),    ESearchCase::IgnoreCase)) return VAlign_Top;
	if (S.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return VAlign_Center;
	if (S.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase)) return VAlign_Bottom;
	return VAlign_Fill;
}

static TSharedPtr<FJsonObject> ColorToJson(const FLinearColor& C)
{
	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("r"), C.R);
	O->SetNumberField(TEXT("g"), C.G);
	O->SetNumberField(TEXT("b"), C.B);
	O->SetNumberField(TEXT("a"), C.A);
	FColor SRGB = C.ToFColor(true);
	O->SetStringField(TEXT("hex"), FString::Printf(TEXT("#%02X%02X%02X%02X"), SRGB.R, SRGB.G, SRGB.B, SRGB.A));
	return O;
}

TSharedPtr<FJsonObject> FWidgetEditor::SerializeSlotProperties(UWidget* Widget)
{
	TSharedPtr<FJsonObject> SlotJson = MakeShared<FJsonObject>();
	UPanelSlot* Slot = Widget->Slot;
	if (!Slot) return SlotJson;

	if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Slot))
	{
		FVector2D Pos = CS->GetPosition(); FVector2D Sz = CS->GetSize();
		FAnchors An = CS->GetAnchors(); FVector2D Al = CS->GetAlignment();

		auto V2 = [](float X, float Y) {
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("x"), X); O->SetNumberField(TEXT("y"), Y); return O;
		};
		SlotJson->SetObjectField(TEXT("position"), V2(Pos.X, Pos.Y));
		SlotJson->SetObjectField(TEXT("size"),     V2(Sz.X,  Sz.Y));
		TSharedPtr<FJsonObject> AnchorObj = MakeShared<FJsonObject>();
		AnchorObj->SetNumberField(TEXT("min_x"), An.Minimum.X); AnchorObj->SetNumberField(TEXT("min_y"), An.Minimum.Y);
		AnchorObj->SetNumberField(TEXT("max_x"), An.Maximum.X); AnchorObj->SetNumberField(TEXT("max_y"), An.Maximum.Y);
		SlotJson->SetObjectField(TEXT("anchors"), AnchorObj);
		SlotJson->SetObjectField(TEXT("alignment"), V2(Al.X, Al.Y));
		SlotJson->SetBoolField(TEXT("auto_size"), CS->GetAutoSize());
		SlotJson->SetNumberField(TEXT("z_order"), CS->GetZOrder());
	}
	else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
	{
		SlotJson->SetStringField(TEXT("h_align"), AlignToString(OS->GetHorizontalAlignment()));
		SlotJson->SetStringField(TEXT("v_align"), VAlignToString(OS->GetVerticalAlignment()));
		FMargin P = OS->GetPadding();
		TSharedPtr<FJsonObject> PO = MakeShared<FJsonObject>();
		PO->SetNumberField(TEXT("left"), P.Left); PO->SetNumberField(TEXT("top"), P.Top);
		PO->SetNumberField(TEXT("right"), P.Right); PO->SetNumberField(TEXT("bottom"), P.Bottom);
		SlotJson->SetObjectField(TEXT("padding"), PO);
	}
	else if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
	{
		SlotJson->SetStringField(TEXT("h_align"), AlignToString(VS->GetHorizontalAlignment()));
		SlotJson->SetStringField(TEXT("v_align"), VAlignToString(VS->GetVerticalAlignment()));
		FSlateChildSize SR = VS->GetSize();
		SlotJson->SetStringField(TEXT("size_rule"), SR.SizeRule == ESlateSizeRule::Automatic ? TEXT("Auto") : TEXT("Fill"));
		SlotJson->SetNumberField(TEXT("fill_weight"), SR.Value);
		FMargin P = VS->GetPadding();
		TSharedPtr<FJsonObject> PO = MakeShared<FJsonObject>();
		PO->SetNumberField(TEXT("left"), P.Left); PO->SetNumberField(TEXT("top"), P.Top);
		PO->SetNumberField(TEXT("right"), P.Right); PO->SetNumberField(TEXT("bottom"), P.Bottom);
		SlotJson->SetObjectField(TEXT("padding"), PO);
	}
	else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
	{
		SlotJson->SetStringField(TEXT("h_align"), AlignToString(HS->GetHorizontalAlignment()));
		SlotJson->SetStringField(TEXT("v_align"), VAlignToString(HS->GetVerticalAlignment()));
		FSlateChildSize SR = HS->GetSize();
		SlotJson->SetStringField(TEXT("size_rule"), SR.SizeRule == ESlateSizeRule::Automatic ? TEXT("Auto") : TEXT("Fill"));
		SlotJson->SetNumberField(TEXT("fill_weight"), SR.Value);
		FMargin P = HS->GetPadding();
		TSharedPtr<FJsonObject> PO = MakeShared<FJsonObject>();
		PO->SetNumberField(TEXT("left"), P.Left); PO->SetNumberField(TEXT("top"), P.Top);
		PO->SetNumberField(TEXT("right"), P.Right); PO->SetNumberField(TEXT("bottom"), P.Bottom);
		SlotJson->SetObjectField(TEXT("padding"), PO);
	}

	return SlotJson;
}

TSharedPtr<FJsonObject> FWidgetEditor::SerializeWidgetProperties(UWidget* Widget)
{
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	if (!Widget) return Props;

	Props->SetStringField(TEXT("visibility"), StaticEnum<ESlateVisibility>()->GetNameStringByValue((int64)Widget->GetVisibility()));
	Props->SetNumberField(TEXT("render_opacity"), Widget->GetRenderOpacity());
	Props->SetBoolField(TEXT("is_enabled"), Widget->GetIsEnabled());

	if (UProgressBar* PB = Cast<UProgressBar>(Widget))
	{
		Props->SetNumberField(TEXT("percent"), PB->GetPercent());
		Props->SetObjectField(TEXT("fill_color"), ColorToJson(PB->GetFillColorAndOpacity()));
		Props->SetStringField(TEXT("bar_fill_type"), StaticEnum<EProgressBarFillType::Type>()->GetNameStringByValue((int64)PB->GetBarFillType()));
		const FProgressBarStyle& Style = PB->GetWidgetStyle();
		Props->SetObjectField(TEXT("style_fill_tint"), ColorToJson(Style.FillImage.TintColor.GetSpecifiedColor()));
		Props->SetObjectField(TEXT("style_background_tint"), ColorToJson(Style.BackgroundImage.TintColor.GetSpecifiedColor()));
	}
	else if (UTextBlock* TB = Cast<UTextBlock>(Widget))
	{
		Props->SetStringField(TEXT("text"), TB->GetText().ToString());
		Props->SetObjectField(TEXT("color"), ColorToJson(TB->GetColorAndOpacity().GetSpecifiedColor()));
		Props->SetNumberField(TEXT("font_size"), TB->GetFont().Size);
	}
	else if (UImage* Img = Cast<UImage>(Widget))
	{
		Props->SetObjectField(TEXT("color_and_opacity"), ColorToJson(Img->GetColorAndOpacity()));
	}
	else if (USizeBox* SB = Cast<USizeBox>(Widget))
	{
		Props->SetNumberField(TEXT("width_override"),      SB->GetWidthOverride());
		Props->SetNumberField(TEXT("height_override"),     SB->GetHeightOverride());
		Props->SetNumberField(TEXT("min_desired_width"),   SB->GetMinDesiredWidth());
		Props->SetNumberField(TEXT("min_desired_height"),  SB->GetMinDesiredHeight());
		Props->SetNumberField(TEXT("max_desired_width"),   SB->GetMaxDesiredWidth());
		Props->SetNumberField(TEXT("max_desired_height"),  SB->GetMaxDesiredHeight());
	}

	return Props;
}

TSharedPtr<FJsonObject> FWidgetEditor::SerializeWidgetTree(UWidget* Widget, UWidgetBlueprint* WBP)
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	if (!Widget) return Node;

	Node->SetStringField(TEXT("name"),  Widget->GetName());
	Node->SetStringField(TEXT("class"), Widget->GetClass()->GetName());

	if (Widget->Slot)
	{
		Node->SetStringField(TEXT("slot_type"), GetSlotTypeName(Widget->Slot));
		Node->SetObjectField(TEXT("slot_properties"), SerializeSlotProperties(Widget));
	}

	Node->SetObjectField(TEXT("visual_properties"), SerializeWidgetProperties(Widget));

	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		TArray<TSharedPtr<FJsonValue>> Children;
		for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
		{
			if (UWidget* Child = Panel->GetChildAt(i))
				Children.Add(MakeShared<FJsonValueObject>(SerializeWidgetTree(Child, WBP)));
		}
		Node->SetArrayField(TEXT("children"), Children);
	}

	return Node;
}

// ============================================================================
// InspectWidgetTree
// ============================================================================

TSharedPtr<FJsonObject> FWidgetEditor::InspectWidgetTree(const FString& AssetPath)
{
	FString LoadError;
	UWidgetBlueprint* WBP = LoadWidgetBlueprint(AssetPath, LoadError);
	if (!WBP) return ErrorResult(LoadError);

	UWidgetTree* WidgetTree = WBP->WidgetTree;
	if (!WidgetTree) return ErrorResult(TEXT("Widget Blueprint has no WidgetTree"));

	TSharedPtr<FJsonObject> Result = SuccessResult(TEXT("Widget tree inspected"));
	Result->SetStringField(TEXT("asset_path"),      AssetPath);
	Result->SetStringField(TEXT("blueprint_name"),  WBP->GetName());

	UWidget* Root = WidgetTree->RootWidget;
	if (Root)
		Result->SetObjectField(TEXT("widget_tree"), SerializeWidgetTree(Root, WBP));
	else
		Result->SetStringField(TEXT("widget_tree"), TEXT("empty"));

	TArray<UWidget*> AllWidgets;
	WidgetTree->GetAllWidgets(AllWidgets);
	Result->SetNumberField(TEXT("total_widgets"), AllWidgets.Num());

	return Result;
}

// ============================================================================
// GetWidgetProperties
// ============================================================================

TSharedPtr<FJsonObject> FWidgetEditor::GetWidgetProperties(const FString& AssetPath, const FString& WidgetName)
{
	FString LoadError;
	UWidgetBlueprint* WBP = LoadWidgetBlueprint(AssetPath, LoadError);
	if (!WBP) return ErrorResult(LoadError);

	UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget)
		return ErrorResult(FString::Printf(TEXT("Widget not found: %s"), *WidgetName));

	TSharedPtr<FJsonObject> Result = SuccessResult(
		FString::Printf(TEXT("Properties for %s (%s)"), *WidgetName, *Widget->GetClass()->GetName()));
	Result->SetStringField(TEXT("widget_name"),  WidgetName);
	Result->SetStringField(TEXT("widget_class"), Widget->GetClass()->GetName());
	Result->SetObjectField(TEXT("visual_properties"), SerializeWidgetProperties(Widget));

	if (Widget->Slot)
	{
		Result->SetStringField(TEXT("slot_type"), GetSlotTypeName(Widget->Slot));
		Result->SetObjectField(TEXT("slot_properties"), SerializeSlotProperties(Widget));
	}

	return Result;
}

// ============================================================================
// CreateWidgetBlueprint
// ============================================================================

TSharedPtr<FJsonObject> FWidgetEditor::CreateWidgetBlueprint(const FString& Name, const FString& PackagePath,
	const FString& ParentClass, const FString& RootWidgetClass)
{
	UClass* ParentUClass = UUserWidget::StaticClass();
	if (!ParentClass.IsEmpty() && !ParentClass.Equals(TEXT("UserWidget"), ESearchCase::IgnoreCase))
	{
		ParentUClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/UMG.%s"), *ParentClass));
		if (!ParentUClass) ParentUClass = LoadClass<UUserWidget>(nullptr, *ParentClass);
		if (!ParentUClass)
			return ErrorResult(FString::Printf(TEXT("Could not find parent class: %s"), *ParentClass));
	}

	UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
	Factory->ParentClass = ParentUClass;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* NewAsset = AssetTools.CreateAsset(Name, PackagePath, UWidgetBlueprint::StaticClass(), Factory);
	if (!NewAsset)
		return ErrorResult(FString::Printf(TEXT("Failed to create Widget Blueprint: %s/%s"), *PackagePath, *Name));

	UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(NewAsset);
	if (!WBP)
		return ErrorResult(TEXT("Created asset is not a Widget Blueprint"));

	if (!RootWidgetClass.IsEmpty() && !RootWidgetClass.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		if (UClass* RootClass = ResolveWidgetClass(RootWidgetClass))
		{
			if (UWidget* Root = WBP->WidgetTree->ConstructWidget<UWidget>(RootClass, FName(TEXT("RootPanel"))))
				WBP->WidgetTree->RootWidget = Root;
		}
	}

	WBP->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

	TSharedPtr<FJsonObject> Result = SuccessResult(FString::Printf(TEXT("Created Widget Blueprint: %s"), *WBP->GetPathName()));
	Result->SetStringField(TEXT("path"), WBP->GetPathName());
	return Result;
}

// ============================================================================
// AddWidget
// ============================================================================

TSharedPtr<FJsonObject> FWidgetEditor::AddWidget(UWidgetBlueprint* WBP, const FString& WidgetClass,
	const FString& WidgetName, const FString& ParentName)
{
	if (!WBP || !WBP->WidgetTree)
		return ErrorResult(TEXT("Invalid Widget Blueprint or missing WidgetTree"));

	UClass* WidgetUClass = ResolveWidgetClass(WidgetClass);
	if (!WidgetUClass)
		return ErrorResult(FString::Printf(TEXT("Unknown widget class: %s"), *WidgetClass));

	if (WBP->WidgetTree->FindWidget(FName(*WidgetName)))
		return ErrorResult(FString::Printf(TEXT("Widget with name '%s' already exists"), *WidgetName));

	UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetUClass, FName(*WidgetName));
	if (!NewWidget)
		return ErrorResult(FString::Printf(TEXT("Failed to construct widget of class %s"), *WidgetClass));

	UPanelWidget* Parent = nullptr;
	if (ParentName.IsEmpty() || ParentName.Equals(TEXT("root"), ESearchCase::IgnoreCase))
	{
		Parent = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
	}
	else
	{
		UWidget* ParentWidget = WBP->WidgetTree->FindWidget(FName(*ParentName));
		if (!ParentWidget)
		{
			WBP->WidgetTree->RemoveWidget(NewWidget);
			return ErrorResult(FString::Printf(TEXT("Parent widget not found: %s"), *ParentName));
		}
		Parent = Cast<UPanelWidget>(ParentWidget);
		if (!Parent)
		{
			WBP->WidgetTree->RemoveWidget(NewWidget);
			return ErrorResult(FString::Printf(TEXT("Parent '%s' is not a panel widget"), *ParentName));
		}
	}

	if (!Parent)
	{
		if (!WBP->WidgetTree->RootWidget)
		{
			WBP->WidgetTree->RootWidget = NewWidget;
			WBP->MarkPackageDirty();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
			TSharedPtr<FJsonObject> R = SuccessResult(FString::Printf(TEXT("Set %s as root widget"), *WidgetName));
			R->SetStringField(TEXT("widget_name"), WidgetName);
			R->SetStringField(TEXT("widget_class"), WidgetClass);
			return R;
		}
		WBP->WidgetTree->RemoveWidget(NewWidget);
		return ErrorResult(TEXT("Root widget is not a panel — cannot add children"));
	}

	UPanelSlot* Slot = Parent->AddChild(NewWidget);
	if (!Slot)
	{
		WBP->WidgetTree->RemoveWidget(NewWidget);
		return ErrorResult(TEXT("AddChild returned null slot"));
	}

	WBP->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

	TSharedPtr<FJsonObject> Result = SuccessResult(
		FString::Printf(TEXT("Added %s '%s' to %s"), *WidgetClass, *WidgetName, *Parent->GetName()));
	Result->SetStringField(TEXT("widget_name"),  WidgetName);
	Result->SetStringField(TEXT("widget_class"), WidgetClass);
	Result->SetStringField(TEXT("parent_name"),  Parent->GetName());
	Result->SetStringField(TEXT("slot_type"),    GetSlotTypeName(Slot));
	return Result;
}

// ============================================================================
// RemoveWidget
// ============================================================================

TSharedPtr<FJsonObject> FWidgetEditor::RemoveWidget(UWidgetBlueprint* WBP, const FString& WidgetName)
{
	if (!WBP || !WBP->WidgetTree)
		return ErrorResult(TEXT("Invalid Widget Blueprint"));

	UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget)
		return ErrorResult(FString::Printf(TEXT("Widget not found: %s"), *WidgetName));

	FString WidgetClass = Widget->GetClass()->GetName();
	if (!WBP->WidgetTree->RemoveWidget(Widget))
		return ErrorResult(FString::Printf(TEXT("Failed to remove widget: %s"), *WidgetName));

	WBP->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

	return SuccessResult(FString::Printf(TEXT("Removed widget %s (%s)"), *WidgetName, *WidgetClass));
}

// ============================================================================
// SetWidgetProperty
// ============================================================================

TSharedPtr<FJsonObject> FWidgetEditor::SetWidgetProperty(UWidgetBlueprint* WBP, const FString& WidgetName,
	const TSharedPtr<FJsonObject>& Properties)
{
	if (!WBP || !WBP->WidgetTree)
		return ErrorResult(TEXT("Invalid Widget Blueprint"));

	UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget)
		return ErrorResult(FString::Printf(TEXT("Widget not found: %s"), *WidgetName));

	TArray<FString> SetProperties;
	FString StrVal; double NumVal; bool BoolVal;

	if (Properties->TryGetNumberField(TEXT("render_opacity"), NumVal))
	{
		Widget->SetRenderOpacity(static_cast<float>(NumVal));
		SetProperties.Add(TEXT("render_opacity"));
	}
	if (Properties->TryGetBoolField(TEXT("is_enabled"), BoolVal))
	{
		Widget->SetIsEnabled(BoolVal);
		SetProperties.Add(TEXT("is_enabled"));
	}
	if (Properties->TryGetStringField(TEXT("visibility"), StrVal))
	{
		int64 EnumVal = StaticEnum<ESlateVisibility>()->GetValueByNameString(StrVal);
		if (EnumVal != INDEX_NONE) { Widget->SetVisibility((ESlateVisibility)EnumVal); SetProperties.Add(TEXT("visibility")); }
	}

	if (UProgressBar* PB = Cast<UProgressBar>(Widget))
	{
		if (Properties->TryGetNumberField(TEXT("percent"), NumVal)) { PB->SetPercent((float)NumVal); SetProperties.Add(TEXT("percent")); }
		if (Properties->TryGetStringField(TEXT("fill_color"), StrVal)) { PB->SetFillColorAndOpacity(ParseColor(StrVal)); SetProperties.Add(TEXT("fill_color")); }
	}
	else if (UTextBlock* TB = Cast<UTextBlock>(Widget))
	{
		if (Properties->TryGetStringField(TEXT("text"), StrVal)) { TB->SetText(FText::FromString(StrVal)); SetProperties.Add(TEXT("text")); }
		if (Properties->TryGetStringField(TEXT("color"), StrVal)) { TB->SetColorAndOpacity(FSlateColor(ParseColor(StrVal))); SetProperties.Add(TEXT("color")); }
		if (Properties->TryGetNumberField(TEXT("font_size"), NumVal))
		{
			FSlateFontInfo FontInfo = TB->GetFont();
			FontInfo.Size = (int32)NumVal;
			TB->SetFont(FontInfo);
			SetProperties.Add(TEXT("font_size"));
		}
	}
	else if (UImage* Img = Cast<UImage>(Widget))
	{
		if (Properties->TryGetStringField(TEXT("color_and_opacity"), StrVal)) { Img->SetColorAndOpacity(ParseColor(StrVal)); SetProperties.Add(TEXT("color_and_opacity")); }
	}
	else if (USizeBox* SB = Cast<USizeBox>(Widget))
	{
		if (Properties->TryGetNumberField(TEXT("width_override"),     NumVal)) { SB->SetWidthOverride((float)NumVal);     SetProperties.Add(TEXT("width_override")); }
		if (Properties->TryGetNumberField(TEXT("height_override"),    NumVal)) { SB->SetHeightOverride((float)NumVal);    SetProperties.Add(TEXT("height_override")); }
		if (Properties->TryGetNumberField(TEXT("min_desired_width"),  NumVal)) { SB->SetMinDesiredWidth((float)NumVal);   SetProperties.Add(TEXT("min_desired_width")); }
		if (Properties->TryGetNumberField(TEXT("min_desired_height"), NumVal)) { SB->SetMinDesiredHeight((float)NumVal);  SetProperties.Add(TEXT("min_desired_height")); }
		if (Properties->TryGetNumberField(TEXT("max_desired_width"),  NumVal)) { SB->SetMaxDesiredWidth((float)NumVal);   SetProperties.Add(TEXT("max_desired_width")); }
		if (Properties->TryGetNumberField(TEXT("max_desired_height"), NumVal)) { SB->SetMaxDesiredHeight((float)NumVal);  SetProperties.Add(TEXT("max_desired_height")); }
	}

	if (SetProperties.Num() == 0)
		return ErrorResult(TEXT("No recognized properties were set. Check property names and widget type."));

	WBP->MarkPackageDirty();

	return SuccessResult(FString::Printf(TEXT("Set %d properties on %s: %s"),
		SetProperties.Num(), *WidgetName, *FString::Join(SetProperties, TEXT(", "))));
}

// ============================================================================
// SetSlotProperty
// ============================================================================

static FMargin ExtractPadding(const TSharedPtr<FJsonObject>& PadObj)
{
	FMargin Pad(0); double Val;
	if (PadObj->TryGetNumberField(TEXT("left"),   Val)) Pad.Left   = (float)Val;
	if (PadObj->TryGetNumberField(TEXT("top"),    Val)) Pad.Top    = (float)Val;
	if (PadObj->TryGetNumberField(TEXT("right"),  Val)) Pad.Right  = (float)Val;
	if (PadObj->TryGetNumberField(TEXT("bottom"), Val)) Pad.Bottom = (float)Val;
	return Pad;
}

TSharedPtr<FJsonObject> FWidgetEditor::SetSlotProperty(UWidgetBlueprint* WBP, const FString& WidgetName,
	const TSharedPtr<FJsonObject>& SlotProperties)
{
	if (!WBP || !WBP->WidgetTree)
		return ErrorResult(TEXT("Invalid Widget Blueprint"));

	UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget)
		return ErrorResult(FString::Printf(TEXT("Widget not found: %s"), *WidgetName));

	UPanelSlot* Slot = Widget->Slot;
	if (!Slot)
		return ErrorResult(FString::Printf(TEXT("Widget '%s' has no slot"), *WidgetName));

	TArray<FString> SetProps; FString StrVal; double NumVal; bool BoolVal;

	if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Slot))
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (SlotProperties->TryGetObjectField(TEXT("position"), Obj))
		{
			FVector2D Pos = CS->GetPosition(); double X, Y;
			if ((*Obj)->TryGetNumberField(TEXT("x"), X)) Pos.X = (float)X;
			if ((*Obj)->TryGetNumberField(TEXT("y"), Y)) Pos.Y = (float)Y;
			CS->SetPosition(Pos); SetProps.Add(TEXT("position"));
		}
		if (SlotProperties->TryGetObjectField(TEXT("size"), Obj))
		{
			FVector2D Sz = CS->GetSize(); double X, Y;
			if ((*Obj)->TryGetNumberField(TEXT("x"), X)) Sz.X = (float)X;
			if ((*Obj)->TryGetNumberField(TEXT("y"), Y)) Sz.Y = (float)Y;
			CS->SetSize(Sz); SetProps.Add(TEXT("size"));
		}
		if (SlotProperties->TryGetObjectField(TEXT("anchors"), Obj))
		{
			FAnchors An = CS->GetAnchors(); double Val;
			if ((*Obj)->TryGetNumberField(TEXT("min_x"), Val)) An.Minimum.X = (float)Val;
			if ((*Obj)->TryGetNumberField(TEXT("min_y"), Val)) An.Minimum.Y = (float)Val;
			if ((*Obj)->TryGetNumberField(TEXT("max_x"), Val)) An.Maximum.X = (float)Val;
			if ((*Obj)->TryGetNumberField(TEXT("max_y"), Val)) An.Maximum.Y = (float)Val;
			CS->SetAnchors(An); SetProps.Add(TEXT("anchors"));
		}
		if (SlotProperties->TryGetBoolField(TEXT("auto_size"), BoolVal)) { CS->SetAutoSize(BoolVal); SetProps.Add(TEXT("auto_size")); }
		if (SlotProperties->TryGetNumberField(TEXT("z_order"), NumVal))  { CS->SetZOrder((int32)NumVal); SetProps.Add(TEXT("z_order")); }
	}
	else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
	{
		if (SlotProperties->TryGetStringField(TEXT("h_align"), StrVal)) { OS->SetHorizontalAlignment(StringToHAlign(StrVal)); SetProps.Add(TEXT("h_align")); }
		if (SlotProperties->TryGetStringField(TEXT("v_align"), StrVal)) { OS->SetVerticalAlignment(StringToVAlign(StrVal));   SetProps.Add(TEXT("v_align")); }
		const TSharedPtr<FJsonObject>* PadObj;
		if (SlotProperties->TryGetObjectField(TEXT("padding"), PadObj)) { OS->SetPadding(ExtractPadding(*PadObj)); SetProps.Add(TEXT("padding")); }
	}
	else if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
	{
		if (SlotProperties->TryGetStringField(TEXT("h_align"), StrVal)) { VS->SetHorizontalAlignment(StringToHAlign(StrVal)); SetProps.Add(TEXT("h_align")); }
		if (SlotProperties->TryGetStringField(TEXT("v_align"), StrVal)) { VS->SetVerticalAlignment(StringToVAlign(StrVal));   SetProps.Add(TEXT("v_align")); }
		if (SlotProperties->TryGetStringField(TEXT("size_rule"), StrVal))
		{
			FSlateChildSize SR = VS->GetSize();
			SR.SizeRule = StrVal.Equals(TEXT("Auto"), ESearchCase::IgnoreCase) ? ESlateSizeRule::Automatic : ESlateSizeRule::Fill;
			VS->SetSize(SR); SetProps.Add(TEXT("size_rule"));
		}
		if (SlotProperties->TryGetNumberField(TEXT("fill_weight"), NumVal))
		{
			FSlateChildSize SR = VS->GetSize(); SR.Value = (float)NumVal; VS->SetSize(SR); SetProps.Add(TEXT("fill_weight"));
		}
		const TSharedPtr<FJsonObject>* PadObj;
		if (SlotProperties->TryGetObjectField(TEXT("padding"), PadObj)) { VS->SetPadding(ExtractPadding(*PadObj)); SetProps.Add(TEXT("padding")); }
	}
	else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
	{
		if (SlotProperties->TryGetStringField(TEXT("h_align"), StrVal)) { HS->SetHorizontalAlignment(StringToHAlign(StrVal)); SetProps.Add(TEXT("h_align")); }
		if (SlotProperties->TryGetStringField(TEXT("v_align"), StrVal)) { HS->SetVerticalAlignment(StringToVAlign(StrVal));   SetProps.Add(TEXT("v_align")); }
		if (SlotProperties->TryGetStringField(TEXT("size_rule"), StrVal))
		{
			FSlateChildSize SR = HS->GetSize();
			SR.SizeRule = StrVal.Equals(TEXT("Auto"), ESearchCase::IgnoreCase) ? ESlateSizeRule::Automatic : ESlateSizeRule::Fill;
			HS->SetSize(SR); SetProps.Add(TEXT("size_rule"));
		}
		if (SlotProperties->TryGetNumberField(TEXT("fill_weight"), NumVal))
		{
			FSlateChildSize SR = HS->GetSize(); SR.Value = (float)NumVal; HS->SetSize(SR); SetProps.Add(TEXT("fill_weight"));
		}
		const TSharedPtr<FJsonObject>* PadObj;
		if (SlotProperties->TryGetObjectField(TEXT("padding"), PadObj)) { HS->SetPadding(ExtractPadding(*PadObj)); SetProps.Add(TEXT("padding")); }
	}
	else
	{
		return ErrorResult(FString::Printf(TEXT("Unsupported slot type: %s"), *Slot->GetClass()->GetName()));
	}

	if (SetProps.Num() == 0)
		return ErrorResult(TEXT("No recognized slot properties were set."));

	WBP->MarkPackageDirty();
	return SuccessResult(FString::Printf(TEXT("Set %d slot properties on %s: %s"),
		SetProps.Num(), *WidgetName, *FString::Join(SetProps, TEXT(", "))));
}

// ============================================================================
// SaveWidgetBlueprint
// ============================================================================

TSharedPtr<FJsonObject> FWidgetEditor::SaveWidgetBlueprint(UWidgetBlueprint* WBP)
{
	if (!WBP) return ErrorResult(TEXT("Null Widget Blueprint"));
	FString AssetPath = WBP->GetPathName();
	bool bSaved = UEditorAssetLibrary::SaveAsset(AssetPath, false);
	return bSaved ? SuccessResult(FString::Printf(TEXT("Saved: %s"), *AssetPath))
	              : ErrorResult(FString::Printf(TEXT("Failed to save: %s"), *AssetPath));
}

// ============================================================================
// Batch
// ============================================================================

TSharedPtr<FJsonObject> FWidgetEditor::DispatchBatchOp(UWidgetBlueprint* WBP, const TSharedPtr<FJsonObject>& OpData)
{
	FString OpName; OpData->TryGetStringField(TEXT("op"), OpName); OpName = OpName.ToLower();

	if (OpName == TEXT("add_widget"))
	{
		FString WClass, WName, ParentName;
		if (!OpData->TryGetStringField(TEXT("widget_class"), WClass) || WClass.IsEmpty())
			return ErrorResult(TEXT("add_widget: missing widget_class"));
		if (!OpData->TryGetStringField(TEXT("widget_name"), WName) || WName.IsEmpty())
			return ErrorResult(TEXT("add_widget: missing widget_name"));
		OpData->TryGetStringField(TEXT("parent_name"), ParentName);
		return AddWidget(WBP, WClass, WName, ParentName);
	}
	else if (OpName == TEXT("remove_widget"))
	{
		FString WName;
		if (!OpData->TryGetStringField(TEXT("widget_name"), WName) || WName.IsEmpty())
			return ErrorResult(TEXT("remove_widget: missing widget_name"));
		return RemoveWidget(WBP, WName);
	}
	else if (OpName == TEXT("set_property"))
	{
		FString WName;
		if (!OpData->TryGetStringField(TEXT("widget_name"), WName) || WName.IsEmpty())
			return ErrorResult(TEXT("set_property: missing widget_name"));
		const TSharedPtr<FJsonObject>* PropsObj;
		if (!OpData->TryGetObjectField(TEXT("properties"), PropsObj))
			return ErrorResult(TEXT("set_property: missing properties object"));
		return SetWidgetProperty(WBP, WName, *PropsObj);
	}
	else if (OpName == TEXT("set_slot"))
	{
		FString WName;
		if (!OpData->TryGetStringField(TEXT("widget_name"), WName) || WName.IsEmpty())
			return ErrorResult(TEXT("set_slot: missing widget_name"));
		const TSharedPtr<FJsonObject>* SlotObj;
		if (!OpData->TryGetObjectField(TEXT("slot_properties"), SlotObj))
			return ErrorResult(TEXT("set_slot: missing slot_properties object"));
		return SetSlotProperty(WBP, WName, *SlotObj);
	}

	return ErrorResult(FString::Printf(TEXT("Unknown batch op: '%s'"), *OpName));
}

TSharedPtr<FJsonObject> FWidgetEditor::ExecuteBatch(UWidgetBlueprint* WBP, const TArray<TSharedPtr<FJsonValue>>& Operations)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!WBP) { Result->SetBoolField(TEXT("success"), false); Result->SetStringField(TEXT("error"), TEXT("Null WBP")); return Result; }

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 OKCount = 0, ErrCount = 0;

	for (int32 i = 0; i < Operations.Num(); ++i)
	{
		if (!Operations[i].IsValid() || Operations[i]->Type != EJson::Object)
		{
			ResultsArray.Add(MakeShared<FJsonValueObject>(ErrorResult(FString::Printf(TEXT("[%d] not a JSON object"), i))));
			ErrCount++; continue;
		}
		TSharedPtr<FJsonObject> OpData = Operations[i]->AsObject();
		TSharedPtr<FJsonObject> OpResult = DispatchBatchOp(WBP, OpData);
		ResultsArray.Add(MakeShared<FJsonValueObject>(OpResult));
		bool bOK = false; OpResult->TryGetBoolField(TEXT("success"), bOK);
		if (bOK) OKCount++; else ErrCount++;
	}

	Result->SetBoolField(TEXT("success"), ErrCount == 0);
	Result->SetArrayField(TEXT("results"), ResultsArray);
	Result->SetNumberField(TEXT("ok_count"), OKCount);
	Result->SetNumberField(TEXT("error_count"), ErrCount);
	Result->SetNumberField(TEXT("total"), Operations.Num());
	return Result;
}
