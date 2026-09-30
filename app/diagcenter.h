// diagcenter.h -- MiniEQ Diagnostics Center.
//
// Answers "is the EQ actually working, and if not, why?" with measured
// evidence instead of registry guessing:
//   - Engine: audiodg.exe PID + whether MiniEQ_APO.dll is really loaded in it
//     (best-effort module snapshot; access-denied is reported as unknown).
//   - Heartbeat: the APO's APOProcess call counter from the status channel.
//   - Registration: SFX slot state, DLL path/existence, Audio Enhancements.
//   - Sessions: per-app audio sessions on this endpoint (chrome.exe, PID,
//     active/idle, peak level) via IAudioSessionEnumerator.
//   - Verdict: one plain-language diagnosis of the real issue + next step.

#pragma once

#include <windows.h>

#include <string>
#include <vector>

// One audio session on the endpoint (e.g. chrome.exe playing music).
struct DiagSessionInfo {
    std::wstring exe;       // "chrome.exe", or "System sounds"
    DWORD        pid = 0;
    bool         active = false;      // AudioSessionStateActive right now
    bool         systemSounds = false;
    float        peak = -1.0f;        // 0..1 since last poll; < 0 = unknown
};

enum class DiagEnhancements { Unknown, On, Off };

// Spatial-sound mode for one endpoint. Read from the endpoint property store:
// {9637B4B9-11EE-4C35-B43C-7B2452C993CC},1 holds the active spatial-mode
// CLSID as REG_SZ (absent/empty = Off). The friendly name (e.g. "Dolby Atmos
// for Headphones") is resolved from HKCR\CLSID when available.
enum class DiagSpatial { Unknown, Off, On };

struct DiagSpatialInfo {
    DiagSpatial state = DiagSpatial::Unknown;
    std::wstring name;
};

DiagSpatialInfo MiniEQ_ReadSpatialSound(const std::wstring& endpointId);

// Clears the active spatial-sound mode for one endpoint (writes the Off
// state back to the endpoint property store). Returns false when the
// property store can't be opened for write or the commit fails -- the
// caller should then fall back to the manual Settings path.
bool MiniEQ_SetSpatialSoundOff(const std::wstring& endpointId);

// Turns spatial sound off through the public WinRT API
// (Windows.Media.Audio.SpatialAudioDeviceConfiguration) -- the same channel
// the Sound settings page uses, so the audio service rebuilds the running
// graph immediately with no service restart: the Dolby-level transition.
// Runs its async wait on the calling thread, so call it from a worker
// thread, never the UI thread. Returns true when the operation ran to
// completion; the caller then watches the APO heartbeat to confirm the
// path healed. Returns false when WinRT is unavailable or the call failed
// -- the caller should fall back to MiniEQ_SetSpatialSoundOff plus a
// chained audio-service restart.
bool MiniEQ_SetSpatialSoundOffWinRT(const std::wstring& endpointId);

// Writes the "Audio enhancements" switch for one endpoint (on = device
// default effects, off = the engine skips every APO). Same fallback
// contract as MiniEQ_SetSpatialSoundOff.
bool MiniEQ_SetAudioEnhancements(const std::wstring& endpointId, bool on);

// Full COM-registration path of MiniEQ_APO.dll
// (HKCR\CLSID\{...}\InprocServer32). Empty when not registered.
std::wstring MiniEQ_ApoDllPath();
struct DiagSnapshot {
    std::wstring deviceName;
    std::wstring endpointId;

    // Registration layer.
    bool         attached = false;   // SFX slot points at MiniEQ_APO
    std::wstring dllPath;            // InprocServer32 path of our CLSID
    bool         dllExists = false;
    DiagEnhancements enhancements = DiagEnhancements::Unknown;

    // Engine layer.
    DWORD        audiodgPid = 0;
    int          dllLoaded = -1;      // 1 = yes, 0 = no, -1 = unknown
    bool         statusChannelOk = false;
    bool         heartbeatFresh = false; // heartbeat advanced between polls
    int64_t      heartbeatCalls = 0;

    // Stream details reported by the APO itself (valid when the status
    // channel is up). Locked = Initialize completed on a real stream.
    bool         apoLocked = false;
    bool         apoInitOk = false;
    int32_t      apoChannels = 0;
    int32_t      apoSampleRate = 0;

    // Playback layer.
    std::vector<DiagSessionInfo> sessions;
    bool         anySessionActive = false;
    // Exclusive layer: an app holding this endpoint in WASAPI exclusive mode
    // bypasses the engine (and every APO) by Windows design. Probed by
    // attempting a shared-mode IAudioClient::Initialize and checking for
    // AUDCLNT_E_DEVICE_IN_USE -- quick, no stream is left running.
    bool         exclusiveHeld = false;
};

enum class DiagSeverity { Neutral, Good, Warn, Bad };

struct DiagVerdict {
    DiagSeverity severity = DiagSeverity::Neutral;
    std::wstring title;      // plain-language diagnosis
    std::wstring detail;     // the evidence behind it
    std::wstring nextStep;   // what to do; may be empty
};

// Collect one full snapshot for an endpoint. Safe to call on the UI thread
// (about once a second); every failing probe degrades to "unknown".
DiagSnapshot MiniEQ_RunDiagnosis(const std::wstring& endpointId);

// Turn a snapshot into the plain-language verdict.
DiagVerdict MiniEQ_MakeVerdict(const DiagSnapshot& snap);

// The copyable plain-text report.
std::wstring MiniEQ_FormatReport(const DiagSnapshot& snap, const DiagVerdict& v);

// Cheap single-key read of the Audio Enhancements switch
// (PKEY_AudioEndpoint_Disable_SysFx) for the main window's status timer.
// Unknown = the key couldn't be read; never a wrong value.
DiagEnhancements MiniEQ_ReadEnhancements(const std::wstring& endpointId);

// Show (or raise) the modeless Diagnostics Center window.
void MiniEQ_ShowDiagCenter(HINSTANCE hInst, HWND hParent);

// Tell the center which endpoint it is watching (call on device change and
// after attach/detach). Empty string = no device.
void MiniEQ_DiagCenterSetDevice(const std::wstring& endpointId);
