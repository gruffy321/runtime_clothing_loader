// Runtime Clothing Loader - UE 4.19.2
// Milestone 8: a plain-data copy of the body that weight and morph transfer read on a worker thread.
//
// Milestone 8 - header for TD review. No implementation yet.
//
// What is copied (game thread, once per body, then shared by every load):
//  - LOD0 render vertices of the body and of each attachment (Futa): position and normal in
//    component space, reference pose. The engine keeps these CPU copies in 4.19 by default
//    ("to support merging of skeletal meshes"); r.FreeSkeletalMeshBuffers=1 would remove them
//    (not set in FluidGame's config), in which case Capture fails with a clear message.
//  - Their skin weights, as target skeleton bone indices (section BoneMaps resolved).
//  - Every morph target: sparse position and normal deltas per vertex (UMorphTarget::MorphLODModels,
//    always kept). Attachment morphs are included under their own names.
//  - A uniform grid over the positions for nearest-vertex queries.
//
// Licensing: this copy lives in memory only and is never written anywhere (sign-off decision on
// the proprietary base character). Garments built from it are RF_Transient (Milestone 6).

#pragma once

#include "CoreMinimal.h"
#include "RCLImportData.h"
#include "RCLSkeletonSnapshot.h"

class USkeletalMesh;

/** Uniform grid over the body's vertices. Read-only after Build; safe to share between workers. */
struct FRCLPointGrid
{
	FVector Origin = FVector::ZeroVector;
	float CellSize = 2.0f;                // cm; about one body vertex spacing
	FIntVector Dims = FIntVector(0, 0, 0);
	TArray<int32> CellStart;              // Dims.X * Dims.Y * Dims.Z + 1 entries
	TArray<int32> CellPoints;             // point indices, grouped by cell

	void Build(const TArray<FVector>& Points, float InCellSize);

	/** Up to MaxResults points within Radius of Query, nearest first. */
	void FindNearest(const TArray<FVector>& Points, const FVector& Query, float Radius, int32 MaxResults,
		TArray<int32>& OutIndices, TArray<float>& OutDistances) const;
};

/** One morph target of the body (or an attachment), sparse. */
struct FRCLBodyMorph
{
	FName Name;
	TArray<int32> Vertex;         // index into FRCLBodySnapshot::Positions
	TArray<FVector> PositionDelta;
	TArray<FVector> NormalDelta;
};

struct FRCLBodySnapshot
{
	/** Body and attachment names, for logs and the cache key. */
	FString SourceName;

	/** Body LOD0 render vertices first, then each attachment's, concatenated. */
	TArray<FVector> Positions;
	TArray<FVector> Normals;

	/** Skin weights: Influences[InfluenceStart[V] .. InfluenceStart[V + 1]), JointIndex = target bone. */
	TArray<FRCLInfluence> Influences;
	TArray<uint32> InfluenceStart;

	TArray<FRCLBodyMorph> Morphs;

	FRCLPointGrid Grid;

	int32 NumVertices() const { return Positions.Num(); }
	SIZE_T GetAllocatedSize() const;

	/**
	 * Game thread. Copies Body (+ Attachments) into OutSnapshot. Target is the skeleton snapshot the
	 * garment binds against (Body's own), used to turn every bone into a target bone index.
	 * bWithMorphs false skips the morph copy (rigid items, or morph transfer turned off).
	 * Fails (with a player-readable reason in OutError) if LOD0 has no CPU vertex data, or an
	 * attachment uses a bone the target does not have.
	 */
	static bool Capture(const USkeletalMesh& Body, const TArray<USkeletalMesh*>& Attachments,
		const FRCLSkeletonSnapshot& Target, bool bWithMorphs, FRCLBodySnapshot& OutSnapshot, FString& OutError);
};

/**
 * One snapshot per body (+ attachments, + with/without morphs), captured on first use and shared.
 * The first load for a body pays the copy (a few to a few tens of milliseconds, depending on the
 * number of body morphs); Prepare Body For Clothing lets the game pay it during a loading screen.
 */
class FRCLBodySnapshotCache
{
public:
	static FRCLBodySnapshotCache& Get();

	/** Game thread. Cached snapshot, capturing it if needed. Null (and OutError) on failure. */
	TSharedPtr<const FRCLBodySnapshot, ESPMode::ThreadSafe> FindOrCapture(const USkeletalMesh& Body,
		const TArray<USkeletalMesh*>& Attachments, bool bWithMorphs, FString& OutError);

	/** Game thread. Drops every snapshot (module shutdown; running workers keep theirs alive). */
	void Reset();

private:
	TMap<FString, TSharedPtr<const FRCLBodySnapshot, ESPMode::ThreadSafe>> Snapshots;
};
