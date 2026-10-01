// settings_channel.c -- mapping-name + flat-default helpers.

#include "settings_channel.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>  // _wtof
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h> // Interlocked* for the seqlock reader
#include <shlobj.h>  // SHGetFolderPathW for the machine INI path
#endif

// FNV-1a 64-bit over the endpoint ID's UTF-16 code units (little-endian
// byte order, so the digest is identical on every platform regardless of
// wchar_t size or signedness). A fixed-length hex digest replaces the old
// sanitize-and-truncate scheme, whose truncation could map two different
// endpoints onto the same channel name.
static uint64_t HashEndpointId(const wchar_t* s) {
    uint64_t h = 14695981039346656037ULL; // FNV offset basis
    for (; *s != L'\0'; ++s) {
        const uint16_t u = (uint16_t)*s;
        h ^= (uint64_t)(u & 0xFF);
        h *= 1099511628211ULL; // FNV prime
        h ^= (uint64_t)((u >> 8) & 0xFF);
        h *= 1099511628211ULL;
    }
    return h;
}

static void BuildHashedName(const wchar_t* prefix, const wchar_t* endpointId,
                            wchar_t* outName, size_t outNameChars) {
    const uint64_t h = HashEndpointId(endpointId);
    // 16 fixed hex digits: no truncation, no collisions short of a 64-bit
    // hash collision.
    swprintf(outName, outNameChars, L"%ls%016llx", prefix,
             (unsigned long long)h);
}

// The pre-hash naming scheme (sanitize + truncate), kept only so a new UI
// can still talk to an old APO during the upgrade window. New code must
// not create channels under these names.
static void BuildLegacyName(const wchar_t* prefix, const wchar_t* endpointId,
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
    // Global\ namespace: the APO runs inside the audio engine (session 0,
    // service identity) while the UI runs in the user's session. Local\
    // objects can never cross that boundary, so the APO -- which holds
    // SeCreateGlobalPrivilege -- creates both channels and the UI opens
    // them. (A user-session process cannot create Global\ objects.)
    static const wchar_t prefix[] = L"Global\\MiniEQ_";
    BuildHashedName(prefix, endpointId, outName, outNameChars);
}

void MiniEQ_StatusNameForEndpoint(const wchar_t* endpointId,
                                  wchar_t* outName, size_t outNameChars) {
    // See above: Global\ is required for the APO (session 0) <-> UI
    // (user session) channel.
    static const wchar_t prefix[] = L"Global\\MiniEQ_Status_";
    BuildHashedName(prefix, endpointId, outName, outNameChars);
}

void MiniEQ_LegacyMappingNameForEndpoint(const wchar_t* endpointId,
                                         wchar_t* outName,
                                         size_t outNameChars) {
    // Pre-hash scheme (sanitize + truncate). Open-only fallback for a new
    // UI talking to an old APO during the upgrade window; never create.
    static const wchar_t prefix[] = L"Global\\MiniEQ_";
    BuildLegacyName(prefix, endpointId, outName, outNameChars);
}

void MiniEQ_LegacyStatusNameForEndpoint(const wchar_t* endpointId,
                                        wchar_t* outName,
                                        size_t outNameChars) {
    // See above.
    static const wchar_t prefix[] = L"Global\\MiniEQ_Status_";
    BuildLegacyName(prefix, endpointId, outName, outNameChars);
}

void MiniEQ_GlobalStateName(wchar_t* outName, size_t outNameChars) {
    // Same Global\ requirement as the per-endpoint channels: the APO
    // (running in session 0) creates it; the UI (user session) only opens
    // it. One flat name -- no endpoint GUID -- because the flag is global.
    static const wchar_t name[] = L"Global\\MiniEQ__Enabled";
    BuildLegacyName(name, NULL, outName, outNameChars);
}

void MiniEQ_SettingsInitFlat(EqSettings* s) {    memset(s, 0, sizeof(*s));
    for (int i = 0; i < MINIEQ_MAX_BANDS; ++i) {
        s->bandGainDb[i] = 0.0f;
    }
    s->masterGainDb = 0.0f;
    s->bypass = 0;
    s->numBands = MINIEQ_NUM_BANDS; // default: 5-band
    s->virtualization = 0; // default: crossfeed off (zero DSP cost)
    // Even: a consistent snapshot. The seqlock protocol needs the counter
    // even whenever no write is in flight, including at creation.
    s->sequence = 0;
}

int MiniEQ_SettingsReadSeqlock(const EqSettings* src, EqSettings* out, int tries) {
    if (src == NULL || out == NULL || tries <= 0) {
        return 0;
    }
#ifdef _WIN32
    volatile LONG64* seqAddr = (volatile LONG64*)&src->sequence;
    for (int attempt = 0; attempt < tries; ++attempt) {
        // Atomic snapshot (InterlockedCompareExchange64 with 0,0 is a
        // full-barrier read on MSVC).
        const int64_t seq = InterlockedCompareExchange64(seqAddr, 0, 0);
        if (seq & 1) {
            continue; // writer mid-update; retry
        }
        EqSettings candidate;
        memcpy(&candidate, (const void*)src, sizeof(EqSettings));
        const int64_t seq2 = InterlockedCompareExchange64(seqAddr, 0, 0);
        if (seq2 != seq) {
            continue; // raced a writer; retry
        }
        memcpy(out, &candidate, sizeof(EqSettings));
        return 1;
    }
    return 0; // writer kept racing; caller keeps its previous state
#else
    memcpy(out, src, sizeof(EqSettings));
    return 1;
#endif
}

int MiniEQ_MachineIniPath(wchar_t* out, size_t outChars) {
    if (out == NULL || outChars == 0) {
        return 0;
    }
    out[0] = L'\0';
#ifdef _WIN32
    wchar_t base[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL,
                                SHGFP_TYPE_CURRENT, base))) {
        return 0;
    }
    // Keep the whole thing comfortably inside MAX_PATH.
    if (swprintf(out, outChars, L"%ls\\MiniEQ\\devices.ini", base) < 0) {
        out[0] = L'\0';
        return 0;
    }
    return 1;
#else
    return 0;
#endif
}

int MiniEQ_LoadDeviceSettingsFromIni(const wchar_t* iniPath,
                                     const wchar_t* endpointId,
                                     EqSettings* out) {
    if (iniPath == NULL || endpointId == NULL || out == NULL) {
        return 0;
    }
#ifdef _WIN32
    // Presence of Band0 decides whether this device was ever saved.
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(endpointId, L"Band0", L"", buf,
                             (DWORD)(sizeof(buf) / sizeof(buf[0])), iniPath);
    if (buf[0] == L'\0') {
        return 0; // never saved: caller keeps flat defaults
    }
    MiniEQ_SettingsInitFlat(out);
    // The INI is user-editable: validate every float before it can reach
    // the DSP. NaN comparisons are always false, so the !(x >= min) form
    // catches NaN as well as out-of-range values.
    for (int i = 0; i < MINIEQ_MAX_BANDS; ++i) {
        wchar_t key[16] = {};
        swprintf(key, sizeof(key) / sizeof(key[0]), L"Band%d", i);
        GetPrivateProfileStringW(endpointId, key, L"0", buf,
                                 (DWORD)(sizeof(buf) / sizeof(buf[0])), iniPath);
        float g = (float)_wtof(buf);
        if (!(g >= MINIEQ_GAIN_MIN_DB)) g = MINIEQ_GAIN_MIN_DB;
        if (!(g <= MINIEQ_GAIN_MAX_DB)) g = MINIEQ_GAIN_MAX_DB;
        out->bandGainDb[i] = g;
    }
    GetPrivateProfileStringW(endpointId, L"Master", L"0", buf,
                             (DWORD)(sizeof(buf) / sizeof(buf[0])), iniPath);
    float mg = (float)_wtof(buf);
    if (!(mg >= MINIEQ_GAIN_MIN_DB)) mg = MINIEQ_GAIN_MIN_DB;
    if (!(mg <= MINIEQ_GAIN_MAX_DB)) mg = MINIEQ_GAIN_MAX_DB;
    out->masterGainDb = mg;
    out->bypass = GetPrivateProfileIntW(endpointId, L"Bypass", 0, iniPath);
    const int nb = GetPrivateProfileIntW(endpointId, L"NumBands",
                                        MINIEQ_NUM_BANDS, iniPath);
    out->numBands = (nb == MINIEQ_MAX_BANDS) ? MINIEQ_MAX_BANDS
                                            : MINIEQ_NUM_BANDS;
    out->virtualization =
        GetPrivateProfileIntW(endpointId, L"Virtualization", 0, iniPath) ? 1 : 0;
    // sequence stays 0 (even = consistent) from MiniEQ_SettingsInitFlat.
    return 1;
#else
    (void)iniPath; (void)endpointId; (void)out;
    return 0;
#endif
}
