// Runtime Clothing Loader - UE 4.19.2
// Milestone 7: runs load and check requests for the Blueprint nodes. One instance, owned by the module.
//
// Milestone 7 - header for TD review. No implementation yet.
//
// Thread plan per request (the same split RCL.Spawn proved in Milestone 6):
//   game thread  - validate inputs, snapshot the body skeleton, look up the cache
//   worker       - parse, bind, build (Milestones 3 to 5), at most MaxConcurrentWorkers at once
//   game thread  - write the mesh (Milestone 6), store it in the cache, fire the node's pin
// UObjects are only touched on the game thread. Worker results travel in a shared, plain-data job.

#pragma once

#include "CoreMinimal.h"
#include "RCLTypes.h"
#include "RCLImportData.h"
#include "RCLSkeletonSnapshot.h"
#include "RCLSkeletonBinder.h"
#include "RCLMeshBuilder.h"

class USkeletalMesh;
class UWorld;

/** Called on the game thread when a load request ends. Not called if the request was dropped. */
DECLARE_DELEGATE_ThreeParams(FRCLLoadDone, bool /*bSucceeded*/, const TArray<USkeletalMesh*>& /*Meshes*/, const FRCLLoadReport& /*Report*/);

/** Called on the game thread when a check request ends. Not called if the request was dropped. */
DECLARE_DELEGATE_TwoParams(FRCLCheckDone, bool /*bUsable*/, const FRCLCheckResult& /*Result*/);

/**
 * Identifies "the same mesh" for the cache: same file content, same body, same options that change the
 * geometry. Materials are not part of the key (they are per component in the game).
 */
struct FRCLCacheKey
{
	FString FilePath;            // full, normalised path
	int64 FileSize = 0;
	FDateTime FileTimestamp;     // a Workshop update replaces the file, so a new timestamp = a new key
	FName BodyMeshPath;          // object path of Options.BodyMesh
	uint32 OptionsHash = 0;      // skinning, parts, scale, influences, limits

	bool operator==(const FRCLCacheKey& Other) const;
	friend uint32 GetTypeHash(const FRCLCacheKey& Key);

	/** Game thread (reads the file's size and timestamp, and the body's path). False if the file is missing. */
	static bool Make(const FString& FilePath, const FRCLLoadOptions& Options, FRCLCacheKey& OutKey);
};

class FRCLLoadPipeline
{
public:
	/** Simultaneous worker jobs. Two keeps memory and CPU use modest while VR frames render;
	 *  further requests wait in a queue, oldest first. */
	enum { MaxConcurrentWorkers = 2 };

	static FRCLLoadPipeline& Get();

	/**
	 * Game thread. Starts (or joins, or answers from the cache) a load. Returns a request id for
	 * Cancel. Done is called exactly once unless the request is cancelled or World goes away first.
	 * A cache hit still answers on the next tick, never inside this call, so Blueprint pins always
	 * fire after the node returns.
	 */
	uint32 RequestLoad(UWorld* World, const FString& FilePath, const FRCLLoadOptions& Options, FRCLLoadDone Done);

	/** Game thread. As RequestLoad, but builds nothing and never uses or fills the cache. */
	uint32 RequestCheck(UWorld* World, const FString& FilePath, const FRCLLoadOptions& Options, FRCLCheckDone Done);

	/** Game thread. Drops a request: its callback will not run. Worker work already started finishes
	 *  and is discarded (or still fills the cache, if another request is waiting on the same key). */
	void Cancel(uint32 RequestId);

	/** Game thread. Drops every request for World (bound to world cleanup by the module). */
	void CancelAllForWorld(UWorld* World);

	/** Module shutdown: cancels everything and waits for running worker jobs to finish. */
	void Shutdown();

private:
	FRCLLoadPipeline() = default;

	/** Plain-data state of one build, shared by the worker and the game thread. */
	struct FJob;

	/** A caller waiting on a job. Several waiters on one job = merged duplicate requests. */
	struct FWaiter
	{
		uint32 RequestId = 0;
		TWeakObjectPtr<UWorld> World;
		FRCLLoadDone LoadDone;     // set for load requests
		FRCLCheckDone CheckDone;   // set for check requests
	};

	// ---- Steps, in order ---------------------------------------------------------------------

	/** Game thread: options, file type, size limit, body present. Fills the report on failure. */
	bool ValidateRequest(const FString& FilePath, const FRCLLoadOptions& Options, FRCLLoadReport& OutReport) const;

	/** Game thread: a live cached mesh for Key, if any (stale weak entries are removed). */
	bool FindCached(const FRCLCacheKey& Key, TArray<USkeletalMesh*>& OutMeshes);

	/** Game thread: queue the job or start it now if a worker slot is free. */
	void Enqueue(TSharedRef<FJob> Job);

	/** Worker: parse, bind, build every garment; fills the job's report and built meshes. */
	static void RunWorker(TSharedRef<FJob> Job);

	/** Game thread: write meshes (loads only), fill the cache, answer every live waiter, start the next job. */
	void CompleteOnGameThread(TSharedRef<FJob> Job);

	/** Fills Report.ErrorMessage from a failure reason and the technical detail (detail goes to the log). */
	static void SetFailure(FRCLLoadReport& Report, ERCLFailureReason Reason, const FString& Detail);

	// ---- State (game thread only) ------------------------------------------------------------

	TMap<FRCLCacheKey, TArray<TWeakObjectPtr<USkeletalMesh>>> Cache;
	TMap<FRCLCacheKey, TSharedRef<FJob>> InFlightLoads;   // merges duplicate loads
	TArray<TSharedRef<FJob>> Queue;
	int32 RunningWorkers = 0;
	uint32 NextRequestId = 1;
};
