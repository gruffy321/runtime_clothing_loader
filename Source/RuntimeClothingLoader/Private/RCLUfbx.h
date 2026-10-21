// Runtime Clothing Loader - UE 4.19.2
// The only way plugin code includes ufbx. Keeps third-party warnings and Windows macros out
// of the plugin's own translation units.
//
// Milestone 3 - header for TD review. No implementation yet.
//
// ufbx.c itself is compiled exactly once, by RCLUfbx.cpp (Milestone 3 implementation),
// through the same wrapper. No other file may include ufbx.c.
//
// Checked 2026-10-20: ufbx.c uses none of Unreal's macro names (check, verify, ensure, TEXT,
// min, max) as identifiers; the only matches are inside comments and strings.

#pragma once

#include "CoreMinimal.h"

THIRD_PARTY_INCLUDES_START
#include "ufbx.h"
THIRD_PARTY_INCLUDES_END
