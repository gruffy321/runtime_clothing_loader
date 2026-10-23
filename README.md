# Runtime Clothing Loader (UE 4.19.2)

Runtime plugin that loads FBX (primary) and OBJ clothing from disk and binds it
to a base `USkeleton`, for use with `SetMasterPoseComponent`. No editor
dependencies. Parser: ufbx v0.23.1.

Design document (living, canonical):
https://claude.ai/code/artifact/5aa967c7-c6ee-4bdc-ac25-a4987aa173c6

## Status

| Milestone | State |
| --- | --- |
| 1. Architecture, .uplugin, Build.cs, vendored parser | Signed off 2026-10-20 |
| 2. Core types and module | Signed off 2026-10-20 |
| 3. FBX / OBJ parser (ufbx) | Signed off 2026-10-20 |
| 4. Skeleton binder | Verified 2026-10-21: DAZ bodysuit binds to the female body with 0.00 cm bind offset |
| 5. Mesh builder | Verified 2026-10-21: DAZ bodysuit builds to 8682 vertices, 5 sections, in 1.5 ms (`RCL.Build`) |
| 6. Resource writer | Verified 2026-10-23: DAZ bodysuit worn by the sandbox character through master pose (`RCL.Spawn`, 1.6 ms on the game thread); Shipping build links |
| 7. Blueprint nodes and game integration | Verified 2026-10-23: RCL.Spawn through the pipeline, cache reuse, Set Clothing Materials variant, RCL.Check, nodes visible in Blueprint, Shipping build |

Each verified milestone has a frozen `verified/mN` branch; new work happens on a `milestone-N` branch
and is merged into `main` only after it builds and passes its editor test.

## Install

Copy or junction this folder to `<Project>/Plugins/RuntimeClothingLoader/`.

`EnabledByDefault` is false in the `.uplugin`, so the project ignores the plugin
until it is enabled. To enable it, add
`{ "Name": "RuntimeClothingLoader", "Enabled": true }` to the project's `.uproject`
Plugins list (already done in the test branch).

## Layout

```
RuntimeClothingLoader/
  RuntimeClothingLoader.uplugin
  README.md
  .p4ignore
  Source/
    RuntimeClothingLoader/
      RuntimeClothingLoader.Build.cs
      Public/    (Milestone 2)
      Private/   (Milestone 2)
    ThirdParty/
      ufbx/
        ufbx.Build.cs
        README.md
        LICENSE
        include/
          ufbx.h
          ufbx.c
```
