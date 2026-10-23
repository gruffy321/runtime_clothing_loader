// Runtime Clothing Loader - UE 4.19.2
// Milestone 5: turns one parsed garment plus its bound skin weights into render-ready LOD data.
//
// Milestone 5 - header for TD review. No implementation yet.
//
// Worker thread only. Plain data in, plain data out: no UObjects, no RHI. Milestone 6 (resource
// writer) copies FRCLBuiltMesh into FSkeletalMeshLODRenderData on the game thread, which is then a
// straight copy with no further decisions.
//
// Layout facts this file is built around (checked against the 4.19 engine headers):
//  - FSkelMeshRenderSection: one material, a contiguous index range AND a contiguous vertex range
//    (BaseVertexIndex, NumVertices), plus a BoneMap of up to 256 entries (InfluenceBones is uint8).
//  - TSkinWeightInfo: uint8 bone indices (into the section's BoneMap) and uint8 weights that must
//    sum to 255. 4 influences per vertex, or 8 with bExtraBoneInfluences (MAX_TOTAL_INFLUENCES).
//  - Skeletal meshes use at most MAX_TEXCOORDS (4) UV channels.
//  - Tangent basis: TangentZ.W holds the bitangent sign, TangentY = (TangentZ ^ TangentX) * W.

#pragma once

#include "CoreMinimal.h"
#include "BoneIndices.h"          // FBoneIndexType (uint16)
#include "GPUSkinPublicDefs.h"    // MAX_TOTAL_INFLUENCES (8), MAX_INFLUENCES_PER_STREAM (4)
#include "RCLTypes.h"
#include "RCLImportData.h"
#include "RCLTransfer.h"      // FRCLMorphDeltas (Milestone 8)

/** Skin weight of one render vertex, already in the engine's packed form. */
struct FRCLBuiltSkinWeight
{
	/** Indices into the owning section's BoneMap. Unused slots are 0 with weight 0. */
	uint8 Bones[MAX_TOTAL_INFLUENCES];

	/** Sum to exactly 255. Sorted largest first. */
	uint8 Weights[MAX_TOTAL_INFLUENCES];

	FRCLBuiltSkinWeight()
	{
		FMemory::Memzero(Bones);
		FMemory::Memzero(Weights);
	}
};

/** One render section: maps 1:1 onto an FSkelMeshRenderSection. */
struct FRCLBuiltSection
{
	/** Index into FRCLBuiltMesh::MaterialSlots (not into the file's materials). */
	int32 MaterialSlot = 0;

	/** Triangles: Indices[BaseIndex .. BaseIndex + NumTriangles * 3). */
	uint32 BaseIndex = 0;
	uint32 NumTriangles = 0;

	/** Vertices: [BaseVertexIndex .. BaseVertexIndex + NumVertices). No other section uses them. */
	uint32 BaseVertexIndex = 0;
	uint32 NumVertices = 0;

	/** Section-local bone -> garment bone (index into FRCLBindResult::BoneNames). Sorted ascending. */
	TArray<FBoneIndexType> BoneMap;

	/** Largest number of non-zero influences on any vertex of this section (1..MaxBoneInfluences). */
	int32 MaxBoneInfluences = 1;

	/** Index of the file section this came from. Several built sections share one when a material
	 *  had to be split because it used more bones than MaxBonesPerSection. */
	int32 SourceSectionIndex = 0;
};

/** Counts for the load report and the RCL.Build console command. */
struct FRCLBuildStats
{
	int32 NumSourceVertices = 0;
	int32 NumRenderVertices = 0;           // after duplication at bone-limit splits
	int32 NumTriangles = 0;
	int32 NumSections = 0;
	int32 NumSectionsSplitForBones = 0;    // file sections that needed more than one chunk

	int32 MaxInfluencesInFile = 0;         // before trimming
	int32 NumVerticesTrimmed = 0;          // had more influences than MaxBoneInfluences
	float MaxWeightTrimmed = 0.0f;         // largest single weight dropped by trimming (0..1)
	int32 NumVerticesUnweighted = 0;       // had no weight at all; bound 100% to the root

	bool bTangentsComputed = false;        // false when the file supplied tangents and we kept them
	int32 NumDegenerateTangents = 0;       // no usable UV gradient; an arbitrary perpendicular was used

	bool bUse32BitIndices = false;         // true from MAX_uint16 (65535) vertices, as the engine decides
	double BuildTimeMs = 0.0;
};

/**
 * Render-ready data for one garment (one FRCLImportMesh). Parallel arrays, one entry per render
 * vertex, ordered section by section so every section's vertices are contiguous.
 */
/** Milestone 8: one morph target on render vertices, ready for a UMorphTarget's LOD 0. */
struct FRCLBuiltMorph
{
	FName Name;
	TArray<uint32> RenderVertex;      // ascending
	TArray<FVector> PositionDelta;
	TArray<FVector> TangentZDelta;    // normal delta
	TArray<int32> SectionIndices;     // sections containing at least one moved vertex
};

struct FRCLBuiltMesh
{
	FString Name;

	// ---- Vertex streams ----------------------------------------------------------------------

	TArray<FVector> Positions;

	/** Unit tangent (TangentX). */
	TArray<FVector> TangentX;

	/** Unit normal (TangentZ) in XYZ, bitangent sign (+1 / -1) in W. */
	TArray<FVector4> TangentZ;

	/** NumTexCoords arrays, each NumVertices() long. Always at least one channel (zeros if the file
	 *  had none, so materials still compile). */
	TArray<TArray<FVector2D>> UVs;

	/** Empty when the file had no vertex colours (the writer then skips the colour buffer). */
	TArray<FColor> Colors;

	TArray<FRCLBuiltSkinWeight> SkinWeights;

	/** Max influences used by any vertex; the writer sets bExtraBoneInfluences when this is above 4. */
	int32 MaxBoneInfluences = 1;

	// ---- Topology ----------------------------------------------------------------------------

	TArray<uint32> Indices;
	TArray<FRCLBuiltSection> Sections;

	/** Material slot -> index into FRCLImportData::Materials. Only materials this garment uses, in
	 *  first-use order, so the USkeletalMesh gets no empty slots. */
	TArray<int32> MaterialSlots;

	// ---- Skeleton usage (copied to FSkeletalMeshLODRenderData) -------------------------------

	/** Every garment bone named in a BoneMap, plus all their parents. Sorted ascending. */
	TArray<FBoneIndexType> ActiveBoneIndices;
	TArray<FBoneIndexType> RequiredBones;

	// ---- Bounds and provenance ---------------------------------------------------------------

	/** Reference-pose bounds of Positions. */
	FBoxSphereBounds Bounds;

	/** Render vertex -> vertex index in the source FRCLImportMesh. Used by Milestone 8 (morph
	 *  transfer, which works on source vertices) and for debugging. */
	TArray<int32> SourceVertex;

	/** Milestone 8: morph targets transferred from the body (empty when none). */
	TArray<FRCLBuiltMorph> Morphs;

	FRCLBuildStats Stats;
	TArray<FString> Warnings;

	int32 NumVertices() const { return Positions.Num(); }
	int32 NumTexCoords() const { return UVs.Num(); }

	/** Approximate heap size, for logging and limits. */
	SIZE_T GetAllocatedSize() const;
};

/** Builder inputs. Plain values, safe on a worker. */
struct FRCLBuildSettings
{
	/** 1..8. Above 4 makes the garment use the engine's 8-influence path. From FRCLLoadOptions. */
	int32 MaxBoneInfluences = 4;

	/** Most bones one render section may use. The GPU skinning limit, read on the game thread with
	 *  FGPUBaseSkinVertexFactory::GetMaxGPUSkinBones() (256 on SM5) and never above 256. */
	int32 MaxBonesPerSection = 256;

	/** Recompute tangents even when the file has them. Always true for now: the parser does not
	 *  read file tangents yet (FRCLImportMesh::Tangents is empty), so this is for later. */
	bool bRecomputeTangents = true;

	/** Same limit as the parser, re-checked after vertices are duplicated at bone-limit splits. */
	int32 MaxVertices = 200000;

	/** MaxGPUSkinBones must come from the game thread; see MaxBonesPerSection. */
	static FRCLBuildSettings FromLoadOptions(const FRCLLoadOptions& Options, int32 MaxGPUSkinBones);
};

/**
 * Stateless builder. All work happens in Build; the private steps are listed in the order they run.
 *
 * Weights come in separately from the mesh, so the same builder serves file weights (from
 * FRCLBindResult::MeshInfluences), rigid binding, and Milestone 8 weight transfer.
 */
class FRCLMeshBuilder
{
public:
	/**
	 * Builds one garment.
	 *
	 * @param Mesh             Parsed geometry (one entry of FRCLImportData::Meshes).
	 * @param Influences       Per-vertex influences, JointIndex = garment bone index. Same layout as
	 *                         FRCLImportMesh::Influences.
	 * @param InfluenceStart   Mesh.NumVertices() + 1 entries.
	 * @param BoneParents      FRCLBindResult::ParentIndices (garment skeleton, parent-first).
	 *
	 * Returns false (with reason and a player-readable error) for: no triangles, index or influence
	 * out of range, NaN/Inf in positions, empty influence arrays (Milestone 8 must fill them first),
	 * or more than MaxVertices render vertices.
	 */
	static bool Build(const FRCLImportMesh& Mesh, const TArray<FRCLInfluence>& Influences,
		const TArray<uint32>& InfluenceStart, const TArray<int32>& BoneParents, const FRCLBuildSettings& Settings,
		FRCLBuiltMesh& OutMesh, ERCLFailureReason& OutReason, FString& OutError);

	/**
	 * Milestone 8: expands source-vertex morph deltas onto render vertices through
	 * OutMesh.SourceVertex (every render copy of a vertex moves the same) and fills Morphs. Call after
	 * Build. Morphs that end up empty are dropped.
	 */
	static void BuildMorphs(const TArray<FRCLMorphDeltas>& SourceMorphs, FRCLBuiltMesh& InOutMesh);

private:
	/** Untrusted-input checks: array lengths agree, indices and bone indices in range, finite values. */
	static bool Validate(const FRCLImportMesh& Mesh, const TArray<FRCLInfluence>& Influences,
		const TArray<uint32>& InfluenceStart, int32 NumBones, ERCLFailureReason& OutReason, FString& OutError);

	/**
	 * Per source vertex: sort influences by weight, keep the largest MaxBoneInfluences, renormalise,
	 * then quantise to uint8 summing to exactly 255 (largest-remainder rounding, so no vertex loses
	 * weight to rounding). Influences that round to 0 are dropped. A vertex with no weight is bound
	 * 100% to garment bone 0 (the root) and counted in Stats.NumVerticesUnweighted.
	 * Output bone indices are still garment bones; SplitSections converts them to section-local.
	 */
	static void PrepareWeights(const FRCLImportMesh& Mesh, const TArray<FRCLInfluence>& Influences,
		const TArray<uint32>& InfluenceStart, const FRCLBuildSettings& Settings,
		TArray<FRCLBuiltSkinWeight>& OutGarmentWeights, TArray<FBoneIndexType>& OutGarmentBones, FRCLBuildStats& Stats);

	/**
	 * Per source vertex tangent frame from positions, normals and UV channel 0: per-triangle UV
	 * gradients accumulated area-weighted onto each vertex, Gram-Schmidt against the normal, sign from
	 * the bitangent's handedness. Vertices with no usable gradient (no UVs, collapsed UVs) get an
	 * arbitrary unit perpendicular to the normal. Runs in UE space after the parser's Y mirror and V
	 * flip, so signs come out in the engine's convention without special cases.
	 */
	static void ComputeTangents(const FRCLImportMesh& Mesh, TArray<FVector>& OutTangentX,
		TArray<FVector4>& OutTangentZ, FRCLBuildStats& Stats);

	/**
	 * Splits each file section into chunks of at most MaxBonesPerSection bones (greedy, triangle by
	 * triangle in file order), then gives every chunk its own contiguous vertex range. A source
	 * vertex used by two chunks is duplicated. With SKN_BaseCharacter (200 bones) a chunk never
	 * splits; the path exists for other skeletons and lower GPU limits. Fills Sections, Indices,
	 * SourceVertex and the section-local bone indices in SkinWeights.
	 */
	static void SplitSections(const FRCLImportMesh& Mesh, const TArray<FRCLBuiltSkinWeight>& GarmentWeights,
		const TArray<FBoneIndexType>& GarmentBones, const FRCLBuildSettings& Settings, FRCLBuiltMesh& OutMesh);

	/** Copies positions, tangents, UVs (clamped to MAX_TEXCOORDS, at least one) and colours into
	 *  render order via SourceVertex. */
	static void EmitVertexStreams(const FRCLImportMesh& Mesh, const TArray<FVector>& TangentX,
		const TArray<FVector4>& TangentZ, FRCLBuiltMesh& OutMesh);

	/** MaterialSlots, ActiveBoneIndices / RequiredBones (BoneMap bones plus parents), Bounds,
	 *  16- or 32-bit index choice. */
	static void Finalise(const FRCLImportMesh& Mesh, const TArray<int32>& BoneParents, FRCLBuiltMesh& OutMesh);
};
