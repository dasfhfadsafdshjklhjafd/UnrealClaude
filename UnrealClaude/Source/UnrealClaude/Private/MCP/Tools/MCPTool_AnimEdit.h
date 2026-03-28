// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class FMCPTool_AnimEdit : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("anim_edit");
		Info.Description = TEXT(
			"Animation editing tool for bone tracks, resampling, skeleton operations, and mesh inspection.\n\n"
			"Track Operations:\n"
			"- 'adjust_track': Batch-adjust bone track keys (location offset, rotation offset, scale override)\n"
			"- 'inspect_track': Read bone transform data at specific frames\n"
			"- 'resample': Resample one or more animations to a target frame rate\n"
			"- 'replace_skeleton': Replace skeleton reference on animations (batch)\n"
			"- 'extract_range': Extract a frame range from an animation into a new asset\n\n"
			"Skeleton/Mesh Operations:\n"
			"- 'rename_bone': Rename a bone in skeleton and/or mesh\n"
			"- 'set_ref_pose': Modify reference pose transform for a bone\n"
			"- 'set_additive_type': Set additive animation type and base pose settings\n"
			"- 'transform_vertices': Apply transform to all vertices in a skeletal mesh\n"
			"- 'inspect_mesh': Inspect skeletal mesh bones, ref pose, bind matrices, and skin weights\n"
			"- 'sync_mesh_bones': Sync bones from skeleton to skeletal mesh (requires SkeletonModifier plugin)"
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("operation"), TEXT("string"), TEXT("Operation: adjust_track, inspect_track, resample, replace_skeleton, extract_range, rename_bone, set_ref_pose, set_additive_type, transform_vertices, inspect_mesh, sync_mesh_bones"), true),
			FMCPToolParameter(TEXT("asset_path"), TEXT("string"), TEXT("Path to animation asset")),
			FMCPToolParameter(TEXT("asset_paths"), TEXT("array"), TEXT("Array of animation paths for batch operations")),
			FMCPToolParameter(TEXT("bone_name"), TEXT("string"), TEXT("Bone name")),
			FMCPToolParameter(TEXT("bone_names"), TEXT("array"), TEXT("Array of bone names (for sync_mesh_bones filter)")),
			FMCPToolParameter(TEXT("location_offset"), TEXT("object"), TEXT("Location offset {x,y,z} to add to all keys")),
			FMCPToolParameter(TEXT("rotation_offset"), TEXT("object"), TEXT("Rotation offset {pitch,yaw,roll} to pre-multiply into all keys")),
			FMCPToolParameter(TEXT("scale_override"), TEXT("object"), TEXT("Scale override {x,y,z} to set on all keys (absolute)")),
			FMCPToolParameter(TEXT("sample_frames"), TEXT("array"), TEXT("Frame indices to sample (for inspect_track). -1 = last frame.")),
			FMCPToolParameter(TEXT("target_fps"), TEXT("number"), TEXT("Target frames per second (for resample)")),
			FMCPToolParameter(TEXT("skeleton_path"), TEXT("string"), TEXT("Skeleton or skeletal mesh path (for replace_skeleton)")),
			FMCPToolParameter(TEXT("mesh_path"), TEXT("string"), TEXT("Skeletal mesh path")),
			FMCPToolParameter(TEXT("old_name"), TEXT("string"), TEXT("Old bone name (for rename_bone)")),
			FMCPToolParameter(TEXT("new_name"), TEXT("string"), TEXT("New bone name (for rename_bone)")),
			FMCPToolParameter(TEXT("position"), TEXT("object"), TEXT("Position {x,y,z} (for set_ref_pose)")),
			FMCPToolParameter(TEXT("rotation"), TEXT("object"), TEXT("Rotation {pitch,yaw,roll} (for set_ref_pose, transform_vertices)")),
			FMCPToolParameter(TEXT("scale"), TEXT("object"), TEXT("Scale {x,y,z} or uniform float (for transform_vertices)")),
			FMCPToolParameter(TEXT("translation"), TEXT("object"), TEXT("Translation {x,y,z} (for transform_vertices)")),
			FMCPToolParameter(TEXT("retransform_vertices"), TEXT("boolean"), TEXT("Retransform mesh vertices after set_ref_pose"), false, TEXT("true")),
			FMCPToolParameter(TEXT("additive_anim_type"), TEXT("string"), TEXT("Additive type: None, LocalSpaceBase, RotationOffsetMeshSpace (for set_additive_type)")),
			FMCPToolParameter(TEXT("base_pose_type"), TEXT("string"), TEXT("Base pose type: None, RefPose, AnimScaled, AnimFrame, LocalAnimFrame"), false, TEXT("RefPose")),
			FMCPToolParameter(TEXT("ref_pose_seq"), TEXT("string"), TEXT("Reference pose animation path (for AnimScaled/AnimFrame base pose type)")),
			FMCPToolParameter(TEXT("ref_frame_index"), TEXT("number"), TEXT("Reference frame index (for AnimFrame base pose type)"), false, TEXT("0")),
			FMCPToolParameter(TEXT("start_frame"), TEXT("number"), TEXT("Start frame for extract_range")),
			FMCPToolParameter(TEXT("end_frame"), TEXT("number"), TEXT("End frame for extract_range (-1 = last frame)")),
			FMCPToolParameter(TEXT("dest_path"), TEXT("string"), TEXT("Destination package path for extract_range (default: same as source)")),
			FMCPToolParameter(TEXT("new_name"), TEXT("string"), TEXT("New asset name for extract_range")),
			FMCPToolParameter(TEXT("save"), TEXT("boolean"), TEXT("Save modified assets to disk"), false, TEXT("true"))
		};
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult HandleAdjustTrack(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleInspectTrack(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleResample(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleReplaceSkeleton(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleSyncMeshBones(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleRenameBone(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleSetRefPose(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleSetAdditiveType(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleTransformVertices(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleInspectMesh(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult HandleExtractRange(const TSharedRef<FJsonObject>& Params);
};
