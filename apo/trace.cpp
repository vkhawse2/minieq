// trace.cpp -- diagnostic trace for the APO load path. See trace.h.

#include "trace.h"

#include <stdarg.h>
#include <strsafe.h>

#if MINIEQ_APO_TRACE

static INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_cs;
static wchar_t g_logPath[MAX_PATH] = {};
static bool g_fileOk = false;

static void WriteLine(const wchar_t* line) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamped[1408] = {};
    StringCchPrintfW(stamped, ARRAYSIZE(stamped),
                     L"[%02u:%02u:%02u.%03u pid=%lu tid=%lu] %s\r\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     GetCurrentProcessId(), GetCurrentThreadId(), line);
    OutputDebugStringW(stamped);
    if (!g_fileOk || g_logPath[0] == L'\0') {
        return;
    }
    EnterCriticalSection(&g_cs);
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        if (GetFileSize(h, nullptr) == 0) {
            static const BYTE bom[2] = { 0xFF, 0xFE }; // UTF-16LE
            DWORD written = 0;
            WriteFile(h, bom, sizeof(bom), &written, nullptr);
        }
        DWORD written = 0;
        WriteFile(h, stamped,
                  (DWORD)(wcslen(stamped) * sizeof(wchar_t)),
                  &written, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_cs);
}

static BOOL CALLBACK InitOnceCallback(PINIT_ONCE /*once*/, PVOID /*param*/,
                                      PVOID* /*ctx*/) {
    InitializeCriticalSection(&g_cs);

    // Preferred sink: %PROGRAMDATA%\MiniEQ\apo-trace.log. The UI app runs
    // as the user and can create this dir; audiodg (service identity) can
    // then append to the file. Fallback: %WINDIR%\Temp, writable by the
    // audio service identity.
    wchar_t dir[MAX_PATH] = {};
    bool ok = false;
    DWORD n = GetEnvironmentVariableW(L"PROGRAMDATA", dir, ARRAYSIZE(dir));
    if (n > 0 && n < ARRAYSIZE(dir)) {
        StringCchCatW(dir, ARRAYSIZE(dir), L"\\MiniEQ");
        CreateDirectoryW(dir, nullptr); // ignore ERROR_ALREADY_EXISTS
        StringCchPrintfW(g_logPath, ARRAYSIZE(g_logPath),
                         L"%s\\apo-trace.log", dir);
        HANDLE h = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            ok = true;
        }
    }
    if (!ok) {
        wchar_t win[MAX_PATH] = {};
        UINT m = GetWindowsDirectoryW(win, ARRAYSIZE(win));
        if (m > 0 && m < ARRAYSIZE(win)) {
            StringCchPrintfW(g_logPath, ARRAYSIZE(g_logPath),
                             L"%s\\Temp\\MiniEQ-apo-trace.log", win);
            HANDLE h = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                ok = true;
            }
        }
    }
    g_fileOk = ok;
    return TRUE;
}

extern "C" void MiniEQ_TraceInit(void) {
    InitOnceExecuteOnce(&g_initOnce, InitOnceCallback, nullptr, nullptr);
    wchar_t host[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, host, ARRAYSIZE(host));
    wchar_t banner[700] = {};
    StringCchPrintfW(banner, ARRAYSIZE(banner),
                     L"===== MiniEQ_APO trace start host=\"%s\" log=\"%s\" =====",
                     host, g_fileOk ? g_logPath : L"<OutputDebugString only>");
    WriteLine(banner);
}

extern "C" void MiniEQ_Trace(const wchar_t* fmt, ...) {
    wchar_t buf[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(buf, ARRAYSIZE(buf), fmt, ap);
    va_end(ap);
    WriteLine(buf);
}

extern "C" void MiniEQ_TraceNoFile(const wchar_t* msg) {
    if (msg != nullptr) {
        OutputDebugStringW(msg);
    }
}

#else // !MINIEQ_APO_TRACE -- compiled out

extern "C" void MiniEQ_TraceInit(void) {}
extern "C" void MiniEQ_Trace(const wchar_t* /*fmt*/, ...) {}
extern "C" void MiniEQ_TraceNoFile(const wchar_t* /*msg*/) {}

#endif
