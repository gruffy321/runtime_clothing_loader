# ufbx (vendored)

| Field | Value |
| --- | --- |
| Upstream | https://github.com/ufbx/ufbx |
| Version | v0.23.1 |
| Commit | 26a482ae66871d7de36eb722aa060bce95bce274 |
| Licence | MIT or public domain, at our choice (see LICENSE) |
| Language level | C99, also compiles as C++11 |
| Formats | FBX binary and ASCII (version 3000+), Wavefront OBJ/MTL |

Files taken unmodified from the tag:

| File | SHA-256 |
| --- | --- |
| include/ufbx.h | f787be529af577efc04f3e3e735844a08556718dbb613d1211c2b313d72895dc |
| include/ufbx.c | 4a956c26a708e40d82ecb0aeaee54da709d5ba7c144c9642174970059bda7e1d |

ufbx.c is in include/ on purpose: an External module cannot compile source, so
RuntimeClothingLoader/Private/RCLFbxParser.cpp includes it once. No other file
may include ufbx.c.

Pre-integration check (2026-10-20, GCC, C++11, no exceptions): compiles cleanly;
loads skinned FBX from Blender, 3ds Max and Maya; rejects random and truncated
files with an error instead of crashing. Not yet compiled with VS2015/2017
(UE 4.19 toolchain): first task of Milestone 3.
