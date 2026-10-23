// Runtime Clothing Loader - UE 4.19.2
// Milestone 7: Blueprint helpers. See RCLClothingLibrary.h.

#include "RCLClothingLibrary.h"
#include "RuntimeClothingLoader.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"

USkeletalMeshComponent* URCLClothingLibrary::PutClothingOnCharacter(USkeletalMeshComponent* Body, USkeletalMesh* Clothing,
	FName AttachBone, FTransform AttachOffset, FName ComponentName)
{
	if (!Body || !Clothing || !Body->SkeletalMesh)
	{
		UE_LOG(LogRuntimeClothing, Warning, TEXT("Put Clothing On Character: the body, its mesh or the clothing is missing."));
		return nullptr;
	}
	AActor* Owner = Body->GetOwner();
	if (!Owner)
	{
		UE_LOG(LogRuntimeClothing, Warning, TEXT("Put Clothing On Character: body component '%s' has no actor."), *Body->GetName());
		return nullptr;
	}
	if (Clothing->Skeleton != Body->SkeletalMesh->Skeleton)
	{
		UE_LOG(LogRuntimeClothing, Warning, TEXT("Put Clothing On Character: '%s' was loaded for a different skeleton than '%s'."),
			*Clothing->GetName(), *Body->SkeletalMesh->GetName());
		return nullptr;
	}
	const bool bAttachToBone = AttachBone != NAME_None;
	if (bAttachToBone && Body->GetBoneIndex(AttachBone) == INDEX_NONE && !Body->DoesSocketExist(AttachBone))
	{
		UE_LOG(LogRuntimeClothing, Warning, TEXT("Put Clothing On Character: '%s' is not a bone or socket of '%s'."),
			*AttachBone.ToString(), *Body->SkeletalMesh->GetName());
		return nullptr;
	}

	const FName BaseName = ComponentName != NAME_None ? ComponentName : Clothing->GetFName();
	USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(Owner,
		MakeUniqueObjectName(Owner, USkeletalMeshComponent::StaticClass(), BaseName), RF_Transient);
	Component->SetSkeletalMesh(Clothing);

	if (bAttachToBone)
	{
		// Same as the game's AttachBone / AttachOffset items: rigid on a bone, placed by the offset.
		Component->SetupAttachment(Body, AttachBone);
		Component->SetRelativeTransform(AttachOffset);
		Component->RegisterComponent();
	}
	else
	{
		// Normal clothing: follows every pose through the body's bones.
		Component->SetupAttachment(Body);
		Component->RegisterComponent();
		Component->SetMasterPoseComponent(Body);   // after registering, as in RCL.Spawn
	}
	return Component;
}

void URCLClothingLibrary::RemoveClothingFromCharacter(USkeletalMeshComponent* ClothingComponent)
{
	if (ClothingComponent && !ClothingComponent->IsPendingKill())
	{
		ClothingComponent->DestroyComponent();
	}
}

int32 URCLClothingLibrary::SetClothingMaterials(USkeletalMeshComponent* ClothingComponent, const TMap<FName, UMaterialInterface*>& MaterialsBySlot)
{
	if (!ClothingComponent || !ClothingComponent->SkeletalMesh)
	{
		UE_LOG(LogRuntimeClothing, Warning, TEXT("Set Clothing Materials: no clothing component or mesh."));
		return 0;
	}
	int32 NumSet = 0;
	for (const TPair<FName, UMaterialInterface*>& Pair : MaterialsBySlot)
	{
		const int32 Slot = ClothingComponent->GetMaterialIndex(Pair.Key);
		if (Slot == INDEX_NONE)
		{
			UE_LOG(LogRuntimeClothing, Warning, TEXT("Set Clothing Materials: '%s' has no material slot named '%s'."),
				*ClothingComponent->SkeletalMesh->GetName(), *Pair.Key.ToString());
			continue;
		}
		ClothingComponent->SetMaterial(Slot, Pair.Value);   // per component: the shared mesh is untouched
		++NumSet;
	}
	return NumSet;
}

bool URCLClothingLibrary::IsSupportedClothingFile(const FString& FilePath)
{
	const FString Extension = FPaths::GetExtension(FilePath).ToLower();
	return Extension == TEXT("fbx") || Extension == TEXT("obj");
}

TArray<FString> URCLClothingLibrary::FindClothingFilesInFolder(const FString& Folder)
{
	TArray<FString> Result;
	if (Folder.IsEmpty() || !IFileManager::Get().DirectoryExists(*Folder))
	{
		return Result;
	}
	for (const TCHAR* Extension : { TEXT("fbx"), TEXT("obj") })
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *Folder, Extension);
		for (const FString& Name : Names)
		{
			Result.Add(FPaths::Combine(Folder, Name));
		}
	}
	Result.Sort();
	return Result;
}
