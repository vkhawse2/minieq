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
};

// StatusLink -- UI side of the APO->UI heartbeat.
//
// Creates the "Local\\MiniEQ_Status_{endpoint}" mapping (PAGE_READWRITE) so
// the APO's worker thread can publish its heartbeat there. Read() copies a
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

private:
    HANDLE           m_hMap = nullptr;
    MiniEQApoStatus* m_pView = nullptr;
};

std::wstring MiniEQ_IniPath();
void MiniEQ_SaveDeviceSettings(const std::wstring& endpointId, const EqSettings& s);
bool MiniEQ_LoadDeviceSettings(const std::wstring& endpointId, EqSettings* out);
