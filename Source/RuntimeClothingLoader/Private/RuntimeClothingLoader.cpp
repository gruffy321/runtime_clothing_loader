// Runtime Clothing Loader - UE 4.19.2
// Module entry point, plus test console commands in non-Shipping builds.

#include "RuntimeClothingLoader.h"
#include "RCLLoadPipeline.h"        // all builds: world cleanup and shutdown
#include "RCLBodySnapshot.h"
#include "Engine/World.h"          // FWorldDelegates

#if !UE_BUILD_SHIPPING
#include "IRCLMeshParser.h"
#include "RCLImportData.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Async/Async.h"
#include "Misc/Paths.h"
#include "RCLSkeletonSnapshot.h"
#include "RCLSkeletonBinder.h"
#include "RCLMeshBuilder.h"
#include "GPUSkinVertexFactory.h"
#include "RCLMeshWriter.h"
#include "RCLLoadPipeline.h"
#include "RCLClothingLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UObjectIterator.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/UObjectGlobals.h"
#endif

#define RCL_PLUGIN_VERSION TEXT("0.9.0")

DEFINE_LOG_CATEGORY(LogRuntimeClothing);

#if !UE_BUILD_SHIPPING
namespace RCLConsole
{
	/** Joins console arguments (paths may contain spaces) and strips surrounding quotes. */
	FString PathFromArgs(const TArray<FString>& Args)
	{
		FString Path = FString::Join(Args, TEXT(" ")).TrimStartAndEnd();
		Path.RemoveFromStart(TEXT("\""));
		Path.RemoveFromEnd(TEXT("\""));
		return Path;
	}

	/** Readable name for a failure reason. No UObject lookups: this runs on worker threads. */
	const TCHAR* FailureName(ERCLFailureReason Reason)
	{
		switch (Reason)
		{
		case ERCLFailureReason::None:                 return TEXT("None");
		case ERCLFailureReason::FileNotFound:         return TEXT("FileNotFound");
		case ERCLFailureReason::FileTooLarge:         return TEXT("FileTooLarge");
		case ERCLFailureReason::UnsupportedFormat:    return TEXT("UnsupportedFormat");
		case ERCLFailureReason::ParseError:           return TEXT("ParseError");
		case ERCLFailureReason::TooManyVertices:      return TEXT("TooManyVertices");
		case ERCLFailureReason::TooManyBones:         return TEXT("TooManyBones");
		case ERCLFailureReason::NoMeshInFile:         return TEXT("NoMeshInFile");
		case ERCLFailureReason::UnknownBonesRejected: return TEXT("UnknownBonesRejected");
		case ERCLFailureReason::MissingTargetSkeleton:return TEXT("MissingTargetSkeleton");
		case ERCLFailureReason::MissingBodyMesh:      return TEXT("MissingBodyMesh");
		case ERCLFailureReason::Cancelled:            return TEXT("Cancelled");
		case ERCLFailureReason::InternalError:        return TEXT("InternalError");
		}
		return TEXT("Unknown");
	}

	void Inspect(const TArray<FString>& Args)
	{
		const FString Path = PathFromArgs(Args);
		if (Path.IsEmpty())
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("Usage: RCL.Inspect <file.fbx|file.obj>"));
			return;
		}

		Async<void>(EAsyncExecution::ThreadPool, [Path]()
		{
			TUniquePtr<IRCLMeshParser> Parser = IRCLMeshParser::CreateForFile(Path);
			if (!Parser)
			{
				UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Inspect: unsupported file type: %s"), *Path);
				return;
			}

			TArray<FRCLMeshNodeInfo> Nodes;
			ERCLFailureReason Reason = ERCLFailureReason::None;
			FString Error;
			if (!Parser->Inspect(Path, FRCLParseSettings::FromLoadOptions(FRCLLoadOptions()), Nodes, Reason, Error))
			{
				UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Inspect failed (%s): %s"), FailureName(Reason), *Error);
				return;
			}

			UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.Inspect %s: %d part(s)"), *FPaths::GetCleanFilename(Path), Nodes.Num());
			for (const FRCLMeshNodeInfo& Node : Nodes)
			{
				UE_LOG(LogRuntimeClothing, Log, TEXT("  '%s': %d vertices, %d triangles, %d material(s)%s%s"),
					*Node.Name, Node.NumVertices, Node.NumTriangles, Node.NumMaterials,
					Node.bIsSkinned ? TEXT(", skinned") : TEXT(""),
					Node.bLooksLikeDazFigure ? TEXT(", looks like a DAZ figure") : TEXT(""));
			}
		});
	}

	void Parse(const TArray<FString>& Args)
	{
		const FString Path = PathFromArgs(Args);
		if (Path.IsEmpty())
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("Usage: RCL.Parse <file.fbx|file.obj>"));
			return;
		}

		Async<void>(EAsyncExecution::ThreadPool, [Path]()
		{
			TUniquePtr<IRCLMeshParser> Parser = IRCLMeshParser::CreateForFile(Path);
			if (!Parser)
			{
				UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Parse: unsupported file type: %s"), *Path);
				return;
			}

			const double Start = FPlatformTime::Seconds();
			FRCLImportData Data;
			ERCLFailureReason Reason = ERCLFailureReason::None;
			FString Error;
			if (!Parser->Parse(Path, FRCLParseSettings::FromLoadOptions(FRCLLoadOptions()), Data, Reason, Error))
			{
				UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Parse failed (%s): %s"), FailureName(Reason), *Error);
				return;
			}
			const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;

			UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.Parse %s: %d garment(s), %d joint(s), %d material(s), %d texture(s), %.1f ms, %.1f KB"),
				*FPaths::GetCleanFilename(Path), Data.Meshes.Num(), Data.Joints.Num(), Data.Materials.Num(), Data.Textures.Num(),
				Ms, Data.GetAllocatedSize() / 1024.0);

			for (const FRCLImportMesh& Mesh : Data.Meshes)
			{
				FBox Bounds(Mesh.Positions);
				UE_LOG(LogRuntimeClothing, Log, TEXT("  Garment '%s': %d vertices, %d triangles, %d section(s), %d UV set(s), %s, bounds min %s max %s (cm)"),
					*Mesh.Name, Mesh.NumVertices(), Mesh.NumTriangles(), Mesh.Sections.Num(), Mesh.UVs.Num(),
					Mesh.HasSkinWeights() ? TEXT("skinned") : TEXT("not skinned"),
					*Bounds.Min.ToString(), *Bounds.Max.ToString());
			}
			for (int32 JointIndex = 0; JointIndex < Data.Joints.Num(); ++JointIndex)
			{
				const FRCLImportJoint& Joint = Data.Joints[JointIndex];
				UE_LOG(LogRuntimeClothing, Log, TEXT("  Joint %d '%s' parent %d%s at %s"), JointIndex, *Joint.Name.ToString(),
					Joint.ParentIndex, Joint.bInfluencesVertices ? TEXT(" (weighted)") : TEXT(""),
					*Joint.BindPoseComponentSpace.GetTranslation().ToString());
			}
			for (const FRCLImportTexture& Texture : Data.Textures)
			{
				UE_LOG(LogRuntimeClothing, Log, TEXT("  Texture '%s'%s"), *Texture.RelativePath,
					Texture.IsEmbedded() ? *FString::Printf(TEXT(" (embedded, %d bytes)"), Texture.EmbeddedBytes.Num()) : TEXT(""));
			}
			for (const FString& Warning : Data.Warnings)
			{
				UE_LOG(LogRuntimeClothing, Log, TEXT("  Warning: %s"), *Warning);
			}
			for (const FString& Notice : Data.TechnicalWarnings)
			{
				UE_LOG(LogRuntimeClothing, Log, TEXT("  Reader notice: %s"), *Notice);
			}
		});
	}

	/** Default body for RCL.Bind: the female base body (sign-off decision 5). */
	static const TCHAR* DefaultBodyPath = TEXT("/Game/Characters/Base/Female/FemaleBase_8_MorphTools_2.FemaleBase_8_MorphTools_2");

	/**
	 * Shared by RCL.Bind and RCL.Build. Splits "<file> [/Game/BodyMesh]" and, on the game thread,
	 * loads the body and copies its skeleton so the worker never touches UObjects.
	 */
	bool PrepareBodyCommand(const TCHAR* Command, const TArray<FString>& Args, FString& OutPath, FRCLSkeletonSnapshot& OutSnapshot,
		USkeletalMesh** OutBody = nullptr)
	{
		// Optional last argument: a body mesh object path (starts with /Game/). Everything before
		// it is the file path, which may contain spaces.
		TArray<FString> FileArgs = Args;
		FString BodyPath = DefaultBodyPath;
		if (FileArgs.Num() > 1 && FileArgs.Last().StartsWith(TEXT("/Game/")))
		{
			BodyPath = FileArgs.Pop();
			if (!BodyPath.Contains(TEXT(".")))
			{
				BodyPath += TEXT(".") + FPaths::GetBaseFilename(BodyPath);
			}
		}
		OutPath = PathFromArgs(FileArgs);
		if (OutPath.IsEmpty())
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("Usage: %s <file.fbx|file.obj> [/Game/path/to/BodyMesh]"), Command);
			return false;
		}

		USkeletalMesh* Body = LoadObject<USkeletalMesh>(nullptr, *BodyPath);
		if (!Body)
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("%s: could not load body mesh '%s'."), Command, *BodyPath);
			return false;
		}
		OutSnapshot = FRCLSkeletonSnapshot::FromSkeletalMesh(*Body);
		if (OutBody)
		{
			*OutBody = Body;
		}
		return true;
	}

	void Bind(const TArray<FString>& Args)
	{
		FString Path;
		FRCLSkeletonSnapshot Snapshot;
		if (!PrepareBodyCommand(TEXT("RCL.Bind"), Args, Path, Snapshot))
		{
			return;
		}

		Async<void>(EAsyncExecution::ThreadPool, [Path, Snapshot]()
		{
			TUniquePtr<IRCLMeshParser> Parser = IRCLMeshParser::CreateForFile(Path);
			if (!Parser)
			{
				UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Bind: unsupported file type: %s"), *Path);
				return;
			}

			const FRCLLoadOptions Options;
			FRCLImportData Data;
			ERCLFailureReason Reason = ERCLFailureReason::None;
			FString Error;
			if (!Parser->Parse(Path, FRCLParseSettings::FromLoadOptions(Options), Data, Reason, Error))
			{
				UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Bind: parse failed (%s): %s"), FailureName(Reason), *Error);
				return;
			}

			const double Start = FPlatformTime::Seconds();
			FRCLBindResult Result;
			if (!FRCLSkeletonBinder::Bind(Data, Snapshot, FRCLBindSettings::FromLoadOptions(Options), Result, Reason, Error))
			{
				UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Bind failed (%s): %s"), FailureName(Reason), *Error);
				return;
			}
			const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;

			UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.Bind %s -> %s (%d bones): %d garment bone(s), %.2f ms"),
				*FPaths::GetCleanFilename(Path), *Snapshot.SourceName, Snapshot.NumBones(), Result.BoneNames.Num(), Ms);
			UE_LOG(LogRuntimeClothing, Log, TEXT("  Root: file '%s' %s"), *Result.FileRootName.ToString(),
				Result.bRootMappedByPosition ? *FString::Printf(TEXT("mapped to '%s' by position"), *Snapshot.BoneNames[0].ToString()) : TEXT("matched by name"));
			UE_LOG(LogRuntimeClothing, Log, TEXT("  Matched %d bone(s), unknown %d, reparented %d (%d vertices, %.1f%% of weight)%s"),
				Result.MatchedBones.Num(), Result.UnknownBones.Num(), Result.ReparentedBones.Num(), Result.NumVerticesReparented,
				Result.UnknownWeightShare * 100.0f, Result.bTransferRecommended ? TEXT(", weight transfer recommended") : TEXT(""));
			for (const FName& Unknown : Result.UnknownBones)
			{
				UE_LOG(LogRuntimeClothing, Log, TEXT("    Unknown bone: %s"), *Unknown.ToString());
			}
			UE_LOG(LogRuntimeClothing, Log, TEXT("  Largest bind offset vs body: %.2f cm ('%s'); %d bone(s) over tolerance"),
				Result.MaxBindOffsetCm, *Result.MaxBindOffsetBone.ToString(), Result.BonesOverBindTolerance.Num());
			for (int32 MeshIndex = 0; MeshIndex < Result.MeshInfluences.Num(); ++MeshIndex)
			{
				UE_LOG(LogRuntimeClothing, Log, TEXT("  Garment '%s': %d influences over %d vertices"), *Data.Meshes[MeshIndex].Name,
					Result.MeshInfluences[MeshIndex].Num(), FMath::Max(Result.MeshInfluenceStart[MeshIndex].Num() - 1, 0));
			}
			for (const FString& Warning : Result.Warnings)
			{
				UE_LOG(LogRuntimeClothing, Log, TEXT("  Warning: %s"), *Warning);
			}
		});
	}

	void Build(const TArray<FString>& Args)
	{
		FString Path;
		FRCLSkeletonSnapshot Snapshot;
		if (!PrepareBodyCommand(TEXT("RCL.Build"), Args, Path, Snapshot))
		{
			return;
		}
		// GPU skinning bone limit: read on the game thread, as the real loader will (Milestone 7).
		const int32 MaxGPUSkinBones = FGPUBaseSkinVertexFactory::GetMaxGPUSkinBones();

		Async<void>(EAsyncExecution::ThreadPool, [Path, Snapshot, MaxGPUSkinBones]()
		{
			TUniquePtr<IRCLMeshParser> Parser = IRCLMeshParser::CreateForFile(Path);
			if (!Parser)
			{
				UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Build: unsupported file type: %s"), *Path);
				return;
			}

			const FRCLLoadOptions Options;
			FRCLImportData Data;
			ERCLFailureReason Reason = ERCLFailureReason::None;
			FString Error;
			if (!Parser->Parse(Path, FRCLParseSettings::FromLoadOptions(Options), Data, Reason, Error))
			{
				UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Build: parse failed (%s): %s"), FailureName(Reason), *Error);
				return;
			}

			FRCLBindResult Bound;
			if (!FRCLSkeletonBinder::Bind(Data, Snapshot, FRCLBindSettings::FromLoadOptions(Options), Bound, Reason, Error))
			{
				UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Build: bind failed (%s): %s"), FailureName(Reason), *Error);
				return;
			}

			const FRCLBuildSettings Settings = FRCLBuildSettings::FromLoadOptions(Options, MaxGPUSkinBones);
			UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.Build %s -> %s: %d garment(s), %d garment bone(s), max %d influences, max %d bones per section"),
				*FPaths::GetCleanFilename(Path), *Snapshot.SourceName, Data.Meshes.Num(), Bound.BoneNames.Num(),
				Settings.MaxBoneInfluences, Settings.MaxBonesPerSection);

			for (int32 MeshIndex = 0; MeshIndex < Data.Meshes.Num(); ++MeshIndex)
			{
				const FRCLImportMesh& Mesh = Data.Meshes[MeshIndex];
				FRCLBuiltMesh Built;
				if (!FRCLMeshBuilder::Build(Mesh, Bound.MeshInfluences[MeshIndex], Bound.MeshInfluenceStart[MeshIndex],
					Bound.ParentIndices, Settings, Built, Reason, Error))
				{
					UE_LOG(LogRuntimeClothing, Warning, TEXT("  Garment '%s': build failed (%s): %s"), *Mesh.Name, FailureName(Reason), *Error);
					continue;
				}

				const FRCLBuildStats& Stats = Built.Stats;
				UE_LOG(LogRuntimeClothing, Log, TEXT("  Garment '%s': %d -> %d vertices, %d triangles, %d section(s) (%d split for bones), %d material slot(s), %d active bone(s), %d UV set(s)%s, %d-bit indices, %.2f ms, %.1f KB"),
					*Built.Name, Stats.NumSourceVertices, Stats.NumRenderVertices, Stats.NumTriangles, Stats.NumSections,
					Stats.NumSectionsSplitForBones, Built.MaterialSlots.Num(), Built.ActiveBoneIndices.Num(), Built.NumTexCoords(),
					Built.Colors.Num() > 0 ? TEXT(", vertex colours") : TEXT(""), Stats.bUse32BitIndices ? 32 : 16,
					Stats.BuildTimeMs, Built.GetAllocatedSize() / 1024.0);
				UE_LOG(LogRuntimeClothing, Log, TEXT("    Influences: max %d used, %d in file; %d vertices trimmed (largest dropped weight %.1f%%), %d unweighted"),
					Built.MaxBoneInfluences, Stats.MaxInfluencesInFile, Stats.NumVerticesTrimmed, Stats.MaxWeightTrimmed * 100.0f,
					Stats.NumVerticesUnweighted);
				UE_LOG(LogRuntimeClothing, Log, TEXT("    Tangents: %s, %d without a usable UV direction"),
					Stats.bTangentsComputed ? TEXT("computed") : TEXT("from file"), Stats.NumDegenerateTangents);
				for (int32 SectionIndex = 0; SectionIndex < Built.Sections.Num(); ++SectionIndex)
				{
					const FRCLBuiltSection& Section = Built.Sections[SectionIndex];
					const int32 Material = Built.MaterialSlots[Section.MaterialSlot];
					UE_LOG(LogRuntimeClothing, Log, TEXT("    Section %d: material '%s', %u triangles, %u vertices, %d bones, up to %d influences"),
						SectionIndex, Data.Materials.IsValidIndex(Material) ? *Data.Materials[Material].Name.ToString() : TEXT("?"),
						Section.NumTriangles, Section.NumVertices, Section.BoneMap.Num(), Section.MaxBoneInfluences);
				}
				UE_LOG(LogRuntimeClothing, Log, TEXT("    Bounds: origin %s, extent %s, radius %.1f cm"),
					*Built.Bounds.Origin.ToString(), *Built.Bounds.BoxExtent.ToString(), Built.Bounds.SphereRadius);
				for (const FString& Warning : Built.Warnings)
				{
					UE_LOG(LogRuntimeClothing, Log, TEXT("    Warning: %s"), *Warning);
				}
			}
		});
	}

	/** Components made by RCL.Spawn, so RCL.Clear can remove them. Game thread only. */
	static TArray<TWeakObjectPtr<USkeletalMeshComponent>> SpawnedComponents;

	/** The Play-In-Editor world, or the game world in a standalone game. */
	UWorld* FindPlayWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game) && Context.World())
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	/** How well a component fits as "the body to dress". 0 = not a candidate. */
	struct FBodyCandidate
	{
		USkeletalMeshComponent* Component = nullptr;
		int32 Score = 0;
		bool bOnScreen = false;
		bool bPlayer = false;
		bool bExactMesh = false;
	};

	/**
	 * Every component that could be the body. Only components that drive their own pose count
	 * (MasterPoseComponent unset): clothing shares the skeleton but follows the body, and 4.19 does
	 * not chain master pose. Ranking, strongest first:
	 *   visible and drawn on screen in the last second (helper actors such as
	 *   BP_MorphCollisionCalculator keep a hidden copy of the body), then the player's pawn, then
	 *   the exact body mesh over another mesh on the same skeleton.
	 * ActorFilter, when set, keeps only actors whose name contains it.
	 */
	TArray<FBodyCandidate> FindBodyCandidates(UWorld* World, USkeletalMesh* Body, const FString& ActorFilter)
	{
		const APlayerController* Player = World->GetFirstPlayerController();
		const APawn* PlayerPawn = Player ? Player->GetPawn() : nullptr;
		const float Now = World->GetTimeSeconds();

		TArray<FBodyCandidate> Candidates;
		for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
		{
			USkeletalMeshComponent* Component = *It;
			AActor* Owner = Component->GetOwner();
			if (Component->GetWorld() != World || !Owner || !Component->SkeletalMesh || Component->IsPendingKill()
				|| Component->MasterPoseComponent.IsValid()
				|| SpawnedComponents.Contains(TWeakObjectPtr<USkeletalMeshComponent>(Component)))
			{
				continue;
			}
			if (!ActorFilter.IsEmpty() && !Owner->GetName().Contains(ActorFilter))
			{
				continue;
			}

			FBodyCandidate Candidate;
			Candidate.Component = Component;
			Candidate.bExactMesh = Component->SkeletalMesh == Body;
			if (!Candidate.bExactMesh && Component->SkeletalMesh->Skeleton != Body->Skeleton)
			{
				continue;
			}
			Candidate.bOnScreen = Component->IsVisible() && !Component->bHiddenInGame && !Owner->bHidden
				&& Now - Component->LastRenderTimeOnScreen < 1.0f;
			Candidate.bPlayer = PlayerPawn && Owner == PlayerPawn;
			Candidate.Score = (Candidate.bOnScreen ? 8 : 0) + (Candidate.bPlayer ? 4 : 0) + (Candidate.bExactMesh ? 2 : 1);
			Candidates.Add(Candidate);
		}
		Candidates.Sort([](const FBodyCandidate& A, const FBodyCandidate& B) { return A.Score > B.Score; });
		return Candidates;
	}

	FString DescribeCandidate(const FBodyCandidate& Candidate)
	{
		return FString::Printf(TEXT("%s (%s), mesh '%s'%s%s%s"),
			*Candidate.Component->GetOwner()->GetName(), *Candidate.Component->GetName(), *Candidate.Component->SkeletalMesh->GetName(),
			Candidate.bOnScreen ? TEXT(", on screen") : TEXT(", not on screen"),
			Candidate.bPlayer ? TEXT(", player") : TEXT(""),
			Candidate.bExactMesh ? TEXT(", exact body mesh") : TEXT(", same skeleton"));
	}

	/** Splits off "-actor=Name" and "-halfuv"; returns the remaining arguments. */
	TArray<FString> TakeSwitches(const TArray<FString>& Args, FString& OutActorFilter, bool& bOutHalfPrecisionUVs)
	{
		TArray<FString> Rest;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("-halfuv"), ESearchCase::IgnoreCase))
			{
				bOutHalfPrecisionUVs = true;
			}
			else if (Arg.StartsWith(TEXT("-actor="), ESearchCase::IgnoreCase))
			{
				OutActorFilter = Arg.Mid(7);
			}
			else
			{
				Rest.Add(Arg);
			}
		}
		return Rest;
	}

	void ListBodies(const TArray<FString>& Args)
	{
		FString ActorFilter;
		bool bUnused = false;
		TArray<FString> Rest = TakeSwitches(Args, ActorFilter, bUnused);

		UWorld* World = FindPlayWorld();
		if (!World)
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.ListBodies: start Play-In-Editor first."));
			return;
		}
		// Only a /Game/ path counts as a body; anything else (a file pasted by mistake) is ignored.
		FString BodyPath = Rest.Num() > 0 && Rest.Last().StartsWith(TEXT("/Game/")) ? Rest.Last() : FString(DefaultBodyPath);
		if (!BodyPath.Contains(TEXT(".")))
		{
			BodyPath += TEXT(".") + FPaths::GetBaseFilename(BodyPath);
		}
		USkeletalMesh* Body = LoadObject<USkeletalMesh>(nullptr, *BodyPath);
		if (!Body)
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.ListBodies: could not load body mesh '%s'."), *BodyPath);
			return;
		}
		const TArray<FBodyCandidate> Candidates = FindBodyCandidates(World, Body, ActorFilter);
		UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.ListBodies: %d candidate(s) for '%s' (first is what RCL.Spawn picks):"), Candidates.Num(), *Body->GetName());
		for (const FBodyCandidate& Candidate : Candidates)
		{
			UE_LOG(LogRuntimeClothing, Log, TEXT("  [%d] %s"), Candidate.Score, *DescribeCandidate(Candidate));
		}
	}

	/** The play world if any, else the first world (the editor's) so RCL.Check also works outside play. */
	UWorld* AnyWorld()
	{
		if (UWorld* Play = FindPlayWorld())
		{
			return Play;
		}
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World())
				{
					return Context.World();
				}
			}
		}
		return nullptr;
	}

	void Spawn(const TArray<FString>& Args)
	{
		// Switches: -halfuv writes 16-bit UVs (the engine import default) for comparison;
		// -actor=Name picks the character whose actor name contains Name (see RCL.ListBodies).
		FString ActorFilter;
		bool bHalfPrecisionUVs = false;
		// Weights and bone orientations come from the body by default (Milestone 8, TD 2026-10-23).
		// -fileweights keeps the file's own instead, to compare; -bodyweights is accepted and changes nothing.
		bool bFileWeights = false;
		TArray<FString> Remaining;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("-fileweights"), ESearchCase::IgnoreCase))
			{
				bFileWeights = true;
			}
			else if (Arg.Equals(TEXT("-bodyweights"), ESearchCase::IgnoreCase))
			{
				// the default now
			}
			else
			{
				Remaining.Add(Arg);
			}
		}
		const TArray<FString> FileArgs = TakeSwitches(Remaining, ActorFilter, bHalfPrecisionUVs);

		UWorld* World = FindPlayWorld();
		if (!World)
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Spawn: start Play-In-Editor first; the garment is put on a character in the running game."));
			return;
		}

		FString Path;
		FRCLSkeletonSnapshot UnusedSnapshot;
		USkeletalMesh* Body = nullptr;
		if (!PrepareBodyCommand(TEXT("RCL.Spawn"), FileArgs, Path, UnusedSnapshot, &Body))
		{
			return;
		}

		const TArray<FBodyCandidate> Candidates = FindBodyCandidates(World, Body, ActorFilter);
		if (Candidates.Num() == 0)
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Spawn: no character in the play world uses '%s' or its skeleton%s."),
				*Body->GetName(), ActorFilter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" with '%s' in its actor name"), *ActorFilter));
			return;
		}
		USkeletalMeshComponent* BodyComponent = Candidates[0].Component;
		UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.Spawn: dressing %s%s"), *DescribeCandidate(Candidates[0]),
			Candidates.Num() > 1 ? *FString::Printf(TEXT(" (%d other candidate(s); RCL.ListBodies lists them)"), Candidates.Num() - 1) : TEXT(""));

		// Same path as the Load Clothing node: the pipeline, then Put Clothing On Character.
		FRCLLoadOptions Options;
		Options.BodyMesh = BodyComponent->SkeletalMesh;   // the body actually worn
		Options.bUseFullPrecisionUVs = !bHalfPrecisionUVs;
		if (bFileWeights)
		{
			Options.SkinningMode = ERCLSkinningMode::FileWeights;
		}
		const TWeakObjectPtr<USkeletalMeshComponent> WeakBody(BodyComponent);

		FRCLLoadPipeline::Get().RequestLoad(World, Path, Options, FRCLLoadDone::CreateLambda(
			[WeakBody, Path](bool bSucceeded, const TArray<USkeletalMesh*>& Meshes, const FRCLLoadReport& Report)
		{
			if (!bSucceeded)
			{
				if (Report.FailureReason != ERCLFailureReason::Cancelled)
				{
					UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Spawn: %s (%s)"), *Report.ErrorMessage, FailureName(Report.FailureReason));
				}
				return;
			}
			USkeletalMeshComponent* BodyComponent = WeakBody.Get();
			if (!BodyComponent)
			{
				UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Spawn: the character went away before the garment was ready."));
				return;
			}
			for (USkeletalMesh* Garment : Meshes)
			{
				USkeletalMeshComponent* Component = URCLClothingLibrary::PutClothingOnCharacter(BodyComponent, Garment, NAME_None, FTransform::Identity);
				if (!Component)
				{
					continue;
				}
				SpawnedComponents.Add(Component);
				UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.Spawn %s: '%s' on %s (%s), master pose; %d vertices, %d section(s), %d material slot(s), %d morph target(s), fit %.0f%%%s; worker %.1f ms, game thread %.2f ms"),
					*FPaths::GetCleanFilename(Path), *Garment->GetName(), *BodyComponent->GetOwner()->GetName(), *BodyComponent->GetName(),
					Report.NumVertices, Report.NumSections, Garment->Materials.Num(), Garment->MorphTargets.Num(), Report.TransferMatchRatio * 100.0f,
					Report.bUsedWeightTransfer ? TEXT(", weights from the body") : TEXT(""), Report.WorkerTimeMs, Report.GameThreadTimeMs);
			}
			for (const FString& Warning : Report.Warnings)
			{
				UE_LOG(LogRuntimeClothing, Log, TEXT("  Warning: %s"), *Warning);
			}
		}));
	}

	void Check(const TArray<FString>& Args)
	{
		UWorld* World = AnyWorld();
		FString Path;
		FRCLSkeletonSnapshot UnusedSnapshot;
		USkeletalMesh* Body = nullptr;
		if (!World || !PrepareBodyCommand(TEXT("RCL.Check"), Args, Path, UnusedSnapshot, &Body))
		{
			return;
		}
		FRCLLoadOptions Options;
		Options.BodyMesh = Body;
		FRCLLoadPipeline::Get().RequestCheck(World, Path, Options, FRCLCheckDone::CreateLambda(
			[Path](bool bUsable, const FRCLCheckResult& Result)
		{
			const FRCLLoadReport& Report = Result.Report;
			UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.Check %s: %s%s"), *FPaths::GetCleanFilename(Path),
				bUsable ? TEXT("usable") : TEXT("NOT usable: "), bUsable ? TEXT("") : *Report.ErrorMessage);
			UE_LOG(LogRuntimeClothing, Log, TEXT("  %d vertices, %d triangles, %d section(s); %d bone(s) matched, %d unknown; worker %.1f ms"),
				Report.NumVertices, Report.NumTriangles, Report.NumSections, Report.MatchedBones.Num(), Report.UnknownBones.Num(), Report.WorkerTimeMs);
			for (const FRCLMeshNodeInfo& Part : Result.Parts)
			{
				UE_LOG(LogRuntimeClothing, Log, TEXT("  Part '%s': %d vertices, %d triangles%s%s"), *Part.Name, Part.NumVertices, Part.NumTriangles,
					Part.bIsSkinned ? TEXT(", skinned") : TEXT(""), Part.bLooksLikeDazFigure ? TEXT(", DAZ figure (skipped by default)") : TEXT(""));
			}
			for (const FString& Warning : Report.Warnings)
			{
				UE_LOG(LogRuntimeClothing, Log, TEXT("  Warning: %s"), *Warning);
			}
		}));
	}

	void Variant(const TArray<FString>& Args)
	{
		USkeletalMeshComponent* Component = nullptr;
		for (int32 Index = SpawnedComponents.Num() - 1; Index >= 0 && !Component; --Index)
		{
			Component = SpawnedComponents[Index].Get();
		}
		if (Args.Num() != 2 || !Component)
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("Usage: RCL.Variant <slot name> <material object path>, after RCL.Spawn. Example: RCL.Variant suit_front /Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
			return;
		}
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Args[1]);
		if (!Material)
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Variant: could not load material '%s'."), *Args[1]);
			return;
		}
		TMap<FName, UMaterialInterface*> MaterialsBySlot;
		MaterialsBySlot.Add(FName(*Args[0]), Material);
		const int32 NumSet = URCLClothingLibrary::SetClothingMaterials(Component, MaterialsBySlot);
		UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.Variant: set %d slot(s) on '%s'."), NumSet, *Component->GetName());
	}

	/** RCL.Morph [name value]: sets a morph on the BODY the last garment follows, so the garment's copy of
	 *  that morph can be seen following it. With no arguments, lists the garment's morph targets. */
	void Morph(const TArray<FString>& Args)
	{
		USkeletalMeshComponent* Garment = nullptr;
		for (int32 Index = SpawnedComponents.Num() - 1; Index >= 0 && !Garment; --Index)
		{
			Garment = SpawnedComponents[Index].Get();
		}
		USkeletalMeshComponent* Body = Garment ? Cast<USkeletalMeshComponent>(Garment->MasterPoseComponent.Get()) : nullptr;
		if (!Garment || !Body || !Garment->SkeletalMesh)
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("RCL.Morph: spawn a garment with RCL.Spawn first."));
			return;
		}
		if (Args.Num() < 2)
		{
			const TArray<UMorphTarget*>& Morphs = Garment->SkeletalMesh->MorphTargets;
			UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.Morph: '%s' has %d morph target(s). Usage: RCL.Morph <name> <value 0..1>"), *Garment->SkeletalMesh->GetName(), Morphs.Num());
			for (int32 Index = 0; Index < Morphs.Num(); ++Index)
			{
				if (Morphs[Index])
				{
					UE_LOG(LogRuntimeClothing, Log, TEXT("  %s"), *Morphs[Index]->GetName());
				}
			}
			return;
		}
		const float Value = FCString::Atof(*Args[1]);
		Body->SetMorphTarget(FName(*Args[0]), Value);
		UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.Morph: body morph '%s' = %.2f on %s (the garment follows through master pose)."), *Args[0], Value, *Body->GetName());
	}

	void Clear(const TArray<FString>& Args)
	{
		int32 Removed = 0;
		for (const TWeakObjectPtr<USkeletalMeshComponent>& Weak : SpawnedComponents)
		{
			if (USkeletalMeshComponent* Component = Weak.Get())
			{
				Component->DestroyComponent();
				++Removed;
			}
		}
		SpawnedComponents.Reset();
		UE_LOG(LogRuntimeClothing, Log, TEXT("RCL.Clear: removed %d garment(s); their meshes are freed at the next garbage collection."), Removed);
	}
}

void FRuntimeClothingLoaderModule::RegisterConsoleCommands()
{
	IConsoleManager& Console = IConsoleManager::Get();
	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("RCL.Inspect"),
		TEXT("RCL.Inspect <file>: list the mesh parts in an FBX or OBJ file."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RCLConsole::Inspect),
		ECVF_Default));
	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("RCL.Parse"),
		TEXT("RCL.Parse <file>: parse an FBX or OBJ file with default options and log the result."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RCLConsole::Parse),
		ECVF_Default));
	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("RCL.Bind"),
		TEXT("RCL.Bind <file> [/Game/path/to/BodyMesh]: parse a file and bind it to a body's skeleton (default: the female body)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RCLConsole::Bind),
		ECVF_Default));
	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("RCL.Build"),
		TEXT("RCL.Build <file> [/Game/path/to/BodyMesh]: parse, bind and build render data for a file, and log the result (default body: the female body)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RCLConsole::Build),
		ECVF_Default));
	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("RCL.Spawn"),
		TEXT("RCL.Spawn <file> [/Game/path/to/BodyMesh] [-actor=Name] [-halfuv] [-fileweights]: during Play-In-Editor, load a file and put it on the on-screen character wearing that body (default: the female body), following it through master pose."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RCLConsole::Spawn),
		ECVF_Default));
	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("RCL.ListBodies"),
		TEXT("RCL.ListBodies [/Game/path/to/BodyMesh] [-actor=Name]: during Play-In-Editor, list the characters RCL.Spawn could dress, best first."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RCLConsole::ListBodies),
		ECVF_Default));
	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("RCL.Check"),
		TEXT("RCL.Check <file> [/Game/path/to/BodyMesh]: run Check Clothing File on a file and log the result (default body: the female body)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RCLConsole::Check),
		ECVF_Default));
	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("RCL.Variant"),
		TEXT("RCL.Variant <slot name> <material object path>: set one material slot on the last RCL.Spawn garment (Set Clothing Materials)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RCLConsole::Variant),
		ECVF_Default));
	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("RCL.Morph"),
		TEXT("RCL.Morph [name value]: set a body morph to see the last RCL.Spawn garment follow it; no arguments lists the garment's morphs."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RCLConsole::Morph),
		ECVF_Default));
	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("RCL.Clear"),
		TEXT("RCL.Clear: remove every garment added by RCL.Spawn."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RCLConsole::Clear),
		ECVF_Default));
}

void FRuntimeClothingLoaderModule::UnregisterConsoleCommands()
{
	for (IConsoleObject* Command : ConsoleCommands)
	{
		if (Command)
		{
			IConsoleManager::Get().UnregisterConsoleObject(Command);
		}
	}
	ConsoleCommands.Reset();
}
#endif // !UE_BUILD_SHIPPING

void FRuntimeClothingLoaderModule::StartupModule()
{
	UE_LOG(LogRuntimeClothing, Log, TEXT("Runtime Clothing Loader %s started."), RCL_PLUGIN_VERSION);

	// A world being torn down (level change, play stopped) cancels its pending loads.
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* World, bool /*bSessionEnded*/, bool /*bCleanupResources*/)
	{
		FRCLLoadPipeline::Get().CancelAllForWorld(World);
	});

#if !UE_BUILD_SHIPPING
	RegisterConsoleCommands();
#endif
}

void FRuntimeClothingLoaderModule::ShutdownModule()
{
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	FRCLLoadPipeline::Get().Shutdown();
	FRCLBodySnapshotCache::Get().Reset();

#if !UE_BUILD_SHIPPING
	UnregisterConsoleCommands();
#endif
	UE_LOG(LogRuntimeClothing, Log, TEXT("Runtime Clothing Loader shut down."));
}

IMPLEMENT_MODULE(FRuntimeClothingLoaderModule, RuntimeClothingLoader)
