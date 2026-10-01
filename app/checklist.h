// checklist.h -- MiniEQ "Audio Path Checklist" popup.
//
// A modal popup, opened from the "Checklist" button next to Crossfeed.
// It runs every prerequisite check for "audio goes through MiniEQ" and shows
// each as green (working), yellow (not active yet) or red (error), with the
// exact fix on red rows. The checks reuse MiniEQ_RunDiagnosis(), so the
// checklist can never disagree with the Diagnostics Center.

#pragma once

#include <windows.h>

#include <string>

// Modal. endpointId/deviceName are captured at open time; the parent window
// is disabled until the popup closes.
void MiniEQ_ShowChecklist(HINSTANCE hInst, HWND hParent,
                          const std::wstring& endpointId,
                          const std::wstring& deviceName);

// Disarms the checklist's "Watch engine" auto-recovery, if armed. The
// circuit breaker calls this when it fires so the watch doesn't fight the
// breaker over the engine.
void MiniEQ_ChecklistDisarmWatch();
