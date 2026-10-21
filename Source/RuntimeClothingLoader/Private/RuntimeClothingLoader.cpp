// Runtime Clothing Loader - UE 4.19.2
// Module entry point, plus test console commands in non-Shipping builds.

#include "RuntimeClothingLoader.h"

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
#include "Engine/SkeletalMesh.h"
#include "UObject/UObjectGlobals.h"
#endif

#define RCL_PLUGIN_VERSION TEXT("0.5.0")

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
		});
	}

	/** Default body for RCL.Bind: the female base body (sign-off decision 5). */
	static const TCHAR* DefaultBodyPath = TEXT("/Game/Characters/Base/Female/FemaleBase_8_MorphTools_2.FemaleBase_8_MorphTools_2");

	/**
	 * Shared by RCL.Bind and RCL.Build. Splits "<file> [/Game/BodyMesh]" and, on the game thread,
	 * loads the body and copies its skeleton so the worker never touches UObjects.
	 */
	bool PrepareBodyCommand(const TCHAR* Command, const TArray<FString>& Args, FString& OutPath, FRCLSkeletonSnapshot& OutSnapshot)
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
	// No global state to create: every load builds its own parser, binder and builders.
	UE_LOG(LogRuntimeClothing, Log, TEXT("Runtime Clothing Loader %s started."), RCL_PLUGIN_VERSION);

#if !UE_BUILD_SHIPPING
	RegisterConsoleCommands();
#endif
}

void FRuntimeClothingLoaderModule::ShutdownModule()
{
#if !UE_BUILD_SHIPPING
	UnregisterConsoleCommands();
#endif
	UE_LOG(LogRuntimeClothing, Log, TEXT("Runtime Clothing Loader shut down."));
}

IMPLEMENT_MODULE(FRuntimeClothingLoaderModule, RuntimeClothingLoader)
