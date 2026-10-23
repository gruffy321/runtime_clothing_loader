// Runtime Clothing Loader - UE 4.19.2
// Milestone 7: Load Clothing / Check Clothing File nodes. See RCLLoadClothingAsync.h.

#include "RCLLoadClothingAsync.h"
#include "RuntimeClothingLoader.h"
#include "RCLLoadPipeline.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace RCLNodes
{
	/** 4.19's UBlueprintAsyncActionBase only sets RF_StrongRefOnFrame, which protects the node while the
	 *  Blueprint frame that made it references it. A load outlives that frame, so the node roots itself
	 *  while it works (also keeping its UPROPERTY Options alive) and every exit unroots it. */
	void Hold(UObject* Node) { Node->AddToRoot(); }
	void Release(UBlueprintAsyncActionBase* Node)
	{
		Node->SetReadyToDestroy();
		if (Node->IsRooted())
		{
			Node->RemoveFromRoot();
		}
	}

	UWorld* WorldOf(const TWeakObjectPtr<UObject>& WorldContext)
	{
		return GEngine && WorldContext.IsValid()
			? GEngine->GetWorldFromContextObject(WorldContext.Get(), EGetWorldErrorMode::LogAndReturnNull)
			: nullptr;
	}
}

// ---- Load Clothing -------------------------------------------------------------------------------

URCLLoadClothingAsync* URCLLoadClothingAsync::LoadClothing(UObject* WorldContextObject, const FString& FilePath, const FRCLLoadOptions& Options)
{
	URCLLoadClothingAsync* Node = NewObject<URCLLoadClothingAsync>();
	Node->WorldContext = WorldContextObject;
	Node->FilePath = FilePath;
	Node->Options = Options;
	return Node;
}

void URCLLoadClothingAsync::Activate()
{
	RCLNodes::Hold(this);
	UWorld* World = RCLNodes::WorldOf(WorldContext);
	if (!World)
	{
		FRCLLoadReport Report;
		Report.SourceFile = FilePath;
		Report.FailureReason = ERCLFailureReason::InternalError;
		Report.ErrorMessage = TEXT("The clothing could not be prepared.");
		UE_LOG(LogRuntimeClothing, Warning, TEXT("Load Clothing: no world for the node's context object."));
		OnFailure.Broadcast(TArray<USkeletalMesh*>(), Report);
		RCLNodes::Release(this);
		return;
	}
	RequestId = FRCLLoadPipeline::Get().RequestLoad(World, FilePath, Options,
		FRCLLoadDone::CreateUObject(this, &URCLLoadClothingAsync::Finish));
}

void URCLLoadClothingAsync::Finish(bool bSucceeded, const TArray<USkeletalMesh*>& Meshes, const FRCLLoadReport& Report)
{
	if (Report.FailureReason != ERCLFailureReason::Cancelled)
	{
		if (bSucceeded)
		{
			OnSuccess.Broadcast(Meshes, Report);
		}
		else
		{
			OnFailure.Broadcast(Meshes, Report);
		}
	}
	RCLNodes::Release(this);
}

// ---- Check Clothing File -------------------------------------------------------------------------

URCLCheckClothingAsync* URCLCheckClothingAsync::CheckClothingFile(UObject* WorldContextObject, const FString& FilePath, const FRCLLoadOptions& Options)
{
	URCLCheckClothingAsync* Node = NewObject<URCLCheckClothingAsync>();
	Node->WorldContext = WorldContextObject;
	Node->FilePath = FilePath;
	Node->Options = Options;
	return Node;
}

void URCLCheckClothingAsync::Activate()
{
	RCLNodes::Hold(this);
	UWorld* World = RCLNodes::WorldOf(WorldContext);
	if (!World)
	{
		FRCLCheckResult Result;
		Result.Report.SourceFile = FilePath;
		Result.Report.FailureReason = ERCLFailureReason::InternalError;
		Result.Report.ErrorMessage = TEXT("The clothing could not be checked.");
		UE_LOG(LogRuntimeClothing, Warning, TEXT("Check Clothing File: no world for the node's context object."));
		OnChecked.Broadcast(false, Result);
		RCLNodes::Release(this);
		return;
	}
	RequestId = FRCLLoadPipeline::Get().RequestCheck(World, FilePath, Options,
		FRCLCheckDone::CreateUObject(this, &URCLCheckClothingAsync::Finish));
}

void URCLCheckClothingAsync::Finish(bool bUsable, const FRCLCheckResult& Result)
{
	if (Result.Report.FailureReason != ERCLFailureReason::Cancelled)
	{
		OnChecked.Broadcast(bUsable, Result);
	}
	RCLNodes::Release(this);
}
