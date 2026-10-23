// Runtime Clothing Loader - UE 4.19.2
// Milestone 7: runs load and check requests for the Blueprint nodes. One instance, owned by the module.
//
// Thread plan per request (the same split RCL.Spawn proved in Milestone 6):
//   game thread  - validate inputs, snapshot the body skeleton, look up the cache, and (Milestone 8)
//                  fetch the shared body snapshot when weights or morphs will be transferred
//   worker       - parse, bind, transfer (Milestone 8), build, at most MaxConcurrentWorkers at once
//   game thread  - write the mesh (Milestone 6), store it in the cache, answer the request
// UObjects are only touched on the game thread. Worker results travel in a shared, plain-data job.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeCounter.h"
#include "UObject/WeakObjectPtr.h"
#include "RCLTypes.h"

class USkeletalMesh;
class UWorld;

/** Game thread. Answers a load request. A dropped request (Cancel, world cleanup) is answered with
 *  bSucceeded false and Report.FailureReason Cancelled, so the caller can clean up. */
DECLARE_DELEGATE_ThreeParams(FRCLLoadDone, bool /*bSucceeded*/, const TArray<USkeletalMesh*>& /*Meshes*/, const FRCLLoadReport& /*Report*/);

/** Game thread. Answers a check request; dropped requests as above. */
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
	FString BodyMeshPath;        // object path of Options.BodyMesh
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
	 * Cancel. Done is called exactly once, always on a later tick (never inside this call), so
	 * Blueprint pins fire after the node returns.
	 */
	uint32 RequestLoad(UWorld* World, const FString& FilePath, const FRCLLoadOptions& Options, FRCLLoadDone Done);

	/** Game thread. As RequestLoad, but builds no mesh and never uses or fills the cache. */
	uint32 RequestCheck(UWorld* World, const FString& FilePath, const FRCLLoadOptions& Options, FRCLCheckDone Done);

	/** Game thread. Answers the request now with Cancelled. Worker work already started finishes and is
	 *  discarded (or still fills the cache, if another request is waiting on the same key). */
	void Cancel(uint32 RequestId);

	/** Game thread. Cancels every request for World, and any whose world is gone (world cleanup). */
	void CancelAllForWorld(UWorld* World);

	/** Module shutdown: cancels everything and waits (up to 10 s) for running worker jobs to finish. */
	void Shutdown();

private:
	FRCLLoadPipeline() = default;

	/** Plain-data state of one build, shared by the worker and the game thread (defined in the .cpp). */
	struct FJob;

	/** A caller waiting for an answer. Several waiters on one job = merged duplicate loads. */
	struct FWaiter
	{
		TWeakObjectPtr<UWorld> World;
		FRCLLoadDone LoadDone;     // set for load requests
		FRCLCheckDone CheckDone;   // set for check requests
		FString FilePath;          // for the Cancelled report
	};

	// ---- Steps, in order ---------------------------------------------------------------------

	/** Game thread: options, file type, size limit, body present. Fills the report on failure. */
	bool ValidateRequest(const FString& FilePath, const FRCLLoadOptions& Options, FRCLLoadReport& OutReport) const;

	/** Game thread: live cached meshes for Key (if all are still alive; stale entries are removed) and
	 *  the report of the build that made them (Milestone 8: a cache hit reports real counts). */
	bool FindCached(const FRCLCacheKey& Key, TArray<USkeletalMesh*>& OutMeshes, FRCLLoadReport& OutReport);

	/** Game thread: queue the job and start jobs while worker slots are free. */
	void Enqueue(const TSharedRef<FJob, ESPMode::ThreadSafe>& Job);
	void StartQueuedJobs();

	/** Worker: parse, bind, build every garment; fills the job's report and built meshes. */
	static void RunWorker(FJob& Job);

	/** Game thread: frees the worker slot; checks are answered now, successful loads go to the write
	 *  queue (Milestone 8), failed loads are finished now. */
	void CompleteOnGameThread(const TSharedRef<FJob, ESPMode::ThreadSafe>& Job);

	/** Game thread, core ticker (Milestone 8): writes queued meshes, oldest load first, within the
	 *  per-frame budget (console variable rcl.WriteBudgetMs, default 2 ms). Returns false (unregisters)
	 *  when the queue is empty. */
	bool TickWrites(float DeltaTime);

	/** Game thread: fill the cache, answer every live waiter, forget the in-flight entry. */
	void FinishLoad(const TSharedRef<FJob, ESPMode::ThreadSafe>& Job, bool bSucceeded, const TArray<USkeletalMesh*>& Meshes);

	/** Game thread: answer one request (if still active) and forget it. */
	void AnswerLoad(uint32 RequestId, bool bSucceeded, const TArray<USkeletalMesh*>& Meshes, const FRCLLoadReport& Report);
	void AnswerCheck(uint32 RequestId, bool bUsable, const FRCLCheckResult& Result);

	/** Answers on the next game-thread tick (used for answers known before any work starts). */
	void AnswerLoadLater(uint32 RequestId, bool bSucceeded, const TArray<TWeakObjectPtr<USkeletalMesh>>& Meshes, const FRCLLoadReport& Report);
	void AnswerCheckLater(uint32 RequestId, bool bUsable, const FRCLCheckResult& Result);

	/** Sets FailureReason, a player-readable ErrorMessage, and logs the technical detail. */
	static void SetFailure(FRCLLoadReport& Report, ERCLFailureReason Reason, const FString& Detail,
		const FString& PlayerMessage = FString());

	// ---- State (game thread only, except WorkersInFlight) ------------------------------------

	TMap<uint32, FWaiter> ActiveRequests;
	struct FCacheEntry
	{
		TArray<TWeakObjectPtr<USkeletalMesh>> Meshes;
		FRCLLoadReport Report;        // of the build that made them
	};
	TMap<FRCLCacheKey, FCacheEntry> Cache;
	TMap<FRCLCacheKey, TSharedRef<FJob, ESPMode::ThreadSafe>> InFlightLoads;   // merges duplicate loads
	TArray<TSharedRef<FJob, ESPMode::ThreadSafe>> Queue;
	TArray<TSharedRef<FJob, ESPMode::ThreadSafe>> WritingJobs;   // worker done, meshes being written over frames
	FDelegateHandle WriteTickerHandle;
	int32 RunningJobs = 0;
	uint32 NextRequestId = 1;
	bool bShuttingDown = false;

	/** Worker lambdas still running; read by Shutdown from the game thread. */
	FThreadSafeCounter WorkersInFlight;
};
