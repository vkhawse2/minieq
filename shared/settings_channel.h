// settings_channel.h -- shared between the APO DLL and the UI app.
//
// The live settings channel between MiniEQ (UI) and MiniEQ_APO (DSP).
//
// Design: a named file-mapping object (shared memory) holding one EqSettings
// struct, plus a 64-bit sequence counter. The UI is the single writer; the
// APO is the single reader. The APO's real-time thread only ever performs an
// atomic load of the sequence counter and, when it changes, a plain memcpy of
// the struct -- no locks, no syscalls, no COM on the audio thread.
//
// The mapping name is derived from the Windows audio endpoint ID, so each
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
    int32_t          _reserved[6];
} EqSettings;

// "MiniEQ_{sanitized-endpoint-id}" -- caller supplies a buffer.
void MiniEQ_MappingNameForEndpoint(const wchar_t* endpointId,
                                  wchar_t* outName, size_t outNameChars);

// Fill an EqSettings with flat (no-op) values, sequence = 1.
void MiniEQ_SettingsInitFlat(EqSettings* s);

#ifdef __cplusplus
}
#endif
