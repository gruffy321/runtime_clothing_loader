// Runtime Clothing Loader - UE 4.19.2
// Milestone 8: weight transfer (for unweighted garments) and morph transfer (always, sign-off
// decision 7) from the body onto a garment.
//
// Milestone 8 - header for TD review. No implementation yet.
//
// Worker thread only; reads a shared FRCLBodySnapshot. Works on SOURCE vertices (the parser's
// FRCLImportMesh), before the builder splits and duplicates them, so every render copy of a vertex
// gets the same result.
//
// Method: each garment vertex is matched to its nearest body vertices (up to NumNeighbours within
// SearchRadiusCm), preferring those whose normal faces the same way, so a skirt vertex between the
// legs follows the nearer leg's outside rather than the other leg. Weights and morph deltas are the
// inverse-distance blend of the matched vertices. The garment and body are both in the stock
// Genesis 8 Female bind pose (Milestone 4 measured 0.00 cm), so no pose correction is needed.

#pragma once

#include "CoreMinimal.h"
#include "RCLTypes.h"
#include "RCLImportData.h"
#include "RCLBodySnapshot.h"

/** Which body vertices a garment vertex follows, and how much. */
struct FRCLVertexMatch
{
	enum { MaxNeighbours = 4 };

	int32 BodyVertex[MaxNeighbours];
	float Weight[MaxNeighbours];      // sums to 1
	int32 Num = 0;
	float DistanceCm = 0.0f;          // to the nearest matched body vertex
	bool bGood = false;               // within MaxGoodDistanceCm and facing the same way
};

/** A garment morph target before the builder expands it to render vertices. */
struct FRCLMorphDeltas
{
	FName Name;                       // the body's morph name, so the body's sliders drive it
	TArray<int32> SourceVertex;       // index into FRCLImportMesh vertices
	TArray<FVector> PositionDelta;
	TArray<FVector> NormalDelta;
};

struct FRCLTransferSettings
{
	int32 NumNeighbours = 4;          // 1..FRCLVertexMatch::MaxNeighbours
	float SearchRadiusCm = 30.0f;     // nothing further is ever matched
	float MaxGoodDistanceCm = 5.0f;   // beyond this a match is counted as poor in the report
	float MorphThresholdCm = 0.005f;  // smaller per-vertex morph moves are dropped (saves memory)

	static FRCLTransferSettings FromLoadOptions(const FRCLLoadOptions& Options);
};

/** Report numbers (copied into FRCLLoadReport's TransferMatchRatio / TransferMaxDistanceCm / NumMorphTargets). */
struct FRCLTransferStats
{
	float MatchRatio = 0.0f;          // share of garment vertices with a good match
	float MaxDistanceCm = 0.0f;
	int32 NumUnmatched = 0;           // nothing within SearchRadiusCm: weights fall back to the root,
	                                  // morphs leave these vertices still
	int32 NumMorphsTransferred = 0;
	int32 NumMorphsSkipped = 0;       // no effect on this garment (facial morphs on a bodysuit, etc.)
	double TimeMs = 0.0;
};

class FRCLTransfer
{
public:
	/** Matches every vertex of Mesh to the body. */
	static void MatchVertices(const FRCLImportMesh& Mesh, const FRCLBodySnapshot& Body, const FRCLTransferSettings& Settings,
		TArray<FRCLVertexMatch>& OutMatches, FRCLTransferStats& Stats);

	/**
	 * Weights for every garment vertex from its matches, as TARGET bone indices (the binder's
	 * AddTargetInfluences turns them into garment bones). Same layout as FRCLImportMesh::Influences.
	 * Not trimmed: the builder still keeps the strongest MaxBoneInfluences and renormalises.
	 */
	static void TransferWeights(const TArray<FRCLVertexMatch>& Matches, const FRCLBodySnapshot& Body,
		TArray<FRCLInfluence>& OutTargetInfluences, TArray<uint32>& OutInfluenceStart);

	/** One FRCLMorphDeltas per body morph that moves this garment; others skipped (counted in Stats). */
	static void TransferMorphs(const TArray<FRCLVertexMatch>& Matches, const FRCLBodySnapshot& Body,
		const FRCLTransferSettings& Settings, TArray<FRCLMorphDeltas>& OutMorphs, FRCLTransferStats& Stats);
};
