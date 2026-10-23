// Runtime Clothing Loader - UE 4.19.2
// Milestone 8: body snapshot for weight and morph transfer. See RCLBodySnapshot.h.

#include "RCLBodySnapshot.h"
#include "RuntimeClothingLoader.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/MorphTarget.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "HAL/PlatformTime.h"

// ---- Grid ---------------------------------------------------------------------------------------

void FRCLPointGrid::Build(const TArray<FVector>& Points, float InCellSize)
{
	CellSize = FMath::Max(InCellSize, 0.1f);
	CellStart.Reset();
	CellPoints.Reset();
	if (Points.Num() == 0)
	{
		Dims = FIntVector(0, 0, 0);
		return;
	}

	FVector Min = Points[0], Max = Points[0];
	for (const FVector& P : Points)
	{
		Min = Min.ComponentMin(P);
		Max = Max.ComponentMax(P);
	}
	Origin = Min;
	const FVector Extent = Max - Min;
	Dims = FIntVector(FMath::FloorToInt(Extent.X / CellSize) + 1, FMath::FloorToInt(Extent.Y / CellSize) + 1, FMath::FloorToInt(Extent.Z / CellSize) + 1);
	const int32 NumCells = Dims.X * Dims.Y * Dims.Z;

	// Counting sort of points into cells.
	TArray<int32> CellOfPoint;
	CellOfPoint.SetNumUninitialized(Points.Num());
	CellStart.SetNumZeroed(NumCells + 1);
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		const FVector Local = (Points[I] - Origin) / CellSize;
		const int32 X = FMath::Clamp(FMath::FloorToInt(Local.X), 0, Dims.X - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt(Local.Y), 0, Dims.Y - 1);
		const int32 Z = FMath::Clamp(FMath::FloorToInt(Local.Z), 0, Dims.Z - 1);
		const int32 Cell = (Z * Dims.Y + Y) * Dims.X + X;
		CellOfPoint[I] = Cell;
		++CellStart[Cell + 1];
	}
	for (int32 C = 0; C < NumCells; ++C)
	{
		CellStart[C + 1] += CellStart[C];
	}
	TArray<int32> Fill = CellStart;
	CellPoints.SetNumUninitialized(Points.Num());
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		CellPoints[Fill[CellOfPoint[I]]++] = I;
	}
}

void FRCLPointGrid::FindNearest(const TArray<FVector>& Points, const FVector& Query, float Radius, int32 MaxResults,
	TArray<int32>& OutIndices, TArray<float>& OutDistances) const
{
	OutIndices.Reset();
	OutDistances.Reset();
	if (Dims.X == 0 || MaxResults <= 0)
	{
		return;
	}

	const FVector Local = (Query - Origin) / CellSize;
	const int32 CX = FMath::FloorToInt(Local.X), CY = FMath::FloorToInt(Local.Y), CZ = FMath::FloorToInt(Local.Z);
	const int32 MaxRing = FMath::CeilToInt(Radius / CellSize) + 1;
	const float RadiusSq = Radius * Radius;

	// Grow rings of cells until the nearest MaxResults are certain (the next ring cannot hold a
	// closer point) or the radius is exhausted.
	for (int32 Ring = 0; Ring <= MaxRing; ++Ring)
	{
		for (int32 Z = CZ - Ring; Z <= CZ + Ring; ++Z)
		{
			for (int32 Y = CY - Ring; Y <= CY + Ring; ++Y)
			{
				for (int32 X = CX - Ring; X <= CX + Ring; ++X)
				{
					const bool bShell = FMath::Abs(X - CX) == Ring || FMath::Abs(Y - CY) == Ring || FMath::Abs(Z - CZ) == Ring;
					if (!bShell || X < 0 || Y < 0 || Z < 0 || X >= Dims.X || Y >= Dims.Y || Z >= Dims.Z)
					{
						continue;
					}
					const int32 Cell = (Z * Dims.Y + Y) * Dims.X + X;
					for (int32 K = CellStart[Cell]; K < CellStart[Cell + 1]; ++K)
					{
						const int32 P = CellPoints[K];
						const float DistSq = FVector::DistSquared(Points[P], Query);
						if (DistSq > RadiusSq)
						{
							continue;
						}
						// Insert sorted, keep at most MaxResults.
						int32 Slot = OutDistances.Num();
						while (Slot > 0 && OutDistances[Slot - 1] > DistSq)
						{
							--Slot;
						}
						if (Slot < MaxResults)
						{
							OutIndices.Insert(P, Slot);
							OutDistances.Insert(DistSq, Slot);
							if (OutIndices.Num() > MaxResults)
							{
								OutIndices.Pop(false);
								OutDistances.Pop(false);
							}
						}
					}
				}
			}
		}
		// Everything at least Ring * CellSize away has been seen; stop once the farthest kept is closer.
		const float Covered = Ring * CellSize;
		if (OutIndices.Num() == MaxResults && OutDistances.Last() <= Covered * Covered)
		{
			break;
		}
	}
	for (float& D : OutDistances)
	{
		D = FMath::Sqrt(D);
	}
}

// ---- Snapshot -----------------------------------------------------------------------------------

SIZE_T FRCLBodySnapshot::GetAllocatedSize() const
{
	SIZE_T Size = Positions.GetAllocatedSize() + Normals.GetAllocatedSize() + Influences.GetAllocatedSize()
		+ InfluenceStart.GetAllocatedSize() + Morphs.GetAllocatedSize() + Grid.CellStart.GetAllocatedSize() + Grid.CellPoints.GetAllocatedSize();
	for (const FRCLBodyMorph& Morph : Morphs)
	{
		Size += Morph.Vertex.GetAllocatedSize() + Morph.PositionDelta.GetAllocatedSize() + Morph.NormalDelta.GetAllocatedSize();
	}
	return Size;
}

namespace RCLBody
{
	/** Appends one mesh's LOD0 surface, weights and morphs. Game thread. */
	bool AppendMesh(const USkeletalMesh& Mesh, const FRCLSkeletonSnapshot& Target, bool bWithMorphs,
		FRCLBodySnapshot& Out, FString& OutError)
	{
		FSkeletalMeshRenderData* RenderData = Mesh.GetResourceForRendering();
		if (!RenderData || RenderData->LODRenderData.Num() == 0)
		{
			OutError = FString::Printf(TEXT("'%s' has no render data."), *Mesh.GetName());
			return false;
		}
		FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
		FPositionVertexBuffer& PositionBuffer = LOD.StaticVertexBuffers.PositionVertexBuffer;
		FStaticMeshVertexBuffer& TangentBuffer = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
		FSkinWeightVertexBuffer& WeightBuffer = LOD.SkinWeightVertexBuffer;
		const int32 NumVertices = (int32)PositionBuffer.GetNumVertices();

		// CPU copies are kept by default in 4.19 (r.FreeSkeletalMeshBuffers=0).
		if (NumVertices == 0 || !PositionBuffer.GetVertexData() || !TangentBuffer.GetTangentData()
			|| (int32)WeightBuffer.GetNumVertices() != NumVertices || WeightBuffer.GetVertexDataSize() == 0)
		{
			OutError = FString::Printf(TEXT("'%s' has no CPU copy of its vertices (is r.FreeSkeletalMeshBuffers set?)."), *Mesh.GetName());
			return false;
		}

		// Mesh bone -> target bone, by name (identity for the body itself).
		const FReferenceSkeleton& RefSkeleton = Mesh.RefSkeleton;
		TArray<int32> TargetOfMeshBone;
		TargetOfMeshBone.SetNumUninitialized(RefSkeleton.GetRawBoneNum());
		for (int32 Bone = 0; Bone < RefSkeleton.GetRawBoneNum(); ++Bone)
		{
			TargetOfMeshBone[Bone] = Target.FindBoneIndex(RefSkeleton.GetBoneName(Bone));
		}

		const int32 First = Out.Positions.Num();
		Out.Positions.Reserve(First + NumVertices);
		Out.Normals.Reserve(First + NumVertices);
		for (int32 V = 0; V < NumVertices; ++V)
		{
			Out.Positions.Add(PositionBuffer.VertexPosition(V));
			Out.Normals.Add(TangentBuffer.VertexTangentZ(V).GetSafeNormal());
		}

		// Weights, section by section (bone indices are section-local through BoneMap).
		const bool bExtra = WeightBuffer.HasExtraBoneInfluences();
		const int32 NumSlots = bExtra ? MAX_TOTAL_INFLUENCES : MAX_INFLUENCES_PER_STREAM;
		TArray<int32> SectionOfVertex;
		SectionOfVertex.Init(INDEX_NONE, NumVertices);
		for (int32 S = 0; S < LOD.RenderSections.Num(); ++S)
		{
			const FSkelMeshRenderSection& Section = LOD.RenderSections[S];
			for (uint32 V = Section.BaseVertexIndex; V < Section.BaseVertexIndex + Section.NumVertices && (int32)V < NumVertices; ++V)
			{
				SectionOfVertex[V] = S;
			}
		}
		if (Out.InfluenceStart.Num() == 0)
		{
			Out.InfluenceStart.Add(0);
		}
		int32 NumUnmapped = 0;
		for (int32 V = 0; V < NumVertices; ++V)
		{
			const int32 S = SectionOfVertex[V];
			if (S != INDEX_NONE)
			{
				const TArray<FBoneIndexType>& BoneMap = LOD.RenderSections[S].BoneMap;
				for (int32 Slot = 0; Slot < NumSlots; ++Slot)
				{
					const uint8 LocalBone = bExtra ? WeightBuffer.GetSkinWeightPtr<true>(V)->InfluenceBones[Slot] : WeightBuffer.GetSkinWeightPtr<false>(V)->InfluenceBones[Slot];
					const uint8 Weight = bExtra ? WeightBuffer.GetSkinWeightPtr<true>(V)->InfluenceWeights[Slot] : WeightBuffer.GetSkinWeightPtr<false>(V)->InfluenceWeights[Slot];
					if (Weight == 0 || LocalBone >= BoneMap.Num())
					{
						continue;
					}
					const int32 MeshBone = BoneMap[LocalBone];
					const int32 TargetBone = TargetOfMeshBone.IsValidIndex(MeshBone) ? TargetOfMeshBone[MeshBone] : INDEX_NONE;
					if (TargetBone == INDEX_NONE)
					{
						++NumUnmapped;
						continue;
					}
					FRCLInfluence Influence;
					Influence.JointIndex = TargetBone;
					Influence.Weight = Weight / 255.0f;
					Out.Influences.Add(Influence);
				}
			}
			Out.InfluenceStart.Add(Out.Influences.Num());
		}
		if (NumUnmapped > 0)
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("Body snapshot: %d influence(s) in '%s' use bones the target skeleton lacks and were skipped."), NumUnmapped, *Mesh.GetName());
		}

		if (bWithMorphs)
		{
			for (UMorphTarget* Morph : Mesh.MorphTargets)
			{
				if (!Morph || Morph->MorphLODModels.Num() == 0)
				{
					continue;
				}
				const FMorphTargetLODModel& Model = Morph->MorphLODModels[0];
				FRCLBodyMorph& Copy = Out.Morphs[Out.Morphs.AddDefaulted()];
				Copy.Name = Morph->GetFName();
				Copy.Vertex.Reserve(Model.Vertices.Num());
				Copy.PositionDelta.Reserve(Model.Vertices.Num());
				Copy.NormalDelta.Reserve(Model.Vertices.Num());
				for (const FMorphTargetDelta& Delta : Model.Vertices)
				{
					if ((int32)Delta.SourceIdx < NumVertices)
					{
						Copy.Vertex.Add(First + (int32)Delta.SourceIdx);
						Copy.PositionDelta.Add(FRCLHalfVector(Delta.PositionDelta));
						Copy.NormalDelta.Add(FRCLHalfVector(Delta.TangentZDelta));
					}
				}
			}
		}
		return true;
	}
}

bool FRCLBodySnapshot::Capture(const USkeletalMesh& Body, const TArray<USkeletalMesh*>& Attachments,
	const FRCLSkeletonSnapshot& Target, bool bWithMorphs, FRCLBodySnapshot& OutSnapshot, FString& OutError)
{
	check(IsInGameThread());
	OutSnapshot = FRCLBodySnapshot();
	OutSnapshot.SourceName = Body.GetName();

	if (!RCLBody::AppendMesh(Body, Target, bWithMorphs, OutSnapshot, OutError))
	{
		return false;
	}
	for (const USkeletalMesh* Attachment : Attachments)
	{
		if (!Attachment)
		{
			continue;
		}
		OutSnapshot.SourceName += TEXT(" + ") + Attachment->GetName();
		if (!RCLBody::AppendMesh(*Attachment, Target, bWithMorphs, OutSnapshot, OutError))
		{
			return false;
		}
	}

	OutSnapshot.Grid.Build(OutSnapshot.Positions, 2.0f);
	return true;
}

// ---- Cache --------------------------------------------------------------------------------------

FRCLBodySnapshotCache& FRCLBodySnapshotCache::Get()
{
	static FRCLBodySnapshotCache Instance;
	return Instance;
}

TSharedPtr<const FRCLBodySnapshot, ESPMode::ThreadSafe> FRCLBodySnapshotCache::FindOrCapture(const USkeletalMesh& Body,
	const TArray<USkeletalMesh*>& Attachments, bool bWithMorphs, FString& OutError)
{
	check(IsInGameThread());
	FString Key = Body.GetPathName();
	for (const USkeletalMesh* Attachment : Attachments)
	{
		Key += TEXT("|") + (Attachment ? Attachment->GetPathName() : FString());
	}
	Key += bWithMorphs ? TEXT("|morphs") : TEXT("|nomorphs");

	if (const TSharedPtr<const FRCLBodySnapshot, ESPMode::ThreadSafe>* Found = Snapshots.Find(Key))
	{
		return *Found;
	}

	const double Start = FPlatformTime::Seconds();
	TSharedPtr<FRCLBodySnapshot, ESPMode::ThreadSafe> Snapshot = MakeShared<FRCLBodySnapshot, ESPMode::ThreadSafe>();
	const FRCLSkeletonSnapshot Target = FRCLSkeletonSnapshot::FromSkeletalMesh(Body);
	if (!FRCLBodySnapshot::Capture(Body, Attachments, Target, bWithMorphs, *Snapshot, OutError))
	{
		UE_LOG(LogRuntimeClothing, Warning, TEXT("Body snapshot of '%s' failed: %s"), *Body.GetName(), *OutError);
		return nullptr;
	}
	UE_LOG(LogRuntimeClothing, Log, TEXT("Body snapshot of '%s': %d vertices, %d morph(s), %.1f MB, %.1f ms (game thread, once per body)."),
		*Snapshot->SourceName, Snapshot->NumVertices(), Snapshot->Morphs.Num(), Snapshot->GetAllocatedSize() / (1024.0 * 1024.0),
		(FPlatformTime::Seconds() - Start) * 1000.0);

	TSharedPtr<const FRCLBodySnapshot, ESPMode::ThreadSafe> Shared = Snapshot;
	Snapshots.Add(Key, Shared);
	return Shared;
}

void FRCLBodySnapshotCache::Reset()
{
	Snapshots.Reset();
}
