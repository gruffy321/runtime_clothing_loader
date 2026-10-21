// Runtime Clothing Loader - UE 4.19.2
// Runtime-only module. Must build in a Shipping target with no editor modules.

using UnrealBuildTool;

public class RuntimeClothingLoader : ModuleRules
{
	public RuntimeClothingLoader(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			"RHI",
			"ufbx"
		});

		// ufbx reports errors through ufbx_error and never throws,
		// so C++ exceptions stay disabled (engine default).

		// Keep RCLUfbx.cpp (which compiles ufbx.c) out of unity batches: ufbx.c's internal
		// macros and statics must not leak into plugin code. The module is small, so the
		// build-time cost is negligible.
		bFasterWithoutUnity = true;

		// Deliberately absent from every dependency list:
		// UnrealEd, MeshBuilder, MeshUtilities, RawMesh, SkeletalMeshUtilitiesCommon, FBX.
	}
}
