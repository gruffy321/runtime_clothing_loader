// Runtime Clothing Loader - UE 4.19.2
// Engine-neutral result of parsing one file. Plain C++, no UObjects: built on a worker thread.
//
// Milestone 2 - header for TD review. No implementation yet.
//
// Conventions for everything in this file, guaranteed by every IRCLMeshParser:
//  - Unreal space: Z-up, left-handed, centimetres (unless the caller turned conversion off).
//  - Node transforms are baked in. Included mesh nodes are merged into one FRCLImportMesh
//    (ERCLMeshGrouping::MergeAll) or kept as one FRCLImportMesh each (SplitByMeshNode).
//    Joints, materials and textures are shared by all meshes of one file.
//  - Triangles only; polygons are triangulated by the parser.
//  - Vertices are already split wherever normal, UV or material differs (render-ready).

#pragma once

#include "CoreMinimal.h"
#include "RCLTypes.h"

/** One bone influence on one vertex. */
struct FRCLInfluence
{
	/** Index into FRCLImportData::Joints. */
	int32 JointIndex = INDEX_NONE;

	/** 0..1. Not yet normalised or trimmed; the mesh builder does that. */
	float Weight = 0.0f;
};

/** A joint (bone) from the file's skeleton, as the file defines it. */
struct FRCLImportJoint
{
	/** Name exactly as in the file, for example "lThighBend" for DAZ Genesis 8. */
	FName Name = NAME_None;

	/** Index into FRCLImportData::Joints, or INDEX_NONE for a root. Parents come before children. */
	int32 ParentIndex = INDEX_NONE;

	/** Pose the mesh was skinned in (inverse of the inverse bind matrix), component space. */
	FTransform BindPoseComponentSpace = FTransform::Identity;

	/** True if at least one vertex has weight on this joint. */
	bool bInfluencesVertices = false;
};

/** A contiguous run of triangles that share one material. */
struct FRCLImportSection
{
	/** Index into FRCLImportData::Materials. */
	int32 MaterialIndex = 0;

	/** First index in FRCLImportMesh::Indices (a multiple of 3). */
	uint32 FirstIndex = 0;

	uint32 NumTriangles = 0;
};

/** Geometry of one garment, stored as parallel arrays (one entry per vertex) for cache-friendly packing. */
struct FRCLImportMesh
{
	/** Garment name: the node name when split; the file name when merged. */
	FString Name;

	/** Node(s) this mesh came from. One entry when split; every included node when merged. */
	TArray<FString> SourceNodes;

	TArray<FVector> Positions;

	/** Unit length. */
	TArray<FVector> Normals;

	/** XYZ tangent, W = bitangent sign (+1 or -1). Empty if the file had none; the builder computes them. */
	TArray<FVector4> Tangents;

	/** One array per UV channel, each Positions.Num() long. At most MAX_TEXCOORDS channels. */
	TArray<TArray<FVector2D>> UVs;

	/** Empty if the file had no vertex colours. */
	TArray<FColor> Colors;

	/** Triangle list, 3 per triangle. */
	TArray<uint32> Indices;

	TArray<FRCLImportSection> Sections;

	/** Influences for vertex V are Influences[InfluenceStart[V] .. InfluenceStart[V + 1]).
	 *  InfluenceStart has Positions.Num() + 1 entries. Both empty for unskinned files. */
	TArray<FRCLInfluence> Influences;
	TArray<uint32> InfluenceStart;

	int32 NumVertices() const { return Positions.Num(); }
	int32 NumTriangles() const { return Indices.Num() / 3; }
	bool HasSkinWeights() const { return Influences.Num() > 0; }
};

/** A texture the file refers to or embeds. Carried from Milestone 3, decoded in Milestone 9. */
struct FRCLImportTexture
{
	/** Path as written in the file. Resolved only inside the file's own folder (untrusted input). */
	FString RelativePath;

	/** Raw image bytes when the texture is embedded in the file; empty otherwise. */
	TArray<uint8> EmbeddedBytes;

	bool IsEmbedded() const { return EmbeddedBytes.Num() > 0; }
};

/** A material as the file describes it. Used for section names and, from Milestone 9, textures. */
struct FRCLImportMaterial
{
	FName Name = NAME_None;

	FLinearColor BaseColorFactor = FLinearColor::White;

	/** Index into FRCLImportData::Textures per slot, or INDEX_NONE. */
	int32 TextureIndices[(int32)ERCLTextureSlot::Count];

	FRCLImportMaterial()
	{
		for (int32& Index : TextureIndices)
		{
			Index = INDEX_NONE;
		}
	}
};

/** Everything a parser extracts from one file. */
struct FRCLImportData
{
	FString SourceFile;

	ERCLSourceFormat Format = ERCLSourceFormat::Unknown;

	/** One entry when merged; one per included mesh node when split. Never empty on success. */
	TArray<FRCLImportMesh> Meshes;

	/** Empty for OBJ and for unskinned FBX. */
	TArray<FRCLImportJoint> Joints;

	TArray<FRCLImportMaterial> Materials;

	TArray<FRCLImportTexture> Textures;

	/** Non-fatal problems found while parsing; copied into FRCLLoadReport::Warnings. */
	TArray<FString> Warnings;

	/** Milestone 8: notices from the FBX reader itself (for example "Bad UTF-8 string (x19)": names it
	 *  had to clean up). Logged, never shown to players. */
	TArray<FString> TechnicalWarnings;

	/** Approximate heap size, for logging and limits. */
	SIZE_T GetAllocatedSize() const;
};
