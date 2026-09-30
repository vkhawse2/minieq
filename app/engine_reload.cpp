// engine_reload.cpp -- no-service audio-path reload (see engine_reload.h).

#include "engine_reload.h"

#include <audioclient.h>
#include <audiopolicy.h>
#include <propkey.h> // PROPERTYKEY
#include <mmdeviceapi.h>
#include <strsafe.h>

#include <vector>

#include "diag.h"

// PKEY_AudioEngine_DeviceFormat = {F19F064D-082C-4E27-BC73-6882A1BB8E4C}, 0.
// functiondiscoverykeys_devpkey.h only *declares* this key -- no import lib
// provides the definition, so referencing it is a link error (LNK2019).
// Define it TU-local instead (same pattern as eq_apo.cpp).
static const PROPERTYKEY kPkeyAudioEngineDeviceFormat = {
    { 0xF19F064D, 0x082C, 0x4E27,
      { 0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C } },
    0
};

// The build this UI was compiled from; CI stamps the short commit SHA.
#ifdef MINIEQ_BUILD_ID
#define MINIEQ_EXPECTED_BUILD MINIEQ_BUILD_ID
#else
#define MINIEQ_EXPECTED_BUILD "dev"
#endif

const char* MiniEQ_ExpectedBuildId() {
    return MINIEQ_EXPECTED_BUILD;
}

namespace {

bool ReadApoStatusOnce(const std::wstring& endpointId, MiniEQApoStatus* out) {
    wchar_t name[160] = {};
    MiniEQ_StatusNameForEndpoint(endpointId.c_str(), name, ARRAYSIZE(name));
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (h == nullptr) {
        return false;
    }
    void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(MiniEQApoStatus));
    bool ok = false;
    if (v != nullptr) {
        memcpy(out, v, sizeof(MiniEQApoStatus));
        ok = true;
        UnmapViewOfFile(v);
    }
    CloseHandle(h);
    return ok;
}

IMMDevice* OpenEndpoint(const std::wstring& endpointId) {
    IMMDeviceEnumerator* penum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&penum)))) {
        return nullptr;
    }
    IMMDevice* dev = nullptr;
    const HRESULT hr = penum->GetDevice(endpointId.c_str(), &dev);
    penum->Release();
    return SUCCEEDED(hr) ? dev : nullptr;
}

} // namespace

BuildFreshness MiniEQ_EngineBuildFreshness(const std::wstring& endpointId,
                                           std::wstring* reportedBuild) {
    MiniEQApoStatus st = {};
    if (!ReadApoStatusOnce(endpointId, &st) ||
        st.structSize < sizeof(MiniEQApoStatus) || st.version < 1) {
        return BuildFreshness::Unknown;
    }
    if (st.version < 2) {
        // Predates the build stamp: a one-time migration reload.
        if (reportedBuild != nullptr) {
            *reportedBuild = L"v1";
        }
        return BuildFreshness::Stale;
    }
    char id[16] = {};
    strncpy_s(id, ARRAYSIZE(id), st.buildId, _TRUNCATE);
    if (id[0] == '\0') {
        return BuildFreshness::Unknown; // channel just created, not stamped yet
    }
    if (strcmp(id, MINIEQ_EXPECTED_BUILD) == 0) {
        return BuildFreshness::Fresh;
    }
    if (reportedBuild != nullptr) {
        wchar_t w[16] = {};
        size_t n = 0;
        mbstowcs_s(&n, w, ARRAYSIZE(w), id, _TRUNCATE);
        *reportedBuild = w;
    }
    return BuildFreshness::Stale;
}

bool MiniEQ_AudioPlaying(const std::wstring& endpointId, std::wstring* activeName) {
    IMMDevice* dev = OpenEndpoint(endpointId);
    if (dev == nullptr) {
        return false;
    }
    IAudioSessionManager2* mgr = nullptr;
    HRESULT hr = dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&mgr));
    dev->Release();
    if (FAILED(hr)) {
        return false;
    }
    IAudioSessionEnumerator* senum = nullptr;
    hr = mgr->GetSessionEnumerator(&senum);
    mgr->Release();
    if (FAILED(hr)) {
        return false;
    }
    bool playing = false;
    int count = 0;
    if (SUCCEEDED(senum->GetCount(&count))) {
        for (int i = 0; i < count && !playing; ++i) {
            IAudioSessionControl* ctl = nullptr;
            if (FAILED(senum->GetSession(i, &ctl))) {
                continue;
            }
            AudioSessionState state = AudioSessionStateInactive;
            if (SUCCEEDED(ctl->GetState(&state)) && state == AudioSessionStateActive) {
                playing = true;
                if (activeName != nullptr) {
                    IAudioSessionControl2* ctl2 = nullptr;
                    wchar_t name[128] = {};
                    if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                      reinterpret_cast<void**>(&ctl2)))) {
                        LPWSTR disp = nullptr;
                        if (SUCCEEDED(ctl2->GetDisplayName(&disp)) && disp != nullptr &&
                            *disp != L'\0') {
                            StringCchCopyW(name, ARRAYSIZE(name), disp);
                        }
                        CoTaskMemFree(disp);
                        ctl2->Release();
                    }
                    *activeName = (name[0] != L'\0') ? name : L"an app";
                }
            }
            ctl->Release();
        }
    }
    senum->Release();
    return playing;
}

namespace {

bool ReadDeviceFormatBlob(IMMDevice* dev, std::vector<BYTE>* blobOut) {
    IPropertyStore* st = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &st))) {
        return false;
    }
    PROPVARIANT v;
    PropVariantInit(&v);
    const HRESULT hr = st->GetValue(kPkeyAudioEngineDeviceFormat, &v);
    st->Release();
    bool ok = false;
    if (SUCCEEDED(hr) && v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX) &&
        v.blob.pBlobData != nullptr) {
        blobOut->assign(v.blob.pBlobData, v.blob.pBlobData + v.blob.cbSize);
        ok = true;
    }
    PropVariantClear(&v);
    return ok;
}

bool WriteDeviceFormatBlob(IMMDevice* dev, const std::vector<BYTE>& blob,
                           std::wstring* detail) {
    IPropertyStore* st = nullptr;
    HRESULT hr = dev->OpenPropertyStore(STGM_WRITE, &st);
    if (FAILED(hr)) {
        if (detail != nullptr) {
            *detail = L"couldn't open the endpoint properties (access denied?)";
        }
        return false;
    }
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_BLOB;
    v.blob.cbSize = static_cast<ULONG>(blob.size());
    v.blob.pBlobData = const_cast<BYTE*>(blob.data());
    hr = st->SetValue(kPkeyAudioEngineDeviceFormat, v);
    if (SUCCEEDED(hr)) {
        hr = st->Commit(); // harmless when the store commits on release
    }
    st->Release();
    if (FAILED(hr)) {
        if (detail != nullptr) {
            *detail = L"the engine rejected the format change";
        }
        return false;
    }
    return true;
}

} // namespace

bool MiniEQ_FlipDefaultFormat(const std::wstring& endpointId, std::wstring* detail) {
    IMMDevice* dev = OpenEndpoint(endpointId);
    if (dev == nullptr) {
        if (detail != nullptr) {
            *detail = L"endpoint not found";
        }
        return false;
    }
    std::vector<BYTE> orig;
    if (!ReadDeviceFormatBlob(dev, &orig)) {
        dev->Release();
        if (detail != nullptr) {
            *detail = L"couldn't read the current default format";
        }
        return false;
    }
    const WAVEFORMATEX* wfx = reinterpret_cast<const WAVEFORMATEX*>(orig.data());
    if (wfx->wFormatTag != WAVE_FORMAT_PCM &&
        wfx->wFormatTag != WAVE_FORMAT_IEEE_FLOAT &&
        wfx->wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
        dev->Release();
        if (detail != nullptr) {
            *detail = L"unexpected endpoint format";
        }
        return false;
    }
    // A neighbouring standard rate; restored right after.
    static const DWORD kRates[] = { 44100, 48000, 96000, 192000, 32000 };
    DWORD alt = 0;
    for (DWORD r : kRates) {
        if (r != wfx->nSamplesPerSec) {
            alt = r;
            break;
        }
    }
    std::vector<BYTE> flipped = orig;
    WAVEFORMATEX* f2 = reinterpret_cast<WAVEFORMATEX*>(flipped.data());
    f2->nSamplesPerSec = alt;
    f2->nAvgBytesPerSec = alt * f2->nBlockAlign;

    bool ok = WriteDeviceFormatBlob(dev, flipped, detail);
    if (ok) {
        // Let the engine tear the graph down and rebuild it on the new
        // format before restoring the original.
        Sleep(900);
        ok = WriteDeviceFormatBlob(dev, orig, detail);
        if (!ok && detail != nullptr) {
            *detail = L"flipped, but couldn't restore the original format -- "
                      L"re-select it in Sound settings";
        }
    }
    dev->Release();
    return ok;
}

EngineReloadOutcome* MiniEQ_RunReloadJob(const std::wstring& endpointId, bool force) {
    EngineReloadOutcome* out = new EngineReloadOutcome();
    out->endpoint = endpointId;
    std::wstring reported;
    const bool stale =
        MiniEQ_EngineBuildFreshness(endpointId, &reported) == BuildFreshness::Stale;
    if (!force && !stale) {
        out->result = EngineReloadResult::UpToDate;
        return out;
    }
    out->reportedBuild = reported;
    if (!force) {
        std::wstring who;
        if (MiniEQ_AudioPlaying(endpointId, &who)) {
            out->result = EngineReloadResult::Deferred;
            out->activeSession = who;
            return out;
        }
    }
    std::wstring detail;
    if (!MiniEQ_FlipDefaultFormat(endpointId, &detail)) {
        out->result = EngineReloadResult::FlipError;
        out->detail = detail;
        return out;
    }
    if (stale) {
        // Verify: the rebuilt graph must load the installed DLL, which
        // stamps its build id into the status channel.
        for (int i = 0; i < 14; ++i) {
            Sleep(500);
            if (MiniEQ_EngineBuildFreshness(endpointId, nullptr) ==
                BuildFreshness::Fresh) {
                out->result = EngineReloadResult::Reloaded;
                return out;
            }
        }
        out->result = EngineReloadResult::Failed;
        wchar_t why[128] = {};
        StringCchPrintfW(why, ARRAYSIZE(why),
                         L"the engine still runs build %s after the reload",
                         reported.c_str());
        out->detail = why;
        return out;
    }
    // Forced refresh on an already-fresh (or never-loaded) engine -- the
    // post-attach chain: the flip itself rebuilt the graph so the new
    // registration takes effect. There is no build change to verify; the
    // heartbeat and the status pill confirm liveness from here.
    out->result = EngineReloadResult::Reloaded;
    return out;
}
