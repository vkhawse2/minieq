// dsp.h -- minimal peaking-EQ DSP for MiniEQ.
//
// A bank of up to MINIEQ_MAX_BANDS biquad peaking filters (RBJ cookbook) plus
// a master gain stage, with an optional bs2b-style Bauer crossfeed
// ("Virtualization") for headphone listening. One instance handles all
// channels of one audio stream. The active band count (5 or 10) comes from
// the settings toggle.
//
// Real-time notes: after construction, Process() performs no allocation and
// no syscalls. UpdateGains() only does arithmetic. The crossfeed state is
// heap-allocated lazily -- it exists only while virtualization is turned on,
// so the toggle costs zero RAM and zero per-sample CPU when off.

#pragma once

#include "../shared/settings_channel.h"
#include <stdint.h>
#include <memory>

class EqDsp {
public:
    EqDsp();
    ~EqDsp() = default;

    EqDsp(const EqDsp&) = delete;
    EqDsp& operator=(const EqDsp&) = delete;

    // (Re)configure for a stream format. Clears filter state.
    void Configure(float sampleRateHz, uint32_t numChannels);

    // Push new gains; recomputes biquad coefficients. Cheap: pure math.
    // numBands is 5 or 10 (clamped); only that many filters run in Process().
    void UpdateGains(const float bandGainDb[MINIEQ_MAX_BANDS], int numBands,
                     float masterGainDb);

    // Process `numFrames` interleaved float32 frames in place.
    // `bypass` short-circuits to a plain copy-free pass-through.
    void Process(float* interleaved, uint32_t numFrames, bool bypass);

    // Turn headphone virtualization on/off. Enabling allocates the tiny
    // crossfeed state (a few dozen bytes); disabling frees it again, so an
    // untouched toggle costs nothing. Safe to call any time.
    void SetVirtualization(bool on);

    // Drop filter state (call when the input goes silent).
    void Reset();

private:
    // Normalized biquad coefficients (a0 == 1): y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2
    struct Biquad {
        float b0, b1, b2, a1, a2;
    };
    // Transposed Direct Form II state, per channel per band.
    struct Tdf2State {
        float s1, s2;
    };

    static void PeakingCoeffs(float freqHz, float q, float gainDb,
                              float sampleRateHz, Biquad* out);

    float     m_sampleRate = 48000.0f;
    uint32_t  m_channels = 0;
    int       m_numBands = MINIEQ_NUM_BANDS;
    Biquad    m_bands[MINIEQ_MAX_BANDS];
    Tdf2State m_state[MINIEQ_MAX_CHANNELS][MINIEQ_MAX_BANDS];
    float     m_masterLinear = 1.0f;

    // ---- Optional headphone virtualization (bs2b-style Bauer crossfeed) ----
    // Lazily allocated: null until the user turns the toggle on.
    // Coefficients follow the bs2b derivation (default preset: lowpass
    // Fc = 700 Hz at Gd = -6.75 dB, highboost derived for Fc_h ~= 995 Hz):
    //   lo[n] = a0*in[n] + b1*lo[n-1]
    //   hi[n] = a0h*in[n] + a1h*in[n-1] + b1h*hi[n-1]
    //   outL  = hi(L) + lo(R),  outR = hi(R) + lo(L)   (stereo only)
    // The inter-channel time delay comes from the single-pole filters'
    // phase response itself (as in the analog Bauer/Linkwitz/Moy circuits),
    // which also avoids comb-filter coloration.
    struct XFeed {
        float sampleRate = 0.0f;
        float a0 = 0, b1 = 0;            // lowpass coeffs
        float a0h = 0, a1h = 0, b1h = 0; // highboost coeffs
        float loL = 0, loR = 0;          // lowpass states
        float hiL = 0, hiR = 0;          // highboost states
        float prevL = 0, prevR = 0;      // highboost x[n-1]
    };
    std::unique_ptr<XFeed> m_xfeed;
};
