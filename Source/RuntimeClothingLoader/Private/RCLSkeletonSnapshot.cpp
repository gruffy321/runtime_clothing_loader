// Runtime Clothing Loader - UE 4.19.2

#include "RCLSkeletonSnapshot.h"
#include "RuntimeClothingLoader.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"

FRCLSkeletonSnapshot FRCLSkeletonSnapshot::FromSkeletalMesh(const USkeletalMesh& Mesh)
{
	check(IsInGameThread());
	return FromReferenceSkeleton(Mesh.RefSkeleton, Mesh.GetName());
}

FRCLSkeletonSnapshot FRCLSkeletonSnapshot::FromSkeleton(const USkeleton& Skeleton)
{
	check(IsInGameThread());
	return FromReferenceSkeleton(Skeleton.GetReferenceSkeleton(), Skeleton.GetName());
}

FRCLSkeletonSnapshot FRCLSkeletonSnapshot::FromReferenceSkeleton(const FReferenceSkeleton& RefSkeleton, const FString& InSourceName)
{
	FRCLSkeletonSnapshot Snapshot;
	Snapshot.SourceName = InSourceName;

	// Raw bones only: in 4.19 the raw arrays exclude virtual bones, which are never skin targets.
	const TArray<FMeshBoneInfo>& BoneInfo = RefSkeleton.GetRawRefBoneInfo();
	const TArray<FTransform>& BonePose = RefSkeleton.GetRawRefBonePose();
	const int32 NumBones = RefSkeleton.GetRawBoneNum();

	if (BoneInfo.Num() != NumBones || BonePose.Num() != NumBones)
	{
		UE_LOG(LogRuntimeClothing, Error, TEXT("Skeleton '%s' has inconsistent reference data (%d bones, %d infos, %d poses)."),
			*InSourceName, NumBones, BoneInfo.Num(), BonePose.Num());
		return Snapshot;
	}

	Snapshot.BoneNames.Reserve(NumBones);
	Snapshot.ParentIndices.Reserve(NumBones);
	Snapshot.RefPoseLocal.Reserve(NumBones);
	Snapshot.RefPoseComponent.Reserve(NumBones);
	Snapshot.NameToIndex.Reserve(NumBones);

	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		const FMeshBoneInfo& Info = BoneInfo[BoneIndex];
		const int32 ParentIndex = Info.ParentIndex;

		// FReferenceSkeleton guarantees parents precede children; verify rather than trust.
		if (ParentIndex >= BoneIndex)
		{
			UE_LOG(LogRuntimeClothing, Error, TEXT("Skeleton '%s': bone %d '%s' has parent %d that does not precede it."),
				*InSourceName, BoneIndex, *Info.Name.ToString(), ParentIndex);
			return FRCLSkeletonSnapshot();
		}

		Snapshot.BoneNames.Add(Info.Name);
		Snapshot.ParentIndices.Add(ParentIndex);
		Snapshot.RefPoseLocal.Add(BonePose[BoneIndex]);

		const FTransform ComponentPose = (ParentIndex == INDEX_NONE)
			? BonePose[BoneIndex]
			: BonePose[BoneIndex] * Snapshot.RefPoseComponent[ParentIndex];
		Snapshot.RefPoseComponent.Add(ComponentPose);

		Snapshot.NameToIndex.Add(Info.Name, BoneIndex);
	}

	return Snapshot;
}

int32 FRCLSkeletonSnapshot::FindBoneIndex(FName BoneName) const
{
	const int32* Found = NameToIndex.Find(BoneName);
	return Found ? *Found : INDEX_NONE;
}

bool FRCLSkeletonSnapshot::IsAncestorOf(int32 AncestorIndex, int32 BoneIndex) const
{
	if (!ParentIndices.IsValidIndex(AncestorIndex) || !ParentIndices.IsValidIndex(BoneIndex))
	{
		return false;
	}

	// Parents always have lower indices, so this walk terminates.
	int32 Current = ParentIndices[BoneIndex];
	while (Current != INDEX_NONE)
	{
		if (Current == AncestorIndex)
		{
			return true;
		}
		Current = ParentIndices[Current];
	}
	return false;
}
