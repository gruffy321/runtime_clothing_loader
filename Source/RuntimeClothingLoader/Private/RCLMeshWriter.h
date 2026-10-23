// Runtime Clothing Loader - UE 4.19.2
// Milestone 6: turns built render data (FRCLBuiltMesh) into a live, renderable USkeletalMesh.
//
// Milestone 6 - header for TD review. No implementation yet.
//
// GAME THREAD ONLY: creates UObjects and starts render-resource initialisation. Expected cost is
// buffer copies only (no decisions are left to make after Milestone 5), so a few milliseconds for
// a 10k-vertex garment.
//
// No engine changes. Every engine call below was checked against the UE4Editor-Engine.dll export
// table of this engine (GameWorks DualQuat 4.19.2) or is defined inline in its headers:
//   USkeletalMesh:   AllocateResourceForRendering, InitResources, CalculateInvRefMatrices,
//                    SetImportedBounds (exported); GetResourceForRendering (inline).
//   FReferenceSkeletonModifier: constructor, Add, destructor (exported).
//   FSkeletalMeshLODRenderData: default constructor (inline, empty). Its own InitResources is NOT
//                    exported, so it is only ever reached through USkeletalMesh::InitResources.
//   Vertex buffers:  FPositionVertexBuffer::Init(TArray<FVector>), FStaticMeshVertexBuffer::Init,
//                    FColorVertexBuffer::InitFromColorArray, FMultiSizeIndexContainer::
//                    RebuildIndexBuffer (exported); SetVertexTangents / SetVertexUV /
//                    SetUseFullPrecisionUVs / SetHasExtraBoneInfluences (inline).
//   Skin weights:    FSkinWeightVertexBuffer::Init(TArray<FSoftSkinVertex>) is exported but exists
//                    only WITH_EDITOR; the templated operator=(TArray<TSkinWeightInfo<>>) needs the
//                    unexported AllocateData. So: editor builds use Init, game/Shipping builds
//                    (monolithic, everything links) use operator=. Both paths are compiled in their
//                    own configuration only, so the Shipping build must be tested separately.
//   FDuplicatedVerticesBuffer: Init is inline. On SM5 the engine check()s that every section has
//                    one, so each section gets one with no overlapping vertices (what the engine
//                    builds for a mesh without overlaps).
//
// GameWorks-specific: USkeletalMesh::IndexBufferRanges (Blast bone hiding) can only be rebuilt
// WITH_EDITOR. It is left empty; the engine checks Num() != 0 before using it, so the only effect
// is that BHM_Dynamic_Index_Buffer bone hiding does nothing on runtime garments.

#pragma once

#include "CoreMinimal.h"
#include "RCLTypes.h"
#include "RCLImportData.h"
#include "RCLSkeletonBinder.h"
#include "RCLMeshBuilder.h"

class UObject;
class USkeletalMesh;
class USkeleton;
class UMaterialInterface;
class FSkeletalMeshLODRenderData;

/** Writer inputs. Holds UObject pointers, so it lives on the game thread only; the caller keeps
 *  them alive for the duration of the call (Skeleton and materials are referenced by the body). */
struct FRCLWriteSettings
{
	/** The body's skeleton. Required. The garment's bones are a subset of it (Milestone 4), so the
	 *  skeleton is never modified. */
	USkeleton* Skeleton = nullptr;

	/** Material per slot, by slot index (FRCLBuiltMesh::MaterialSlots order, which is the file's
	 *  first-use order). Missing or null entries use DefaultMaterial. */
	TArray<UMaterialInterface*> MaterialOverrides;

	/** Fallback material. Null means the engine default surface material, so a garment is never
	 *  invisible for lack of a material. */
	UMaterialInterface* DefaultMaterial = nullptr;

	/** 32-bit UVs instead of 16-bit. Costs 4 bytes per vertex per UV channel; avoids visible texture
	 *  swimming on large or tiled UV layouts. Decision for TD: default true. */
	bool bUseFullPrecisionUVs = true;

	/** Keep CPU copies of the vertex data (FSkeletalMeshLODInfo::bAllowCPUAccess). Needed for
	 *  per-poly collision and CPU-skinned vertex queries; off by default (sign-off decision 9). */
	bool bAllowCPUAccess = false;

	/** Outer for the new mesh. Null means the transient package. The mesh is always RF_Transient:
	 *  it can never be saved, so transferred body data (Milestone 8) never reaches disk. */
	UObject* Outer = nullptr;

	/** Object name; NAME_None makes a unique "RCL_<garment>" name. */
	FName MeshName = NAME_None;

	static FRCLWriteSettings FromLoadOptions(const FRCLLoadOptions& Options, USkeleton* Skeleton);
};

/**
 * Stateless writer. Create runs the private steps in the order listed.
 *
 * Result: a USkeletalMesh with one LOD, ready for USkinnedMeshComponent::SetSkeletalMesh followed
 * by SetMasterPoseComponent(Body). Bones are matched to the body by name, so the garment follows
 * every body animation and pose, and morph curves reach it through the master pose (Milestone 8
 * gives it morph targets to respond to).
 *
 * Lifetime: the caller owns the result through a UPROPERTY (normally the component's SkeletalMesh).
 * When it is garbage-collected, USkeletalMesh::BeginDestroy releases the render resources as for
 * any other mesh; nothing in this plugin needs cleaning up.
 */
class FRCLMeshWriter
{
public:
	/**
	 * Creates the mesh. Returns null (with reason and a player-readable error) for: not on the
	 * game thread, no skeleton, a bone missing from the skeleton, or empty built data.
	 */
	static USkeletalMesh* Create(const FRCLBuiltMesh& Built, const FRCLBindResult& Bound,
		const FRCLImportData& Data, const FRCLWriteSettings& Settings,
		ERCLFailureReason& OutReason, FString& OutError);

private:
	/** NewObject<USkeletalMesh>(Outer, Name, RF_Transient), Skeleton, bHasVertexColors,
	 *  bUseFullPrecisionUVs, one FSkeletalMeshLODInfo (ScreenSize 1, CPU access flag). */
	static USkeletalMesh* CreateMeshObject(const FRCLBuiltMesh& Built, const FRCLWriteSettings& Settings);

	/** RefSkeleton from FRCLBindResult (names, parents, RefPoseLocal) through
	 *  FReferenceSkeletonModifier, then CalculateInvRefMatrices. Checks every bone exists in
	 *  Settings.Skeleton with the same parent, so master pose cannot mis-map. */
	static bool BuildReferenceSkeleton(USkeletalMesh& Mesh, const FRCLBindResult& Bound,
		const FRCLWriteSettings& Settings, FString& OutError);

	/** One FSkeletalMaterial per slot: override, else default, else the engine default material.
	 *  Slot names come from the file's material names (used by Milestone 7 / 9 for lookups). */
	static void AssignMaterials(USkeletalMesh& Mesh, const FRCLBuiltMesh& Built, const FRCLImportData& Data,
		const FRCLWriteSettings& Settings);

	/** FSkelMeshRenderSection per built section: material slot, index and vertex ranges, BoneMap,
	 *  MaxBoneInfluences, shadows on, recompute-tangent off, empty duplicated-vertices buffer. */
	static void FillSections(FSkeletalMeshLODRenderData& LOD, const FRCLBuiltMesh& Built);

	/** Position, tangent (TangentY = (TangentZ ^ TangentX) * W), UV and optional colour buffers. */
	static void FillVertexBuffers(FSkeletalMeshLODRenderData& LOD, const FRCLBuiltMesh& Built,
		const FRCLWriteSettings& Settings);

	/** Skin weight buffer: extra influences if MaxBoneInfluences > 4; editor/game paths as above. */
	static void FillSkinWeights(FSkeletalMeshLODRenderData& LOD, const FRCLBuiltMesh& Built,
		const FRCLWriteSettings& Settings);

	/** 16-bit indices when the vertex count is below MAX_uint16 (the engine's own rule), else
	 *  32-bit; ActiveBoneIndices and RequiredBones. No adjacency buffer: it is only used for
	 *  tessellation, which garment materials do not use. */
	static void FillIndexBufferAndBones(FSkeletalMeshLODRenderData& LOD, const FRCLBuiltMesh& Built);

	/** Milestone 8: one UMorphTarget per built morph (outer = the mesh, named as the body's morph so
	 *  the body's sliders drive it through master pose), registered with USkeletalMesh::InitMorphTargets
	 *  (exported) BEFORE InitResources, which builds the GPU morph buffers from them. */
	static void CreateMorphTargets(USkeletalMesh& Mesh, const FRCLBuiltMesh& Built);

	/** AllocateResourceForRendering, add the LOD, SetImportedBounds, InitResources. */
	static void FinaliseAndInit(USkeletalMesh& Mesh, FSkeletalMeshLODRenderData* LOD, const FRCLBuiltMesh& Built);
};
