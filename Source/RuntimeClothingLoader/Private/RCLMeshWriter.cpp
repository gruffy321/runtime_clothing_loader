// Runtime Clothing Loader - UE 4.19.2
// Milestone 6: resource writer. See RCLMeshWriter.h for the contract and the engine-API audit.

#include "RCLMeshWriter.h"
#include "RuntimeClothingLoader.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Animation/MorphTarget.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#if WITH_EDITOR
#include "Rendering/SkeletalMeshLODModel.h"   // FSoftSkinVertex (editor-only type)
#endif

FRCLWriteSettings FRCLWriteSettings::FromLoadOptions(const FRCLLoadOptions& Options, USkeleton* Skeleton)
{
	FRCLWriteSettings Settings;
	Settings.Skeleton = Skeleton;
	Settings.MaterialOverrides = Options.MaterialOverrides;
	Settings.DefaultMaterial = Options.DefaultMaterial;
	Settings.bAllowCPUAccess = Options.bEnablePerPolyCollision;
	Settings.bUseFullPrecisionUVs = Options.bUseFullPrecisionUVs;
	return Settings;
}

namespace RCLWrite
{
	/** Object names may not contain spaces, dots or other separators. */
	FString SanitiseObjectName(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			const TCHAR Char = In[Index];
			Out.AppendChar(FChar::IsAlnum(Char) ? Char : TEXT('_'));
		}
		return Out.IsEmpty() ? FString(TEXT("Garment")) : Out;
	}

	/** Copies built weights (section-local bones, sum 255) into the engine's packed layout. */
	template <bool bExtra>
	void CopyWeights(const FRCLBuiltSkinWeight& Src, TSkinWeightInfo<bExtra>& Dst)
	{
		for (int32 Slot = 0; Slot < TSkinWeightInfo<bExtra>::NumInfluences; ++Slot)
		{
			Dst.InfluenceBones[Slot] = Src.Bones[Slot];
			Dst.InfluenceWeights[Slot] = Src.Weights[Slot];
		}
	}
}

USkeletalMesh* FRCLMeshWriter::CreateMeshObject(const FRCLBuiltMesh& Built, const FRCLWriteSettings& Settings)
{
	UObject* Outer = Settings.Outer ? Settings.Outer : GetTransientPackage();
	const FName BaseName = Settings.MeshName != NAME_None
		? Settings.MeshName
		: FName(*(TEXT("RCL_") + RCLWrite::SanitiseObjectName(Built.Name)));
	const FName UniqueName = MakeUniqueObjectName(Outer, USkeletalMesh::StaticClass(), BaseName);

	// RF_Transient: never saved, so nothing derived from the body (Milestone 8) can reach disk.
	USkeletalMesh* Mesh = NewObject<USkeletalMesh>(Outer, UniqueName, RF_Transient);

	Mesh->Skeleton = Settings.Skeleton;
	Mesh->bHasVertexColors = Built.Colors.Num() > 0;
	Mesh->bUseFullPrecisionUVs = Settings.bUseFullPrecisionUVs;
	Mesh->bEnablePerPolyCollision = false;   // sign-off decision 9; see FRCLLoadOptions

	FSkeletalMeshLODInfo LODInfo;
	LODInfo.ScreenSize = 1.0f;               // single LOD: always shown
	LODInfo.LODHysteresis = 0.02f;           // editor import default
	LODInfo.bAllowCPUAccess = Settings.bAllowCPUAccess;
	Mesh->LODInfo.Add(LODInfo);

	return Mesh;
}

bool FRCLMeshWriter::BuildReferenceSkeleton(USkeletalMesh& Mesh, const FRCLBindResult& Bound,
	const FRCLWriteSettings& Settings, FString& OutError)
{
	const FReferenceSkeleton& SkeletonRef = Settings.Skeleton->GetReferenceSkeleton();

	// Every garment bone must be in the skeleton with the same parent, or master pose would
	// mis-map it. The binder guarantees this against the body mesh; this guards the case where the
	// body mesh and the skeleton passed in disagree.
	for (int32 Bone = 0; Bone < Bound.BoneNames.Num(); ++Bone)
	{
		const int32 SkeletonIndex = SkeletonRef.FindBoneIndex(Bound.BoneNames[Bone]);
		if (SkeletonIndex == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("Bone '%s' is not in skeleton '%s'."),
				*Bound.BoneNames[Bone].ToString(), *Settings.Skeleton->GetName());
			return false;
		}
		const int32 Parent = Bound.ParentIndices[Bone];
		const int32 SkeletonParent = SkeletonRef.GetParentIndex(SkeletonIndex);
		const FName ExpectedParent = SkeletonParent != INDEX_NONE ? SkeletonRef.GetBoneName(SkeletonParent) : NAME_None;
		const FName ActualParent = Parent != INDEX_NONE ? Bound.BoneNames[Parent] : NAME_None;
		if (ExpectedParent != ActualParent)
		{
			OutError = FString::Printf(TEXT("Bone '%s' has parent '%s' in the garment but '%s' in skeleton '%s'."),
				*Bound.BoneNames[Bone].ToString(), *ActualParent.ToString(), *ExpectedParent.ToString(), *Settings.Skeleton->GetName());
			return false;
		}
	}

	{
		// The modifier rebuilds the final reference skeleton (including the skeleton's virtual
		// bones, appended after the raw bones so raw indices and BoneMaps stay valid) when it goes
		// out of scope.
		FReferenceSkeletonModifier Modifier(Mesh.RefSkeleton, Settings.Skeleton);
		for (int32 Bone = 0; Bone < Bound.BoneNames.Num(); ++Bone)
		{
			const FString ExportName = Bound.BoneNames[Bone].ToString();
			Modifier.Add(FMeshBoneInfo(Bound.BoneNames[Bone], ExportName, Bound.ParentIndices[Bone]), Bound.RefPoseLocal[Bone]);
		}
	}

	Mesh.CalculateInvRefMatrices();
	return true;
}

void FRCLMeshWriter::AssignMaterials(USkeletalMesh& Mesh, const FRCLBuiltMesh& Built, const FRCLImportData& Data,
	const FRCLWriteSettings& Settings)
{
	UMaterialInterface* Fallback = Settings.DefaultMaterial
		? Settings.DefaultMaterial
		: UMaterial::GetDefaultMaterial(MD_Surface);

	for (int32 Slot = 0; Slot < Built.MaterialSlots.Num(); ++Slot)
	{
		UMaterialInterface* Material = Settings.MaterialOverrides.IsValidIndex(Slot) && Settings.MaterialOverrides[Slot]
			? Settings.MaterialOverrides[Slot]
			: Fallback;

		const int32 FileMaterial = Built.MaterialSlots[Slot];
		const FName SlotName = Data.Materials.IsValidIndex(FileMaterial) && Data.Materials[FileMaterial].Name != NAME_None
			? Data.Materials[FileMaterial].Name
			: FName(*FString::Printf(TEXT("Slot%d"), Slot));

		Mesh.Materials.Add(FSkeletalMaterial(Material, /*bEnableShadowCasting*/ true, /*bRecomputeTangent*/ false, SlotName, SlotName));
	}
}

void FRCLMeshWriter::FillSections(FSkeletalMeshLODRenderData& LOD, const FRCLBuiltMesh& Built)
{
	const TMap<int32, TArray<int32>> NoOverlappingVertices;

	LOD.RenderSections.Reserve(Built.Sections.Num());
	for (const FRCLBuiltSection& Source : Built.Sections)
	{
		FSkelMeshRenderSection& Section = LOD.RenderSections[LOD.RenderSections.AddDefaulted()];
		Section.MaterialIndex = (uint16)Source.MaterialSlot;
		Section.BaseIndex = Source.BaseIndex;
		Section.NumTriangles = Source.NumTriangles;
		Section.bRecomputeTangent = false;
		Section.bCastShadow = true;
		Section.BaseVertexIndex = Source.BaseVertexIndex;
		Section.NumVertices = Source.NumVertices;
		Section.BoneMap = Source.BoneMap;
		Section.MaxBoneInfluences = Source.MaxBoneInfluences;
		Section.CorrespondClothAssetIndex = INDEX_NONE;
		Section.bDisabled = false;

		// Required on SM5 (FSkeletalMeshLODRenderData::InitResources check()s it). No overlapping
		// vertices gives a one-entry dummy buffer, exactly what the engine builds in that case.
		Section.DuplicatedVerticesBuffer.Init(Source.NumVertices, NoOverlappingVertices);
	}
}

void FRCLMeshWriter::FillVertexBuffers(FSkeletalMeshLODRenderData& LOD, const FRCLBuiltMesh& Built,
	const FRCLWriteSettings& Settings)
{
	const uint32 NumVertices = (uint32)Built.NumVertices();
	const uint32 NumTexCoords = (uint32)Built.NumTexCoords();
	FStaticMeshVertexBuffers& Buffers = LOD.StaticVertexBuffers;

	Buffers.PositionVertexBuffer.Init(NumVertices, Settings.bAllowCPUAccess);

	// UV precision must be chosen before Init sizes the buffer.
	Buffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(Settings.bUseFullPrecisionUVs);
	Buffers.StaticMeshVertexBuffer.Init(NumVertices, NumTexCoords, Settings.bAllowCPUAccess);

	for (uint32 V = 0; V < NumVertices; ++V)
	{
		Buffers.PositionVertexBuffer.VertexPosition(V) = Built.Positions[V];

		const FVector TangentX = Built.TangentX[V];
		const FVector4& Z4 = Built.TangentZ[V];
		const FVector TangentZ(Z4.X, Z4.Y, Z4.Z);
		// SetVertexTangents stores W = GetBasisDeterminantSign(X, Y, Z), which equals Z4.W when
		// Y is rebuilt with the engine's own formula.
		const FVector TangentY = (TangentZ ^ TangentX) * Z4.W;
		Buffers.StaticMeshVertexBuffer.SetVertexTangents(V, TangentX, TangentY, TangentZ);

		for (uint32 Channel = 0; Channel < NumTexCoords; ++Channel)
		{
			Buffers.StaticMeshVertexBuffer.SetVertexUV(V, Channel, Built.UVs[Channel][V]);
		}
	}

	// Only meshes with colours get a colour buffer; InitResources skips it otherwise
	// (USkeletalMesh::bHasVertexColors, set in CreateMeshObject).
	if (Built.Colors.Num() == (int32)NumVertices && NumVertices > 0)
	{
		Buffers.ColorVertexBuffer.InitFromColorArray(Built.Colors.GetData(), NumVertices);
	}
}

void FRCLMeshWriter::FillSkinWeights(FSkeletalMeshLODRenderData& LOD, const FRCLBuiltMesh& Built,
	const FRCLWriteSettings& Settings)
{
	const int32 NumVertices = Built.NumVertices();
	const bool bExtraInfluences = Built.MaxBoneInfluences > MAX_INFLUENCES_PER_STREAM;

	FSkinWeightVertexBuffer& Weights = LOD.SkinWeightVertexBuffer;
	Weights.SetNeedsCPUAccess(Settings.bAllowCPUAccess);
	Weights.SetHasExtraBoneInfluences(bExtraInfluences);

#if WITH_EDITOR
	// Editor (modular build): Init(TArray<FSoftSkinVertex>) is the exported path. Only the bones
	// and weights are read from the vertices.
	TArray<FSoftSkinVertex> SoftVertices;
	SoftVertices.SetNumZeroed(NumVertices);
	for (int32 V = 0; V < NumVertices; ++V)
	{
		FMemory::Memcpy(SoftVertices[V].InfluenceBones, Built.SkinWeights[V].Bones, sizeof(SoftVertices[V].InfluenceBones));
		FMemory::Memcpy(SoftVertices[V].InfluenceWeights, Built.SkinWeights[V].Weights, sizeof(SoftVertices[V].InfluenceWeights));
	}
	Weights.Init(SoftVertices);
#else
	// Game / Shipping (monolithic build): the templated assignment links because AllocateData is
	// in the same binary.
	if (bExtraInfluences)
	{
		TArray<TSkinWeightInfo<true>> Packed;
		Packed.SetNumUninitialized(NumVertices);
		for (int32 V = 0; V < NumVertices; ++V)
		{
			RCLWrite::CopyWeights<true>(Built.SkinWeights[V], Packed[V]);
		}
		Weights = Packed;
	}
	else
	{
		TArray<TSkinWeightInfo<false>> Packed;
		Packed.SetNumUninitialized(NumVertices);
		for (int32 V = 0; V < NumVertices; ++V)
		{
			RCLWrite::CopyWeights<false>(Built.SkinWeights[V], Packed[V]);
		}
		Weights = Packed;
	}
#endif
}

void FRCLMeshWriter::FillIndexBufferAndBones(FSkeletalMeshLODRenderData& LOD, const FRCLBuiltMesh& Built)
{
	// The engine's rule (BuildFromLODModel): 16-bit below MAX_uint16 vertices.
	const uint8 DataTypeSize = Built.NumVertices() < (int32)MAX_uint16 ? sizeof(uint16) : sizeof(uint32);
	LOD.MultiSizeIndexContainer.RebuildIndexBuffer(DataTypeSize, Built.Indices);

	// No adjacency buffer: only tessellation reads it, and InitResources skips an empty container.
	LOD.ActiveBoneIndices = Built.ActiveBoneIndices;
	LOD.RequiredBones = Built.RequiredBones;
}

void FRCLMeshWriter::CreateMorphTargets(USkeletalMesh& Mesh, const FRCLBuiltMesh& Built)
{
	if (Built.Morphs.Num() == 0)
	{
		return;
	}
	for (const FRCLBuiltMorph& Source : Built.Morphs)
	{
		// Named exactly as the body's morph: master pose hands the body's value to it by name.
		UMorphTarget* Morph = NewObject<UMorphTarget>(&Mesh, Source.Name);
		Morph->BaseSkelMesh = &Mesh;
		FMorphTargetLODModel& Model = Morph->MorphLODModels[Morph->MorphLODModels.AddDefaulted()];
		Model.NumBaseMeshVerts = Built.NumVertices();
		Model.SectionIndices = Source.SectionIndices;
		Model.Vertices.SetNumUninitialized(Source.RenderVertex.Num());
		for (int32 K = 0; K < Source.RenderVertex.Num(); ++K)
		{
			FMorphTargetDelta& Delta = Model.Vertices[K];
			Delta.PositionDelta = Source.PositionDelta[K];
			Delta.TangentZDelta = Source.TangentZDelta[K];
			Delta.SourceIdx = Source.RenderVertex[K];
		}
		Mesh.MorphTargets.Add(Morph);
	}
	// Builds the name -> index map (and registers the names as curves on the skeleton, which already
	// has them from the body). Must run before InitResources, which builds the GPU morph buffers.
	Mesh.InitMorphTargets();
}

void FRCLMeshWriter::FinaliseAndInit(USkeletalMesh& Mesh, FSkeletalMeshLODRenderData* LOD, const FRCLBuiltMesh& Built)
{
	Mesh.AllocateResourceForRendering();
	Mesh.GetResourceForRendering()->LODRenderData.Add(LOD);   // the render data now owns the LOD

	Mesh.SetImportedBounds(Built.Bounds);

	// Starts render-thread initialisation of every buffer (index, position, tangent/UV, skin
	// weights, colours if any, duplicated-vertices per section).
	Mesh.InitResources();
}

USkeletalMesh* FRCLMeshWriter::Create(const FRCLBuiltMesh& Built, const FRCLBindResult& Bound,
	const FRCLImportData& Data, const FRCLWriteSettings& Settings,
	ERCLFailureReason& OutReason, FString& OutError)
{
	OutReason = ERCLFailureReason::None;

	if (!IsInGameThread())
	{
		OutReason = ERCLFailureReason::InternalError;
		OutError = TEXT("The mesh writer must run on the game thread.");
		return nullptr;
	}
	if (!Settings.Skeleton)
	{
		OutReason = ERCLFailureReason::MissingTargetSkeleton;
		OutError = TEXT("No target skeleton was given.");
		return nullptr;
	}
	if (Built.NumVertices() == 0 || Built.Indices.Num() == 0 || Built.Sections.Num() == 0 || Bound.BoneNames.Num() == 0)
	{
		OutReason = ERCLFailureReason::InternalError;
		OutError = FString::Printf(TEXT("Garment '%s' has no render data."), *Built.Name);
		return nullptr;
	}

	USkeletalMesh* Mesh = CreateMeshObject(Built, Settings);

	if (!BuildReferenceSkeleton(*Mesh, Bound, Settings, OutError))
	{
		OutReason = ERCLFailureReason::MissingTargetSkeleton;
		Mesh->MarkPendingKill();   // no render data yet, so nothing to release
		return nullptr;
	}

	AssignMaterials(*Mesh, Built, Data, Settings);

	// Plain C++ object, not exported but with an inline constructor; ownership passes to the
	// mesh's render data in FinaliseAndInit.
	FSkeletalMeshLODRenderData* LOD = new FSkeletalMeshLODRenderData();
	FillSections(*LOD, Built);
	FillVertexBuffers(*LOD, Built, Settings);
	FillSkinWeights(*LOD, Built, Settings);
	FillIndexBufferAndBones(*LOD, Built);

	CreateMorphTargets(*Mesh, Built);
	FinaliseAndInit(*Mesh, LOD, Built);
	return Mesh;
}
