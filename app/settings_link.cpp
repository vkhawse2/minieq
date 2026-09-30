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

    m_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                0, sizeof(EqSettings), name);
    if (m_hMap == nullptr) {
        return false;
    }
    const bool existed = (GetLastError() == ERROR_ALREADY_EXISTS);
    m_pView = static_cast<EqSettings*>(
        MapViewOfFile(m_hMap, FILE_MAP_WRITE, 0, 0, sizeof(EqSettings)));
    if (m_pView == nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
        return false;
    }
    if (existed) {
        // Adopt whatever is live (another UI instance may have set it).
        memcpy(&m_staging, m_pView, sizeof(EqSettings));
        m_seq = m_staging.sequence;
    } else {
        MiniEQ_SettingsInitFlat(&m_staging);
        m_seq = m_staging.sequence;
        Push(); // publish the flat defaults
    }
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
    for (int i = 0; i < MINIEQ_MAX_BANDS; ++i) {
        m_pView->bandGainDb[i] = m_staging.bandGainDb[i];
    }
    m_pView->masterGainDb = m_staging.masterGainDb;
    m_pView->bypass = m_staging.bypass;
    m_pView->numBands = m_staging.numBands;
    MemoryBarrier();
    m_pView->sequence = ++m_seq;
    m_staging.sequence = m_seq;
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
