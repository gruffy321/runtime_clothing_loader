// Runtime Clothing Loader - UE 4.19.2
// Milestone 4: skeleton binder. See RCLSkeletonBinder.h for the contract.

#include "RCLSkeletonBinder.h"
#include "RuntimeClothingLoader.h"

FRCLBindSettings FRCLBindSettings::FromLoadOptions(const FRCLLoadOptions& Options)
{
	FRCLBindSettings Settings;
	Settings.SkinningMode = Options.SkinningMode;
	Settings.RigidAttachBone = Options.RigidAttachBone;
	Settings.UnknownBonePolicy = Options.UnknownBonePolicy;
	return Settings;
}

namespace RCLBind
{
	/** Adds Weight to Bone in a small per-vertex list, merging duplicates. */
	void AccumulateInfluence(TArray<FRCLInfluence>& VertexInfluences, int32 Bone, float Weight)
	{
		for (FRCLInfluence& Existing : VertexInfluences)
		{
			if (Existing.JointIndex == Bone)
			{
				Existing.Weight += Weight;
				return;
			}
		}
		FRCLInfluence Influence;
		Influence.JointIndex = Bone;
		Influence.Weight = Weight;
		VertexInfluences.Add(Influence);
	}
}

void FRCLSkeletonBinder::MapJoints(const FRCLImportData& Data, const FRCLSkeletonSnapshot& Target,
	TArray<int32>& OutTargetOfJoint, FRCLBindResult& OutResult)
{
	OutTargetOfJoint.SetNumUninitialized(Data.Joints.Num());

	for (int32 JointIndex = 0; JointIndex < Data.Joints.Num(); ++JointIndex)
	{
		const FRCLImportJoint& Joint = Data.Joints[JointIndex];
		const int32 TargetIndex = Target.FindBoneIndex(Joint.Name);
		OutTargetOfJoint[JointIndex] = TargetIndex;

		if (TargetIndex != INDEX_NONE)
		{
			OutResult.MatchedBones.Add(Joint.Name);
		}
		else if (Joint.ParentIndex == INDEX_NONE)
		{
			// The file's root (a DAZ garment's own figure root, a Blender "Armature") stands for
			// the target root whatever it is called. Its pose is not used: see BuildGarmentSkeleton.
			OutTargetOfJoint[JointIndex] = 0;
			if (OutResult.FileRootName == NAME_None)
			{
				OutResult.FileRootName = Joint.Name;
				OutResult.bRootMappedByPosition = true;
			}
		}
		else
		{
			OutResult.UnknownBones.Add(Joint.Name);
		}

		if (Joint.ParentIndex == INDEX_NONE && OutResult.FileRootName == NAME_None)
		{
			OutResult.FileRootName = Joint.Name; // root matched by name
		}
	}
}

void FRCLSkeletonBinder::ResolveUnknownJoints(const FRCLImportData& Data, TArray<int32>& InOutTargetOfJoint,
	const TArray<bool>& bJointIsKnown, FRCLBindResult& OutResult)
{
	// Parents precede children in the file joints, so resolving in index order means an unknown
	// joint's parent is already resolved (known, or itself moved to a known ancestor).
	for (int32 JointIndex = 0; JointIndex < Data.Joints.Num(); ++JointIndex)
	{
		if (bJointIsKnown[JointIndex])
		{
			continue;
		}
		const int32 Parent = Data.Joints[JointIndex].ParentIndex;
		InOutTargetOfJoint[JointIndex] = (Parent != INDEX_NONE) ? InOutTargetOfJoint[Parent] : 0;

		if (Data.Joints[JointIndex].bInfluencesVertices)
		{
			OutResult.ReparentedBones.Add(Data.Joints[JointIndex].Name);
		}
	}
}

void FRCLSkeletonBinder::BuildGarmentSkeleton(const FRCLImportData& Data, const FRCLSkeletonSnapshot& Target,
	const TArray<int32>& TargetOfJoint, const TArray<bool>& bTargetBoneUsed, FRCLBindResult& OutResult)
{
	// Needed = used bones plus every ancestor, always including the root.
	TArray<bool> bNeeded;
	bNeeded.SetNumZeroed(Target.NumBones());
	bNeeded[0] = true;
	for (int32 Bone = 0; Bone < Target.NumBones(); ++Bone)
	{
		if (!bTargetBoneUsed[Bone])
		{
			continue;
		}
		for (int32 Current = Bone; Current != INDEX_NONE && !bNeeded[Current]; Current = Target.ParentIndices[Current])
		{
			bNeeded[Current] = true;
		}
	}

	// File joints matched by name supply the bind pose for their bone (the root mapped by
	// position does not: its file pose describes the file's own figure root, not the target's).
	TMap<int32, int32> FileJointOfTarget;
	for (int32 JointIndex = 0; JointIndex < Data.Joints.Num(); ++JointIndex)
	{
		const int32 TargetIndex = Target.FindBoneIndex(Data.Joints[JointIndex].Name);
		if (TargetIndex != INDEX_NONE && !FileJointOfTarget.Contains(TargetIndex))
		{
			FileJointOfTarget.Add(TargetIndex, JointIndex);
		}
	}

	TMap<int32, int32> GarmentBoneOfTarget;
	for (int32 Bone = 0; Bone < Target.NumBones(); ++Bone)
	{
		if (!bNeeded[Bone])
		{
			continue;
		}

		const int32 TargetParent = Target.ParentIndices[Bone];
		const int32* GarmentParent = (TargetParent != INDEX_NONE) ? GarmentBoneOfTarget.Find(TargetParent) : nullptr;

		const int32* FileJoint = FileJointOfTarget.Find(Bone);
		const FTransform Component = FileJoint
			? Data.Joints[*FileJoint].BindPoseComponentSpace
			: Target.RefPoseComponent[Bone];

		const int32 GarmentIndex = OutResult.BoneNames.Add(Target.BoneNames[Bone]);
		OutResult.ParentIndices.Add(GarmentParent ? *GarmentParent : INDEX_NONE);
		OutResult.TargetBoneIndices.Add(Bone);
		OutResult.RefPoseComponent.Add(Component);
		OutResult.RefPoseLocal.Add(GarmentParent
			? Component.GetRelativeTransform(OutResult.RefPoseComponent[*GarmentParent])
			: Component);

		GarmentBoneOfTarget.Add(Bone, GarmentIndex);
	}
}

void FRCLSkeletonBinder::RemapInfluences(const FRCLImportData& Data, const TArray<int32>& TargetOfJoint,
	const TMap<int32, int32>& GarmentBoneOfTarget, FRCLBindResult& OutResult)
{
	OutResult.MeshInfluences.SetNum(Data.Meshes.Num());
	OutResult.MeshInfluenceStart.SetNum(Data.Meshes.Num());

	TArray<FRCLInfluence> VertexInfluences;
	for (int32 MeshIndex = 0; MeshIndex < Data.Meshes.Num(); ++MeshIndex)
	{
		const FRCLImportMesh& Mesh = Data.Meshes[MeshIndex];
		if (!Mesh.HasSkinWeights())
		{
			continue; // weights come from transfer (Milestone 8)
		}

		TArray<FRCLInfluence>& Influences = OutResult.MeshInfluences[MeshIndex];
		TArray<uint32>& InfluenceStart = OutResult.MeshInfluenceStart[MeshIndex];
		Influences.Reserve(Mesh.Influences.Num());
		InfluenceStart.Reserve(Mesh.NumVertices() + 1);
		InfluenceStart.Add(0);

		for (int32 Vertex = 0; Vertex < Mesh.NumVertices(); ++Vertex)
		{
			VertexInfluences.Reset();
			for (uint32 Index = Mesh.InfluenceStart[Vertex]; Index < Mesh.InfluenceStart[Vertex + 1]; ++Index)
			{
				const FRCLInfluence& Influence = Mesh.Influences[Index];
				const int32* GarmentBone = GarmentBoneOfTarget.Find(TargetOfJoint[Influence.JointIndex]);
				if (GarmentBone && Influence.Weight > 0.0f)
				{
					RCLBind::AccumulateInfluence(VertexInfluences, *GarmentBone, Influence.Weight);
				}
			}
			Influences.Append(VertexInfluences);
			InfluenceStart.Add((uint32)Influences.Num());
		}
	}
}

void FRCLSkeletonBinder::MeasureBindOffsets(const FRCLImportData& Data, const FRCLSkeletonSnapshot& Target,
	const TArray<int32>& TargetOfJoint, const FRCLBindSettings& Settings, FRCLBindResult& OutResult)
{
	for (int32 JointIndex = 0; JointIndex < Data.Joints.Num(); ++JointIndex)
	{
		const FRCLImportJoint& Joint = Data.Joints[JointIndex];
		const int32 TargetIndex = Target.FindBoneIndex(Joint.Name);
		if (TargetIndex == INDEX_NONE)
		{
			continue; // unknown or root-by-position: nothing to compare against
		}

		const float Offset = (Joint.BindPoseComponentSpace.GetTranslation() - Target.RefPoseComponent[TargetIndex].GetTranslation()).Size();
		if (Offset > OutResult.MaxBindOffsetCm)
		{
			OutResult.MaxBindOffsetCm = Offset;
			OutResult.MaxBindOffsetBone = Joint.Name;
		}
		if (Offset > Settings.BindToleranceCm)
		{
			OutResult.BonesOverBindTolerance.Add(Joint.Name);
		}
	}

	if (OutResult.BonesOverBindTolerance.Num() > 0)
	{
		OutResult.Warnings.Add(FString::Printf(
			TEXT("%d bone(s) sit more than %.1f cm from the body's (largest: '%s', %.1f cm). The garment may have been fitted to a different pose, figure or scale."),
			OutResult.BonesOverBindTolerance.Num(), Settings.BindToleranceCm,
			*OutResult.MaxBindOffsetBone.ToString(), OutResult.MaxBindOffsetCm));
	}
}

bool FRCLSkeletonBinder::Bind(const FRCLImportData& Data, const FRCLSkeletonSnapshot& Target, const FRCLBindSettings& Settings,
	FRCLBindResult& OutResult, ERCLFailureReason& OutReason, FString& OutError)
{
	OutResult = FRCLBindResult();
	OutReason = ERCLFailureReason::None;

	if (!Target.IsValid())
	{
		OutReason = ERCLFailureReason::MissingTargetSkeleton;
		OutError = TEXT("No target skeleton to bind the garment to.");
		return false;
	}

	TArray<bool> bTargetBoneUsed;
	bTargetBoneUsed.SetNumZeroed(Target.NumBones());

	// ---- RigidToBone: every vertex follows one bone; file weights ignored ----------------------
	if (Settings.SkinningMode == ERCLSkinningMode::RigidToBone)
	{
		const int32 RigidBone = Target.FindBoneIndex(Settings.RigidAttachBone);
		if (RigidBone == INDEX_NONE)
		{
			OutReason = ERCLFailureReason::InternalError;
			OutError = FString::Printf(TEXT("Attach bone '%s' is not in the skeleton '%s'."),
				*Settings.RigidAttachBone.ToString(), *Target.SourceName);
			return false;
		}
		bTargetBoneUsed[RigidBone] = true;
		BuildGarmentSkeleton(FRCLImportData(), Target, TArray<int32>(), bTargetBoneUsed, OutResult);

		const int32 GarmentBone = OutResult.TargetBoneIndices.IndexOfByKey(RigidBone);
		OutResult.MeshInfluences.SetNum(Data.Meshes.Num());
		OutResult.MeshInfluenceStart.SetNum(Data.Meshes.Num());
		for (int32 MeshIndex = 0; MeshIndex < Data.Meshes.Num(); ++MeshIndex)
		{
			const int32 NumVertices = Data.Meshes[MeshIndex].NumVertices();
			TArray<FRCLInfluence>& Influences = OutResult.MeshInfluences[MeshIndex];
			TArray<uint32>& InfluenceStart = OutResult.MeshInfluenceStart[MeshIndex];
			FRCLInfluence Full;
			Full.JointIndex = GarmentBone;
			Full.Weight = 1.0f;
			Influences.Init(Full, NumVertices);
			InfluenceStart.SetNumUninitialized(NumVertices + 1);
			for (int32 Vertex = 0; Vertex <= NumVertices; ++Vertex)
			{
				InfluenceStart[Vertex] = (uint32)Vertex;
			}
		}
		return true;
	}

	// ---- TransferAlways, or a file with no skin: weights come from the body (Milestone 8) ------
	const bool bHasAnyWeights = Data.Joints.Num() > 0 && Data.Meshes.ContainsByPredicate(
		[](const FRCLImportMesh& Mesh) { return Mesh.HasSkinWeights(); });

	if (Settings.SkinningMode == ERCLSkinningMode::TransferAlways || !bHasAnyWeights)
	{
		BuildGarmentSkeleton(FRCLImportData(), Target, TArray<int32>(), bTargetBoneUsed, OutResult);
		OutResult.MeshInfluences.SetNum(Data.Meshes.Num());
		OutResult.MeshInfluenceStart.SetNum(Data.Meshes.Num());
		OutResult.bTransferRecommended = true;
		if (!bHasAnyWeights)
		{
			OutResult.Warnings.Add(TEXT("The file has no skin weights; they will be copied from the body."));
		}
		return true;
	}

	// ---- Auto: remap the file's weights ----------------------------------------------------------
	TArray<int32> TargetOfJoint;
	MapJoints(Data, Target, TargetOfJoint, OutResult);

	TArray<bool> bJointIsKnown;
	bJointIsKnown.SetNumUninitialized(Data.Joints.Num());
	TArray<FName> WeightedUnknownBones;
	for (int32 JointIndex = 0; JointIndex < Data.Joints.Num(); ++JointIndex)
	{
		bJointIsKnown[JointIndex] = TargetOfJoint[JointIndex] != INDEX_NONE;
		if (!bJointIsKnown[JointIndex] && Data.Joints[JointIndex].bInfluencesVertices)
		{
			WeightedUnknownBones.Add(Data.Joints[JointIndex].Name);
		}
	}

	if (Settings.UnknownBonePolicy == ERCLUnknownBonePolicy::Reject && WeightedUnknownBones.Num() > 0)
	{
		FString Names;
		for (int32 Index = 0; Index < WeightedUnknownBones.Num() && Index < 10; ++Index)
		{
			Names += (Index > 0 ? TEXT(", ") : TEXT("")) + WeightedUnknownBones[Index].ToString();
		}
		OutReason = ERCLFailureReason::UnknownBonesRejected;
		OutError = FString::Printf(TEXT("%d bone(s) are not in the skeleton '%s': %s%s"), WeightedUnknownBones.Num(),
			*Target.SourceName, *Names, WeightedUnknownBones.Num() > 10 ? TEXT(", ...") : TEXT(""));
		return false;
	}

	ResolveUnknownJoints(Data, TargetOfJoint, bJointIsKnown, OutResult);

	// Weight statistics and the set of target bones in use.
	double TotalWeight = 0.0;
	double UnknownWeight = 0.0;
	for (const FRCLImportMesh& Mesh : Data.Meshes)
	{
		if (!Mesh.HasSkinWeights())
		{
			continue;
		}
		for (int32 Vertex = 0; Vertex < Mesh.NumVertices(); ++Vertex)
		{
			bool bTouchesUnknown = false;
			for (uint32 Index = Mesh.InfluenceStart[Vertex]; Index < Mesh.InfluenceStart[Vertex + 1]; ++Index)
			{
				const FRCLInfluence& Influence = Mesh.Influences[Index];
				if (Influence.Weight <= 0.0f)
				{
					continue;
				}
				TotalWeight += Influence.Weight;
				if (!bJointIsKnown[Influence.JointIndex])
				{
					UnknownWeight += Influence.Weight;
					bTouchesUnknown = true;
				}
				bTargetBoneUsed[TargetOfJoint[Influence.JointIndex]] = true;
			}
			OutResult.NumVerticesReparented += bTouchesUnknown ? 1 : 0;
		}
	}
	OutResult.UnknownWeightShare = TotalWeight > 0.0 ? (float)(UnknownWeight / TotalWeight) : 0.0f;
	OutResult.bTransferRecommended = OutResult.UnknownWeightShare > Settings.TransferRecommendedShare;

	if (OutResult.ReparentedBones.Num() > 0)
	{
		OutResult.Warnings.Add(FString::Printf(TEXT("%d bone(s) not in the skeleton had their weights moved to a parent bone (%d vertices, %.0f%% of all weight)."),
			OutResult.ReparentedBones.Num(), OutResult.NumVerticesReparented, OutResult.UnknownWeightShare * 100.0f));
	}

	BuildGarmentSkeleton(Data, Target, TargetOfJoint, bTargetBoneUsed, OutResult);

	TMap<int32, int32> GarmentBoneOfTarget;
	for (int32 GarmentBone = 0; GarmentBone < OutResult.TargetBoneIndices.Num(); ++GarmentBone)
	{
		GarmentBoneOfTarget.Add(OutResult.TargetBoneIndices[GarmentBone], GarmentBone);
	}
	RemapInfluences(Data, TargetOfJoint, GarmentBoneOfTarget, OutResult);

	MeasureBindOffsets(Data, Target, TargetOfJoint, Settings, OutResult);

	UE_LOG(LogRuntimeClothing, Verbose, TEXT("Bound '%s' to '%s': %d garment bones, %d matched, %d unknown, max bind offset %.2f cm."),
		*Data.SourceFile, *Target.SourceName, OutResult.BoneNames.Num(), OutResult.MatchedBones.Num(),
		OutResult.UnknownBones.Num(), OutResult.MaxBindOffsetCm);
	return true;
}
