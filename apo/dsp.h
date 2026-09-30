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
#include <atomic>
#include <memory>

class EqDsp {
public:
    EqDsp();
    ~EqDsp();

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

    // Turn headphone virtualization on/off. RT-safe: this only arms a
    // request -- the actual heap allocation/free happens in
    // ServiceVirtualizationWorker(), which must be called periodically from
    // a NON-real-time thread. Until the worker publishes the state, audio
    // flows without crossfeed (a few ms at most).
    void SetVirtualization(bool on);

    // Background-thread service: performs the pending crossfeed allocation
    // (with coefficient derivation) and frees retired states. Call every
    // ~100 ms from a non-RT thread while the APO is locked for process.
    void ServiceVirtualizationWorker();

    // Synchronously drop any crossfeed state (live or retired). Call from a
    // non-RT thread (stream teardown / destruction), never from Process().
    void ShutdownVirtualization();

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
    // Lock-free handoff: the RT thread only ever *reads* m_xfeedLive and
    // publishes retirements through m_xfeedOrphan; the worker thread owns
    // every new/delete. Zero heap activity on the RT thread, zero RAM/CPU
    // while the toggle is off.
    static void DeriveXFeedCoeffs(XFeed* xf, float sampleRateHz);

    std::atomic<XFeed*> m_xfeedLive{nullptr};   // RT-readable crossfeed state
    std::atomic<XFeed*> m_xfeedOrphan{nullptr}; // retired; worker frees it
    std::atomic<bool>   m_virtWanted{false};   // toggle state (RT-written)
    std::atomic<bool>   m_allocReq{false};     // worker: allocation pending
};
