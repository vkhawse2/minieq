// main.cpp -- MiniEQ UI: a tiny native Win32 window.
//
// One window: a prominent current-device header, device picker, 5- or 10-band
// EQ sliders (settings toggle reshapes the UI live), master gain, bypass,
// presets, and a one-click (elevated) "attach to this device" action. On open it auto-selects the system default output (aux,
// USB-C or Bluetooth -- whatever you're listening on) and re-lists endpoints
// live when devices are plugged/unplugged. No frameworks, no runtime beyond
// the Windows SDK: the whole app is well under a megabyte and a few MB of RAM.
//
// Usage: MiniEQ.exe [--attach <endpoint-id> [--force] | --detach <endpoint-id>
//                    | --attach-all | --detach-all]
// The --attach/--detach forms are used for the elevated self-relaunch and
// exit after doing the registry work. The --attach-all/--detach-all forms
// are the MSI's deferred custom actions: silent, no UI at all (the installer
// runs them as SYSTEM with no interactive desktop, where a message box
// would hang the setup), best-effort per endpoint. --attach-all forces the
// slot rewrite + device re-enumeration on every endpoint so a fresh install
// (or an upgrade that replaced the DLL) takes effect immediately instead of
// leaving the engine on its stale chain.

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <mmdeviceapi.h> // RunCircuitBreakerSweep enumerates endpoints directly
#include <shellapi.h>
#include <strsafe.h>

#include <string>
#include <thread>
#include <vector>

#include "audio_devices.h"
#include "settings_link.h"
#include "diag.h"
#include "diagcenter.h"
#include "checklist.h"
#include "engine_reload.h"
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
    IDC_DIAG_CENTER  = 183,
    IDC_DIAG_SOUND   = 184, // "Open Sound settings" (enhancements-off state)
    IDC_DIAG_HINT    = 185, // one-line contextual fix guidance under the pill
    IDC_CHECKLIST    = 186, // "Checklist" button next to Crossfeed
    IDC_POWER        = 187, // global MiniEQ on/off button (pill row)
    IDC_BANNERTEXT   = 188, // auto-attach banner text
    IDC_BANNERBTN    = 189, // auto-attach banner "Attach MiniEQ" button
};

// App version: bump for every handed-over build. Shown in the main window
// title; the MSI filename/version and the CI artifact name are bumped to
// match (installer/MiniEQ.wxs, .github/workflows/build.yml).
#define MINIEQ_APP_VERSION L"0.2.2"

#define IDT_DIAG 1 // 500 ms EQ-path status poll
#define IDT_DEVSETTLE 2 // WM_DEVICECHANGE coalescing: rebuild once the storm ends
#define IDT_DEVGHOST 3  // one-shot: a device still missing after 4 s is really gone
#define IDT_AUTORECOVER 4 // one-shot: next step of the endpoint-reappearance backoff


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
static bool                 g_linkSynced = false; // saved EQ pushed to the live channel
static bool                 g_attached = false;
static StatusLink           g_statusLink;    // APO heartbeat (APO -> UI)
static HWND                 g_pill, g_btnLog, g_btnDiagCenter;
static HWND                 g_btnSound, g_hint; // sound-settings btn + fix hint
static HWND                 g_power;      // global MiniEQ on/off button
static HWND                 g_bannerText, g_bannerBtn; // banner text + action button
static HBRUSH               g_bannerOkBrush = nullptr;   // green tint: reload done
static HBRUSH               g_bannerWarnBrush = nullptr; // amber tint: reload pending/failed
static HWND                 g_preset[4];  // preset buttons (for power dimming)
static HWND                 g_masterLabel, g_presetLabel, g_settingsLabel;
static HWND                 g_bandsLabel, g_note; // fixed labels repositioned by LayoutContent

static void LayoutContent(); // forward: called before its definition below
static bool DoElevatedAttach(bool attach, bool force); // forward: verify escalates
static HWND                 g_btnChecklist; // "Checklist" button
static int                  g_diagState = -1; // -1 unset; see DIAG_* below
static int64_t              g_lastCalls = 0;
static ULONGLONG            g_lastTick = 0;
static ULONGLONG            g_bypassSince = 0; // "playing + stale heartbeat" streak start (verdict debounce)
static bool                 g_breakerLatchSeen = false; // breaker-latch edge detection for arm cancellation
// Automatic endpoint-reappearance recovery (0.2.0): when a device-change
// storm settles and our endpoint is back but the EQ path isn't live, the UI
// runs a bounded backoff of non-elevated path reloads -- 100/250/500 ms,
// 1 s, 2 s -- then stops and waits for the next device-change event.
// Never loops, never escalates to UAC by itself: if the silent reloads
// don't restore the heartbeat, the post-attach verification (armed below)
// takes over with its one warned escalation. Step 0 = inactive.
static int                  g_autoRecStep = 0;
static const int            kAutoRecBackoffMs[5] = { 100, 250, 500, 1000, 2000 };
static HBRUSH               g_diagBrush[8] = {}; // one per DIAG_* state
// Banner: shared slot for the auto-attach offer and the engine-reload
// states. Reload states outrank attach states while they are active.
enum class BannerKind {
    None,
    ReloadPending, // update ready, waiting for audio to stop (amber)
    ReloadFailed,  // flip didn't take: manual step (amber)
    ReloadDone,    // engine reloaded + new build verified (green)
    AttachNote,    // post-attach note (plain)
    AttachOffer,   // new default device, offer one-click attach (plain)
    VerifyNote,    // post-attach heartbeat verification (plain, custom text)
};
static BannerKind           g_bannerKind = BannerKind::None;
// Auto-attach banner: shown when the Windows default render endpoint changed
// to a device MiniEQ isn't attached to. Never auto-detaches old devices.
static std::wstring         g_lastDefaultId; // last seen system default endpoint
static int                  g_contentDy = 0; // banner pushes content down by this
static bool                 g_bannerVisible = false;
// Device-change coalescing: a device restart (our own re-attach, a
// Bluetooth reconnect) fires a burst of WM_DEVICECHANGE. Rebuilding the
// device list + relaying out the window on every one tore the UI, so the
// messages are folded into a single rebuild 750 ms after the last change.
static int                  g_devChangeCoalesced = 0;
static bool                 g_forceReselect = false; // ghost timer: drop stale sel
static bool                 g_inLayout = false; // LayoutContent re-entrancy guard
static bool                 g_bannerNote = false; // post-attach note showing
static ULONGLONG            g_bannerNoteTick = 0;
static constexpr ULONGLONG  kBannerNoteMs = 120000; // 2 min
static std::wstring         g_verifyText; // custom text for BannerKind::VerifyNote
// Post-attach verification: after an attach the engine must actually load
// the APO. We watch for DIAG_LIVE; if it doesn't arrive in time we escalate
// exactly once with a forced re-attach (slot rewrite + device re-enumeration,
// one more UAC prompt). The banner explains each step before it happens, so
// the escalation never surprises.
static ULONGLONG            g_verifyUntil = 0; // 0 = not verifying
static bool                 g_verifyEscalated = false;
static bool                 g_verifyNudgePending = false;
static bool                 g_verifyLoggedStuck = false; // init-without-lock diagnosis logged
static ULONGLONG            g_lastEscalateTick = 0; // rate-limit for the UAC escalation
static constexpr ULONGLONG  kVerifyMs = 20000; // 20 s per verification round
// Engine auto-reload: when the status channel reports a stale APO build, the
// app flips the default format itself (no services, no UAC) and verifies the
// new build. One attempt per (endpoint, stale build); never loops.
#define WM_APP_RELOAD_DONE (WM_APP + 102)
enum class ReloadUiState { None, Working, Pending, DoneNote, FailedNote };
static ReloadUiState        g_reloadUi = ReloadUiState::None;
static std::wstring         g_reloadKey; // endpoint|reported-build: once per detection
static bool                 g_reloadAttempted = false; // auto-arm fired for g_reloadKey
static std::wstring         g_reloadBuild;   // new live build id, for banner/log text
static std::wstring         g_reloadSession; // Deferred: who is playing
static std::wstring         g_reloadDetail;  // Failed/FlipError: why
static ULONGLONG            g_reloadNoteTick = 0;  // DoneNote/FailedNote expiry
static ULONGLONG            g_reloadRetryTick = 0; // Pending: re-check cadence
static bool                 g_reloadDeferredLogged = false;
static bool                 g_reloadWorkerBusy = false;
static constexpr ULONGLONG  kReloadRetryMs = 15000; // Pending: re-check every 15 s
static GlobalStateLink      g_globalLink; // UI side of the global on/off flag
// Audio-enhancements switch, re-read every few seconds (cheap single-key
// property read; never a wrong value -- Unknown when unreadable).
static DiagEnhancements     g_enhState = DiagEnhancements::Unknown;
static ULONGLONG            g_enhCheckTick = 0;
// Graph-rebuild tracking: once the heartbeat has been seen live on this
// device, a later loss is treated as a settings-driven rebuild (amber,
// auto-recovering) for a grace period instead of an instant red error.
static bool                 g_sawLive = false;
static ULONGLONG            g_lastLiveTick = 0;

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

// Global MiniEQ on/off UI: button label + dimming/disabling every EQ
// control (sliders, master, per-device bypass, presets, band-count radios,
// Crossfeed). Called at startup, on toggle, and after any slider
// rebuild (a rebuild re-enables fresh controls).
static void ApplyPowerUI() {
    const bool on = MiniEQ_GlobalEnabledGet();
    if (g_power != nullptr) {
        SetWindowTextW(g_power, on ? L"\u23FB Turn off MiniEQ"
                                   : L"\u23FB Turn on MiniEQ");
    }
    const BOOL en = on ? TRUE : FALSE;
    for (int i = 0; i < MINIEQ_MAX_BANDS; ++i) {
        if (g_band[i] != nullptr) {
            EnableWindow(g_band[i], en);
        }
    }
    if (g_master != nullptr)    EnableWindow(g_master, en);
    if (g_bypass != nullptr)    EnableWindow(g_bypass, en);
    if (g_bands5 != nullptr)    EnableWindow(g_bands5, en);
    if (g_bands10 != nullptr)   EnableWindow(g_bands10, en);
    if (g_virtCheck != nullptr) EnableWindow(g_virtCheck, en);
    for (int i = 0; i < 4; ++i) {
        if (g_preset[i] != nullptr) {
            EnableWindow(g_preset[i], en);
        }
    }
}

// Push the persisted on/off choice into the APO's global channel. The APO
// creates the channel when it locks a stream; until then the open just fails
// and we retry on the 500 ms status timer, so a persisted "off" is picked up
// even if the audio engine restarts after us. If the channel already carries
// our state, this is a no-op read.
static void SyncGlobalEnabled() {
    if (!g_globalLink.IsOpen() && !g_globalLink.Open()) {
        return; // APO hasn't created the channel yet; retry next tick
    }
    const bool want = MiniEQ_GlobalEnabledGet();
    MiniEQGlobalState gs = {};
    if (g_globalLink.Read(&gs) && gs.enabled == (want ? 1 : 0)) {
        return;
    }
    g_globalLink.WriteEnabled(want);
    MiniEQ_AppLog(L"UI: global MiniEQ state re-asserted: %s", want ? L"ON" : L"OFF");
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
        // y positions shift with the auto-attach banner (g_contentDy);
        // LayoutContent() repositions live sliders when it toggles.
        g_band[i] = CreateWindowW(TRACKBAR_CLASSW, nullptr,
                                  WS_CHILD | WS_VISIBLE | TBS_VERT | TBS_AUTOTICKS,
                                  x + (spacing - sliderW) / 2, 200 + g_contentDy, sliderW, 170,
                                  g_hwnd, (HMENU)(IDC_BAND0 + i),
                                  g_hInst, nullptr);
        SendMessageW(g_band[i], TBM_SETRANGE, TRUE, MAKELONG(-120, 120));
        SendMessageW(g_band[i], TBM_SETPAGESIZE, 0, 20);
        SendMessageW(g_band[i], TBM_SETTICFREQ, 60, 0);

        g_bandName[i] = CreateWindowW(L"STATIC", names[i],
                                      WS_CHILD | WS_VISIBLE | SS_CENTER,
                                      x, 374 + g_contentDy, spacing, 18, g_hwnd, nullptr,
                                      g_hInst, nullptr);
        SendMessageW(g_bandName[i], WM_SETFONT, (WPARAM)font, TRUE);
        g_bandVal[i] = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
                                     x, 392 + g_contentDy, spacing, 18, g_hwnd,
                                     (HMENU)(INT_PTR)(IDC_BANDVAL0 + i),
                                     g_hInst, nullptr);
        SendMessageW(g_bandVal[i], WM_SETFONT, (WPARAM)font, TRUE);
    }
    g_numBandsShown = numBands;
    ApplyPowerUI(); // a rebuild re-enables controls; re-apply the off-state dimming
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

// Banner: one shared slot. Engine-reload states outrank the auto-attach
// states while active. Safe to call from the 500 ms timer: note expiry is
// the only timer-driven change.
static void UpdateBanner() {
    // Expire the timed notes.
    const ULONGLONG now = GetTickCount64();
    if (g_bannerNote && now - g_bannerNoteTick >= kBannerNoteMs) {
        g_bannerNote = false;
        g_verifyText.clear(); // custom verify/success text expires with the note
    }
    if ((g_reloadUi == ReloadUiState::DoneNote ||
         g_reloadUi == ReloadUiState::FailedNote) &&
        now - g_reloadNoteTick >= kBannerNoteMs) {
        g_reloadUi = ReloadUiState::None;
    }
    BannerKind kind = BannerKind::None;
    wchar_t wantText[384] = {};
    wchar_t wantBtn[48] = {};
    bool wantBtnVisible = false;
    if (g_reloadUi == ReloadUiState::Pending) {
        kind = BannerKind::ReloadPending;
        StringCchPrintfW(wantText, ARRAYSIZE(wantText),
            L"Update ready \u2014 waiting for audio to stop before reloading "
            L"the engine (%s is playing).",
            g_reloadSession.empty() ? L"an app" : g_reloadSession.c_str());
        StringCchCopyW(wantBtn, ARRAYSIZE(wantBtn), L"Reload now");
        wantBtnVisible = true;
    } else if (g_reloadUi == ReloadUiState::FailedNote) {
        kind = BannerKind::ReloadFailed;
        StringCchPrintfW(wantText, ARRAYSIZE(wantText),
            L"Engine reload didn\u2019t take (%s). Open Sound settings, change "
            L"the Default Format and change it back.",
            g_reloadDetail.empty() ? L"build still stale" : g_reloadDetail.c_str());
        StringCchCopyW(wantBtn, ARRAYSIZE(wantBtn), L"Try again");
        wantBtnVisible = true;
    } else if (g_reloadUi == ReloadUiState::DoneNote) {
        kind = BannerKind::ReloadDone;
        StringCchPrintfW(wantText, ARRAYSIZE(wantText),
            L"Audio engine reloaded automatically \u2014 build %s is live.",
            g_reloadBuild.empty() ? L"?" : g_reloadBuild.c_str());
        StringCchCopyW(wantBtn, ARRAYSIZE(wantBtn), L"View log");
        wantBtnVisible = true;
    } else if (g_reloadUi == ReloadUiState::Working) {
        // The worker is mid-flip: keep the previous banner (if any) rather
        // than flashing. The pill already shows the rebuild state.
        kind = g_bannerKind;
        if (kind != BannerKind::None) {
            GetWindowTextW(g_bannerText, wantText, ARRAYSIZE(wantText));
            GetWindowTextW(g_bannerBtn, wantBtn, ARRAYSIZE(wantBtn));
            wantBtnVisible = IsWindowVisible(g_bannerBtn) == TRUE;
        }
    } else if (g_verifyUntil != 0 || !g_verifyText.empty()) {
        kind = BannerKind::VerifyNote;
        StringCchCopyW(wantText, ARRAYSIZE(wantText),
            g_verifyText.empty() ? L"Attached \u2014 waiting for the audio engine to pick up MiniEQ."
                                 : g_verifyText.c_str());
    } else if (g_bannerNote) {
        kind = BannerKind::AttachNote;
        StringCchCopyW(wantText, ARRAYSIZE(wantText),
            L"Attached \u2014 reloading the audio path to pick up the change.");
    } else if (!g_endpointId.empty() && !g_attached &&
               !g_lastDefaultId.empty() && g_endpointId == g_lastDefaultId) {
        // The selected device IS the current system default and MiniEQ isn't
        // attached to it: this is the "new default device" case.
        kind = BannerKind::AttachOffer;
        wchar_t dev[128] = {};
        GetWindowTextW(g_deviceName, dev, ARRAYSIZE(dev));
        StringCchPrintfW(wantText, ARRAYSIZE(wantText),
            L"New default device detected. Windows switched playback to %s. "
            L"Attach MiniEQ to it? (one-time admin consent, then permanent)",
            dev[0] ? dev : L"this device");
        StringCchCopyW(wantBtn, ARRAYSIZE(wantBtn), L"Attach MiniEQ");
        wantBtnVisible = true;
    }
    g_bannerKind = kind;
    const bool show = (kind != BannerKind::None);
    if (show) {
        // Set text/button only when they changed (this runs on the timer).
        wchar_t cur[384] = {};
        GetWindowTextW(g_bannerText, cur, ARRAYSIZE(cur));
        if (wcscmp(cur, wantText) != 0) {
            SetWindowTextW(g_bannerText, wantText);
        }
        wchar_t curBtn[48] = {};
        GetWindowTextW(g_bannerBtn, curBtn, ARRAYSIZE(curBtn));
        if (wantBtnVisible && wcscmp(curBtn, wantBtn) != 0) {
            SetWindowTextW(g_bannerBtn, wantBtn);
        }
        ShowWindow(g_bannerBtn, wantBtnVisible ? SW_SHOW : SW_HIDE);
        EnableWindow(g_bannerBtn, TRUE);
    }
    if (show == g_bannerVisible) {
        return;
    }
    g_bannerVisible = show;
    ShowWindow(g_bannerText, show ? SW_SHOW : SW_HIDE);
    if (!show) {
        ShowWindow(g_bannerBtn, SW_HIDE);
    }
    g_contentDy = show ? 44 : 0;
    LayoutContent();
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
    UpdateBanner();
}

// Engine auto-reload driver, called from the 500 ms timer via
// UpdateDiagStatus. When the loaded APO build is stale: if audio is playing
// the reload waits (banner offers "Reload now"); when idle it fires on its
// own. One attempt per (endpoint, stale build) -- never a loop.
static void SpawnReloadWorker(bool force);
static void TryOpenChannels(); // defined below; re-opens the APO channels

static void UpdateEngineReload() {
    if (g_endpointId.empty() || !g_attached || g_reloadWorkerBusy) {
        return;
    }
    if (MiniEQ_BreakerLatched()) {
        return; // SAFE/DETACHED: no automatic stale-build reloads either
    }
    // The APO must actually be loaded before a reload means anything.
    TryOpenChannels();
    MiniEQApoStatus st = {};
    const bool live = g_statusLink.IsOpen() && g_statusLink.Read(&st) &&
                      st.structSize >= sizeof(MiniEQApoStatus) && st.version >= 1;
    if (!live) {
        return;
    }
    std::wstring reported;
    const bool stale =
        MiniEQ_EngineBuildFreshness(g_endpointId, &reported) == BuildFreshness::Stale;
    const std::wstring key =
        g_endpointId + L"|" + (stale ? reported : L"fresh");
    if (key != g_reloadKey) {
        // New detection episode (new stale build, or the reload landed).
        g_reloadKey = key;
        g_reloadAttempted = false;
        g_reloadDeferredLogged = false;
        if (g_reloadUi != ReloadUiState::DoneNote &&
            g_reloadUi != ReloadUiState::FailedNote) {
            g_reloadUi = ReloadUiState::None;
        }
    }
    if (!stale) {
        return;
    }
    if (g_reloadUi == ReloadUiState::None && !g_reloadAttempted) {
        // Fire exactly once per detection: after a failure the banner's
        // "Try again" is the only way back in (manual consent), never the
        // timer.
        g_reloadAttempted = true;
        MiniEQ_AppLogCat(L"ENGINE",
            L"stale APO detected -- engine runs build %s, installed %S; auto-reload armed",
            reported.c_str(), MiniEQ_ExpectedBuildId());
        SpawnReloadWorker(false);
    } else if (g_reloadUi == ReloadUiState::Pending &&
               GetTickCount64() - g_reloadRetryTick >= kReloadRetryMs) {
        // Still deferred: re-check whether the coast is clear now.
        g_reloadRetryTick = GetTickCount64();
        SpawnReloadWorker(false);
    }
}

static void SpawnReloadWorker(bool force) {
    if (g_reloadWorkerBusy || g_endpointId.empty() || g_hwnd == nullptr) {
        return;
    }
    // The format flip restarts the stream on purpose -- keep the breaker's
    // passive detector from reading that as a crash loop.
    MiniEQ_BreakerNoteUserAction();
    g_reloadWorkerBusy = true;
    g_reloadUi = ReloadUiState::Working;
    const std::wstring endpoint = g_endpointId;
    const HWND hwnd = g_hwnd;
    std::thread([endpoint, hwnd, force]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        EngineReloadOutcome* out = MiniEQ_RunReloadJob(endpoint, force);
        CoUninitialize();
        PostMessageW(hwnd, WM_APP_RELOAD_DONE, 0, reinterpret_cast<LPARAM>(out));
    }).detach();
    UpdateBanner();
}

// The APO (running in the audio engine, session 0) creates the Global\
// channels when it locks a stream; the UI (user session) can only open
// them. Keep retrying until they appear, then restore this device's saved
// EQ exactly once. Called from the 500 ms status timer and on device
// selection, so no MessageBox nagging when the APO isn't up yet.
static void TryOpenChannels() {
    if (g_endpointId.empty()) {
        return;
    }
    // Upgrade window: the links may sit on legacy (pre-hash) channels
    // because the engine still ran the old APO when they opened. Once the
    // hashed channels appear, the new APO is up -- switch so the heartbeat
    // doesn't freeze on the old APO's abandoned channel. The settings link
    // re-pushes the staged EQ (the new channel holds flat defaults).
    if (g_link.IsOpen() && g_link.UsingLegacyName()) {
        if (g_link.MaybeUpgrade(g_endpointId)) {
            MiniEQ_AppLog(L"settings channel upgraded to hashed name; EQ re-pushed");
        }
    }
    if (g_statusLink.IsOpen() && g_statusLink.UsingLegacyName()) {
        g_statusLink.MaybeUpgrade(g_endpointId);
    }
    if (!g_link.IsOpen() && g_link.Open(g_endpointId)) {
        // The channel just appeared: the APO published flat defaults (or
        // older live values). This device's saved EQ wins -- the INI is the
        // source of truth for what the sliders show, so a reconnect never
        // snaps the user's sliders back to flat.
        EqSettings saved;
        if (MiniEQ_LoadDeviceSettings(g_endpointId, &saved)) {
            g_link.Staging() = saved;
        }
        g_link.Push();
        g_linkSynced = true;
        ApplyStagingToUI();
        MiniEQ_AppLog(L"settings channel open; saved EQ restored");
    }
    if (!g_statusLink.IsOpen()) {
        g_statusLink.Open(g_endpointId);
    }
}

// Path states for the diagnostics pill.
enum {
    DIAG_IDLE = 0,   // not attached
    DIAG_LIVE = 1,   // heartbeat advancing: EQ is really processing
    DIAG_WAITING = 2,// attached, healthy path, no audio right now
    DIAG_ERROR = 3,  // audio playing but bypassing MiniEQ (genuinely broken)
    DIAG_ENHOFF = 4, // Audio enhancements off: Windows skips the whole chain
    DIAG_REBUILD = 5,// heartbeat lost after being live: settings-driven
                     // graph rebuild, recovering on its own
    DIAG_OFF = 6,    // global MiniEQ switch off: user chose unprocessed audio
    DIAG_STALEWAIT = 7, // update installed, engine runs a stale DLL: waiting
                     // for audio to stop before the automatic reload
};

// The honest liveness check: the APO's worker thread publishes a heartbeat
// (APOProcess call count) into the status mapping. A heartbeat that keeps
// advancing means audio is REALLY being processed by our code -- no registry
// guessing. Combined with the endpoint peak meter and the Audio Enhancements
// switch we get seven live states: idle / live / waiting / error /
// enhancements-off / rebuilding / update-pending. A settings change
// (enhancements, spatial sound) tears the graph down and rebuilds it; that
// transition is amber and self-healing -- never a red error, never a service
// restart.
static void UpdateDiagStatus() {
    int state = DIAG_IDLE;
    wchar_t text[160] = {};
    wchar_t hint[256] = {};
    // The global switch outranks every path state: when MiniEQ is off, the
    // APO passes audio through untouched (heartbeat still advances -- that's
    // by design, so the link reads alive), and the pill says so in gray.
    if (!MiniEQ_GlobalEnabledGet()) {
        state = DIAG_OFF;
        StringCchCopyW(text, ARRAYSIZE(text),
            L"\u25CB MiniEQ is off \u2014 audio plays unprocessed");
        StringCchCopyW(hint, ARRAYSIZE(hint),
            L"Click \u201CTurn on MiniEQ\u201D to resume the EQ.");
    } else if (g_attached && !g_endpointId.empty()) {
        // The APO creates the channels when it locks a stream; the UI can
        // only open them, so keep retrying until the APO is up.
        TryOpenChannels();
        const ULONGLONG now = GetTickCount64();
        // An update is installed but the engine still runs the old DLL: the
        // auto-reload is waiting for playback to stop. This outranks the
        // live/waiting states -- it is the most useful thing to say.
        if (g_reloadUi == ReloadUiState::Pending) {
            state = DIAG_STALEWAIT;
            StringCchCopyW(text, ARRAYSIZE(text),
                L"\u25CF Update pending \u2014 reloads when audio stops");
            StringCchCopyW(hint, ARRAYSIZE(hint),
                L"MiniEQ reloads the audio path by itself once playback stops. "
                L"Nothing is restarted; or click \u201CReload now\u201D above.");
        } else {
        // Re-read the enhancements switch every ~3 s: immediately after a
        // settings change this is what tells "off, fix it in Settings" apart
        // from "rebuilding, wait a moment".
        if (g_enhCheckTick == 0 || now - g_enhCheckTick >= 3000) {
            g_enhCheckTick = now;
            g_enhState = MiniEQ_ReadEnhancements(g_endpointId);
        }
        MiniEQApoStatus st = {};
        if (g_statusLink.Read(&st) && st.processCalls > 0) {
            if (st.processCalls != g_lastCalls) {
                g_lastCalls = st.processCalls;
                g_lastTick = now;
            }
        }
        const bool fresh = (g_lastCalls > 0) && (now - g_lastTick < 2000);
        // Verdict debounce: "playing but bypassing" is claimed only after
        // audio has been playing with a stale heartbeat for ~4 s straight.
        // Inside the grace window the state stays a neutral wait, so a
        // graph rebuild between songs never flashes a false failure.
        const bool playing = MiniEQ_EndpointPeakLevel(g_endpointId) > 0.001f;
        // Instantiated but not locked yet (Initialize ran, no processing
        // stream): starting up, never a bypass -- don't accrue the streak.
        const bool startingUp = st.initCalls > 0 && st.processCalls == 0 &&
                                st.locked == 0;
        if (playing && !fresh && !startingUp) {
            if (g_bypassSince == 0) {
                g_bypassSince = now;
            }
        } else {
            g_bypassSince = 0;
        }
        const bool bypassSettled = playing && !fresh && !startingUp &&
                                   g_bypassSince != 0 &&
                                   (now - g_bypassSince >= 4000);
        if (g_enhState == DiagEnhancements::Off) {
            state = DIAG_ENHOFF;
            StringCchCopyW(text, ARRAYSIZE(text),
                L"\u25CF Audio enhancements are Off \u2014 MiniEQ is bypassed");
            wchar_t dev[96] = {};
            GetWindowTextW(g_deviceName, dev, ARRAYSIZE(dev));
            StringCchPrintfW(hint, ARRAYSIZE(hint),
                L"Fix: Settings \u2192 System \u2192 Sound \u2192 %s \u2192 "
                L"Audio enhancements \u2192 \u201CDevice Default Effects\u201D",
                dev[0] ? dev : L"this device");
        } else if (fresh) {
            state = DIAG_LIVE;
            g_sawLive = true;
            g_lastLiveTick = now;
            StringCchCopyW(text, ARRAYSIZE(text),
                L"\u25CF EQ live \u2014 audio is passing through MiniEQ");
        } else if (g_sawLive && (now - g_lastLiveTick < 15000)) {
            // The path was live moments ago and died: Windows is rebuilding
            // the audio graph (e.g. after an enhancements/spatial-sound
            // change). The APO re-creates its channels on lock; our 500 ms
            // retry re-opens them and the heartbeat resumes by itself.
            state = DIAG_REBUILD;
            StringCchCopyW(text, ARRAYSIZE(text),
                L"\u25CF Audio engine is rebuilding \u2014 recovering\u2026");
            StringCchCopyW(hint, ARRAYSIZE(hint),
                L"A settings change rebuilt the audio path; "
                L"this clears on its own, no restart needed.");
        } else if (bypassSettled) {
            state = DIAG_ERROR;
            StringCchCopyW(text, ARRAYSIZE(text),
                L"\u25CF Audio is playing but NOT going through MiniEQ");
            StringCchCopyW(hint, ARRAYSIZE(hint),
                L"Try replaying the audio. If it stays red, open the Audio Path "
                L"Checklist and press Re-attach (Recovery section) \u2014 one click, "
                L"no settings to hunt for.");
        } else {
            state = DIAG_WAITING;
            StringCchCopyW(text, ARRAYSIZE(text),
                L"\u25CF Waiting for audio \u2014 play something on this device");
        }
        } // end: not a pending engine reload
    } else {
        StringCchCopyW(text, ARRAYSIZE(text),
            L"\u25CB Not attached \u2014 attach MiniEQ to this device to begin.");
    }

    if (state != g_diagState) {
        g_diagState = state;
        static const wchar_t* names[] = {
            L"IDLE", L"LIVE", L"WAITING", L"ERROR", L"ENHOFF", L"REBUILD", L"OFF",
            L"STALEWAIT"
        };
        MiniEQ_AppLog(L"path state -> %s", names[state]);
        InvalidateRect(g_pill, nullptr, TRUE);
    }
    wchar_t cur[160] = {};
    GetWindowTextW(g_pill, cur, ARRAYSIZE(cur));
    if (wcscmp(cur, text) != 0) {
        SetWindowTextW(g_pill, text);
    }
    // The contextual fix guidance lives right under the pill.
    wchar_t curHint[256] = {};
    GetWindowTextW(g_hint, curHint, ARRAYSIZE(curHint));
    if (wcscmp(curHint, hint) != 0) {
        SetWindowTextW(g_hint, hint);
    }
    ShowWindow(g_hint, hint[0] ? SW_SHOW : SW_HIDE);
    // The broken states get the one action that actually fixes them:
    // enhancements-off or a bypassed path -> open Sound settings (the
    // Default Format toggle there rebuilds the audio path). Rebuilds and
    // pending reloads need no button at all.
    ShowWindow(g_btnSound, (state == DIAG_ERROR || state == DIAG_ENHOFF) ? SW_SHOW : SW_HIDE);
    // Keep the global on/off flag pushed to the APO (the APO creates the
    // channel on lock; until then this just retries the open), drive the
    // engine auto-reload check, and let the banner notes expire on the timer.
    SyncGlobalEnabled();
    UpdateEngineReload();
    UpdateBanner();
}

// Post-attach verification, driven by the 500 ms status timer (called right
// after UpdateDiagStatus). After an attach the engine must actually load the
// APO: DIAG_LIVE within 20 s means done. Otherwise escalate exactly once
// with a forced re-attach (slot rewrite + device re-enumeration, one more
// UAC prompt) -- the banner warns before the prompt appears, so the
// escalation never surprises. Never loops: after the escalation the outcome
// is reported honestly and the user drives.
static void ArmAttachVerify() {
    if (MiniEQ_BreakerLatched()) {
        return; // SAFE/DETACHED: no automatic verification/escalation until the user re-attaches
    }
    g_verifyUntil = GetTickCount64() + kVerifyMs;
    g_verifyEscalated = false;
    g_verifyNudgePending = false;
    g_verifyLoggedStuck = false;
    g_verifyText.clear();
    MiniEQ_AppLogCat(L"ENGINE", L"attach verify armed: watching for live heartbeat");
}

static void UpdateAttachVerify() {
    if (MiniEQ_BreakerLatched()) {
        // The breaker owns the state now: verification (and its forced
        // re-attach escalation) stays off until the user re-attaches.
        g_verifyUntil = 0;
        g_verifyNudgePending = false;
        return;
    }
    if (g_verifyNudgePending) {
        g_verifyNudgePending = false;
        const ULONGLONG nowEsc = GetTickCount64();
        if (g_lastEscalateTick != 0 && nowEsc - g_lastEscalateTick < 60000) {
            // Already escalated very recently: don't stack UAC prompts.
            // The previous escalation already extended the deadline.
            MiniEQ_AppLogCat(L"ENGINE", L"attach verify: escalation suppressed (rate-limited)");
            return;
        }
        g_lastEscalateTick = nowEsc;
        MiniEQ_AppLogCat(L"ENGINE", L"attach verify: escalating with forced re-attach");
        MiniEQ_BreakerNoteUserAction();
        if (DoElevatedAttach(true, /*force=*/true)) {
            g_verifyUntil = GetTickCount64() + kVerifyMs;
            g_verifyEscalated = true;
            g_verifyText.clear();
        } else {
            g_verifyUntil = 0; // elevation cancelled: stop verifying quietly
            g_verifyText.clear();
            UpdateBanner();
        }
        return;
    }
    if (g_verifyUntil == 0 || g_endpointId.empty()) {
        return;
    }
    if (g_diagState == DIAG_LIVE) {
        g_verifyUntil = 0;
        g_verifyText =
            L"Attached \u2014 MiniEQ is processing audio on this device.";
        g_bannerNote = true;
        g_bannerNoteTick = GetTickCount64();
        MiniEQ_AppLogCat(L"ENGINE", L"attach verify: heartbeat live, done");
        UpdateBanner();
        return;
    }
    // v4: while waiting, distinguish "the engine instantiated the APO but
    // never put it in the processing path" from "the APO never
    // instantiated". The former (initCalls > 0, processCalls == 0,
    // locked == 0) is the stuck state a forced re-enumeration fixes --
    // log the diagnosis once so the escalation below reads as a finding,
    // not a guess.
    if (!g_verifyLoggedStuck) {
        MiniEQApoStatus vst = {};
        if (g_statusLink.IsOpen() && g_statusLink.Read(&vst) &&
            vst.initCalls > 0 && vst.processCalls == 0 && vst.locked == 0) {
            g_verifyLoggedStuck = true;
            MiniEQ_AppLogCat(L"ENGINE",
                L"attach verify: APO instantiated %lld time(s) but never entered the processing path (Initialize without LockForProcess) -- forced re-enumeration is the fix",
                (long long)vst.initCalls);
        }
    }
    if (g_reloadWorkerBusy) {
        // The engine-reload worker is still working on it: don't time out
        // while it runs; the deadline slides with the timer.
        g_verifyUntil += 500;
        return;
    }
    if (GetTickCount64() >= g_verifyUntil) {
        if (!g_verifyEscalated) {
            // Warn first; the nudge (and its UAC prompt) fires on the next
            // tick so the user reads why it appears.
            g_verifyText =
                L"MiniEQ is attached, but the engine hasn't picked it up yet "
                L"\u2014 nudging it (one more admin prompt)\u2026";
            g_verifyNudgePending = true;
            UpdateBanner();
        } else {
            g_verifyUntil = 0;
            g_verifyText =
                L"MiniEQ is attached, but Windows still isn't loading it. "
                L"Turn this device off and back on (or unplug/replug it), then re-attach MiniEQ.";
            g_bannerNote = true;
            g_bannerNoteTick = GetTickCount64();
            MiniEQ_AppLogCat(L"ENGINE",
                L"attach verify: still not live after forced re-attach");
            UpdateBanner();
        }
    }
}

// Automatic endpoint-reappearance recovery (0.2.0): starts the bounded
// backoff of silent path reloads. Called when a device-change storm
// settles with our endpoint present but the EQ path not live. Each step
// re-runs the non-elevated engine reload; the 500 ms status poll observes
// the result. Stops on DIAG_LIVE, after 5 steps, or when superseded.
static void StartAutoRecovery(HWND hwnd) {
    if (MiniEQ_BreakerLatched()) {
        return; // SAFE/DETACHED: automatic recovery stays off until the user re-attaches
    }
    if (g_autoRecStep != 0) {
        return; // already running
    }
    g_autoRecStep = 1;
    MiniEQ_AppLogCat(L"ENGINE",
        L"auto-recovery: endpoint present but path not live; backoff started");
    SetTimer(hwnd, IDT_AUTORECOVER, kAutoRecBackoffMs[0], nullptr);
}

// Circuit-breaker enforcement, run once on the latch's rising edge: cancel
// every pending automatic recovery arm. The per-path gates (StartAutoRecovery,
// ArmAttachVerify, UpdateAttachVerify, UpdateEngineReload, the checklist
// watch) keep anything from re-arming while the latch holds.
static void CancelRecoveryForBreaker(HWND hwnd) {
    if (g_verifyUntil != 0 || g_verifyNudgePending) {
        g_verifyUntil = 0;
        g_verifyNudgePending = false;
        g_verifyText.clear();
        MiniEQ_AppLogCat(L"BREAKER",
            L"attach verification cancelled -- circuit breaker is latched");
    }
    if (g_autoRecStep != 0) {
        KillTimer(hwnd, IDT_AUTORECOVER);
        g_autoRecStep = 0;
        MiniEQ_AppLogCat(L"BREAKER",
            L"auto-recovery cancelled -- circuit breaker is latched");
    }
    UpdateBanner();
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
    g_linkSynced = false;
    // The APO creates the channel when it locks the stream; the UI only
    // opens it and keeps retrying on the status timer -- no error popup.
    TryOpenChannels();
    // The heartbeat channel follows the selected device.
    g_statusLink.Close();
    g_statusLink.Open(g_endpointId);
    g_lastCalls = 0;
    g_lastTick = 0;
    // Fresh device, fresh path history: the rebuild grace and the
    // enhancements reading start over.
    g_sawLive = false;
    g_lastLiveTick = 0;
    g_bypassSince = 0; // verdict debounce streak is per-device too
    g_enhCheckTick = 0;
    g_enhState = DiagEnhancements::Unknown;
    // A manual device switch ends any post-attach note from another device.
    g_bannerNote = false;
    g_verifyUntil = 0; // verification is per-endpoint; a switch ends it
    g_verifyNudgePending = false;
    g_verifyText.clear();
    // The Diagnostics Center watches the same device.
    MiniEQ_DiagCenterSetDevice(g_endpointId);
    ApplyStagingToUI();
    UpdateAttachStatus();
    UpdateDiagStatus();
}

static bool EndpointInList(const std::wstring& id) {
    for (const auto& d : g_devices) {
        if (d.id == id) {
            return true;
        }
    }
    return false;
}

static void CheckDefaultDevice(); // defined after RefreshDeviceList
static void RefreshDeviceList() {
    int keep = (int)SendMessageW(g_combo, CB_GETCURSEL, 0, 0);
    std::wstring keepId = (keep >= 0 && keep < (int)g_devices.size())
                          ? g_devices[(size_t)keep].id : L"";
    if (keepId.empty()) {
        // The combo selection may already be cleared (we clear it while the
        // kept device is transiently absent): fall back to the live binding.
        keepId = g_endpointId;
    }
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
    if (sel >= 0) {
        SendMessageW(g_combo, CB_SETCURSEL, (WPARAM)sel, 0);
        SelectDevice(sel);
    } else if (g_forceReselect || keepId.empty()) {
        // Fresh open with no default, or the ghost timer decided the kept
        // device is really gone: fall back to the system default (the old
        // behavior). Never strand the UI on a ghost device.
        const std::wstring def = MiniEQ_GetDefaultRenderEndpointId();
        for (size_t i = 0; i < g_devices.size(); ++i) {
            if (g_devices[i].id == def) {
                sel = (int)i;
                break;
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
    } else {
        // The kept device is transiently absent (device restart / Bluetooth
        // reconnect storm). Do NOT flap to another device and do NOT rebind
        // channels: keep the selection stable until it reappears. The ghost
        // timer (IDT_DEVGHOST) falls back to the default if it never does.
        SendMessageW(g_combo, CB_SETCURSEL, (WPARAM)-1, 0);
        UpdateAttachStatus(); // banner re-evaluates for the kept device
    }
    // A device change may have moved the system default (unplug, Bluetooth
    // reconnect, user switch in Settings). Follow it and offer the attach.
    CheckDefaultDevice();
}

// Default-device tracking: whenever the Windows default render endpoint
// changes -- and once at startup -- check whether our APO CLSID is in that
// endpoint's SFX slot (the same attach-check the UI uses). If it isn't,
// select the new default and show the one-click attach banner. Never
// auto-detaches old devices; never steals a manual selection unless the
// default itself moved.
static void CheckDefaultDevice() {
    const std::wstring def = MiniEQ_GetDefaultRenderEndpointId();
    if (def.empty() || def == g_lastDefaultId) {
        return;
    }
    const bool firstSeen = g_lastDefaultId.empty();
    g_lastDefaultId = def;
    if (firstSeen) {
        // Baseline at startup: RefreshDeviceList already pre-selected the
        // default; just evaluate the banner for it.
        UpdateBanner();
        return;
    }
    MiniEQ_AppLog(L"default render endpoint changed; following it");
    for (size_t i = 0; i < g_devices.size(); ++i) {
        if (g_devices[i].id == def) {
            SendMessageW(g_combo, CB_SETCURSEL, (WPARAM)i, 0);
            SelectDevice((int)i); // -> UpdateAttachStatus -> UpdateBanner
            return;
        }
    }
    UpdateBanner();
}

static bool DoElevatedAttach(bool attach, bool force) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::wstring args = attach ? L"--attach \"" : L"--detach \"";
    args += g_endpointId + L"\"";
    if (attach && force) {
        // Forced: rewrite the slot and re-enumerate the device even when the
        // slot already points at us (the engine never picked it up).
        args += L" --force";
    }

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_NORMAL;
    if (!ShellExecuteExW(&sei)) {
        return false; // elevation cancelled; caller decides whether to nag
    }
    // Give the elevated helper a moment, then re-read the state.
    Sleep(800);
    UpdateAttachStatus();
    // Attaching/detaching disturbs the engine on purpose -- the breaker's
    // passive detector must not mistake our own churn for a crash loop.
    MiniEQ_BreakerNoteUserAction();
    const bool ok = attach ? g_attached : !g_attached;
    if (!attach) {
        g_verifyUntil = 0; // a detach ends any pending attach verification
        g_verifyNudgePending = false;
        g_verifyText.clear();
    } else if (ok) {
        // The engine must actually load the APO now: watch for the live
        // heartbeat and escalate once (forced re-attach) if it never comes.
        ArmAttachVerify();
    }
    return ok;
}

static void RelaunchElevatedAttach(bool attach, bool force) {
    if (!DoElevatedAttach(attach, force)) {
        MessageBoxW(g_hwnd, L"Elevation was cancelled.", L"MiniEQ", MB_ICONINFORMATION);
    }
}

// Banner-driven attach: after the one-click attach, the fresh SFX
// registration needs the engine to rebuild the graph once. Chain the same
// format-flip reload the auto path uses: no services, no extra UAC beyond
// the attach itself. No MessageBox -- the banner reports the outcome.
static void ChainReloadForBanner() {
    g_bannerNote = true;
    g_bannerNoteTick = GetTickCount64();
    MiniEQ_AppLogCat(L"ENGINE",
        L"banner attach: reloading the audio path so the new registration takes effect");
    SpawnReloadWorker(/*force=*/true);
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
                                 12, 10, 456, 30, hwnd, (HMENU)IDC_DEVICENAME,
                                 g_hInst, nullptr);
    SendMessageW(g_deviceName, WM_SETFONT, (WPARAM)nameFont, TRUE);

    CreateWindowW(L"STATIC", L"Device:", WS_CHILD | WS_VISIBLE,
                  12, 52, 52, 18, hwnd, nullptr, g_hInst, nullptr);
    g_combo = CreateWindowW(L"COMBOBOX", nullptr,
                            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                            66, 48, 318, 200, hwnd, (HMENU)IDC_DEVICE_COMBO,
                            g_hInst, nullptr);
    applyFont(g_combo);
    g_refresh = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              390, 47, 78, 24, hwnd, (HMENU)IDC_REFRESH, g_hInst, nullptr);
    applyFont(g_refresh);

    g_attach = CreateWindowW(L"BUTTON", L"Attach to this device",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             12, 80, 170, 26, hwnd, (HMENU)IDC_ATTACH, g_hInst, nullptr);
    applyFont(g_attach);
    g_status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                             190, 85, 278, 18, hwnd, (HMENU)IDC_STATUS, g_hInst, nullptr);
    applyFont(g_status);

    // Auto-attach banner: shown when the Windows default render endpoint
    // changed to a device MiniEQ isn't attached to (see
    // UpdateBanner). Hidden otherwise; pushes the content below down
    // via LayoutContent().
    g_bannerText = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT,
                                 12, 108, 340, 36, hwnd, (HMENU)IDC_BANNERTEXT,
                                 g_hInst, nullptr);
    applyFont(g_bannerText);
    g_bannerBtn = CreateWindowW(L"BUTTON", L"Attach MiniEQ",
                                WS_CHILD | BS_PUSHBUTTON,
                                358, 114, 110, 26, hwnd, (HMENU)IDC_BANNERBTN,
                                g_hInst, nullptr);
    applyFont(g_bannerBtn);

    // Diagnostics: the honest EQ-path status pill, color-coded by the APO
    // heartbeat (see UpdateDiagStatus) -- never by registry guesses. The
    // hint line under it carries the exact fix for the current state. The
    // global MiniEQ on/off button sits at the right end of the pill row.
    g_pill = CreateWindowW(L"STATIC", L"",
                           WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                           12, 108, 330, 24, hwnd, (HMENU)IDC_DIAG_PILL,
                           g_hInst, nullptr);
    applyFont(g_pill);
    g_power = CreateWindowW(L"BUTTON", L"\u23FB Turn off MiniEQ",
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            348, 108, 120, 24, hwnd, (HMENU)IDC_POWER,
                            g_hInst, nullptr);
    applyFont(g_power);
    g_hint = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT,
                           12, 134, 456, 30, hwnd, (HMENU)IDC_DIAG_HINT,
                           g_hInst, nullptr);
    applyFont(g_hint);
    g_btnLog = CreateWindowW(L"BUTTON", L"View live log",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             12, 168, 120, 26, hwnd, (HMENU)IDC_DIAG_LOG,
                             g_hInst, nullptr);
    applyFont(g_btnLog);
    // Shown only when Audio Enhancements are off or the path is bypassed --
    // Sound settings is where the fix lives (the Default Format toggle
    // there rebuilds the audio path).
    g_btnSound = CreateWindowW(L"BUTTON", L"Open Sound settings",
                               WS_CHILD | BS_PUSHBUTTON,
                               140, 168, 170, 26, hwnd,
                               (HMENU)IDC_DIAG_SOUND, g_hInst, nullptr);
    applyFont(g_btnSound);
    g_btnDiagCenter = CreateWindowW(L"BUTTON", L"Diagnostics",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    378, 168, 90, 26, hwnd,
                                    (HMENU)IDC_DIAG_CENTER, g_hInst, nullptr);
    applyFont(g_btnDiagCenter);

    // Band sliders are built by BuildBandControls() (5 or 10, per the
    // settings toggle); the initial set is created in WM_CREATE.

    g_masterLabel = CreateWindowW(L"STATIC", L"Master", WS_CHILD | WS_VISIBLE,
                                  12, 428, 60, 18, hwnd, nullptr, g_hInst, nullptr);
    applyFont(g_masterLabel);
    g_master = CreateWindowW(TRACKBAR_CLASSW, nullptr,
                             WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                             70, 422, 230, 34, hwnd, (HMENU)IDC_MASTER, g_hInst, nullptr);
    SendMessageW(g_master, TBM_SETRANGE, TRUE, MAKELONG(-120, 120));
    SendMessageW(g_master, TBM_SETPAGESIZE, 0, 20);
    g_masterVal = CreateWindowW(L"STATIC", L"+0.0 dB", WS_CHILD | WS_VISIBLE,
                                308, 428, 70, 18, hwnd, (HMENU)IDC_MASTERVAL,
                                g_hInst, nullptr);
    applyFont(g_masterVal);

    g_bypass = CreateWindowW(L"BUTTON", L"Bypass (EQ off)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             12, 460, 140, 20, hwnd, (HMENU)IDC_BYPASS, g_hInst, nullptr);
    applyFont(g_bypass);

    g_presetLabel = CreateWindowW(L"STATIC", L"Presets:", WS_CHILD | WS_VISIBLE,
                                  12, 494, 60, 18, hwnd, nullptr, g_hInst, nullptr);
    applyFont(g_presetLabel);
    const wchar_t* presetNames[4] = { L"Flat", L"Bass", L"Vocal", L"Bright" };
    for (int i = 0; i < 4; ++i) {
        g_preset[i] = CreateWindowW(L"BUTTON", presetNames[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    76 + i * 78, 490, 70, 26, hwnd,
                                    (HMENU)(INT_PTR)(IDC_PRESET_FLAT + i), g_hInst, nullptr);
        applyFont(g_preset[i]);
    }

    g_settingsLabel = CreateWindowW(L"STATIC", L"Settings:", WS_CHILD | WS_VISIBLE,
                                    12, 528, 60, 18, hwnd, nullptr, g_hInst, nullptr);
    applyFont(g_settingsLabel);
    g_bandsLabel = CreateWindowW(L"STATIC", L"Bands:", WS_CHILD | WS_VISIBLE,
                                 76, 528, 44, 18, hwnd, nullptr, g_hInst, nullptr);
    applyFont(g_bandsLabel);
    g_bands5 = CreateWindowW(L"BUTTON", L"5", WS_CHILD | WS_VISIBLE |
                             BS_AUTORADIOBUTTON | WS_GROUP,
                             122, 526, 36, 20, hwnd, (HMENU)IDC_BANDS5,
                             g_hInst, nullptr);
    applyFont(g_bands5);
    g_bands10 = CreateWindowW(L"BUTTON", L"10", WS_CHILD | WS_VISIBLE |
                              BS_AUTORADIOBUTTON,
                              160, 526, 40, 20, hwnd, (HMENU)IDC_BANDS10,
                              g_hInst, nullptr);
    applyFont(g_bands10);
    // Optional headphone crossfeed (bs2b-style). Costs nothing
    // until turned on: the APO allocates its tiny state lazily.
    g_virtCheck = CreateWindowW(L"BUTTON", L"Crossfeed",
                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                210, 526, 120, 20, hwnd,
                                (HMENU)IDC_VIRTUALIZATION, g_hInst, nullptr);
    applyFont(g_virtCheck);
    // Audio-path checklist: every prerequisite for "audio goes through
    // MiniEQ", green/yellow/red with the fix on red rows.
    g_btnChecklist = CreateWindowW(L"BUTTON", L"Checklist",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   336, 524, 72, 24, hwnd,
                                   (HMENU)IDC_CHECKLIST, g_hInst, nullptr);
    applyFont(g_btnChecklist);

    g_note = CreateWindowW(L"STATIC",
        L"Attach once per device (asks for admin). Sliders apply live.",
        WS_CHILD | WS_VISIBLE, 12, 554, 456, 30, hwnd, nullptr, g_hInst, nullptr);
    applyFont(g_note);

    // Position everything once (banner hidden at startup).
    LayoutContent();
}

// Repositions every control at/below the pill row for the auto-attach
// banner: when the banner shows, content shifts down 44 px and the window
// grows; when it hides, everything returns. Base Y coordinates are the
// layout's; the shift is g_contentDy.
static void LayoutContent() {
    if (g_inLayout) {
        return; // a layout pass never nests inside another
    }
    g_inLayout = true;
    const int dy = g_contentDy;
    auto place = [&](HWND h, int x, int baseY, int w, int hgt) {
        if (h != nullptr) {
            MoveWindow(h, x, baseY + dy, w, hgt, TRUE);
        }
    };
    place(g_pill, 12, 108, 330, 24);
    place(g_power, 348, 108, 120, 24);
    place(g_hint, 12, 134, 456, 30);
    place(g_btnLog, 12, 168, 120, 26);
    place(g_btnSound, 140, 168, 170, 26);
    place(g_btnDiagCenter, 378, 168, 90, 26);
    // Band sliders are dynamic; recover their x/width and set the shifted y.
    for (int i = 0; i < MINIEQ_MAX_BANDS; ++i) {
        RECT rc = {};
        if (g_band[i] != nullptr) {
            GetWindowRect(g_band[i], &rc);
            MapWindowPoints(HWND_DESKTOP, g_hwnd, reinterpret_cast<LPPOINT>(&rc), 2);
            MoveWindow(g_band[i], rc.left, 200 + dy, rc.right - rc.left, 170, TRUE);
        }
        if (g_bandName[i] != nullptr) {
            GetWindowRect(g_bandName[i], &rc);
            MapWindowPoints(HWND_DESKTOP, g_hwnd, reinterpret_cast<LPPOINT>(&rc), 2);
            MoveWindow(g_bandName[i], rc.left, 374 + dy, rc.right - rc.left, 18, TRUE);
        }
        if (g_bandVal[i] != nullptr) {
            GetWindowRect(g_bandVal[i], &rc);
            MapWindowPoints(HWND_DESKTOP, g_hwnd, reinterpret_cast<LPPOINT>(&rc), 2);
            MoveWindow(g_bandVal[i], rc.left, 392 + dy, rc.right - rc.left, 18, TRUE);
        }
    }
    place(g_masterLabel, 12, 428, 60, 18);
    place(g_master, 70, 422, 230, 34);
    place(g_masterVal, 308, 428, 70, 18);
    place(g_bypass, 12, 460, 140, 20);
    place(g_presetLabel, 12, 494, 60, 18);
    for (int i = 0; i < 4; ++i) {
        place(g_preset[i], 76 + i * 78, 490, 70, 26);
    }
    place(g_settingsLabel, 12, 528, 60, 18);
    place(g_bandsLabel, 76, 528, 44, 18);
    place(g_bands5, 122, 526, 36, 20);
    place(g_bands10, 160, 526, 40, 20);
    place(g_virtCheck, 210, 526, 120, 20);
    place(g_btnChecklist, 336, 524, 72, 24);
    place(g_note, 12, 554, 456, 30);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 480, 632 + dy,
                 SWP_NOMOVE | SWP_NOZORDER);
    g_inLayout = false;
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
            static const COLORREF bgc[8] = {
                RGB(240, 240, 240), RGB(233, 247, 238),
                RGB(255, 248, 232), RGB(253, 238, 238),
                RGB(253, 238, 238), RGB(255, 248, 232),
                RGB(235, 235, 235), RGB(255, 248, 232)
            };
            for (int i = 0; i < 8; ++i) {
                g_diagBrush[i] = CreateSolidBrush(bgc[i]);
            }
            // Banner tints for the engine-reload states.
            g_bannerOkBrush = CreateSolidBrush(RGB(233, 247, 238));
            g_bannerWarnBrush = CreateSolidBrush(RGB(255, 248, 232));
        }
        RefreshDeviceList();
        ApplyStagingToUI(); // builds the band sliders if no device was selected
        ApplyPowerUI(); // global on/off: button label + EQ control dimming
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
            UpdateAttachVerify(); // post-attach heartbeat watch, if armed
            // Circuit breaker: poll the passive crash-loop detector about
            // every 5 s. It fires non-blockingly and only on a genuine
            // restart loop; the outcome arrives as WM_APP_BREAKER_DONE.
            static int breakerTick = 0;
            if ((++breakerTick % 10) == 0) {
                MiniEQ_BreakerPoll(hwnd);
            }
            // SAFE/DETACHED latch: on its rising edge, cancel every pending
            // automatic recovery arm exactly once. The gates on each
            // recovery entry point keep them off while the latch holds.
            const bool breakerLatched = MiniEQ_BreakerLatched();
            if (breakerLatched && !g_breakerLatchSeen) {
                g_breakerLatchSeen = true;
                MiniEQ_AppLogCat(L"BREAKER",
                    L"latched SAFE/DETACHED -- automatic recovery suspended until you re-attach");
                CancelRecoveryForBreaker(hwnd);
            } else if (!breakerLatched) {
                g_breakerLatchSeen = false;
            }
        } else if (wParam == IDT_DEVSETTLE) {
            KillTimer(hwnd, IDT_DEVSETTLE);
            if (g_devChangeCoalesced > 1) {
                MiniEQ_AppLogCat(L"UI",
                    L"device-change storm settled (%d notifications): rebuilding once",
                    g_devChangeCoalesced);
            }
            g_devChangeCoalesced = 0;
            RefreshDeviceList();
            if (!g_forceReselect && !g_endpointId.empty() &&
                !EndpointInList(g_endpointId)) {
                // The kept device is still missing after the storm. It may
                // be a slow re-enumeration (Bluetooth) or a real unplug:
                // give it 4 s, then fall back instead of showing a ghost.
                SetTimer(hwnd, IDT_DEVGHOST, 4000, nullptr);
            } else if (g_attached && !g_endpointId.empty() &&
                       EndpointInList(g_endpointId) &&
                       g_verifyUntil == 0 && g_autoRecStep == 0) {
                // The endpoint survived the storm (Bluetooth reconnect, dock
                // re-plug) and we're still registered on it. If the EQ path
                // isn't live, run the automatic reattach: a bounded backoff
                // of silent path reloads, no UAC, no loops.
                UpdateDiagStatus();
                if (g_diagState != DIAG_LIVE) {
                    StartAutoRecovery(hwnd);
                }
            }
        } else if (wParam == IDT_AUTORECOVER) {
            KillTimer(hwnd, IDT_AUTORECOVER);
            if (MiniEQ_BreakerLatched()) {
                // The breaker tripped mid-backoff: stop, don't reload.
                g_autoRecStep = 0;
                MiniEQ_AppLogCat(L"BREAKER",
                    L"auto-recovery stopped -- circuit breaker is latched");
            } else if (g_autoRecStep <= 0 || g_autoRecStep > 5) {
                g_autoRecStep = 0;
            } else if (g_diagState == DIAG_LIVE) {
                // The 500 ms poll saw the heartbeat come back: recovered.
                MiniEQ_AppLogCat(L"ENGINE",
                    L"auto-recovery: path live, stopping");
                g_autoRecStep = 0;
            } else {
                MiniEQ_AppLogCat(L"ENGINE",
                    L"auto-recovery: silent reload %d/5", g_autoRecStep);
                SpawnReloadWorker(/*force=*/true);
                ++g_autoRecStep;
                if (g_autoRecStep <= 5) {
                    SetTimer(hwnd, IDT_AUTORECOVER,
                             kAutoRecBackoffMs[g_autoRecStep - 1], nullptr);
                } else {
                    // Backoff exhausted and the path is still dead: stop the
                    // silent attempts. Arm the post-attach verification so
                    // its one warned escalation (forced re-attach) can take
                    // over -- the user sees the banner before any prompt.
                    g_autoRecStep = 0;
                    MiniEQ_AppLogCat(L"ENGINE",
                        L"auto-recovery: backoff exhausted, arming verification");
                    ArmAttachVerify();
                }
            }
        } else if (wParam == IDT_DEVGHOST) {
            KillTimer(hwnd, IDT_DEVGHOST);
            if (!g_endpointId.empty() && !EndpointInList(g_endpointId)) {
                MiniEQ_AppLogCat(L"UI",
                    L"device still missing after 4 s: falling back to default");
                g_forceReselect = true;
                RefreshDeviceList();
                g_forceReselect = false;
            }
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        // The diagnostics pill is color-coded by path state.
        if ((HWND)lParam == g_pill && g_diagState >= 0 && g_diagState <= 7) {
            static const COLORREF bg[8] = {
                RGB(240, 240, 240), RGB(233, 247, 238),
                RGB(255, 248, 232), RGB(253, 238, 238),
                RGB(253, 238, 238), RGB(255, 248, 232),
                RGB(235, 235, 235), RGB(255, 248, 232)
            };
            static const COLORREF fg[8] = {
                RGB(85, 85, 85), RGB(20, 83, 45),
                RGB(122, 91, 0), RGB(143, 29, 29),
                RGB(143, 29, 29), RGB(122, 91, 0),
                RGB(110, 110, 110), RGB(122, 91, 0)
            };
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, bg[g_diagState]);
            SetTextColor(hdc, fg[g_diagState]);
            return (LRESULT)g_diagBrush[g_diagState];
        }
        // The banner tints green/amber for the engine-reload states, so the
        // outcome reads at a glance. Plain otherwise.
        if ((HWND)lParam == g_bannerText) {
            HDC hdc = (HDC)wParam;
            if (g_bannerKind == BannerKind::ReloadDone && g_bannerOkBrush != nullptr) {
                SetBkColor(hdc, RGB(233, 247, 238));
                SetTextColor(hdc, RGB(20, 83, 45));
                return (LRESULT)g_bannerOkBrush;
            }
            if ((g_bannerKind == BannerKind::ReloadPending ||
                 g_bannerKind == BannerKind::ReloadFailed) &&
                g_bannerWarnBrush != nullptr) {
                SetBkColor(hdc, RGB(255, 248, 232));
                SetTextColor(hdc, RGB(122, 91, 0));
                return (LRESULT)g_bannerWarnBrush;
            }
            break;
        }
        // The hint line: quiet gray on the window background.
        if ((HWND)lParam == g_hint) {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            SetTextColor(hdc, RGB(90, 90, 90));
            return (LRESULT)(COLOR_WINDOW + 1);
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
                // A deliberate attach/detach is the one user action that
                // clears the circuit-breaker latch.
                MiniEQ_BreakerUserResume();
                RelaunchElevatedAttach(!g_attached, /*force=*/false);
            }
        } else if (id == IDC_POWER) {
            // Global MiniEQ on/off: persists across restarts (devices.ini
            // [MiniEQ]), applies to every device, and reaches the APO through
            // the global channel (pushed now; the status timer re-asserts it
            // if the APO re-creates the channel later).
            const bool on = !MiniEQ_GlobalEnabledGet();
            MiniEQ_GlobalEnabledSet(on);
            if (!g_globalLink.IsOpen()) {
                g_globalLink.Open();
            }
            g_globalLink.WriteEnabled(on);
            MiniEQ_AppLog(L"UI: MiniEQ turned %s (global)", on ? L"ON" : L"OFF");
            ApplyPowerUI();
            UpdateDiagStatus();
        } else if (id == IDC_BANNERBTN) {
            // The banner button's meaning follows the banner kind.
            switch (g_bannerKind) {
            case BannerKind::AttachOffer: {
                // One-click attach for the new default device: elevated
                // attach, then chain the format-flip reload so the fresh
                // registration takes effect now.
                if (!g_endpointId.empty() && !g_attached) {
                    MiniEQ_AppLog(L"UI: attach banner accepted for new default device");
                    MiniEQ_BreakerUserResume(); // deliberate attach clears the latch
                    if (DoElevatedAttach(true, /*force=*/false)) {
                        ChainReloadForBanner();
                    }
                    UpdateAttachStatus(); // re-evaluates the banner (note or hide)
                }
                break;
            }
            case BannerKind::ReloadPending:
                // "Reload now": explicit consent, flip even with audio playing.
                MiniEQ_AppLogCat(L"ENGINE", L"manual reload requested from banner");
                g_reloadDeferredLogged = true; // the manual run supersedes the wait
                SpawnReloadWorker(/*force=*/true);
                break;
            case BannerKind::ReloadFailed:
                MiniEQ_AppLogCat(L"ENGINE", L"reload retry requested from banner");
                SpawnReloadWorker(/*force=*/false);
                break;
            case BannerKind::ReloadDone:
                MiniEQ_ShowLogViewer(g_hInst, g_hwnd);
                break;
            default:
                break;
            }
        } else if (id == IDC_DIAG_LOG) {
            MiniEQ_ShowLogViewer(g_hInst, g_hwnd);
        } else if (id == IDC_DIAG_SOUND) {
            // Audio Enhancements live under Settings -> System -> Sound ->
            // [device]. Land the user on the Sound page; the hint line under
            // the pill names the exact device and value to pick.
            MiniEQ_AppLog(L"UI: opening Sound settings (enhancements fix)");
            ShellExecuteW(nullptr, L"open", L"ms-settings:sound",
                          nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == IDC_DIAG_CENTER) {
            MiniEQ_DiagCenterSetDevice(g_endpointId);
            MiniEQ_ShowDiagCenter(g_hInst, g_hwnd);
        } else if (id == IDC_CHECKLIST) {
            wchar_t dev[128] = {};
            GetWindowTextW(g_deviceName, dev, ARRAYSIZE(dev));
            MiniEQ_ShowChecklist(g_hInst, g_hwnd, g_endpointId, dev);
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

    case WM_APP_RELOAD_DONE: {
        // A reload worker finished (lParam owns an EngineReloadOutcome*).
        EngineReloadOutcome* out =
            reinterpret_cast<EngineReloadOutcome*>(lParam);
        g_reloadWorkerBusy = false;
        if (out == nullptr) {
            break;
        }
        // The user may have switched devices mid-reload: a stale outcome
        // must not paint the new device's banner. The flip itself is
        // harmless (it always restores the format), so just log and drop.
        if (out->endpoint != g_endpointId) {
            MiniEQ_AppLogCat(L"ENGINE",
                L"reload outcome for a deselected device arrived late; ignored");
            delete out;
            break;
        }
        switch (out->result) {
        case EngineReloadResult::UpToDate:
            g_reloadUi = ReloadUiState::None;
            break;
        case EngineReloadResult::Deferred:
            g_reloadUi = ReloadUiState::Pending;
            g_reloadSession = out->activeSession;
            g_reloadRetryTick = GetTickCount64();
            if (!g_reloadDeferredLogged) {
                g_reloadDeferredLogged = true;
                MiniEQ_AppLogCat(L"ENGINE",
                    L"auto-reload deferred -- active session \"%s\"; "
                    L"waiting for audio to stop (banner shown)",
                    out->activeSession.c_str());
            }
            break;
        case EngineReloadResult::Reloaded: {
            g_reloadUi = ReloadUiState::DoneNote;
            g_reloadNoteTick = GetTickCount64();
            // The banner names the build that is live NOW (the installed
            // one), not the stale build it replaced.
            wchar_t live[16] = {};
            size_t conv = 0;
            mbstowcs_s(&conv, live, ARRAYSIZE(live),
                       MiniEQ_ExpectedBuildId(), _TRUNCATE);
            g_reloadBuild = live;
            g_bannerNote = false; // the reload banner supersedes the attach note
            // Mark this build as seen so the timer doesn't re-arm.
            g_reloadKey = g_endpointId + L"|fresh";
            MiniEQ_AppLogCat(L"ENGINE",
                L"auto-reload done -- engine instantiated build %S (was %s); "
                L"instantiation verified, live processing is confirmed by the heartbeat",
                MiniEQ_ExpectedBuildId(), out->reportedBuild.c_str());
            break;
        }
        case EngineReloadResult::Failed:
            g_reloadUi = ReloadUiState::FailedNote;
            g_reloadNoteTick = GetTickCount64();
            g_reloadDetail = out->detail;
            MiniEQ_AppLogCat(L"ENGINE", L"auto-reload FAILED -- %s",
                             out->detail.c_str());
            break;
        case EngineReloadResult::FlipError:
            g_reloadUi = ReloadUiState::FailedNote;
            g_reloadNoteTick = GetTickCount64();
            g_reloadDetail = out->detail;
            MiniEQ_AppLogCat(L"ENGINE", L"auto-reload could not flip the format -- %s",
                             out->detail.c_str());
            break;
        }
        delete out;
        UpdateDiagStatus(); // pill + banner reflect the outcome immediately
        break;
    }

    case WM_APP_BREAKER_DONE: {
        // The circuit breaker's elevated sweep finished (wParam: outcome
        // code -- see diagcenter.h).
        const int outcome = (int)wParam;
        if (outcome == BreakerOutcomeLaunched) {
            MiniEQ_AppLogCat(L"BREAKER",
                L"sweep done -- telling the user the EQ was sacrificed for stability");
            MessageBoxW(hwnd,
                L"MiniEQ detected the Windows audio engine restarting in a loop "
                L"and detached itself from all your audio devices to keep your "
                L"sound stable.\n\n"
                L"Your audio should be working again now, without the EQ. "
                L"MiniEQ will stay detached and won't try to re-attach itself: "
                L"when you're ready, click \"Attach to this device\" to turn "
                L"the EQ back on.",
                L"MiniEQ circuit breaker", MB_ICONWARNING);
        } else if (outcome == BreakerOutcomeLaunchFailed ||
                   outcome == BreakerOutcomeSweepFailed) {
            MiniEQ_AppLogCat(L"BREAKER",
                L"sweep could not detach -- asking the user to detach manually");
            MessageBoxW(hwnd,
                L"MiniEQ detected the Windows audio engine restarting in a loop, "
                L"but could not detach itself automatically.\n\n"
                L"Please detach MiniEQ from your devices (Devices list \u2192 "
                L"Detach) to restore stable audio.",
                L"MiniEQ circuit breaker", MB_ICONWARNING);
        } else if (outcome == BreakerOutcomePartial) {
            MiniEQ_AppLogCat(L"BREAKER",
                L"sweep only partly detached -- asking the user to finish manually");
            MessageBoxW(hwnd,
                L"MiniEQ detected the Windows audio engine restarting in a loop "
                L"and detached itself from some of your devices, but a few "
                L"could not be detached.\n\n"
                L"Please detach MiniEQ from the remaining devices (Devices "
                L"list \u2192 Detach) to restore stable audio.",
                L"MiniEQ circuit breaker", MB_ICONWARNING);
        }
        // Outcome 1 (UAC declined): the user already said no -- stay silent.
        UpdateAttachStatus();
        UpdateDiagStatus();
        return 0;
    }

    case WM_DEVICECHANGE:
        // Aux / USB-C / Bluetooth (un)plugged while the app is open -- and,
        // notably, our own device restart after an attach. A restart fires a
        // burst of these; rebuilding the list, rebinding channels and
        // relaying out the window on every one tore the UI. Coalesce: one
        // rebuild, 750 ms after the last notification.
        KillTimer(hwnd, IDT_DEVSETTLE);
        KillTimer(hwnd, IDT_DEVGHOST); // storm continues, or the device is back
        ++g_devChangeCoalesced;
        SetTimer(hwnd, IDT_DEVSETTLE, 750, nullptr);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_DIAG);
        for (int i = 0; i < 8; ++i) {
            if (g_diagBrush[i] != nullptr) {
                DeleteObject(g_diagBrush[i]);
                g_diagBrush[i] = nullptr;
            }
        }
        if (g_bannerOkBrush != nullptr) {
            DeleteObject(g_bannerOkBrush);
            g_bannerOkBrush = nullptr;
        }
        if (g_bannerWarnBrush != nullptr) {
            DeleteObject(g_bannerWarnBrush);
            g_bannerWarnBrush = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//------------------------------------------------------------------------------
// Entry
//------------------------------------------------------------------------------

static int RunBulkEndpointHelper(bool attach) {
    // MSI install/uninstall worker: attach (or detach) MiniEQ on every
    // active render endpoint present right now. Silent by design -- see the
    // Usage note above. Always exits 0: one stubborn device must never fail
    // the whole setup; the in-app Attach button remains for devices plugged
    // in later.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        return 0;
    }
    const std::vector<AudioEndpoint> devices = MiniEQ_ListRenderEndpoints();
    int ok = 0, failed = 0;
    for (const AudioEndpoint& ep : devices) {
        const HRESULT r = attach ? MiniEQ_AttachToEndpointEx(ep.id.c_str(),
                                                             /*force=*/true)
                                 : MiniEQ_DetachFromEndpoint(ep.id.c_str());
        if (SUCCEEDED(r)) {
            ++ok;
        } else {
            ++failed;
        }
    }
    CoUninitialize();
    MiniEQ_EnsureLogDir();
    MiniEQ_AppLogCat(L"SETUP", L"%s: %d ok, %d failed (%d endpoints present)",
                     attach ? L"attach-all" : L"detach-all",
                     ok, failed, (int)devices.size());
    return 0;
}

static int RunCircuitBreakerSweep() {
    // Circuit-breaker worker: runs ELEVATED and SILENT (no message boxes --
    // the main UI explains what happened). Detaches MiniEQ from every
    // endpoint that is attached -- active or not -- and re-enumerates each
    // affected device node so the engine drops our APO from the chain. Never
    // touches the audio service; one stubborn device must never fail the
    // whole sweep.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        return 0;
    }
    int detached = 0, failed = 0;
    IMMDeviceEnumerator* pEnum = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   (void**)&pEnum)) && pEnum != nullptr) {
        IMMDeviceCollection* pColl = nullptr;
        // All states, not just active: an attached-but-unplugged device
        // leaves a stale registration behind too.
        if (SUCCEEDED(pEnum->EnumAudioEndpoints(
                eRender, DEVICE_STATEMASK_ALL, &pColl)) && pColl != nullptr) {
            UINT count = 0;
            pColl->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* pDev = nullptr;
                LPWSTR id = nullptr;
                if (FAILED(pColl->Item(i, &pDev)) || pDev == nullptr) {
                    continue;
                }
                if (SUCCEEDED(pDev->GetId(&id)) && id != nullptr) {
                    bool attached = false;
                    if (SUCCEEDED(MiniEQ_IsAttachedToEndpoint(id, &attached)) &&
                        attached) {
                        if (SUCCEEDED(MiniEQ_DetachFromEndpoint(id))) {
                            ++detached;
                            // Re-enumerate so the engine re-reads the chain
                            // now; a missing devnode just fails quietly.
                            MiniEQ_ReenumerateEndpointDevice(id);
                        } else {
                            ++failed;
                        }
                    }
                    CoTaskMemFree(id);
                }
                pDev->Release();
            }
            pColl->Release();
        }
        pEnum->Release();
    }
    CoUninitialize();
    MiniEQ_EnsureLogDir();
    MiniEQ_AppLogCat(L"BREAKER",
        L"sweep finished: detached %d endpoint(s), %d failure(s)",
        detached, failed);
    // The exit code IS the result for the parent: 0 = clean (or nothing was
    // attached), 1 = partial (some detached, some failed), 2 = total failure.
    // The parent waits on the process and reads this -- a sweep that failed
    // everywhere must not claim "your audio should be working again".
    return (failed == 0) ? 0 : (detached > 0 ? 1 : 2);
}

static int RunElevatedHelper(LPWSTR* argv, int argc) {
    // argv: [exe, --attach|--detach, <endpoint-id>, [--force]]
    if (argc < 3) {
        return 1;
    }
    const bool attach = (_wcsicmp(argv[1], L"--attach") == 0);
    const bool force = attach && argc >= 4 && (_wcsicmp(argv[3], L"--force") == 0);
    HRESULT hr = attach ? MiniEQ_AttachToEndpointEx(argv[2], force)
                        : MiniEQ_DetachFromEndpoint(argv[2]);
    if (SUCCEEDED(hr)) {
        // Attach owns its restart decision now: MiniEQ_AttachToEndpointEx
        // restarts the endpoint device (disable + enable) when the slot
        // changed, or always with --force, and returns S_FALSE when the slot
        // already held us and no force was given (restart skipped -- no
        // audio interruption at all). Detach still needs the explicit
        // restart below; its function does not do it.
        bool restarted = false;
        if (attach) {
            restarted = (hr == S_OK);
        } else {
            restarted = SUCCEEDED(MiniEQ_ReenumerateEndpointDevice(argv[2]));
        }
        if (attach && hr == S_FALSE) {
            MessageBoxW(nullptr,
                        L"MiniEQ is already attached to this device.\nNo restart was needed -- your audio was not interrupted.",
                        L"MiniEQ", MB_ICONINFORMATION);
        } else if (restarted) {
            MessageBoxW(nullptr,
                        attach ? L"MiniEQ is now attached to this device.\nThe device was restarted -- EQ is live. During the restart your audio may briefly play through your speakers, then return to this device."
                               : L"MiniEQ has been detached from this device.\nThe device was restarted. During the restart your audio may briefly play through your speakers.",
                        L"MiniEQ", MB_ICONINFORMATION);
        } else {
            MessageBoxW(nullptr,
                        attach ? L"MiniEQ is now attached to this device.\nThe device could not be restarted automatically -- please unplug and replug it (or restart the computer) for the EQ to take effect."
                               : L"MiniEQ has been detached from this device.\nThe device could not be restarted automatically -- please unplug and replug it (or restart the computer).",
                        L"MiniEQ", MB_ICONWARNING);
        }
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
        int rc;
        if (_wcsicmp(argv[1], L"--attach-all") == 0) {
            rc = RunBulkEndpointHelper(true);
        } else if (_wcsicmp(argv[1], L"--detach-all") == 0) {
            rc = RunBulkEndpointHelper(false);
        } else if (_wcsicmp(argv[1], L"--breaker") == 0) {
            rc = RunCircuitBreakerSweep();
        } else {
            rc = RunElevatedHelper(argv, argc);
        }
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

    wchar_t title[64] = {};
    StringCchPrintfW(title, ARRAYSIZE(title), L"MiniEQ %s", MINIEQ_APP_VERSION);
    HWND hwnd = CreateWindowExW(0, L"MiniEQWnd", title,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 480, 632,
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

