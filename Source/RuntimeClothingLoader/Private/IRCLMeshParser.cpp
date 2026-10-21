// Runtime Clothing Loader - UE 4.19.2

#include "IRCLMeshParser.h"
#include "RuntimeClothingLoader.h"
#include "RCLFbxParser.h"
#include "Misc/Paths.h"

FRCLParseSettings FRCLParseSettings::FromLoadOptions(const FRCLLoadOptions& Options)
{
	FRCLParseSettings Settings;

	Settings.bConvertToUnrealSpace = Options.bConvertToUnrealSpace;
	Settings.ImportScale = FMath::Max(Options.ImportScale, 0.001f);

	Settings.MaxFileSizeBytes = int64(FMath::Max(Options.MaxFileSizeMB, 1)) * 1024 * 1024;
	Settings.MaxVertices = FMath::Max(Options.MaxVertices, 3);
	Settings.MaxBones = FMath::Clamp(Options.MaxBones, 1, 256);

	Settings.bReadTextures = true;

	Settings.Grouping = Options.Grouping;
	Settings.IncludeMeshNodes = Options.IncludeMeshNodes;
	Settings.ExcludeMeshNodes = Options.ExcludeMeshNodes;
	Settings.bExcludeDazFigureMeshes = Options.bExcludeDazFigureMeshes;

	return Settings;
}

TUniquePtr<IRCLMeshParser> IRCLMeshParser::CreateForFile(const FString& FilePath)
{
	const FString Extension = FPaths::GetExtension(FilePath).ToLower();
	if (Extension == TEXT("fbx") || Extension == TEXT("obj"))
	{
		return MakeUnique<FRCLFbxParser>();
	}

	UE_LOG(LogRuntimeClothing, Verbose, TEXT("No parser for '%s'."), *FPaths::GetCleanFilename(FilePath));
	return nullptr;
}
