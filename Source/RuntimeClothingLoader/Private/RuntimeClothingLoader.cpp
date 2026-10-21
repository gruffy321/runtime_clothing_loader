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
#endif

#define RCL_PLUGIN_VERSION TEXT("0.3.0")

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
