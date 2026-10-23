// Runtime Clothing Loader - UE 4.19.2
// Milestone 4: binds a parsed garment's joints to the target skeleton (SKN_BaseCharacter).
//
// Milestone 4 - header for TD review. No implementation yet.
//
// Worker thread only. Inputs are plain data (FRCLImportData from the parser, FRCLSkeletonSnapshot
// taken on the game thread), so binding never touches a UObject.

#pragma once

#include "CoreMinimal.h"
#include "RCLTypes.h"
#include "RCLImportData.h"
#include "RCLSkeletonSnapshot.h"

/**
 * The garment's own reference skeleton and its skin weights, expressed against the target.
 *
 * Bone set: every target bone the garment's weights land on, plus all of their ancestors up to
 * the target root, in the target's order (so parents always precede children). Hierarchy comes
 * from the target, never from the file, so it always agrees with the body under
 * SetMasterPoseComponent, which matches bones by name.
 *
 * Reference pose: for bones the file has, the file's bind pose (the pose the garment was skinned
 * in); for ancestors the file lacks, the target's reference pose. Under master pose each vertex is
 * skinned with the body's bone transform times the inverse of this pose, so a garment fitted to a
 * slightly different pose or proportion still follows the body correctly.
 */
struct FRCLBindResult
{
	// ---- Garment reference skeleton (parallel arrays, parent-first) --------------------------

	TArray<FName> BoneNames;
	TArray<int32> ParentIndices;          // INDEX_NONE for the root
	TArray<int32> TargetBoneIndices;      // index of the same bone in FRCLSkeletonSnapshot
	TArray<FTransform> RefPoseComponent;  // bind pose, component space (cm, UE axes)
	TArray<FTransform> RefPoseLocal;      // relative to parent, derived from RefPoseComponent

	// ---- Skin weights, one entry per FRCLImportData::Meshes[i] -----------------------------

	/** Same layout as FRCLImportMesh::Influences / InfluenceStart, but JointIndex is now an index
	 *  into BoneNames above. Influences on the same bone are merged. Not yet normalised or trimmed
	 *  to MaxBoneInfluences (the mesh builder does that, Milestone 5). Empty for a mesh whose
	 *  weights will come from weight transfer (Milestone 8). */
	TArray<TArray<FRCLInfluence>> MeshInfluences;
	TArray<TArray<uint32>> MeshInfluenceStart;

	// ---- Report (copied into FRCLLoadReport) -------------------------------------------------

	/** File root joint name and whether it was mapped to the target root by position in the
	 *  hierarchy rather than by name (DAZ garments: "modesty_suit_7630" -> "Genesis8Female"). */
	FName FileRootName = NAME_None;
	bool bRootMappedByPosition = false;

	TArray<FName> MatchedBones;
	TArray<FName> UnknownBones;

	/** Unknown bones whose weights were moved to an ancestor, and how many vertices that touched. */
	TArray<FName> ReparentedBones;
	int32 NumVerticesReparented = 0;

	/** Share of total skin weight that sat on unknown bones (0..1). Above
	 *  FRCLBindSettings::TransferRecommendedShare, Auto mode should use weight transfer instead. */
	float UnknownWeightShare = 0.0f;
	bool bTransferRecommended = false;

	/** Largest distance between a matched bone's file bind position and the same bone in the
	 *  target reference pose. Small (a few mm) for a garment fitted to stock Genesis 8 Female;
	 *  large values mean a different pose, scale or figure, and are reported as a warning. */
	float MaxBindOffsetCm = 0.0f;
	FName MaxBindOffsetBone = NAME_None;
	TArray<FName> BonesOverBindTolerance;

	TArray<FString> Warnings;
};

/** Binder inputs that come from FRCLLoadOptions; plain values, safe on a worker. */
struct FRCLBindSettings
{
	ERCLSkinningMode SkinningMode = ERCLSkinningMode::Auto;
	FName RigidAttachBone = NAME_None;
	ERCLUnknownBonePolicy UnknownBonePolicy = ERCLUnknownBonePolicy::ReparentToAncestor;

	/** Bind offset above which a bone is listed in BonesOverBindTolerance. */
	float BindToleranceCm = 2.0f;

	/** Unknown-weight share above which bTransferRecommended is set. */
	float TransferRecommendedShare = 0.25f;

	static FRCLBindSettings FromLoadOptions(const FRCLLoadOptions& Options);
};

/**
 * Stateless binder. All work happens in Bind; the private steps are listed so the TD can see the
 * order of operations.
 */
class FRCLSkeletonBinder
{
public:
	/**
	 * Binds every garment in Data to Target. Returns false (with reason and a player-readable
	 * error) for: an empty or invalid target, RigidAttachBone missing from the target, or
	 * unknown bones under the Reject policy.
	 *
	 * Modes:
	 *  - Auto, FileWeights: file weights, remapped. Garments with no weights get empty influence
	 *    arrays. The pipeline then replaces every garment's weights and pose with the body's for Auto
	 *    (Milestone 8, TD 2026-10-23), and only fills the empty ones for FileWeights. Binding the
	 *    file's weights first is what lets Auto fall back to them if the body cannot be read.
	 *  - RigidToBone: every vertex 100% on RigidAttachBone; file weights ignored.
	 *  - TransferAlways: empty influence arrays for every garment.
	 */
	static bool Bind(const FRCLImportData& Data, const FRCLSkeletonSnapshot& Target, const FRCLBindSettings& Settings,
		FRCLBindResult& OutResult, ERCLFailureReason& OutReason, FString& OutError);

	/**
	 * Milestone 8: replaces garment MeshIndex's influences with ones from weight transfer, given as
	 * TARGET bone indices. Bones the garment skeleton lacks are added (with their ancestors), in target
	 * order, and every garment's influences are remapped to the new bone indices.
	 * bUseTargetPose true (body weights): EVERY bone takes the target's reference pose, since the
	 * weights were authored against it. False: existing bones keep their file bind pose.
	 */
	static void AddTargetInfluences(int32 MeshIndex, const TArray<FRCLInfluence>& TargetInfluences,
		const TArray<uint32>& TargetInfluenceStart, const FRCLSkeletonSnapshot& Target, bool bUseTargetPose,
		FRCLBindResult& InOutResult);

private:
	/** File joint -> target bone index (or INDEX_NONE). The file root joint (no parent) maps to
	 *  the target root when its name isn't a target bone. */
	static void MapJoints(const FRCLImportData& Data, const FRCLSkeletonSnapshot& Target,
		TArray<int32>& OutTargetOfJoint, FRCLBindResult& OutResult);

	/** For each unknown joint, the nearest file ancestor that maps (or the target root). */
	static void ResolveUnknownJoints(const FRCLImportData& Data, TArray<int32>& InOutTargetOfJoint,
		const TArray<bool>& bJointIsKnown, FRCLBindResult& OutResult);

	/** Collects the target bones in use, adds ancestors, and fills the garment skeleton arrays
	 *  and reference pose. */
	static void BuildGarmentSkeleton(const FRCLImportData& Data, const FRCLSkeletonSnapshot& Target,
		const TArray<int32>& TargetOfJoint, const TArray<bool>& bTargetBoneUsed, FRCLBindResult& OutResult);

	/** Rewrites each garment's influences to garment bone indices, merging duplicates. */
	static void RemapInfluences(const FRCLImportData& Data, const TArray<int32>& TargetOfJoint,
		const TMap<int32, int32>& GarmentBoneOfTarget, FRCLBindResult& OutResult);

	/** Fills MaxBindOffsetCm / BonesOverBindTolerance from matched bones. */
	static void MeasureBindOffsets(const FRCLImportData& Data, const FRCLSkeletonSnapshot& Target,
		const TArray<int32>& TargetOfJoint, const FRCLBindSettings& Settings, FRCLBindResult& OutResult);
};
