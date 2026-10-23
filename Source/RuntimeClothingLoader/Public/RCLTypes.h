// Runtime Clothing Loader - UE 4.19.2
// Blueprint-facing enums, load options and the load report.
//
// Milestone 2 - header for TD review. No implementation yet.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "RCLTypes.generated.h"

class USkeletalMesh;
class UMaterialInterface;

/** File formats the plugin can read. Decided from the file extension, then confirmed by the parser. */
UENUM(BlueprintType)
enum class ERCLSourceFormat : uint8
{
	Unknown,
	FBX,
	OBJ
};

/**
 * How the garment gets its skin weights (sign-off decision 6).
 * Auto is right for almost every file; the others are explicit overrides.
 */
UENUM(BlueprintType)
enum class ERCLSkinningMode : uint8
{
	/** Keep the file's weights for bones the target skeleton has; transfer weights from the
	 *  body for game-only bones (jiggle bones) and for files with no usable skin at all. */
	Auto,

	/** Every vertex 100% on RigidAttachBone. For hats, glasses, helmets. Matches the game's
	 *  existing AttachBone / AttachOffset clothing fields. */
	RigidToBone,

	/** Ignore the file's weights and transfer everything from the body. */
	TransferAlways
};

/** What to do with a file bone the target skeleton does not have (sign-off decision 6). */
UENUM(BlueprintType)
enum class ERCLUnknownBonePolicy : uint8
{
	/** Move its weights to the nearest ancestor (in the file's hierarchy) that the target has. Default. */
	ReparentToAncestor,

	/** Fail the load and list the unknown bones in the report. */
	Reject
};

/**
 * How the mesh parts (nodes) inside one file become garments.
 * One file can hold a single garment made of parts (a shirt plus its buttons) or a whole
 * outfit (top, skirt, shoes) that belongs in different clothing slots.
 */
UENUM(BlueprintType)
enum class ERCLMeshGrouping : uint8
{
	/** All included parts become one garment, one section per material. Default. */
	MergeAll,

	/** Each included part becomes its own garment, named after the part. */
	SplitByMeshNode
};

/** Texture roles a material can carry (sign-off decision 10). Decoded in Milestone 9. */
UENUM(BlueprintType)
enum class ERCLTextureSlot : uint8
{
	BaseColor,
	Normal,
	Specular,
	Roughness,
	Opacity,
	Emissive,

	Count UMETA(Hidden)
};

/** Why a load failed. Mirrors the OnFailure reason string so Blueprint can branch on it. */
UENUM(BlueprintType)
enum class ERCLFailureReason : uint8
{
	None,
	FileNotFound,
	FileTooLarge,
	UnsupportedFormat,
	ParseError,
	TooManyVertices,
	TooManyBones,
	NoMeshInFile,
	UnknownBonesRejected,
	MissingTargetSkeleton,
	MissingBodyMesh,
	Cancelled,
	InternalError
};

/**
 * Everything a caller can set for one load. Copied by value to the worker thread, so the
 * UObject pointers here are only ever dereferenced on the game thread (before dispatch and
 * when the mesh is assembled).
 */
USTRUCT(BlueprintType)
struct RUNTIMECLOTHINGLOADER_API FRCLLoadOptions
{
	GENERATED_BODY()

	// ---- Skinning ------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skinning")
	ERCLSkinningMode SkinningMode = ERCLSkinningMode::Auto;

	/** Bone every vertex follows when SkinningMode is RigidToBone. Must exist in the target skeleton. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skinning")
	FName RigidAttachBone = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skinning")
	ERCLUnknownBonePolicy UnknownBonePolicy = ERCLUnknownBonePolicy::ReparentToAncestor;

	/** Bone influences kept per vertex after normalisation: 4 (default) or up to 8 (sign-off decision 9). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skinning", meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxBoneInfluences = 4;

	// ---- Body (weight and morph transfer source, sign-off decision 5) ----------------

	/** The body the garment is worn on: FemaleBase_8_MorphTools_2 or MaleBase_DeletedMorphs.
	 *  Source of transferred weights and of every morph target (sign-off decision 7). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body")
	USkeletalMesh* BodyMesh = nullptr;

	/** Extra meshes attached to the body that garments may cover, for example the Futa attachment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body")
	TArray<USkeletalMesh*> BodyAttachmentMeshes;

	/** Build morph targets on the garment from the body's morphs, named to match. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body")
	bool bTransferMorphTargets = true;

	// ---- Mesh parts -----------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Parts")
	ERCLMeshGrouping Grouping = ERCLMeshGrouping::MergeAll;

	/** If not empty, only these parts are loaded (names from InspectClothingFile). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Parts")
	TArray<FString> IncludeMeshNodes;

	/** Parts never loaded, by name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Parts")
	TArray<FString> ExcludeMeshNodes;

	/** Skip parts that look like a DAZ figure body (named Genesis8Female*, Genesis8Male* and so on).
	 *  DAZ exports often include the figure the clothing was fitted on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Parts")
	bool bExcludeDazFigureMeshes = true;

	// ---- Geometry -------------------------------------------------------------------

	/** Extra uniform scale applied after unit conversion. 1 = trust the file's units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry", meta = (ClampMin = "0.001"))
	float ImportScale = 1.0f;

	/** Convert the file's axes and handedness to UE (Z-up, left-handed, centimetres). Leave on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bConvertToUnrealSpace = true;

	/** Keep CPU-side data so per-poly line traces work. Off by default (sign-off decision 9;
	 *  TD reports per-poly on skeletal meshes is unreliable in this game). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bEnablePerPolyCollision = false;

	// ---- Materials -------------------------------------------------------------------

	/** Material per section, by index. Missing entries fall back to DefaultMaterial. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Materials")
	TArray<UMaterialInterface*> MaterialOverrides;

	/** Used for any section without an override. Null = the engine's default surface material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Materials")
	UMaterialInterface* DefaultMaterial = nullptr;

	// ---- Limits (sign-off decision 9; player files are untrusted) --------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Limits", meta = (ClampMin = "1"))
	int32 MaxFileSizeMB = 64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Limits", meta = (ClampMin = "3"))
	int32 MaxVertices = 200000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Limits", meta = (ClampMin = "1", ClampMax = "256"))
	int32 MaxBones = 256;
};

/**
 * One mesh part found by InspectClothingFile, without building anything. Lets the game show
 * a player the parts of a file and let them pick (for example, one garment per clothing slot).
 */
USTRUCT(BlueprintType)
struct RUNTIMECLOTHINGLOADER_API FRCLMeshNodeInfo
{
	GENERATED_BODY()

	/** Node name in the file; pass back in IncludeMeshNodes / ExcludeMeshNodes. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Part")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Part")
	int32 NumVertices = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Part")
	int32 NumTriangles = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Part")
	int32 NumMaterials = 0;

	/** True if the part has its own skin weights. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Part")
	bool bIsSkinned = false;

	/** True if the part looks like a DAZ figure body; skipped by default. */
	UPROPERTY(BlueprintReadOnly, Category = "Mesh Part")
	bool bLooksLikeDazFigure = false;
};

/**
 * What happened during a load: returned with both OnSuccess and OnFailure so the game can
 * show a player why a garment loaded badly or not at all.
 */
USTRUCT(BlueprintType)
struct RUNTIMECLOTHINGLOADER_API FRCLLoadReport
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Report")
	FString SourceFile;

	UPROPERTY(BlueprintReadOnly, Category = "Report")
	ERCLSourceFormat Format = ERCLSourceFormat::Unknown;

	/** Mesh part(s) this garment was built from. One name per part when merged. */
	UPROPERTY(BlueprintReadOnly, Category = "Report")
	TArray<FString> MeshNodes;

	UPROPERTY(BlueprintReadOnly, Category = "Report")
	ERCLFailureReason FailureReason = ERCLFailureReason::None;

	/** Milestone 7: why the load failed, as one sentence a player can read ("The file is larger than
	 *  64 MB."). Empty on success. */
	UPROPERTY(BlueprintReadOnly, Category = "Report")
	FString ErrorMessage;

	UPROPERTY(BlueprintReadOnly, Category = "Report")
	int32 NumVertices = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Report")
	int32 NumTriangles = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Report")
	int32 NumSections = 0;

	/** File bones matched to the target skeleton by name. */
	UPROPERTY(BlueprintReadOnly, Category = "Report")
	TArray<FName> MatchedBones;

	/** File bones the target skeleton does not have (reparented or rejected per policy). */
	UPROPERTY(BlueprintReadOnly, Category = "Report")
	TArray<FName> UnknownBones;

	/** True if any weights came from the body rather than the file. */
	UPROPERTY(BlueprintReadOnly, Category = "Report")
	bool bUsedWeightTransfer = false;

	/** Share of garment vertices that found a good body match (0..1). Filled from Milestone 8. */
	UPROPERTY(BlueprintReadOnly, Category = "Report")
	float TransferMatchRatio = 0.0f;

	/** Furthest garment vertex from the body surface, in cm. Filled from Milestone 8. */
	UPROPERTY(BlueprintReadOnly, Category = "Report")
	float TransferMaxDistanceCm = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Report")
	int32 NumMorphTargets = 0;

	/** Non-fatal problems, one line each, for logs and UI. */
	UPROPERTY(BlueprintReadOnly, Category = "Report")
	TArray<FString> Warnings;

	/** Worker-thread time (parse, bind, transfer, pack), in ms. */
	UPROPERTY(BlueprintReadOnly, Category = "Report")
	float WorkerTimeMs = 0.0f;

	/** Game-thread time (mesh assembly), in ms. Must stay small: the game runs in VR. */
	UPROPERTY(BlueprintReadOnly, Category = "Report")
	float GameThreadTimeMs = 0.0f;
};

/**
 * Milestone 7: what Check Clothing File returns. Nothing is built; this describes what Load Clothing
 * would produce with the same file and options.
 */
USTRUCT(BlueprintType)
struct RUNTIMECLOTHINGLOADER_API FRCLCheckResult
{
	GENERATED_BODY()

	/** As Load Clothing's report, including FailureReason / ErrorMessage when unusable. */
	UPROPERTY(BlueprintReadOnly, Category = "Check")
	FRCLLoadReport Report;

	/** Every mesh part in the file, including ones the options would skip, so the upload screen can
	 *  offer them (pass names back in IncludeMeshNodes / ExcludeMeshNodes). */
	UPROPERTY(BlueprintReadOnly, Category = "Check")
	TArray<FRCLMeshNodeInfo> Parts;
};
