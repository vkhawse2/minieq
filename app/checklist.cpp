// checklist.cpp -- MiniEQ "Audio Path Checklist" popup.
//
// Every prerequisite for "audio goes through MiniEQ", checked live and shown
// as green (working) / yellow (not active yet) / red (error). Red rows carry
// the exact fix. All checks come from MiniEQ_RunDiagnosis() plus the spatial
// read, so this popup agrees with the Diagnostics Center by construction.

#include "checklist.h"

#include "diagcenter.h"
#include "diag.h"
#include "settings_link.h"
#include "engine_reload.h"
#include "../apo/registration.h"

#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>

#include <string>
#include <thread>

namespace {

enum class CheckState { Ok, Idle, Error }; // green, yellow, red

struct CheckRow {
    CheckState   state = CheckState::Idle;
    std::wstring title;
    std::wstring detail; // gray, up to 2 lines
    std::wstring fix;    // red, 1 line, shown only on Error rows
};

enum {
    IDC_CL_DEVNAME = 401,
    IDC_CL_SUB,
    IDC_CL_SECT1,
    IDC_CL_SECT2,
    IDC_CL_SECT3,
    IDC_CL_DOT0   = 410, // + row index (8 rows)
    IDC_CL_TITLE0 = 420,
    IDC_CL_DETAIL0 = 430,
    IDC_CL_FIX0   = 440,
    IDC_CL_LEGEND = 450,
    IDC_CL_REFRESH,
    IDC_CL_CLOSE,
    IDC_CL_SPATIALFIX,
    IDC_CL_ENHFIX,
    IDC_CL_SECT4,
    IDC_CL_RECSEG,   // owner-drawn 3-way recovery toggle
    IDC_CL_RECDESC,  // what the selected fix does
    IDC_CL_RECSLT,   // last recovery outcome
};

constexpr int kRows = 9;

const wchar_t* kClass = L"MiniEQChecklist";

const COLORREF kDotColor[3] = {
    RGB(29, 158, 75),   // Ok    -- green
    RGB(224, 168, 0),   // Idle  -- yellow
    RGB(212, 58, 47),   // Error -- red
};

HINSTANCE   s_hInst = nullptr;
HWND        s_hDlg = nullptr;
bool        s_open = false;
std::wstring s_endpoint;
std::wstring s_deviceName;

HWND      s_hDevName = nullptr;
HWND      s_hSub = nullptr;
HWND      s_hSect[4] = {};
HWND      s_hDot[kRows] = {};
HWND      s_hTitle[kRows] = {};
HWND      s_hDetail[kRows] = {};
HWND      s_hFix[kRows] = {};
HWND      s_hLegend = nullptr;
HWND      s_hSpatialFix = nullptr; // one-click "Turn off" on the spatial row
bool      s_spatialFixFailed = false;
HWND      s_hEnhFix = nullptr; // one-click "Turn on" on the enhancements row
bool      s_enhFixFailed = false;

// --- Recovery: three-way toggle (Reload path / Re-attach / Watch engine) ---
// Each position targets one suspect for "0 APOProcess calls while audio
// plays": (0) the engine never loaded MiniEQ (stale graph) -- rebuild the
// path via the format flip; (1) the engine loaded MiniEQ but routes around
// it -- re-write the SFX slot (needs elevation), then rebuild; (2) the
// engine keeps dying -- stay armed and re-establish MiniEQ whenever
// audiodg's pid changes. Positions 0/1 run once per tap; 2 toggles.
// Nothing here touches the audio service.
enum class RecoverySel { Reload = 0, Reattach = 1, Watch = 2 };
HWND        s_hRecSeg = nullptr;    // owner-drawn 3-segment control
HWND        s_hRecDesc = nullptr;   // what the selected fix does
HWND        s_hRecResult = nullptr; // last recovery outcome
RecoverySel s_recSel = RecoverySel::Reload;
bool        s_recBusy = false;      // a one-shot worker is running
bool        s_recWatchArmed = false;
DWORD       s_recWatchPid = 0;      // audiodg pid baseline while armed
DWORD       s_lastAudiodgPid = 0;   // latest pid seen by RefreshChecklist
ULONGLONG   s_recWatchLastMs = 0;   // cooldown between auto-recoveries
int         s_recWatchCount = 0;    // auto-disarm after kWatchMaxRecoveries
int         s_recSeq = 0;           // invalidates stale worker completions
std::wstring s_recResultText;
static constexpr ULONGLONG kWatchCooldownMs = 30000;
static constexpr int kWatchMaxRecoveries = 5;
#define WM_APP_RECOVERYDONE (WM_APP + 102)

const wchar_t* kRecDesc[3] = {
    L"Suspect: the engine never loaded MiniEQ (stale graph).\r\n"
    L"Rebuilds the audio path so the engine reloads MiniEQ from disk, then "
    L"watches for the processing heartbeat.",
    L"Suspect: the engine loaded MiniEQ but routes audio around it.\r\n"
    L"Re-writes MiniEQ\u2019s slot in the effects chain (one admin approval), "
    L"then rebuilds the path.",
    L"Suspect: the audio engine keeps dying and restarting.\r\n"
    L"Stays on: when the engine restarts itself, MiniEQ re-establishes "
    L"automatically. Never touches the audio service.",
};
// What a one-click fix reports on its row afterwards. Switching: the WinRT
// worker is running. Watching: the live write went through and we're
// waiting for the heartbeat to prove the graph rebuilt. AppliedLive: it
// did -- no reload needed. Reloaded: the live path didn't heal in time, so
// we chained the format-flip engine reload (no services). The note shows for
// ~2 minutes, on the fixed row only (s_fixNoteRow: 3 = enhancements,
// 4 = spatial).
enum class FixNote { None, Switching, Watching, AppliedLive, Reloaded };
FixNote   s_fixNote = FixNote::None;
int       s_fixNoteRow = -1;
ULONGLONG s_fixNoteTick = 0;
int       s_fixSeq = 0; // invalidates stale worker completions
static constexpr ULONGLONG kFixWatchMs = 10000;
static constexpr ULONGLONG kFixNoteMs = 120000;
#define WM_APP_SPATIALDONE (WM_APP + 101)

// After a one-click property fix the engine can keep the old graph for
// already-running streams: a raw property write doesn't invalidate it.
// (The Settings app routes through the audio service, which is why its own
// toggles apply live.) Chain the same format-flip engine reload the main
// window uses: no services, no UAC. Runs on a worker thread; the row note
// below is informational only.
static void ChainReloadFlip(int row) {
    s_fixNote = FixNote::Reloaded;
    s_fixNoteRow = row;
    s_fixNoteTick = GetTickCount64();
    const std::wstring endpoint = s_endpoint;
    std::thread([endpoint]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring detail;
        const bool ok = MiniEQ_FlipDefaultFormat(endpoint, &detail);
        CoUninitialize();
        MiniEQ_AppLogCat(L"ENGINE",
            ok ? L"checklist fix: audio path reloaded via format flip"
               : L"checklist fix: format flip failed (%s)", detail.c_str());
    }).detach();
}

// Marks the start of the live-apply watch: the WinRT switch completed, now
// the heartbeat has to prove the running graph picked it up.
static void BeginFixWatch(int row) {
    s_fixNote = FixNote::Watching;
    s_fixNoteRow = row;
    s_fixNoteTick = GetTickCount64();
}

struct SpatialFixCtx {
    HWND hwnd;
    std::wstring endpoint;
    int seq;
};

// Worker thread: the WinRT spatial switch blocks on the async operation,
// so it must not run on the UI thread. Posts WM_APP_SPATIALDONE back.
static DWORD WINAPI SpatialFixThread(LPVOID param) {
    SpatialFixCtx* ctx = static_cast<SpatialFixCtx*>(param);
    const bool ok = MiniEQ_SetSpatialSoundOffWinRT(ctx->endpoint);
    const HWND hwnd = ctx->hwnd;
    const int seq = ctx->seq;
    delete ctx;
    PostMessageW(hwnd, WM_APP_SPATIALDONE, ok ? 1 : 0, seq);
    return 0;
}



HFONT     s_font = nullptr;
HFONT     s_fontBold = nullptr;
HFONT     s_fontName = nullptr; // larger device header; freed on destroy
CheckRow  s_rows[kRows];

// Heartbeat freshness, tracked locally (same pattern as the main window:
// needs two samples to claim "advancing").
std::wstring s_freshEp;
int64_t      s_freshCalls = 0;
ULONGLONG    s_freshTick = 0;

HFONT ClMakeFont(bool bold) {
    HDC hdc = GetDC(nullptr);
    const int px = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, hdc);
    return CreateFontW(px, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void SetTextIfChanged(HWND h, const std::wstring& text) {
    wchar_t cur[2048] = {};
    GetWindowTextW(h, cur, ARRAYSIZE(cur));
    if (text != cur) {
        SetWindowTextW(h, text.c_str());
    }
}

std::wstring FormatCalls(int64_t calls) {
    wchar_t buf[64] = {};
    // Thousand separators for readability ("12,408").
    std::wstring s = std::to_wstring(calls < 0 ? 0 : calls);
    for (size_t i = s.size(); i > 3; i -= 3) {
        s.insert(i - 3, L",");
    }
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"%s", s.c_str());
    return buf;
}

// ---------------------------------------------------------------------------
// The eight checks
// ---------------------------------------------------------------------------

void BuildRows(const DiagSnapshot& snap, const DiagSpatialInfo& spatial,
               const std::wstring& dllPath) {
    for (int i = 0; i < kRows; ++i) {
        s_rows[i] = CheckRow{};
    }
    if (s_endpoint.empty()) {
        for (int i = 0; i < kRows; ++i) {
            s_rows[i].state = CheckState::Idle;
            s_rows[i].detail = L"No device selected \u2014 pick an output device "
                              L"in the main window.";
        }
        s_rows[0].title = L"APO registered";
        s_rows[1].title = L"DLL present";
        s_rows[2].title = L"Attached to this device";
        s_rows[3].title = L"Audio enhancements";
        s_rows[4].title = L"Spatial sound";
        s_rows[5].title = L"Exclusive-mode apps";
        s_rows[6].title = L"Audio playing on this device";
        s_rows[7].title = L"APO processing audio";
        s_rows[8].title = L"App \u2194 APO link";
        return;
    }
    const std::wstring dev =
        s_deviceName.empty() ? L"this device" : s_deviceName;

    // 1 -- COM registration.
    s_rows[0].title = L"APO registered";
    if (!dllPath.empty()) {
        s_rows[0].state = CheckState::Ok;
        s_rows[0].detail = L"MiniEQ_APO.dll is registered with Windows.";
    } else {
        s_rows[0].state = CheckState::Error;
        s_rows[0].detail = L"MiniEQ_APO.dll is not registered with Windows.";
        s_rows[0].fix = L"Fix: reinstall MiniEQ from the MSI.";
    }

    // 2 -- DLL file on disk.
    s_rows[1].title = L"DLL present";
    if (!dllPath.empty() && snap.dllExists) {
        s_rows[1].state = CheckState::Ok;
        s_rows[1].detail = dllPath + L"\r\nexists.";
    } else if (!dllPath.empty()) {
        s_rows[1].state = CheckState::Error;
        s_rows[1].detail = L"Registered, but the file is missing:\r\n" + dllPath;
        s_rows[1].fix = L"Fix: reinstall MiniEQ from the MSI.";
    } else {
        s_rows[1].state = CheckState::Error;
        s_rows[1].detail = L"No registered path \u2014 see the row above.";
    }

    // 3 -- SFX slot attachment.
    s_rows[2].title = L"Attached to this device";
    if (snap.attached) {
        s_rows[2].state = CheckState::Ok;
        s_rows[2].detail = L"SFX slot points at MiniEQ_APO.";
    } else {
        s_rows[2].state = CheckState::Idle;
        s_rows[2].detail = L"Not attached \u2014 click \u201CAttach to this device\u201D "
                           L"in the main window (one admin approval).";
    }

    // 4 -- Audio Enhancements switch.
    s_rows[3].title = L"Audio enhancements";
    const bool enhNote = s_fixNoteRow == 3 && s_fixNote == FixNote::Reloaded &&
        (GetTickCount64() - s_fixNoteTick < kFixNoteMs);
    if (snap.enhancements == DiagEnhancements::On) {
        s_rows[3].state = CheckState::Ok;
        s_rows[3].detail = enhNote
            ? L"Device Default Effects \u2014 change applied, path reloaded (format flip)."
            : L"Device Default Effects \u2014 system effects are allowed.";
    } else if (snap.enhancements == DiagEnhancements::Off) {
        s_rows[3].state = CheckState::Error;
        s_rows[3].detail = L"Off \u2014 Windows skips every APO, MiniEQ included.";
        if (s_enhFixFailed) {
            s_rows[3].fix = L"Couldn't switch it automatically \u2014 turn it on "
                            L"manually: Settings \u2192 System \u2192 Sound \u2192 "
                            L"Audio enhancements \u2192 \u201CDevice Default Effects\u201D";
        } else {
            wchar_t fix[256] = {};
            StringCchPrintfW(fix, ARRAYSIZE(fix),
                L"Fix: Settings \u2192 System \u2192 Sound \u2192 %s \u2192 "
                L"Audio enhancements \u2192 \u201CDevice Default Effects\u201D",
                dev.c_str());
            s_rows[3].fix = fix;
        }
    } else {
        s_rows[3].state = CheckState::Idle;
        wchar_t detail[256] = {};
        StringCchPrintfW(detail, ARRAYSIZE(detail),
            L"Couldn't read the setting \u2014 check manually:\r\n"
            L"Settings \u2192 System \u2192 Sound \u2192 %s \u2192 Audio enhancements",
            dev.c_str());
        s_rows[3].detail = detail;
    }

    // 5 -- Spatial sound.
    s_rows[4].title = L"Spatial sound";
    const bool spatNote = s_fixNoteRow == 4 &&
        (s_fixNote == FixNote::Watching || s_fixNote == FixNote::AppliedLive ||
         s_fixNote == FixNote::Reloaded) &&
        (GetTickCount64() - s_fixNoteTick < kFixNoteMs);
    if (spatial.state == DiagSpatial::Off) {
        s_rows[4].state = CheckState::Ok;
        if (spatNote) {
            switch (s_fixNote) {
            case FixNote::Watching:
                s_rows[4].detail = L"Off \u2014 change applied, waiting for the audio path to rebuild\u2026";
                break;
            case FixNote::AppliedLive:
                s_rows[4].detail = L"Off \u2014 change applied live, no reload needed.";
                break;
            case FixNote::Reloaded:
                s_rows[4].detail = L"Off \u2014 change applied, path reloaded (format flip).";
                break;
            default:
                s_rows[4].detail = L"Off.";
                break;
            }
        } else {
            s_rows[4].detail = L"Off.";
        }
    } else if (spatial.state == DiagSpatial::On) {
        s_rows[4].state = CheckState::Error;
        const std::wstring name =
            spatial.name.empty() ? L"A spatial mode" : spatial.name;
        s_rows[4].detail = name + L" is on \u2014 the spatial graph can starve "
                           L"MiniEQ of audio.";
        if (s_fixNote == FixNote::Switching && s_fixNoteRow == 4) {
            s_rows[4].detail = name + L" is on \u2014 switching off\u2026";
        }
        if (s_spatialFixFailed) {
            s_rows[4].fix = L"Couldn't switch it automatically \u2014 turn it off "
                            L"manually: Settings \u2192 System \u2192 Sound \u2192 "
                            L"Spatial sound \u2192 Off, then replay";
        } else {
            wchar_t fix[256] = {};
            StringCchPrintfW(fix, ARRAYSIZE(fix),
                L"Fix: Settings \u2192 System \u2192 Sound \u2192 %s \u2192 "
                L"Spatial sound \u2192 Off, then replay",
                dev.c_str());
            s_rows[4].fix = fix;
        }
    } else {
        s_rows[4].state = CheckState::Idle;
        wchar_t detail[256] = {};
        StringCchPrintfW(detail, ARRAYSIZE(detail),
            L"Couldn't determine the spatial mode \u2014 check manually:\r\n"
            L"Settings \u2192 System \u2192 Sound \u2192 %s \u2192 Spatial sound",
            dev.c_str());
        s_rows[4].detail = detail;
    }

    // 6 -- Exclusive-mode apps: an app holding this endpoint exclusively
    // bypasses the engine (and every APO) by Windows design -- the one
    // bypass no MiniEQ setting can fix, so it gets its own row with the
    // in-app fix. Probed with a shared-mode IAudioClient::Initialize;
    // AUDCLNT_E_DEVICE_IN_USE means an exclusive holder is present.
    s_rows[5].title = L"Exclusive-mode apps";
    if (snap.exclusiveHeld) {
        s_rows[5].state = CheckState::Error;
        s_rows[5].detail = L"An app is holding this device in WASAPI exclusive mode "
                           L"\u2014 Windows sends its audio straight to the driver, "
                           L"bypassing the engine and every APO, MiniEQ included. "
                           L"No setting in MiniEQ can intercept it.";
        s_rows[5].fix = L"In that app, switch its output from \u201Cexclusive\u201D "
                        L"to \u201Cshared\u201D (Qobuz: Settings \u2192 Audio \u2192 "
                        L"WASAPI shared; Tidal/Roon/Foobar2000 have the same toggle), "
                        L"then replay.";
    } else {
        s_rows[5].state = CheckState::Ok;
        s_rows[5].detail = L"No app is holding this device in exclusive mode \u2014 "
                           L"all shared-mode audio flows through the engine.";
    }

    // 7 -- Playback on this endpoint.
    s_rows[6].title = L"Audio playing on this device";
    {
        const DiagSessionInfo* first = nullptr;
        for (const DiagSessionInfo& si : snap.sessions) {
            if (si.active) { first = &si; break; }
        }
        if (first != nullptr) {
            s_rows[6].state = CheckState::Ok;
            wchar_t detail[192] = {};
            if (first->peak >= 0.0f) {
                StringCchPrintfW(detail, ARRAYSIZE(detail),
                    L"%s is active (peak %d%%).", first->exe.c_str(),
                    (int)(first->peak * 100.0f + 0.5f));
            } else {
                StringCchPrintfW(detail, ARRAYSIZE(detail),
                    L"%s is active.", first->exe.c_str());
            }
            s_rows[6].detail = detail;
        } else if (!snap.sessions.empty()) {
            s_rows[6].state = CheckState::Idle;
            s_rows[6].detail = L"Sessions are idle \u2014 play something on this device.";
        } else {
            s_rows[6].state = CheckState::Idle;
            s_rows[6].detail = L"Nothing is playing \u2014 play something on this device.";
        }
    }

    // 8 -- The heartbeat: APOProcess calls advancing.
    s_rows[7].title = L"APO processing audio";
    if (!MiniEQ_GlobalEnabledGet()) {
        // User choice, not a failure: the APO passes every buffer through
        // untouched and the heartbeat still advances, so the link below
        // stays green. Neutral (yellow) instead of red.
        s_rows[7].state = CheckState::Idle;
        s_rows[7].detail = L"MiniEQ is off \u2014 audio passing through unprocessed.";
    } else {
        bool fresh = false;
        if (s_freshEp != s_endpoint) {
            s_freshEp = s_endpoint;
            s_freshCalls = 0;
            s_freshTick = 0;
        }
        const ULONGLONG now = GetTickCount64();
        if (snap.statusChannelOk && snap.heartbeatCalls > 0) {
            if (s_freshCalls > 0 && snap.heartbeatCalls > s_freshCalls &&
                (now - s_freshTick) < 4000) {
                fresh = true;
            }
            s_freshCalls = snap.heartbeatCalls;
            s_freshTick = now;
        }
        if (fresh) {
            s_rows[7].state = CheckState::Ok;
            wchar_t detail[192] = {};
            if (snap.apoLocked && snap.apoChannels > 0) {
                StringCchPrintfW(detail, ARRAYSIZE(detail),
                    L"%s APOProcess calls and counting \u00B7 %d ch @ %d Hz.",
                    FormatCalls(snap.heartbeatCalls).c_str(),
                    snap.apoChannels, snap.apoSampleRate);
            } else {
                StringCchPrintfW(detail, ARRAYSIZE(detail),
                    L"%s APOProcess calls and counting.",
                    FormatCalls(snap.heartbeatCalls).c_str());
            }
            s_rows[7].detail = detail;
        } else if (!snap.anySessionActive) {
            s_rows[7].state = CheckState::Idle;
            s_rows[7].detail = L"Waiting for audio \u2014 the counter starts with playback.";
        } else if (snap.statusChannelOk && snap.heartbeatCalls > 0) {
            s_rows[7].state = CheckState::Idle;
            wchar_t detail[192] = {};
            StringCchPrintfW(detail, ARRAYSIZE(detail),
                L"%s calls so far \u2014 confirming they're advancing\u2026",
                FormatCalls(snap.heartbeatCalls).c_str());
            s_rows[7].detail = detail;
        } else {
            s_rows[7].state = CheckState::Error;
            s_rows[7].detail = L"0 APOProcess calls while audio is playing \u2014 "
                               L"sound is bypassing MiniEQ.";
            s_rows[7].fix = L"Fix the red items above, then replay the audio.";
        }
    }

    // 9 -- UI <-> APO shared-memory link.
    s_rows[8].title = L"App \u2194 APO link";
    if (snap.statusChannelOk) {
        s_rows[8].state = CheckState::Ok;
        s_rows[8].detail = L"Shared memory open \u2014 EQ settings are flowing to the APO.";
    } else {
        s_rows[8].state = CheckState::Idle;
        s_rows[8].detail = L"No heartbeat channel yet \u2014 it appears once the APO "
                           L"processes its first buffer.";
    }
}

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------

int RowHeight(const CheckRow& r) {
    int h = 20 + 4; // title + gap
    int lines = 1;
    for (wchar_t c : r.detail) {
        if (c == L'\n') ++lines;
    }
    h += lines * 16;
    if (r.state == CheckState::Error && !r.fix.empty()) {
        h += 4 + 16;
    }
    return h + 8; // bottom pad
}

void LayoutRows() {
    int y = 62;
    auto place = [&](HWND h, int x, int w, int hh) {
        MoveWindow(h, x, y, w, hh, TRUE);
    };
    const int sectH = 22;

    place(s_hSect[0], 14, 472, sectH); y += sectH + 4;
    for (int i = 0; i < 3; ++i) {
        const int rh = RowHeight(s_rows[i]);
        MoveWindow(s_hDot[i], 16, y + 2, 18, 18, TRUE);
        MoveWindow(s_hTitle[i], 38, y, 448, 20, TRUE);
        int dy = y + 24;
        int lines = 1;
        for (wchar_t c : s_rows[i].detail) {
            if (c == L'\n') ++lines;
        }
        MoveWindow(s_hDetail[i], 38, dy, 448, lines * 16, TRUE);
        dy += lines * 16 + 4;
        const bool showFix = (s_rows[i].state == CheckState::Error &&
                              !s_rows[i].fix.empty());
        MoveWindow(s_hFix[i], 38, dy, 448, 16, TRUE);
        ShowWindow(s_hFix[i], showFix ? SW_SHOW : SW_HIDE);
        y += rh;
    }
    place(s_hSect[1], 14, 472, sectH); y += sectH + 4;
    for (int i = 3; i < 5; ++i) {
        int rh = RowHeight(s_rows[i]);
        // One-click fix buttons under the fix line, while the row is red:
        // "Turn on" for audio enhancements (index 3), "Turn off" for
        // spatial sound (index 4).
        HWND hFixBtn = nullptr;
        if (i == 3 && s_rows[i].state == CheckState::Error) {
            hFixBtn = s_hEnhFix;
        } else if (i == 4 && s_rows[i].state == CheckState::Error) {
            hFixBtn = s_hSpatialFix;
        }
        if (hFixBtn != nullptr) {
            rh += 34;
        }
        MoveWindow(s_hDot[i], 16, y + 2, 18, 18, TRUE);
        MoveWindow(s_hTitle[i], 38, y, 448, 20, TRUE);
        int dy = y + 24;
        int lines = 1;
        for (wchar_t c : s_rows[i].detail) {
            if (c == L'\n') ++lines;
        }
        MoveWindow(s_hDetail[i], 38, dy, 448, lines * 16, TRUE);
        dy += lines * 16 + 4;
        const bool showFix = (s_rows[i].state == CheckState::Error &&
                              !s_rows[i].fix.empty());
        MoveWindow(s_hFix[i], 38, dy, 448, 16, TRUE);
        ShowWindow(s_hFix[i], showFix ? SW_SHOW : SW_HIDE);
        if (hFixBtn != nullptr) {
            MoveWindow(hFixBtn, 38, dy + 16 + 6, 110, 26, TRUE);
            ShowWindow(hFixBtn, SW_SHOW);
        }
        if (i == 3 && hFixBtn != s_hEnhFix) {
            ShowWindow(s_hEnhFix, SW_HIDE);
        }
        if (i == 4 && hFixBtn != s_hSpatialFix) {
            ShowWindow(s_hSpatialFix, SW_HIDE);
        }
        y += rh;
    }
    place(s_hSect[2], 14, 472, sectH); y += sectH + 4;
    for (int i = 5; i < 9; ++i) {
        const int rh = RowHeight(s_rows[i]);
        MoveWindow(s_hDot[i], 16, y + 2, 18, 18, TRUE);
        MoveWindow(s_hTitle[i], 38, y, 448, 20, TRUE);
        int dy = y + 24;
        int lines = 1;
        for (wchar_t c : s_rows[i].detail) {
            if (c == L'\n') ++lines;
        }
        MoveWindow(s_hDetail[i], 38, dy, 448, lines * 16, TRUE);
        dy += lines * 16 + 4;
        const bool showFix = (s_rows[i].state == CheckState::Error &&
                              !s_rows[i].fix.empty());
        MoveWindow(s_hFix[i], 38, dy, 448, 16, TRUE);
        ShowWindow(s_hFix[i], showFix ? SW_SHOW : SW_HIDE);
        y += rh;
    }
    // Recovery: three-way toggle (Reload path / Re-attach / Watch engine).
    MoveWindow(s_hSect[3], 14, y, 472, sectH, TRUE); y += sectH + 6;
    MoveWindow(s_hRecSeg, 14, y, 472, 32, TRUE); y += 32 + 6;
    MoveWindow(s_hRecDesc, 14, y, 472, 40, TRUE); y += 40 + 4;
    MoveWindow(s_hRecResult, 14, y, 472, 34, TRUE); y += 34 + 4;
    MoveWindow(s_hLegend, 14, y + 6, 300, 18, TRUE);
    MoveWindow(GetDlgItem(s_hDlg, IDC_CL_REFRESH), 316, y + 2, 80, 26, TRUE);
    MoveWindow(GetDlgItem(s_hDlg, IDC_CL_CLOSE), 404, y + 2, 80, 26, TRUE);

    // Rows move between refreshes: repaint the whole client area so a
    // shrunken row leaves no ghost text behind.
    InvalidateRect(s_hDlg, nullptr, TRUE);

    // Grow the window if the content needs more room (long device names,
    // two-line details); never shrink below the designed size.
    RECT rc = {};
    GetWindowRect(s_hDlg, &rc);
    const int needClient = y + 40;
    RECT want = { 0, 0, 500, needClient > 700 ? needClient : 700 };
    AdjustWindowRect(&want, (DWORD)GetWindowLongW(s_hDlg, GWL_STYLE), FALSE);
    const int wantH = want.bottom - want.top;
    if ((rc.bottom - rc.top) < wantH) {
        SetWindowPos(s_hDlg, nullptr, 0, 0, rc.right - rc.left, wantH,
                     SWP_NOMOVE | SWP_NOZORDER);
    }
}

void RefreshChecklist() {
    if (s_hDlg == nullptr) {
        return;
    }
    // Expire the one-click-fix note (Switching/Watching are driven by the
    // worker and the timer, not by time alone).
    if (s_fixNote != FixNote::None && s_fixNote != FixNote::Switching &&
        s_fixNote != FixNote::Watching &&
        (GetTickCount64() - s_fixNoteTick >= kFixNoteMs)) {
        s_fixNote = FixNote::None;
        s_fixNoteRow = -1;
    }
    const std::wstring dllPath = MiniEQ_ApoDllPath();
    const DiagSnapshot snap = MiniEQ_RunDiagnosis(s_endpoint);
    s_lastAudiodgPid = snap.audiodgPid; // feeds the recovery watch
    const DiagSpatialInfo spatial = MiniEQ_ReadSpatialSound(s_endpoint);
    BuildRows(snap, spatial, dllPath);

    for (int i = 0; i < kRows; ++i) {
        SetTextIfChanged(s_hDot[i], L"\u25CF");
        SetTextIfChanged(s_hTitle[i], s_rows[i].title);
        SetTextIfChanged(s_hDetail[i], s_rows[i].detail);
        SetTextIfChanged(s_hFix[i], s_rows[i].fix);
        InvalidateRect(s_hDot[i], nullptr, TRUE);
        InvalidateRect(s_hDetail[i], nullptr, TRUE);
        InvalidateRect(s_hFix[i], nullptr, TRUE);
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t sub[160] = {};
    StringCchPrintfW(sub, ARRAYSIZE(sub),
        L"%s \u00B7 checked %02u:%02u:%02u \u2014 Refresh re-runs every check",
        s_deviceName.empty() ? L"(no device)" : s_deviceName.c_str(),
        st.wHour, st.wMinute, st.wSecond);
    SetTextIfChanged(s_hSub, sub);

    LayoutRows();
}

LRESULT ClOnCtlColorStatic(HDC hdc, HWND hctl) {
    for (int i = 0; i < kRows; ++i) {
        if (hctl == s_hDot[i]) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, kDotColor[(int)s_rows[i].state]);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        if (hctl == s_hDetail[i]) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(90, 90, 90));
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        if (hctl == s_hFix[i]) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(143, 29, 29));
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
    }
    if (hctl == s_hSub || hctl == s_hLegend || hctl == s_hRecDesc) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(110, 110, 110));
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    for (int s = 0; s < 4; ++s) {
        if (hctl == s_hSect[s]) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(120, 120, 120));
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
    }
    return 0;
}

void ClOnCreate(HWND hwnd) {
    s_font = ClMakeFont(false);
    s_fontBold = ClMakeFont(true);

    auto makeStatic = [&](int id, const wchar_t* text, int x, int y, int w,
                          int h, bool bold) -> HWND {
        HWND ctl = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 x, y, w, h, hwnd, (HMENU)(INT_PTR)id,
                                 s_hInst, nullptr);
        SendMessageW(ctl, WM_SETFONT, (WPARAM)(bold ? s_fontBold : s_font), TRUE);
        return ctl;
    };
    auto makeButton = [&](int id, const wchar_t* text) -> HWND {
        HWND ctl = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 80, 26, hwnd, (HMENU)(INT_PTR)id,
                                 s_hInst, nullptr);
        SendMessageW(ctl, WM_SETFONT, (WPARAM)s_font, TRUE);
        return ctl;
    };

    s_hDevName = makeStatic(IDC_CL_DEVNAME,
        s_deviceName.empty() ? L"(no device)" : s_deviceName.c_str(),
        14, 10, 472, 26, true);
    // Larger device header, like the main window. The control uses the
    // HFONT handle directly, so it must outlive the dialog.
    s_fontName = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(s_hDevName, WM_SETFONT, (WPARAM)s_fontName, TRUE);

    s_hSub = makeStatic(IDC_CL_SUB, L"", 14, 38, 472, 18, false);

    const wchar_t* sects[3] = {
        L"INSTALL & REGISTRATION", L"WINDOWS SETTINGS", L"LIVE AUDIO PATH"
    };
    for (int s = 0; s < 3; ++s) {
        s_hSect[s] = makeStatic(IDC_CL_SECT1 + s, sects[s], 14, 0, 472, 22, false);
    }

    for (int i = 0; i < kRows; ++i) {
        s_hDot[i] = makeStatic(IDC_CL_DOT0 + i, L"\u25CF", 16, 0, 18, 18, true);
        s_hTitle[i] = makeStatic(IDC_CL_TITLE0 + i, L"", 38, 0, 448, 20, true);
        s_hDetail[i] = makeStatic(IDC_CL_DETAIL0 + i, L"", 38, 0, 448, 32, false);
        s_hFix[i] = makeStatic(IDC_CL_FIX0 + i, L"", 38, 0, 448, 16, false);
    }

    s_hLegend = makeStatic(IDC_CL_LEGEND,
        L"\u25CF working    \u25CF not active yet    \u25CF error",
        14, 0, 300, 18, false);

    makeButton(IDC_CL_REFRESH, L"Refresh");
    makeButton(IDC_CL_CLOSE, L"Close");
    // One-click fix for the spatial-sound row: visible only while spatial
    // is On (row 4 in Error). LayoutRows positions it.
    s_hSpatialFix = makeButton(IDC_CL_SPATIALFIX, L"Turn off");
    ShowWindow(s_hSpatialFix, SW_HIDE);
    // One-click fix for the audio-enhancements row: visible only while
    // enhancements are Off (row 3 in Error). LayoutRows positions it.
    s_hEnhFix = makeButton(IDC_CL_ENHFIX, L"Turn on");
    ShowWindow(s_hEnhFix, SW_HIDE);

    // Recovery: three-way toggle. LayoutRows positions everything.
    s_hSect[3] = makeStatic(IDC_CL_SECT4, L"RECOVERY", 14, 0, 472, 22, false);
    s_hRecSeg = CreateWindowW(L"STATIC", L"",
                              WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY,
                              0, 0, 472, 32, hwnd,
                              (HMENU)(INT_PTR)IDC_CL_RECSEG, s_hInst, nullptr);
    s_hRecDesc = makeStatic(IDC_CL_RECDESC, kRecDesc[0], 14, 0, 472, 40, false);
    s_hRecResult = makeStatic(IDC_CL_RECSLT, L"", 14, 0, 472, 34, false);

    SetTimer(hwnd, 1, 1500, nullptr);
    RefreshChecklist();
}

// ---------------------------------------------------------------------------
// Recovery toggle implementation
// ---------------------------------------------------------------------------

static void UpdateRecoveryTexts() {
    if (s_hRecDesc == nullptr) {
        return;
    }
    SetTextIfChanged(s_hRecDesc, kRecDesc[(int)s_recSel]);
    SetTextIfChanged(s_hRecResult, s_recResultText);
    if (s_hRecSeg != nullptr) {
        InvalidateRect(s_hRecSeg, nullptr, TRUE);
    }
}

struct RecoveryJob {
    HWND hwnd;
    std::wstring endpoint;
    int action; // 0 = reload, 1 = re-attach (slot already re-written), 2 = watch auto-recovery
    int seq;
};

struct RecoveryDone {
    int seq;
    int action;
    bool ok;
    std::wstring text;
};

// Strong verification: the heartbeat must ADVANCE, not just exist. Returns
// 0 = advancing (MiniEQ is processing), 1 = path rebuilt but nothing is
// playing (can't prove it yet), 2 = still no heartbeat.
static int VerifyHeartbeatAdvance(const std::wstring& endpoint) {
    if (!MiniEQ_AudioPlaying(endpoint, nullptr)) {
        return 1;
    }
    DiagSnapshot s0 = MiniEQ_RunDiagnosis(endpoint);
    int64_t base = s0.statusChannelOk ? s0.heartbeatCalls : 0;
    for (int i = 0; i < 12; ++i) {
        Sleep(1000);
        DiagSnapshot s = MiniEQ_RunDiagnosis(endpoint);
        if (s.statusChannelOk && s.heartbeatCalls > base) {
            // One advancing sample isn't enough (the row-8 freshness rule
            // needs two); confirm with a second.
            const int64_t mid = s.heartbeatCalls;
            Sleep(1000);
            DiagSnapshot s2 = MiniEQ_RunDiagnosis(endpoint);
            if (s2.heartbeatCalls > mid) {
                return 0;
            }
            base = mid;
        }
    }
    return 2;
}

static DWORD WINAPI RecoveryThread(LPVOID param) {
    RecoveryJob* job = static_cast<RecoveryJob*>(param);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring detail;
    RecoveryDone* done = new RecoveryDone{ job->seq, job->action, false, L"" };
    const wchar_t* kind = job->action == 1 ? L"re-attach"
                        : job->action == 2 ? L"watch-recover" : L"reload";
    if (!MiniEQ_FlipDefaultFormat(job->endpoint, &detail)) {
        done->text = L"Couldn't rebuild the audio path (" + detail + L").";
        MiniEQ_AppLogCat(L"ENGINE", L"recovery %s: flip failed (%s)",
                         kind, detail.c_str());
    } else {
        const int v = VerifyHeartbeatAdvance(job->endpoint);
        if (v == 0) {
            done->ok = true;
            done->text = job->action == 1
                ? L"Re-attached and reloaded \u2014 heartbeat advancing, MiniEQ is processing audio."
                : L"Reloaded \u2014 heartbeat advancing, MiniEQ is processing audio.";
        } else if (v == 1) {
            done->ok = true;
            done->text = L"Path rebuilt, but nothing is playing \u2014 play audio on this device to confirm.";
        } else {
            done->text = L"Path rebuilt, but still no heartbeat. The rebuild may not "
                         L"take effect on this device \u2014 try unplugging and "
                         L"replugging the earphone, then replay.";
        }
        MiniEQ_AppLogCat(L"ENGINE", L"recovery %s: %s", kind, done->text.c_str());
    }
    CoUninitialize();
    if (!PostMessageW(job->hwnd, WM_APP_RECOVERYDONE, 0, (LPARAM)done)) {
        delete done; // dialog already gone
    }
    delete job;
    return 0;
}

static void SpawnRecoveryJob(int action) {
    s_recBusy = true;
    ++s_recSeq;
    RecoveryJob* job = new RecoveryJob{ s_hDlg, s_endpoint, action, s_recSeq };
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, 0, RecoveryThread, job, 0, &tid);
    if (h != nullptr) {
        CloseHandle(h);
    } else {
        delete job;
        s_recBusy = false;
        s_recResultText = L"Couldn't start the recovery worker.";
        UpdateRecoveryTexts();
    }
}

// Re-writes the SFX slot. The FxProperties value lives under HKLM, so this
// needs elevation -- same self-relaunch pattern as the main window's
// "Attach to this device" (UAC prompt, --attach helper). Runs on the UI
// thread, like the existing attach flow.
static bool RelaunchElevatedReattach(const std::wstring& endpoint) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::wstring args = L"--attach \"";
    args += endpoint;
    args += L"\"";
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_NORMAL;
    if (!ShellExecuteExW(&sei)) {
        return false; // elevation cancelled
    }
    Sleep(800); // give the elevated helper a moment, then re-read
    bool attached = false;
    return SUCCEEDED(MiniEQ_IsAttachedToEndpoint(endpoint.c_str(), &attached)) &&
           attached;
}

static void OnRecoveryTap(int index) {
    if (s_recBusy || s_hDlg == nullptr || s_endpoint.empty()) {
        return;
    }
    if (index == 2) {
        // Watch engine: toggle the armed state.
        s_recSel = RecoverySel::Watch;
        s_recWatchArmed = !s_recWatchArmed;
        if (s_recWatchArmed) {
            s_recWatchPid = s_lastAudiodgPid; // baseline; never fires on arm
            s_recWatchCount = 0;
            s_recWatchLastMs = 0;
            s_recResultText =
                L"Watching the audio engine \u2014 MiniEQ will re-establish "
                L"itself if the engine restarts.";
            MiniEQ_AppLogCat(L"ENGINE", L"recovery watch armed (audiodg pid %u)",
                             s_recWatchPid);
        } else {
            s_recResultText = L"Watch stopped.";
            MiniEQ_AppLogCat(L"ENGINE", L"recovery watch disarmed");
        }
        UpdateRecoveryTexts();
        return;
    }
    s_recSel = static_cast<RecoverySel>(index);
    if (index == 1) {
        s_recResultText =
            L"Requesting admin approval to re-write the effects-chain slot\u2026";
        UpdateRecoveryTexts();
        if (!RelaunchElevatedReattach(s_endpoint)) {
            s_recResultText =
                L"Admin approval was cancelled or the slot couldn't be re-written "
                L"\u2014 MiniEQ's registration is unchanged.";
            MiniEQ_AppLogCat(L"ENGINE", L"recovery re-attach: elevation cancelled/failed");
            UpdateRecoveryTexts();
            return;
        }
    }
    s_recResultText = L"Working\u2026 rebuilding the audio path.";
    UpdateRecoveryTexts();
    SpawnRecoveryJob(index);
}

// Called from the 1.5 s timer while the watch is armed: a changed audiodg
// pid means the engine restarted on its own (we never restart it). One
// format-flip recovery per new pid, 30 s cooldown, auto-disarm after 5 so
// a crash loop can't churn forever.
static void CheckWatchEngine() {
    if (!s_recWatchArmed || s_endpoint.empty() || s_recBusy) {
        return;
    }
    const DWORD pid = s_lastAudiodgPid;
    if (pid == 0) {
        return;
    }
    if (s_recWatchPid == 0) {
        s_recWatchPid = pid; // late baseline
        return;
    }
    if (pid == s_recWatchPid) {
        return;
    }
    wchar_t changed[128] = {};
    StringCchPrintfW(changed, ARRAYSIZE(changed),
                     L"Engine restarted (pid %u \u2192 %u)", s_recWatchPid, pid);
    s_recWatchPid = pid;
    if (s_recWatchCount >= kWatchMaxRecoveries) {
        s_recWatchArmed = false;
        s_recResultText =
            L"Engine restarted 5 times \u2014 auto-recovery stopped to avoid churn. "
            L"Check Event Viewer for audiodg.exe crashes.";
        MiniEQ_AppLogCat(L"ENGINE", L"recovery watch: auto-disarmed after 5 recoveries");
        UpdateRecoveryTexts();
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - s_recWatchLastMs < kWatchCooldownMs) {
        MiniEQ_AppLogCat(L"ENGINE", L"recovery watch: engine restarted inside cooldown, waiting");
        return;
    }
    s_recWatchLastMs = now;
    ++s_recWatchCount;
    s_recResultText = std::wstring(changed) + L" \u2014 re-establishing MiniEQ\u2026";
    MiniEQ_AppLogCat(L"ENGINE", L"recovery watch: %s (recovery %d of %d)",
                     changed, s_recWatchCount, kWatchMaxRecoveries);
    UpdateRecoveryTexts();
    SpawnRecoveryJob(2);
}

// Owner-drawn 3-segment control, drawn to match the light dialog: gray
// track, white pill on the selected segment, gray labels otherwise.
static void DrawRecoverySeg(const DRAWITEMSTRUCT* di) {
    HDC hdc = di->hDC;
    const RECT rc = di->rcItem;
    const int w = rc.right - rc.left;
    const int sel = static_cast<int>(s_recSel);
    const int segW = w / 3;

    HBRUSH track = CreateSolidBrush(RGB(227, 227, 227));
    HBRUSH pill = CreateSolidBrush(RGB(255, 255, 255));
    HPEN edge = CreatePen(PS_SOLID, 1, RGB(198, 198, 198));
    HGDIOBJ oldBrush = SelectObject(hdc, track);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 12, 12);

    RECT pr = { rc.left + sel * segW + 2, rc.top + 2,
                rc.left + (sel + 1) * segW - 2, rc.bottom - 2 };
    SelectObject(hdc, pill);
    SelectObject(hdc, edge);
    RoundRect(hdc, pr.left, pr.top, pr.right, pr.bottom, 10, 10);

    const wchar_t* labels[3] = { L"Reload path", L"Re-attach", L"Watch engine" };
    SetBkMode(hdc, TRANSPARENT);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, s_font));
    for (int i = 0; i < 3; ++i) {
        RECT lr = { rc.left + i * segW, rc.top, rc.left + (i + 1) * segW, rc.bottom };
        if (i == static_cast<int>(RecoverySel::Watch) && s_recWatchArmed) {
            lr.right -= 14; // room for the armed dot
        }
        SetTextColor(hdc, i == sel ? RGB(26, 26, 26) : RGB(110, 110, 110));
        DrawTextW(hdc, labels[i], -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (s_recWatchArmed) {
        // Green "armed" dot at the right edge of the Watch segment.
        const int cx = rc.left + 2 * segW + segW - 11;
        const int cy = (rc.top + rc.bottom) / 2;
        HBRUSH dot = CreateSolidBrush(RGB(29, 158, 75));
        HGDIOBJ oldB = SelectObject(hdc, dot);
        SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, cx - 4, cy - 4, cx + 4, cy + 4);
        SelectObject(hdc, oldB);
        DeleteObject(dot);
    }
    SelectObject(hdc, oldFont);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(track);
    DeleteObject(pill);
    DeleteObject(edge);
}

LRESULT CALLBACK ClWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        s_hDlg = hwnd;
        ClOnCreate(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            RefreshChecklist();
            CheckWatchEngine();
            // Live-apply watch: after the WinRT switch the APO heartbeat
            // has to prove the running graph picked it up. If it heals on
            // its own -- the Dolby-level path -- no reload is needed.
            if (s_fixNote == FixNote::Watching && s_fixNoteRow >= 0 &&
                s_fixNoteRow < kRows) {
                // "Applied live" needs both halves of the proof: the
                // endpoint reads back as spatial Off (the switch really
                // took) and the heartbeat is advancing (the running graph
                // picked it up with no reload).
                if (s_rows[4].state == CheckState::Ok &&
                    s_rows[7].state == CheckState::Ok) {
                    s_fixNote = FixNote::AppliedLive;
                    s_fixNoteTick = GetTickCount64();
                    MiniEQ_AppLogCat(L"UI", L"checklist fix applied live, audio path healed");
                    RefreshChecklist();
                } else if (GetTickCount64() - s_fixNoteTick >= kFixWatchMs) {
                    if (s_rows[6].state == CheckState::Ok) {
                        // Audio is playing but the old graph is still
                        // alive: fall back to the chained format flip.
                        MiniEQ_AppLogCat(L"ENGINE", L"checklist live fix timed out with audio playing, chaining reload");
                        ChainReloadFlip(s_fixNoteRow);
                    } else {
                        // Nothing playing: nothing to heal, the change is
                        // in place for the next stream.
                        MiniEQ_AppLogCat(L"ENGINE", L"checklist fix applied while idle, no reload needed");
                        s_fixNote = FixNote::AppliedLive;
                        s_fixNoteTick = GetTickCount64();
                    }
                    RefreshChecklist();
                }
            }
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CL_RECSEG && HIWORD(wp) == STN_CLICKED) {
            // Recovery toggle tap: hit-test which third was tapped.
            POINT pt = {};
            GetCursorPos(&pt);
            ScreenToClient(s_hRecSeg, &pt);
            RECT rc = {};
            GetClientRect(s_hRecSeg, &rc);
            const int w = max(rc.right - rc.left, 1);
            int idx = (pt.x * 3) / w;
            if (idx < 0) idx = 0;
            if (idx > 2) idx = 2;
            OnRecoveryTap(idx);
            return 0;
        }
        switch (LOWORD(wp)) {
        case IDC_CL_REFRESH:
            // Reset freshness baselines so "confirming they're advancing"
            // re-verifies from this instant.
            s_freshCalls = 0;
            s_freshTick = 0;
            RefreshChecklist();
            MiniEQ_AppLogCat(L"UI", L"checklist refreshed");
            return 0;
        case IDC_CL_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case IDC_CL_SPATIALFIX: {
            // One-click fix, Dolby-level: switch spatial off through the
            // public WinRT API -- the Sound settings page's own channel --
            // on a worker thread, so the audio service rebuilds the running
            // graph immediately with no service restart at all. The heartbeat
            // watch confirms the path healed; if the WinRT call fails we
            // fall back to the direct write plus a chained format flip.
            s_spatialFixFailed = false;
            s_fixNote = FixNote::Switching;
            s_fixNoteRow = 4;
            s_fixNoteTick = GetTickCount64();
            ++s_fixSeq;
            EnableWindow(s_hSpatialFix, FALSE);
            SpatialFixCtx* ctx = new SpatialFixCtx{ s_hDlg, s_endpoint, s_fixSeq };
            MiniEQ_AppLogCat(L"UI", L"spatial turn-off: trying WinRT live switch");
            DWORD tid = 0;
            HANDLE hThread = CreateThread(nullptr, 0, SpatialFixThread, ctx, 0, &tid);
            if (hThread != nullptr) {
                CloseHandle(hThread);
            } else {
                // Thread creation failed: fall back synchronously.
                delete ctx;
                EnableWindow(s_hSpatialFix, TRUE);
                s_fixNote = FixNote::None;
                s_fixNoteRow = -1;
                const bool ok = MiniEQ_SetSpatialSoundOff(s_endpoint);
                s_spatialFixFailed = !ok;
                if (ok) {
                    ChainReloadFlip(4);
                }
            }
            RefreshChecklist();
            return 0;
        }
        case IDC_CL_ENHFIX: {
            // One-click fix: switch enhancements back to device defaults
            // so the SysFx chain (MiniEQ's SFX APO) runs again, then flip
            // the format so the engine rebuilds the graph and the change
            // takes effect now.
            const bool ok = MiniEQ_SetAudioEnhancements(s_endpoint, true);
            s_enhFixFailed = !ok;
            MiniEQ_AppLogCat(L"UI", ok ? L"audio enhancements turned on from checklist"
                                       : L"checklist enhancements turn-on failed");
            if (ok) {
                ChainReloadFlip(3);
            }
            RefreshChecklist();
            return 0;
        }
        }
        break;
    case WM_DRAWITEM:
        if (wp == (WPARAM)IDC_CL_RECSEG) {
            DrawRecoverySeg((const DRAWITEMSTRUCT*)lp);
            return TRUE;
        }
        break;
    case WM_APP_RECOVERYDONE: {
        RecoveryDone* done = (RecoveryDone*)lp;
        if (s_open && done->seq == s_recSeq) {
            s_recBusy = false;
            s_recResultText = done->text;
            UpdateRecoveryTexts();
            RefreshChecklist(); // the path may have healed; re-run the rows
        }
        delete done;
        return 0;
    }
    case WM_APP_SPATIALDONE:
        if (s_open && (int)lp == s_fixSeq) {
            EnableWindow(s_hSpatialFix, TRUE);
            if (wp != 0) {
                MiniEQ_AppLogCat(L"UI", L"spatial turn-off: WinRT switch completed, watching path");
                BeginFixWatch(4);
            } else {
                MiniEQ_AppLogCat(L"UI", L"spatial turn-off: WinRT failed, direct write + reload");
                const bool ok = MiniEQ_SetSpatialSoundOff(s_endpoint);
                s_spatialFixFailed = !ok;
                if (ok) {
                    ChainReloadFlip(4);
                } else {
                    s_fixNote = FixNote::None;
                    s_fixNoteRow = -1;
                }
            }
            RefreshChecklist();
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        const LRESULT r = ClOnCtlColorStatic((HDC)wp, (HWND)lp);
        if (r != 0) {
            return r;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        DeleteObject(s_font);
        DeleteObject(s_fontBold);
        DeleteObject(s_fontName);
        s_font = nullptr;
        s_fontBold = nullptr;
        s_fontName = nullptr;
        s_hDlg = nullptr;
        s_open = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void MiniEQ_ShowChecklist(HINSTANCE hInst, HWND hParent,
                          const std::wstring& endpointId,
                          const std::wstring& deviceName) {
    if (s_open && s_hDlg != nullptr) {
        ShowWindow(s_hDlg, SW_SHOW);
        SetForegroundWindow(s_hDlg);
        RefreshChecklist();
        return;
    }
    s_hInst = hInst;
    s_endpoint = endpointId;
    s_deviceName = deviceName;
    s_spatialFixFailed = false;
    s_enhFixFailed = false;
    s_fixNote = FixNote::None;
    s_fixNoteRow = -1;
    s_fixNoteTick = 0;
    ++s_fixSeq; // invalidate any in-flight worker completion
    // Recovery toggle: fresh selection, disarmed watch, no stale results.
    s_recSel = RecoverySel::Reload;
    s_recBusy = false;
    s_recWatchArmed = false;
    s_recWatchPid = 0;
    s_lastAudiodgPid = 0;
    s_recWatchLastMs = 0;
    s_recWatchCount = 0;
    s_recResultText.clear();
    ++s_recSeq;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ClWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    MiniEQ_AppLogCat(L"UI", L"checklist opened for %s",
                     deviceName.empty() ? L"(no device)" : deviceName.c_str());

    HWND hDlg = CreateWindowExW(0, kClass, L"Audio Path Checklist",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, 510, 720,
                                hParent, nullptr, hInst, nullptr);
    if (hDlg == nullptr) {
        return;
    }
    // Modal: disable the parent and run a nested message loop.
    EnableWindow(hParent, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    s_open = true;
    MSG msg;
    while (s_open && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
}
