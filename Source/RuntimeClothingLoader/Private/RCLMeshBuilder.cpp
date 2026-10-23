// Runtime Clothing Loader - UE 4.19.2
// Milestone 5: mesh builder. See RCLMeshBuilder.h for the contract.

#include "RCLMeshBuilder.h"
#include "RuntimeClothingLoader.h"
#include "Components.h"           // MAX_TEXCOORDS
#include "HAL/PlatformTime.h"

FRCLBuildSettings FRCLBuildSettings::FromLoadOptions(const FRCLLoadOptions& Options, int32 MaxGPUSkinBones)
{
	FRCLBuildSettings Settings;
	Settings.MaxBoneInfluences = Options.MaxBoneInfluences;
	Settings.MaxBonesPerSection = MaxGPUSkinBones;
	Settings.MaxVertices = Options.MaxVertices;
	return Settings;
}

SIZE_T FRCLBuiltMesh::GetAllocatedSize() const
{
	SIZE_T Size = Positions.GetAllocatedSize() + TangentX.GetAllocatedSize() + TangentZ.GetAllocatedSize()
		+ UVs.GetAllocatedSize() + Colors.GetAllocatedSize() + SkinWeights.GetAllocatedSize()
		+ Indices.GetAllocatedSize() + Sections.GetAllocatedSize() + MaterialSlots.GetAllocatedSize()
		+ ActiveBoneIndices.GetAllocatedSize() + RequiredBones.GetAllocatedSize() + SourceVertex.GetAllocatedSize();
	for (const TArray<FVector2D>& Channel : UVs)
	{
		Size += Channel.GetAllocatedSize();
	}
	for (const FRCLBuiltSection& Section : Sections)
	{
		Size += Section.BoneMap.GetAllocatedSize();
	}
	Size += Morphs.GetAllocatedSize();
	for (const FRCLBuiltMorph& Morph : Morphs)
	{
		Size += Morph.RenderVertex.GetAllocatedSize() + Morph.PositionDelta.GetAllocatedSize()
			+ Morph.TangentZDelta.GetAllocatedSize() + Morph.SectionIndices.GetAllocatedSize();
	}
	return Size;
}

namespace RCLBuild
{
	/** Influences of one vertex while weights are being prepared. Garment bone indices. */
	struct FWorkInfluence
	{
		int32 Bone;
		float Weight;
	};

	/** Weight descending, then bone ascending, so the result does not depend on file order. */
	struct FByWeightDescending
	{
		bool operator()(const FWorkInfluence& A, const FWorkInfluence& B) const
		{
			return A.Weight != B.Weight ? A.Weight > B.Weight : A.Bone < B.Bone;
		}
	};

	bool IsFinite(const FVector2D& V)
	{
		return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y);
	}

	FString Percent(float Value01)
	{
		return FString::Printf(TEXT("%.1f%%"), Value01 * 100.0f);
	}
}

bool FRCLMeshBuilder::Validate(const FRCLImportMesh& Mesh, const TArray<FRCLInfluence>& Influences,
	const TArray<uint32>& InfluenceStart, int32 NumBones, ERCLFailureReason& OutReason, FString& OutError)
{
	const int32 NumVertices = Mesh.NumVertices();

	if (NumVertices == 0 || Mesh.Indices.Num() == 0 || Mesh.Indices.Num() % 3 != 0 || Mesh.Sections.Num() == 0)
	{
		OutReason = ERCLFailureReason::NoMeshInFile;
		OutError = FString::Printf(TEXT("Garment '%s' has no triangles."), *Mesh.Name);
		return false;
	}

	// Internal consistency: the parser and binder guarantee these, so a failure here is our bug,
	// but the data came from an untrusted file and is checked before it can index anything.
	OutReason = ERCLFailureReason::InternalError;

	if (Mesh.Normals.Num() != NumVertices || (Mesh.Colors.Num() != 0 && Mesh.Colors.Num() != NumVertices)
		|| (Mesh.Tangents.Num() != 0 && Mesh.Tangents.Num() != NumVertices))
	{
		OutError = FString::Printf(TEXT("Garment '%s': vertex arrays have different lengths."), *Mesh.Name);
		return false;
	}
	for (const TArray<FVector2D>& Channel : Mesh.UVs)
	{
		if (Channel.Num() != NumVertices)
		{
			OutError = FString::Printf(TEXT("Garment '%s': a UV channel has the wrong length."), *Mesh.Name);
			return false;
		}
		for (const FVector2D& UV : Channel)
		{
			if (!RCLBuild::IsFinite(UV))
			{
				OutError = FString::Printf(TEXT("Garment '%s' has invalid UV values."), *Mesh.Name);
				OutReason = ERCLFailureReason::ParseError;
				return false;
			}
		}
	}

	for (int32 V = 0; V < NumVertices; ++V)
	{
		if (Mesh.Positions[V].ContainsNaN() || Mesh.Normals[V].ContainsNaN())
		{
			OutError = FString::Printf(TEXT("Garment '%s' has invalid vertex positions or normals."), *Mesh.Name);
			OutReason = ERCLFailureReason::ParseError;
			return false;
		}
	}

	for (uint32 Index : Mesh.Indices)
	{
		if (Index >= (uint32)NumVertices)
		{
			OutError = FString::Printf(TEXT("Garment '%s': triangle index out of range."), *Mesh.Name);
			return false;
		}
	}
	for (const FRCLImportSection& Section : Mesh.Sections)
	{
		const uint64 End = (uint64)Section.FirstIndex + (uint64)Section.NumTriangles * 3;
		if (Section.FirstIndex % 3 != 0 || End > (uint64)Mesh.Indices.Num() || Section.MaterialIndex < 0)
		{
			OutError = FString::Printf(TEXT("Garment '%s': section out of range."), *Mesh.Name);
			return false;
		}
	}

	if (NumBones <= 0 || NumBones > (int32)MAX_uint16)
	{
		OutError = FString::Printf(TEXT("Garment '%s': skeleton has %d bones."), *Mesh.Name, NumBones);
		return false;
	}

	if (Influences.Num() == 0)
	{
		OutError = FString::Printf(TEXT("Garment '%s' has no skin weights. Unweighted garments need weight transfer, which arrives in Milestone 8."), *Mesh.Name);
		return false;
	}
	if (InfluenceStart.Num() != NumVertices + 1 || InfluenceStart[0] != 0 || InfluenceStart.Last() != (uint32)Influences.Num())
	{
		OutError = FString::Printf(TEXT("Garment '%s': skin weight table does not match the vertices."), *Mesh.Name);
		return false;
	}
	for (int32 V = 0; V < NumVertices; ++V)
	{
		if (InfluenceStart[V] > InfluenceStart[V + 1])
		{
			OutError = FString::Printf(TEXT("Garment '%s': skin weight table is not ordered."), *Mesh.Name);
			return false;
		}
	}
	for (const FRCLInfluence& Influence : Influences)
	{
		if (Influence.JointIndex < 0 || Influence.JointIndex >= NumBones || !FMath::IsFinite(Influence.Weight) || Influence.Weight < 0.0f)
		{
			OutError = FString::Printf(TEXT("Garment '%s': invalid skin weight."), *Mesh.Name);
			return false;
		}
	}

	OutReason = ERCLFailureReason::None;
	return true;
}

void FRCLMeshBuilder::PrepareWeights(const FRCLImportMesh& Mesh, const TArray<FRCLInfluence>& Influences,
	const TArray<uint32>& InfluenceStart, const FRCLBuildSettings& Settings,
	TArray<FRCLBuiltSkinWeight>& OutGarmentWeights, TArray<FBoneIndexType>& OutGarmentBones, FRCLBuildStats& Stats)
{
	// OutGarmentWeights[V].Weights holds the final uint8 weights; the matching garment bone indices
	// (which do not fit in uint8) are in OutGarmentBones[V * MAX_TOTAL_INFLUENCES + Slot] until
	// SplitSections turns them into section-local indices.
	const int32 NumVertices = Mesh.NumVertices();
	const int32 MaxInfluences = FMath::Clamp(Settings.MaxBoneInfluences, 1, (int32)MAX_TOTAL_INFLUENCES);

	OutGarmentWeights.SetNum(NumVertices);
	OutGarmentBones.SetNumZeroed(NumVertices * MAX_TOTAL_INFLUENCES);

	TArray<RCLBuild::FWorkInfluence, TInlineAllocator<32>> Work;

	for (int32 V = 0; V < NumVertices; ++V)
	{
		Work.Reset();
		float Total = 0.0f;
		for (uint32 I = InfluenceStart[V]; I < InfluenceStart[V + 1]; ++I)
		{
			const FRCLInfluence& Influence = Influences[I];
			if (Influence.Weight <= 0.0f)
			{
				continue;
			}
			// The binder merges duplicates, but transferred or rigid weights might not; merge here too.
			bool bMerged = false;
			for (RCLBuild::FWorkInfluence& Existing : Work)
			{
				if (Existing.Bone == Influence.JointIndex)
				{
					Existing.Weight += Influence.Weight;
					bMerged = true;
					break;
				}
			}
			if (!bMerged)
			{
				Work.Add({ Influence.JointIndex, Influence.Weight });
			}
			Total += Influence.Weight;
		}

		FRCLBuiltSkinWeight& Out = OutGarmentWeights[V];
		FBoneIndexType* OutBones = &OutGarmentBones[V * MAX_TOTAL_INFLUENCES];

		if (Work.Num() == 0 || !(Total > 0.0f) || !FMath::IsFinite(Total))
		{
			// No usable weight: follow the root so the vertex at least moves with the character.
			OutBones[0] = 0;
			Out.Weights[0] = 255;
			++Stats.NumVerticesUnweighted;
			continue;
		}

		Work.Sort(RCLBuild::FByWeightDescending());
		Stats.MaxInfluencesInFile = FMath::Max(Stats.MaxInfluencesInFile, Work.Num());

		if (Work.Num() > MaxInfluences)
		{
			++Stats.NumVerticesTrimmed;
			Stats.MaxWeightTrimmed = FMath::Max(Stats.MaxWeightTrimmed, Work[MaxInfluences].Weight / Total);
			Work.SetNum(MaxInfluences, false);
		}

		float Kept = 0.0f;
		for (const RCLBuild::FWorkInfluence& Influence : Work)
		{
			Kept += Influence.Weight;
		}

		// Largest-remainder quantisation: floor everything, then hand the missing units to the
		// largest fractions, so the sum is exactly 255 and no vertex loses weight to rounding.
		int32 Quantised[MAX_TOTAL_INFLUENCES];
		float Fraction[MAX_TOTAL_INFLUENCES];
		int32 Sum = 0;
		for (int32 Slot = 0; Slot < Work.Num(); ++Slot)
		{
			const float Scaled = Work[Slot].Weight / Kept * 255.0f;
			Quantised[Slot] = FMath::Clamp(FMath::FloorToInt(Scaled), 0, 255);
			Fraction[Slot] = Scaled - (float)Quantised[Slot];
			Sum += Quantised[Slot];
		}
		// The missing units equal the sum of the fractions, so there are fewer than Work.Num() of
		// them: each goes to a different slot, largest fraction first.
		int32 Order[MAX_TOTAL_INFLUENCES];
		for (int32 Slot = 0; Slot < Work.Num(); ++Slot)
		{
			Order[Slot] = Slot;
		}
		for (int32 I = 1; I < Work.Num(); ++I)   // insertion sort, at most 8 entries
		{
			for (int32 J = I; J > 0 && Fraction[Order[J]] > Fraction[Order[J - 1]]; --J)
			{
				Swap(Order[J], Order[J - 1]);
			}
		}
		for (int32 Missing = 255 - Sum, I = 0; Missing > 0; --Missing, ++I)
		{
			++Quantised[Order[I % Work.Num()]];   // the wrap only matters with float error
		}

		// Write out, dropping influences that rounded to zero, largest first.
		int32 OutSlot = 0;
		for (int32 Pass = 0; Pass < Work.Num(); ++Pass)
		{
			int32 Best = INDEX_NONE;
			for (int32 Slot = 0; Slot < Work.Num(); ++Slot)
			{
				if (Quantised[Slot] > 0 && (Best == INDEX_NONE || Quantised[Slot] > Quantised[Best]))
				{
					Best = Slot;
				}
			}
			if (Best == INDEX_NONE)
			{
				break;
			}
			OutBones[OutSlot] = (FBoneIndexType)Work[Best].Bone;
			Out.Weights[OutSlot] = (uint8)Quantised[Best];
			Quantised[Best] = 0;
			++OutSlot;
		}
	}
}

void FRCLMeshBuilder::ComputeTangents(const FRCLImportMesh& Mesh, TArray<FVector>& OutTangentX,
	TArray<FVector4>& OutTangentZ, FRCLBuildStats& Stats)
{
	const int32 NumVertices = Mesh.NumVertices();
	TArray<FVector> TangentSum;
	TArray<FVector> BitangentSum;
	TangentSum.SetNumZeroed(NumVertices);
	BitangentSum.SetNumZeroed(NumVertices);

	if (Mesh.UVs.Num() > 0)
	{
		const TArray<FVector2D>& UV = Mesh.UVs[0];
		for (int32 I = 0; I + 2 < Mesh.Indices.Num(); I += 3)
		{
			const uint32 A = Mesh.Indices[I], B = Mesh.Indices[I + 1], C = Mesh.Indices[I + 2];
			const FVector E1 = Mesh.Positions[B] - Mesh.Positions[A];
			const FVector E2 = Mesh.Positions[C] - Mesh.Positions[A];
			const FVector2D D1 = UV[B] - UV[A];
			const FVector2D D2 = UV[C] - UV[A];

			const float Det = D1.X * D2.Y - D2.X * D1.Y;
			const float Area = (E1 ^ E2).Size();   // twice the area; only ratios matter
			if (FMath::Abs(Det) < 1e-12f || Area < 1e-10f)
			{
				continue;
			}

			// dP/du and dP/dv. V was flipped by the parser, so this is the engine's UV space and
			// dP/dv is the engine's TangentY direction (DirectX-style normal maps).
			const FVector Tangent = ((E1 * D2.Y - E2 * D1.Y) / Det).GetSafeNormal();
			const FVector Bitangent = ((E2 * D1.X - E1 * D2.X) / Det).GetSafeNormal();

			for (uint32 Corner : { A, B, C })
			{
				TangentSum[Corner] += Tangent * Area;
				BitangentSum[Corner] += Bitangent * Area;
			}
		}
	}

	OutTangentX.SetNumUninitialized(NumVertices);
	OutTangentZ.SetNumUninitialized(NumVertices);

	for (int32 V = 0; V < NumVertices; ++V)
	{
		FVector Normal = Mesh.Normals[V].GetSafeNormal();
		if (Normal.IsZero())
		{
			Normal = FVector::UpVector;
		}

		// Gram-Schmidt: make the tangent perpendicular to the normal.
		FVector Tangent = TangentSum[V] - Normal * (Normal | TangentSum[V]);
		float Sign = 1.0f;
		if (Tangent.SizeSquared() > 1e-12f)
		{
			Tangent.Normalize();
			// Engine rebuilds TangentY = (TangentZ ^ TangentX) * W, so W is the side BitangentSum is on.
			Sign = ((Normal ^ Tangent) | BitangentSum[V]) < 0.0f ? -1.0f : 1.0f;
		}
		else
		{
			FVector Unused;
			Normal.FindBestAxisVectors(Tangent, Unused);
			Tangent = (Tangent - Normal * (Normal | Tangent)).GetSafeNormal();
			++Stats.NumDegenerateTangents;
		}

		OutTangentX[V] = Tangent;
		OutTangentZ[V] = FVector4(Normal, Sign);
	}

	Stats.bTangentsComputed = true;
}

void FRCLMeshBuilder::SplitSections(const FRCLImportMesh& Mesh, const TArray<FRCLBuiltSkinWeight>& GarmentWeights,
	const TArray<FBoneIndexType>& GarmentBones, const FRCLBuildSettings& Settings, FRCLBuiltMesh& OutMesh)
{
	// A triangle can bring at most 3 * MAX_TOTAL_INFLUENCES new bones, so a chunk can always take
	// at least one triangle.
	const int32 MaxBones = FMath::Clamp(Settings.MaxBonesPerSection, 3 * (int32)MAX_TOTAL_INFLUENCES, 256);
	const int32 NumVertices = Mesh.NumVertices();

	int32 NumBones = 0;
	for (FBoneIndexType Bone : GarmentBones)
	{
		NumBones = FMath::Max(NumBones, (int32)Bone + 1);
	}

	// Stamped lookups, so nothing needs clearing between chunks.
	TArray<int32> VertexStamp, RenderIndexOfVertex, BoneStamp, LocalIndexOfBone;
	VertexStamp.Init(INDEX_NONE, NumVertices);
	RenderIndexOfVertex.SetNumUninitialized(NumVertices);
	BoneStamp.Init(INDEX_NONE, NumBones);
	LocalIndexOfBone.SetNumUninitialized(NumBones);

	TArray<FBoneIndexType> ChunkBones;
	TArray<uint32> ChunkTriangles;   // first index of each triangle in Mesh.Indices
	int32 Chunk = 0;

	auto NumBonesOfVertex = [&GarmentWeights](uint32 V)
	{
		int32 Count = 0;
		while (Count < MAX_TOTAL_INFLUENCES && GarmentWeights[V].Weights[Count] > 0)
		{
			++Count;
		}
		return Count;
	};

	auto EmitChunk = [&](int32 SourceSectionIndex)
	{
		if (ChunkTriangles.Num() == 0)
		{
			return;
		}

		FRCLBuiltSection& Section = OutMesh.Sections[OutMesh.Sections.AddDefaulted()];
		Section.MaterialSlot = Mesh.Sections[SourceSectionIndex].MaterialIndex;   // compacted in Finalise
		Section.SourceSectionIndex = SourceSectionIndex;
		Section.BaseIndex = OutMesh.Indices.Num();
		Section.NumTriangles = ChunkTriangles.Num();
		Section.BaseVertexIndex = OutMesh.SourceVertex.Num();

		ChunkBones.Sort();
		Section.BoneMap = ChunkBones;
		for (int32 Local = 0; Local < ChunkBones.Num(); ++Local)
		{
			LocalIndexOfBone[ChunkBones[Local]] = Local;
		}

		for (uint32 First : ChunkTriangles)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 V = Mesh.Indices[First + Corner];
				if (VertexStamp[V] != Chunk)
				{
					VertexStamp[V] = Chunk;
					RenderIndexOfVertex[V] = OutMesh.SourceVertex.Add((int32)V);

					FRCLBuiltSkinWeight Weight = GarmentWeights[V];
					const int32 Count = NumBonesOfVertex(V);
					for (int32 Slot = 0; Slot < Count; ++Slot)
					{
						Weight.Bones[Slot] = (uint8)LocalIndexOfBone[GarmentBones[V * MAX_TOTAL_INFLUENCES + Slot]];
					}
					OutMesh.SkinWeights.Add(Weight);
					Section.MaxBoneInfluences = FMath::Max(Section.MaxBoneInfluences, Count);
				}
				OutMesh.Indices.Add((uint32)RenderIndexOfVertex[V]);
			}
		}

		Section.NumVertices = OutMesh.SourceVertex.Num() - Section.BaseVertexIndex;
		ChunkTriangles.Reset();
		ChunkBones.Reset();
		++Chunk;
	};

	for (int32 SectionIndex = 0; SectionIndex < Mesh.Sections.Num(); ++SectionIndex)
	{
		const FRCLImportSection& Source = Mesh.Sections[SectionIndex];
		const int32 FirstChunk = Chunk;

		for (uint32 T = 0; T < Source.NumTriangles; ++T)
		{
			const uint32 First = Source.FirstIndex + T * 3;

			// Bones this triangle adds to the current chunk.
			FBoneIndexType NewBones[3 * MAX_TOTAL_INFLUENCES];
			int32 NumNew = 0;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 V = Mesh.Indices[First + Corner];
				const int32 Count = NumBonesOfVertex(V);
				for (int32 Slot = 0; Slot < Count; ++Slot)
				{
					const FBoneIndexType Bone = GarmentBones[V * MAX_TOTAL_INFLUENCES + Slot];
					if (BoneStamp[Bone] != Chunk)
					{
						bool bSeen = false;
						for (int32 N = 0; N < NumNew; ++N)
						{
							bSeen |= NewBones[N] == Bone;
						}
						if (!bSeen)
						{
							NewBones[NumNew++] = Bone;
						}
					}
				}
			}

			if (ChunkBones.Num() + NumNew > MaxBones)
			{
				EmitChunk(SectionIndex);
				// New chunk: every bone of this triangle is new again.
				NumNew = 0;
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const uint32 V = Mesh.Indices[First + Corner];
					const int32 Count = NumBonesOfVertex(V);
					for (int32 Slot = 0; Slot < Count; ++Slot)
					{
						const FBoneIndexType Bone = GarmentBones[V * MAX_TOTAL_INFLUENCES + Slot];
						bool bSeen = false;
						for (int32 N = 0; N < NumNew; ++N)
						{
							bSeen |= NewBones[N] == Bone;
						}
						if (!bSeen)
						{
							NewBones[NumNew++] = Bone;
						}
					}
				}
			}

			for (int32 N = 0; N < NumNew; ++N)
			{
				BoneStamp[NewBones[N]] = Chunk;
				ChunkBones.Add(NewBones[N]);
			}
			ChunkTriangles.Add(First);
		}

		EmitChunk(SectionIndex);
		if (Chunk - FirstChunk > 1)
		{
			++OutMesh.Stats.NumSectionsSplitForBones;
		}
	}
}

void FRCLMeshBuilder::EmitVertexStreams(const FRCLImportMesh& Mesh, const TArray<FVector>& TangentX,
	const TArray<FVector4>& TangentZ, FRCLBuiltMesh& OutMesh)
{
	const int32 NumRender = OutMesh.SourceVertex.Num();
	const int32 NumTexCoords = FMath::Clamp(Mesh.UVs.Num(), 1, (int32)MAX_TEXCOORDS);
	const bool bHasColors = Mesh.Colors.Num() == Mesh.NumVertices();

	if (Mesh.UVs.Num() > MAX_TEXCOORDS)
	{
		OutMesh.Warnings.Add(FString::Printf(TEXT("Garment '%s' has %d UV channels; skeletal meshes use the first %d."),
			*Mesh.Name, Mesh.UVs.Num(), (int32)MAX_TEXCOORDS));
	}

	OutMesh.Positions.SetNumUninitialized(NumRender);
	OutMesh.TangentX.SetNumUninitialized(NumRender);
	OutMesh.TangentZ.SetNumUninitialized(NumRender);
	OutMesh.UVs.SetNum(NumTexCoords);
	for (int32 Channel = 0; Channel < NumTexCoords; ++Channel)
	{
		if (Channel < Mesh.UVs.Num())
		{
			OutMesh.UVs[Channel].SetNumUninitialized(NumRender);
		}
		else
		{
			OutMesh.UVs[Channel].SetNumZeroed(NumRender);   // file had no UVs
		}
	}
	if (bHasColors)
	{
		OutMesh.Colors.SetNumUninitialized(NumRender);
	}

	for (int32 R = 0; R < NumRender; ++R)
	{
		const int32 V = OutMesh.SourceVertex[R];
		OutMesh.Positions[R] = Mesh.Positions[V];
		OutMesh.TangentX[R] = TangentX[V];
		OutMesh.TangentZ[R] = TangentZ[V];
		for (int32 Channel = 0; Channel < NumTexCoords && Channel < Mesh.UVs.Num(); ++Channel)
		{
			OutMesh.UVs[Channel][R] = Mesh.UVs[Channel][V];
		}
		if (bHasColors)
		{
			OutMesh.Colors[R] = Mesh.Colors[V];
		}
	}
}

void FRCLMeshBuilder::Finalise(const FRCLImportMesh& Mesh, const TArray<int32>& BoneParents, FRCLBuiltMesh& OutMesh)
{
	// Material slots: file material indices in first-use order.
	for (FRCLBuiltSection& Section : OutMesh.Sections)
	{
		Section.MaterialSlot = OutMesh.MaterialSlots.AddUnique(Section.MaterialSlot);
		OutMesh.MaxBoneInfluences = FMath::Max(OutMesh.MaxBoneInfluences, Section.MaxBoneInfluences);
	}

	// Bones in use plus every ancestor, ascending (parents come first in the garment skeleton).
	TArray<bool> bActive;
	bActive.Init(false, BoneParents.Num());
	for (const FRCLBuiltSection& Section : OutMesh.Sections)
	{
		for (FBoneIndexType Bone : Section.BoneMap)
		{
			for (int32 B = Bone; B != INDEX_NONE && !bActive[B]; B = BoneParents[B])
			{
				bActive[B] = true;
			}
		}
	}
	for (int32 B = 0; B < bActive.Num(); ++B)
	{
		if (bActive[B])
		{
			OutMesh.ActiveBoneIndices.Add((FBoneIndexType)B);
		}
	}
	OutMesh.RequiredBones = OutMesh.ActiveBoneIndices;

	OutMesh.Bounds = FBoxSphereBounds(OutMesh.Positions.GetData(), (uint32)OutMesh.Positions.Num());

	FRCLBuildStats& Stats = OutMesh.Stats;
	Stats.NumSourceVertices = Mesh.NumVertices();
	Stats.NumRenderVertices = OutMesh.NumVertices();
	Stats.NumTriangles = OutMesh.Indices.Num() / 3;
	Stats.NumSections = OutMesh.Sections.Num();
	// The engine's own rule (FSkeletalMeshLODRenderData::BuildFromLODModel): 16-bit below MAX_uint16.
	Stats.bUse32BitIndices = OutMesh.NumVertices() >= (int32)MAX_uint16;
}

bool FRCLMeshBuilder::Build(const FRCLImportMesh& Mesh, const TArray<FRCLInfluence>& Influences,
	const TArray<uint32>& InfluenceStart, const TArray<int32>& BoneParents, const FRCLBuildSettings& Settings,
	FRCLBuiltMesh& OutMesh, ERCLFailureReason& OutReason, FString& OutError)
{
	const double Start = FPlatformTime::Seconds();
	OutMesh = FRCLBuiltMesh();
	OutMesh.Name = Mesh.Name;
	OutReason = ERCLFailureReason::None;

	for (int32 B = 0; B < BoneParents.Num(); ++B)
	{
		if (BoneParents[B] >= B || (BoneParents[B] < 0 && BoneParents[B] != INDEX_NONE))
		{
			OutReason = ERCLFailureReason::InternalError;
			OutError = FString::Printf(TEXT("Garment '%s': skeleton is not parent-first."), *Mesh.Name);
			return false;
		}
	}

	if (!Validate(Mesh, Influences, InfluenceStart, BoneParents.Num(), OutReason, OutError))
	{
		return false;
	}

	TArray<FRCLBuiltSkinWeight> GarmentWeights;
	TArray<FBoneIndexType> GarmentBones;
	PrepareWeights(Mesh, Influences, InfluenceStart, Settings, GarmentWeights, GarmentBones, OutMesh.Stats);

	TArray<FVector> TangentX;
	TArray<FVector4> TangentZ;
	if (Settings.bRecomputeTangents || Mesh.Tangents.Num() != Mesh.NumVertices())
	{
		ComputeTangents(Mesh, TangentX, TangentZ, OutMesh.Stats);
	}
	else
	{
		TangentX.SetNumUninitialized(Mesh.NumVertices());
		TangentZ.SetNumUninitialized(Mesh.NumVertices());
		for (int32 V = 0; V < Mesh.NumVertices(); ++V)
		{
			const FVector4& T = Mesh.Tangents[V];
			TangentX[V] = FVector(T.X, T.Y, T.Z).GetSafeNormal();
			TangentZ[V] = FVector4(Mesh.Normals[V].GetSafeNormal(), T.W < 0.0f ? -1.0f : 1.0f);
		}
	}

	SplitSections(Mesh, GarmentWeights, GarmentBones, Settings, OutMesh);

	if (OutMesh.SourceVertex.Num() > Settings.MaxVertices)
	{
		OutReason = ERCLFailureReason::TooManyVertices;
		OutError = FString::Printf(TEXT("Garment '%s' needs %d vertices; the limit is %d."),
			*Mesh.Name, OutMesh.SourceVertex.Num(), Settings.MaxVertices);
		OutMesh = FRCLBuiltMesh();
		return false;
	}

	EmitVertexStreams(Mesh, TangentX, TangentZ, OutMesh);
	Finalise(Mesh, BoneParents, OutMesh);

	const FRCLBuildStats& Stats = OutMesh.Stats;
	if (Stats.NumVerticesTrimmed > 0)
	{
		OutMesh.Warnings.Add(FString::Printf(TEXT("%d vertices had more than %d bone influences (up to %d); the smallest were dropped (largest dropped weight %s)."),
			Stats.NumVerticesTrimmed, FMath::Clamp(Settings.MaxBoneInfluences, 1, (int32)MAX_TOTAL_INFLUENCES),
			Stats.MaxInfluencesInFile, *RCLBuild::Percent(Stats.MaxWeightTrimmed)));
	}
	if (Stats.NumVerticesUnweighted > 0)
	{
		OutMesh.Warnings.Add(FString::Printf(TEXT("%d vertices had no skin weight and follow the root bone."), Stats.NumVerticesUnweighted));
	}
	if (Stats.NumDegenerateTangents > 0)
	{
		OutMesh.Warnings.Add(FString::Printf(TEXT("%d vertices have no usable UV direction; normal maps may look wrong there."), Stats.NumDegenerateTangents));
	}

	OutMesh.Stats.BuildTimeMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	return true;
}

void FRCLMeshBuilder::BuildMorphs(const TArray<FRCLMorphDeltas>& SourceMorphs, FRCLBuiltMesh& InOutMesh)
{
	InOutMesh.Morphs.Reset();
	if (SourceMorphs.Num() == 0 || InOutMesh.NumVertices() == 0)
	{
		return;
	}

	// Source vertex -> its render copies (the builder duplicates vertices at section boundaries),
	// and render vertex -> section.
	int32 NumSource = 0;
	for (int32 Source : InOutMesh.SourceVertex)
	{
		NumSource = FMath::Max(NumSource, Source + 1);
	}
	TArray<int32> CopyStart, Copies;
	CopyStart.SetNumZeroed(NumSource + 1);
	for (int32 Source : InOutMesh.SourceVertex)
	{
		++CopyStart[Source + 1];
	}
	for (int32 S = 0; S < NumSource; ++S)
	{
		CopyStart[S + 1] += CopyStart[S];
	}
	Copies.SetNumUninitialized(InOutMesh.NumVertices());
	{
		TArray<int32> Fill = CopyStart;
		for (int32 Render = 0; Render < InOutMesh.NumVertices(); ++Render)
		{
			Copies[Fill[InOutMesh.SourceVertex[Render]]++] = Render;
		}
	}
	TArray<int32> SectionOfRender;
	SectionOfRender.Init(0, InOutMesh.NumVertices());
	for (int32 S = 0; S < InOutMesh.Sections.Num(); ++S)
	{
		const FRCLBuiltSection& Section = InOutMesh.Sections[S];
		for (uint32 R = Section.BaseVertexIndex; R < Section.BaseVertexIndex + Section.NumVertices; ++R)
		{
			SectionOfRender[R] = S;
		}
	}

	struct FEntry { uint32 Render; FVector Position; FVector Normal; };
	TArray<FEntry> Entries;
	for (const FRCLMorphDeltas& Source : SourceMorphs)
	{
		Entries.Reset();
		for (int32 K = 0; K < Source.SourceVertex.Num(); ++K)
		{
			const int32 SourceVertex = Source.SourceVertex[K];
			if (SourceVertex < 0 || SourceVertex >= NumSource)
			{
				continue;   // a source vertex with no render copy (not referenced by any triangle)
			}
			for (int32 C = CopyStart[SourceVertex]; C < CopyStart[SourceVertex + 1]; ++C)
			{
				Entries.Add({ (uint32)Copies[C], Source.PositionDelta[K], Source.NormalDelta[K] });
			}
		}
		if (Entries.Num() == 0)
		{
			continue;
		}
		Entries.Sort([](const FEntry& A, const FEntry& B) { return A.Render < B.Render; });

		FRCLBuiltMorph& Morph = InOutMesh.Morphs[InOutMesh.Morphs.AddDefaulted()];
		Morph.Name = Source.Name;
		Morph.RenderVertex.Reserve(Entries.Num());
		Morph.PositionDelta.Reserve(Entries.Num());
		Morph.TangentZDelta.Reserve(Entries.Num());
		for (const FEntry& Entry : Entries)
		{
			Morph.RenderVertex.Add(Entry.Render);
			Morph.PositionDelta.Add(Entry.Position);
			Morph.TangentZDelta.Add(Entry.Normal);
			Morph.SectionIndices.AddUnique(SectionOfRender[Entry.Render]);
		}
		Morph.SectionIndices.Sort();
	}
}
