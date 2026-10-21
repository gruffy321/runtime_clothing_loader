// ufbx v0.23.1 (MIT or public domain) - FBX (binary + ASCII, v3000+) and OBJ loader.
// Pinned commit 26a482ae66871d7de36eb722aa060bce95bce274. See README.md.

using System.IO;
using UnrealBuildTool;

public class ufbx : ModuleRules
{
	public ufbx(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		// System include path: suppresses third-party warnings under UE's warning levels.
		// External modules cannot compile source, so ufbx.c sits beside ufbx.h and is
		// compiled exactly once by being #included from
		// RuntimeClothingLoader/Private/RCLFbxParser.cpp.
		PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "include"));
	}
}
