// settings_link.h -- UI side of the live settings channel.
//
// Opens (creating if needed) the named file mapping for one endpoint and
// pushes EqSettings updates. Also persists per-device settings to
// %APPDATA%\MiniEQ\devices.ini so each Bluetooth device remembers its EQ.

#pragma once

#include <windows.h>

#include <string>

#include "../shared/settings_channel.h"

class SettingsLink {
public:
    SettingsLink();
    ~SettingsLink();

    SettingsLink(const SettingsLink&) = delete;
    SettingsLink& operator=(const SettingsLink&) = delete;

    // Create-or-open the channel for this endpoint. On open, the current
    // channel contents (if any) are synced into the staging copy.
    bool Open(const std::wstring& endpointId);
    void Close();
    bool IsOpen() const { return m_pView != nullptr; }

    // True when Open fell back to the legacy (pre-hash) channel name: the
    // engine still runs the old APO. MaybeUpgrade switches to the hashed
    // channel once the new APO creates it, preserving the staged EQ.
    bool UsingLegacyName() const { return m_legacy; }
    bool MaybeUpgrade(const std::wstring& endpointId);

    // Current staged settings (what the sliders show).
    const EqSettings& Current() const { return m_staging; }

    // Push staged settings to the APO: data fields first, then the sequence
    // bump, so the RT reader never sees a torn update.
    void Push();

    // Direct staging access for slider handlers.
    EqSettings& Staging() { return m_staging; }

private:
    HANDLE     m_hMap = nullptr;
    EqSettings* m_pView = nullptr;
    EqSettings m_staging;
    int64_t    m_seq = 0;
    // True when the open fell back to the legacy channel name.
    bool       m_legacy = false;
};

// StatusLink -- UI side of the APO->UI heartbeat.
//
// Opens the APO-created "Global\\MiniEQ_Status_<hash>" mapping (read-only;
// the APO's worker thread publishes its heartbeat there). Read() copies a
// snapshot for the status timer; false means the channel isn't up (yet).
class StatusLink {
public:
    StatusLink();
    ~StatusLink();

    StatusLink(const StatusLink&) = delete;
    StatusLink& operator=(const StatusLink&) = delete;

    bool Open(const std::wstring& endpointId);
    void Close();
    bool IsOpen() const { return m_pView != nullptr; }
    bool Read(MiniEQApoStatus* out);

    // Same upgrade-window story as SettingsLink: true while on the legacy
    // status channel, and MaybeUpgrade switches to the hashed one once the
    // new APO publishes it.
    bool UsingLegacyName() const { return m_legacy; }
    bool MaybeUpgrade(const std::wstring& endpointId);

private:
    HANDLE           m_hMap = nullptr;
    MiniEQApoStatus* m_pView = nullptr;
    // Section bytes validated at Open(); Read() never copies past it.
    uint32_t         m_sectionSize = 0;
    // True when the open fell back to the legacy channel name.
    bool             m_legacy = false;
};

// GlobalStateLink -- the UI side of the global MiniEQ on/off flag.
// The APO creates "Global\MiniEQ__Enabled" (only it can: the UI runs in the
// user's session, which cannot create Global\ objects); this class only
// opens it and writes the desired state. Fail-open: if the mapping doesn't
// exist yet (APO not loaded), reads report the in-memory value and writes
// are no-ops -- the status timer re-asserts the persisted choice once the
// APO creates the channel.
class GlobalStateLink {
public:
    GlobalStateLink();
    ~GlobalStateLink();

    GlobalStateLink(const GlobalStateLink&) = delete;
    GlobalStateLink& operator=(const GlobalStateLink&) = delete;

    bool Open();
    void Close();
    bool IsOpen() const { return m_pView != nullptr; }
    bool Read(MiniEQGlobalState* out);
    void WriteEnabled(bool enabled);

private:
    HANDLE            m_hMap = nullptr;
    MiniEQGlobalState* m_pView = nullptr;
    int64_t           m_seq = 0;
};

// Global MiniEQ on/off (all devices at once), persisted in
// %APPDATA%\MiniEQ\devices.ini under [MiniEQ] Enabled=1/0. The in-memory
// value is authoritative for the UI; the APO learns it through the
// GlobalStateLink channel. Default: on.
bool MiniEQ_GlobalEnabledGet();
void MiniEQ_GlobalEnabledSet(bool on);

std::wstring MiniEQ_IniPath();
void MiniEQ_SaveDeviceSettings(const std::wstring& endpointId, const EqSettings& s);
bool MiniEQ_LoadDeviceSettings(const std::wstring& endpointId, EqSettings* out);
