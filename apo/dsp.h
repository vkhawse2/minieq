// dsp.h -- minimal peaking-EQ DSP for MiniEQ.
//
// A bank of up to MINIEQ_MAX_BANDS biquad peaking filters (RBJ cookbook) plus
// a master gain stage, with an optional bs2b-style Bauer crossfeed
// ("Crossfeed") for headphone listening. One instance handles all
// channels of one audio stream. The active band count (5 or 10) comes from
// the settings toggle.
//
// Real-time notes: after construction, Process() performs no allocation and
// no syscalls. UpdateGains() only does arithmetic. The crossfeed state is
// heap-allocated lazily -- it exists only while crossfeed is turned on,
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
    // Click-free: the new coefficients become TARGETS -- Process() sweeps
    // the live ones toward them with a short one-pole ramp, so slider drags
    // and preset switches never produce clicks. RT-safe (pure arithmetic).
    void UpdateGains(const float bandGainDb[MINIEQ_MAX_BANDS], int numBands,
                     float masterGainDb);

    // Process `numFrames` interleaved float32 frames in place.
    // `bypass` crossfades to a bit-transparent pass-through (~5 ms); the
    // steady bypassed state costs nothing.
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

    // A band whose center sits at/above 45% of the sample rate is disabled
    // (its coefficients stay flat at 0 dB): RBJ peaking coefficients go
    // unstable near Nyquist, and a 14/16 kHz band is meaningless at an
    // 8/16 kHz stream rate. The band's gains keep sweeping/storing normally
    // so a later format change to a higher rate brings it back correctly.
    bool BandNyquistSafe(int band) const {
        return MiniEQ_BandFreq(m_numBands, band) < 0.45f * m_sampleRate;
    }
    // Effective gain for coefficient computation: 0 dB for Nyquist-unsafe
    // bands, the live (swept) gain otherwise.
    float BandEffectiveGain(int band) const {
        return BandNyquistSafe(band) ? m_liveGainDb[band] : 0.0f;
    }

    // ---- Click-free parameter changes ----
    // The live coefficients (m_bands / m_masterLinear) are swept toward
    // their targets once per audio frame with a one-pole ramp (~8 ms time
    // constant). An instant coefficient jump with stale filter state is
    // what made slider/preset moves crackle; the sweep keeps the transient
    // below audibility. SnapCoeffs() parks the live set exactly on the
    // targets -- used when no audio is flowing (bypassed/silent/reconfig),
    // where a sweep would be pointless.
    void SnapCoeffs();
    void AdvanceCoeffs();

    float     m_sampleRate = 48000.0f;
    uint32_t  m_channels = 0;
    int       m_numBands = MINIEQ_NUM_BANDS;
    Biquad    m_bands[MINIEQ_MAX_BANDS];       // live coefficients (RT)
    float     m_liveGainDb[MINIEQ_MAX_BANDS] = {};  // live gains, swept smoothly
    float     m_targetGainDb[MINIEQ_MAX_BANDS] = {};// where UpdateGains() writes
    // NOTE: we sweep the *gains* (dB), not raw coefficients. Interpolating
    // raw biquad coefficients draws a straight line through coefficient space
    // that can exit the stability triangle at high sample rates (96 kHz+),
    // causing blowup. Recomputing via PeakingCoeffs() keeps every intermediate
    // on the stable RBJ manifold (the Equalizer APO approach).
    Tdf2State m_state[MINIEQ_MAX_CHANNELS][MINIEQ_MAX_BANDS];
    float     m_masterLinear = 1.0f;           // live master gain
    float     m_targetMaster = 1.0f;           // smoothed toward this too
    bool      m_settling = false;              // live coeffs chasing targets
    float     m_smoothAlpha = 0.0026f;         // one-pole step (~8 ms @ 48 kHz)
    bool      m_coeffsDirty = false;           // gains moved: recompute coeffs
    uint32_t  m_coeffTick = 0;                 // decimates the recompute

    // ---- 5<->10 band layout crossfade ----
    // Switching band count moves every center frequency at once. Keeping
    // the old filter states under the new coefficients would glitch, so
    // the pre-switch bank (coefficients + state + master) is frozen into
    // m_oldBands/m_oldState and keeps filtering while a short (~20 ms)
    // crossfade blends to the rebuilt main bank. The new bank starts with
    // fresh (zeroed) state, so its startup transient is fully masked by
    // the fade, which begins at 100% old. Fixed-size, allocation-free,
    // RT-safe. A second switch mid-fade simply re-freezes from the main
    // bank's live state (the dropped in-flight residual is bounded and
    // brief -- far below the click the old instant switch made).
    Biquad    m_oldBands[MINIEQ_MAX_BANDS];
    Tdf2State m_oldState[MINIEQ_MAX_CHANNELS][MINIEQ_MAX_BANDS];
    int       m_oldNumBands = 0;
    float     m_oldMaster = 1.0f;
    float     m_layoutMix = 1.0f;      // 1 = new bank only, 0 = old only
    float     m_layoutMixStep = 0.0f;  // per-frame fade step
    uint32_t  m_layoutXfadeLeft = 0;  // frames remaining in the crossfade

    // ---- Bypass crossfade ----
    // The old code hard-switched between dry and wet: a click. Now the
    // bypass control ramps a dry/wet mix over ~5 ms. A transition needs the
    // dry signal, so the block is copied to m_scratch first (allocated in
    // Configure, non-RT). Steady-state bypass is still a zero-CPU early-out.
    float     m_mix = 1.0f;                    // 1 = fully wet, 0 = fully dry
    float     m_mixTarget = 1.0f;
    float     m_mixStep = 1.0f / 240.0f;       // 5 ms @ 48 kHz
    float*    m_scratch = nullptr;
    uint32_t  m_scratchFrames = 0;

    // ---- Optional headphone crossfeed (bs2b-style Bauer) ----
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
    //
    // The toggle itself is click-free: SetVirtualization() only moves
    // m_xfMixTarget, and Process() fades the stage in/out (~10 ms). The RT
    // thread retires the crossfeed state to the worker once a fade-out
    // completes -- never an instant switch mid-stream.
    static void DeriveXFeedCoeffs(XFeed* xf, float sampleRateHz);

    float m_xfMix = 0.0f;             // crossfeed fade: 1 = full, 0 = dry
    float m_xfMixTarget = 0.0f;
    float m_xfMixStep = 1.0f / 480.0f; // 10 ms @ 48 kHz

    std::atomic<XFeed*> m_xfeedLive{nullptr};   // RT-readable crossfeed state
    std::atomic<XFeed*> m_xfeedOrphan{nullptr}; // retired; worker frees it
    std::atomic<bool>   m_virtWanted{false};   // toggle state (RT-written)
    std::atomic<bool>   m_allocReq{false};     // worker: allocation pending
};
