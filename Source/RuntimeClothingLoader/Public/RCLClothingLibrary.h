// Runtime Clothing Loader - UE 4.19.2
// Milestone 7: plain (non-latent) Blueprint helpers that go with Load Clothing.
//
// Milestone 7 - header for TD review. No implementation yet.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RCLTypes.h"
#include "RCLClothingLibrary.generated.h"

class USkeletalMesh;
class USkeletalMeshComponent;

UCLASS()
class RUNTIMECLOTHINGLOADER_API URCLClothingLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Put Clothing On Character. Creates a skeletal mesh component on the body's actor showing
	 * Clothing, and returns it so the game can store it (FFG_S_ClothingItemData::SkeletalMeshComponent)
	 * and set its materials.
	 *
	 * Two ways to wear it, chosen by AttachBone:
	 *  - None (normal clothing): attached to Body and driven by it through SetMasterPoseComponent, so it
	 *    follows every pose and animation. This is how the RCL.Spawn test works.
	 *  - A bone name (glasses, ears, props): attached to that bone of Body with AttachOffset, the same as
	 *    the game's AttachBone / AttachOffset / FaceOffsets do for built-in items. Load such items with
	 *    SkinningMode RigidToBone and RigidAttachBone = the skeleton root, so the mesh keeps its own
	 *    origin and the offset places it.
	 *
	 * Game thread only. Returns null (and logs why) if Body or Clothing is null, the skeletons differ,
	 * or AttachBone is not a bone of Body.
	 */
	UFUNCTION(BlueprintCallable, Category = "Runtime Clothing")
	static USkeletalMeshComponent* PutClothingOnCharacter(USkeletalMeshComponent* Body, USkeletalMesh* Clothing,
		FName AttachBone, FTransform AttachOffset, FName ComponentName = NAME_None);

	/** Removes a component made by Put Clothing On Character. The mesh is freed by garbage collection
	 *  once nothing else wears it. */
	UFUNCTION(BlueprintCallable, Category = "Runtime Clothing")
	static void RemoveClothingFromCharacter(USkeletalMeshComponent* ClothingComponent);

	/** True for file types Load Clothing reads (.fbx, .obj), by extension only. */
	UFUNCTION(BlueprintPure, Category = "Runtime Clothing")
	static bool IsSupportedClothingFile(const FString& FilePath);

	/**
	 * Every .fbx / .obj directly inside Folder (not subfolders), sorted by name. For a Workshop item,
	 * pass the folder Steam reports for it. Empty if the folder does not exist.
	 */
	UFUNCTION(BlueprintCallable, Category = "Runtime Clothing")
	static TArray<FString> FindClothingFilesInFolder(const FString& Folder);
};
