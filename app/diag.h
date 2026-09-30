// diag.h -- in-app diagnostics: live log, EQ-path status, audio-service fix.
//
// The UI's side of the "is audio really passing through MiniEQ" story:
//  - MiniEQ_DiagLogPath / MiniEQ_AppLog: the trace file the APO writes to,
//    plus the app's own timestamped lines in the same timeline.
//  - MiniEQ_EndpointPeakLevel: is any audio flowing on an endpoint right now?
//  - MiniEQ_RestartAudioService: one-click fix for the broken path (runs in
//    the ELEVATED helper -- it stops/starts Audiosrv and its dependents).
//  - MiniEQ_ShowLogViewer: modeless window tailing the live log.

#pragma once

#include <windows.h>

#include <string>

// The trace file the APO appends to: %PROGRAMDATA%\MiniEQ\apo-trace.log
// (falling back to %WINDIR%\Temp\MiniEQ-apo-trace.log, mirroring trace.cpp).
std::wstring MiniEQ_DiagLogPath();

// Create %PROGRAMDATA%\MiniEQ so the APO (inside audiodg.exe) can append its
// trace. Harmless when the directory already exists.
void MiniEQ_EnsureLogDir();

// Append one timestamped "UI: ..." line to the log (UTF-16LE, like trace.cpp).
void MiniEQ_AppLog(const wchar_t* fmt, ...);

// Append one timestamped "[CATEGORY] ..." line to the log (UTF-16LE).
// Categories used across the app: UI, DIAG, ENGINE, SESSION, REG, APO.
// MiniEQ_AppLog is shorthand for category "UI".
void MiniEQ_AppLogCat(const wchar_t* category, const wchar_t* fmt, ...);

// Peak level (0..1) of the given render endpoint right now, or < 0 on error.
// Used to tell "no audio playing" apart from "audio bypassing the APO".
float MiniEQ_EndpointPeakLevel(const std::wstring& endpointId);

// Restart the Windows Audio service and its active dependents (the same dance
// as `Restart-Service Audiosrv -Force`). Must run elevated.
bool MiniEQ_RestartAudioService();

// Show (or raise) the modeless live-log viewer.
void MiniEQ_ShowLogViewer(HINSTANCE hInst, HWND hParent);
