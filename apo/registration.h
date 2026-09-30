// registration.h -- one-time system registration for MiniEQ_APO.
//
// Everything here writes to HKLM and therefore requires elevation. The UI
// relaunches itself elevated (or the installer runs elevated) to call these.
//
// Three registration layers, matching how Windows discovers APOs:
//   1. COM class: HKLM\SOFTWARE\Classes\CLSID\{clsid}\InprocServer32
//   2. APO declaration: HKLM\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\{clsid}
//   3. Per-endpoint attach: ...\MMDevices\Audio\Render\{endpoint-guid}\FxProperties
//      value "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" (SFX slot) = "{clsid}"
//      NOTE: the registry key uses the bare endpoint GUID (the part of the
//      MMDevice ID after "{0.0.0.00000000}."), not the full device ID.
//
// Layers 1+2 are per-install (DllRegisterServer). Layer 3 is per-device, done
// from the UI's "Attach to this device" action. This mirrors the layout
// Equalizer APO uses, which is the form Windows reliably honors.

#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes the COM class key for our DLL. Requires elevation.
HRESULT MiniEQ_RegisterComClass(const wchar_t* dllPath);
HRESULT MiniEQ_UnregisterComClass();

// Writes the AudioEngine APO declaration. Requires elevation.
HRESULT MiniEQ_RegisterApoDeclaration();
HRESULT MiniEQ_UnregisterApoDeclaration();

// Creates %PROGRAMDATA%\MiniEQ with a DACL that lets the audio engine
// append to the diagnostic trace log. Requires elevation; best-effort.
HRESULT MiniEQ_EnsureLogDir();

// Attaches/detaches our SFX APO to one render endpoint. Requires elevation.
// NOTE: even elevated, administrators cannot CREATE subkeys under FxProperties;
// the key already exists, so we open it and set the value in place.
HRESULT MiniEQ_AttachToEndpoint(const wchar_t* endpointId);
HRESULT MiniEQ_DetachFromEndpoint(const wchar_t* endpointId);

// True if the SFX slot of this endpoint already points at our APO.
HRESULT MiniEQ_IsAttachedToEndpoint(const wchar_t* endpointId, bool* attached);

#ifdef __cplusplus
}
#endif
