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
}

void EqDsp::Process(float* interleaved, uint32_t numFrames, bool bypass) {
    if (interleaved == nullptr || numFrames == 0 || m_channels == 0) {
        return;
    }
    if (bypass) {
        return; // in-place: nothing to do
    }

    const uint32_t ch = m_channels;
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
