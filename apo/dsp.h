// dsp.h -- minimal peaking-EQ DSP for MiniEQ.
//
// A bank of MINIEQ_NUM_BANDS biquad peaking filters (RBJ cookbook) plus a
// master gain stage. One instance handles all channels of one audio stream.
//
// Real-time notes: after construction, Process() performs no allocation and
// no syscalls. UpdateGains() only does arithmetic. All state lives in
// fixed-size member arrays, so the object is safe to use on the APO's
// real-time audio thread.

#pragma once

#include "../shared/settings_channel.h"
#include <stdint.h>

class EqDsp {
public:
    EqDsp();
    ~EqDsp() = default;

    EqDsp(const EqDsp&) = delete;
    EqDsp& operator=(const EqDsp&) = delete;

    // (Re)configure for a stream format. Clears filter state.
    void Configure(float sampleRateHz, uint32_t numChannels);

    // Push new gains; recomputes biquad coefficients. Cheap: pure math.
    void UpdateGains(const float bandGainDb[MINIEQ_NUM_BANDS], float masterGainDb);

    // Process `numFrames` interleaved float32 frames in place.
    // `bypass` short-circuits to a plain copy-free pass-through.
    void Process(float* interleaved, uint32_t numFrames, bool bypass);

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
    Biquad    m_bands[MINIEQ_NUM_BANDS];
    Tdf2State m_state[MINIEQ_MAX_CHANNELS][MINIEQ_NUM_BANDS];
    float     m_masterLinear = 1.0f;
};
