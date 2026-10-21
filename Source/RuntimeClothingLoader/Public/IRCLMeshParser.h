// Runtime Clothing Loader - UE 4.19.2
// Parser interface: file on disk in, FRCLImportData out. Implemented by FRCLFbxParser (Milestone 3),
// which handles FBX and OBJ through ufbx. glTF can be added later as another implementation.
//
// Milestone 2 - header for TD review. No implementation yet.

#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"
#include "RCLTypes.h"

struct FRCLImportData;

/** The subset of FRCLLoadOptions a parser needs. Plain values only, safe to copy to a worker. */
struct FRCLParseSettings
{
	bool bConvertToUnrealSpace = true;
	float ImportScale = 1.0f;

	/** Checked before reading the file. */
	int64 MaxFileSizeBytes = 64ll * 1024 * 1024;

	/** Checked after parsing, per garment (per FRCLImportMesh); exceeding either fails the parse. */
	int32 MaxVertices = 200000;
	int32 MaxBones = 256;

	/** Read texture references and embedded images (always on from Milestone 3; decoded in Milestone 9). */
	bool bReadTextures = true;

	/** Mesh part selection and grouping, copied from FRCLLoadOptions. */
	ERCLMeshGrouping Grouping = ERCLMeshGrouping::MergeAll;
	TArray<FString> IncludeMeshNodes;
	TArray<FString> ExcludeMeshNodes;
	bool bExcludeDazFigureMeshes = true;

	static FRCLParseSettings FromLoadOptions(const FRCLLoadOptions& Options);
};

/**
 * Turns one file into FRCLImportData.
 *
 * Threading: Parse is called on a worker thread. Implementations must not touch UObjects,
 * must not keep state between calls, and must be safe to run several loads at once.
 *
 * Untrusted input: files come from players and the Steam Workshop. Implementations must
 * never crash on a malformed file, must enforce the limits in FRCLParseSettings, and must
 * not read files outside the source file's own folder.
 */
class RUNTIMECLOTHINGLOADER_API IRCLMeshParser
{
public:
	virtual ~IRCLMeshParser() {}

	/** Format this parser produces for the given path, or Unknown if it can't read it. Any thread. */
	virtual ERCLSourceFormat DetectFormat(const FString& FilePath) const = 0;

	/**
	 * Parse the file. Returns true on success. On failure OutData is left empty, and
	 * OutReason and OutError say why (OutError is a player-readable line).
	 */
	virtual bool Parse(
		const FString& FilePath,
		const FRCLParseSettings& Settings,
		FRCLImportData& OutData,
		ERCLFailureReason& OutReason,
		FString& OutError) = 0;

	/**
	 * List the file's mesh parts without building geometry, so the game can show a player
	 * what is inside and let them choose (InspectClothingFile in Blueprint, Milestone 7).
	 * Same threading and untrusted-input rules as Parse; the file-size limit applies.
	 */
	virtual bool Inspect(
		const FString& FilePath,
		const FRCLParseSettings& Settings,
		TArray<FRCLMeshNodeInfo>& OutNodes,
		ERCLFailureReason& OutReason,
		FString& OutError) = 0;

	/** Parser for this file's extension (.fbx, .obj), or null if no parser supports it. Any thread. */
	static TUniquePtr<IRCLMeshParser> CreateForFile(const FString& FilePath);
};
