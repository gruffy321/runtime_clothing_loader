// Runtime Clothing Loader - UE 4.19.2
// Milestone 7: load pipeline. See RCLLoadPipeline.h for the contract.

#include "RCLLoadPipeline.h"
#include "RuntimeClothingLoader.h"
#include "IRCLMeshParser.h"
#include "RCLImportData.h"
#include "RCLSkeletonSnapshot.h"
#include "RCLSkeletonBinder.h"
#include "RCLMeshBuilder.h"
#include "RCLMeshWriter.h"
#include "Async/Async.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Animation/Skeleton.h"
#include "GPUSkinVertexFactory.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

// ---- Cache key ----------------------------------------------------------------------------------

bool FRCLCacheKey::operator==(const FRCLCacheKey& Other) const
{
	return FileSize == Other.FileSize && FileTimestamp == Other.FileTimestamp && OptionsHash == Other.OptionsHash
		&& FilePath.Equals(Other.FilePath, ESearchCase::IgnoreCase) && BodyMeshPath == Other.BodyMeshPath;
}

uint32 GetTypeHash(const FRCLCacheKey& Key)
{
	uint32 Hash = GetTypeHash(Key.FilePath.ToLower());
	Hash = HashCombine(Hash, GetTypeHash(Key.FileSize));
	Hash = HashCombine(Hash, GetTypeHash(Key.FileTimestamp));
	Hash = HashCombine(Hash, GetTypeHash(Key.BodyMeshPath));
	return HashCombine(Hash, Key.OptionsHash);
}

bool FRCLCacheKey::Make(const FString& FilePath, const FRCLLoadOptions& Options, FRCLCacheKey& OutKey)
{
	OutKey.FilePath = FPaths::ConvertRelativePathToFull(FilePath);
	FPaths::NormalizeFilename(OutKey.FilePath);
	OutKey.FileSize = IFileManager::Get().FileSize(*OutKey.FilePath);
	if (OutKey.FileSize < 0)
	{
		return false;
	}
	OutKey.FileTimestamp = IFileManager::Get().GetTimeStamp(*OutKey.FilePath);
	OutKey.BodyMeshPath = Options.BodyMesh ? Options.BodyMesh->GetPathName() : FString();

	// Every option that changes the built geometry or skinning. Materials are deliberately absent.
	uint32 Hash = GetTypeHash((uint32)Options.SkinningMode);
	Hash = HashCombine(Hash, GetTypeHash(Options.RigidAttachBone));
	Hash = HashCombine(Hash, GetTypeHash((uint32)Options.UnknownBonePolicy));
	Hash = HashCombine(Hash, GetTypeHash(Options.MaxBoneInfluences));
	Hash = HashCombine(Hash, GetTypeHash((uint32)Options.Grouping));
	for (const FString& Name : Options.IncludeMeshNodes)
	{
		Hash = HashCombine(Hash, GetTypeHash(Name));
	}
	Hash = HashCombine(Hash, 0x9e3779b9u);   // separates the include and exclude lists
	for (const FString& Name : Options.ExcludeMeshNodes)
	{
		Hash = HashCombine(Hash, GetTypeHash(Name));
	}
	Hash = HashCombine(Hash, GetTypeHash((uint32)Options.bExcludeDazFigureMeshes));
	Hash = HashCombine(Hash, GetTypeHash(Options.ImportScale));
	Hash = HashCombine(Hash, GetTypeHash((uint32)Options.bConvertToUnrealSpace));
	Hash = HashCombine(Hash, GetTypeHash((uint32)Options.bEnablePerPolyCollision));
	Hash = HashCombine(Hash, GetTypeHash((uint32)Options.bUseFullPrecisionUVs));
	Hash = HashCombine(Hash, GetTypeHash((uint32)Options.bTransferMorphTargets));
	Hash = HashCombine(Hash, GetTypeHash(Options.MaxVertices));
	Hash = HashCombine(Hash, GetTypeHash(Options.MaxBones));
	for (const USkeletalMesh* Attachment : Options.BodyAttachmentMeshes)
	{
		Hash = HashCombine(Hash, GetTypeHash(Attachment ? Attachment->GetPathName() : FString()));
	}
	OutKey.OptionsHash = Hash;
	return true;
}

// ---- Job ----------------------------------------------------------------------------------------

struct FRCLLoadPipeline::FJob
{
	bool bCheckOnly = false;
	FString FilePath;
	FRCLLoadOptions Options;          // UObject pointers inside are only used on the game thread
	FRCLCacheKey Key;                 // loads only
	TArray<uint32> WaiterIds;

	// Taken on the game thread before dispatch.
	FRCLSkeletonSnapshot Snapshot;
	TWeakObjectPtr<USkeleton> Skeleton;
	TWeakObjectPtr<USkeletalMesh> BodyMesh;
	int32 MaxGPUSkinBones = 256;

	// Filled by the worker.
	bool bWorkerSucceeded = false;
	FRCLImportData Data;
	FRCLBindResult Bound;
	TArray<FRCLBuiltMesh> Built;
	TArray<FRCLMeshNodeInfo> Parts;   // checks only
	FRCLLoadReport Report;
};

// ---- Public -------------------------------------------------------------------------------------

FRCLLoadPipeline& FRCLLoadPipeline::Get()
{
	static FRCLLoadPipeline Instance;
	return Instance;
}

uint32 FRCLLoadPipeline::RequestLoad(UWorld* World, const FString& FilePath, const FRCLLoadOptions& Options, FRCLLoadDone Done)
{
	check(IsInGameThread());
	const uint32 Id = NextRequestId++;
	FWaiter& Waiter = ActiveRequests.Add(Id);
	Waiter.World = World;
	Waiter.LoadDone = Done;
	Waiter.FilePath = FilePath;

	FRCLLoadReport Report;
	Report.SourceFile = FilePath;
	if (bShuttingDown)
	{
		SetFailure(Report, ERCLFailureReason::Cancelled, TEXT("The loader is shutting down."));
		AnswerLoadLater(Id, false, {}, Report);
		return Id;
	}
	if (!ValidateRequest(FilePath, Options, Report))
	{
		AnswerLoadLater(Id, false, {}, Report);
		return Id;
	}

	FRCLCacheKey Key;
	if (!FRCLCacheKey::Make(FilePath, Options, Key))
	{
		SetFailure(Report, ERCLFailureReason::FileNotFound, FString::Printf(TEXT("File not found: %s"), *FilePath));
		AnswerLoadLater(Id, false, {}, Report);
		return Id;
	}

	TArray<USkeletalMesh*> Cached;
	if (FindCached(Key, Cached))
	{
		UE_LOG(LogRuntimeClothing, Log, TEXT("Load Clothing: '%s' served from the cache (%d mesh(es))."), *FPaths::GetCleanFilename(FilePath), Cached.Num());
		Report.Warnings.Add(TEXT("Reused an already loaded copy of this item."));
		TArray<TWeakObjectPtr<USkeletalMesh>> Weak;
		for (USkeletalMesh* Mesh : Cached)
		{
			Weak.Add(Mesh);
		}
		AnswerLoadLater(Id, true, Weak, Report);
		return Id;
	}

	if (TSharedRef<FJob, ESPMode::ThreadSafe>* Running = InFlightLoads.Find(Key))
	{
		(*Running)->WaiterIds.Add(Id);   // same item already being built: share it
		return Id;
	}

	TSharedRef<FJob, ESPMode::ThreadSafe> Job = MakeShared<FJob, ESPMode::ThreadSafe>();
	Job->FilePath = FilePath;
	Job->Options = Options;
	Job->Key = Key;
	Job->WaiterIds.Add(Id);
	Job->BodyMesh = Options.BodyMesh;
	Job->Skeleton = Options.BodyMesh->Skeleton;
	Job->Snapshot = FRCLSkeletonSnapshot::FromSkeletalMesh(*Options.BodyMesh);
	Job->MaxGPUSkinBones = FGPUBaseSkinVertexFactory::GetMaxGPUSkinBones();
	Job->Report.SourceFile = FilePath;
	InFlightLoads.Add(Key, Job);
	Enqueue(Job);
	return Id;
}

uint32 FRCLLoadPipeline::RequestCheck(UWorld* World, const FString& FilePath, const FRCLLoadOptions& Options, FRCLCheckDone Done)
{
	check(IsInGameThread());
	const uint32 Id = NextRequestId++;
	FWaiter& Waiter = ActiveRequests.Add(Id);
	Waiter.World = World;
	Waiter.CheckDone = Done;
	Waiter.FilePath = FilePath;

	FRCLCheckResult Result;
	Result.Report.SourceFile = FilePath;
	if (bShuttingDown)
	{
		SetFailure(Result.Report, ERCLFailureReason::Cancelled, TEXT("The loader is shutting down."));
		AnswerCheckLater(Id, false, Result);
		return Id;
	}
	if (!ValidateRequest(FilePath, Options, Result.Report))
	{
		AnswerCheckLater(Id, false, Result);
		return Id;
	}

	TSharedRef<FJob, ESPMode::ThreadSafe> Job = MakeShared<FJob, ESPMode::ThreadSafe>();
	Job->bCheckOnly = true;
	Job->FilePath = FilePath;
	Job->Options = Options;
	Job->WaiterIds.Add(Id);
	Job->BodyMesh = Options.BodyMesh;
	Job->Skeleton = Options.BodyMesh->Skeleton;
	Job->Snapshot = FRCLSkeletonSnapshot::FromSkeletalMesh(*Options.BodyMesh);
	Job->MaxGPUSkinBones = FGPUBaseSkinVertexFactory::GetMaxGPUSkinBones();
	Job->Report.SourceFile = FilePath;
	Enqueue(Job);
	return Id;
}

void FRCLLoadPipeline::Cancel(uint32 RequestId)
{
	check(IsInGameThread());
	FWaiter Waiter;
	if (!ActiveRequests.RemoveAndCopyValue(RequestId, Waiter))
	{
		return;
	}
	FRCLLoadReport Report;
	Report.SourceFile = Waiter.FilePath;
	SetFailure(Report, ERCLFailureReason::Cancelled, FString::Printf(TEXT("Request %u cancelled."), RequestId));
	if (Waiter.LoadDone.IsBound())
	{
		Waiter.LoadDone.Execute(false, TArray<USkeletalMesh*>(), Report);
	}
	else
	{
		FRCLCheckResult Result;
		Result.Report = Report;
		Waiter.CheckDone.ExecuteIfBound(false, Result);
	}
}

void FRCLLoadPipeline::CancelAllForWorld(UWorld* World)
{
	TArray<uint32> Doomed;
	for (const TPair<uint32, FWaiter>& Pair : ActiveRequests)
	{
		if (!Pair.Value.World.IsValid() || Pair.Value.World.Get() == World)
		{
			Doomed.Add(Pair.Key);
		}
	}
	for (uint32 Id : Doomed)
	{
		Cancel(Id);
	}
}

void FRCLLoadPipeline::Shutdown()
{
	bShuttingDown = true;
	TArray<uint32> All;
	ActiveRequests.GetKeys(All);
	for (uint32 Id : All)
	{
		Cancel(Id);
	}
	Queue.Reset();
	InFlightLoads.Reset();
	Cache.Reset();

	// Worker lambdas hold their own job references; wait so none outlives the module's code.
	const double GiveUpAt = FPlatformTime::Seconds() + 10.0;
	while (WorkersInFlight.GetValue() > 0 && FPlatformTime::Seconds() < GiveUpAt)
	{
		FPlatformProcess::Sleep(0.01f);
	}
	if (WorkersInFlight.GetValue() > 0)
	{
		UE_LOG(LogRuntimeClothing, Warning, TEXT("Shutdown: %d clothing load(s) still running after 10 s."), WorkersInFlight.GetValue());
	}
}

// ---- Steps --------------------------------------------------------------------------------------

bool FRCLLoadPipeline::ValidateRequest(const FString& FilePath, const FRCLLoadOptions& Options, FRCLLoadReport& OutReport) const
{
	if (FilePath.IsEmpty())
	{
		SetFailure(OutReport, ERCLFailureReason::FileNotFound, TEXT("Empty file path."));
		return false;
	}
	const FString Extension = FPaths::GetExtension(FilePath).ToLower();
	if (Extension != TEXT("fbx") && Extension != TEXT("obj"))
	{
		SetFailure(OutReport, ERCLFailureReason::UnsupportedFormat, FString::Printf(TEXT("Unsupported extension '.%s': %s"), *Extension, *FilePath));
		return false;
	}
	const int64 Size = IFileManager::Get().FileSize(*FilePath);
	if (Size < 0)
	{
		SetFailure(OutReport, ERCLFailureReason::FileNotFound, FString::Printf(TEXT("File not found: %s"), *FilePath));
		return false;
	}
	if (Size > (int64)FMath::Max(Options.MaxFileSizeMB, 1) * 1024 * 1024)
	{
		SetFailure(OutReport, ERCLFailureReason::FileTooLarge, FString::Printf(TEXT("%lld bytes, limit %d MB: %s"), Size, Options.MaxFileSizeMB, *FilePath),
			FString::Printf(TEXT("The clothing file is larger than %d MB."), Options.MaxFileSizeMB));
		return false;
	}
	if (!Options.BodyMesh || !Options.BodyMesh->Skeleton)
	{
		SetFailure(OutReport, ERCLFailureReason::MissingBodyMesh, TEXT("Options.BodyMesh is not set, or has no skeleton."));
		return false;
	}
	return true;
}

bool FRCLLoadPipeline::FindCached(const FRCLCacheKey& Key, TArray<USkeletalMesh*>& OutMeshes)
{
	TArray<TWeakObjectPtr<USkeletalMesh>>* Entry = Cache.Find(Key);
	if (!Entry)
	{
		return false;
	}
	for (const TWeakObjectPtr<USkeletalMesh>& Weak : *Entry)
	{
		USkeletalMesh* Mesh = Weak.Get();
		if (!Mesh || Mesh->IsPendingKill())
		{
			// One part was freed: the set is incomplete, so rebuild it rather than return a partial item.
			Cache.Remove(Key);
			OutMeshes.Reset();
			return false;
		}
		OutMeshes.Add(Mesh);
	}
	return OutMeshes.Num() > 0;
}

void FRCLLoadPipeline::Enqueue(const TSharedRef<FJob, ESPMode::ThreadSafe>& Job)
{
	Queue.Add(Job);
	StartQueuedJobs();
}

void FRCLLoadPipeline::StartQueuedJobs()
{
	while (RunningJobs < MaxConcurrentWorkers && Queue.Num() > 0 && !bShuttingDown)
	{
		TSharedRef<FJob, ESPMode::ThreadSafe> Job = Queue[0];
		Queue.RemoveAt(0);
		++RunningJobs;
		WorkersInFlight.Increment();

		Async<void>(EAsyncExecution::ThreadPool, [Job]()
		{
			FRCLLoadPipeline::RunWorker(*Job);
			AsyncTask(ENamedThreads::GameThread, [Job]()
			{
				FRCLLoadPipeline& Pipeline = FRCLLoadPipeline::Get();
				if (!Pipeline.bShuttingDown)
				{
					Pipeline.CompleteOnGameThread(Job);
				}
			});
			FRCLLoadPipeline::Get().WorkersInFlight.Decrement();
		});
	}
}

void FRCLLoadPipeline::RunWorker(FJob& Job)
{
	const double Start = FPlatformTime::Seconds();
	FRCLLoadReport& Report = Job.Report;
	ERCLFailureReason Reason = ERCLFailureReason::None;
	FString Error;

	TUniquePtr<IRCLMeshParser> Parser = IRCLMeshParser::CreateForFile(Job.FilePath);
	if (!Parser)
	{
		SetFailure(Report, ERCLFailureReason::UnsupportedFormat, FString::Printf(TEXT("No parser for %s"), *Job.FilePath));
		return;
	}
	const FRCLParseSettings ParseSettings = FRCLParseSettings::FromLoadOptions(Job.Options);
	Report.Format = Parser->DetectFormat(Job.FilePath);

	if (Job.bCheckOnly && !Parser->Inspect(Job.FilePath, ParseSettings, Job.Parts, Reason, Error))
	{
		SetFailure(Report, Reason, Error);
		return;
	}

	if (!Parser->Parse(Job.FilePath, ParseSettings, Job.Data, Reason, Error))
	{
		SetFailure(Report, Reason, Error);
		return;
	}
	Report.Warnings.Append(Job.Data.Warnings);
	for (const FRCLImportMesh& Mesh : Job.Data.Meshes)
	{
		Report.MeshNodes.Append(Mesh.SourceNodes);
	}

	if (!FRCLSkeletonBinder::Bind(Job.Data, Job.Snapshot, FRCLBindSettings::FromLoadOptions(Job.Options), Job.Bound, Reason, Error))
	{
		SetFailure(Report, Reason, Error);
		return;
	}
	Report.MatchedBones = Job.Bound.MatchedBones;
	Report.UnknownBones = Job.Bound.UnknownBones;
	Report.Warnings.Append(Job.Bound.Warnings);

	const FRCLBuildSettings BuildSettings = FRCLBuildSettings::FromLoadOptions(Job.Options, Job.MaxGPUSkinBones);
	TArray<FString> FailedParts;
	FString FirstError;
	ERCLFailureReason FirstReason = ERCLFailureReason::None;
	for (int32 MeshIndex = 0; MeshIndex < Job.Data.Meshes.Num(); ++MeshIndex)
	{
		const FRCLImportMesh& Mesh = Job.Data.Meshes[MeshIndex];
		if (Job.Bound.MeshInfluences[MeshIndex].Num() == 0)
		{
			// Unweighted garment in Auto mode: needs weight transfer (Milestone 8) or a rigid bone.
			FailedParts.Add(Mesh.Name);
			if (FirstReason == ERCLFailureReason::None)
			{
				FirstReason = ERCLFailureReason::InternalError;
				FirstError = FString::Printf(TEXT("Garment '%s' has no skin weights."), *Mesh.Name);
			}
			continue;
		}

		FRCLBuiltMesh Built;
		if (!FRCLMeshBuilder::Build(Mesh, Job.Bound.MeshInfluences[MeshIndex], Job.Bound.MeshInfluenceStart[MeshIndex],
			Job.Bound.ParentIndices, BuildSettings, Built, Reason, Error))
		{
			FailedParts.Add(Mesh.Name);
			if (FirstReason == ERCLFailureReason::None)
			{
				FirstReason = Reason;
				FirstError = Error;
			}
			continue;
		}
		Report.NumVertices += Built.NumVertices();
		Report.NumTriangles += Built.Stats.NumTriangles;
		Report.NumSections += Built.Sections.Num();
		Report.Warnings.Append(Built.Warnings);
		Job.Built.Add(MoveTemp(Built));
	}

	if (Job.Built.Num() == 0)
	{
		const bool bUnweighted = FirstError.Contains(TEXT("no skin weights"));
		SetFailure(Report, FirstReason, FirstError, bUnweighted
			? FString(TEXT("This clothing has no skin weights. Set it to follow a bone, or re-export it rigged to the character."))
			: FString());
		return;
	}
	for (const FString& Part : FailedParts)
	{
		Report.Warnings.Add(FString::Printf(TEXT("Part '%s' was skipped: it could not be built."), *Part));
	}

	Report.WorkerTimeMs = (float)((FPlatformTime::Seconds() - Start) * 1000.0);
	Job.bWorkerSucceeded = true;
}

void FRCLLoadPipeline::CompleteOnGameThread(const TSharedRef<FJob, ESPMode::ThreadSafe>& Job)
{
	--RunningJobs;
	FRCLLoadReport& Report = Job->Report;

	if (Job->bCheckOnly)
	{
		FRCLCheckResult Result;
		Result.Report = Report;
		Result.Parts = Job->Parts;
		for (uint32 Id : Job->WaiterIds)
		{
			AnswerCheck(Id, Job->bWorkerSucceeded, Result);
		}
		StartQueuedJobs();
		return;
	}

	InFlightLoads.Remove(Job->Key);

	TArray<USkeletalMesh*> Meshes;
	bool bSucceeded = Job->bWorkerSucceeded;
	if (bSucceeded)
	{
		const double Start = FPlatformTime::Seconds();
		USkeleton* Skeleton = Job->Skeleton.Get();
		if (!Skeleton || !Job->BodyMesh.IsValid())
		{
			SetFailure(Report, ERCLFailureReason::MissingTargetSkeleton, TEXT("The body or its skeleton was unloaded during the load."));
			bSucceeded = false;
		}
		else
		{
			const FRCLWriteSettings WriteSettings = FRCLWriteSettings::FromLoadOptions(Job->Options, Skeleton);
			for (const FRCLBuiltMesh& Built : Job->Built)
			{
				ERCLFailureReason Reason = ERCLFailureReason::None;
				FString Error;
				USkeletalMesh* Mesh = FRCLMeshWriter::Create(Built, Job->Bound, Job->Data, WriteSettings, Reason, Error);
				if (!Mesh)
				{
					SetFailure(Report, Reason, Error);
					bSucceeded = false;
					Meshes.Reset();
					break;
				}
				Meshes.Add(Mesh);
			}
		}
		Report.GameThreadTimeMs = (float)((FPlatformTime::Seconds() - Start) * 1000.0);
	}

	if (bSucceeded)
	{
		TArray<TWeakObjectPtr<USkeletalMesh>>& Entry = Cache.FindOrAdd(Job->Key);
		Entry.Reset();
		for (USkeletalMesh* Mesh : Meshes)
		{
			Entry.Add(Mesh);
		}
		UE_LOG(LogRuntimeClothing, Log, TEXT("Load Clothing: '%s' built %d mesh(es), %d vertices; worker %.1f ms, game thread %.2f ms."),
			*FPaths::GetCleanFilename(Job->FilePath), Meshes.Num(), Report.NumVertices, Report.WorkerTimeMs, Report.GameThreadTimeMs);
	}

	for (uint32 Id : Job->WaiterIds)
	{
		AnswerLoad(Id, bSucceeded, Meshes, Report);
	}
	StartQueuedJobs();
}

void FRCLLoadPipeline::AnswerLoad(uint32 RequestId, bool bSucceeded, const TArray<USkeletalMesh*>& Meshes, const FRCLLoadReport& Report)
{
	FWaiter Waiter;
	if (!ActiveRequests.RemoveAndCopyValue(RequestId, Waiter))
	{
		return;   // cancelled earlier
	}
	if (!Waiter.World.IsValid())
	{
		FRCLLoadReport Dropped = Report;
		SetFailure(Dropped, ERCLFailureReason::Cancelled, TEXT("The world was torn down during the load."));
		Waiter.LoadDone.ExecuteIfBound(false, TArray<USkeletalMesh*>(), Dropped);
		return;
	}
	Waiter.LoadDone.ExecuteIfBound(bSucceeded, Meshes, Report);
}

void FRCLLoadPipeline::AnswerCheck(uint32 RequestId, bool bUsable, const FRCLCheckResult& Result)
{
	FWaiter Waiter;
	if (!ActiveRequests.RemoveAndCopyValue(RequestId, Waiter))
	{
		return;
	}
	Waiter.CheckDone.ExecuteIfBound(bUsable && Waiter.World.IsValid(), Result);
}

void FRCLLoadPipeline::AnswerLoadLater(uint32 RequestId, bool bSucceeded, const TArray<TWeakObjectPtr<USkeletalMesh>>& Meshes, const FRCLLoadReport& Report)
{
	AsyncTask(ENamedThreads::GameThread, [RequestId, bSucceeded, Meshes, Report]()
	{
		FRCLLoadPipeline& Pipeline = FRCLLoadPipeline::Get();
		TArray<USkeletalMesh*> Live;
		bool bOk = bSucceeded;
		for (const TWeakObjectPtr<USkeletalMesh>& Weak : Meshes)
		{
			if (USkeletalMesh* Mesh = Weak.Get())
			{
				Live.Add(Mesh);
			}
			else
			{
				bOk = false;   // freed between the cache lookup and this tick (very unlikely)
			}
		}
		FRCLLoadReport Final = Report;
		if (bSucceeded && !bOk)
		{
			Live.Reset();
			SetFailure(Final, ERCLFailureReason::InternalError, TEXT("A cached mesh was freed before it could be returned."),
				TEXT("The clothing could not be prepared. Please try again."));
		}
		Pipeline.AnswerLoad(RequestId, bOk, Live, Final);
	});
}

void FRCLLoadPipeline::AnswerCheckLater(uint32 RequestId, bool bUsable, const FRCLCheckResult& Result)
{
	AsyncTask(ENamedThreads::GameThread, [RequestId, bUsable, Result]()
	{
		FRCLLoadPipeline::Get().AnswerCheck(RequestId, bUsable, Result);
	});
}

void FRCLLoadPipeline::SetFailure(FRCLLoadReport& Report, ERCLFailureReason Reason, const FString& Detail, const FString& PlayerMessage)
{
	Report.FailureReason = Reason;
	if (!PlayerMessage.IsEmpty())
	{
		Report.ErrorMessage = PlayerMessage;
	}
	else
	{
		switch (Reason)
		{
		case ERCLFailureReason::FileNotFound:          Report.ErrorMessage = TEXT("The clothing file could not be found."); break;
		case ERCLFailureReason::FileTooLarge:          Report.ErrorMessage = TEXT("The clothing file is too large."); break;
		case ERCLFailureReason::UnsupportedFormat:     Report.ErrorMessage = TEXT("Only .fbx and .obj clothing files are supported."); break;
		case ERCLFailureReason::ParseError:            Report.ErrorMessage = TEXT("The clothing file could not be read. It may be damaged."); break;
		case ERCLFailureReason::TooManyVertices:       Report.ErrorMessage = TEXT("The clothing has too many vertices."); break;
		case ERCLFailureReason::TooManyBones:          Report.ErrorMessage = TEXT("The clothing uses too many bones."); break;
		case ERCLFailureReason::NoMeshInFile:          Report.ErrorMessage = TEXT("No clothing mesh was found in the file."); break;
		case ERCLFailureReason::UnknownBonesRejected:  Report.ErrorMessage = TEXT("The clothing uses bones the character does not have."); break;
		case ERCLFailureReason::MissingTargetSkeleton: Report.ErrorMessage = TEXT("The clothing could not be fitted to the character's skeleton."); break;
		case ERCLFailureReason::MissingBodyMesh:       Report.ErrorMessage = TEXT("No character body was given to fit the clothing to."); break;
		case ERCLFailureReason::Cancelled:             Report.ErrorMessage = TEXT("Loading was cancelled."); break;
		default:                                       Report.ErrorMessage = TEXT("The clothing could not be prepared."); break;
		}
	}
	if (Reason != ERCLFailureReason::Cancelled)
	{
		UE_LOG(LogRuntimeClothing, Warning, TEXT("Load Clothing failed for '%s': %s"), *Report.SourceFile, *Detail);
	}
}
