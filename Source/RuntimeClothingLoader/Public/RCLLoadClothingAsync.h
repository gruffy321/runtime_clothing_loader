// Runtime Clothing Loader - UE 4.19.2
// Milestone 7: the Blueprint nodes the game's Workshop screens use.
//
// Milestone 7 - header for TD review. No implementation yet.
//
// Two latent ("async") Blueprint nodes, both run the heavy work on a worker thread and fire their
// output pin on the game thread a few frames later:
//   Load Clothing        - file path in, finished USkeletalMesh(es) out. Used when equipping.
//   Check Clothing File  - file path in, "usable?" + a readable report out. Nothing is created.
//                          Used on the upload screen before a creator publishes.
//
// The plugin never reads or writes the game's item settings (slot, gender, mask, flags): the game
// keeps those in its own save data (FFG_S_ClothingItemData). The nodes take plain values.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "RCLTypes.h"
#include "RCLLoadClothingAsync.generated.h"

class USkeletalMesh;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FRCLOnClothingLoaded, const TArray<USkeletalMesh*>&, Meshes, const FRCLLoadReport&, Report);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FRCLOnClothingChecked, bool, bUsable, const FRCLCheckResult&, Result);

/**
 * Load Clothing (latent node).
 *
 * Output pins:
 *   On Success - Meshes: one per garment (one for MergeAll, one per part for SplitByMeshNode), each a
 *                transient USkeletalMesh on Options.BodyMesh's skeleton, ready for Put Clothing On
 *                Character or SetSkeletalMesh + SetMasterPoseComponent. Report: counts, bones,
 *                warnings, timings.
 *   On Failure - Report.FailureReason + Report.ErrorMessage (player-readable, one sentence).
 *
 * Exactly one pin fires, once. If the world is torn down first (level change, play stopped), neither
 * fires and the work is discarded.
 *
 * Shared cache: loading the same file for the same body with the same options while an earlier mesh is
 * still in use returns that mesh instead of building another (ten characters wearing one Workshop item
 * build it once). Two loads of the same file started together are merged into one build. Meshes are
 * held weakly: when nothing wears an item any more, garbage collection frees it as usual.
 * Materials are shared with the cached mesh, so per-character colours belong on the component
 * (SetMaterial / dynamic material instances), which is how the game's clothing already works.
 */
UCLASS()
class RUNTIMECLOTHINGLOADER_API URCLLoadClothingAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FRCLOnClothingLoaded OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FRCLOnClothingLoaded OnFailure;

	/**
	 * @param FilePath  Full path to an .fbx or .obj, for example the file inside the folder Steam
	 *                  reports for a subscribed Workshop item.
	 * @param Options   Options.BodyMesh is required (the female or male base body); everything else
	 *                  has working defaults.
	 */
	UFUNCTION(BlueprintCallable, Category = "Runtime Clothing",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Load Clothing"))
	static URCLLoadClothingAsync* LoadClothing(UObject* WorldContextObject, const FString& FilePath, const FRCLLoadOptions& Options);

	/** UBlueprintAsyncActionBase: starts the request (called by the node right after construction). */
	virtual void Activate() override;

private:
	/** Called on the game thread by the loader when the request finishes or is dropped. */
	void Finish(bool bSucceeded, const TArray<USkeletalMesh*>& Meshes, const FRCLLoadReport& Report);

	TWeakObjectPtr<UObject> WorldContext;
	FString FilePath;
	FRCLLoadOptions Options;

	/** Id in the loader's queue; lets the loader drop this request if the world goes away. */
	uint32 RequestId = 0;
};

/**
 * Check Clothing File (latent node).
 *
 * Runs the same parse, bind and build as Load Clothing against Options.BodyMesh, then throws the
 * result away: nothing is created, so it is safe to call on every file a creator picks.
 *
 * On Checked fires once with:
 *   bUsable - true if Load Clothing would succeed with these options.
 *   Result  - the report (as Load Clothing's), plus every mesh part found in the file so the upload
 *             screen can let the creator include or exclude parts.
 */
UCLASS()
class RUNTIMECLOTHINGLOADER_API URCLCheckClothingAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FRCLOnClothingChecked OnChecked;

	UFUNCTION(BlueprintCallable, Category = "Runtime Clothing",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Check Clothing File"))
	static URCLCheckClothingAsync* CheckClothingFile(UObject* WorldContextObject, const FString& FilePath, const FRCLLoadOptions& Options);

	virtual void Activate() override;

private:
	void Finish(bool bUsable, const FRCLCheckResult& Result);

	TWeakObjectPtr<UObject> WorldContext;
	FString FilePath;
	FRCLLoadOptions Options;
	uint32 RequestId = 0;
};
