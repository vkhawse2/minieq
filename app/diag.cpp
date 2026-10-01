// diag.cpp -- in-app diagnostics implementation.

#include "diag.h"

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <shlobj.h>
#include <stdarg.h>
#include <strsafe.h>
#include <aclapi.h>

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

// True when the directory's DACL already grants Users FILE_ADD_FILE.
static bool MiniEQ_UsersCanWriteDir(const wchar_t* path, PSID usersSid) {
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    bool ok = false;
    if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &acl, nullptr, &sd) == ERROR_SUCCESS) {
        ACL_SIZE_INFORMATION info = {};
        if (GetAclInformation(acl, &info, sizeof(info), AclSizeInformation)) {
            for (DWORD i = 0; i < info.AceCount && !ok; ++i) {
                ACCESS_ALLOWED_ACE* ace = nullptr;
                if (!GetAce(acl, i, reinterpret_cast<void**>(&ace)) ||
                    ace == nullptr) {
                    continue;
                }
                if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) {
                    continue;
                }
                if (EqualSid(usersSid, reinterpret_cast<PSID>(&ace->SidStart)) &&
                    (ace->Mask & FILE_ADD_FILE) != 0) {
                    ok = true;
                }
            }
        }
        LocalFree(sd);
    }
    return ok;
}

// Grants the local Users group file-create rights on the directory, merged
// into the existing DACL (never replaced). The installer's --attach-all
// helper runs as SYSTEM before the user ever launches the app; a
// SYSTEM-created %PROGRAMDATA% subdirectory is not user-writable, which
// would silently break the app's own diagnostic log on fresh installs.
static void MiniEQ_GrantUsersWriteDir(const wchar_t* path) {
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    PSID usersSid = nullptr;
    if (!AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0,
                                  &usersSid)) {
        return;
    }
    if (!MiniEQ_UsersCanWriteDir(path, usersSid)) {
        EXPLICIT_ACCESSW ea = {};
        ea.grfAccessPermissions = FILE_ADD_FILE | FILE_LIST_DIRECTORY | SYNCHRONIZE;
        ea.grfAccessMode = GRANT_ACCESS;
        ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(usersSid);
        PACL oldAcl = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, &oldAcl, nullptr, &sd) == ERROR_SUCCESS) {
            PACL newAcl = nullptr;
            if (SetEntriesInAclW(1, &ea, oldAcl, &newAcl) == ERROR_SUCCESS &&
                newAcl != nullptr) {
                SetNamedSecurityInfoW(const_cast<LPWSTR>(path), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, newAcl, nullptr);
                LocalFree(newAcl);
            }
            LocalFree(sd);
        }
    }
    FreeSid(usersSid);
}

void MiniEQ_EnsureLogDir() {
    wchar_t dir[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr,
                                0, dir))) {
        return;
    }
    std::wstring d = std::wstring(dir) + L"\\MiniEQ";
    if (!CreateDirectoryW(d.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return;
    }
    MiniEQ_GrantUsersWriteDir(d.c_str());
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

bool MiniEQ_CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (text.empty()) {
        return false;
    }
    if (!OpenClipboard(hwnd)) {
        return false;
    }
    bool ok = false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h != nullptr) {
        void* p = GlobalLock(h);
        if (p != nullptr) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        }
        if (!ok) {
            GlobalFree(h);
        }
    }
    CloseClipboard();
    return ok;
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
// Live log viewer (modeless)
//------------------------------------------------------------------------------

namespace {

const wchar_t kLogWndClass[] = L"MiniEQLogViewer";
const UINT_PTR kLogTimer = 7;

HWND  s_hLogWnd = nullptr;
HWND  s_hLogEdit = nullptr;
HWND  s_hLogPause = nullptr;
HWND  s_hLogAutoChk = nullptr;
HWND  s_hLogCopy = nullptr;
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
    SetWindowPos(s_hLogCopy, nullptr, 330, H - barH, 90, 26, SWP_NOZORDER);
}

static void LogViewerCopyAll(HWND hwnd) {
    if (s_hLogEdit == nullptr) {
        return;
    }
    const LRESULT len = SendMessageW(s_hLogEdit, WM_GETTEXTLENGTH, 0, 0);
    if (len <= 0 || len > 4 * 1024 * 1024) {
        return; // empty, or absurdly large: don't wedge the clipboard
    }
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    SendMessageW(s_hLogEdit, WM_GETTEXT, (WPARAM)text.size(),
                 (LPARAM)text.data());
    text.resize(static_cast<size_t>(len));
    if (MiniEQ_CopyTextToClipboard(hwnd, text)) {
        MiniEQ_AppLogCat(L"UI", L"live log copied to clipboard (%lld chars)",
                         static_cast<long long>(len));
    }
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
        s_hLogCopy =
            CreateWindowW(L"BUTTON", L"Copy all",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 90,
                          26, hwnd, (HMENU)(INT_PTR)205, hInst, nullptr);
        SendMessageW(s_hLogPause, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(s_hLogAutoChk, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(s_hLogCopy, WM_SETFONT, (WPARAM)font, TRUE);
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
        } else if (id == 205) {
            LogViewerCopyAll(hwnd);
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
        s_hLogCopy = nullptr;
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
