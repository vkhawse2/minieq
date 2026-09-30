// dsp.cpp -- biquad peaking-EQ implementation (RBJ Audio EQ Cookbook).

#include "dsp.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

EqDsp::EqDsp() {
    memset(m_bands, 0, sizeof(m_bands));
    memset(m_state, 0, sizeof(m_state));
    // Default to flat (unity) coefficients so a zeroed struct is a no-op.
    for (int b = 0; b < MINIEQ_MAX_BANDS; ++b) {
        m_bands[b].b0 = 1.0f;
    }
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
    Reset();
}

void EqDsp::PeakingCoeffs(float freqHz, float q, float gainDb,
                          float sampleRateHz, Biquad* out) {
    // RBJ "peakingEQ": https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
    const float A = powf(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * (float)M_PI * freqHz / sampleRateHz;
    const float alpha = sinf(w0) / (2.0f * q);
    const float cw0 = cosf(w0);

    const float b0 = 1.0f + alpha * A;
    const float b1 = -2.0f * cw0;
    const float b2 = 1.0f - alpha * A;
    const float a0 = 1.0f + alpha / A;
    const float a1 = -2.0f * cw0;
    const float a2 = 1.0f - alpha / A;

    out->b0 = b0 / a0;
    out->b1 = b1 / a0;
    out->b2 = b2 / a0;
    out->a1 = a1 / a0;
    out->a2 = a2 / a0;
}

void EqDsp::UpdateGains(const float bandGainDb[MINIEQ_MAX_BANDS], int numBands,
                      float masterGainDb) {
    if (numBands != MINIEQ_MAX_BANDS) {
        numBands = MINIEQ_NUM_BANDS; // only 5 or 10 are valid
    }
    m_numBands = numBands;
    for (int b = 0; b < numBands; ++b) {
        float g = bandGainDb[b];
        if (g < MINIEQ_GAIN_MIN_DB) g = MINIEQ_GAIN_MIN_DB;
        if (g > MINIEQ_GAIN_MAX_DB) g = MINIEQ_GAIN_MAX_DB;
        PeakingCoeffs(MiniEQ_BandFreq(numBands, b), MINIEQ_BAND_Q, g,
                      m_sampleRate, &m_bands[b]);
    }
    if (masterGainDb < MINIEQ_GAIN_MIN_DB) masterGainDb = MINIEQ_GAIN_MIN_DB;
    if (masterGainDb > MINIEQ_GAIN_MAX_DB) masterGainDb = MINIEQ_GAIN_MAX_DB;
    m_masterLinear = powf(10.0f, masterGainDb / 20.0f);
}

void EqDsp::Reset() {
    memset(m_state, 0, sizeof(m_state));
    if (m_xfeed) {
        m_xfeed->loL = m_xfeed->loR = 0.0f;
        m_xfeed->hiL = m_xfeed->hiR = 0.0f;
        m_xfeed->prevL = m_xfeed->prevR = 0.0f;
    }
}

void EqDsp::SetVirtualization(bool on) {
    if (!on) {
        m_xfeed.reset(); // free the state: zero RAM while the toggle is off
        return;
    }
    if (!m_xfeed) {
        m_xfeed = std::make_unique<XFeed>(); // allocate only on first enable
    }
    if (m_xfeed->sampleRate == m_sampleRate) {
        return; // coefficients already derived for this rate
    }
    // bs2b default preset ("700Hz, 4.5dB"): lowpass Fc = 700 Hz at
    // Gd = -6.75 dB; the highboost cutoff (~995 Hz) is derived for the
    // smoothest summed response. Coefficient derivation follows the bs2b
    // theory (bs2b.sourceforge.net): single-pole recursive filters
    //   lo[n] = a0*in[n] + b1*lo[n-1]
    //   hi[n] = a0h*in[n] + a1h*in[n-1] + b1h*hi[n-1]
    const double s = (double)m_sampleRate;
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
    m_xfeed->a0 = (float)(G * (1.0 - x));
    m_xfeed->b1 = (float)x;
    m_xfeed->a0h = (float)(1.0 - Gh * (1.0 - xh));
    m_xfeed->a1h = (float)(-xh);
    m_xfeed->b1h = (float)xh;
    m_xfeed->sampleRate = m_sampleRate;
    m_xfeed->loL = m_xfeed->loR = 0.0f;
    m_xfeed->hiL = m_xfeed->hiR = 0.0f;
    m_xfeed->prevL = m_xfeed->prevR = 0.0f;
}

void EqDsp::Process(float* interleaved, uint32_t numFrames, bool bypass) {
    if (interleaved == nullptr || numFrames == 0 || m_channels == 0) {
        return;
    }
    if (bypass) {
        return; // in-place: nothing to do
    }

    const uint32_t ch = m_channels;

    // (1) Optional headphone virtualization: bs2b-style Bauer crossfeed.
    // Runs BEFORE the EQ bands -- crossfeed rebuilds a speaker-like stereo
    // image, the EQ then shapes the final tonality. Stereo only. The whole
    // stage is skipped (one branch) when the toggle is off.
    if (m_xfeed && ch == 2) {
        if (m_xfeed->sampleRate != m_sampleRate) {
            SetVirtualization(true); // re-derive coeffs for the new rate
        }
        XFeed* xf = m_xfeed.get();
        const float a0 = xf->a0, b1 = xf->b1;
        const float a0h = xf->a0h, a1h = xf->a1h, b1h = xf->b1h;
        float loL = xf->loL, loR = xf->loR;
        float hiL = xf->hiL, hiR = xf->hiR;
        float prevL = xf->prevL, prevR = xf->prevR;
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
            frame[0] = hiL + loR; // each ear also hears the other's lows
            frame[1] = hiR + loL;
        }
        // Park state; flush denormals once per buffer, not per sample.
        xf->loL = (fabsf(loL) < 1e-30f) ? 0.0f : loL;
        xf->loR = (fabsf(loR) < 1e-30f) ? 0.0f : loR;
        xf->hiL = (fabsf(hiL) < 1e-30f) ? 0.0f : hiL;
        xf->hiR = (fabsf(hiR) < 1e-30f) ? 0.0f : hiR;
        xf->prevL = prevL;
        xf->prevR = prevR;
    }

    // (2) EQ bands + master gain.
    const float master = m_masterLinear;
    const int nb = m_numBands;

    for (uint32_t f = 0; f < numFrames; ++f) {
        float* frame = interleaved + (size_t)f * ch;
        for (uint32_t c = 0; c < ch; ++c) {
            float x = frame[c];
            for (int b = 0; b < nb; ++b) {
                const Biquad* k = &m_bands[b];
                Tdf2State* st = &m_state[c][b];
                // Transposed Direct Form II.
                const float y = k->b0 * x + st->s1;
                st->s1 = k->b1 * x + st->s2 - k->a1 * y;
                st->s2 = k->b2 * x - k->a2 * y;
                x = y;
            }
            frame[c] = x * master;
        }
    }
}
