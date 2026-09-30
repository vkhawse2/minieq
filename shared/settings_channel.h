// settings_channel.h -- shared between the APO DLL and the UI app.
//
// The live settings channel between MiniEQ (UI) and MiniEQ_APO (DSP).
//
// Design: a named file-mapping object (shared memory) holding one EqSettings
// struct, synchronized with a seqlock protocol on the 64-bit sequence counter.
// The UI is the single writer; the APO is the single reader.
//
// Protocol: the writer brackets every update with InterlockedIncrement64.
// An ODD sequence means "a write is in flight -- do not trust the struct";
// an EVEN sequence means "the struct is consistent". The reader takes an
// atomic snapshot of the counter; if it is even and differs from the last
// applied value, it copies the struct and re-reads the counter -- the copy
// is accepted only if the counter is unchanged (bounded retries; on failure
// the old settings are kept and the next audio buffer tries again).
//
// The APO's real-time thread therefore only ever performs atomic 64-bit
// counter loads and, on change, a plain memcpy -- no locks, no syscalls,
// no COM on the audio thread. Interlocked* ops are used instead of C++
// atomics so the protocol is identical in the C and C++ translation units
// that share this header.
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
// `sequence` is the seqlock counter: the writer brackets each update with
// InterlockedIncrement64 (odd = write in flight, even = struct consistent);
// the reader accepts a copy only if the counter is the same even value
// before and after. Must stay 8-byte aligned (it is: offset 0).
typedef struct EqSettings {
    int64_t          sequence;                       // seqlock version counter
    float            bandGainDb[MINIEQ_MAX_BANDS]; // per-band gain, dB
    float            masterGainDb;                 // master trim, dB
    int32_t          bypass;                       // 0 = process, 1 = bypass
    int32_t          numBands;                     // 5 or 10 (settings toggle)
    int32_t          virtualization;               // 0 = off, 1 = headphone crossfeed on
    int32_t          _reserved[5];
} EqSettings;

// "MiniEQ_{sanitized-endpoint-id}" -- caller supplies a buffer.
void MiniEQ_MappingNameForEndpoint(const wchar_t* endpointId,
                                  wchar_t* outName, size_t outNameChars);

// Fill an EqSettings with flat (no-op) values, sequence = 1.
void MiniEQ_SettingsInitFlat(EqSettings* s);

#ifdef __cplusplus
}
#endif
