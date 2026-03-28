// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class UBlendSpace;

class FMCPTool_BlendSpace : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("blend_space");
		Info.Description = TEXT(
			"BlendSpace asset editing tool.\n\n"
			"Read Operations:\n"
			"- 'inspect': Get full blend space data including axes, samples, and interpolation settings\n"
			"- 'list': Search for blend space assets in a content folder\n\n"
			"Write Operations:\n"
			"- 'create': Create a new BlendSpace, BlendSpace1D, AimOffsetBlendSpace, or AimOffsetBlendSpace1D\n"
			"- 'add_sample': Add animation sample at position (x, y)\n"
			"- 'remove_sample': Remove sample by index\n"
			"- 'move_sample': Reposition existing sample\n"
			"- 'set_sample_animation': Replace animation on existing sample\n"
			"- 'set_axis': Configure axis parameters (name, range, grid, interpolation)\n"
			"- 'save': Save blend space to disk\n"
			"- 'batch': Execute multiple operations with a single ResampleData at the end\n\n"
			"Blend Space Types: BlendSpace1D (default), BlendSpace, AimOffsetBlendSpace1D, AimOffsetBlendSpace\n"
			"Interpolation Types: Average, Linear, Cubic, EaseInOut, ExponentialDecay, SpringDamper"
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("operation"), TEXT("string"), TEXT("Operation: inspect, list, create, add_sample, remove_sample, move_sample, set_sample_animation, set_axis, save, batch"), true),
			FMCPToolParameter(TEXT("asset_path"), TEXT("string"), TEXT("Path to blend space asset (e.g., '/Game/Animations/BS_Walk')")),
			FMCPToolParameter(TEXT("folder_path"), TEXT("string"), TEXT("Content folder to search (for list)")),
			FMCPToolParameter(TEXT("recursive"), TEXT("boolean"), TEXT("Search recursively (for list)"), false, TEXT("false")),
			FMCPToolParameter(TEXT("package_path"), TEXT("string"), TEXT("Package path for new asset (for create)")),
			FMCPToolParameter(TEXT("name"), TEXT("string"), TEXT("Name for new blend space (for create)")),
			FMCPToolParameter(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton path (for create)")),
			FMCPToolParameter(TEXT("type"), TEXT("string"), TEXT("Blend space type: BlendSpace1D, BlendSpace, AimOffsetBlendSpace1D, AimOffsetBlendSpace"), false, TEXT("BlendSpace1D")),
			FMCPToolParameter(TEXT("animation_path"), TEXT("string"), TEXT("Path to animation sequence (for add_sample, set_sample_animation)")),
			FMCPToolParameter(TEXT("sample_index"), TEXT("number"), TEXT("Sample index (for remove_sample, move_sample, set_sample_animation)")),
			FMCPToolParameter(TEXT("x"), TEXT("number"), TEXT("X position for sample"), false, TEXT("0")),
			FMCPToolParameter(TEXT("y"), TEXT("number"), TEXT("Y position for sample (2D only)"), false, TEXT("0")),
			FMCPToolParameter(TEXT("rate_scale"), TEXT("number"), TEXT("Animation rate scale (-1 to leave unchanged)"), false, TEXT("-1")),
			FMCPToolParameter(TEXT("axis_index"), TEXT("number"), TEXT("Axis index 0=X, 1=Y (for set_axis)")),
			FMCPToolParameter(TEXT("axis_name"), TEXT("string"), TEXT("Axis display name (for set_axis)")),
			FMCPToolParameter(TEXT("min_axis_value"), TEXT("number"), TEXT("Axis minimum value (for set_axis)")),
			FMCPToolParameter(TEXT("max_axis_value"), TEXT("number"), TEXT("Axis maximum value (for set_axis)")),
			FMCPToolParameter(TEXT("grid_divisions"), TEXT("number"), TEXT("Number of grid divisions (for set_axis)")),
			FMCPToolParameter(TEXT("snap_to_grid"), TEXT("boolean"), TEXT("Snap samples to grid (for set_axis)")),
			FMCPToolParameter(TEXT("wrap_input"), TEXT("boolean"), TEXT("Wrap axis input (for set_axis)")),
			FMCPToolParameter(TEXT("interpolation_type"), TEXT("string"), TEXT("Interpolation: Average, Linear, Cubic, EaseInOut, ExponentialDecay, SpringDamper (for set_axis)")),
			FMCPToolParameter(TEXT("operations"), TEXT("array"), TEXT("Array of operation objects for batch. Each has 'op' field and operation-specific params."))
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult HandleInspect(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleList(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleCreate(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleAddSample(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleRemoveSample(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleMoveSample(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleSetSampleAnimation(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleSetAxis(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleSave(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleBatch(const TSharedRef<FJsonObject>& Params);
};
