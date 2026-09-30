// eq_apo.cpp -- CEqApo implementation.
//
// Modeled on Microsoft's SwapAPO sample (audio/sysvad/APO/SwapAPO), replacing
// the channel-swap DSP with our biquad EQ bank and adding the shared-memory
// settings channel for live UI control.

#include "eq_apo.h"
#include "guids.h"

#include <mmdeviceapi.h>
#include <ksmedia.h>   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
#include <strsafe.h>

#include <cstring>

//------------------------------------------------------------------------------
// IUnknown
//------------------------------------------------------------------------------

STDMETHODIMP CEqApo::QueryInterface(REFIID riid, void** ppv) {
    if (ppv == nullptr) {
        return E_POINTER;
    }
    if (riid == IID_IUnknown) {
        *ppv = static_cast<IAudioProcessingObject*>(this);
    } else if (riid == IID_IAudioProcessingObject) {
        *ppv = static_cast<IAudioProcessingObject*>(this);
    } else if (riid == IID_IAudioProcessingObjectRT) {
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (riid == IID_IAudioProcessingObjectConfiguration) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CEqApo::AddRef() {
    return (ULONG)InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) CEqApo::Release() {
    ULONG c = (ULONG)InterlockedDecrement(&m_refCount);
    if (c == 0) {
        delete this;
    }
    return c;
}

CEqApo::CEqApo() {
    MiniEQ_SettingsInitFlat(&m_localCopy);
    m_lastSequence = m_localCopy.sequence;
}

CEqApo::~CEqApo() {
    if (m_pSettings != nullptr) {
        UnmapViewOfFile(m_pSettings);
    }
    if (m_hMap != nullptr) {
        CloseHandle(m_hMap);
    }
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

STDMETHODIMP CEqApo::Initialize(UINT32 cbDataSize, BYTE* pbyData) {
    if (m_initialized) {
        return AEERR_ALREADY_INITIALIZED;
    }
    if (pbyData == nullptr || cbDataSize == 0) {
        return E_INVALIDARG;
    }

    // APOInitSystemEffects3 is a superset of APOInitSystemEffects2, so the v2
    // fields can be read from either. This keeps us working on Win10 and Win11.
    if (cbDataSize < sizeof(APOInitSystemEffects2)) {
        return E_INVALIDARG;
    }
    APOInitSystemEffects2* init = reinterpret_cast<APOInitSystemEffects2*>(pbyData);
    if (init->pDeviceCollection == nullptr) {
        return E_INVALIDARG;
    }

    // Our endpoint is the last device in the collection.
    UINT32 count = 0;
    HRESULT hr = init->pDeviceCollection->GetCount(&count);
    if (FAILED(hr) || count == 0) {
        return E_UNEXPECTED;
    }
    IMMDevice* endpoint = nullptr;
    hr = init->pDeviceCollection->Item(count - 1, &endpoint);
    if (FAILED(hr)) {
        return hr;
    }
    LPWSTR id = nullptr;
    hr = endpoint->GetId(&id);
    if (SUCCEEDED(hr) && id != nullptr) {
        m_endpointId = id;
        CoTaskMemFree(id);
    }
    endpoint->Release();

    if (!m_endpointId.empty()) {
        MiniEQ_MappingNameForEndpoint(m_endpointId.c_str(), m_mappingName,
                                     ARRAYSIZE(m_mappingName));
    }

    m_initialized = true;
    return S_OK;
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
    WAVEFORMATEX* wfx = nullptr;
    HRESULT hr = pRequestedInputFormat->GetAudioFormat(&wfx);
    if (FAILED(hr)) {
        return hr;
    }
    bool ok = IsFloat32Format(wfx);
    CoTaskMemFree(wfx);
    if (!ok) {
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
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
    p->iidAPOInterfaceList[0] = IID_IAudioProcessingObject;

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

//------------------------------------------------------------------------------
// IAudioProcessingObjectConfiguration
//------------------------------------------------------------------------------

STDMETHODIMP CEqApo::LockForProcess(UINT32 u32NumInputConnections,
                                   APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                   UINT32 u32NumOutputConnections,
                                   APO_CONNECTION_DESCRIPTOR** ppOutputConnections) {
    if (m_locked) {
        return APOERR_ALREADY_LOCKED;
    }
    if (u32NumInputConnections != 1 || u32NumOutputConnections != 1 ||
        ppInputConnections == nullptr || ppOutputConnections == nullptr ||
        ppInputConnections[0] == nullptr || ppInputConnections[0]->pFormat == nullptr) {
        return E_INVALIDARG;
    }

    WAVEFORMATEX* wfx = nullptr;
    HRESULT hr = ppInputConnections[0]->pFormat->GetAudioFormat(&wfx);
    if (FAILED(hr)) {
        return hr;
    }
    if (!IsFloat32Format(wfx)) {
        CoTaskMemFree(wfx);
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    const float rate = static_cast<float>(wfx->nSamplesPerSec);
    const uint32_t channels = wfx->nChannels;
    CoTaskMemFree(wfx);

    m_dsp.Configure(rate, channels);
    m_channels = channels;

    // Open the live settings channel (non-RT thread: file mapping is fine).
    // If the UI isn't running there is no mapping yet -- stay flat.
    if (m_mappingName[0] != L'\0') {
        m_hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, m_mappingName);
        if (m_hMap != nullptr) {
            m_pSettings = static_cast<const EqSettings*>(
                MapViewOfFile(m_hMap, FILE_MAP_READ, 0, 0, sizeof(EqSettings)));
            if (m_pSettings == nullptr) {
                CloseHandle(m_hMap);
                m_hMap = nullptr;
            }
        }
    }

    m_locked = true;
    return S_OK;
}

STDMETHODIMP CEqApo::UnlockForProcess() {
    if (!m_locked) {
        return S_OK;
    }
    if (m_pSettings != nullptr) {
        UnmapViewOfFile(m_pSettings);
        m_pSettings = nullptr;
    }
    if (m_hMap != nullptr) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
    }
    m_locked = false;
    return S_OK;
}

//------------------------------------------------------------------------------
// IAudioProcessingObjectRT -- real-time audio thread. No blocking, no syscalls,
// no COM, no allocation here.
//------------------------------------------------------------------------------

STDMETHODIMP_(void) CEqApo::APOProcess(UINT32 /*u32NumInputConnections*/,
                                      APO_CONNECTION_PROPERTY** ppInputConnections,
                                      UINT32 u32NumOutputConnections,
                                      APO_CONNECTION_PROPERTY** ppOutputConnections) {
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

    FLOAT32* frames = reinterpret_cast<FLOAT32*>(in->pBuffer);
    const UINT32 validFrames = in->u32ValidFrameCount;

    if (in->u32BufferFlags == BUFFER_SILENT) {
        // Keep filter state honest across silence.
        m_dsp.Reset();
        if (frames != nullptr && validFrames > 0 && m_channels > 0) {
            memset(frames, 0, (size_t)validFrames * m_channels * sizeof(FLOAT32));
        }
    } else {
        // Pick up new UI settings, lock-free: single 64-bit sequence check.
        if (m_pSettings != nullptr) {
            const int64_t seq = m_pSettings->sequence; // aligned: atomic on x64
            if (seq != m_lastSequence) {
                MemoryBarrier();
                memcpy(&m_localCopy, (const void*)m_pSettings, sizeof(EqSettings));
                MemoryBarrier();
                // Re-read to guard against a torn write racing us.
                if (m_localCopy.sequence == seq) {
                    m_dsp.UpdateGains(m_localCopy.bandGainDb, m_localCopy.numBands,
                                      m_localCopy.masterGainDb);
                    // Idempotent: allocates the crossfeed state on first
                    // enable, frees it on disable -- zero cost when off.
                    m_dsp.SetVirtualization(m_localCopy.virtualization != 0);
                    m_lastSequence = seq;
                }
            }
        }
        m_dsp.Process(frames, validFrames, m_localCopy.bypass != 0);
    }

    // In-place SFX: output aliases input; still, honor the contract when the
    // engine gave us a distinct output buffer.
    if (u32NumOutputConnections > 0 && ppOutputConnections != nullptr &&
        ppOutputConnections[0] != nullptr) {
        APO_CONNECTION_PROPERTY* out = ppOutputConnections[0];
        if (out->pBuffer != in->pBuffer && out->pBuffer != nullptr && frames != nullptr) {
            memcpy(out->pBuffer, frames,
                   (size_t)validFrames * m_channels * sizeof(FLOAT32));
        }
        out->u32BufferFlags = in->u32BufferFlags;
        out->u32ValidFrameCount = validFrames;
    }
}
