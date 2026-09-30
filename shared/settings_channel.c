// settings_channel.c -- mapping-name + flat-default helpers.

#include "settings_channel.h"

#include <string.h>
#include <wchar.h>

static void BuildMappingName(const wchar_t* prefix, const wchar_t* endpointId,
                             wchar_t* outName, size_t outNameChars) {
    size_t o = 0;
    for (size_t i = 0; prefix[i] != L'\0' && o + 1 < outNameChars; ++i) {
        outName[o++] = prefix[i];
    }
    if (endpointId != NULL) {
        for (size_t i = 0; endpointId[i] != L'\0' && o + 1 < outNameChars; ++i) {
            wchar_t c = endpointId[i];
            const int ok = (c >= L'0' && c <= L'9') ||
                           (c >= L'A' && c <= L'Z') ||
                           (c >= L'a' && c <= L'z');
            outName[o++] = ok ? c : L'_';
        }
    }
    outName[o] = L'\0';
}

void MiniEQ_MappingNameForEndpoint(const wchar_t* endpointId,
                                  wchar_t* outName, size_t outNameChars) {
    static const wchar_t prefix[] = L"Local\\MiniEQ_";
    BuildMappingName(prefix, endpointId, outName, outNameChars);
}

void MiniEQ_StatusNameForEndpoint(const wchar_t* endpointId,
                                  wchar_t* outName, size_t outNameChars) {
    static const wchar_t prefix[] = L"Local\\MiniEQ_Status_";
    BuildMappingName(prefix, endpointId, outName, outNameChars);
}

void MiniEQ_SettingsInitFlat(EqSettings* s) {
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < MINIEQ_MAX_BANDS; ++i) {
        s->bandGainDb[i] = 0.0f;
    }
    s->masterGainDb = 0.0f;
    s->bypass = 0;
    s->numBands = MINIEQ_NUM_BANDS; // default: 5-band
    s->virtualization = 0; // default: crossfeed off (zero DSP cost)
    s->sequence = 1;
}
