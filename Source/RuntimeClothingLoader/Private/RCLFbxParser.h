// Runtime Clothing Loader - UE 4.19.2
// IRCLMeshParser implementation for FBX (binary and ASCII, version 3000+) and OBJ, using ufbx.
//
// Milestone 3 - header for TD review. No implementation yet.

#pragma once

#include "CoreMinimal.h"
#include "IRCLMeshParser.h"
#include "RCLImportData.h"

// ufbx types, forward-declared so this header never pulls in ufbx.h.
struct ufbx_scene;
struct ufbx_node;
struct ufbx_material;

/**
 * Reads FBX and OBJ files into FRCLImportData.
 *
 * Coordinate system: ufbx converts the file to right-handed Z-up in centimetres (its built-in
 * space conversion, applied to geometry and to bind poses). The parser then mirrors Y and
 * flips triangle winding to reach Unreal's left-handed space. That is the same result UE's own
 * FBX importer produces, so a garment lines up with the body exactly as the body did when it
 * was imported from DAZ.
 *
 * Untrusted input:
 *  - The file size is checked before anything is read.
 *  - ufbx sanitises indices and never reads out of bounds; its temp and result memory are
 *    capped (4x the file size limit, so a small file can't expand into gigabytes).
 *  - Every external file ufbx asks to open (.mtl for OBJ) goes through an open-file callback
 *    that refuses any path outside the source file's own folder. Image files are never opened
 *    by the parser; only their paths are recorded.
 *  - Vertex and bone limits apply per garment after grouping.
 *
 * Stateless: every call loads, converts and frees its own ufbx scene. Safe to run several
 * loads at once on different workers.
 */
class FRCLFbxParser final : public IRCLMeshParser
{
public:
	// IRCLMeshParser
	virtual ERCLSourceFormat DetectFormat(const FString& FilePath) const override;

	virtual bool Parse(
		const FString& FilePath,
		const FRCLParseSettings& Settings,
		FRCLImportData& OutData,
		ERCLFailureReason& OutReason,
		FString& OutError) override;

	virtual bool Inspect(
		const FString& FilePath,
		const FRCLParseSettings& Settings,
		TArray<FRCLMeshNodeInfo>& OutNodes,
		ERCLFailureReason& OutReason,
		FString& OutError) override;

private:
	/** Owns a loaded scene; frees it with ufbx_free_scene on destruction. */
	struct FSceneHandle
	{
		ufbx_scene* Scene = nullptr;
		~FSceneHandle();
	};

	/** Existence, extension and size checks. Never opens the file for reading. */
	static bool PreflightFile(const FString& FilePath, const FRCLParseSettings& Settings,
		ERCLFailureReason& OutReason, FString& OutError);

	/** Loads the scene with axis/unit conversion, memory caps and the folder-restricted open callback. */
	static bool LoadScene(const FString& FilePath, const FRCLParseSettings& Settings, bool bGeometry,
		FSceneHandle& OutScene, ERCLFailureReason& OutReason, FString& OutError);

	/** Mesh nodes to load, after include/exclude lists and DAZ figure detection. Warnings for skipped ones. */
	static void SelectMeshNodes(const ufbx_scene& Scene, const FRCLParseSettings& Settings,
		TArray<const ufbx_node*>& OutNodes, TArray<FString>& OutWarnings);

	/** True for nodes that look like a DAZ figure body: name starts with Genesis3/8/8_1/9 and the
	 *  mesh is skinned to that figure's own root. Only used to skip a part when the file has more
	 *  than one skinned part: a single-part file is never skipped, because players rename a
	 *  garment's root to "Genesis8Female" for the editor importer, which makes a garment's names
	 *  look like the figure's (TD note, 2026-10-20). */
	static bool LooksLikeDazFigure(const ufbx_node& Node);

	/** Joints: every bone any selected mesh is skinned to, plus all ancestors up to the root, in
	 *  parent-first order, with bind poses taken from the skin clusters. The file's root joint is
	 *  kept under its own name; the binder (Milestone 4) maps it to the target root whatever it is
	 *  called, so players no longer need to rename a DAZ garment's root to "Genesis8Female".
	 *  Returns a map from ufbx node to joint index for AppendMeshNode. */
	static void BuildJoints(const ufbx_scene& Scene, const TArray<const ufbx_node*>& MeshNodes,
		FRCLImportData& OutData, TMap<const ufbx_node*, int32>& OutJointOfNode);

	/** Materials (name, base colour) and texture references or embedded bytes for six slots. */
	static void ReadMaterials(const ufbx_scene& Scene, FRCLImportData& OutData,
		TMap<const ufbx_material*, int32>& OutMaterialIndexOf);

	/** Triangulates one mesh node and appends it to OutMesh: positions, normals, tangents, UVs,
	 *  colours and influences, de-duplicated into render vertices, one section per material. */
	static bool AppendMeshNode(const ufbx_node& Node, const TMap<const ufbx_node*, int32>& JointOfNode,
		const TMap<const ufbx_material*, int32>& MaterialIndexOf, FRCLImportMesh& OutMesh, TArray<FString>& OutWarnings);

	/** Mirrors Y and flips winding on a finished mesh (and joint bind poses) to reach UE space. */
	static void ConvertToUnrealHandedness(FRCLImportData& Data);

	/** Per-garment vertex and bone limits. */
	static bool CheckLimits(const FRCLImportData& Data, const FRCLParseSettings& Settings,
		ERCLFailureReason& OutReason, FString& OutError);
};
