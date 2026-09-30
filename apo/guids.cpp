// guids.cpp -- the single definition site for our GUIDs.
//
// guids.h uses DEFINE_GUID, which emits a *declaration* unless INITGUID (via
// <initguid.h>) is defined first. This TU defines it, so every other TU that
// includes guids.h just references the symbols. Compiled into both the APO
// DLL and the UI app.

#include <initguid.h>
#include "guids.h"
