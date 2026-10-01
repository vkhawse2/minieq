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

#include <memory>
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

// Writes the "Audio enhancements" switch for one endpoint (on = device
// default effects, off = the engine skips every APO). Returns false when
// the property store can't be opened for write or the commit fails -- the
// caller should then fall back to the manual Settings path.
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
    // Full registration picture: the audio engine needs the AudioEngine
    // declaration key (layer 2) even when the SFX slot (layer 3) is right.
    // 1 = declaration present and consistent, 0 = missing/inconsistent,
    // -1 = unknown (registry unreadable).
    int          apoDeclared = -1;
    std::wstring sfxSlotClsid;        // empty = SFX slot value absent
    std::wstring efxSlotClsid;        // empty = EFX slot value absent
    std::wstring childStashClsid;     // empty = no displaced-APO stash

    // Engine layer.
    DWORD        audiodgPid = 0;
    // Crash-loop: the engine PID changed 3+ times in the last 10 minutes
    // (tracked passively across snapshots). Tells "audiodg keeps dying"
    // apart from "the APO is merely bypassed".
    bool         audiodgRestartLoop = false;
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
    // bypasses the engine (and every APO) by Windows design. Deliberately
    // NOT probed live anymore: the old IAudioClient check created a real
    // audio stream and churned the engine (audiodg restarts every ~3 s), so
    // this stays false and the UI explains the bypass instead of detecting
    // it.
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

// Snapshot + spatial read, serialized through one process-wide lock. Every
// background diagnosis in the app (Diagnostics Center, Checklist, recovery
// verify) goes through here: the freshness and crash-loop detectors keep
// "last poll" statics that only stay coherent when polls don't overlap.
struct DiagBundle {
    DiagSnapshot  snap;
    DiagSpatialInfo spatial;
};
DiagBundle MiniEQ_RunDiagnosisLocked(const std::wstring& endpointId);

// Circuit breaker: passive audiodg.exe crash-loop detection with an
// automatic response. NoteUserAction starts a 60 s stand-down after a
// deliberate engine disturbance (attach/detach/rebuild) so the detector
// doesn't mistake the user's own churn for a crash loop. Poll feeds the
// detector from the main window's status timer (~every 5 s) and fires the
// breaker non-blockingly; the outcome arrives as WM_APP_BREAKER_DONE.
void MiniEQ_BreakerNoteUserAction();
void MiniEQ_BreakerPoll(HWND owner);
// SAFE/DETACHED latch: set the moment the breaker trips and cleared only by
// a deliberate user re-attach (MiniEQ_BreakerUserResume). While latched, no
// automatic recovery -- attach verification, silent reloads, engine watch --
// may run or re-arm, so MiniEQ can never feed the crash loop it detected.
bool MiniEQ_BreakerLatched();
void MiniEQ_BreakerUserResume();
#define WM_APP_BREAKER_DONE (WM_APP + 103)
// wParam outcomes for WM_APP_BREAKER_DONE:
enum { BreakerOutcomeLaunched = 0, BreakerOutcomeUacDeclined = 1,
       BreakerOutcomeLaunchFailed = 2, BreakerOutcomePartial = 3,
       BreakerOutcomeSweepFailed = 4 };

// Background diagnosis worker: runs the heavy probes off the UI thread and
// hands the finished bundle to the window via a posted message, so the
// Diagnostics Center / Checklist never freeze while data is loading. The
// window owns the state (static storage); the worker never touches window
// handles except the single PostMessage.
struct DiagAsyncState {
    HWND hwnd = nullptr;
    UINT doneMsg = 0;
    HANDLE hThread = nullptr;
    HANDLE hWake = nullptr; // auto-reset: a refresh was requested
    HANDLE hStop = nullptr; // manual-reset: shut down
    SRWLOCK lock = SRWLOCK_INIT;
    std::wstring endpoint;
    std::unique_ptr<DiagBundle> result; // guarded by lock
    bool hasResult = false;
    bool busy = false; // a run is in flight; new requests coalesce
    // Bumped on every Start/Stop: a worker finishing a probe from a previous
    // incarnation drops its bundle instead of posting it to a new window.
    uint64_t generation = 0;
};

void MiniEQ_DiagAsyncStart(DiagAsyncState* a, HWND hwnd, UINT doneMsg,
                           const std::wstring& endpoint);
void MiniEQ_DiagAsyncSetEndpoint(DiagAsyncState* a,
                                 const std::wstring& endpoint);
// Ask for a fresh bundle. Cheap: if a run is already in flight the request
// coalesces and the next timer tick re-asks.
void MiniEQ_DiagAsyncRequest(DiagAsyncState* a);
// Take the latest finished bundle (true) or report none waiting (false).
bool MiniEQ_DiagAsyncTake(DiagAsyncState* a, DiagBundle& out);
// Shut down: no more posted messages, worker joined (bounded wait).
void MiniEQ_DiagAsyncStop(DiagAsyncState* a);

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
