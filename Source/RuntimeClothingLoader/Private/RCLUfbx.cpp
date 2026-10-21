// Runtime Clothing Loader - UE 4.19.2
// Compiles the vendored ufbx library (ufbx.c) exactly once, as C++, inside the plugin module.
// The module sets bFasterWithoutUnity so this file is never merged into a unity batch with
// plugin code: ufbx.c defines many internal macros and statics that must stay contained.

#include "RCLUfbx.h"

THIRD_PARTY_INCLUDES_START
#include "ufbx.c"
THIRD_PARTY_INCLUDES_END
