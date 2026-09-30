// eq_apo.cpp -- CEqApo implementation.
//
// Modeled on Microsoft's SwapAPO sample (audio/sysvad/APO/SwapAPO), replacing
// the channel-swap DSP with our biquad EQ bank and adding the shared-memory
// settings channel for live UI control.
//
// COM model (do not "simplify"): the audio engine AGGREGATES system-effect
// APOs -- CreateInstance arrives with a non-null controlling unknown and
// IID_IUnknown -- and then requires IAudioSystemEffects3 plus apartment
// agility (free-threaded marshaler + IAgileObject). See eq_apo.h.

#include "eq_apo.h"
#include "guids.h"
#include "trace.h"

#include <mmdeviceapi.h>
#include <ks.h>          // must come before ksmedia.h
#include <ksmedia.h>   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
#include <strsafe.h>
#include <sddl.h>      // ConvertStringSecurityDescriptorToSecurityDescriptorW
#include <xmmintrin.h> // _mm_getcsr/_mm_setcsr: FTZ|DAZ denormal safety on RT threads

// Build id stamped into the APO status channel so the UI can tell a stale
// loaded DLL apart from the installed one. CI passes the short commit SHA
// via -DMINIEQ_BUILD_ID; local builds get "dev".
static const char* MiniEQ_ApoBuildId() {
#ifdef MINIEQ_BUILD_ID
    return MINIEQ_BUILD_ID;
#else
    return "dev";
#endif
}

// PKEY_AudioEndpoint_GUID = {[1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E}, 4}.
// functiondiscoverykeys_devpkey.h only *declares* this key -- no import
// lib provides the definition, so referencing it is a link error
// (LNK2019). Define it TU-local instead.
static const PROPERTYKEY kPkeyAudioEndpointGuid = {
    { 0x1DA5D803, 0xD492, 0x4EDD, { 0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E } },
    4
};

#include <cstring>

//------------------------------------------------------------------------------
// APO error-code compatibility. The APOERR_* names have shifted between SDK
// generations; pin down the ones we rely on so the build works regardless of
// which Windows SDK is installed.
//------------------------------------------------------------------------------
#ifndef APOERR_APO_LOCKED
#define APOERR_APO_LOCKED E_UNEXPECTED
#endif
// No APOERR_* name exists for double-Initialize in every SDK; use the Win32
// "already initialized" code wrapped as an HRESULT instead.
#define MINIEQ_HR_ALREADY_INITIALIZED HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED)

//------------------------------------------------------------------------------
// IUnknown -- delegating (forwards to the controlling unknown) and
// non-delegating (the real one; owns the object).
//------------------------------------------------------------------------------

STDMETHODIMP CEqApo::QueryInterface(REFIID riid, void** ppv) {
    return m_pUnkOuter->QueryInterface(riid, ppv);
}

STDMETHODIMP_(ULONG) CEqApo::AddRef() {
    return m_pUnkOuter->AddRef();
}

STDMETHODIMP_(ULONG) CEqApo::Release() {
    return m_pUnkOuter->Release();
}

HRESULT CEqApo::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (ppv == nullptr) {
        return E_POINTER;
    }
    *ppv = nullptr;

    if (riid == IID_IUnknown) {
        *ppv = static_cast<IUnknown*>(&m_inner); // identity
    } else if (riid == __uuidof(IAudioProcessingObject)) {
        *ppv = static_cast<IAudioProcessingObject*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else if (riid == __uuidof(IAudioSystemEffects)) {
        *ppv = static_cast<IAudioSystemEffects*>(
            static_cast<IAudioSystemEffects3*>(this));
    } else if (riid == __uuidof(IAudioSystemEffects2)) {
        *ppv = static_cast<IAudioSystemEffects2*>(
            static_cast<IAudioSystemEffects3*>(this));
    } else if (riid == __uuidof(IAudioSystemEffects3)) {
        *ppv = static_cast<IAudioSystemEffects3*>(this);
    } else if (riid == __uuidof(IAgileObject)) {
        *ppv = static_cast<IUnknown*>(&m_inner);
    } else if (riid == __uuidof(IMarshal) && m_pFTM != nullptr) {
        return m_pFTM->QueryInterface(riid, ppv);
    } else {
        return E_NOINTERFACE;
    }
    m_inner.AddRef();
    return S_OK;
}

CEqApo::CEqApo(IUnknown* pUnkOuter)
    : m_inner(this)
    , m_pUnkOuter(pUnkOuter != nullptr ? pUnkOuter
                                     : static_cast<IUnknown*>(&m_inner)) {
    // ThreadingModel "Both": the engine may call from any apartment, so
    // aggregate the free-threaded marshaler instead of making COM build a
    // proxy -- refusing IMarshal/IAgileObject stalls the handshake.
    CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(&m_inner), &m_pFTM);
    MiniEQ_SettingsInitFlat(&m_localCopy);
    m_lastSequence = m_localCopy.sequence;
    MiniEQ_Trace(L"MiniEQ_APO: CEqApo constructed");
}

CEqApo::~CEqApo() {
    StopWorker();
    CloseStatusMapping();
    CloseGlobalMapping();
    const EqSettings* settings = m_pSettings.exchange(nullptr);
    if (settings != nullptr) {
        UnmapViewOfFile(settings);
    }
    if (m_hMap != nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
    }
    m_dsp.ShutdownVirtualization();
    if (m_pFTM != nullptr) {
        m_pFTM->Release();
        m_pFTM = nullptr;
    }
}

//------------------------------------------------------------------------------
// IAudioSystemEffects(2/3). Our own UI drives everything, so there is nothing
// to enumerate -- an empty list is a valid answer; refusing is not.
//------------------------------------------------------------------------------

STDMETHODIMP CEqApo::GetEffectsList(GUID** ppEffectsIds, UINT* pcEffects,
                                   HANDLE /*hEvent*/) {
    MiniEQ_Trace(L"MiniEQ_APO: GetEffectsList called");
    if (ppEffectsIds == nullptr || pcEffects == nullptr) {
        return E_POINTER;
    }
    *ppEffectsIds = nullptr;
    *pcEffects = 0;
    return S_OK;
}

STDMETHODIMP CEqApo::GetControllableSystemEffectsList(AUDIO_SYSTEMEFFECT** ppEffects,
                                                     UINT* pcEffects,
                                                     HANDLE /*hEvent*/) {
    MiniEQ_Trace(L"MiniEQ_APO: GetControllableSystemEffectsList called");
    if (ppEffects == nullptr || pcEffects == nullptr) {
        return E_POINTER;
    }
    *ppEffects = nullptr;
    *pcEffects = 0;
    return S_OK;
}

STDMETHODIMP CEqApo::SetAudioSystemEffectState(GUID /*effectId*/,
                                               AUDIO_SYSTEMEFFECT_STATE /*state*/) {
    return S_OK;
}

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

bool CEqApo::IsFloat32Format(const WAVEFORMATEX* wfx) {
    if (wfx == nullptr) {
        return false;
    }
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32) {
        return true;
    }
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
            ext->Format.wBitsPerSample == 32) {
            return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------------
// IAudioProcessingObject
//------------------------------------------------------------------------------

STDMETHODIMP CEqApo::Initialize(UINT32 cbDataSize, BYTE* pbyData) noexcept
try {
    MiniEQ_Trace(L"MiniEQ_APO: Initialize cbDataSize=%lu pbyData=%s", cbDataSize,
                 pbyData != nullptr ? L"ok" : L"null");
    if (m_initialized) {
        MiniEQ_Trace(L"MiniEQ_APO: Initialize -> already initialized");
        return MINIEQ_HR_ALREADY_INITIALIZED;
    }
    if (pbyData == nullptr || cbDataSize == 0) {
        MiniEQ_Trace(L"MiniEQ_APO: Initialize -> E_INVALIDARG (null/empty data)");
        return E_INVALIDARG;
    }

    // pAPOEndpointProperties is the first field after APOInitBaseStruct in
    // APOInitSystemEffects (v1), APOInitSystemEffects2 AND
    // APOInitSystemEffects3 -- the one layout anchor that is stable across
    // versions. (The v3 struct the Win11 engine passes once we claim
    // IAudioSystemEffects3 is 80 bytes, SMALLER than v2's 88, so a
    // sizeof(v2) check -- or reading pDeviceCollection at the v2 offset --
    // rejects/misreads it and the APO can never initialize.)
    struct InitHead {
        APOInitBaseStruct base;
        IPropertyStore* pAPOEndpointProperties;
    };
    if (cbDataSize < sizeof(InitHead)) {
        MiniEQ_Trace(L"MiniEQ_APO: Initialize -> E_INVALIDARG (cbDataSize %lu < InitHead %zu)",
                     cbDataSize, sizeof(InitHead));
        return E_INVALIDARG;
    }
    const InitHead* head = reinterpret_cast<const InitHead*>(pbyData);
    if (head->pAPOEndpointProperties == nullptr) {
        MiniEQ_Trace(L"MiniEQ_APO: Initialize -> E_INVALIDARG (null endpoint props)");
        return E_INVALIDARG;
    }

    // Read our endpoint's GUID from its own property store.
    wchar_t epGuid[64] = {};
    PROPVARIANT var;
    PropVariantInit(&var);
    HRESULT hr = head->pAPOEndpointProperties->GetValue(kPkeyAudioEndpointGuid, &var);
    if (SUCCEEDED(hr) && var.vt == VT_LPWSTR && var.pwszVal != nullptr) {
        wcsncpy_s(epGuid, ARRAYSIZE(epGuid), var.pwszVal, _TRUNCATE);
    }
    PropVariantClear(&var);
    MiniEQ_Trace(L"MiniEQ_APO: Initialize endpoint GUID from props = \"%s\"", epGuid);
    // Never fail the user's audio stream over endpoint identification: if we
    // cannot resolve the endpoint, the APO still loads and processes audio
    // (with flat EQ) -- only the UI's live channel stays unavailable.
    if (epGuid[0] != L'\0') {
        // Find the IMMDevice carrying that GUID. (The old code assumed "our
        // endpoint is the last device in the collection" -- wrong on any machine
        // with more than one audio endpoint: the mapping name would be built
        // from the wrong device and the UI's settings would never arrive.)
        IMMDeviceEnumerator* pEnum = nullptr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
        if (FAILED(hr) || pEnum == nullptr) {
            MiniEQ_Trace(L"MiniEQ_APO: Initialize WARNING: MMDeviceEnumerator failed hr=0x%08X; no channels",
                         FAILED(hr) ? hr : (HRESULT)E_UNEXPECTED);
        } else {
            IMMDeviceCollection* pColl = nullptr;
            hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pColl);
            pEnum->Release();
            if (FAILED(hr) || pColl == nullptr) {
                MiniEQ_Trace(L"MiniEQ_APO: Initialize WARNING: EnumAudioEndpoints failed hr=0x%08X; no channels",
                             FAILED(hr) ? hr : (HRESULT)E_UNEXPECTED);
            } else {
                UINT32 count = 0;
                pColl->GetCount(&count);
                for (UINT32 i = 0; i < count; ++i) {
                    IMMDevice* pDev = nullptr;
                    if (FAILED(pColl->Item(i, &pDev)) || pDev == nullptr) {
                        continue;
                    }
                    bool match = false;
                    IPropertyStore* pStore = nullptr;
                    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pStore)) && pStore != nullptr) {
                        PROPVARIANT v2;
                        PropVariantInit(&v2);
                        if (SUCCEEDED(pStore->GetValue(kPkeyAudioEndpointGuid, &v2)) &&
                            v2.vt == VT_LPWSTR && v2.pwszVal != nullptr &&
                            _wcsicmp(v2.pwszVal, epGuid) == 0) {
                            match = true;
                        }
                        PropVariantClear(&v2);
                        pStore->Release();
                    }
                    if (match) {
                        LPWSTR id = nullptr;
                        if (SUCCEEDED(pDev->GetId(&id)) && id != nullptr) {
                            m_endpointId = id;
                            CoTaskMemFree(id);
                        }
                        pDev->Release();
                        break;
                    }
                    pDev->Release();
                }
                pColl->Release();
            }
        }
    } else {
        MiniEQ_Trace(L"MiniEQ_APO: Initialize WARNING: empty endpoint GUID; no channels");
    }

    if (!m_endpointId.empty()) {
        MiniEQ_MappingNameForEndpoint(m_endpointId.c_str(), m_mappingName,
                                     ARRAYSIZE(m_mappingName));
        MiniEQ_StatusNameForEndpoint(m_endpointId.c_str(), m_statusName,
                                     ARRAYSIZE(m_statusName));
    }
    // Global on/off flag: one flat name for every endpoint. The APO creates
    // it in LockForProcess -- it must be the creator, because only session 0
    // holds SeCreateGlobalPrivilege; the UI (user session) only opens it.
    MiniEQ_GlobalStateName(m_globalName, ARRAYSIZE(m_globalName));
    MiniEQ_Trace(L"MiniEQ_APO: Initialize -> S_OK device=\"%s\" mapping=\"%s\"",
                 m_endpointId.empty() ? L"<NO MATCH>" : m_endpointId.c_str(),
                 m_mappingName[0] ? m_mappingName : L"<none>");

    m_initialized = true;
    return S_OK;
} catch (...) {
    // Never let a C++ exception cross the COM boundary: the audio engine
    // (audiodg.exe) hosts this DLL in-process, and an escaping exception
    // would terminate the whole engine. Fail the init loudly instead.
    MiniEQ_Trace(L"MiniEQ_APO: Initialize swallowed C++ exception -> E_FAIL");
    return E_FAIL;
}

STDMETHODIMP CEqApo::IsInputFormatSupported(IAudioMediaType* /*pOppositeFormat*/,
                                            IAudioMediaType* pRequestedInputFormat,
                                            IAudioMediaType** ppSupportedInputFormat) {
    if (ppSupportedInputFormat == nullptr) {
        return E_POINTER;
    }
    *ppSupportedInputFormat = nullptr;
    if (pRequestedInputFormat == nullptr) {
        return E_INVALIDARG;
    }
    // IAudioMediaType::GetAudioFormat returns the format owned by the media
    // type (no out-param, no transfer of ownership -- do not free it).
    const WAVEFORMATEX* wfx = pRequestedInputFormat->GetAudioFormat();
    if (wfx == nullptr) {
        return E_INVALIDARG;
    }
    MiniEQ_Trace(L"MiniEQ_APO: IsInputFormatSupported tag=%u bits=%u ch=%u rate=%lu float32=%d",
                 wfx->wFormatTag, wfx->wBitsPerSample, wfx->nChannels,
                 wfx->nSamplesPerSec, IsFloat32Format(wfx) ? 1 : 0);
    if (!IsFloat32Format(wfx)) {
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    // Our DSP state is sized for MINIEQ_MAX_CHANNELS channels: decline wider
    // formats honestly instead of misprocessing them.
    if (wfx->nChannels == 0 || wfx->nChannels > MINIEQ_MAX_CHANNELS) {
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    // Negotiation policy: accept-and-adapt. Any engine-proposed sample rate
    // is accepted -- EqDsp::Configure derives its smoothing constants from
    // the real rate (absurd rates are clamped there, never here) -- and any
    // channel count our fixed RT-safe state supports (1..MINIEQ_MAX_CHANNELS,
    // covering stereo / 5.1 / 7.1 spatial layouts). Rejecting a format makes
    // the engine silently drop the APO, so we only decline what we genuinely
    // cannot process: non-float32, zero channels, or wider than our state.
    // In-place SFX: we accept the requested format as-is.
    *ppSupportedInputFormat = pRequestedInputFormat;
    (*ppSupportedInputFormat)->AddRef();
    return S_OK;
}

STDMETHODIMP CEqApo::IsOutputFormatSupported(IAudioMediaType* pOppositeFormat,
                                            IAudioMediaType* pRequestedOutputFormat,
                                            IAudioMediaType** ppSupportedOutputFormat) {
    // Same format in and out.
    return IsInputFormatSupported(pOppositeFormat, pRequestedOutputFormat,
                                 ppSupportedOutputFormat);
}

STDMETHODIMP CEqApo::GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps) {
    if (ppRegProps == nullptr) {
        return E_POINTER;
    }
    APO_REG_PROPERTIES* p = static_cast<APO_REG_PROPERTIES*>(
        CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
    if (p == nullptr) {
        return E_OUTOFMEMORY;
    }
    p->clsid = CLSID_MiniEQAPO;
    p->Flags = APO_FLAG_INPLACE;
    StringCchCopyW(p->szFriendlyName,
                   ARRAYSIZE(p->szFriendlyName), L"MiniEQ");
    StringCchCopyW(p->szCopyrightInfo,
                   ARRAYSIZE(p->szCopyrightInfo), L"Copyright (c) Vishal");
    p->u32MajorVersion = 1;
    p->u32MinorVersion = 0;
    p->u32MinInputConnections = 1;
    p->u32MaxInputConnections = 1;
    p->u32MinOutputConnections = 1;
    p->u32MaxOutputConnections = 1;
    p->u32MaxInstances = UINT32_MAX; // one instance per endpoint
    p->u32NumAPOInterfaces = 1;
    p->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);

    *ppRegProps = p;
    return S_OK;
}

STDMETHODIMP CEqApo::GetInputChannelCount(UINT32* pu32ChannelCount) {
    if (pu32ChannelCount == nullptr) {
        return E_POINTER;
    }
    *pu32ChannelCount = m_channels;
    return S_OK;
}

STDMETHODIMP CEqApo::GetLatency(HNSTIME* pTime) {
    if (pTime == nullptr) {
        return E_POINTER;
    }
    *pTime = 0; // minimum-phase IIR: no added block latency
    return S_OK;
}

STDMETHODIMP CEqApo::Reset() {
    // The engine calls this between streams; drop filter state so the next
    // stream starts clean. Safe on the RT thread: no allocation, no syscalls.
    m_dsp.Reset();
    return S_OK;
}

//------------------------------------------------------------------------------
// IAudioProcessingObjectConfiguration
//------------------------------------------------------------------------------

STDMETHODIMP CEqApo::LockForProcess(UINT32 u32NumInputConnections,
                                   APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                   UINT32 u32NumOutputConnections,
                                   APO_CONNECTION_DESCRIPTOR** ppOutputConnections) noexcept
try {
    if (m_locked) {
        return APOERR_APO_LOCKED;
    }
    if (u32NumInputConnections != 1 || u32NumOutputConnections != 1 ||
        ppInputConnections == nullptr || ppOutputConnections == nullptr ||
        ppInputConnections[0] == nullptr || ppOutputConnections[0] == nullptr ||
        ppInputConnections[0]->pFormat == nullptr ||
        ppOutputConnections[0]->pFormat == nullptr) {
        return E_INVALIDARG;
    }

    const WAVEFORMATEX* wfx = ppInputConnections[0]->pFormat->GetAudioFormat();
    const WAVEFORMATEX* wfxOut = ppOutputConnections[0]->pFormat->GetAudioFormat();
    if (wfx == nullptr || wfxOut == nullptr) {
        return E_INVALIDARG;
    }
    if (!IsFloat32Format(wfx) || !IsFloat32Format(wfxOut)) {
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    // In-place SFX contract: both sides must carry the identical format. A
    // mismatch (e.g. the engine rewiring around a spatial-audio switch)
    // means the graph is miswired for an in-place APO -- fail the lock
    // loudly instead of misprocessing buffers.
    if (wfx->nChannels != wfxOut->nChannels ||
        wfx->nSamplesPerSec != wfxOut->nSamplesPerSec) {
        MiniEQ_Trace(L"MiniEQ_APO: LockForProcess -> E_INVALIDARG (in/out mismatch: %u ch @ %lu Hz vs %u ch @ %lu Hz)",
                     wfx->nChannels, wfx->nSamplesPerSec,
                     wfxOut->nChannels, wfxOut->nSamplesPerSec);
        return E_INVALIDARG;
    }
    const float rate = static_cast<float>(wfx->nSamplesPerSec);
    const uint32_t channels = wfx->nChannels;
    if (channels == 0 || channels > MINIEQ_MAX_CHANNELS) {
        return APOERR_FORMAT_NOT_SUPPORTED;
    }

    // Denormal safety, part 1: biquad feedback loops are classic denormal
    // producers, and one denormal operand can stall the FPU ~100x -- fatal
    // inside a real-time budget. Arm FTZ|DAZ here; MXCSR is per-thread and
    // LockForProcess runs on an engine setup thread, so the RT thread gets
    // its own one-time arming in APOProcess (part 2).
    _mm_setcsr(_mm_getcsr() | 0x8000u /* FTZ */ | 0x0040u /* DAZ */);

    // (Re)initialize the DSP for the negotiated layout: per-channel biquad
    // state is resized to nChannels and coefficients recomputed for the new
    // rate, so a spatial-audio switch (stereo -> 5.1/7.1/object layouts)
    // can never index past the filter state.
    m_dsp.Configure(rate, channels);
    m_channels = channels;
    m_sampleRate = wfx->nSamplesPerSec;
    MiniEQ_Trace(L"MiniEQ_APO: LockForProcess ch=%lu rate=%.0f",
                 channels, (double)rate);

    // Background worker: retries the settings mapping until the UI has
    // created it, and performs the crossfeed heap work off the RT thread.
    // (LockForProcess runs on an engine setup thread -- thread creation and
    // file mappings are fine here, just never in APOProcess.)
    m_hWorkerStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_hWorkerStop != nullptr) {
        m_hWorkerThread = CreateThread(nullptr, 0, WorkerThreadProc, this, 0, nullptr);
        if (m_hWorkerThread == nullptr) {
            CloseHandle(m_hWorkerStop);
            m_hWorkerStop = nullptr;
        }
    }

    // Create the live settings channel. The APO must be the creator: it runs
    // in the audio engine (session 0, with SeCreateGlobalPrivilege) while
    // the UI runs in the user's session -- Local\ objects can never cross
    // that boundary, and the UI cannot create Global\ objects.
    CreateSettingsMapping();
    MiniEQ_Trace(L"MiniEQ_APO: LockForProcess mapping=\"%s\" settings=%s",
                 m_mappingName[0] ? m_mappingName : L"<none>",
                 m_pSettings.load(std::memory_order_acquire) != nullptr
                     ? L"CREATED" : L"not yet");

    // Create the heartbeat channel too (single creation point, no race);
    // publish once here so the UI sees the header immediately.
    CreateStatusMapping();
    PublishStatus();
    MiniEQ_Trace(L"MiniEQ_APO: LockForProcess status=\"%s\" heartbeat=%s",
                 m_statusName[0] ? m_statusName : L"<none>",
                 m_pStatus != nullptr ? L"CREATED" : L"not yet");

    // Create the global on/off channel (one for all endpoints). Multiple APO
    // instances (different endpoints, same audiodg) race here -- the
    // open-first order inside makes the loser adopt, never double-create.
    CreateGlobalMapping();
    MiniEQ_Trace(L"MiniEQ_APO: LockForProcess global=\"%s\" enabled-channel=%s",
                 m_globalName[0] ? m_globalName : L"<none>",
                 m_pGlobal.load(std::memory_order_acquire) != nullptr
                     ? L"CREATED" : L"not yet");

    m_locked = true;
    return S_OK;
} catch (...) {
    // Never let a C++ exception cross the COM boundary into audiodg.exe.
    MiniEQ_Trace(L"MiniEQ_APO: LockForProcess swallowed C++ exception -> E_FAIL");
    return E_FAIL;
}

STDMETHODIMP CEqApo::UnlockForProcess() {
    if (!m_locked) {
        return S_OK;
    }
    StopWorker();
    CloseStatusMapping();
    CloseGlobalMapping();
    const EqSettings* settings = m_pSettings.exchange(nullptr);
    if (settings != nullptr) {
        UnmapViewOfFile(settings);
    }
    if (m_hMap != nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
    }
    m_dsp.ShutdownVirtualization();
    m_locked = false;
    return S_OK;
}

//------------------------------------------------------------------------------
// Background worker (non-RT thread)
//------------------------------------------------------------------------------

DWORD WINAPI CEqApo::WorkerThreadProc(LPVOID pParam) {
    CEqApo* self = static_cast<CEqApo*>(pParam);
    while (WaitForSingleObject(self->m_hWorkerStop, 100) == WAIT_TIMEOUT) {
        self->WorkerStep();
    }
    return 0;
}

void CEqApo::WorkerStep() {
    if (m_pSettings.load(std::memory_order_acquire) == nullptr) {
        CreateSettingsMapping();
    }
    if (m_pStatus == nullptr) {
        CreateStatusMapping();
    }
    if (m_pGlobal.load(std::memory_order_acquire) == nullptr) {
        CreateGlobalMapping();
    }
    PublishStatus();
    m_dsp.ServiceVirtualizationWorker();
}

// Opens (or creates, if no APO instance has made it yet) a named file
// mapping in the Global\ namespace with a DACL that lets the UI open it
// from the user's session. The APO is the only side that can create these:
// it runs inside the audio engine (session 0) as a service identity, which
// holds SeCreateGlobalPrivilege; the UI runs in the user's session, where
// creating Global\ objects is denied and Local\ objects are invisible here.
//
// The open-first order matters. CreateFileMappingW issued against an
// EXISTING named object can fail with ERROR_ACCESS_DENIED -- observed
// 2026-09-30: every second audiodg instance for an endpoint got gle=5, so
// its APO ran with settings=NULL (audio processed with a flat, silent EQ
// and no heartbeat), and the worker retry never recovered. OpenFileMappingW
// with read+write -- the exact call the UI makes -- succeeds against the
// same DACL, so open first and create only when the object is truly absent.
static HANDLE OpenOrCreateGlobalChannel(const wchar_t* name, DWORD byteSize,
                                        bool* fresh) {
    HANDLE h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (h != nullptr) {
        if (fresh != nullptr) {
            *fresh = false;
        }
        return h;
    }
    if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        return nullptr; // transient; the worker thread retries
    }
    PSECURITY_DESCRIPTOR pSD = nullptr;
    // D: Everyone read+write. (EQ gains and a heartbeat are not sensitive;
    // the UI must be able to open this from another session.)
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GRGW;;;WD)", SDDL_REVISION_1, &pSD, nullptr)) {
        return nullptr;
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), pSD, FALSE };
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                           0, byteSize, name);
    const DWORD gle = GetLastError();
    LocalFree(pSD);
    if (h == nullptr) {
        SetLastError(gle);
        return nullptr;
    }
    if (fresh != nullptr) {
        // A racing APO instance may have created it between our open and
        // our create; ERROR_ALREADY_EXISTS means adopt, don't re-init.
        *fresh = (gle != ERROR_ALREADY_EXISTS);
    }
    return h;
}

void CEqApo::CreateSettingsMapping() {
    if (m_mappingName[0] == L'\0') {
        // Endpoint not identified at Initialize -- nothing to name the
        // channel with, and retrying would be pointless.
        return;
    }
    bool fresh = false;
    HANDLE h = OpenOrCreateGlobalChannel(m_mappingName,
                                         (DWORD)sizeof(EqSettings), &fresh);
    if (h == nullptr) {
        static LONG s_failLogged = 0;
        if (InterlockedCompareExchange(&s_failLogged, 1, 0) == 0) {
            MiniEQ_Trace(L"MiniEQ_APO: settings channel open failed gle=%lu name=\"%s\"",
                         GetLastError(), m_mappingName);
        }
        return;
    }
    void* v = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                            sizeof(EqSettings));
    if (v == nullptr) {
        CloseHandle(h);
        return;
    }
    EqSettings* s = static_cast<EqSettings*>(v);
    if (fresh) {
        // We created it: publish flat defaults so a UI opening later sees a
        // valid channel immediately. (LockForProcess runs before the first
        // APOProcess call, so the RT thread cannot race this write.)
        EqSettings flat;
        MiniEQ_SettingsInitFlat(&flat);
        memcpy(s, &flat, sizeof(flat));
    }
    const EqSettings* expected = nullptr;
    if (m_pSettings.compare_exchange_strong(expected,
                                            static_cast<const EqSettings*>(v))) {
        m_hMap = h;
        m_lastSequence = 0; // force a settings pickup on the next RT block
        MiniEQ_Trace(L"MiniEQ_APO: settings channel %s \"%s\"",
                     fresh ? L"CREATED" : L"adopted", m_mappingName);
    } else {
        UnmapViewOfFile(v);
        CloseHandle(h);
    }
}

void CEqApo::CreateStatusMapping() {
    if (m_statusName[0] == L'\0') {
        return;
    }
    bool fresh = false;
    HANDLE h = OpenOrCreateGlobalChannel(m_statusName,
                                         (DWORD)sizeof(MiniEQApoStatus), &fresh);
    if (h == nullptr) {
        return; // worker retries; see CreateSettingsMapping for the why
    }
    void* v = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                            sizeof(MiniEQApoStatus));
    if (v == nullptr) {
        CloseHandle(h);
        return;
    }
    MiniEQApoStatus* st = static_cast<MiniEQApoStatus*>(v);
    if (fresh || st->version < MINIEQ_STATUS_VERSION) {
        // We created it, or a newer build is adopting a channel left behind
        // by an older one: (re)initialize the header. On a same-version
        // adopt, another live instance owns the counters -- the worker
        // republishes the live fields below on every step, so leave its
        // header alone. Never memset on adopt: that would wipe live counters.
        st->structSize = sizeof(MiniEQApoStatus);
        st->version = MINIEQ_STATUS_VERSION;
        StringCchCopyA(st->buildId, ARRAYSIZE(st->buildId), MiniEQ_ApoBuildId());
    }
    LARGE_INTEGER freq;
    if (QueryPerformanceFrequency(&freq)) {
        m_qpcFreq = freq.QuadPart;
    }
    if (fresh) {
        st->qpcFrequency = m_qpcFreq;
    }
    m_pStatus = st;
    m_hStatusMap = h;
    MiniEQ_Trace(L"MiniEQ_APO: status channel %s \"%s\"",
                 fresh ? L"CREATED" : L"adopted", m_statusName);
}

void CEqApo::PublishStatus() {
    if (m_pStatus == nullptr) {
        return;
    }
    m_pStatus->processCalls = (int64_t)m_rtCalls.load(std::memory_order_relaxed);
    m_pStatus->lastProcessQpc = m_rtLastQpc.load(std::memory_order_relaxed);
    m_pStatus->locked = m_locked ? 1 : 0;
    m_pStatus->channels = (int32_t)m_channels;
    m_pStatus->sampleRate = (int32_t)m_sampleRate;
    m_pStatus->initOk = m_initialized ? 1 : 0;
    // Idempotent per instance: keeps the build stamp correct even when this
    // instance adopted a channel created by an older build.
    StringCchCopyA(m_pStatus->buildId, ARRAYSIZE(m_pStatus->buildId), MiniEQ_ApoBuildId());
}

void CEqApo::CloseStatusMapping() {
    if (m_pStatus != nullptr) {
        UnmapViewOfFile(m_pStatus);
        m_pStatus = nullptr;
    }
    if (m_hStatusMap != nullptr) {
        CloseHandle(m_hStatusMap);
        m_hStatusMap = nullptr;
    }
}

// Global on/off channel (UI -> APO). One flat name shared by every endpoint,
// so the first APO instance across all of audiodg creates it and the rest
// adopt it -- the open-first order in OpenOrCreateGlobalChannel makes the
// loser adopt, never double-create. Fail-open: until this exists the RT
// thread treats MiniEQ as enabled.
void CEqApo::CreateGlobalMapping() {
    if (m_globalName[0] == L'\0') {
        return;
    }
    bool fresh = false;
    HANDLE h = OpenOrCreateGlobalChannel(m_globalName,
                                         (DWORD)sizeof(MiniEQGlobalState), &fresh);
    if (h == nullptr) {
        return; // worker retries; see CreateSettingsMapping for the why
    }
    void* v = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                            sizeof(MiniEQGlobalState));
    if (v == nullptr) {
        CloseHandle(h);
        return;
    }
    MiniEQGlobalState* s = static_cast<MiniEQGlobalState*>(v);
    if (fresh) {
        // We created it: default to enabled (fail-open). The UI re-asserts
        // its persisted choice on its status timer if it differs.
        s->structSize = sizeof(MiniEQGlobalState);
        s->version = MINIEQ_GLOBAL_VERSION;
        s->enabled = 1;
        MemoryBarrier();
        s->sequence = 1;
    }
    const MiniEQGlobalState* expected = nullptr;
    if (m_pGlobal.compare_exchange_strong(expected,
                                          static_cast<const MiniEQGlobalState*>(v))) {
        m_hGlobalMap = h;
        m_lastGlobalSeq = 0; // force a pickup on the next RT block
        MiniEQ_Trace(L"MiniEQ_APO: global channel %s \"%s\"",
                     fresh ? L"CREATED" : L"adopted", m_globalName);
    } else {
        UnmapViewOfFile(v);
        CloseHandle(h);
    }
}

void CEqApo::CloseGlobalMapping() {
    const MiniEQGlobalState* gs = m_pGlobal.exchange(nullptr);
    if (gs != nullptr) {
        UnmapViewOfFile(gs);
    }
    if (m_hGlobalMap != nullptr) {
        CloseHandle(m_hGlobalMap);
        m_hGlobalMap = nullptr;
    }
}

void CEqApo::StopWorker() {
    if (m_hWorkerStop != nullptr) {
        SetEvent(m_hWorkerStop);
    }
    if (m_hWorkerThread != nullptr) {
        WaitForSingleObject(m_hWorkerThread, 2000);
        CloseHandle(m_hWorkerThread);
        m_hWorkerThread = nullptr;
    }
    if (m_hWorkerStop != nullptr) {
        CloseHandle(m_hWorkerStop);
        m_hWorkerStop = nullptr;
    }
}

//------------------------------------------------------------------------------
// IAudioProcessingObjectRT -- real-time audio thread. No blocking, no syscalls,
// no COM, no allocation here.
//------------------------------------------------------------------------------

STDMETHODIMP_(UINT32) CEqApo::CalcInputFrames(UINT32 u32OutputFrameCount) {
    return u32OutputFrameCount; // 1:1 in-place processing
}

STDMETHODIMP_(UINT32) CEqApo::CalcOutputFrames(UINT32 u32InputFrameCount) {
    return u32InputFrameCount; // 1:1 in-place processing
}

STDMETHODIMP_(void) CEqApo::APOProcess(UINT32 /*u32NumInputConnections*/,
                                      APO_CONNECTION_PROPERTY** ppInputConnections,
                                      UINT32 u32NumOutputConnections,
                                      APO_CONNECTION_PROPERTY** ppOutputConnections) noexcept
try {
    if (ppInputConnections == nullptr || ppInputConnections[0] == nullptr) {
        return;
    }
    APO_CONNECTION_PROPERTY* in = ppInputConnections[0];

    switch (in->u32BufferFlags) {
    case BUFFER_VALID:
    case BUFFER_SILENT:
        break;
    default:
        return; // BUFFER_INVALID: never happens; do nothing
    }

    // Denormal safety, part 2 (see LockForProcess): MXCSR is per-thread, and
    // APOProcess runs on the engine's real-time thread -- not the thread
    // that ran LockForProcess. Arm FTZ|DAZ once per processing thread; the
    // mode persists, so this is a single branch after the first block.
    static thread_local bool s_ftzArmed = false;
    if (!s_ftzArmed) {
        _mm_setcsr(_mm_getcsr() | 0x8000u /* FTZ */ | 0x0040u /* DAZ */);
        s_ftzArmed = true;
    }

    FLOAT32* frames = reinterpret_cast<FLOAT32*>(in->pBuffer);
    const UINT32 validFrames = in->u32ValidFrameCount;

    // Heartbeat for the UI's "is audio really passing through" status. The
    // counter bumps every block; QPC is snapshotted every 64th block (cheap
    // enough, and plenty for a 2-second freshness window). This thread is
    // the only writer: plain relaxed atomics, no fences.
    m_rtCalls.fetch_add(1, std::memory_order_relaxed);
    if ((m_rtQpcTick++ & 63) == 0) {
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        m_rtLastQpc.store(qpc.QuadPart, std::memory_order_relaxed);
    }

    {
        // Diagnostic only: one file write on the RT thread, first call only.
        static LONG s_firstLogged = 0;
        if (InterlockedCompareExchange(&s_firstLogged, 1, 0) == 0) {
            const EqSettings* st = m_pSettings.load(std::memory_order_acquire);
            MiniEQ_Trace(L"MiniEQ_APO: APOProcess FIRST call frames=%lu ch=%lu settings=%s bypass=%d gain0=%.1f",
                         validFrames, m_channels,
                         st != nullptr ? L"open" : L"NULL",
                         m_localCopy.bypass, m_localCopy.bandGainDb[0]);
        }
    }

    if (in->u32BufferFlags == BUFFER_SILENT) {
        // Keep filter state honest across silence.
        m_dsp.Reset();
        if (frames != nullptr && validFrames > 0 && m_channels > 0) {
            memset(frames, 0, (size_t)validFrames * m_channels * sizeof(FLOAT32));
        }
    } else {
        // Pick up new UI settings, lock-free: single 64-bit sequence check.
        const EqSettings* settings = m_pSettings.load(std::memory_order_acquire);
        if (settings != nullptr) {
            const int64_t seq = settings->sequence; // aligned: atomic on x64
            if (seq != m_lastSequence) {
                MemoryBarrier();
                memcpy(&m_localCopy, (const void*)settings, sizeof(EqSettings));
                MemoryBarrier();
                // Re-read to guard against a torn write racing us.
                if (m_localCopy.sequence == seq) {
                    m_dsp.UpdateGains(m_localCopy.bandGainDb, m_localCopy.numBands,
                                      m_localCopy.masterGainDb);
                    // RT-safe: only arms the request; the worker thread does
                    // the actual crossfeed allocation/free.
                    m_dsp.SetVirtualization(m_localCopy.virtualization != 0);
                    m_lastSequence = seq;
                }
            }
        }
        // Global on/off (UI -> APO): one flag shared by every endpoint.
        // Fail-open -- an absent channel means enabled. The writer bumps
        // `sequence` AFTER `enabled`; adopt the pair only when the two
        // sequence reads match, so a racing UI write can't tear the read.
        // Combined with the per-endpoint bypass below; both ride the same
        // click-free crossfade in EqDsp::Process, and the heartbeat counter
        // above keeps advancing while bypassed so the UI link looks alive.
        const MiniEQGlobalState* gs = m_pGlobal.load(std::memory_order_acquire);
        if (gs != nullptr) {
            const int64_t gseq = gs->sequence; // aligned: atomic on x64
            if (gseq != m_lastGlobalSeq) {
                const int32_t gen = gs->enabled;
                MemoryBarrier();
                if (gs->sequence == gseq) { // untorn read
                    m_globalEnabled = (gen != 0);
                    m_lastGlobalSeq = gseq;
                    MiniEQ_Trace(L"MiniEQ_APO: global enabled=%d (seq=%lld)",
                                 m_globalEnabled ? 1 : 0, (long long)gseq);
                }
            }
        }
        const bool bypass = (m_localCopy.bypass != 0) || !m_globalEnabled;
        m_dsp.Process(frames, validFrames, bypass);
    }

    // In-place SFX: output aliases input; still, honor the contract when the
    // engine gave us a distinct output buffer.
    if (u32NumOutputConnections > 0 && ppOutputConnections != nullptr &&
        ppOutputConnections[0] != nullptr) {
        APO_CONNECTION_PROPERTY* out = ppOutputConnections[0];
        // APO_CONNECTION_PROPERTY::pBuffer is a UINT_PTR, not a pointer.
        if (out->pBuffer != in->pBuffer && out->pBuffer != 0 && frames != nullptr) {
            memcpy(reinterpret_cast<void*>(out->pBuffer), frames,
                   (size_t)validFrames * m_channels * sizeof(FLOAT32));
        }
        out->u32BufferFlags = in->u32BufferFlags;
        out->u32ValidFrameCount = validFrames;
    }
} catch (...) {
    // Fail open: leave the buffer untouched so unprocessed audio keeps
    // flowing instead of taking the engine down with us.
    MiniEQ_Trace(L"MiniEQ_APO: APOProcess swallowed C++ exception (fail-open bypass)");
}
