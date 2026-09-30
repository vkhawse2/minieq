// diag.cpp -- in-app diagnostics implementation.

#include "diag.h"

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <shlobj.h>
#include <stdarg.h>
#include <strsafe.h>

#include <vector>

//------------------------------------------------------------------------------
// Log file
//------------------------------------------------------------------------------

std::wstring MiniEQ_DiagLogPath() {
    wchar_t dir[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr,
                                   0, dir))) {
        return std::wstring(dir) + L"\\MiniEQ\\apo-trace.log";
    }
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, ARRAYSIZE(win));
    return std::wstring(win) + L"\\Temp\\MiniEQ-apo-trace.log";
}

void MiniEQ_EnsureLogDir() {
    wchar_t dir[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr,
                                0, dir))) {
        return;
    }
    std::wstring d = std::wstring(dir) + L"\\MiniEQ";
    CreateDirectoryW(d.c_str(), nullptr); // already exists: harmless
}

static void MiniEQ_AppLogV(const wchar_t* category, const wchar_t* fmt, va_list ap) {
    if (fmt == nullptr) {
        return;
    }
    wchar_t msg[1024] = {};
    StringCchVPrintfW(msg, ARRAYSIZE(msg), fmt, ap);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t line[1200] = {};
    StringCchPrintfW(line, ARRAYSIZE(line),
                     L"[%02u:%02u:%02u.%03u pid=%lu] [%s] %s\r\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     GetCurrentProcessId(),
                     (category != nullptr && *category != L'\0') ? category : L"UI",
                     msg);

    HANDLE h = CreateFileW(MiniEQ_DiagLogPath().c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(h, &size) && size.QuadPart == 0) {
        // Fresh file: UTF-16LE BOM, matching trace.cpp.
        static const BYTE bom[2] = { 0xFF, 0xFE };
        DWORD w = 0;
        WriteFile(h, bom, sizeof(bom), &w, nullptr);
    }
    DWORD w = 0;
    WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
}

void MiniEQ_AppLogCat(const wchar_t* category, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    MiniEQ_AppLogV(category, fmt, ap);
    va_end(ap);
}

void MiniEQ_AppLog(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    MiniEQ_AppLogV(L"UI", fmt, ap);
    va_end(ap);
}

//------------------------------------------------------------------------------
// Endpoint peak meter: is audio flowing right now?
//------------------------------------------------------------------------------

float MiniEQ_EndpointPeakLevel(const std::wstring& endpointId) {
    if (endpointId.empty()) {
        return -1.0f;
    }
    float peak = -1.0f;
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&pEnum))) {
        return -1.0f;
    }
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDevice(endpointId.c_str(), &pDev)) &&
        pDev != nullptr) {
        IAudioMeterInformation* pMeter = nullptr;
        if (SUCCEEDED(pDev->Activate(__uuidof(IAudioMeterInformation),
                                     CLSCTX_ALL, nullptr,
                                     (void**)&pMeter)) && pMeter != nullptr) {
            if (FAILED(pMeter->GetPeakValue(&peak))) {
                peak = -1.0f;
            }
            pMeter->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
    return peak;
}

//------------------------------------------------------------------------------
// Audio service restart (ELEVATED helper only)
//------------------------------------------------------------------------------

namespace {

bool WaitForServiceState(SC_HANDLE svc, DWORD want, DWORD timeoutMs) {
    SERVICE_STATUS ss = {};
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < end) {
        if (!QueryServiceStatus(svc, &ss)) {
            return false;
        }
        if (ss.dwCurrentState == want) {
            return true;
        }
        if (ss.dwCurrentState != SERVICE_STOP_PENDING &&
            ss.dwCurrentState != SERVICE_START_PENDING) {
            return false; // settled into some other state
        }
        DWORD wait = (ss.dwWaitHint != 0) ? ss.dwWaitHint / 10 : 200;
        if (wait < 100) {
            wait = 100;
        }
        if (wait > 1000) {
            wait = 1000;
        }
        Sleep(wait);
    }
    return false;
}

bool StopOneService(SC_HANDLE scm, const wchar_t* name) {
    SC_HANDLE svc =
        OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (svc == nullptr) {
        MiniEQ_AppLog(L"  open %s failed (%lu)", name, GetLastError());
        return false;
    }
    SERVICE_STATUS ss = {};
    bool ok = true;
    if (ControlService(svc, SERVICE_CONTROL_STOP, &ss)) {
        ok = WaitForServiceState(svc, SERVICE_STOPPED, 15000);
    } else {
        const DWORD e = GetLastError();
        ok = (e == ERROR_SERVICE_NOT_ACTIVE); // already stopped: fine
        if (!ok) {
            MiniEQ_AppLog(L"  stop %s failed (%lu)", name, e);
        }
    }
    CloseServiceHandle(svc);
    return ok;
}

bool StartOneService(SC_HANDLE scm, const wchar_t* name) {
    SC_HANDLE svc =
        OpenServiceW(scm, name, SERVICE_START | SERVICE_QUERY_STATUS);
    if (svc == nullptr) {
        MiniEQ_AppLog(L"  open %s failed (%lu)", name, GetLastError());
        return false;
    }
    bool ok = true;
    if (StartServiceW(svc, 0, nullptr)) {
        ok = WaitForServiceState(svc, SERVICE_RUNNING, 15000);
    } else {
        const DWORD e = GetLastError();
        ok = (e == ERROR_SERVICE_ALREADY_RUNNING); // already up: fine
        if (!ok) {
            MiniEQ_AppLog(L"  start %s failed (%lu)", name, e);
        }
    }
    CloseServiceHandle(svc);
    return ok;
}

} // namespace

bool MiniEQ_RestartAudioService() {
    MiniEQ_AppLog(L"restarting Windows Audio service...");
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        MiniEQ_AppLog(L"OpenSCManager failed (%lu)", GetLastError());
        return false;
    }
    SC_HANDLE audio =
        OpenServiceW(scm, L"Audiosrv",
                     SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS |
                     SERVICE_ENUMERATE_DEPENDENTS);
    if (audio == nullptr) {
        MiniEQ_AppLog(L"OpenService Audiosrv failed (%lu)", GetLastError());
        CloseServiceHandle(scm);
        return false;
    }

    // Active dependents must stop first (mirrors Restart-Service -Force).
    std::vector<std::wstring> dependents;
    DWORD needed = 0, count = 0;
    EnumDependentServicesW(audio, SERVICE_ACTIVE, nullptr, 0, &needed, &count);
    if (needed > 0) {
        std::vector<BYTE> buf(needed);
        if (EnumDependentServicesW(
                audio, SERVICE_ACTIVE,
                reinterpret_cast<ENUM_SERVICE_STATUSW*>(buf.data()), needed,
                &needed, &count)) {
            const ENUM_SERVICE_STATUSW* list =
                reinterpret_cast<const ENUM_SERVICE_STATUSW*>(buf.data());
            for (DWORD i = 0; i < count; ++i) {
                dependents.push_back(list[i].lpServiceName);
            }
        }
    }

    bool ok = true;
    for (const auto& d : dependents) {
        MiniEQ_AppLog(L"  stopping dependent %s...", d.c_str());
        ok = StopOneService(scm, d.c_str()) && ok;
    }
    MiniEQ_AppLog(L"  stopping Audiosrv...");
    ok = StopOneService(scm, L"Audiosrv") && ok;
    MiniEQ_AppLog(L"  starting Audiosrv...");
    ok = StartOneService(scm, L"Audiosrv") && ok;
    for (auto it = dependents.rbegin(); it != dependents.rend(); ++it) {
        MiniEQ_AppLog(L"  starting dependent %s...", it->c_str());
        ok = StartOneService(scm, it->c_str()) && ok;
    }
    CloseServiceHandle(audio);
    CloseServiceHandle(scm);
    MiniEQ_AppLog(L"audio service restart %s", ok ? L"done" : L"FAILED");
    return ok;
}

//------------------------------------------------------------------------------
// Live log viewer (modeless)
//------------------------------------------------------------------------------

namespace {

const wchar_t kLogWndClass[] = L"MiniEQLogViewer";
const UINT_PTR kLogTimer = 7;

HWND  s_hLogWnd = nullptr;
HWND  s_hLogEdit = nullptr;
HWND  s_hLogPause = nullptr;
HWND  s_hLogAutoChk = nullptr;
HFONT s_hLogFont = nullptr;
UINT64 s_logOffset = 0;
bool  s_logPaused = false;
bool  s_logClassRegistered = false;

void LogViewerPump() {
    if (s_hLogEdit == nullptr || s_logPaused) {
        return;
    }
    HANDLE h = CreateFileW(MiniEQ_DiagLogPath().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return;
    }
    const UINT64 sz = static_cast<UINT64>(size.QuadPart);
    if (sz < s_logOffset) {
        s_logOffset = 0; // file was truncated/rotated: start over
    }
    if (sz > s_logOffset) {
        UINT64 toRead = sz - s_logOffset;
        if (toRead > 256 * 1024) {
            // Way behind (e.g. viewer just opened on an old log): show the
            // tail instead of chewing through megabytes.
            s_logOffset = sz - 256 * 1024;
            toRead = 256 * 1024;
        }
        std::vector<char> buf(static_cast<size_t>(toRead));
        LARGE_INTEGER off = {};
        off.QuadPart = static_cast<LONGLONG>(s_logOffset);
        SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
        DWORD got = 0;
        if (ReadFile(h, buf.data(), static_cast<DWORD>(toRead), &got,
                     nullptr) &&
            got > 0) {
            const bool fromStart = (s_logOffset == 0);
            s_logOffset += got;
            // UTF-16LE, BOM on a fresh file (see trace.cpp).
            size_t start = 0;
            if (fromStart && got >= 2 &&
                static_cast<BYTE>(buf[0]) == 0xFF &&
                static_cast<BYTE>(buf[1]) == 0xFE) {
                start = 2;
            }
            const size_t wlen = (got - start) / 2;
            if (wlen > 0) {
                const std::wstring w(
                    reinterpret_cast<const wchar_t*>(buf.data() + start),
                    wlen);
                // Cap the view: drop the oldest half past 400k chars.
                const LRESULT len =
                    SendMessageW(s_hLogEdit, WM_GETTEXTLENGTH, 0, 0);
                if (len > 400000) {
                    SendMessageW(s_hLogEdit, EM_SETSEL, 0, len / 2);
                    SendMessageW(s_hLogEdit, EM_REPLACESEL, FALSE,
                                 (LPARAM)L"");
                }
                const LRESULT curLen =
                    SendMessageW(s_hLogEdit, WM_GETTEXTLENGTH, 0, 0);
                SendMessageW(s_hLogEdit, EM_SETSEL, curLen, curLen);
                SendMessageW(s_hLogEdit, EM_REPLACESEL, FALSE,
                             (LPARAM)w.c_str());
                if (SendMessageW(s_hLogAutoChk, BM_GETCHECK, 0, 0) ==
                    BST_CHECKED) {
                    SendMessageW(s_hLogEdit, EM_SETSEL, (WPARAM)(INT_PTR)-1,
                                 (LPARAM)(INT_PTR)-1);
                    SendMessageW(s_hLogEdit, EM_SCROLLCARET, 0, 0);
                }
            }
        }
    }
    CloseHandle(h);
}

void LogViewerClear() {
    HANDLE h = CreateFileW(MiniEQ_DiagLogPath().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
    SetWindowTextW(s_hLogEdit, L"");
    s_logOffset = 0;
    MiniEQ_AppLog(L"log cleared by user");
}

void LogViewerLayout(HWND hwnd) {
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    const int W = rc.right - rc.left;
    const int H = rc.bottom - rc.top;
    const int barH = 40;
    SetWindowPos(s_hLogEdit, nullptr, 8, 8, W - 16, H - barH - 16,
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, 202), nullptr, 8, H - barH, 90, 26,
                 SWP_NOZORDER);
    SetWindowPos(s_hLogPause, nullptr, 104, H - barH, 90, 26, SWP_NOZORDER);
    SetWindowPos(s_hLogAutoChk, nullptr, 204, H - barH, 120, 26, SWP_NOZORDER);
}

LRESULT CALLBACK LogWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                            LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        const HINSTANCE hInst =
            ((LPCREATESTRUCTW)lParam)->hInstance;
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        s_hLogEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                ES_AUTOVSCROLL,
            0, 0, 100, 100, hwnd, (HMENU)(INT_PTR)201, hInst, nullptr);
        s_hLogFont = CreateFontW(
            -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessageW(s_hLogEdit, WM_SETFONT, (WPARAM)s_hLogFont, TRUE);

        CreateWindowW(L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      0, 0, 90, 26, hwnd, (HMENU)(INT_PTR)202, hInst, nullptr);
        s_hLogPause = CreateWindowW(L"BUTTON", L"Pause",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
                                    90, 26, hwnd, (HMENU)(INT_PTR)203, hInst,
                                    nullptr);
        s_hLogAutoChk =
            CreateWindowW(L"BUTTON", L"Auto-scroll",
                          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 120,
                          26, hwnd, (HMENU)(INT_PTR)204, hInst, nullptr);
        SendMessageW(s_hLogPause, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(s_hLogAutoChk, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(GetDlgItem(hwnd, 202), WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(s_hLogAutoChk, BM_SETCHECK, BST_CHECKED, 0);
        LogViewerLayout(hwnd);
        SetTimer(hwnd, kLogTimer, 500, nullptr);
        LogViewerPump(); // paint immediately, don't wait for the first tick
        return 0;
    }
    case WM_SIZE:
        LogViewerLayout(hwnd);
        return 0;
    case WM_TIMER:
        if (wParam == kLogTimer) {
            LogViewerPump();
        }
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == 202) {
            LogViewerClear();
        } else if (id == 203) {
            s_logPaused = !s_logPaused;
            SetWindowTextW(s_hLogPause, s_logPaused ? L"Resume" : L"Pause");
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kLogTimer);
        if (s_hLogFont != nullptr) {
            DeleteObject(s_hLogFont);
            s_hLogFont = nullptr;
        }
        s_hLogWnd = nullptr;
        s_hLogEdit = nullptr;
        s_hLogPause = nullptr;
        s_hLogAutoChk = nullptr;
        s_logPaused = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void MiniEQ_ShowLogViewer(HINSTANCE hInst, HWND hParent) {
    if (s_hLogWnd != nullptr) {
        ShowWindow(s_hLogWnd, SW_SHOW);
        SetForegroundWindow(s_hLogWnd);
        return;
    }
    if (!s_logClassRegistered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = LogWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kLogWndClass;
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        if (RegisterClassW(&wc) != 0 ||
            GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            s_logClassRegistered = true;
        }
    }
    s_logOffset = 0;
    s_logPaused = false;
    s_hLogWnd = CreateWindowExW(0, kLogWndClass, L"MiniEQ \u2014 live log",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 700, 420, hParent, nullptr,
                                hInst, nullptr);
    if (s_hLogWnd != nullptr) {
        ShowWindow(s_hLogWnd, SW_SHOW);
        UpdateWindow(s_hLogWnd);
    }
}
