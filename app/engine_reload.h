// engine_reload.h -- reload the Windows audio path without touching services.
//
// When the UI detects the audio engine is running a stale MiniEQ_APO.dll
// (build id in the APO status channel != the installed build), the fix is to
// make the engine rebuild the audio graph: briefly flip the endpoint's
// default format (PKEY_AudioEngine_DeviceFormat) to a neighbouring sample
// rate and back. That is the same property the Sound control panel writes --
// no service restarts, no elevation.
//
// Everything here that sleeps or blocks runs on a worker thread.

#pragma once

#include <windows.h>

#include <string>

#include "settings_channel.h"

// What the engine reports about the loaded APO build.
enum class BuildFreshness {
    Unknown, // no status channel / APO never locked: nothing to compare
    Stale,   // a build is loaded and it isn't the installed one
    Fresh,   // the loaded build matches the installed one
};

// One-shot read of the APO status channel for endpointId. Never keeps the
// mapping open. reportedBuild receives the loaded build id ("v1" for builds
// that predate the stamp) when the freshness is Stale.
BuildFreshness MiniEQ_EngineBuildFreshness(const std::wstring& endpointId,
                                           std::wstring* reportedBuild);

// True while any render session on the endpoint is audibly active.
// activeName receives a friendly name ("Chrome") when one is available.
bool MiniEQ_AudioPlaying(const std::wstring& endpointId, std::wstring* activeName);

// Flip the endpoint's default format away and back so the engine rebuilds
// the graph and reloads MiniEQ_APO.dll from disk. Restores the original
// format before returning. Sleeps ~1 s; call on a worker thread.
bool MiniEQ_FlipDefaultFormat(const std::wstring& endpointId, std::wstring* detail);

enum class EngineReloadResult {
    UpToDate,  // engine already runs this build (or nothing to reload)
    Deferred,  // stale, but audio is playing: caller should wait
    Reloaded,  // flip done and the new build verified in the engine
    Failed,    // flip done but the engine still reports the stale build
    FlipError, // the format flip itself failed
};

struct EngineReloadOutcome {
    EngineReloadResult result = EngineReloadResult::UpToDate;
    std::wstring endpoint;      // the endpoint this job ran against
    std::wstring reportedBuild; // stale build the engine was running
    std::wstring activeSession; // Deferred: who is playing
    std::wstring detail;        // Failed / FlipError: why
};

// The build id this UI was compiled from (CI stamps the short commit SHA;
// local builds report "dev"). The engine is stale when it reports anything
// else.
const char* MiniEQ_ExpectedBuildId();

// Full job for the worker thread: re-check staleness, honour the session
// policy unless force is set, flip, then verify the new build is live.
// force also flips when the build already matches (post-attach refresh),
// in which case there is no build change to verify. The caller owns the
// returned pointer; it is posted back to the UI thread.
EngineReloadOutcome* MiniEQ_RunReloadJob(const std::wstring& endpointId, bool force);
