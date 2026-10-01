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

    // Preferred: the hashed name (fixed length, collision-resistant).
    // Fallback: the legacy sanitize-and-truncate name, for the upgrade
    // window where the audio engine still runs the old APO. Never create
    // the legacy name -- only the APO creates channels, and the new APO
    // only creates hashed ones.
    wchar_t name[160] = {};
    wchar_t legacy[160] = {};
    MiniEQ_MappingNameForEndpoint(endpointId.c_str(), name, ARRAYSIZE(name));
    MiniEQ_LegacyMappingNameForEndpoint(endpointId.c_str(), legacy,
                                       ARRAYSIZE(legacy));

    // OPEN only -- the APO (audio engine, session 0) is the creator of the
    // Global\ channel. The UI (user session) cannot create Global\ objects
    // (no SeCreateGlobalPrivilege), and creating a same-named mapping here
    // would shadow the APO's real one instead of connecting to it.
    m_hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (m_hMap == nullptr) {
        m_hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                  legacy);
        if (m_hMap != nullptr) {
            m_legacy = true;
            StringCchCopyW(name, ARRAYSIZE(name), legacy);
        } else {
            return false;
        }
    }
    m_pView = static_cast<EqSettings*>(
        MapViewOfFile(m_hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(EqSettings)));
    if (m_pView == nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
        return false;
    }
    // Adopt whatever is live (the APO published flat defaults at creation,
    // or another UI instance set it). Bounded seqlock read: snapshot,
    // copy, re-verify -- never adopt a torn struct if the writer races us.
    // The sequence adopt keeps the UI and APO in sync; the saved-EQ restore
    // happens in TryOpenChannels.
    EqSettings live;
    if (MiniEQ_SettingsReadSeqlock(m_pView, &live, 4)) {
        m_staging = live;
        m_seq = live.sequence;
    } else {
        // The writer kept racing us (pathological); fall back to flat
        // defaults rather than a possibly torn adopt.
        MiniEQ_SettingsInitFlat(&m_staging);
        m_seq = m_staging.sequence;
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
    m_legacy = false;
}

// Upgrade window: we opened the legacy channel because the engine still
// ran the old APO. Once the hashed channel appears, the new APO is up --
// switch to it. The new channel holds the new APO's flat defaults, so the
// staged EQ (the user's settings) is preserved across the re-open and
// re-pushed; without that the sliders would keep showing the user's EQ
// while the APO played flat.
bool SettingsLink::MaybeUpgrade(const std::wstring& endpointId) {
    if (!m_legacy || !IsOpen()) {
        return false;
    }
    wchar_t name[160] = {};
    MiniEQ_MappingNameForEndpoint(endpointId.c_str(), name, ARRAYSIZE(name));
    HANDLE probe = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (probe == nullptr) {
        return false; // old APO still the only one around
    }
    CloseHandle(probe);
    const EqSettings keepStaging = m_staging;
    const int64_t keepSeq = m_seq;
    if (!Open(endpointId)) {
        // Vanishing race; keep the user's staging, the caller retries.
        m_staging = keepStaging;
        m_seq = keepSeq;
        return false;
    }
    m_staging = keepStaging;
    m_seq = keepSeq;
    Push();
    return true;
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

    // Hashed name first, legacy fallback for the upgrade window (see
    // SettingsLink::Open). Open-only: the APO creates the channel.
    wchar_t name[160] = {};
    wchar_t legacy[160] = {};
    MiniEQ_StatusNameForEndpoint(endpointId.c_str(), name, ARRAYSIZE(name));
    MiniEQ_LegacyStatusNameForEndpoint(endpointId.c_str(), legacy,
                                       ARRAYSIZE(legacy));

    // OPEN only -- the APO is the creator of the Global\ status channel;
    // see SettingsLink::Open. The APO initializes the header at Lock time.
    m_hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (m_hMap == nullptr) {
        m_hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                  legacy);
        if (m_hMap != nullptr) {
            m_legacy = true;
        } else {
            return false;
        }
    }
    // Map the whole section: the channel may come from an older build with
    // a smaller struct. The header's structSize tells us what is really
    // there; Read() never touches beyond it.
    m_pView = static_cast<MiniEQApoStatus*>(
        MapViewOfFile(m_hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
    if (m_pView == nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
        return false;
    }
    // Validate the header before trusting anything. Minimum: the v1 fixed
    // fields (through initOk); below that (racing creator, corrupt) we back
    // off and let the caller retry on its next poll.
    const uint32_t v = m_pView->version;
    const uint32_t sz = m_pView->structSize;
    const size_t kMinStatusSize =
        offsetof(MiniEQApoStatus, initOk) + sizeof(m_pView->initOk);
    if (v < 1 || v > MINIEQ_STATUS_VERSION ||
        sz < kMinStatusSize || sz > 65536) {
        Close();
        return false;
    }
    m_sectionSize = sz;
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
    m_sectionSize = 0;
    m_legacy = false;
}

// Upgrade window (see SettingsLink::MaybeUpgrade): switch from the legacy
// status channel to the hashed one once the new APO publishes it, so the
// heartbeat doesn't freeze on the old APO's abandoned channel.
bool StatusLink::MaybeUpgrade(const std::wstring& endpointId) {
    if (!m_legacy || !IsOpen()) {
        return false;
    }
    wchar_t name[160] = {};
    MiniEQ_StatusNameForEndpoint(endpointId.c_str(), name, ARRAYSIZE(name));
    HANDLE probe = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (probe == nullptr) {
        return false;
    }
    CloseHandle(probe);
    return Open(endpointId);
}

bool StatusLink::Read(MiniEQApoStatus* out) {
    if (m_pView == nullptr || out == nullptr) {
        return false;
    }
    // Copy at most what the section holds (an older build's channel may be
    // smaller); fields past it stay zeroed. The UI treats version < 3 as
    // "no seqlock": best-effort copy, same as before.
    const size_t kSeqlockSize =
        offsetof(MiniEQApoStatus, sequence) + sizeof(out->sequence);
    const bool hasSeqlock =
        (m_pView->version >= 3 && m_sectionSize >= kSeqlockSize);
    memset(out, 0, sizeof(MiniEQApoStatus));
    const size_t copySize =
        m_sectionSize < sizeof(MiniEQApoStatus) ? m_sectionSize
                                               : sizeof(MiniEQApoStatus);
    if (hasSeqlock) {
#ifdef _WIN32
        volatile LONG64* seqAddr =
            reinterpret_cast<volatile LONG64*>(&m_pView->sequence);
        for (int attempt = 0; attempt < 4; ++attempt) {
            const int64_t seq = InterlockedCompareExchange64(seqAddr, 0, 0);
            if (seq & 1) {
                continue; // worker mid-write; retry
            }
            memcpy(out, (const void*)m_pView, copySize);
            const int64_t seq2 = InterlockedCompareExchange64(seqAddr, 0, 0);
            if (seq2 == seq) {
                return true;
            }
        }
        return false; // writer kept racing; caller keeps its previous state
#else
        memcpy(out, m_pView, copySize);
        return true;
#endif
    }
    memcpy(out, m_pView, copySize);
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
    // Seqlock reader, mirroring the APO's own read: the writer brackets
    // updates with an odd sequence, so accept the snapshot only when the
    // two sequence reads match and are even. Bounded retries -- a writer
    // that died mid-update leaves the sequence odd, and we must not spin
    // forever on the UI thread.
    volatile LONG64* seqAddr =
        const_cast<volatile LONG64*>(&m_pView->sequence);
    for (int tries = 0; tries < 4; ++tries) {
        const int64_t seq1 = InterlockedCompareExchange64(seqAddr, 0, 0);
        if (seq1 & 1) {
            continue; // write in flight (or a dead writer's odd residue)
        }
        MiniEQGlobalState snap;
        memcpy(&snap, m_pView, sizeof(snap));
        const int64_t seq2 = InterlockedCompareExchange64(seqAddr, 0, 0);
        if (seq1 == seq2) {
            memcpy(out, &snap, sizeof(snap));
            return true;
        }
    }
    return false;
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

static void SaveToIni(const std::wstring& ini, const std::wstring& endpointId,
                      const EqSettings& s) {
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

// DEVICE IDENTITY NOTE (2026-10-01 audit): settings are keyed by MMDevice
// endpoint ID, which is stable for wired/USB devices but can change for
// Bluetooth endpoints across re-enumeration or re-pairing (and A2DP vs HFP
// profiles are always separate endpoints). The reliable stable key is the
// device container ID (PKEY_Device_ContainerId), which groups all profiles
// of one physical BT device and survives re-enumeration. Migrating the key
// scheme is a settings-migration feature, not a stability fix, so it is
// deliberately left for a later batch -- documented, not half-built. Until
// then, a re-enumerated BT device simply starts from flat defaults (or the
// machine-wide mirror) instead of crashing or misbehaving.
void MiniEQ_SaveDeviceSettings(const std::wstring& endpointId, const EqSettings& s) {
    std::wstring ini = MiniEQ_IniPath();
    if (!ini.empty()) {
        SaveToIni(ini, endpointId, s);
    }
    // Machine-wide mirror (best-effort): the APO cannot reach the user's
    // %APPDATA% from session 0, so it cold-starts from here when the UI
    // isn't running. A locked-down machine where the directory can't be
    // created simply skips the mirror -- the per-user INI above is the
    // source of truth and the APO falls back to flat defaults.
    wchar_t machineIni[MAX_PATH] = {};
    if (MiniEQ_MachineIniPath(machineIni, ARRAYSIZE(machineIni))) {
        std::wstring dir = machineIni;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir.resize(slash);
            CreateDirectoryW(dir.c_str(), nullptr); // ok if it exists
        }
        SaveToIni(machineIni, endpointId, s);
    }
}

bool MiniEQ_LoadDeviceSettings(const std::wstring& endpointId, EqSettings* out) {
    std::wstring ini = MiniEQ_IniPath();
    if (ini.empty() || out == nullptr) {
        return false;
    }
    return MiniEQ_LoadDeviceSettingsFromIni(ini.c_str(), endpointId.c_str(),
                                            out) != 0;
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
