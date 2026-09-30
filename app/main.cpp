// main.cpp -- MiniEQ UI: a tiny native Win32 window.
//
// One window: a prominent current-device header, device picker, 5- or 10-band
// EQ sliders (settings toggle reshapes the UI live), master gain, bypass,
// presets, and a one-click (elevated) "attach to this device" action. On open it auto-selects the system default output (aux,
// USB-C or Bluetooth -- whatever you're listening on) and re-lists endpoints
// live when devices are plugged/unplugged. No frameworks, no runtime beyond
// the Windows SDK: the whole app is well under a megabyte and a few MB of RAM.
//
// Usage: MiniEQ.exe [--attach <endpoint-id> | --detach <endpoint-id>]
// The --attach/--detach forms are used for the elevated self-relaunch and
// exit after doing the registry work.

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>

#include <string>
#include <vector>

#include "audio_devices.h"
#include "settings_link.h"
#include "diag.h"
#include "diagcenter.h"
#include "../apo/registration.h"

//------------------------------------------------------------------------------
// Control IDs & layout
//------------------------------------------------------------------------------

enum {
    IDC_DEVICENAME   = 100,
    IDC_DEVICE_COMBO = 101,
    IDC_REFRESH      = 102,
    IDC_ATTACH       = 103,
    IDC_STATUS       = 104,
    IDC_BAND0        = 110, IDC_BAND1, IDC_BAND2, IDC_BAND3, IDC_BAND4,
    IDC_BANDVAL0     = 120, IDC_BANDVAL1, IDC_BANDVAL2, IDC_BANDVAL3, IDC_BANDVAL4,
    IDC_MASTER       = 130,
    IDC_MASTERVAL    = 131,
    IDC_BYPASS       = 140,
    IDC_PRESET_FLAT  = 150, IDC_PRESET_BASS, IDC_PRESET_VOCAL, IDC_PRESET_BRIGHT,
    IDC_BANDS5       = 160, IDC_BANDS10,
    IDC_VIRTUALIZATION = 170,
    IDC_DIAG_PILL    = 180,
    IDC_DIAG_LOG     = 181,
    IDC_DIAG_RESTART = 182,
    IDC_DIAG_CENTER  = 183,
};

#define IDT_DIAG 1 // 500 ms EQ-path status poll


static const wchar_t* kBandNames5[MINIEQ_NUM_BANDS] = {
    L"60", L"230", L"910", L"3.6k", L"14k"
};
static const wchar_t* kBandNames10[MINIEQ_MAX_BANDS] = {
    L"31", L"62", L"125", L"250", L"500",
    L"1k", L"2k", L"4k", L"8k", L"16k"
};
static const float kPresets5[4][MINIEQ_NUM_BANDS] = {
    { 0, 0, 0, 0, 0 },   // Flat
    { 6, 4, 1, 0, 0 },   // Bass
    { -2, -1, 3, 4, 2 }, // Vocal
    { 0, 0, -1, 3, 5 },  // Bright
};
static const float kPresets10[4][MINIEQ_MAX_BANDS] = {
    { 0, 0, 0, 0, 0,  0, 0, 0, 0, 0 }, // Flat
    { 6, 5, 4, 2, 1,  0, 0, 0, 0, 0 }, // Bass
    { -1, -1, 0, 1, 2,  3, 4, 3, 1, 0 }, // Vocal
    { -1, 0, 0, 0, 0,  1, 2, 3, 4, 5 }, // Bright
};

//------------------------------------------------------------------------------
// State
//------------------------------------------------------------------------------

static HINSTANCE            g_hInst;
static HWND                 g_hwnd;
static HWND                 g_deviceName;
static HWND                 g_combo, g_refresh, g_attach, g_status;
static HWND                 g_band[MINIEQ_MAX_BANDS], g_bandVal[MINIEQ_MAX_BANDS];
static HWND                 g_bandName[MINIEQ_MAX_BANDS];
static HWND                 g_master, g_masterVal, g_bypass;
static HWND                 g_bands5, g_bands10, g_virtCheck;
static int                  g_numBandsShown = 0; // band sliders currently built
static std::vector<AudioEndpoint> g_devices;
static std::wstring         g_endpointId;
static SettingsLink         g_link;
static bool                 g_attached = false;
static StatusLink           g_statusLink;    // APO heartbeat (APO -> UI)
static HWND                 g_pill, g_btnLog, g_btnRestart, g_btnDiagCenter;
static int                  g_diagState = -1; // -1 unset,0 idle,1 live,2 wait,3 err
static int64_t              g_lastCalls = 0;
static ULONGLONG            g_lastTick = 0;
static HBRUSH               g_diagBrush[4] = {};

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

static void FormatDb(wchar_t* out, size_t cch, float db, bool compact) {
    wchar_t sign = db < 0 ? L'-' : L'+';
    if (compact) {
        // 10-band mode: narrow columns, so "+3.5" instead of "+3.5 dB".
        StringCchPrintfW(out, cch, L"%c%.1f", sign, db < 0 ? -db : db);
    } else {
        StringCchPrintfW(out, cch, L"%c%.1f dB", sign, db < 0 ? -db : db);
    }
}

static void PushAndSave() {
    g_link.Push();
    if (!g_endpointId.empty()) {
        MiniEQ_SaveDeviceSettings(g_endpointId, g_link.Current());
    }
}

static void SyncControlsFromStaging() {
    const EqSettings& s = g_link.Staging();
    const bool compact = g_numBandsShown > MINIEQ_NUM_BANDS;
    wchar_t buf[32];
    for (int i = 0; i < g_numBandsShown; ++i) {
        SendMessageW(g_band[i], TBM_SETPOS, TRUE, (LPARAM)(int)(s.bandGainDb[i] * 10.0f));
        FormatDb(buf, ARRAYSIZE(buf), s.bandGainDb[i], compact);
        SetWindowTextW(g_bandVal[i], buf);
    }
    SendMessageW(g_master, TBM_SETPOS, TRUE, (LPARAM)(int)(s.masterGainDb * 10.0f));
    FormatDb(buf, ARRAYSIZE(buf), s.masterGainDb, false);
    SetWindowTextW(g_masterVal, buf);
    Button_SetCheck(g_bypass, s.bypass ? BST_CHECKED : BST_UNCHECKED);
}

// (Re)build the band slider columns for `numBands` (5 or 10). The settings
// toggle calls this, so the UI reshapes itself live.
static void DestroyBandControls() {
    for (int i = 0; i < MINIEQ_MAX_BANDS; ++i) {
        if (g_band[i] != nullptr)     { DestroyWindow(g_band[i]);     g_band[i] = nullptr; }
        if (g_bandVal[i] != nullptr)  { DestroyWindow(g_bandVal[i]);  g_bandVal[i] = nullptr; }
        if (g_bandName[i] != nullptr) { DestroyWindow(g_bandName[i]); g_bandName[i] = nullptr; }
    }
    g_numBandsShown = 0;
}

static void BuildBandControls(int numBands) {
    DestroyBandControls();
    if (numBands != MINIEQ_MAX_BANDS) {
        numBands = MINIEQ_NUM_BANDS;
    }
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    const int spacing = (numBands <= MINIEQ_NUM_BANDS) ? 76 : 38;
    const int sliderW = (numBands <= MINIEQ_NUM_BANDS) ? 40 : 28;
    const int x0 = 14;
    const wchar_t* const* names =
        (numBands <= MINIEQ_NUM_BANDS) ? kBandNames5 : kBandNames10;

    for (int i = 0; i < numBands; ++i) {
        const int x = x0 + i * spacing;
        g_band[i] = CreateWindowW(TRACKBAR_CLASSW, nullptr,
                                  WS_CHILD | WS_VISIBLE | TBS_VERT | TBS_AUTOTICKS,
                                  x + (spacing - sliderW) / 2, 168, sliderW, 170,
                                  g_hwnd, (HMENU)(IDC_BAND0 + i),
                                  g_hInst, nullptr);
        SendMessageW(g_band[i], TBM_SETRANGE, TRUE, MAKELONG(-120, 120));
        SendMessageW(g_band[i], TBM_SETPAGESIZE, 0, 20);
        SendMessageW(g_band[i], TBM_SETTICFREQ, 60, 0);

        g_bandName[i] = CreateWindowW(L"STATIC", names[i],
                                      WS_CHILD | WS_VISIBLE | SS_CENTER,
                                      x, 342, spacing, 18, g_hwnd, nullptr,
                                      g_hInst, nullptr);
        SendMessageW(g_bandName[i], WM_SETFONT, (WPARAM)font, TRUE);
        g_bandVal[i] = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
                                     x, 360, spacing, 18, g_hwnd,
                                     (HMENU)(INT_PTR)(IDC_BANDVAL0 + i),
                                     g_hInst, nullptr);
        SendMessageW(g_bandVal[i], WM_SETFONT, (WPARAM)font, TRUE);
    }
    g_numBandsShown = numBands;
}

// Reflect the staged settings (band count + values) in the whole UI.
static void ApplyStagingToUI() {
    const int n = (g_link.Staging().numBands == MINIEQ_MAX_BANDS)
                  ? MINIEQ_MAX_BANDS : MINIEQ_NUM_BANDS;
    Button_SetCheck(g_bands5, n == MINIEQ_NUM_BANDS ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_bands10, n == MINIEQ_MAX_BANDS ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_virtCheck, g_link.Staging().virtualization ? BST_CHECKED
                                                                 : BST_UNCHECKED);
    if (g_numBandsShown != n) {
        BuildBandControls(n);
    }
    SyncControlsFromStaging();
}

static void SetBandCount(int n) {
    if (n != MINIEQ_NUM_BANDS && n != MINIEQ_MAX_BANDS) {
        return;
    }
    g_link.Staging().numBands = n;
    PushAndSave(); // live to the APO + remembered per device
    ApplyStagingToUI();
}

static void UpdateAttachStatus() {
    bool attached = false;
    if (!g_endpointId.empty()) {
        MiniEQ_IsAttachedToEndpoint(g_endpointId.c_str(), &attached);
    }
    g_attached = attached;
    SetWindowTextW(g_attach, attached ? L"Detach from this device" : L"Attach to this device");
    // Honest wording: the registry link alone doesn't prove the EQ is live.
    // The diagnostics pill below shows the measured path state.
    SetWindowTextW(g_status, attached ? L"Attached: linked to this device."
                                      : L"Not attached: attach once (admin).");
}

// Path states for the diagnostics pill.
enum { DIAG_IDLE = 0, DIAG_LIVE = 1, DIAG_WAITING = 2, DIAG_ERROR = 3 };

// The honest liveness check: the APO's worker thread publishes a heartbeat
// (APOProcess call count) into the status mapping. A heartbeat that keeps
// advancing means audio is REALLY being processed by our code -- no registry
// guessing. Combined with the endpoint peak meter we get three real states:
// live / waiting-for-audio / broken-path.
static void UpdateDiagStatus() {
    int state = DIAG_IDLE;
    wchar_t text[160] = {};
    if (g_attached && !g_endpointId.empty()) {
        MiniEQApoStatus st = {};
        const ULONGLONG now = GetTickCount64();
        if (g_statusLink.Read(&st) && st.processCalls > 0) {
            if (st.processCalls != g_lastCalls) {
                g_lastCalls = st.processCalls;
                g_lastTick = now;
            }
        }
        const bool fresh = (g_lastCalls > 0) && (now - g_lastTick < 2000);
        if (fresh) {
            state = DIAG_LIVE;
            StringCchCopyW(text, ARRAYSIZE(text),
                L"\u25CF EQ live \u2014 audio is passing through MiniEQ");
        } else if (MiniEQ_EndpointPeakLevel(g_endpointId) > 0.001f) {
            state = DIAG_ERROR;
            StringCchCopyW(text, ARRAYSIZE(text),
                L"\u25CF Audio is playing but NOT going through MiniEQ");
        } else {
            state = DIAG_WAITING;
            StringCchCopyW(text, ARRAYSIZE(text),
                L"\u25CF Waiting for audio \u2014 play something on this device");
        }
    } else {
        StringCchCopyW(text, ARRAYSIZE(text),
            L"\u25CB Not attached \u2014 attach MiniEQ to this device to begin.");
    }

    if (state != g_diagState) {
        g_diagState = state;
        static const wchar_t* names[] = { L"IDLE", L"LIVE", L"WAITING", L"ERROR" };
        MiniEQ_AppLog(L"path state -> %s", names[state]);
        InvalidateRect(g_pill, nullptr, TRUE);
    }
    wchar_t cur[160] = {};
    GetWindowTextW(g_pill, cur, ARRAYSIZE(cur));
    if (wcscmp(cur, text) != 0) {
        SetWindowTextW(g_pill, text);
    }
    // The fix for the broken path sits one click away, right where the
    // error is shown.
    ShowWindow(g_btnRestart, state == DIAG_ERROR ? SW_SHOW : SW_HIDE);
}

static void RelaunchElevatedRestart() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = L"--restart-audio";
    sei.nShow = SW_NORMAL;
    if (!ShellExecuteExW(&sei)) {
        MessageBoxW(g_hwnd, L"Elevation was cancelled.", L"MiniEQ", MB_ICONINFORMATION);
        return;
    }
    MiniEQ_AppLog(L"UI: audio-service restart requested (elevated helper)");
    // No blocking wait: the 500 ms status timer turns the pill green as soon
    // as the APO heartbeat resumes after the restart.
}

static void SelectDevice(int index) {
    if (index < 0 || index >= (int)g_devices.size()) {
        return;
    }
    g_endpointId = g_devices[(size_t)index].id;
    // The device name is the first thing the user sees: keep it prominent and
    // always in sync with the selection, even if the channel open fails.
    SetWindowTextW(g_deviceName, g_devices[(size_t)index].name.c_str());
    g_link.Close();
    if (!g_link.Open(g_endpointId)) {
        MessageBoxW(g_hwnd, L"Could not open the settings channel.", L"MiniEQ", MB_ICONWARNING);
        return;
    }
    // Per-device memory: restore this device's last EQ if we saved one.
    EqSettings saved;
    if (MiniEQ_LoadDeviceSettings(g_endpointId, &saved)) {
        g_link.Staging() = saved;
        g_link.Push();
    }
    // The heartbeat channel follows the selected device.
    g_statusLink.Close();
    g_statusLink.Open(g_endpointId);
    g_lastCalls = 0;
    g_lastTick = 0;
    // The Diagnostics Center watches the same device.
    MiniEQ_DiagCenterSetDevice(g_endpointId);
    ApplyStagingToUI();
    UpdateAttachStatus();
    UpdateDiagStatus();
}

static void RefreshDeviceList() {
    int keep = (int)SendMessageW(g_combo, CB_GETCURSEL, 0, 0);
    std::wstring keepId = (keep >= 0 && keep < (int)g_devices.size())
                          ? g_devices[(size_t)keep].id : L"";
    if (keepId.empty()) {
        // Fresh open (or the list was empty): pre-select the system default
        // output -- the device the user is actually listening on.
        keepId = MiniEQ_GetDefaultRenderEndpointId();
    }

    g_devices = MiniEQ_ListRenderEndpoints();
    SendMessageW(g_combo, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < g_devices.size(); ++i) {
        SendMessageW(g_combo, CB_ADDSTRING, 0, (LPARAM)g_devices[i].name.c_str());
        if (g_devices[i].id == keepId) {
            sel = (int)i;
        }
    }
    if (sel < 0 && !g_devices.empty()) {
        sel = 0;
    }
    if (sel >= 0) {
        SendMessageW(g_combo, CB_SETCURSEL, (WPARAM)sel, 0);
        SelectDevice(sel);
    } else {
        SetWindowTextW(g_deviceName, L"No output device");
    }
}

static void RelaunchElevatedAttach(bool attach) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::wstring args = attach ? L"--attach \"" : L"--detach \"";
    args += g_endpointId + L"\"";

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_NORMAL;
    if (!ShellExecuteExW(&sei)) {
        MessageBoxW(g_hwnd, L"Elevation was cancelled.", L"MiniEQ", MB_ICONINFORMATION);
        return;
    }
    // Give the elevated helper a moment, then re-read the state.
    Sleep(800);
    UpdateAttachStatus();
}

//------------------------------------------------------------------------------
// Window
//------------------------------------------------------------------------------

static void BuildControls(HWND hwnd) {
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto applyFont = [font](HWND c) { SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE); };

    // Prominent current-device header: the device name is the first thing the
    // user sees when the app opens.
    HFONT nameFont = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_deviceName = CreateWindowW(L"STATIC", L"No output device",
                                 WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                 12, 10, 396, 30, hwnd, (HMENU)IDC_DEVICENAME,
                                 g_hInst, nullptr);
    SendMessageW(g_deviceName, WM_SETFONT, (WPARAM)nameFont, TRUE);

    CreateWindowW(L"STATIC", L"Device:", WS_CHILD | WS_VISIBLE,
                  12, 52, 52, 18, hwnd, nullptr, g_hInst, nullptr);
    g_combo = CreateWindowW(L"COMBOBOX", nullptr,
                            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                            66, 48, 258, 200, hwnd, (HMENU)IDC_DEVICE_COMBO,
                            g_hInst, nullptr);
    applyFont(g_combo);
    g_refresh = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              330, 47, 78, 24, hwnd, (HMENU)IDC_REFRESH, g_hInst, nullptr);
    applyFont(g_refresh);

    g_attach = CreateWindowW(L"BUTTON", L"Attach to this device",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             12, 80, 170, 26, hwnd, (HMENU)IDC_ATTACH, g_hInst, nullptr);
    applyFont(g_attach);
    g_status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                             190, 85, 218, 18, hwnd, (HMENU)IDC_STATUS, g_hInst, nullptr);
    applyFont(g_status);

    // Diagnostics: the honest EQ-path status pill, color-coded by the APO
    // heartbeat (see UpdateDiagStatus) -- never by registry guesses.
    g_pill = CreateWindowW(L"STATIC", L"",
                           WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                           12, 108, 396, 24, hwnd, (HMENU)IDC_DIAG_PILL,
                           g_hInst, nullptr);
    applyFont(g_pill);
    g_btnLog = CreateWindowW(L"BUTTON", L"View live log",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             12, 136, 120, 26, hwnd, (HMENU)IDC_DIAG_LOG,
                             g_hInst, nullptr);
    applyFont(g_btnLog);
    // Hidden until the error state needs it (see UpdateDiagStatus).
    g_btnRestart = CreateWindowW(L"BUTTON", L"Restart audio service",
                                 WS_CHILD | BS_PUSHBUTTON,
                                 140, 136, 170, 26, hwnd,
                                 (HMENU)IDC_DIAG_RESTART, g_hInst, nullptr);
    applyFont(g_btnRestart);
    g_btnDiagCenter = CreateWindowW(L"BUTTON", L"Diagnostics",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    318, 136, 90, 26, hwnd,
                                    (HMENU)IDC_DIAG_CENTER, g_hInst, nullptr);
    applyFont(g_btnDiagCenter);

    // Band sliders are built by BuildBandControls() (5 or 10, per the
    // settings toggle); the initial set is created in WM_CREATE.

    HWND masterLabel = CreateWindowW(L"STATIC", L"Master", WS_CHILD | WS_VISIBLE,
                                     12, 396, 60, 18, hwnd, nullptr, g_hInst, nullptr);
    applyFont(masterLabel);
    g_master = CreateWindowW(TRACKBAR_CLASSW, nullptr,
                             WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                             70, 390, 230, 34, hwnd, (HMENU)IDC_MASTER, g_hInst, nullptr);
    SendMessageW(g_master, TBM_SETRANGE, TRUE, MAKELONG(-120, 120));
    SendMessageW(g_master, TBM_SETPAGESIZE, 0, 20);
    g_masterVal = CreateWindowW(L"STATIC", L"+0.0 dB", WS_CHILD | WS_VISIBLE,
                                308, 396, 70, 18, hwnd, (HMENU)IDC_MASTERVAL,
                                g_hInst, nullptr);
    applyFont(g_masterVal);

    g_bypass = CreateWindowW(L"BUTTON", L"Bypass (EQ off)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             12, 428, 140, 20, hwnd, (HMENU)IDC_BYPASS, g_hInst, nullptr);
    applyFont(g_bypass);

    HWND presetLabel = CreateWindowW(L"STATIC", L"Presets:", WS_CHILD | WS_VISIBLE,
                                     12, 462, 60, 18, hwnd, nullptr, g_hInst, nullptr);
    applyFont(presetLabel);
    const wchar_t* presetNames[4] = { L"Flat", L"Bass", L"Vocal", L"Bright" };
    for (int i = 0; i < 4; ++i) {
        HWND b = CreateWindowW(L"BUTTON", presetNames[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               76 + i * 78, 458, 70, 26, hwnd,
                               (HMENU)(INT_PTR)(IDC_PRESET_FLAT + i), g_hInst, nullptr);
        applyFont(b);
    }

    HWND settingsLabel = CreateWindowW(L"STATIC", L"Settings:", WS_CHILD | WS_VISIBLE,
                                       12, 496, 60, 18, hwnd, nullptr, g_hInst, nullptr);
    applyFont(settingsLabel);
    HWND bandsLabel = CreateWindowW(L"STATIC", L"Bands:", WS_CHILD | WS_VISIBLE,
                                    76, 496, 44, 18, hwnd, nullptr, g_hInst, nullptr);
    applyFont(bandsLabel);
    g_bands5 = CreateWindowW(L"BUTTON", L"5", WS_CHILD | WS_VISIBLE |
                             BS_AUTORADIOBUTTON | WS_GROUP,
                             122, 494, 36, 20, hwnd, (HMENU)IDC_BANDS5,
                             g_hInst, nullptr);
    applyFont(g_bands5);
    g_bands10 = CreateWindowW(L"BUTTON", L"10", WS_CHILD | WS_VISIBLE |
                              BS_AUTORADIOBUTTON,
                              160, 494, 40, 20, hwnd, (HMENU)IDC_BANDS10,
                              g_hInst, nullptr);
    applyFont(g_bands10);
    // Optional headphone virtualization (bs2b-style crossfeed). Costs nothing
    // until turned on: the APO allocates its tiny state lazily.
    g_virtCheck = CreateWindowW(L"BUTTON", L"Virtualization",
                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                210, 494, 150, 20, hwnd,
                                (HMENU)IDC_VIRTUALIZATION, g_hInst, nullptr);
    applyFont(g_virtCheck);

    HWND note = CreateWindowW(L"STATIC",
        L"Attach once per device (asks for admin). Sliders apply live.",
        WS_CHILD | WS_VISIBLE, 12, 522, 396, 30, hwnd, nullptr, g_hInst, nullptr);
    applyFont(note);
}

static void OnSliderChanged(HWND slider) {
    const int pos = (int)SendMessageW(slider, TBM_GETPOS, 0, 0);
    const float db = pos / 10.0f;
    const bool compact = g_numBandsShown > MINIEQ_NUM_BANDS;
    wchar_t buf[32];
    FormatDb(buf, ARRAYSIZE(buf), db, compact);

    for (int i = 0; i < MINIEQ_MAX_BANDS; ++i) {
        if (slider == g_band[i]) {
            g_link.Staging().bandGainDb[i] = db;
            SetWindowTextW(g_bandVal[i], buf);
            PushAndSave();
            return;
        }
    }
    if (slider == g_master) {
        g_link.Staging().masterGainDb = db;
        FormatDb(buf, ARRAYSIZE(buf), db, false);
        SetWindowTextW(g_masterVal, buf);
        PushAndSave();
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        BuildControls(hwnd);
        {
            // Pill background brushes, one per path state (see WM_CTLCOLORSTATIC).
            static const COLORREF bgc[4] = {
                RGB(240, 240, 240), RGB(233, 247, 238),
                RGB(255, 248, 232), RGB(253, 238, 238)
            };
            for (int i = 0; i < 4; ++i) {
                g_diagBrush[i] = CreateSolidBrush(bgc[i]);
            }
        }
        RefreshDeviceList();
        ApplyStagingToUI(); // builds the band sliders if no device was selected
        UpdateDiagStatus();
        SetTimer(hwnd, IDT_DIAG, 500, nullptr);
        return 0;

    case WM_VSCROLL:
    case WM_HSCROLL:
        if (lParam != 0) {
            OnSliderChanged((HWND)lParam);
        }
        return 0;

    case WM_TIMER:
        if (wParam == IDT_DIAG) {
            UpdateDiagStatus();
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        // The diagnostics pill is color-coded by path state.
        if ((HWND)lParam == g_pill && g_diagState >= 0 && g_diagState <= 3) {
            static const COLORREF bg[4] = {
                RGB(240, 240, 240), RGB(233, 247, 238),
                RGB(255, 248, 232), RGB(253, 238, 238)
            };
            static const COLORREF fg[4] = {
                RGB(85, 85, 85), RGB(20, 83, 45),
                RGB(122, 91, 0), RGB(143, 29, 29)
            };
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, bg[g_diagState]);
            SetTextColor(hdc, fg[g_diagState]);
            return (LRESULT)g_diagBrush[g_diagState];
        }
        break;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id == IDC_DEVICE_COMBO && code == CBN_SELCHANGE) {
            SelectDevice((int)SendMessageW(g_combo, CB_GETCURSEL, 0, 0));
        } else if (id == IDC_REFRESH) {
            RefreshDeviceList();
        } else if (id == IDC_ATTACH) {
            if (!g_endpointId.empty()) {
                RelaunchElevatedAttach(!g_attached);
            }
        } else if (id == IDC_DIAG_LOG) {
            MiniEQ_ShowLogViewer(g_hInst, g_hwnd);
        } else if (id == IDC_DIAG_RESTART) {
            RelaunchElevatedRestart();
        } else if (id == IDC_DIAG_CENTER) {
            MiniEQ_DiagCenterSetDevice(g_endpointId);
            MiniEQ_ShowDiagCenter(g_hInst, g_hwnd);
        } else if (id == IDC_BYPASS) {
            g_link.Staging().bypass = (Button_GetCheck(g_bypass) == BST_CHECKED) ? 1 : 0;
            PushAndSave();
        } else if (id == IDC_BANDS5 || id == IDC_BANDS10) {
            SetBandCount(id == IDC_BANDS5 ? MINIEQ_NUM_BANDS : MINIEQ_MAX_BANDS);
        } else if (id == IDC_VIRTUALIZATION) {
            g_link.Staging().virtualization =
                (Button_GetCheck(g_virtCheck) == BST_CHECKED) ? 1 : 0;
            PushAndSave(); // live to the APO + remembered per device
        } else if (id >= IDC_PRESET_FLAT && id <= IDC_PRESET_BRIGHT) {
            const int p = id - IDC_PRESET_FLAT;
            const int n = (g_numBandsShown > 0) ? g_numBandsShown : MINIEQ_NUM_BANDS;
            const float* preset = (n == MINIEQ_MAX_BANDS) ? kPresets10[p] : kPresets5[p];
            for (int i = 0; i < n; ++i) {
                g_link.Staging().bandGainDb[i] = preset[i];
            }
            SyncControlsFromStaging();
            PushAndSave();
        }
        return 0;
    }

    case WM_DEVICECHANGE:
        // Aux / USB-C / Bluetooth (un)plugged while the app is open: re-list
        // endpoints and keep the current selection when it is still present.
        RefreshDeviceList();
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_DIAG);
        for (int i = 0; i < 4; ++i) {
            if (g_diagBrush[i] != nullptr) {
                DeleteObject(g_diagBrush[i]);
                g_diagBrush[i] = nullptr;
            }
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//------------------------------------------------------------------------------
// Entry
//------------------------------------------------------------------------------

static int RunElevatedHelper(LPWSTR* argv, int argc) {
    // argv: [exe, --restart-audio] or [exe, --attach|--detach, <endpoint-id>]
    if (argc >= 2 && _wcsicmp(argv[1], L"--restart-audio") == 0) {
        const bool ok = MiniEQ_RestartAudioService();
        MessageBoxW(nullptr,
                    ok ? L"Windows Audio restarted.\nPlay something on the device: the status pill turns green when audio passes through MiniEQ."
                       : L"Could not restart Windows Audio.\nOpen \"View live log\" for details.",
                    L"MiniEQ", MB_ICONINFORMATION);
        return ok ? 0 : 1;
    }
    if (argc < 3) {
        return 1;
    }
    const bool attach = (_wcsicmp(argv[1], L"--attach") == 0);
    HRESULT hr = attach ? MiniEQ_AttachToEndpoint(argv[2])
                        : MiniEQ_DetachFromEndpoint(argv[2]);
    if (SUCCEEDED(hr)) {
        MessageBoxW(nullptr,
                    attach ? L"MiniEQ is now attached to this device.\nYou may need to restart audio playback."
                           : L"MiniEQ has been detached from this device.",
                    L"MiniEQ", MB_ICONINFORMATION);
        return 0;
    }
    wchar_t msg[384] = {};
    const wchar_t* hint = L"";
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
        hint = L"\nAccess denied: the device's audio settings are locked down.";
    } else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        hint = L"\nThe device's audio settings key was not found.";
    }
    StringCchPrintfW(msg, ARRAYSIZE(msg),
                     L"Operation failed (0x%08X).%s",
                     (unsigned)hr, hint);
    MessageBoxW(nullptr, msg, L"MiniEQ", MB_ICONERROR);
    return 1;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE /*prev*/, LPWSTR cmdLine, int show) {
    g_hInst = hInst;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2 && (argv[1][0] == L'-')) {
        const int rc = RunElevatedHelper(argv, argc);
        LocalFree(argv);
        return rc;
    }
    LocalFree(argv);
    (void)cmdLine;

    // The APO (inside audiodg.exe) appends its trace here; make sure the
    // directory exists before any audio flows.
    MiniEQ_EnsureLogDir();
    MiniEQ_AppLog(L"UI started");

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        return 1;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MiniEQWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"MiniEQWnd", L"MiniEQ",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 420, 600,
                                nullptr, nullptr, hInst, nullptr);
    if (hwnd == nullptr) {
        CoUninitialize();
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    CoUninitialize();
    return 0;
}
