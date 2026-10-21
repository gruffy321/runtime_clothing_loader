// Runtime Clothing Loader - UE 4.19.2
// Module entry point and shared log category.
//
// Milestone 2 - header for TD review. No implementation yet.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

/** Log category for every message the plugin emits. Verbosity: Log, compiled up to All. */
RUNTIMECLOTHINGLOADER_API DECLARE_LOG_CATEGORY_EXTERN(LogRuntimeClothing, Log, All);

/**
 * Module class for the RuntimeClothingLoader runtime module.
 *
 * Owns nothing at startup: the parser, binder and builders are created per load
 * and live only for the duration of that load. Startup only logs the plugin and
 * ufbx versions so a player's log identifies the build that loaded a garment.
 */
class RUNTIMECLOTHINGLOADER_API FRuntimeClothingLoaderModule : public IModuleInterface
{
public:
	// IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** The module is safe to reload: it holds no global state. */
	virtual bool SupportsDynamicReloading() override { return true; }

	/** Convenience accessor; loads the module if needed. Game thread only. */
	static FRuntimeClothingLoaderModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FRuntimeClothingLoaderModule>("RuntimeClothingLoader");
	}

	/** True once StartupModule has run. Callable from any thread. */
	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("RuntimeClothingLoader");
	}

private:
#if !UE_BUILD_SHIPPING
	/**
	 * Test commands for the editor console and development builds (Milestone 3):
	 *   RCL.Inspect <file>  lists the file's mesh parts (FRCLMeshNodeInfo) in the log.
	 *   RCL.Parse <file>    parses with default options and logs counts, bones, materials,
	 *                       textures, warnings and timings. Runs on a worker thread.
	 *   RCL.Bind <file> [body mesh path]  (Milestone 4) parses, snapshots the body's skeleton on
	 *                       the game thread (default: the female body), binds on a worker and logs
	 *                       root mapping, matched / unknown bones and the largest bind offset.
	 *   RCL.Build <file> [body mesh path]  (Milestone 5) as RCL.Bind, then builds render data for
	 *                       each garment and logs vertices, sections, influences, tangents, bounds.
	 * Not compiled into Shipping builds.
	 */
	void RegisterConsoleCommands();
	void UnregisterConsoleCommands();

	TArray<class IConsoleObject*> ConsoleCommands;
#endif
};
