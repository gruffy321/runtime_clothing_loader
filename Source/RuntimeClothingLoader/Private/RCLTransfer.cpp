// Runtime Clothing Loader - UE 4.19.2
// Milestone 8: weight and morph transfer. See RCLTransfer.h.

#include "RCLTransfer.h"
#include "RuntimeClothingLoader.h"
#include "HAL/PlatformTime.h"

FRCLTransferSettings FRCLTransferSettings::FromLoadOptions(const FRCLLoadOptions& Options)
{
	return FRCLTransferSettings();   // fixed for now (TD sign-off); options can expose these later
}

void FRCLTransfer::MatchVertices(const FRCLImportMesh& Mesh, const FRCLBodySnapshot& Body, const FRCLTransferSettings& Settings,
	TArray<FRCLVertexMatch>& OutMatches, FRCLTransferStats& Stats)
{
	const double Start = FPlatformTime::Seconds();
	const int32 NumNeighbours = FMath::Clamp(Settings.NumNeighbours, 1, (int32)FRCLVertexMatch::MaxNeighbours);
	const int32 NumCandidates = 16;   // nearest by distance, then filtered by facing
	const int32 NumVertices = Mesh.NumVertices();

	OutMatches.SetNum(NumVertices);
	TArray<int32> Candidates;
	TArray<float> Distances;
	int32 NumGood = 0;
	Stats.NumUnmatched = 0;
	Stats.MaxDistanceCm = 0.0f;

	for (int32 V = 0; V < NumVertices; ++V)
	{
		FRCLVertexMatch& Match = OutMatches[V];
		Match = FRCLVertexMatch();
		Body.Grid.FindNearest(Body.Positions, Mesh.Positions[V], Settings.SearchRadiusCm, NumCandidates, Candidates, Distances);
		if (Candidates.Num() == 0)
		{
			++Stats.NumUnmatched;
			continue;
		}

		// Prefer body vertices facing the same way as the garment (outside of the nearer limb).
		const FVector Normal = Mesh.Normals[V];
		bool bAnyFacing = false;
		for (int32 C = 0; C < Candidates.Num() && !bAnyFacing; ++C)
		{
			bAnyFacing = (Body.Normals[Candidates[C]] | Normal) > 0.0f;
		}

		float WeightSum = 0.0f;
		for (int32 C = 0; C < Candidates.Num() && Match.Num < NumNeighbours; ++C)
		{
			const int32 B = Candidates[C];
			if (bAnyFacing && (Body.Normals[B] | Normal) <= 0.0f)
			{
				continue;
			}
			// Inverse square, softened at 0.1 mm: a body vertex right under the garment vertex decides on its
			// own; at normal clothing offsets (2 to 10 mm) the neighbours blend smoothly.
			const float W = 1.0f / (Distances[C] * Distances[C] + 0.0001f);
			Match.BodyVertex[Match.Num] = B;
			Match.Weight[Match.Num] = W;
			WeightSum += W;
			if (Match.Num == 0)
			{
				Match.DistanceCm = Distances[C];
			}
			++Match.Num;
		}
		for (int32 N = 0; N < Match.Num; ++N)
		{
			Match.Weight[N] /= WeightSum;
		}
		Match.bGood = bAnyFacing && Match.DistanceCm <= Settings.MaxGoodDistanceCm;
		NumGood += Match.bGood ? 1 : 0;
		Stats.MaxDistanceCm = FMath::Max(Stats.MaxDistanceCm, Match.DistanceCm);
	}

	Stats.MatchRatio = NumVertices > 0 ? (float)NumGood / NumVertices : 0.0f;
	Stats.TimeMs += (FPlatformTime::Seconds() - Start) * 1000.0;
}

void FRCLTransfer::TransferWeights(const TArray<FRCLVertexMatch>& Matches, const FRCLBodySnapshot& Body,
	TArray<FRCLInfluence>& OutTargetInfluences, TArray<uint32>& OutInfluenceStart)
{
	OutTargetInfluences.Reset();
	OutInfluenceStart.Reset();
	OutInfluenceStart.Add(0);

	TArray<FRCLInfluence, TInlineAllocator<32>> Blend;
	for (const FRCLVertexMatch& Match : Matches)
	{
		Blend.Reset();
		for (int32 N = 0; N < Match.Num; ++N)
		{
			const int32 B = Match.BodyVertex[N];
			for (uint32 I = Body.InfluenceStart[B]; I < Body.InfluenceStart[B + 1]; ++I)
			{
				const FRCLInfluence& Source = Body.Influences[I];
				const float Weight = Source.Weight * Match.Weight[N];
				FRCLInfluence* Existing = Blend.FindByPredicate([&Source](const FRCLInfluence& X) { return X.JointIndex == Source.JointIndex; });
				if (Existing)
				{
					Existing->Weight += Weight;
				}
				else
				{
					FRCLInfluence Added;
					Added.JointIndex = Source.JointIndex;
					Added.Weight = Weight;
					Blend.Add(Added);
				}
			}
		}
		if (Blend.Num() == 0)
		{
			// Nothing nearby: follow the root (the builder would do the same for an empty vertex).
			FRCLInfluence Root;
			Root.JointIndex = 0;
			Root.Weight = 1.0f;
			Blend.Add(Root);
		}
		OutTargetInfluences.Append(Blend.GetData(), Blend.Num());
		OutInfluenceStart.Add(OutTargetInfluences.Num());
	}
}

void FRCLTransfer::TransferMorphs(const TArray<FRCLVertexMatch>& Matches, const FRCLBodySnapshot& Body,
	const FRCLTransferSettings& Settings, TArray<FRCLMorphDeltas>& OutMorphs, FRCLTransferStats& Stats)
{
	const double Start = FPlatformTime::Seconds();
	OutMorphs.Reset();
	const float ThresholdSq = Settings.MorphThresholdCm * Settings.MorphThresholdCm;

	// Which body vertices this garment follows: a morph touching none of them is skipped outright.
	TArray<bool> bBodyVertexUsed;
	bBodyVertexUsed.Init(false, Body.NumVertices());
	for (const FRCLVertexMatch& Match : Matches)
	{
		for (int32 N = 0; N < Match.Num; ++N)
		{
			bBodyVertexUsed[Match.BodyVertex[N]] = true;
		}
	}

	// Body vertex -> slot in the current morph's delta arrays (reset after each morph).
	TArray<int32> SlotOfBodyVertex;
	SlotOfBodyVertex.Init(INDEX_NONE, Body.NumVertices());

	for (const FRCLBodyMorph& BodyMorph : Body.Morphs)
	{
		bool bTouchesGarment = false;
		for (int32 K = 0; K < BodyMorph.Vertex.Num(); ++K)
		{
			SlotOfBodyVertex[BodyMorph.Vertex[K]] = K;
			bTouchesGarment |= bBodyVertexUsed[BodyMorph.Vertex[K]];
		}

		if (bTouchesGarment)
		{
			FRCLMorphDeltas Morph;
			Morph.Name = BodyMorph.Name;
			for (int32 V = 0; V < Matches.Num(); ++V)
			{
				const FRCLVertexMatch& Match = Matches[V];
				FVector Position = FVector::ZeroVector;
				FVector Normal = FVector::ZeroVector;
				for (int32 N = 0; N < Match.Num; ++N)
				{
					const int32 Slot = SlotOfBodyVertex[Match.BodyVertex[N]];
					if (Slot != INDEX_NONE)
					{
						Position += BodyMorph.PositionDelta[Slot] * Match.Weight[N];
						Normal += BodyMorph.NormalDelta[Slot] * Match.Weight[N];
					}
				}
				if (Position.SizeSquared() > ThresholdSq)
				{
					Morph.SourceVertex.Add(V);
					Morph.PositionDelta.Add(Position);
					Morph.NormalDelta.Add(Normal);
				}
			}
			if (Morph.SourceVertex.Num() > 0)
			{
				OutMorphs.Add(MoveTemp(Morph));
			}
			else
			{
				++Stats.NumMorphsSkipped;
			}
		}
		else
		{
			++Stats.NumMorphsSkipped;
		}

		for (int32 BodyVertex : BodyMorph.Vertex)
		{
			SlotOfBodyVertex[BodyVertex] = INDEX_NONE;
		}
	}

	Stats.NumMorphsTransferred = OutMorphs.Num();
	Stats.TimeMs += (FPlatformTime::Seconds() - Start) * 1000.0;
}
