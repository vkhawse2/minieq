// trace.h -- diagnostic trace for the APO load path.
//
// Why this exists: when the audio engine silently refuses to load an APO
// (no error, no event log), the only way to see how far it got is to log
// from inside the DLL. Each milestone of the load path -- DllMain,
// DllGetClassObject, CreateInstance, Initialize, format negotiation,
// LockForProcess, settings-channel open, first APOProcess -- emits one
// timestamped line.
//
// Sink: %PROGRAMDATA%\MiniEQ\apo-trace.log (fallback %WINDIR%\Temp\),
// plus OutputDebugStringW (visible in Sysinternals DebugView).
//
// This is a DIAGNOSTIC build aid. Keep the call sites; the whole facility
// compiles out when MINIEQ_APO_TRACE is 0.

#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Resolve the log path and write a banner. Call from DllGetClassObject --
// NOT from DllMain (file I/O under the loader lock is unsafe). Idempotent.
void MiniEQ_TraceInit(void);

// Timestamped line -> log file + OutputDebugStringW.
void MiniEQ_Trace(const wchar_t* fmt, ...);

// Loader-lock safe: OutputDebugStringW only. No file I/O, no locks.
void MiniEQ_TraceNoFile(const wchar_t* msg);

#ifdef __cplusplus
}
#endif
