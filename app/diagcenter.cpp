// diagcenter.cpp -- MiniEQ Diagnostics Center, part 1: the diagnosis engine.
//
// Every probe is best-effort: a failing probe degrades to "unknown" instead
// of breaking the snapshot, and the verdict only claims what the evidence
// supports.

#include "diagcenter.h"

#include "diag.h"
#include "settings_link.h"
#include "../apo/registration.h"
#include "../shared/settings_channel.h"

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <propkey.h> // DEFINE_PROPERTYKEY, needed by functiondiscoverykeys_devpkey.h
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <tlhelp32.h>
#include <strsafe.h>

#include <string.h>
#include <wchar.h>

// PKEY_AudioEndpoint_Disable_SysFx -- the "Audio enhancements" switch.
// {1da5d803-dab9-4c87-aca4-7a043e02e5f0},5 (VT_BOOL; TRUE = enhancements OFF,
// i.e. the engine skips the whole SysFx chain, MiniEQ included). Defined
// locally rather than via the SDK header so the build doesn't depend on SDK
// header version skew. If a future Windows changes this key, the read below
// simply fails and the row reports "unknown" -- never a wrong value.
static const PROPERTYKEY kPkeyDisableSysFx = {
    { 0x1da5d803, 0xdab9, 0x4c87, { 0xac, 0xa4, 0x7a, 0x04, 0x3e, 0x02, 0xe5, 0xf0 } }, 5
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

static std::wstring ReadApoDllPath() {
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
            if (SUCCEEDED(pProps->GetValue(kPkeyDisableSysFx, &pv)) &&
                pv.vt == VT_BOOL) {
                d.enhancements = pv.boolVal ? DiagEnhancements::Off
                                            : DiagEnhancements::On;
            }
            PropVariantClear(&pv);
            pProps->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
    return d;
}

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
                            BOOL sys = FALSE;
                            if (SUCCEEDED(pCtl2->IsSystemSoundsSession(&sys)) && sys) {
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
    s.dllPath = ReadApoDllPath();
    if (!s.dllPath.empty()) {
        s.dllExists = (GetFileAttributesW(s.dllPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    }
    DeviceProps dp = ReadDeviceProps(endpointId);
    s.deviceName = dp.friendlyName;
    s.enhancements = dp.enhancements;

    // Engine layer.
    s.audiodgPid = FindAudiodgPid();
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
    return s;
}

// ---------------------------------------------------------------------------
// Verdict
// ---------------------------------------------------------------------------

DiagVerdict MiniEQ_MakeVerdict(const DiagSnapshot& snap) {
    DiagVerdict v;
    if (snap.endpointId.empty()) {
        v.severity = DiagSeverity::Neutral;
        v.title = L"No device selected.";
        v.detail = L"Pick an output device in the main window first.";
        return v;
    }
    if (!snap.attached) {
        v.severity = DiagSeverity::Warn;
        v.title = L"MiniEQ isn't attached to this device.";
        v.detail = L"The SFX slot of this endpoint doesn't point at MiniEQ_APO, "
                   L"so Windows never loads our equalizer for it.";
        v.nextStep = L"In the main window, click \"Attach to this device\" "
                     L"(one admin approval).";
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
    if (snap.anySessionActive) {
        v.severity = DiagSeverity::Bad;
        v.title = L"Audio is playing, but it's bypassing MiniEQ.";
        if (snap.dllLoaded == 0) {
            v.detail = L"The audio engine (audiodg.exe) never loaded MiniEQ_APO.dll. "
                       L"Registration is correct \u2014 Windows simply didn't "
                       L"instantiate our APO for this stream.";
        } else if (snap.dllLoaded == 1) {
            v.detail = L"Our DLL is inside the audio engine, but no processing "
                       L"calls are arriving \u2014 the stream isn't reaching it.";
        } else {
            v.detail = L"Audio is flowing on the endpoint, but no processing "
                       L"heartbeat is coming back from our APO.";
        }
        v.nextStep = L"Use \"Restart audio service\" in the main window, then replay. "
                     L"If it stays red, copy this report and send it over.";
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

    r += L"\r\n\r\nREGISTRATION\r\n";
    r += L"SFX slot points at MiniEQ: ";
    r += snap.attached ? L"yes" : L"no";
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
// The Diagnostics Center window (modeless)
// ---------------------------------------------------------------------------

#include <commctrl.h>
#include <vector>

enum {
    IDC_DC_VERDICT = 301,
    IDC_DC_ENGINE,
    IDC_DC_REG,
    IDC_DC_SESSIONS,
    IDC_DC_LOG,
    IDC_DC_RUN,
    IDC_DC_COPY,
    IDC_DC_FULLLOG,
    IDC_DC_CLOSE,
};

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
static HFONT       s_font = nullptr;
static HFONT       s_fontBold = nullptr;
static HBRUSH      s_verdictBrush[4] = {};
static DiagSeverity s_verdictSeverity = DiagSeverity::Neutral;

static std::wstring s_endpoint;
static DiagSnapshot s_lastSnap;
static DiagVerdict  s_lastVerdict;

// Transition-logging state: only log when something actually changed.
static bool             s_haveLogged = false;
static std::wstring     s_loggedVerdict;
static int              s_loggedDll = -2;
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

static void UpdateSessionList(const DiagSnapshot& snap) {
    ListView_DeleteAllItems(s_hSessions);
    int row = 0;
    for (const DiagSessionInfo& si : snap.sessions) {
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = const_cast<LPWSTR>(si.exe.c_str());
        ListView_InsertItem(s_hSessions, &it);

        wchar_t pid[32] = {};
        StringCchPrintfW(pid, ARRAYSIZE(pid), L"%lu", si.pid);
        ListView_SetItemText(s_hSessions, row, 1, pid);
        ListView_SetItemText(s_hSessions, row, 2,
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
        ListView_SetItemText(s_hSessions, row, 3, const_cast<LPWSTR>(level.c_str()));
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

static void RefreshDiagCenter(bool forceLog) {
    if (s_hDlg == nullptr) {
        return;
    }
    const DiagSnapshot snap = MiniEQ_RunDiagnosis(s_endpoint);
    const DiagVerdict v = MiniEQ_MakeVerdict(snap);

    std::wstring vt = v.title + L"\r\n\r\n" + v.detail;
    if (!v.nextStep.empty()) {
        vt += L"\r\n\r\nNext step: " + v.nextStep;
    }
    SetTextIfChanged(s_hVerdict, vt);
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

    std::wstring reg = L"SFX slot \u2192 MiniEQ: ";
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
    UpdateLogTail();

    s_lastSnap = snap;
    s_lastVerdict = v;
}

static void DcCopyReport(HWND hwnd) {
    const std::wstring report = MiniEQ_FormatReport(s_lastSnap, s_lastVerdict);
    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        const size_t bytes = (report.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h != nullptr) {
            void* p = GlobalLock(h);
            if (p != nullptr) {
                memcpy(p, report.c_str(), bytes);
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
            } else {
                GlobalFree(h);
            }
        }
        CloseClipboard();
    }
    MiniEQ_AppLogCat(L"DIAG", L"report copied to clipboard");
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
    auto makeGroup = [&](const wchar_t* text, int x, int y, int w, int h) {
        HWND ctl = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                 x, y, w, h, hwnd, nullptr, s_hInst, nullptr);
        SendMessageW(ctl, WM_SETFONT, (WPARAM)s_font, TRUE);
    };

    s_hVerdict = makeStatic(IDC_DC_VERDICT, 12, 10, 596, 102, true, SS_LEFT);

    makeGroup(L"Audio engine", 12, 120, 288, 110);
    s_hEngine = makeStatic(IDC_DC_ENGINE, 22, 142, 268, 80, false, SS_LEFT);
    makeGroup(L"Registration", 308, 120, 300, 110);
    s_hReg = makeStatic(IDC_DC_REG, 318, 142, 280, 80, false, SS_LEFT);

    makeGroup(L"Apps playing on this device", 12, 238, 596, 168);
    s_hSessions = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                  22, 260, 576, 138, hwnd,
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
        ListView_InsertColumn(s_hSessions, i, &col);
    }

    makeGroup(L"Recent log (categorized)", 12, 414, 596, 168);
    s_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                             WS_VSCROLL | ES_AUTOVSCROLL,
                             22, 436, 576, 138, hwnd,
                             (HMENU)(INT_PTR)IDC_DC_LOG, s_hInst, nullptr);
    SendMessageW(s_hLog, WM_SETFONT, (WPARAM)s_font, TRUE);

    makeButton(IDC_DC_RUN, L"Run diagnosis", 12, 594, 140);
    makeButton(IDC_DC_COPY, L"Copy report", 160, 594, 120);
    makeButton(IDC_DC_FULLLOG, L"View full log", 288, 594, 120);
    makeButton(IDC_DC_CLOSE, L"Close", 488, 594, 120);

    SetTimer(hwnd, 1, 1000, nullptr);
    RefreshDiagCenter(false);
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
            RefreshDiagCenter(false);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_DC_RUN:
            RefreshDiagCenter(true);
            return 0;
        case IDC_DC_COPY:
            DcCopyReport(hwnd);
            return 0;
        case IDC_DC_FULLLOG:
            MiniEQ_ShowLogViewer(s_hInst, hwnd);
            return 0;
        case IDC_DC_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
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
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void MiniEQ_ShowDiagCenter(HINSTANCE hInst, HWND hParent) {
    if (s_hDlg != nullptr) {
        ShowWindow(s_hDlg, SW_SHOW);
        SetForegroundWindow(s_hDlg);
        RefreshDiagCenter(false);
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
                    CW_USEDEFAULT, CW_USEDEFAULT, 620, 680,
                    hParent, nullptr, hInst, nullptr);
    if (s_hDlg != nullptr) {
        ShowWindow(s_hDlg, SW_SHOW);
    }
}

void MiniEQ_DiagCenterSetDevice(const std::wstring& endpointId) {
    if (s_endpoint != endpointId) {
        s_endpoint = endpointId;
        s_haveLogged = false; // re-log the baseline for the new device
        if (s_hDlg != nullptr) {
            RefreshDiagCenter(false);
        }
    }
}
