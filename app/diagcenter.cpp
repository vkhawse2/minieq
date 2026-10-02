// diagcenter.cpp -- MiniEQ Diagnostics Center, part 1: the diagnosis engine.
//
// Every probe is best-effort: a failing probe degrades to "unknown" instead
// of breaking the snapshot, and the verdict only claims what the evidence
// supports.

#include "diagcenter.h"

#include "checklist.h" // MiniEQ_ChecklistDisarmWatch (circuit breaker)
#include "diag.h"
#include "settings_link.h"
#include "../apo/registration.h"
#include "../shared/settings_channel.h"

#include <audiopolicy.h>
#include <commctrl.h>
#include <endpointvolume.h>
#include <propkey.h> // DEFINE_PROPERTYKEY, needed by functiondiscoverykeys_devpkey.h
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <shellapi.h> // ShellExecuteExW (circuit-breaker elevated sweep)
#include <shlobj.h>   // SHGetFolderPathW (forensics report goes to the Desktop)
#include <tlhelp32.h>
#include <winevt.h>   // EvtQuery (forensics: Application Error events)
#include <strsafe.h>

#include <string.h>
#include <wchar.h>

#include <mutex>

// PKEY_AudioEndpoint_Disable_SysFx -- the "Audio enhancements" switch.
// {1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},5. The native storage is a
// REG_DWORD (VT_UI4 through the property store): 1 = enhancements OFF
// (the engine skips the whole SysFx chain, MiniEQ included), 0 = on.
// Our own older one-click fix wrote VT_BOOL, so both are accepted.
// Same fmtid as PKEY_AudioEndpoint_GUID (pid 4); this key is pid 5.
// Defined locally rather than via the SDK header so the build doesn't
// depend on SDK header version skew.
static const PROPERTYKEY kPkeyDisableSysFx = {
    { 0x1DA5D803, 0xD492, 0x4EDD, { 0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E } },
    5
};

// Keep in sync with apo/guids.h (CLSID_MiniEQAPO).
static const wchar_t* kApoClsidReg = L"{5E52BF50-F229-46A0-8B7D-AA805D47FB60}";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static DWORD FindAudiodgPid() {
    DWORD pid = 0;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(h, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"audiodg.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(h, &pe));
    }
    CloseHandle(h);
    return pid;
}

// Passive audiodg crash-loop detection. Every snapshot feeds the engine PID
// through here; three PID changes inside ten minutes means audiodg.exe is
// dying and restarting in a loop (e.g. an APO crashing it on stream start).
// Purely observational -- no service is touched. Called from both the main
// window's timer (UI thread) and the diagnostics worker thread, so the
// little history ring is guarded by a lock.
static SRWLOCK    s_loopLock = SRWLOCK_INIT;
static DWORD      s_loopLastPid = 0;
static ULONGLONG  s_loopChangeTicks[4] = {};
static int        s_loopChangeCount = 0;

static bool NoteAudiodgPid(DWORD pid) {
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&s_loopLock);
    if (pid != 0 && pid != s_loopLastPid) {
        if (s_loopLastPid != 0) {
            // A genuine change, not the first sighting: remember when.
            s_loopChangeTicks[s_loopChangeCount % 4] = now;
            ++s_loopChangeCount;
        }
        s_loopLastPid = pid;
    }
    int recent = 0;
    const int n = (s_loopChangeCount < 4) ? s_loopChangeCount : 4;
    for (int i = 0; i < n; ++i) {
        if (now - s_loopChangeTicks[i] <= 10ULL * 60 * 1000) {
            ++recent;
        }
    }
    const bool loop = (recent >= 3);
    ReleaseSRWLockExclusive(&s_loopLock);
    return loop;
}

// ---------------------------------------------------------------------------
// Circuit breaker: when the audio engine is dying in a crash loop, sacrifice
// the EQ to keep audio alive. Detection is purely passive (NoteAudiodgPid
// above); the response is an elevated --breaker sweep that detaches MiniEQ
// from every endpoint and re-enumerates the device nodes. The audio service
// is never touched. The sweep runs on a worker thread -- the UI thread never
// blocks or sleeps.
// ---------------------------------------------------------------------------

// Cross-thread state: MiniEQ_BreakerPoll runs on the UI thread while
// BreakerSweepThread runs on a worker thread, so every flag here is atomic.
static std::atomic<ULONGLONG> s_breakerStandDownUntil{0};   // 60 s after a deliberate user action
static std::atomic<ULONGLONG> s_breakerUacCooldownUntil{0}; // 30 min after a declined UAC prompt
static std::atomic<bool>      s_breakerTripped{false};      // one UAC prompt per loop episode
static std::atomic<bool>      s_breakerSweepActive{false};  // elevated sweep currently in flight
static std::atomic<bool>      s_breakerLatched{false};      // SAFE/DETACHED latch (see BreakerPoll)

// Deliberate user action (attach / detach / path rebuild): the engine may
// legitimately restart right after, so don't mistake that churn for a crash
// loop. Call from every code path that intentionally disturbs the engine.
void MiniEQ_BreakerNoteUserAction() {
    s_breakerStandDownUntil.store(GetTickCount64() + 60ULL * 1000,
                                 std::memory_order_release);
}

bool MiniEQ_BreakerLatched() {
    return s_breakerLatched.load(std::memory_order_acquire);
}

// Deliberate user re-attach: the only way out of the SAFE/DETACHED latch.
void MiniEQ_BreakerUserResume() {
    if (s_breakerLatched.exchange(false, std::memory_order_acq_rel)) {
        MiniEQ_AppLogCat(L"BREAKER",
            L"user re-attach -- circuit-breaker latch cleared, recovery re-enabled");
    }
}

// Outcome codes posted with WM_APP_BREAKER_DONE (see diagcenter.h).

static DWORD WINAPI BreakerSweepThread(LPVOID param) {
    const HWND owner = static_cast<HWND>(param);
    s_breakerSweepActive.store(true, std::memory_order_release);
    int outcome = BreakerOutcomeLaunchFailed;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = L"--breaker";
    sei.nShow = SW_HIDE;
    MiniEQ_AppLogCat(L"BREAKER",
        L"restart loop confirmed -- relaunching elevated to detach MiniEQ everywhere");
    if (ShellExecuteExW(&sei)) {
        outcome = BreakerOutcomeLaunched;
        if (sei.hProcess != nullptr) {
            // Deterministic handoff: wait for the sweep to finish (bounded),
            // so the "all clear" message never precedes the actual detach.
            // The sweep's exit code carries the real result (see
            // RunCircuitBreakerSweep): 0 = clean, 1 = partial, 2 = failed.
            const DWORD wr = WaitForSingleObject(sei.hProcess, 90000);
            if (wr == WAIT_TIMEOUT) {
                MiniEQ_AppLogCat(L"BREAKER",
                    L"sweep still running after 90 s -- leaving it to finish");
            } else {
                DWORD code = 0;
                if (GetExitCodeProcess(sei.hProcess, &code)) {
                    if (code == 1) {
                        outcome = BreakerOutcomePartial;
                    } else if (code == 2) {
                        outcome = BreakerOutcomeSweepFailed;
                    }
                }
            }
            CloseHandle(sei.hProcess);
        }
    } else {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            // The user declined the UAC prompt: respect that, don't nag.
            // Cool down for 30 minutes; the episode latch is released so the
            // poll may ask again afterwards if the loop is still going.
            outcome = BreakerOutcomeUacDeclined;
            s_breakerUacCooldownUntil.store(GetTickCount64() + 30ULL * 60 * 1000,
                                           std::memory_order_release);
            s_breakerTripped.store(false, std::memory_order_release);
            MiniEQ_AppLogCat(L"BREAKER",
                L"UAC prompt declined by user -- cooling down 30 minutes");
        } else {
            MiniEQ_AppLogCat(L"BREAKER",
                L"elevated relaunch failed (err=%u)", (unsigned)err);
        }
    }
    s_breakerSweepActive.store(false, std::memory_order_release);
    PostMessageW(owner, WM_APP_BREAKER_DONE, (WPARAM)outcome, 0);
    return 0;
}

// Feed-only detector sampling (see header): called on every 500 ms status
// tick so a crash loop's short-lived engines are actually observed. The
// trip/response decision stays in MiniEQ_BreakerPoll on its slower cadence.
void MiniEQ_BreakerSampleAudiodg() {
    (void)NoteAudiodgPid(FindAudiodgPid());
}

// Poll entry, called from the main window's status timer about every 5 s.
// Feeds the passive detector with the current engine PID; fires the breaker
// (non-blocking) only when a genuine restart loop is confirmed.
void MiniEQ_BreakerPoll(HWND owner) {
    if (owner == nullptr || s_breakerSweepActive.load(std::memory_order_acquire)) {
        return;
    }
    const bool loop = NoteAudiodgPid(FindAudiodgPid());
    const ULONGLONG now = GetTickCount64();
    if (!loop) {
        s_breakerTripped.store(false, std::memory_order_release); // engine calm: re-arm
        return;
    }
    if (s_breakerLatched.load(std::memory_order_acquire)) {
        return; // SAFE/DETACHED: keep observing, never fire again until the user resumes
    }
    if (s_breakerTripped.load(std::memory_order_acquire)) {
        return; // one UAC prompt per episode
    }
    if (now < s_breakerStandDownUntil.load(std::memory_order_acquire)) {
        return; // user just attached/detached/rebuilt -- their churn, not a crash
    }
    if (now < s_breakerUacCooldownUntil.load(std::memory_order_acquire)) {
        return; // user declined the prompt -- don't nag
    }
    s_breakerTripped.store(true, std::memory_order_release);
    // Latch SAFE/DETACHED before the sweep even launches: from this moment
    // every automatic recovery path (attach verify + forced re-attach,
    // silent-reload backoff, stale-build reload, checklist watch) stays off
    // until the user deliberately re-attaches. The episode flag above only
    // suppresses repeat prompts; this latch is what keeps MiniEQ itself
    // from re-entering the crash loop it just detected.
    s_breakerLatched.store(true, std::memory_order_release);
    MiniEQ_ChecklistDisarmWatch(); // the watch's auto-recovery must not fight the breaker
    MiniEQ_AppLogCat(L"BREAKER",
        L"audiodg.exe restart loop detected -- firing the circuit breaker");
    HANDLE h = CreateThread(nullptr, 0, BreakerSweepThread, owner, 0, nullptr);
    if (h != nullptr) {
        CloseHandle(h);
    } else {
        s_breakerTripped.store(false, std::memory_order_release); // let the next poll retry
        MiniEQ_AppLogCat(L"BREAKER", L"could not start the sweep thread");
    }
}

// 1 = module found in the process, 0 = snapshot worked but module absent,
// -1 = unknown (access denied on a protected audiodg.exe, process gone, ...).
static int IsModuleLoadedIn(DWORD pid, const wchar_t* moduleName) {
    if (pid == 0 || moduleName == nullptr) {
        return -1;
    }
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    int found = 0;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    if (Module32FirstW(h, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleName) == 0) {
                found = 1;
                break;
            }
        } while (Module32NextW(h, &me));
    }
    CloseHandle(h);
    return found;
}

static std::wstring ExeNameFromPid(DWORD pid) {
    if (pid == 0) {
        return std::wstring();
    }
    std::wstring name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h != nullptr) {
        wchar_t path[MAX_PATH] = {};
        DWORD n = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(h, 0, path, &n) && n > 0) {
            const wchar_t* base = wcsrchr(path, L'\\');
            name = (base != nullptr) ? (base + 1) : path;
        }
        CloseHandle(h);
    }
    return name;
}

std::wstring MiniEQ_ApoDllPath() {
    wchar_t path[MAX_PATH] = {};
    DWORD size = sizeof(path);
    std::wstring sub = L"CLSID\\";
    sub += kApoClsidReg;
    sub += L"\\InprocServer32";
    if (RegGetValueW(HKEY_CLASSES_ROOT, sub.c_str(), nullptr,
                     RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS) {
        return path;
    }
    return std::wstring();
}

// Friendly name + enhancements state from the endpoint property store.
struct DeviceProps {
    std::wstring     friendlyName;
    DiagEnhancements enhancements = DiagEnhancements::Unknown;
    bool             ok = false;
};

// Interprets one Disable_SysFx read for both enhancement-read sites below.
// Native storage is REG_DWORD (VT_UI4): nonzero = enhancements off,
// zero = on. VT_BOOL is accepted for values our own older one-click fix
// wrote. Absent/empty/valueless = device defaults = on (the Settings UI
// shows "Device Default Effects" in that state). A genuinely unrecognized
// type stays Unknown -- and is logged so the next trace names it.
static DiagEnhancements EnhancementsFromPropVariant(const PROPVARIANT& pv, HRESULT hr) {
    if (FAILED(hr)) {
        return DiagEnhancements::On;
    }
    switch (pv.vt) {
    case VT_BOOL:
        return pv.boolVal ? DiagEnhancements::Off : DiagEnhancements::On;
    case VT_UI4:
        return pv.ulVal ? DiagEnhancements::Off : DiagEnhancements::On;
    case VT_I4:
        return pv.lVal ? DiagEnhancements::Off : DiagEnhancements::On;
    case VT_UI2:
        return pv.uiVal ? DiagEnhancements::Off : DiagEnhancements::On;
    case VT_I2:
        return pv.iVal ? DiagEnhancements::Off : DiagEnhancements::On;
    case VT_UI1:
        return pv.bVal ? DiagEnhancements::Off : DiagEnhancements::On;
    case VT_I1:
        return pv.cVal ? DiagEnhancements::Off : DiagEnhancements::On;
    case VT_EMPTY:
    case VT_NULL:
        return DiagEnhancements::On;
    default:
        MiniEQ_AppLogCat(L"DIAG",
            L"enhancements read: unexpected variant type %u, treating as unknown",
            static_cast<unsigned>(pv.vt));
        return DiagEnhancements::Unknown;
    }
}

// Single-key read of PKEY_AudioEndpoint_Disable_SysFx for one endpoint.
// Exported for the main window's 500 ms status path: cheap enough to poll
// every few seconds. Device enumeration failures stay Unknown; the key
// itself is interpreted by EnhancementsFromPropVariant above.
DiagEnhancements MiniEQ_ReadEnhancements(const std::wstring& endpointId) {
    if (endpointId.empty()) {
        return DiagEnhancements::Unknown;
    }
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&pEnum))) || pEnum == nullptr) {
        return DiagEnhancements::Unknown;
    }
    DiagEnhancements out = DiagEnhancements::Unknown;
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDevice(endpointId.c_str(), &pDev)) && pDev != nullptr) {
        IPropertyStore* pProps = nullptr;
        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps != nullptr) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            const HRESULT hr = pProps->GetValue(kPkeyDisableSysFx, &pv);
            out = EnhancementsFromPropVariant(pv, hr);
            PropVariantClear(&pv);
            pProps->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
    return out;
}

// PKEY for the active spatial-sound mode: {9637B4B9-11EE-4C35-B43C-7B2452C993CC},1
// (REG_SZ = spatial APO CLSID; absent/empty = spatial sound Off). Same pattern
// as the enhancements key above: defined locally, degrades to Unknown rather
// than a wrong value.
static const PROPERTYKEY kPkeySpatialClsid = {
    { 0x9637B4B9, 0x11EE, 0x4C35, { 0xB4, 0x3C, 0x7B, 0x24, 0x52, 0xC9, 0x93, 0xCC } },
    1
};

// Best-effort friendly name for a spatial APO CLSID, from its COM
// registration. Falls back to the raw CLSID string.
static std::wstring SpatialClsidDisplayName(const std::wstring& clsid) {
    if (clsid.empty()) {
        return std::wstring();
    }
    std::wstring sub = L"CLSID\\";
    sub += clsid;
    wchar_t name[256] = {};
    DWORD size = sizeof(name);
    if (RegGetValueW(HKEY_CLASSES_ROOT, sub.c_str(), nullptr,
                     RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS &&
        name[0] != L'\0') {
        return name;
    }
    return clsid;
}

DiagSpatialInfo MiniEQ_ReadSpatialSound(const std::wstring& endpointId) {
    DiagSpatialInfo out;
    if (endpointId.empty()) {
        return out;
    }
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&pEnum))) || pEnum == nullptr) {
        return out;
    }
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDevice(endpointId.c_str(), &pDev)) && pDev != nullptr) {
        IPropertyStore* pProps = nullptr;
        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps != nullptr) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            const HRESULT hr = pProps->GetValue(kPkeySpatialClsid, &pv);
            if (SUCCEEDED(hr) && pv.vt == VT_LPWSTR &&
                pv.pwszVal != nullptr && pv.pwszVal[0] != L'\0') {
                out.state = DiagSpatial::On;
                out.name = SpatialClsidDisplayName(pv.pwszVal);
            } else if (SUCCEEDED(hr)) {
                // Key present but empty: no active spatial mode.
                out.state = DiagSpatial::Off;
            } else {
                // Key absent while the store itself reads fine: spatial is
                // Off. (A store we can't open at all stays Unknown.)
                out.state = DiagSpatial::Off;
            }
            PropVariantClear(&pv);
            pProps->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
    return out;
}

// Writes the "Audio enhancements" switch: on = 0 (device default
// effects, the SysFx chain runs), off = 1 (the engine skips every
// APO, MiniEQ included). Stored as REG_DWORD (VT_UI4) -- the native
// format the Settings app itself uses, so both tools read each
// other's writes.
bool MiniEQ_SetAudioEnhancements(const std::wstring& endpointId, bool on) {
    if (endpointId.empty()) {
        return false;
    }
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&pEnum))) || pEnum == nullptr) {
        return false;
    }
    bool ok = false;
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDevice(endpointId.c_str(), &pDev)) && pDev != nullptr) {
        IPropertyStore* pProps = nullptr;
        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READWRITE, &pProps)) &&
            pProps != nullptr) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_UI4;
            pv.ulVal = on ? 0 : 1;
            if (SUCCEEDED(pProps->SetValue(kPkeyDisableSysFx, pv)) &&
                SUCCEEDED(pProps->Commit())) {
                ok = true;
            }
            PropVariantClear(&pv);
            pProps->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
    return ok;
}

static DeviceProps ReadDeviceProps(const std::wstring& endpointId) {
    DeviceProps d;
    if (endpointId.empty()) {
        return d;
    }
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&pEnum))) || pEnum == nullptr) {
        return d;
    }
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDevice(endpointId.c_str(), &pDev)) && pDev != nullptr) {
        IPropertyStore* pProps = nullptr;
        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps != nullptr) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR && pv.pwszVal != nullptr) {
                d.friendlyName = pv.pwszVal;
                d.ok = true;
            }
            PropVariantClear(&pv);

            PropVariantInit(&pv);
            {
                const HRESULT hrEnh = pProps->GetValue(kPkeyDisableSysFx, &pv);
                d.enhancements = EnhancementsFromPropVariant(pv, hrEnh);
            }
            PropVariantClear(&pv);
            pProps->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
    return d;
}

// Exclusive-mode probe: does an app hold this endpoint in WASAPI exclusive
// mode? Attempt a shared-mode IAudioClient::Initialize on the endpoint's mix
// format: AUDCLNT_E_DEVICE_IN_USE means something already holds the device
// exclusively. Any other failure (device gone, engine hiccup) reports
// "not held" rather than a false red.
//
// 2026-10-01 -- CRITICAL: IAudioClient::Initialize creates a REAL audio stream
// on the endpoint, and releasing it tears the stream down again. Run on the
// NOTE: there is deliberately NO live exclusive-mode probe anymore. The old
// probe called IAudioClient::Initialize to detect an exclusive holder, but
// that call creates a REAL audio stream: on 2026-10-01 it was observed
// churning the engine (audiodg.exe restarting every ~3 s in lockstep with
// the probes, 96 restarts in ~6 min, each one dropping playback and
// reloading our APO). Exclusive-mode detection is therefore not performed --
// the snapshot reports exclusiveHeld = false and the UI explains that an
// exclusive-mode app bypasses APOs by Windows design.

// Per-app sessions on this endpoint with live peak levels.
static std::vector<DiagSessionInfo> EnumEndpointSessions(const std::wstring& endpointId) {
    std::vector<DiagSessionInfo> out;
    if (endpointId.empty()) {
        return out;
    }
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&pEnum))) || pEnum == nullptr) {
        return out;
    }
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDevice(endpointId.c_str(), &pDev)) && pDev != nullptr) {
        IAudioSessionManager2* pMgr = nullptr;
        if (SUCCEEDED(pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                     nullptr, reinterpret_cast<void**>(&pMgr))) &&
            pMgr != nullptr) {
            IAudioSessionEnumerator* pSessEnum = nullptr;
            if (SUCCEEDED(pMgr->GetSessionEnumerator(&pSessEnum)) && pSessEnum != nullptr) {
                int count = 0;
                if (SUCCEEDED(pSessEnum->GetCount(&count))) {
                    for (int i = 0; i < count; ++i) {
                        IAudioSessionControl* pCtl = nullptr;
                        if (FAILED(pSessEnum->GetSession(i, &pCtl)) || pCtl == nullptr) {
                            continue;
                        }
                        AudioSessionState st = AudioSessionStateInactive;
                        if (FAILED(pCtl->GetState(&st))) {
                            pCtl->Release();
                            continue;
                        }
                        if (st == AudioSessionStateExpired) {
                            pCtl->Release();
                            continue; // stale entry, not real playback
                        }
                        DiagSessionInfo si;
                        si.active = (st == AudioSessionStateActive);

                        IAudioSessionControl2* pCtl2 = nullptr;
                        if (SUCCEEDED(pCtl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                          reinterpret_cast<void**>(&pCtl2))) &&
                            pCtl2 != nullptr) {
                            DWORD pid = 0;
                            if (SUCCEEDED(pCtl2->GetProcessId(&pid))) {
                                si.pid = pid;
                            }
                            if (pCtl2->IsSystemSoundsSession() == S_OK) {
                                si.systemSounds = true;
                                si.exe = L"System sounds";
                            }
                            pCtl2->Release();
                        }

                        // Peak level for this session's mix, 0..1.
                        IAudioMeterInformation* pMeter = nullptr;
                        if (SUCCEEDED(pCtl->QueryInterface(__uuidof(IAudioMeterInformation),
                                                          reinterpret_cast<void**>(&pMeter))) &&
                            pMeter != nullptr) {
                            float peak = 0.0f;
                            if (SUCCEEDED(pMeter->GetPeakValue(&peak))) {
                                si.peak = peak;
                            }
                            pMeter->Release();
                        }

                        if (!si.systemSounds) {
                            si.exe = ExeNameFromPid(si.pid);
                            if (si.exe.empty() || si.pid == 0) {
                                pCtl->Release();
                                continue; // unresolvable leftover, skip
                            }
                        }
                        out.push_back(si);
                        pCtl->Release();
                    }
                }
                pSessEnum->Release();
            }
            pMgr->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
    return out;
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

// The UI side of the APO->UI heartbeat channel. One reader; reopened when the
// watched endpoint changes. (The main window keeps its own reader for the
// pill; two readers of the same read-only mapping are fine.)
static StatusLink    g_dcStatusLink;
static std::wstring  g_dcStatusEndpoint;

DiagSnapshot MiniEQ_RunDiagnosis(const std::wstring& endpointId) {
    DiagSnapshot s;
    s.endpointId = endpointId;
    if (endpointId.empty()) {
        return s;
    }

    // Registration layer.
    bool attached = false;
    if (SUCCEEDED(MiniEQ_IsAttachedToEndpoint(endpointId.c_str(), &attached))) {
        s.attached = attached;
    }
    // Full registration picture: the engine needs the AudioEngine declaration
    // key (layer 2) even when the slot (layer 3) is right, and seeing both
    // slots + the child stash exposes double-registration leftovers.
    bool declared = false;
    if (SUCCEEDED(MiniEQ_QueryApoDeclaration(&declared))) {
        s.apoDeclared = declared ? 1 : 0;
    }
    wchar_t slotClsid[64] = {};
    if (MiniEQ_QuerySlotValue(endpointId.c_str(), false, slotClsid,
                              ARRAYSIZE(slotClsid)) == S_OK) {
        s.sfxSlotClsid = slotClsid;
    }
    if (MiniEQ_QuerySlotValue(endpointId.c_str(), true, slotClsid,
                              ARRAYSIZE(slotClsid)) == S_OK) {
        s.efxSlotClsid = slotClsid;
    }
    wchar_t stashed[64] = {};
    if (MiniEQ_ReadChildApoClsid(endpointId.c_str(), stashed,
                                 ARRAYSIZE(stashed)) == S_OK &&
        stashed[0] != L'\0') {
        s.childStashClsid = stashed;
    }
    s.dllPath = MiniEQ_ApoDllPath();
    if (!s.dllPath.empty()) {
        s.dllExists = (GetFileAttributesW(s.dllPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    }
    DeviceProps dp = ReadDeviceProps(endpointId);
    s.deviceName = dp.friendlyName;
    s.enhancements = dp.enhancements;

    // Engine layer.
    s.audiodgPid = FindAudiodgPid();
    s.audiodgRestartLoop = NoteAudiodgPid(s.audiodgPid);
    s.dllLoaded = IsModuleLoadedIn(s.audiodgPid, L"MiniEQ_APO.dll");

    // Heartbeat layer.
    if (g_dcStatusEndpoint != endpointId) {
        g_dcStatusLink.Close();
        g_dcStatusEndpoint.clear();
    }
    if (!g_dcStatusLink.IsOpen()) {
        if (g_dcStatusLink.Open(endpointId)) {
            g_dcStatusEndpoint = endpointId;
        }
    }
    MiniEQApoStatus st = {};
    if (g_dcStatusLink.IsOpen() && g_dcStatusLink.Read(&st) &&
        st.structSize == sizeof(MiniEQApoStatus) && st.version == MINIEQ_STATUS_VERSION) {
        s.statusChannelOk = true;
        s.heartbeatCalls = st.processCalls;
        s.apoLocked = (st.locked != 0);
        s.apoInitOk = (st.initOk != 0);
        s.apoChannels = st.channels;
        s.apoSampleRate = st.sampleRate;
    }

    // "Fresh" = the counter advanced between our polls (needs two polls to
    // say yes, so the first snapshot after opening the window never claims
    // live on a single sample).
    static std::wstring lastEp;
    static int64_t      lastCalls = 0;
    static ULONGLONG    lastTick = 0;
    if (endpointId != lastEp) {
        lastEp = endpointId;
        lastCalls = 0;
        lastTick = 0;
    }
    const ULONGLONG now = GetTickCount64();
    s.heartbeatFresh = (s.statusChannelOk && lastCalls > 0 &&
                        s.heartbeatCalls > lastCalls && (now - lastTick) < 4000);
    lastCalls = s.heartbeatCalls;
    lastTick = now;

    // Playback layer.
    s.sessions = EnumEndpointSessions(endpointId);
    for (const DiagSessionInfo& si : s.sessions) {
        if (si.active) {
            s.anySessionActive = true;
            break;
        }
    }

    // Exclusive-mode layer: not probed live anymore. The old IAudioClient
    // probe created a real audio stream and churned the engine (audiodg
    // restarts every ~3 s), so the snapshot reports exclusiveHeld = false
    // and the UI explains the bypass instead of detecting it.
    s.exclusiveHeld = false;
    return s;
}

// ---------------------------------------------------------------------------
// Verdict
// ---------------------------------------------------------------------------

// Verdict debounce state: "bypassing MiniEQ" is claimed only after the
// (session active + heartbeat stale) condition has persisted ~4 s across
// consecutive snapshots, so a single stale sample between songs or during
// a graph rebuild never flashes a false failure. Keyed by endpoint.
static std::wstring s_debEndpoint;
static ULONGLONG    s_debBypassSince = 0;

DiagVerdict MiniEQ_MakeVerdict(const DiagSnapshot& snap) {
    DiagVerdict v;
    if (snap.endpointId.empty()) {
        v.severity = DiagSeverity::Neutral;
        v.title = L"No device selected.";
        v.detail = L"Pick an output device in the main window first.";
        return v;
    }
    if (!MiniEQ_GlobalEnabledGet()) {
        v.severity = DiagSeverity::Neutral;
        v.title = L"MiniEQ is off.";
        v.detail = L"Audio plays unprocessed \u2014 the APO passes every buffer "
                   L"through untouched on all devices.";
        v.nextStep = L"Click \u201CTurn on MiniEQ\u201D in the main window to "
                     L"resume the EQ.";
        return v;
    }
    if (!snap.attached) {
        v.severity = DiagSeverity::Warn;
        v.title = L"MiniEQ isn't attached to this device.";
        v.detail = std::wstring(MiniEQ_EffectSlotShortName()) +
                   L" slot of this endpoint doesn't point at MiniEQ_APO, "
                   L"so Windows never loads our equalizer for it.";
        v.nextStep = L"In the main window, click \"Attach to this device\" "
                     L"(one admin approval).";
        return v;
    }
    if (snap.apoDeclared == 0) {
        // The slot points at us but the engine's own APO declaration key is
        // missing -- Windows can't resolve our CLSID, so the slot is a dead
        // pointer. Re-attaching repairs the declaration (self-healing attach).
        v.severity = DiagSeverity::Bad;
        v.title = L"MiniEQ's engine registration is missing.";
        v.detail = L"The endpoint slot points at MiniEQ_APO, but the audio "
                   L"engine's own APO declaration key is missing or "
                   L"inconsistent, so Windows can't resolve it -- the slot "
                   L"is a dead pointer.";
        v.nextStep = L"Detach, then re-attach (Recovery > Re-attach). The "
                     L"attach repairs the missing declaration automatically.";
        return v;
    }
    if (snap.enhancements == DiagEnhancements::Off) {
        v.severity = DiagSeverity::Bad;
        v.title = L"Audio enhancements are Off \u2014 Windows is skipping MiniEQ.";
        v.detail = L"With enhancements off, the audio engine bypasses the entire "
                   L"effects chain, so our APO never loads even though the SFX "
                   L"slot is registered.";
        v.nextStep = L"Settings \u2192 System \u2192 Sound \u2192 " +
                     (snap.deviceName.empty() ? L"this device" : snap.deviceName) +
                     L" \u2192 Audio enhancements \u2192 \"Device Default Effects\", "
                     L"then play something.";
        return v;
    }
    if (snap.heartbeatFresh) {
        v.severity = DiagSeverity::Good;
        v.title = L"Equalizer is live.";
        v.detail = L"MiniEQ_APO.dll is processing this device's audio right now "
                   L"\u2014 slider changes are audible.";
        return v;
    }
    if (snap.audiodgRestartLoop) {
        // The engine dying repeatedly explains every downstream symptom
        // (no DLL load, no heartbeat), so this outranks them.
        v.severity = DiagSeverity::Bad;
        v.title = L"The audio engine keeps crashing and restarting.";
        v.detail = L"audiodg.exe has restarted 3 or more times in the last 10 minutes. "
                   L"Every restart drops MiniEQ from the engine before it can process "
                   L"audio \u2014 that is why no heartbeat arrives. If Windows blames "
                   L"our APO often enough, it can also switch Audio enhancements off "
                   L"by itself for this device (the Disable_SysFx lockout).";
        v.nextStep = L"Detach MiniEQ from this device, replay, and see whether the "
                     L"crashing stops \u2014 that tells us if our APO is the trigger. "
                     L"Then copy this report and send it over.";
        return v;
    }
    // Instantiated but not locked yet: the status channel exists (created
    // in Initialize) while no processing stream has reached the APO. That
    // is "starting up", never a failure.
    if (snap.anySessionActive && snap.statusChannelOk && !snap.apoLocked) {
        s_debBypassSince = 0; // a different story from bypass; don't accrue
        v.severity = DiagSeverity::Neutral;
        v.title = L"MiniEQ is starting up.";
        v.detail = L"The audio engine has loaded MiniEQ, but hasn't put it "
                   L"in the processing path yet \u2014 normal for a moment "
                   L"after an attach or a device change.";
        return v;
    }
    // Debounce: claim a bypass only once "audio playing + no heartbeat" has
    // held for ~4 s across consecutive snapshots. Inside the grace window
    // the honest verdict is a neutral wait, not a failure.
    if (snap.endpointId != s_debEndpoint) {
        s_debEndpoint = snap.endpointId;
        s_debBypassSince = 0;
    }
    const ULONGLONG debNow = GetTickCount64();
    if (!(snap.anySessionActive && !snap.heartbeatFresh)) {
        s_debBypassSince = 0;
    } else if (s_debBypassSince == 0) {
        s_debBypassSince = debNow;
    }
    const bool bypassSettled =
        s_debBypassSince != 0 && (debNow - s_debBypassSince >= 4000);
    if (snap.anySessionActive && !bypassSettled) {
        v.severity = DiagSeverity::Neutral;
        v.title = L"Waiting for audio.";
        v.detail = L"MiniEQ is attached; audio just started or the path is "
                   L"still settling, so this gets a few seconds before "
                   L"it's judged.";
        return v;
    }
    // NOTE: no exclusive-holder verdict anymore. Detecting it required a
    // live IAudioClient probe that created a real stream and churned the
    // engine, so it was removed; the static checklist row explains the
    // bypass instead.
    if (snap.anySessionActive && snap.dllLoaded == 0) {
        // Honest "attached but not loaded": registration is provably right,
        // yet the engine never instantiated our APO for this stream.
        v.severity = DiagSeverity::Bad;
        v.title = L"MiniEQ is attached, but Windows never loaded it.";
        v.detail = std::wstring(MiniEQ_EffectSlotShortName()) +
                   L" slot registration is correct \u2014 it points at MiniEQ_APO "
                   L"and audio enhancements are on \u2014 but audiodg.exe never "
                   L"instantiated our APO for this stream. Windows skipped it "
                   L"silently, with no error.";
        v.nextStep = L"In the Audio Path Checklist, press Re-attach (Recovery "
                     L"section): one click re-writes the effects slot and "
                     L"re-enumerates the device. Then replay. If it stays red, "
                     L"copy this report and send it over.";
        return v;
    }
    if (snap.anySessionActive) {
        v.severity = DiagSeverity::Bad;
        v.title = L"Audio is playing, but it's bypassing MiniEQ.";
        if (snap.dllLoaded == 1) {
            v.detail = L"Our DLL is inside the audio engine, but no processing "
                       L"calls are arriving \u2014 the stream isn't reaching it.";
        } else {
            v.detail = L"Audio is flowing on the endpoint, but no processing "
                       L"heartbeat is coming back from our APO.";
        }
        v.nextStep = L"In the Audio Path Checklist, press Re-attach (Recovery "
                     L"section): one click re-writes the effects slot and "
                     L"re-enumerates the device. Then replay. If it stays red, "
                     L"copy this report and send it over.";
        return v;
    }
    v.severity = DiagSeverity::Neutral;
    v.title = L"Waiting for audio.";
    v.detail = L"MiniEQ is attached and the path looks ready \u2014 play something "
               L"on this device to see the live state.";
    return v;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

static const wchar_t* SeverityLabel(DiagSeverity s) {
    switch (s) {
    case DiagSeverity::Good: return L"OK";
    case DiagSeverity::Warn: return L"ACTION NEEDED";
    case DiagSeverity::Bad:  return L"PROBLEM";
    default:                 return L"INFO";
    }
}

static const wchar_t* DllStateText(int d) {
    return d > 0 ? L"yes" : (d == 0 ? L"no" : L"unknown (access denied?)");
}

static const wchar_t* EnhText(DiagEnhancements e) {
    switch (e) {
    case DiagEnhancements::On:  return L"On (Device Default Effects)";
    case DiagEnhancements::Off: return L"Off (bypasses all effects)";
    default:                     return L"unknown";
    }
}

std::wstring MiniEQ_FormatReport(const DiagSnapshot& snap, const DiagVerdict& v) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t stamp[64] = {};
    StringCchPrintfW(stamp, ARRAYSIZE(stamp), L"%04u-%02u-%02u %02u:%02u:%02u",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring r;
    r += L"MiniEQ diagnostics report \u2014 ";
    r += stamp;
    r += L"\r\n\r\nDevice: ";
    r += snap.deviceName.empty() ? L"(unknown)" : snap.deviceName;
    r += L"\r\nEndpoint: ";
    r += snap.endpointId.empty() ? L"(none)" : snap.endpointId;

    r += L"\r\n\r\nVERDICT [";
    r += SeverityLabel(v.severity);
    r += L"]\r\n";
    r += v.title;
    r += L"\r\n";
    r += v.detail;
    if (!v.nextStep.empty()) {
        r += L"\r\nNext step: ";
        r += v.nextStep;
    }

    r += L"\r\n\r\nENGINE\r\n";
    {
        wchar_t line[160] = {};
        StringCchPrintfW(line, ARRAYSIZE(line), L"audiodg.exe PID: %lu\r\n", snap.audiodgPid);
        r += line;
    }
    r += L"MiniEQ_APO.dll loaded in engine: ";
    r += DllStateText(snap.dllLoaded);
    if (snap.audiodgRestartLoop) {
        r += L"\r\nEngine restarts: 3+ in the last 10 min -- crash loop";
    }
    r += L"\r\nHeartbeat: ";
    if (!snap.statusChannelOk) {
        r += L"no signal yet";
    } else {
        wchar_t line[160] = {};
        StringCchPrintfW(line, ARRAYSIZE(line), L"%s (%lld APOProcess calls)",
                         snap.heartbeatFresh ? L"advancing" : L"stalled",
                         snap.heartbeatCalls);
        r += line;
    }
    r += L"\r\nAPO stream: ";
    if (snap.statusChannelOk && snap.apoLocked) {
        wchar_t line[160] = {};
        StringCchPrintfW(line, ARRAYSIZE(line), L"locked, %d ch @ %d Hz (init %s)",
                         snap.apoChannels, snap.apoSampleRate,
                         snap.apoInitOk ? L"ok" : L"failed");
        r += line;
    } else {
        r += L"not locked";
    }
    r += L"\r\nMiniEQ enabled: ";
    r += MiniEQ_GlobalEnabledGet() ? L"yes" : L"no (user bypass \u2014 audio passes through)";
    r += L"\r\nExclusive-mode holder: ";
    r += L"not probed (the old live check created a real stream and could "
         L"destabilize the engine; an exclusive-mode app bypasses APOs by design)";

    r += L"\r\n\r\nREGISTRATION\r\n";
    r += MiniEQ_EffectSlotShortName();
    r += L" slot points at MiniEQ: ";
    r += snap.attached ? L"yes" : L"no";
    r += L"\r\nEngine APO declaration: ";
    r += snap.apoDeclared > 0 ? L"present" :
         (snap.apoDeclared == 0 ? L"MISSING (slot is a dead pointer)" : L"unknown");
    r += L"\r\nSFX slot: ";
    r += snap.sfxSlotClsid.empty() ? L"(empty)" : snap.sfxSlotClsid.c_str();
    r += L"\r\nEFX slot: ";
    r += snap.efxSlotClsid.empty() ? L"(empty)" : snap.efxSlotClsid.c_str();
    if (!snap.childStashClsid.empty()) {
        r += L"\r\nDisplaced-APO stash: ";
        r += snap.childStashClsid.c_str();
    }
    r += L"\r\nDLL path: ";
    r += snap.dllPath.empty() ? L"(not registered)" : snap.dllPath;
    r += L"\r\nDLL file exists: ";
    r += snap.dllExists ? L"yes" : L"no";
    r += L"\r\nAudio enhancements: ";
    r += EnhText(snap.enhancements);

    r += L"\r\n\r\nSESSIONS\r\n";
    if (snap.sessions.empty()) {
        r += L"(none)";
    } else {
        for (const DiagSessionInfo& si : snap.sessions) {
            wchar_t line[320] = {};
            wchar_t peak[16] = L"?";
            if (si.peak >= 0.0f) {
                StringCchPrintfW(peak, ARRAYSIZE(peak), L"%d%%",
                                 (int)(si.peak * 100.0f + 0.5f));
            }
            StringCchPrintfW(line, ARRAYSIZE(line), L"%s (PID %lu) \u2014 %s, peak %s\r\n",
                             si.exe.c_str(), si.pid,
                             si.active ? L"ACTIVE" : L"idle", peak);
            r += line;
        }
    }
    r += L"\r\n";
    return r;
}

// ---------------------------------------------------------------------------
// Background diagnosis worker
//
// The full snapshot (process/module snapshots, registry + COM reads) used to
// run on the UI thread every second -- any slow probe froze the whole
// window while data was loading. Now one worker thread per window runs the
// probes; the finished bundle is handed over under a lock and the window is
// poked with a posted message. The UI only ever paints the last finished
// bundle, so a slow refresh degrades to "slightly older data", never a hang.
// ---------------------------------------------------------------------------

DiagBundle MiniEQ_RunDiagnosisLocked(const std::wstring& endpointId) {
    // One lock for every background diagnosis in the process: the freshness
    // and crash-loop detectors keep "last poll" statics that only stay
    // coherent when polls don't overlap.
    static std::mutex s_mutex;
    std::lock_guard<std::mutex> lk(s_mutex);
    DiagBundle b;
    b.snap = MiniEQ_RunDiagnosis(endpointId);
    b.spatial = MiniEQ_ReadSpatialSound(endpointId);
    return b;
}

static DWORD WINAPI DiagWorkerProc(LPVOID param) {
    DiagAsyncState* a = static_cast<DiagAsyncState*>(param);
    // The probes use COM (MMDeviceEnumerator, property stores); the recovery
    // thread already runs these same calls in MTA, so do the same here.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE ev[2] = { a->hStop, a->hWake };
    for (;;) {
        const DWORD w = WaitForMultipleObjects(2, ev, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0 + 1) {
            break; // stop requested (or the wait itself failed)
        }
        std::wstring ep;
        uint64_t gen = 0;
        AcquireSRWLockShared(&a->lock);
        ep = a->endpoint;
        gen = a->generation;
        ReleaseSRWLockShared(&a->lock);
        DiagBundle b = MiniEQ_RunDiagnosisLocked(ep);
        AcquireSRWLockExclusive(&a->lock);
        if (gen != a->generation) {
            // The window was torn down and re-created while this probe ran;
            // drop the stale bundle instead of posting it to the new window.
            ReleaseSRWLockExclusive(&a->lock);
            continue;
        }
        a->result.reset(new (std::nothrow) DiagBundle(std::move(b)));
        a->hasResult = (a->result != nullptr);
        a->busy = false;
        const HWND hwnd = a->hwnd;
        const UINT msg = a->doneMsg;
        ReleaseSRWLockExclusive(&a->lock);
        if (hwnd != nullptr) {
            PostMessageW(hwnd, msg, 0, 0);
        }
    }
    CoUninitialize();
    return 0;
}

void MiniEQ_DiagAsyncStart(DiagAsyncState* a, HWND hwnd, UINT doneMsg,
                           const std::wstring& endpoint) {
    AcquireSRWLockExclusive(&a->lock);
    a->hwnd = hwnd;
    a->doneMsg = doneMsg;
    a->endpoint = endpoint;
    ++a->generation; // any in-flight probe from a previous life is now stale
    a->busy = false;
    a->hasResult = false;
    a->result.reset();
    ReleaseSRWLockExclusive(&a->lock);
    a->hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    a->hWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    a->hThread = CreateThread(nullptr, 0, DiagWorkerProc, a, 0, nullptr);
}

void MiniEQ_DiagAsyncSetEndpoint(DiagAsyncState* a,
                                 const std::wstring& endpoint) {
    AcquireSRWLockExclusive(&a->lock);
    a->endpoint = endpoint;
    ReleaseSRWLockExclusive(&a->lock);
}

void MiniEQ_DiagAsyncRequest(DiagAsyncState* a) {
    bool kick = false;
    AcquireSRWLockExclusive(&a->lock);
    if (!a->busy && a->hThread != nullptr) {
        a->busy = true;
        kick = true;
    }
    ReleaseSRWLockExclusive(&a->lock);
    if (kick) {
        SetEvent(a->hWake);
    }
    // Busy: the in-flight run will post doneMsg when it lands, and the
    // window's 1 s timer re-asks anyway -- requests coalesce, never queue.
}

bool MiniEQ_DiagAsyncTake(DiagAsyncState* a, DiagBundle& out) {
    AcquireSRWLockExclusive(&a->lock);
    if (!a->hasResult || a->result == nullptr) {
        ReleaseSRWLockExclusive(&a->lock);
        return false;
    }
    out = std::move(*a->result);
    a->result.reset();
    a->hasResult = false;
    ReleaseSRWLockExclusive(&a->lock);
    return true;
}

void MiniEQ_DiagAsyncStop(DiagAsyncState* a) {
    AcquireSRWLockExclusive(&a->lock);
    a->hwnd = nullptr; // the window is going away: no more PostMessage
    ++a->generation;   // a wedged probe finishing late drops its bundle
    ReleaseSRWLockExclusive(&a->lock);
    if (a->hThread == nullptr) {
        return;
    }
    SetEvent(a->hStop);
    // Bounded wait: a probe stuck hard (wedged COM call) must not hang
    // window teardown. The worker owns no window state, so abandoning it in
    // that pathological case is safe -- it just exits when the call returns.
    WaitForSingleObject(a->hThread, 3000);
    CloseHandle(a->hThread);
    a->hThread = nullptr;
    CloseHandle(a->hWake);
    a->hWake = nullptr;
    CloseHandle(a->hStop);
    a->hStop = nullptr;
    a->result.reset();
    a->hasResult = false;
    a->busy = false;
}

// ---------------------------------------------------------------------------
// The Diagnostics Center window (modeless)
// ---------------------------------------------------------------------------

#include <commctrl.h>
#include <vector>
#include <memory>
#include <atomic>

enum {
    IDC_DC_VERDICT = 301,
    IDC_DC_ENGINE,
    IDC_DC_REG,
    IDC_DC_SESSIONS,
    IDC_DC_LOG,
    IDC_DC_RUN,
    IDC_DC_COPY,
    IDC_DC_COPYLOG,
    IDC_DC_FULLLOG,
    IDC_DC_FORENSICS,
    IDC_DC_CLOSE,
};

// Posted by the background diagnosis worker when a fresh bundle is ready.
#define WM_DC_DIAGDONE (WM_APP + 11)
// Posted by the forensics worker when the report file is written (lParam:
// 0 = ok, path in s_forensicsPath; nonzero = failed).
#define WM_DC_FORENSICSDONE (WM_APP + 12)

static const wchar_t* kDcClass = L"MiniEQDiagCenter";

static const COLORREF kVerdictBg[4] = {
    RGB(243, 243, 243), // Neutral
    RGB(223, 242, 223), // Good
    RGB(255, 243, 205), // Warn
    RGB(253, 226, 226), // Bad
};
static const COLORREF kVerdictFg[4] = {
    RGB(60, 60, 60),
    RGB(30, 90, 30),
    RGB(122, 82, 10),
    RGB(140, 30, 30),
};

static HINSTANCE   s_hInst = nullptr;
static HWND        s_hDlg = nullptr;
static HWND        s_hVerdict = nullptr;
static HWND        s_hEngine = nullptr;
static HWND        s_hReg = nullptr;
static HWND        s_hSessions = nullptr;
static HWND        s_hLog = nullptr;
static HWND        s_hGrpEngine = nullptr;
static HWND        s_hGrpReg = nullptr;
static HWND        s_hGrpSessions = nullptr;
static HWND        s_hGrpLog = nullptr;
static int         s_verdictExtra = 0; // verdict pane growth beyond 102px
static HFONT       s_font = nullptr;
static HFONT       s_fontBold = nullptr;
static HBRUSH      s_verdictBrush[4] = {};
static DiagSeverity s_verdictSeverity = DiagSeverity::Neutral;

static std::wstring s_endpoint;
static DiagSnapshot s_lastSnap;
static DiagVerdict  s_lastVerdict;

// Background diagnosis: the worker thread runs the probes, the window only
// paints finished bundles. Static storage: outlives any in-flight run.
static DiagAsyncState s_diagAsync;
static bool           s_forceLogNext = false; // "Run diagnosis" re-logs baseline

// Transition-logging state: only log when something actually changed.
static bool             s_haveLogged = false;
static std::wstring     s_loggedVerdict;
static int              s_loggedDll = -2;
static bool             s_loggedLoop = false;
static DiagEnhancements s_loggedEnh = DiagEnhancements::Unknown;
static bool             s_loggedHb = false;
static std::wstring     s_loggedSessSig;
static std::wstring     s_shownVerdictTitle;
static ULONGLONG        s_lastLogSize = 0;

static HFONT DcMakeFont(bool bold) {
    HDC hdc = GetDC(nullptr);
    const int px = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, hdc);
    return CreateFontW(px, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void SetTextIfChanged(HWND h, const std::wstring& text) {
    wchar_t cur[4096] = {};
    GetWindowTextW(h, cur, ARRAYSIZE(cur));
    if (text != cur) {
        SetWindowTextW(h, text.c_str());
    }
}

static std::wstring SessionSignature(const std::vector<DiagSessionInfo>& sessions) {
    std::wstring sig;
    for (const DiagSessionInfo& si : sessions) {
        sig += si.exe;
        sig += L"|";
        sig += std::to_wstring(si.pid);
        sig += si.active ? L"|1;" : L"|0;";
    }
    return sig;
}

// Write the evidence trail into the log, but only on change (or on demand).
static void LogTransitions(const DiagSnapshot& snap, const DiagVerdict& v, bool force) {
    const bool all = force || !s_haveLogged;
    if (all || v.title != s_loggedVerdict) {
        MiniEQ_AppLogCat(L"DIAG", L"verdict: %s -- %s", v.title.c_str(), v.detail.c_str());
        s_loggedVerdict = v.title;
    }
    if (all || snap.dllLoaded != s_loggedDll) {
        MiniEQ_AppLogCat(L"ENGINE", L"audiodg.exe pid=%lu, MiniEQ_APO.dll %s",
                         snap.audiodgPid, DllStateText(snap.dllLoaded));
        s_loggedDll = snap.dllLoaded;
    }
    if (all || snap.audiodgRestartLoop != s_loggedLoop) {
        if (snap.audiodgRestartLoop) {
            MiniEQ_AppLogCat(L"ENGINE", L"audiodg.exe restart loop detected "
                             L"(3+ PID changes in 10 min) -- engine keeps dying");
        }
        s_loggedLoop = snap.audiodgRestartLoop;
    }
    if (all || snap.enhancements != s_loggedEnh) {
        MiniEQ_AppLogCat(L"REG", L"audio enhancements: %s", EnhText(snap.enhancements));
        s_loggedEnh = snap.enhancements;
    }
    if (all || snap.heartbeatFresh != s_loggedHb) {
        MiniEQ_AppLogCat(L"ENGINE", L"heartbeat %s (%lld APOProcess calls)",
                         snap.heartbeatFresh ? L"advancing" : L"stalled/absent",
                         snap.heartbeatCalls);
        s_loggedHb = snap.heartbeatFresh;
    }
    const std::wstring sig = SessionSignature(snap.sessions);
    if (all || sig != s_loggedSessSig) {
        if (snap.sessions.empty()) {
            MiniEQ_AppLogCat(L"SESSION", L"no audio sessions on this endpoint");
        } else {
            for (const DiagSessionInfo& si : snap.sessions) {
                wchar_t peak[16] = L"?";
                if (si.peak >= 0.0f) {
                    StringCchPrintfW(peak, ARRAYSIZE(peak), L"%d%%",
                                     (int)(si.peak * 100.0f + 0.5f));
                }
                MiniEQ_AppLogCat(L"SESSION", L"%s pid=%lu %s peak=%s",
                                 si.exe.c_str(), si.pid,
                                 si.active ? L"ACTIVE" : L"idle", peak);
            }
        }
        s_loggedSessSig = sig;
    }
    s_haveLogged = true;
}

// Sets one wide-text cell in the sessions list. The ListView_*W helper
// macros are not available in this build configuration (C3861), so this
// talks to the control directly with SendMessageW -- LVITEMW and the
// LVM_*W message constants come from the same <commctrl.h> that already
// compiles, so there is no new dependency.
static void SetListCellW(HWND lv, int row, int col, LPWSTR text) {
    LVITEMW it = {};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = col;
    it.pszText = text;
    (void)SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

// Skip rebuilds when nothing changed: deleting + re-inserting rows every
// second makes the list flash and steals scroll position mid-crash-loop.
static std::wstring s_lastSessListSig;

static void UpdateSessionList(const DiagSnapshot& snap) {
    const std::wstring sig = SessionSignature(snap.sessions);
    if (sig == s_lastSessListSig) {
        return;
    }
    s_lastSessListSig = sig;
    ListView_DeleteAllItems(s_hSessions);
    int row = 0;
    for (const DiagSessionInfo& si : snap.sessions) {
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = const_cast<LPWSTR>(si.exe.c_str());
        // NOTE: the ListView_*W helper macros are unavailable in this build
        // configuration (C3861), so this sends LVM_INSERTITEMW directly.
        (void)SendMessageW(s_hSessions, LVM_INSERTITEMW, 0, (LPARAM)&it);

        wchar_t pid[32] = {};
        StringCchPrintfW(pid, ARRAYSIZE(pid), L"%lu", si.pid);
        SetListCellW(s_hSessions, row, 1, pid);
        SetListCellW(s_hSessions, row, 2,
                     const_cast<LPWSTR>(si.active ? L"Active" : L"Idle"));

        std::wstring level;
        if (si.peak >= 0.0f) {
            const int n = (int)(si.peak * 10.0f + 0.5f);
            for (int i = 0; i < 10; ++i) {
                level += (i < n) ? L"\u2588" : L"\u2591";
            }
            wchar_t pct[16] = {};
            StringCchPrintfW(pct, ARRAYSIZE(pct), L" %d%%",
                             (int)(si.peak * 100.0f + 0.5f));
            level += pct;
        } else {
            level = L"\u2014";
        }
        SetListCellW(s_hSessions, row, 3, const_cast<LPWSTR>(level.c_str()));
        ++row;
    }
}

// Compact tail of the shared log (last 24 lines), refreshed on file change.
static void UpdateLogTail() {
    if (s_hLog == nullptr) {
        return;
    }
    HANDLE h = CreateFileW(MiniEQ_DiagLogPath().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size = {};
    const bool okSize = GetFileSizeEx(h, &size) && size.QuadPart > 0;
    if (!okSize) {
        CloseHandle(h);
        SetTextIfChanged(s_hLog, L"(log is empty)");
        return;
    }
    if ((ULONGLONG)size.QuadPart == s_lastLogSize) {
        CloseHandle(h);
        return; // unchanged
    }
    s_lastLogSize = (ULONGLONG)size.QuadPart;

    LONGLONG off = size.QuadPart - 8192;
    if (off < 0) {
        off = 0;
    }
    // Align to a UTF-16 code-unit boundary: an odd offset would shift every
    // character by one byte (gibberish) in the reinterpret_cast below.
    off &= ~1LL;
    LARGE_INTEGER li = {};
    li.QuadPart = off;
    SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    const DWORD toRead = (DWORD)(size.QuadPart - off);
    std::vector<BYTE> buf((size_t)toRead + 2, 0);
    DWORD got = 0;
    ReadFile(h, buf.data(), toRead, &got, nullptr);
    CloseHandle(h);

    const wchar_t* w = reinterpret_cast<const wchar_t*>(buf.data());
    size_t nch = got / 2;
    size_t start = 0;
    if (nch > 0 && w[0] == 0xFEFF) {
        start = 1; // BOM
    }
    if (off > 0) {
        while (start < nch && w[start] != L'\n') { // skip partial first line
            ++start;
        }
        if (start < nch) {
            ++start;
        }
    }
    std::vector<size_t> lineStarts;
    lineStarts.push_back(start);
    for (size_t i = start; i < nch; ++i) {
        if (w[i] == L'\n' && i + 1 < nch) {
            lineStarts.push_back(i + 1);
        }
    }
    size_t keep = 0;
    if (lineStarts.size() > 24) {
        keep = lineStarts.size() - 24;
    }
    const std::wstring text(w + lineStarts[keep], nch - lineStarts[keep]);
    SetWindowTextW(s_hLog, text.c_str());
    const LRESULT len = (LRESULT)text.size();
    SendMessageW(s_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(s_hLog, EM_SCROLLCARET, 0, 0);
}

// The verdict copy varies in length -- some diagnoses need three
// paragraphs. Measure the wrapped text in the real (bold) font and grow
// the verdict pane so the whole text is always visible: nothing is
// clipped and no control below is ever painted over. Every below-fold
// control is repositioned from its base coordinates each time, so the
// layout can never drift; the window grows with the content and never
// shrinks below its designed 702 px height.
static void DcRelayoutForVerdict(const std::wstring& vt) {
    if (s_hDlg == nullptr || s_hVerdict == nullptr || s_fontBold == nullptr) {
        return;
    }
    int need = 102;
    HDC hdc = GetDC(s_hDlg);
    if (hdc != nullptr) {
        HFONT old = (HFONT)SelectObject(hdc, s_fontBold);
        RECT rc = { 0, 0, 736, 0 };
        DrawTextW(hdc, vt.c_str(), -1, &rc, DT_WORDBREAK | DT_CALCRECT);
        SelectObject(hdc, old);
        ReleaseDC(s_hDlg, hdc);
        need = rc.bottom + 16; // breathing room above/below the text
        if (need < 102) {
            need = 102;
        }
    }
    const int extra = need - 102;
    if (extra == s_verdictExtra) {
        return;
    }
    s_verdictExtra = extra;
    SetWindowPos(s_hVerdict, nullptr, 0, 0, 736, need, SWP_NOMOVE | SWP_NOZORDER);
    struct DcItem { HWND hwnd; int x, y, w, h; };
    const DcItem items[] = {
        { s_hGrpEngine, 12, 120, 288, 132 },
        { s_hEngine, 22, 142, 268, 102 },
        { s_hGrpReg, 308, 120, 440, 132 },
        { s_hReg, 318, 142, 420, 102 },
        { s_hGrpSessions, 12, 260, 736, 168 },
        { s_hSessions, 22, 282, 716, 138 },
        { s_hGrpLog, 12, 436, 736, 168 },
        { s_hLog, 22, 458, 716, 138 },
    };
    for (const DcItem& it : items) {
        if (it.hwnd != nullptr) {
            SetWindowPos(it.hwnd, nullptr, it.x, it.y + extra, it.w, it.h,
                         SWP_NOZORDER);
        }
    }
    const int btnY = 616 + extra;
    HWND btns[6] = {
        GetDlgItem(s_hDlg, IDC_DC_RUN), GetDlgItem(s_hDlg, IDC_DC_COPY),
        GetDlgItem(s_hDlg, IDC_DC_COPYLOG), GetDlgItem(s_hDlg, IDC_DC_FULLLOG),
        GetDlgItem(s_hDlg, IDC_DC_FORENSICS),
        GetDlgItem(s_hDlg, IDC_DC_CLOSE),
    };
    const int btnX[6] = { 12, 140, 258, 366, 484, 632 };
    for (int i = 0; i < 6; ++i) {
        if (btns[i] != nullptr) {
            SetWindowPos(btns[i], nullptr, btnX[i], btnY, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
        }
    }
    SetWindowPos(s_hDlg, nullptr, 0, 0, 760, 702 + extra,
                 SWP_NOMOVE | SWP_NOZORDER);
    InvalidateRect(s_hDlg, nullptr, TRUE);
}

// Paint one finished bundle. Runs on the UI thread; the heavy probes
// already happened on the worker thread, so this never blocks.
static void RenderDiagSnapshot(const DiagBundle& b, bool forceLog) {
    if (s_hDlg == nullptr) {
        return;
    }
    const DiagSnapshot& snap = b.snap;
    const DiagVerdict v = MiniEQ_MakeVerdict(snap);

    std::wstring vt = v.title + L"\r\n\r\n" + v.detail;
    if (!v.nextStep.empty()) {
        vt += L"\r\n\r\nNext step: " + v.nextStep;
    }
    SetTextIfChanged(s_hVerdict, vt);
    DcRelayoutForVerdict(vt);
    if (v.title != s_shownVerdictTitle) {
        s_shownVerdictTitle = v.title;
        s_verdictSeverity = v.severity;
        InvalidateRect(s_hVerdict, nullptr, TRUE);
    }

    std::wstring eng = L"Audio engine: ";
    if (snap.audiodgPid != 0) {
        eng += L"audiodg.exe (PID " + std::to_wstring(snap.audiodgPid) + L")";
    } else {
        eng += L"audiodg.exe not found";
    }
    eng += L"\r\nMiniEQ_APO.dll in engine: ";
    eng += DllStateText(snap.dllLoaded);
    if (snap.audiodgRestartLoop) {
        eng += L"\r\nEngine restarts: 3+ in the last 10 min \u2014 crash loop!";
    }
    eng += L"\r\nHeartbeat: ";
    if (!snap.statusChannelOk) {
        eng += L"no signal yet";
    } else if (snap.heartbeatFresh) {
        eng += L"advancing (" + std::to_wstring(snap.heartbeatCalls) + L" calls)";
    } else {
        eng += L"stalled (" + std::to_wstring(snap.heartbeatCalls) + L" calls)";
    }
    eng += L"\r\nAPO stream: ";
    if (snap.statusChannelOk && snap.apoLocked) {
        wchar_t line[128] = {};
        StringCchPrintfW(line, ARRAYSIZE(line), L"locked, %d ch @ %d Hz",
                         snap.apoChannels, snap.apoSampleRate);
        eng += line;
    } else {
        eng += L"not locked";
    }
    SetTextIfChanged(s_hEngine, eng);

    std::wstring reg = MiniEQ_EffectSlotShortName();
    reg += L" slot \u2192 MiniEQ: ";
    reg += snap.attached ? L"yes \u2713" : L"no";
    reg += L"\r\nDLL path: ";
    reg += snap.dllPath.empty() ? L"(not registered)" : snap.dllPath;
    reg += L"\r\nDLL file exists: ";
    reg += snap.dllExists ? L"yes" : L"no";
    reg += L"\r\nAudio enhancements: ";
    reg += EnhText(snap.enhancements);
    SetTextIfChanged(s_hReg, reg);

    UpdateSessionList(snap);
    LogTransitions(snap, v, forceLog);
    // (Log tail refreshes on the 1 s timer; no file I/O here.)

    s_lastSnap = snap;
    s_lastVerdict = v;
}

// Ask the worker for a fresh bundle. Cheap: coalesces while a run is in
// flight, so the UI thread never waits on the probes.
static void RequestDiagRefresh() {
    MiniEQ_DiagAsyncRequest(&s_diagAsync);
}

static void DcCopyReport(HWND hwnd) {
    const std::wstring report = MiniEQ_FormatReport(s_lastSnap, s_lastVerdict);
    if (MiniEQ_CopyTextToClipboard(hwnd, report)) {
        MiniEQ_AppLogCat(L"DIAG", L"report copied to clipboard");
    }
}

// Copy the entire APO trace log (apo-trace.log) in one click, so the full
// history can be pasted for support. Capped at 512 KB: the tail stays useful
// even on machines with long uptimes, and the clipboard stays sane.
static void DcCopyLog(HWND hwnd) {
    std::wstring text;
    HANDLE h = CreateFileW(MiniEQ_DiagLogPath().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
            // Cap at the last 512 KB so the clipboard stays sane even on
            // machines with long uptimes.
            const LONGLONG cap = 512LL * 1024;
            LONGLONG off = size.QuadPart - cap;
            const bool truncated = off > 0;
            if (off < 0) {
                off = 0;
            }
            // Align to a UTF-16 code-unit boundary: an odd offset would
            // shift every character by one byte (gibberish).
            off &= ~1LL;
            LARGE_INTEGER li = {};
            li.QuadPart = off;
            SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
            const DWORD toRead = static_cast<DWORD>(size.QuadPart - off);
            std::vector<BYTE> buf(static_cast<size_t>(toRead) + 2, 0);
            DWORD got = 0;
            if (ReadFile(h, buf.data(), toRead, &got, nullptr) && got > 0) {
                const wchar_t* w =
                    reinterpret_cast<const wchar_t*>(buf.data());
                size_t n = got / sizeof(wchar_t);
                if (n > 0 && w[0] == 0xFEFF) {
                    ++w;
                    --n; // drop the BOM
                }
                text.assign(w, n);
                if (truncated) {
                    text = L"...(truncated to the last 512 KB)...\r\n" + text;
                }
            }
        }
        CloseHandle(h);
    }
    if (text.empty()) {
        MessageBoxW(hwnd, L"The APO trace log is empty or unavailable.",
                    L"MiniEQ diagnostics", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (MiniEQ_CopyTextToClipboard(hwnd, text)) {
        MiniEQ_AppLogCat(L"DIAG", L"full APO log copied to clipboard");
    }
}

// ---------------------------------------------------------------------------
// Crash forensics: one click collects everything needed to diagnose an
// audiodg.exe crash loop and writes it as a single UTF-16 report on the
// Desktop. Runs on a worker thread (event-log + filesystem I/O); the UI
// thread never blocks. Sections:
//   1. Current diagnosis (the same report as "Copy report")
//   2. Recent Application Error events (ID 1000) for AUDIODG.EXE, via wevtapi
//   3. WER ReportArchive entries for AUDIODG.EXE crashes (+ Report.wer head)
//   4. Existing C:\CrashDumps\audiodg.exe.*.dmp files (metadata, not contents)
//   5. Last 64 KB of apo-trace.log
// ---------------------------------------------------------------------------

static std::atomic<bool> s_forensicsBusy{false}; // one collection at a time

// Completion result owned by the worker and handed to the UI thread via
// WM_DC_FORENSICSDONE's LPARAM -- no cross-thread statics, no races.
struct ForensicsResult {
    bool         ok = false;
    std::wstring path;
};

// Diagnosis inputs captured on the UI thread so the worker never races the
// live snapshot.
struct ForensicsInput {
    DiagSnapshot snap;
    DiagVerdict  verdict;
    HWND         hwnd;
};

static void ForensicsAppendLine(std::wstring& out, const wchar_t* s) {
    out += s;
    out += L"\r\n";
}

// Last 64 KB of a UTF-16 log, aligned to a code-unit boundary.
static std::wstring ForensicsLogTail(const std::wstring& path, LONGLONG cap) {
    std::wstring text;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return text;
    }
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
        LONGLONG off = size.QuadPart - cap;
        const bool truncated = off > 0;
        if (off < 0) {
            off = 0;
        }
        off &= ~1LL; // UTF-16 alignment: an odd offset shifts every char
        LARGE_INTEGER li = {};
        li.QuadPart = off;
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
        const DWORD toRead = static_cast<DWORD>(size.QuadPart - off);
        std::vector<BYTE> buf(static_cast<size_t>(toRead) + 2, 0);
        DWORD got = 0;
        if (ReadFile(h, buf.data(), toRead, &got, nullptr) && got > 0) {
            const wchar_t* w = reinterpret_cast<const wchar_t*>(buf.data());
            size_t n = got / sizeof(wchar_t);
            if (n > 0 && w[0] == 0xFEFF) {
                ++w;
                --n;
            }
            text.assign(w, n);
            if (truncated) {
                text = L"...(truncated to the last 64 KB)...\r\n" + text;
            }
        }
    }
    CloseHandle(h);
    return text;
}

static void ForensicsAppendEventLog(std::wstring& out) {
    ForensicsAppendLine(out, L"===== APPLICATION ERROR EVENTS (AUDIODG.EXE, ID 1000) =====");
    EVT_HANDLE hQuery = EvtQuery(nullptr, L"Application",
        L"*[System[(EventID=1000)]]",
        EvtQueryChannelPath | EvtQueryReverseDirection);
    if (hQuery == nullptr) {
        ForensicsAppendLine(out, L"(could not open the Application event log)");
        return;
    }
    int kept = 0;
    for (int scanned = 0; scanned < 300 && kept < 20; ++scanned) {
        EVT_HANDLE hEvent = nullptr;
        DWORD returned = 0;
        if (!EvtNext(hQuery, 1, &hEvent, INFINITE, 0, &returned) || returned == 0) {
            break; // no more events
        }
        // Render the event as XML and keep it if it blames AUDIODG.EXE.
        DWORD bufSize = 0, bufUsed = 0, propCount = 0;
        EvtRender(nullptr, hEvent, EvtRenderEventXml, 0, nullptr, &bufUsed, &propCount);
        std::wstring xml;
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bufUsed > 0) {
            std::vector<wchar_t> buf(bufUsed / sizeof(wchar_t) + 1, 0);
            bufSize = (DWORD)(buf.size() * sizeof(wchar_t));
            if (EvtRender(nullptr, hEvent, EvtRenderEventXml, bufSize,
                          buf.data(), &bufUsed, &propCount)) {
                xml.assign(buf.data());
            }
        }
        EvtClose(hEvent);
        if (xml.find(L"AUDIODG.EXE") == std::wstring::npos &&
            xml.find(L"audiodg.exe") == std::wstring::npos) {
            continue;
        }
        ++kept;
        wchar_t head[64] = {};
        StringCchPrintfW(head, ARRAYSIZE(head), L"--- crash event %d ---", kept);
        ForensicsAppendLine(out, head);
        // Cap each event: the XML carries everything (faulting module,
        // exception code, offset) but can be long.
        if (xml.size() > 4000) {
            xml.resize(4000);
            xml += L"\r\n...(event XML truncated)...";
        }
        out += xml;
        out += L"\r\n";
    }
    EvtClose(hQuery);
    if (kept == 0) {
        ForensicsAppendLine(out, L"(no AUDIODG.EXE Application Error events found)");
    }
}

static void ForensicsAppendWerArchive(std::wstring& out) {
    ForensicsAppendLine(out, L"===== WER REPORT ARCHIVE (AUDIODG.EXE) =====");
    const wchar_t* base =
        L"C:\\ProgramData\\Microsoft\\Windows\\WER\\ReportArchive\\";
    WIN32_FIND_DATAW fd = {};
    std::wstring pattern = std::wstring(base) + L"AppCrash_AUDIODG.EXE_*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        ForensicsAppendLine(out, L"(no archived AUDIODG.EXE crash reports)");
        return;
    }
    int n = 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        if (++n > 10) {
            ForensicsAppendLine(out, L"...(more archived reports omitted)...");
            break;
        }
        wchar_t head[320] = {};
        StringCchPrintfW(head, ARRAYSIZE(head), L"--- %s ---", fd.cFileName);
        ForensicsAppendLine(out, head);
        // The Report.wer inside names the faulting module + exception.
        std::wstring wer = std::wstring(base) + fd.cFileName + L"\\Report.wer";
        HANDLE wh = CreateFileW(wer.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
        if (wh != INVALID_HANDLE_VALUE) {
            char buf[2048] = {};
            DWORD got = 0;
            if (ReadFile(wh, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
                // Report.wer is UTF-16LE (with BOM) in practice; sniff the
                // BOM instead of guessing. (The old code tried UTF-8 first,
                // which "succeeded" on a single char of UTF-16 and produced
                // garbage.)
                wchar_t wbuf[2048] = {};
                const unsigned char* ub = (const unsigned char*)buf;
                if (got >= 2 && ub[0] == 0xFF && ub[1] == 0xFE) {
                    // UTF-16LE: copy raw, skipping the BOM.
                    size_t wn = (got - 2) / sizeof(wchar_t);
                    if (wn > ARRAYSIZE(wbuf) - 1) wn = ARRAYSIZE(wbuf) - 1;
                    memcpy(wbuf, buf + 2, wn * sizeof(wchar_t));
                    wbuf[wn] = 0;
                } else {
                    // UTF-8 (with or without BOM) or ANSI: lossy convert.
                    // Use the byte count, not -1: the buffer isn't a C
                    // string and may contain embedded NULs.
                    const char* src = buf;
                    int srclen = (int)got;
                    if (got >= 3 && ub[0] == 0xEF && ub[1] == 0xBB &&
                        ub[2] == 0xBF) {
                        src += 3; // skip UTF-8 BOM
                        srclen -= 3;
                    }
                    MultiByteToWideChar(CP_UTF8, 0, src, srclen, wbuf,
                                        ARRAYSIZE(wbuf) - 1);
                }
                out += wbuf;
                out += L"\r\n";
            }
            CloseHandle(wh);
        } else {
            ForensicsAppendLine(out, L"(Report.wer not readable)");
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void ForensicsAppendCrashDumps(std::wstring& out) {
    ForensicsAppendLine(out, L"===== CRASH DUMPS (C:\\CrashDumps) =====");
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(L"C:\\CrashDumps\\audiodg.exe.*.dmp", &fd);
    if (h == INVALID_HANDLE_VALUE) {
        h = FindFirstFileW(L"C:\\CrashDumps\\AUDIODG.EXE.*.dmp", &fd);
    }
    if (h == INVALID_HANDLE_VALUE) {
        ForensicsAppendLine(out,
            L"(no audiodg.exe dumps -- set up C:\\CrashDumps capture to get one)");
        return;
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const ULONGLONG bytes =
            (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        SYSTEMTIME st = {};
        FileTimeToSystemTime(&fd.ftLastWriteTime, &st);
        wchar_t line[384] = {};
        StringCchPrintfW(line, ARRAYSIZE(line),
            L"%s  (%llu KB, written %04d-%02d-%02d %02d:%02d)",
            fd.cFileName, bytes / 1024,
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        ForensicsAppendLine(out, line);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static DWORD WINAPI ForensicsThread(LPVOID param) {
    // Input was captured on the UI thread (no race with the live snapshot).
    std::unique_ptr<ForensicsInput> in(static_cast<ForensicsInput*>(param));
    const HWND hwnd = in->hwnd;
    std::wstring report;
    ForensicsAppendLine(report, L"MiniEQ crash forensics");
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t when[128] = {};
    StringCchPrintfW(when, ARRAYSIZE(when),
        L"Collected %04d-%02d-%02d %02d:%02d:%02d on this machine",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    ForensicsAppendLine(report, when);
    if (!in->snap.deviceName.empty()) {
        ForensicsAppendLine(report, (L"Device: " + in->snap.deviceName).c_str());
    }
    ForensicsAppendLine(report, L"");
    ForensicsAppendLine(report, L"===== CURRENT DIAGNOSIS =====");
    report += MiniEQ_FormatReport(in->snap, in->verdict);
    report += L"\r\n";
    ForensicsAppendEventLog(report);
    report += L"\r\n";
    ForensicsAppendWerArchive(report);
    report += L"\r\n";
    ForensicsAppendCrashDumps(report);
    report += L"\r\n";
    ForensicsAppendLine(report, L"===== APO TRACE LOG (last 64 KB) =====");
    const std::wstring tail =
        ForensicsLogTail(MiniEQ_DiagLogPath(), 64LL * 1024);
    report += tail.empty() ? L"(log empty or unavailable)" : tail;
    report += L"\r\n";

    // Write UTF-16 with BOM to the Desktop.
    wchar_t desktop[MAX_PATH] = {};
    auto result = std::unique_ptr<ForensicsResult>(
        new (std::nothrow) ForensicsResult());
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr,
                                   0, desktop)) && desktop[0] != 0 &&
        result) {
        wchar_t name[128] = {};
        StringCchPrintfW(name, ARRAYSIZE(name),
            L"MiniEQ-forensics-%04d%02d%02d-%02d%02d%02d.txt",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        result->path = std::wstring(desktop) + L"\\" + name;
        HANDLE h = CreateFileW(result->path.c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const wchar_t bom = 0xFEFF;
            const DWORD want =
                (DWORD)(sizeof(bom) + report.size() * sizeof(wchar_t));
            DWORD written = 0;
            // WriteFile may report success on a partial write: verify the
            // full byte count landed, BOM included.
            BOOL wok = WriteFile(h, &bom, sizeof(bom), &written, nullptr);
            DWORD w2 = 0;
            if (wok && written == sizeof(bom)) {
                wok = WriteFile(h, report.data(),
                                (DWORD)(report.size() * sizeof(wchar_t)),
                                &w2, nullptr);
                written += w2;
            }
            result->ok = (wok != FALSE && written == want);
            CloseHandle(h);
        }
    }
    MiniEQ_AppLogCat(L"DIAG", L"forensics %s: %s",
                     (result && result->ok) ? L"written" : L"FAILED",
                     (result && result->ok) ? result->path.c_str()
                                            : L"(no path)");
    s_forensicsBusy.store(false, std::memory_order_release);
    // The UI thread takes ownership; if the window died mid-run the message
    // simply goes nowhere and the unique_ptr below cleans up. (PostMessage
    // on a stale HWND fails silently -- no crash.)
    if (result) {
        PostMessageW(hwnd, WM_DC_FORENSICSDONE, 0,
                     reinterpret_cast<LPARAM>(result.release()));
    } else {
        PostMessageW(hwnd, WM_DC_FORENSICSDONE, 1, 0);
    }
    return 0;
}

static void DcRunForensics(HWND hwnd) {
    // Atomic check-and-set: the window may have been closed and reopened
    // while a worker runs, so a second button (or a second click) must not
    // start a second collection.
    if (s_forensicsBusy.exchange(true, std::memory_order_acq_rel)) {
        return; // one collection at a time
    }
    // Capture the diagnosis inputs on the UI thread so the worker never
    // races the live snapshot.
    std::unique_ptr<ForensicsInput> in(new (std::nothrow) ForensicsInput());
    if (!in) {
        s_forensicsBusy.store(false, std::memory_order_release);
        return;
    }
    in->snap = s_lastSnap;
    in->verdict = s_lastVerdict;
    in->hwnd = hwnd;
    EnableWindow(GetDlgItem(hwnd, IDC_DC_FORENSICS), FALSE); // busy feedback
    HANDLE h = CreateThread(nullptr, 0, ForensicsThread, in.get(), 0, nullptr);
    if (h != nullptr) {
        in.release(); // the thread owns it now
        CloseHandle(h);
    } else {
        EnableWindow(GetDlgItem(hwnd, IDC_DC_FORENSICS), TRUE);
        s_forensicsBusy.store(false, std::memory_order_release);
        MessageBoxW(hwnd, L"Could not start the forensics collection.",
                    L"MiniEQ diagnostics", MB_OK | MB_ICONWARNING);
    }
}

static void DcOnCreate(HWND hwnd) {
    s_font = DcMakeFont(false);
    s_fontBold = DcMakeFont(true);
    for (int i = 0; i < 4; ++i) {
        s_verdictBrush[i] = CreateSolidBrush(kVerdictBg[i]);
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    auto makeStatic = [&](int id, int x, int y, int w, int h, bool bold,
                          DWORD style) -> HWND {
        HWND ctl = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | style,
                                 x, y, w, h, hwnd, (HMENU)(INT_PTR)id,
                                 s_hInst, nullptr);
        SendMessageW(ctl, WM_SETFONT, (WPARAM)(bold ? s_fontBold : s_font), TRUE);
        return ctl;
    };
    auto makeButton = [&](int id, const wchar_t* text, int x, int y, int w) -> HWND {
        HWND ctl = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 x, y, w, 28, hwnd, (HMENU)(INT_PTR)id,
                                 s_hInst, nullptr);
        SendMessageW(ctl, WM_SETFONT, (WPARAM)s_font, TRUE);
        return ctl;
    };
    auto makeGroup = [&](const wchar_t* text, int x, int y, int w, int h) -> HWND {
        HWND ctl = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                 x, y, w, h, hwnd, nullptr, s_hInst, nullptr);
        SendMessageW(ctl, WM_SETFONT, (WPARAM)s_font, TRUE);
        return ctl;
    };

    s_hVerdict = makeStatic(IDC_DC_VERDICT, 12, 10, 736, 102, true, SS_LEFT);

    s_hGrpEngine = makeGroup(L"Audio engine", 12, 120, 288, 132);
    s_hEngine = makeStatic(IDC_DC_ENGINE, 22, 142, 268, 102, false, SS_LEFT);
    s_hGrpReg = makeGroup(L"Registration", 308, 120, 440, 132);
    s_hReg = makeStatic(IDC_DC_REG, 318, 142, 420, 102, false, SS_LEFT);

    s_hGrpSessions = makeGroup(L"Apps playing on this device", 12, 260, 736, 168);
    s_hSessions = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                  22, 282, 716, 138, hwnd,
                                  (HMENU)(INT_PTR)IDC_DC_SESSIONS, s_hInst, nullptr);
    SendMessageW(s_hSessions, WM_SETFONT, (WPARAM)s_font, TRUE);
    ListView_SetExtendedListViewStyle(s_hSessions, LVS_EX_FULLROWSELECT);
    const struct { const wchar_t* text; int cx; } cols[] = {
        { L"Application", 200 }, { L"PID", 64 }, { L"State", 72 }, { L"Level", 220 },
    };
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(cols[i].text);
        col.cx = cols[i].cx;
        // NOTE: ListView_InsertColumnW is unavailable (see above); direct.
        (void)SendMessageW(s_hSessions, LVM_INSERTCOLUMNW, (WPARAM)i,
                           (LPARAM)&col);
    }

    s_hGrpLog = makeGroup(L"Recent log (categorized)", 12, 436, 736, 168);
    s_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                             WS_VSCROLL | ES_AUTOVSCROLL,
                             22, 458, 716, 138, hwnd,
                             (HMENU)(INT_PTR)IDC_DC_LOG, s_hInst, nullptr);
    SendMessageW(s_hLog, WM_SETFONT, (WPARAM)s_font, TRUE);

    makeButton(IDC_DC_RUN, L"Run diagnosis", 12, 616, 120);
    makeButton(IDC_DC_COPY, L"Copy report", 140, 616, 110);
    makeButton(IDC_DC_COPYLOG, L"Copy log", 258, 616, 100);
    makeButton(IDC_DC_FULLLOG, L"View full log", 366, 616, 110);
    makeButton(IDC_DC_FORENSICS, L"Crash forensics", 484, 616, 140);
    makeButton(IDC_DC_CLOSE, L"Close", 632, 616, 104);

    SetTimer(hwnd, 1, 1000, nullptr);
    MiniEQ_DiagAsyncStart(&s_diagAsync, hwnd, WM_DC_DIAGDONE, s_endpoint);
    RequestDiagRefresh();
}

static LRESULT DcOnCtlColorStatic(HDC hdc, HWND hctl) {
    if (hctl == s_hVerdict) {
        const int idx = (int)s_verdictSeverity;
        SetBkColor(hdc, kVerdictBg[idx]);
        SetTextColor(hdc, kVerdictFg[idx]);
        return (LRESULT)s_verdictBrush[idx];
    }
    return 0;
}

static LRESULT CALLBACK DcWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        s_hDlg = hwnd;
        DcOnCreate(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            RequestDiagRefresh(); // worker probes; UI never blocks
            UpdateLogTail();      // cheap: early-outs when size unchanged
        }
        return 0;
    case WM_DC_DIAGDONE: {
        DiagBundle b;
        if (MiniEQ_DiagAsyncTake(&s_diagAsync, b)) {
            const bool forceLog = s_forceLogNext;
            s_forceLogNext = false;
            RenderDiagSnapshot(b, forceLog);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_DC_RUN:
            s_forceLogNext = true; // re-log the baseline transition
            RequestDiagRefresh();
            return 0;
        case IDC_DC_COPY:
            DcCopyReport(hwnd);
            return 0;
        case IDC_DC_COPYLOG:
            DcCopyLog(hwnd);
            return 0;
        case IDC_DC_FULLLOG:
            MiniEQ_ShowLogViewer(s_hInst, hwnd);
            return 0;
        case IDC_DC_FORENSICS:
            DcRunForensics(hwnd);
            return 0;
        case IDC_DC_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DC_FORENSICSDONE: {
        // Ownership of the heap result arrives in LPARAM (wp==1: worker
        // couldn't even allocate the result -- treat as failure).
        std::unique_ptr<ForensicsResult> result(
            wp == 0 ? reinterpret_cast<ForensicsResult*>(lp) : nullptr);
        EnableWindow(GetDlgItem(hwnd, IDC_DC_FORENSICS), TRUE);
        if (result && result->ok && !result->path.empty()) {
            MiniEQ_AppLogCat(L"DIAG", L"forensics report ready: %s",
                             result->path.c_str());
            MessageBoxW(hwnd,
                (L"The forensics report was saved to your Desktop:\r\n\r\n" +
                 result->path).c_str(),
                L"MiniEQ crash forensics", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hwnd,
                L"Could not write the forensics report to your Desktop.",
                L"MiniEQ crash forensics", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        const LRESULT r = DcOnCtlColorStatic((HDC)wp, (HWND)lp);
        if (r != 0) {
            return r;
        }
        break;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        DeleteObject(s_font);
        DeleteObject(s_fontBold);
        for (int i = 0; i < 4; ++i) {
            DeleteObject(s_verdictBrush[i]);
        }
        s_hDlg = nullptr;
        s_hVerdict = nullptr;
        s_hEngine = nullptr;
        s_hReg = nullptr;
        s_hSessions = nullptr;
        s_hLog = nullptr;
        s_hGrpEngine = nullptr;
        s_hGrpReg = nullptr;
        s_hGrpSessions = nullptr;
        s_hGrpLog = nullptr;
        s_verdictExtra = 0;
        s_font = nullptr;
        s_fontBold = nullptr;
        s_haveLogged = false;
        s_loggedVerdict.clear();
        s_loggedDll = -2;
        s_loggedEnh = DiagEnhancements::Unknown;
        s_loggedHb = false;
        s_loggedSessSig.clear();
        s_shownVerdictTitle.clear();
        s_lastLogSize = 0;
        s_lastSessListSig.clear();
        s_forceLogNext = false;
        MiniEQ_DiagAsyncStop(&s_diagAsync);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void MiniEQ_ShowDiagCenter(HINSTANCE hInst, HWND hParent) {
    if (s_hDlg != nullptr) {
        ShowWindow(s_hDlg, SW_SHOW);
        SetForegroundWindow(s_hDlg);
        RequestDiagRefresh();
        return;
    }
    s_hInst = hInst;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DcWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kDcClass;
    RegisterClassExW(&wc);

    CreateWindowExW(0, kDcClass, L"MiniEQ Diagnostics Center",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                    CW_USEDEFAULT, CW_USEDEFAULT, 760, 702,
                    hParent, nullptr, hInst, nullptr);
    if (s_hDlg != nullptr) {
        ShowWindow(s_hDlg, SW_SHOW);
    }
}

void MiniEQ_DiagCenterSetDevice(const std::wstring& endpointId) {
    if (s_endpoint != endpointId) {
        s_endpoint = endpointId;
        s_haveLogged = false; // re-log the baseline for the new device
        MiniEQ_DiagAsyncSetEndpoint(&s_diagAsync, endpointId);
        if (s_hDlg != nullptr) {
            RequestDiagRefresh();
        }
    }
}
