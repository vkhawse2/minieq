// settings_channel.h -- shared between the APO DLL and the UI app.
//
// Two channels over named file mappings, both keyed by endpoint ID:
//
//  1. Settings (UI -> APO): the live EQ settings. The UI is the single
//     writer; the APO is the single reader. The APO's real-time thread only
//     ever performs an atomic load of the sequence counter and, when it
//     changes, a plain memcpy of the struct -- no locks, no syscalls, no COM
//     on the audio thread.
//
//  2. Status (APO -> UI): the heartbeat. The APO's background worker thread
//     publishes APOProcess call counts and timestamps; the UI polls them on a
//     timer to show whether audio is REALLY flowing through the APO instead
//     of guessing from registry keys.
//
// The mapping names are derived from the Windows audio endpoint ID, so each
// output device (e.g. WH-1000XM4 vs Tribit XSound Go) gets its own independent
// EQ state: per-device EQ memory falls out naturally.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MINIEQ_NUM_BANDS 5      // default band count
#define MINIEQ_MAX_BANDS 10     // max band count (5/10 toggle in settings)
#define MINIEQ_MAX_CHANNELS 8
#define MINIEQ_GAIN_MIN_DB (-12.0f)
#define MINIEQ_GAIN_MAX_DB (+12.0f)

// Fixed band center frequencies (Hz) -- classic 5-band and 10-band layouts.
static const float MINIEQ_BAND_FREQS_5[MINIEQ_NUM_BANDS] = {
    60.0f, 230.0f, 910.0f, 3600.0f, 14000.0f
};
static const float MINIEQ_BAND_FREQS_10[MINIEQ_MAX_BANDS] = {
    31.0f, 62.0f, 125.0f, 250.0f, 500.0f,
    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};
#define MINIEQ_BAND_Q 1.0f

// Center frequency for `band` (0-based) in a `numBands` layout (5 or 10).
static inline float MiniEQ_BandFreq(int numBands, int band) {
    if (numBands == MINIEQ_MAX_BANDS &&
        band >= 0 && band < MINIEQ_MAX_BANDS) {
        return MINIEQ_BAND_FREQS_10[band];
    }
    if (band < 0) band = 0;
    if (band >= MINIEQ_NUM_BANDS) band = MINIEQ_NUM_BANDS - 1;
    return MINIEQ_BAND_FREQS_5[band];
}

// Layout of the shared memory block. Written by the UI, read by the APO.
// `sequence` is bumped AFTER the rest of the struct is fully written; the APO
// copies the struct only after observing a new sequence value.
typedef struct EqSettings {
    volatile int64_t sequence;                    // writer-owned version counter
    float            bandGainDb[MINIEQ_MAX_BANDS]; // per-band gain, dB
    float            masterGainDb;                 // master trim, dB
    int32_t          bypass;                       // 0 = process, 1 = bypass
    int32_t          numBands;                     // 5 or 10 (settings toggle)
    int32_t          virtualization;               // 0 = off, 1 = headphone crossfeed on
    int32_t          _reserved[5];
} EqSettings;

// APO -> UI heartbeat. Written by the APO's background worker thread; read by
// the UI on a timer. processCalls advancing means APOProcess is really being
// called by the engine -- the ground truth for "EQ is live".
//
// Version history:
//   1: original fields.
//   2: added buildId (repurposed the trailing reserved bytes, so the struct
//      size is unchanged): the APO stamps the build it was compiled from,
//      letting the UI tell a stale loaded DLL apart from the installed one.
#define MINIEQ_STATUS_VERSION 2

typedef struct MiniEQApoStatus {
    uint32_t structSize;             // sizeof(MiniEQApoStatus): versioning
    uint32_t version;                // MINIEQ_STATUS_VERSION
    volatile int64_t processCalls;   // APOProcess invocations, all-time
    volatile int64_t lastProcessQpc; // QPC value at the last APOProcess call
    volatile int64_t qpcFrequency;   // QueryPerformanceFrequency() result
    volatile int32_t locked;         // LockForProcess completed
    volatile int32_t channels;       // locked format channel count
    volatile int32_t sampleRate;     // locked format sample rate
    volatile int32_t initOk;         // Initialize succeeded
    char             buildId[16];    // APO build id (short commit SHA), NUL-terminated
} MiniEQApoStatus;

// "MiniEQ_{sanitized-endpoint-id}" -- caller supplies a buffer.
void MiniEQ_MappingNameForEndpoint(const wchar_t* endpointId,
                                  wchar_t* outName, size_t outNameChars);

// "Global\MiniEQ_Status_{sanitized-endpoint-id}" -- caller supplies a buffer.
void MiniEQ_StatusNameForEndpoint(const wchar_t* endpointId,
                                  wchar_t* outName, size_t outNameChars);

// Global on/off (UI -> APO): one flag shared by every endpoint. The APO
// creates "Global\MiniEQ__Enabled" -- it must be the creator, because only
// the audio engine process (session 0) holds SeCreateGlobalPrivilege; the
// UI (user session) can only open it. Fail-open: an absent mapping means
// enabled, so audio keeps working with older UI builds. `sequence` is
// bumped AFTER `enabled` is written; the APO adopts the pair only when the
// two sequence reads match (no torn updates).
#define MINIEQ_GLOBAL_VERSION 1

typedef struct MiniEQGlobalState {
    uint32_t structSize;       // sizeof(MiniEQGlobalState): versioning
    uint32_t version;          // MINIEQ_GLOBAL_VERSION
    volatile int64_t sequence; // writer-owned version counter
    volatile int32_t enabled;  // 1 = MiniEQ processes audio, 0 = bypass all
    volatile int32_t _reserved[3];
} MiniEQGlobalState;

// "Global\MiniEQ__Enabled" -- caller supplies a buffer.
void MiniEQ_GlobalStateName(wchar_t* outName, size_t outNameChars);

// Fill an EqSettings with flat (no-op) values, sequence = 1.
void MiniEQ_SettingsInitFlat(EqSettings* s);

#ifdef __cplusplus
}
#endif
