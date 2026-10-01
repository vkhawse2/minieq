// dsp.cpp -- biquad peaking-EQ implementation (RBJ Audio EQ Cookbook).

#include "dsp.h"

#include <math.h>
#include <new>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

EqDsp::EqDsp() {
    memset(m_bands, 0, sizeof(m_bands));
    memset(m_state, 0, sizeof(m_state));
    memset(m_oldBands, 0, sizeof(m_oldBands));
    memset(m_oldState, 0, sizeof(m_oldState));
    // Default to flat (unity) coefficients so a zeroed struct is a no-op.
    // (m_liveGainDb / m_targetGainDb are zero-initialized in the header.)
    for (int b = 0; b < MINIEQ_MAX_BANDS; ++b) {
        m_bands[b].b0 = 1.0f;
        m_oldBands[b].b0 = 1.0f;
    }
}

EqDsp::~EqDsp() {
    ShutdownVirtualization();
    delete[] m_scratch;
    m_scratch = nullptr;
    m_scratchFrames = 0;
}

void EqDsp::Configure(float sampleRateHz, uint32_t numChannels) {
    if (sampleRateHz < 8000.0f) {
        sampleRateHz = 48000.0f;
    }
    if (numChannels == 0 || numChannels > MINIEQ_MAX_CHANNELS) {
        numChannels = 2;
    }
    m_sampleRate = sampleRateHz;
    m_channels = numChannels;
    // Smoothing time constants from the real stream rate.
    m_smoothAlpha = 1.0f - expf(-1.0f / (0.008f * sampleRateHz)); // ~8 ms
    m_mixStep = 1.0f / (0.005f * sampleRateHz);                   // 5 ms
    m_xfMixStep = 1.0f / (0.010f * sampleRateHz);                 // 10 ms
    // Scratch buffer for the bypass dry/wet crossfade. Allocated here
    // (LockForProcess runs on a setup thread -- never the RT thread).
    delete[] m_scratch;
    m_scratch = nullptr;
    m_scratchFrames = 4096; // engine blocks are far smaller in practice
    m_scratch = new (std::nothrow) float[(size_t)m_scratchFrames * numChannels];
    if (m_scratch == nullptr) {
        m_scratchFrames = 0;
    }
    SnapCoeffs();
    m_mix = m_mixTarget = 1.0f;
    m_xfMix = m_xfMixTarget = 0.0f;
    // A format change is a fresh stream: drop any in-flight layout
    // crossfade and re-park the frozen bank at flat.
    m_layoutMix = 1.0f;
    m_layoutMixStep = 0.0f;
    m_layoutXfadeLeft = 0;
    m_oldNumBands = 0;
    m_oldMaster = 1.0f;
    for (int b = 0; b < MINIEQ_MAX_BANDS; ++b) {
        m_oldBands[b].b0 = 1.0f;
        m_oldBands[b].b1 = m_oldBands[b].b2 =
            m_oldBands[b].a1 = m_oldBands[b].a2 = 0.0f;
    }
    memset(m_oldState, 0, sizeof(m_oldState));
    Reset();
}

void EqDsp::PeakingCoeffs(float freqHz, float q, float gainDb,
                          float sampleRateHz, Biquad* out) {
    // RBJ "peakingEQ": https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
    // Computed in double: at 96 kHz+ the 60 Hz band's stability margin is
    // ~1.5e-5, thinner than float32 rounding. Double keeps the coefficients
    // safely inside the triangle.
    const double A = pow(10.0, (double)gainDb / 40.0);
    const double w0 = 2.0 * M_PI * (double)freqHz / (double)sampleRateHz;
    const double alpha = sin(w0) / (2.0 * (double)q);
    const double cw0 = cos(w0);

    const double b0 = 1.0 + alpha * A;
    const double b1 = -2.0 * cw0;
    const double b2 = 1.0 - alpha * A;
    const double a0 = 1.0 + alpha / A;
    const double a1 = -2.0 * cw0;
    const double a2 = 1.0 - alpha / A;

    out->b0 = (float)(b0 / a0);
    out->b1 = (float)(b1 / a0);
    out->b2 = (float)(b2 / a0);
    out->a1 = (float)(a1 / a0);
    out->a2 = (float)(a2 / a0);
}

void EqDsp::UpdateGains(const float bandGainDb[MINIEQ_MAX_BANDS], int numBands,
                      float masterGainDb) {
    if (numBands != MINIEQ_MAX_BANDS) {
        numBands = MINIEQ_NUM_BANDS; // only 5 or 10 are valid
    }
    if (numBands != m_numBands) {
        // Layout change (5<->10): freeze the live bank so it can keep
        // filtering during the crossfade, then rebuild the main bank for
        // the new layout with fresh state. The fade (started at 100% old
        // in Process) masks the new bank's startup transient; the gain
        // sweep below then moves the new bank to its targets click-free.
        // Pure arithmetic + fixed-size copies: RT-safe.
        for (int b = 0; b < MINIEQ_MAX_BANDS; ++b) {
            m_oldBands[b] = m_bands[b];
        }
        for (uint32_t c = 0; c < MINIEQ_MAX_CHANNELS; ++c) {
            for (int b = 0; b < MINIEQ_MAX_BANDS; ++b) {
                m_oldState[c][b] = m_state[c][b];
            }
        }
        m_oldNumBands = m_numBands;
        m_oldMaster = m_masterLinear;
        memset(m_state, 0, sizeof(m_state)); // new bank starts clean
        const float fadeFrames = 0.020f * m_sampleRate; // ~20 ms
        m_layoutMixStep = (fadeFrames > 1.0f) ? (1.0f / fadeFrames) : 1.0f;
        m_layoutMix = 0.0f;
        m_layoutXfadeLeft = (uint32_t)fadeFrames;
        if (m_layoutXfadeLeft == 0) {
            m_layoutXfadeLeft = 1;
        }
        m_coeffTick = 0;
        m_coeffsDirty = true;
    }
    m_numBands = numBands;
    for (int b = 0; b < numBands; ++b) {
        float g = bandGainDb[b];
        if (!(g >= MINIEQ_GAIN_MIN_DB)) g = MINIEQ_GAIN_MIN_DB; // also catches NaN
        if (!(g <= MINIEQ_GAIN_MAX_DB)) g = MINIEQ_GAIN_MAX_DB; // also catches NaN
        m_targetGainDb[b] = g;
    }
    // Bands above the active count park at flat, so a later 5->10 switch
    // sweeps from a known state instead of a stale gain.
    for (int b = numBands; b < MINIEQ_MAX_BANDS; ++b) {
        m_targetGainDb[b] = 0.0f;
    }
    if (!(masterGainDb >= MINIEQ_GAIN_MIN_DB)) masterGainDb = MINIEQ_GAIN_MIN_DB;
    if (!(masterGainDb <= MINIEQ_GAIN_MAX_DB)) masterGainDb = MINIEQ_GAIN_MAX_DB;
    m_targetMaster = powf(10.0f, masterGainDb / 20.0f);
    // The RT thread sweeps the live *gains* toward these targets in
    // Process() and recomputes coefficients via PeakingCoeffs() -- no instant
    // jump, no click, and every intermediate stays on the stable RBJ manifold.
    m_settling = true;
}

void EqDsp::SnapCoeffs() {
    for (int b = 0; b < MINIEQ_MAX_BANDS; ++b) {
        m_liveGainDb[b] = m_targetGainDb[b];
        PeakingCoeffs(MiniEQ_BandFreq(m_numBands, b), MINIEQ_BAND_Q,
                      BandEffectiveGain(b), m_sampleRate, &m_bands[b]);
    }
    m_masterLinear = m_targetMaster;
    m_coeffsDirty = false;
    m_settling = false;
}

void EqDsp::AdvanceCoeffs() {
    const float a = m_smoothAlpha;
    float maxDiff = 0.0f;
    for (int b = 0; b < MINIEQ_MAX_BANDS; ++b) {
        const float target = m_targetGainDb[b];
        float d = target - m_liveGainDb[b];
        if (fabsf(d) > 1e-7f) {
            m_liveGainDb[b] += d * a;
            maxDiff = fmaxf(maxDiff, fabsf(d));
            m_coeffsDirty = true;
        }
    }
    // Recompute coefficients at a decimated rate: the gain sweep runs
    // per-sample (cheap), but PeakingCoeffs uses transcendentals -- doing
    // it every 64th sample (~750 Hz @ 48 kHz) is smooth and RT-safe.
    // Every intermediate stays on the stable RBJ manifold (see header note).
    if (m_coeffsDirty && ((m_coeffTick++ & 63) == 0)) {
        for (int b = 0; b < MINIEQ_MAX_BANDS; ++b) {
            PeakingCoeffs(MiniEQ_BandFreq(m_numBands, b), MINIEQ_BAND_Q,
                          BandEffectiveGain(b), m_sampleRate, &m_bands[b]);
        }
        m_coeffsDirty = false;
    }
    const float dm = m_targetMaster - m_masterLinear;
    m_masterLinear += dm * a;
    maxDiff = fmaxf(maxDiff, fabsf(dm));
    if (maxDiff < 1e-7f) {
        SnapCoeffs(); // close enough: park exactly, stop sweeping
    }
}

void EqDsp::Reset() {
    memset(m_state, 0, sizeof(m_state));
    // No audio is flowing here (silence or between streams): park the
    // smoothed parameters exactly on their targets instead of sweeping.
    SnapCoeffs();
    m_mix = m_mixTarget;
    m_xfMix = m_xfMixTarget;
    if (m_xfMixTarget <= 0.0f) {
        // Crossfeed is (or is being) turned off: don't leave a half-faded
        // state parked -- hand it to the worker for freeing.
        XFeed* xf = m_xfeedLive.exchange(nullptr, std::memory_order_acq_rel);
        if (xf != nullptr) {
            m_xfeedOrphan.store(xf, std::memory_order_release);
        }
    } else {
        XFeed* xf = m_xfeedLive.load(std::memory_order_acquire);
        if (xf != nullptr) {
            xf->loL = xf->loR = 0.0f;
            xf->hiL = xf->hiR = 0.0f;
            xf->prevL = xf->prevR = 0.0f;
        }
    }
}

// RT-safe: only moves fade targets. The worker thread owns every
// new/delete (see ServiceVirtualizationWorker); the RT thread fades the
// stage in/out in Process() and retires the state once a fade-out
// completes -- never an instant switch mid-stream.
void EqDsp::SetVirtualization(bool on) {
    m_virtWanted.store(on, std::memory_order_release);
    if (on) {
        // Fade the crossfeed stage in (~10 ms) once the worker publishes it.
        m_xfMixTarget = 1.0f;
        // Request the worker to allocate+derive if nothing is live yet.
        // Until it publishes, Process() simply skips the crossfeed stage.
        if (m_xfeedLive.load(std::memory_order_acquire) == nullptr) {
            m_allocReq.store(true, std::memory_order_release);
        }
        return;
    }
    // Fade out on the RT thread; the state is retired to the worker when the
    // fade completes (see Process). No instant switch -> no click.
    m_xfMixTarget = 0.0f;
    m_allocReq.store(false, std::memory_order_release);
}

// Worker thread (non-RT): perform pending crossfeed alloc/free.
void EqDsp::ServiceVirtualizationWorker() {
    XFeed* orphan = m_xfeedOrphan.exchange(nullptr, std::memory_order_acq_rel);
    delete orphan;

    if (!m_allocReq.load(std::memory_order_acquire) ||
        !m_virtWanted.load(std::memory_order_acquire)) {
        return;
    }
    XFeed* xf = new (std::nothrow) XFeed();
    if (xf != nullptr) {
        DeriveXFeedCoeffs(xf, m_sampleRate);
    }
    // Re-check: the toggle may have been flipped while we allocated.
    if (xf != nullptr && m_allocReq.exchange(false, std::memory_order_acq_rel) &&
        m_virtWanted.load(std::memory_order_acquire)) {
        XFeed* old = m_xfeedLive.exchange(xf, std::memory_order_acq_rel);
        delete old; // never expected; avoid a leak if it happens
    } else {
        delete xf;
    }
}

void EqDsp::ShutdownVirtualization() {
    m_virtWanted.store(false, std::memory_order_release);
    m_allocReq.store(false, std::memory_order_release);
    XFeed* orphan = m_xfeedOrphan.exchange(nullptr, std::memory_order_acq_rel);
    delete orphan;
    XFeed* live = m_xfeedLive.exchange(nullptr, std::memory_order_acq_rel);
    delete live;
}

void EqDsp::DeriveXFeedCoeffs(XFeed* xf, float sampleRateHz) {
    // bs2b default preset ("700Hz, 4.5dB"): lowpass Fc = 700 Hz at
    // Gd = -6.75 dB; the highboost cutoff (~995 Hz) is derived for the
    // smoothest summed response. Coefficient derivation follows the bs2b
    // theory (bs2b.sourceforge.net): single-pole recursive filters
    //   lo[n] = a0*in[n] + b1*lo[n-1]
    //   hi[n] = a0h*in[n] + a1h*in[n-1] + b1h*hi[n-1]
    const double s = (double)sampleRateHz;
    const double Fc = 700.0;
    const double Gd = -6.75;
    const double Adh = -2.25;
    const double G = pow(10.0, Gd / 20.0);
    const double Ah = pow(10.0, Adh / 20.0);
    const double Gh = 1.0 - Ah;
    const double Gdh = 20.0 * log10(Gh);
    const double Fch = Fc * pow(2.0, (Gd - Gdh) / 12.0);
    const double x = exp(-2.0 * M_PI * Fc / s);
    const double xh = exp(-2.0 * M_PI * Fch / s);
    xf->a0 = (float)(G * (1.0 - x));
    xf->b1 = (float)x;
    xf->a0h = (float)(1.0 - Gh * (1.0 - xh));
    xf->a1h = (float)(-xh);
    xf->b1h = (float)xh;
    xf->sampleRate = sampleRateHz;
    xf->loL = xf->loR = 0.0f;
    xf->hiL = xf->hiR = 0.0f;
    xf->prevL = xf->prevR = 0.0f;
}

void EqDsp::Process(float* interleaved, uint32_t numFrames, bool bypass) {
    if (interleaved == nullptr || numFrames == 0 || m_channels == 0) {
        return;
    }

    // Bypass crossfade target: 1 = fully wet (EQ on), 0 = fully dry.
    m_mixTarget = bypass ? 0.0f : 1.0f;

    if (m_mix <= 0.0f && m_mixTarget <= 0.0f) {
        // Steady-state bypass: bit-transparent, zero CPU. Coefficients keep
        // tracking silently (snapped: nothing is audible) so un-bypassing
        // fades in from the right state.
        if (m_settling) {
            SnapCoeffs();
        }
        return;
    }

    const uint32_t ch = m_channels;
    const bool steadyWet = (m_mix >= 1.0f && m_mixTarget >= 1.0f);

    // A bypass transition needs the dry signal for the crossfade: copy the
    // block aside first. If the block is bigger than the scratch buffer,
    // fall back to an instant switch for that block (the engine uses small
    // blocks, so this is a just-in-case path, never the common one).
    float* dry = nullptr;
    if (!steadyWet && m_scratch != nullptr && numFrames <= m_scratchFrames) {
        dry = m_scratch;
        memcpy(dry, interleaved, (size_t)numFrames * ch * sizeof(float));
    }

    // (1) Optional headphone crossfeed: bs2b-style Bauer.
    // Runs BEFORE the EQ bands -- crossfeed rebuilds a speaker-like stereo
    // image, the EQ then shapes the final tonality. Stereo only. The whole
    // stage is skipped (one branch) when the toggle is off.
    //
    // RT-safe: only atomic loads; allocation/derivation happens on the
    // worker thread (ServiceVirtualizationWorker). The toggle fades the
    // stage in/out (~10 ms) so it never clicks.
    XFeed* xf = m_xfeedLive.load(std::memory_order_acquire);
    if (xf != nullptr && ch == 2) {
        if (xf->sampleRate != m_sampleRate &&
            m_virtWanted.load(std::memory_order_acquire)) {
            // Rate changed under us: retire the stale state to the worker
            // and request a fresh derivation. Audio passes through
            // un-crossfed until the worker publishes the new state, which
            // then fades in from dry (m_xfMix = 0 below).
            XFeed* stale = m_xfeedLive.exchange(nullptr, std::memory_order_acq_rel);
            if (stale != nullptr) {
                m_xfeedOrphan.store(stale, std::memory_order_release);
            }
            m_allocReq.store(true, std::memory_order_release);
            m_xfMix = 0.0f;
            xf = nullptr;
        }
    } else {
        xf = nullptr;
    }
    if (xf != nullptr) {
        const float a0 = xf->a0, b1 = xf->b1;
        const float a0h = xf->a0h, a1h = xf->a1h, b1h = xf->b1h;
        float loL = xf->loL, loR = xf->loR;
        float hiL = xf->hiL, hiR = xf->hiR;
        float prevL = xf->prevL, prevR = xf->prevR;
        float mix = m_xfMix;
        const float mixTarget = m_xfMixTarget;
        const float mixStep = m_xfMixStep;
        for (uint32_t f = 0; f < numFrames; ++f) {
            float* frame = interleaved + (size_t)f * 2;
            const float inL = frame[0];
            const float inR = frame[1];
            loL = a0 * inL + b1 * loL;
            loR = a0 * inR + b1 * loR;
            hiL = a0h * inL + a1h * prevL + b1h * hiL;
            hiR = a0h * inR + a1h * prevR + b1h * hiR;
            prevL = inL;
            prevR = inR;
            // Fade the crossfeed stage: mix = 0 is plain stereo.
            if (mix < mixTarget) {
                mix += mixStep;
                if (mix > mixTarget) mix = mixTarget;
            } else if (mix > mixTarget) {
                mix -= mixStep;
                if (mix < mixTarget) mix = mixTarget;
            }
            frame[0] = inL + mix * ((hiL + loR) - inL);
            frame[1] = inR + mix * ((hiR + loL) - inR);
        }
        // Park state; flush denormals once per buffer, not per sample.
        xf->loL = (fabsf(loL) < 1e-30f) ? 0.0f : loL;
        xf->loR = (fabsf(loR) < 1e-30f) ? 0.0f : loR;
        xf->hiL = (fabsf(hiL) < 1e-30f) ? 0.0f : hiL;
        xf->hiR = (fabsf(hiR) < 1e-30f) ? 0.0f : hiR;
        xf->prevL = prevL;
        xf->prevR = prevR;
        m_xfMix = mix;
        if (mix <= 0.0f && mixTarget <= 0.0f) {
            // Fade-out complete: hand the state to the worker for freeing.
            XFeed* gone = m_xfeedLive.exchange(nullptr, std::memory_order_acq_rel);
            if (gone != nullptr) {
                m_xfeedOrphan.store(gone, std::memory_order_release);
            }
        }
    }

    // (2) EQ bands + master gain. The live coefficients sweep toward their
    // targets once per frame (shared across channels): a short one-pole
    // ramp instead of an instant jump, so slider drags and preset switches
    // never crackle. A 5<->10 layout change additionally crossfades from
    // the frozen pre-switch bank (see UpdateGains): the new bank's fresh
    // state would otherwise start up audibly.
    const int nb = m_numBands;

    for (uint32_t f = 0; f < numFrames; ++f) {
        if (m_settling) {
            AdvanceCoeffs();
        }
        float* frame = interleaved + (size_t)f * ch;
        const float master = m_masterLinear;
        const bool layoutXfade = (m_layoutXfadeLeft > 0);
        float oldOut[MINIEQ_MAX_CHANNELS];
        for (uint32_t c = 0; c < ch; ++c) {
            float x = frame[c];
            if (layoutXfade) {
                // Frozen pre-switch bank, from the same dry input.
                float xo = x;
                for (int b = 0; b < m_oldNumBands; ++b) {
                    const Biquad* k = &m_oldBands[b];
                    Tdf2State* st = &m_oldState[c][b];
                    // Transposed Direct Form II.
                    const float y = k->b0 * xo + st->s1;
                    st->s1 = k->b1 * xo + st->s2 - k->a1 * y;
                    st->s2 = k->b2 * xo - k->a2 * y;
                    xo = y;
                }
                oldOut[c] = xo * m_oldMaster;
            }
            for (int b = 0; b < nb; ++b) {
                const Biquad* k = &m_bands[b];
                Tdf2State* st = &m_state[c][b];
                // Transposed Direct Form II.
                const float y = k->b0 * x + st->s1;
                st->s1 = k->b1 * x + st->s2 - k->a1 * y;
                st->s2 = k->b2 * x - k->a2 * y;
                x = y;
            }
            float wet = x * master;
            if (layoutXfade) {
                wet = oldOut[c] + m_layoutMix * (wet - oldOut[c]);
            }
            frame[c] = wet;
        }
        if (layoutXfade) {
            // Advance the fade once per frame (shared across channels).
            m_layoutMix += m_layoutMixStep;
            if (m_layoutMix >= 1.0f || --m_layoutXfadeLeft == 0) {
                m_layoutMix = 1.0f;
                m_layoutXfadeLeft = 0;
            }
        }
        // Bypass crossfade (~5 ms): dry <-> wet, no clicks.
        if (!steadyWet) {
            if (dry == nullptr) {
                m_mix = m_mixTarget; // oversized block: instant switch
            } else {
                if (m_mix < m_mixTarget) {
                    m_mix += m_mixStep;
                    if (m_mix > m_mixTarget) m_mix = m_mixTarget;
                } else if (m_mix > m_mixTarget) {
                    m_mix -= m_mixStep;
                    if (m_mix < m_mixTarget) m_mix = m_mixTarget;
                }
                const float m = m_mix;
                const float* dframe = dry + (size_t)f * ch;
                for (uint32_t c = 0; c < ch; ++c) {
                    const float wet = frame[c];
                    frame[c] = dframe[c] + m * (wet - dframe[c]);
                }
            }
        }
    }
}
