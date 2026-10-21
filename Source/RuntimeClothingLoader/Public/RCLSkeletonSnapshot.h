// Runtime Clothing Loader - UE 4.19.2
// A thread-safe copy of a reference skeleton, taken on the game thread before a load is dispatched.
//
// Milestone 2 - header for TD review. No implementation yet.

#pragma once

#include "CoreMinimal.h"

class USkeleton;
class USkeletalMesh;
struct FReferenceSkeleton;

/**
 * Bone names, hierarchy and reference pose copied out of the engine so the worker thread
 * never touches a UObject.
 *
 * Source of truth: the body mesh's own RefSkeleton when a body is given (it is the pose the
 * body was skinned in), otherwise the USkeleton's reference skeleton. For this game both are
 * SKN_BaseCharacter, a DAZ Genesis 8 Female rig with extra jiggle bones.
 *
 * Virtual bones are excluded: they are never skin targets.
 */
struct RUNTIMECLOTHINGLOADER_API FRCLSkeletonSnapshot
{
	TArray<FName> BoneNames;

	/** INDEX_NONE for the root. Parents come before children, as in FReferenceSkeleton. */
	TArray<int32> ParentIndices;

	/** Reference pose relative to the parent bone. */
	TArray<FTransform> RefPoseLocal;

	/** Reference pose in component space, accumulated from RefPoseLocal. */
	TArray<FTransform> RefPoseComponent;

	/** Name of the skeleton asset it came from, for logs. */
	FString SourceName;

	/** Game thread only. Copies the mesh's RefSkeleton (excluding virtual bones). */
	static FRCLSkeletonSnapshot FromSkeletalMesh(const USkeletalMesh& Mesh);

	/** Game thread only. Copies the skeleton asset's reference skeleton (excluding virtual bones). */
	static FRCLSkeletonSnapshot FromSkeleton(const USkeleton& Skeleton);

	/** Any thread. Index of the bone with this name, or INDEX_NONE. */
	int32 FindBoneIndex(FName BoneName) const;

	/** Any thread. Walks up from BoneIndex; true if AncestorIndex is on the way. */
	bool IsAncestorOf(int32 AncestorIndex, int32 BoneIndex) const;

	int32 NumBones() const { return BoneNames.Num(); }
	bool IsValid() const { return BoneNames.Num() > 0 && BoneNames.Num() == ParentIndices.Num(); }

private:
	/** Shared by both factories. */
	static FRCLSkeletonSnapshot FromReferenceSkeleton(const FReferenceSkeleton& RefSkeleton, const FString& InSourceName);

	/** Built once by the factories so FindBoneIndex is O(1) on the worker. */
	TMap<FName, int32> NameToIndex;
};
