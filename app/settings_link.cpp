// settings_link.cpp -- UI-side channel writer + INI persistence.

#include "settings_link.h"

#include <shlobj.h>
#include <strsafe.h>

#include <cstring>

SettingsLink::SettingsLink() {
    MiniEQ_SettingsInitFlat(&m_staging);
}

SettingsLink::~SettingsLink() {
    Close();
}

bool SettingsLink::Open(const std::wstring& endpointId) {
    Close();

    wchar_t name[128] = {};
    MiniEQ_MappingNameForEndpoint(endpointId.c_str(), name, ARRAYSIZE(name));

    // OPEN only -- the APO (audio engine, session 0) is the creator of the
    // Global\ channel. The UI (user session) cannot create Global\ objects
    // (no SeCreateGlobalPrivilege), and creating a same-named mapping here
    // would shadow the APO's real one instead of connecting to it.
    m_hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (m_hMap == nullptr) {
        return false;
    }
    m_pView = static_cast<EqSettings*>(
        MapViewOfFile(m_hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(EqSettings)));
    if (m_pView == nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
        return false;
    }
    // Adopt whatever is live (the APO published flat defaults at creation,
    // or another UI instance set it). The sequence adopt keeps the UI and
    // APO in sync; the saved-EQ restore happens in TryOpenChannels.
    memcpy(&m_staging, m_pView, sizeof(EqSettings));
    m_seq = m_staging.sequence;
    return true;
}

void SettingsLink::Close() {
    if (m_pView != nullptr) {
        UnmapViewOfFile(m_pView);
        m_pView = nullptr;
    }
    if (m_hMap != nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
    }
}

void SettingsLink::Push() {
    if (m_pView == nullptr) {
        return;
    }
    // Seqlock writer (protocol in shared/settings_channel.h): bracket the
    // update so the counter is odd while the struct is being written and
    // even once it is consistent. InterlockedIncrement64 is a full barrier,
    // so the data stores cannot leak outside the bracket on the compiler
    // or the CPU.
    InterlockedIncrement64(&m_pView->sequence); // -> odd: write in flight
    for (int i = 0; i < MINIEQ_MAX_BANDS; ++i) {
        m_pView->bandGainDb[i] = m_staging.bandGainDb[i];
    }
    m_pView->masterGainDb = m_staging.masterGainDb;
    m_pView->bypass = m_staging.bypass;
    m_pView->numBands = m_staging.numBands;
    m_pView->virtualization = m_staging.virtualization;
    m_seq = InterlockedIncrement64(&m_pView->sequence); // -> even: consistent
    m_staging.sequence = m_seq;
}

StatusLink::StatusLink() {
}

StatusLink::~StatusLink() {
    Close();
}

bool StatusLink::Open(const std::wstring& endpointId) {
    Close();

    wchar_t name[160] = {};
    MiniEQ_StatusNameForEndpoint(endpointId.c_str(), name, ARRAYSIZE(name));

    // OPEN only -- the APO is the creator of the Global\ status channel;
    // see SettingsLink::Open. The APO initializes the header at Lock time.
    m_hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (m_hMap == nullptr) {
        return false;
    }
    m_pView = static_cast<MiniEQApoStatus*>(
        MapViewOfFile(m_hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                      sizeof(MiniEQApoStatus)));
    if (m_pView == nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
        return false;
    }
    return true;
}

void StatusLink::Close() {
    if (m_pView != nullptr) {
        UnmapViewOfFile(m_pView);
        m_pView = nullptr;
    }
    if (m_hMap != nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
    }
}

bool StatusLink::Read(MiniEQApoStatus* out) {
    if (m_pView == nullptr || out == nullptr) {
        return false;
    }
    // The APO worker writes aligned fields; a plain copy is a consistent
    // enough snapshot for a 500 ms status poll.
    memcpy(out, m_pView, sizeof(MiniEQApoStatus));
    return true;
}

GlobalStateLink::GlobalStateLink() {
}

GlobalStateLink::~GlobalStateLink() {
    Close();
}

bool GlobalStateLink::Open() {
    Close();

    wchar_t name[64] = {};
    MiniEQ_GlobalStateName(name, ARRAYSIZE(name));

    // OPEN only -- the APO is the creator of the Global\ channel; a
    // user-session process cannot create Global\ objects. If the APO hasn't
    // loaded yet (audio engine rebuilding), this fails and the caller retries
    // later; meanwhile MiniEQ stays enabled (fail-open).
    m_hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (m_hMap == nullptr) {
        return false;
    }
    m_pView = static_cast<MiniEQGlobalState*>(
        MapViewOfFile(m_hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                      sizeof(MiniEQGlobalState)));
    if (m_pView == nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
        return false;
    }
    m_seq = m_pView->sequence;
    return true;
}

void GlobalStateLink::Close() {
    if (m_pView != nullptr) {
        UnmapViewOfFile(m_pView);
        m_pView = nullptr;
    }
    if (m_hMap != nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
    }
}

bool GlobalStateLink::Read(MiniEQGlobalState* out) {
    if (m_pView == nullptr || out == nullptr) {
        return false;
    }
    memcpy(out, m_pView, sizeof(MiniEQGlobalState));
    return true;
}

void GlobalStateLink::WriteEnabled(bool enabled) {
    if (m_pView == nullptr) {
        return;
    }
    // Seqlock writer, same protocol as the settings channel: bracket the
    // update so the counter is odd mid-write and even when consistent, and
    // the APO's torn-read guard sees a consistent pair.
    InterlockedIncrement64(&m_pView->sequence); // -> odd: write in flight
    m_pView->enabled = enabled ? 1 : 0;
    m_seq = InterlockedIncrement64(&m_pView->sequence); // -> even: consistent
}

std::wstring MiniEQ_IniPath() {
    wchar_t appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        return L"";
    }
    std::wstring dir = std::wstring(appdata) + L"\\MiniEQ";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\devices.ini";
}

static void WriteFloat(const std::wstring& ini, const std::wstring& section,
                       const wchar_t* key, float v) {
    wchar_t buf[32] = {};
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"%.2f", v);
    WritePrivateProfileStringW(section.c_str(), key, buf, ini.c_str());
}

void MiniEQ_SaveDeviceSettings(const std::wstring& endpointId, const EqSettings& s) {
    std::wstring ini = MiniEQ_IniPath();
    if (ini.empty()) {
        return;
    }
    // Endpoint IDs contain characters INI sections tolerate; keep them raw so
    // the mapping is obvious when inspecting the file.
    for (int i = 0; i < MINIEQ_MAX_BANDS; ++i) {
        wchar_t key[16] = {};
        StringCchPrintfW(key, ARRAYSIZE(key), L"Band%d", i);
        WriteFloat(ini, endpointId, key, s.bandGainDb[i]);
    }
    WriteFloat(ini, endpointId, L"Master", s.masterGainDb);
    WritePrivateProfileStringW(endpointId.c_str(), L"Bypass",
                               s.bypass ? L"1" : L"0", ini.c_str());
    wchar_t nb[8] = {};
    StringCchPrintfW(nb, ARRAYSIZE(nb), L"%d", s.numBands);
    WritePrivateProfileStringW(endpointId.c_str(), L"NumBands", nb, ini.c_str());
    WritePrivateProfileStringW(endpointId.c_str(), L"Virtualization",
                               s.virtualization ? L"1" : L"0", ini.c_str());
}

bool MiniEQ_LoadDeviceSettings(const std::wstring& endpointId, EqSettings* out) {
    std::wstring ini = MiniEQ_IniPath();
    if (ini.empty() || out == nullptr) {
        return false;
    }
    // Presence of Band0 decides whether we ever saved this device.
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(endpointId.c_str(), L"Band0", L"", buf, ARRAYSIZE(buf), ini.c_str());
    if (buf[0] == L'\0') {
        return false; // never saved: caller keeps channel/flat defaults
    }
    auto getf = [&](const wchar_t* key) -> float {
        GetPrivateProfileStringW(endpointId.c_str(), key, L"0", buf, ARRAYSIZE(buf), ini.c_str());
        return (float)_wtof(buf);
    };
    MiniEQ_SettingsInitFlat(out);
    for (int i = 0; i < MINIEQ_MAX_BANDS; ++i) {
        wchar_t key[16] = {};
        StringCchPrintfW(key, ARRAYSIZE(key), L"Band%d", i);
        out->bandGainDb[i] = getf(key);
    }
    out->masterGainDb = getf(L"Master");
    out->bypass = GetPrivateProfileIntW(endpointId.c_str(), L"Bypass", 0, ini.c_str());
    const int nb = GetPrivateProfileIntW(endpointId.c_str(), L"NumBands",
                                         MINIEQ_NUM_BANDS, ini.c_str());
    out->numBands = (nb == MINIEQ_MAX_BANDS) ? MINIEQ_MAX_BANDS : MINIEQ_NUM_BANDS;
    out->virtualization =
        GetPrivateProfileIntW(endpointId.c_str(), L"Virtualization", 0, ini.c_str()) ? 1 : 0;
    return true;
}

// Global MiniEQ on/off: one [MiniEQ] section, not per-device. The in-memory
// value is authoritative for the UI; the APO learns it through the
// GlobalStateLink channel (re-asserted on the status timer).
static bool s_globalEnabled = true;
static bool s_globalLoaded = false;

static void MiniEQ_SaveGlobalEnabledIni(bool on) {
    std::wstring ini = MiniEQ_IniPath();
    if (ini.empty()) {
        return;
    }
    WritePrivateProfileStringW(L"MiniEQ", L"Enabled", on ? L"1" : L"0", ini.c_str());
}

static bool MiniEQ_LoadGlobalEnabledIni(bool* out) {
    std::wstring ini = MiniEQ_IniPath();
    if (ini.empty() || out == nullptr) {
        return false;
    }
    wchar_t buf[8] = {};
    GetPrivateProfileStringW(L"MiniEQ", L"Enabled", L"", buf, ARRAYSIZE(buf), ini.c_str());
    if (buf[0] == L'\0') {
        return false; // never saved: default stays on
    }
    *out = (buf[0] != L'0');
    return true;
}

bool MiniEQ_GlobalEnabledGet() {
    if (!s_globalLoaded) {
        bool v = true;
        if (MiniEQ_LoadGlobalEnabledIni(&v)) {
            s_globalEnabled = v;
        }
        s_globalLoaded = true;
    }
    return s_globalEnabled;
}

void MiniEQ_GlobalEnabledSet(bool on) {
    s_globalEnabled = on;
    s_globalLoaded = true;
    MiniEQ_SaveGlobalEnabledIni(on);
}
