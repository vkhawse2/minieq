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
//      -- or "...},7" (EFX slot) in EFX test builds. The EFX slot runs after
//      all mixing at the endpoint, downstream of the spatial-sound render, so
//      an EQ there is not bypassed when spatial sound is on.
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
HRESULT MiniEQ_EnsureLogDirForInstall();

// Attaches/detaches our APO to one render endpoint, in the active effect
// slot (SFX by default; EFX in MINIEQ_EFX_SLOT test builds). Requires
// elevation.
// In EFX mode, attach first migrates us out of the SFX slot (if we are still
// there from an earlier install) so the engine never instantiates us twice;
// detach sweeps both slots.
// NOTE: even elevated, administrators cannot CREATE subkeys under FxProperties;
// the key already exists, so we open it and set the value in place.
// Returns S_OK when the slot was (re)written and the endpoint device was
// restarted to pick it up; S_FALSE when the slot already pointed at us --
// the cheap repairs still ran, but the slot rewrite and the device restart
// were skipped (the restart briefly moves playing audio to another output).
HRESULT MiniEQ_AttachToEndpoint(const wchar_t* endpointId);
HRESULT MiniEQ_DetachFromEndpoint(const wchar_t* endpointId);

// Forced attach: rewrites the slot and re-enumerates the endpoint device
// even when the slot already points at us. For the "attached but Windows
// never loaded it" case -- the registry is provably right, yet the running
// engine never picked up the registration (e.g. after an upgrade that
// replaced the DLL). Never used silently: every caller goes through a UAC
// prompt or the installer's attach-all, and the device restart briefly moves
// playing audio to another output.
HRESULT MiniEQ_AttachToEndpointEx(const wchar_t* endpointId, bool force);

// True if the active effect slot of this endpoint already points at our APO.
HRESULT MiniEQ_IsAttachedToEndpoint(const wchar_t* endpointId, bool* attached);

// Restarts the Windows device node behind an audio endpoint (disable +
// enable), forcing the OS to re-enumerate it so the audio engine re-reads
// the FxProperties effect list. Raw registry writes to FxProperties don't
// send change notifications, so without this the engine keeps the effect
// list from when the device was last connected. Requires elevation;
// best-effort (returns an error if the device isn't found, but the attach
// itself already succeeded).
HRESULT MiniEQ_ReenumerateEndpointDevice(const wchar_t* endpointId);

// Diagnostics (read-only): is the AudioEngine APO declaration present and
// consistent with GetRegistrationProperties? S_OK always; *present answers.
HRESULT MiniEQ_QueryApoDeclaration(bool* present);

// Diagnostics (read-only): reads one FxProperties slot value.
// S_OK = slot holds a CLSID string (copied to out), S_FALSE = slot absent.
HRESULT MiniEQ_QuerySlotValue(const wchar_t* endpointId, bool efx,
                             wchar_t* out, size_t cch);

// Selects the effect slot at runtime: false = SFX (default), true = EFX.
// Reserved for a future slot-choice UI; nothing calls it yet. Builds compiled
// with MINIEQ_EFX_SLOT start in EFX mode.
void MiniEQ_SetEffectSlot(bool useEfx);

// Short name of the active effect slot ("SFX" or "EFX") for UI labels.
const wchar_t* MiniEQ_EffectSlotShortName();

// R2: APO-chaining bookkeeping. Attach stashes the incumbent SFX-slot CLSID
// (when it is a real third-party APO, not us) under
// HKLM\SOFTWARE\MiniEQ\ChildAPO\<endpoint-guid>; the APO reads it at
// Initialize to run the displaced APO as its child; detach restores it.
// Writes require elevation; reads are world-readable.
HRESULT MiniEQ_StashChildApoClsid(const wchar_t* endpointId,
                                 const wchar_t* childClsid);
HRESULT MiniEQ_ReadChildApoClsid(const wchar_t* endpointId, wchar_t* out,
                                size_t cch); // S_OK = found, S_FALSE = absent
HRESULT MiniEQ_ClearChildApoClsid(const wchar_t* endpointId);

#ifdef __cplusplus
}
#endif
