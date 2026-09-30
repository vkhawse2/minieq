// eq_apo.h -- CEqApo: our system-effect APO (SFX).
//
// Implements IAudioProcessingObject + IAudioProcessingObjectRT +
// IAudioProcessingObjectConfiguration with plain C++ COM (no ATL), so the DLL
// builds with just the Windows SDK.
//
// Audio path: the engine calls APOProcess() on its real-time thread with
// interleaved IEEE-float32 frames. We process in place with the EqDsp biquad
// bank. Live band gains arrive through the shared-memory settings channel
// (shared/settings_channel.h); APOProcess only does an atomic sequence check
// plus a memcpy when settings change -- no locks, no syscalls, no COM on the
// RT thread.
//
// NOTE on IAudioSystemEffects: Microsoft's docs list it among the interfaces
// a *modern* APO exposes, but the legacy single-CLSID SFX-slot registration we
// use (the exact form Equalizer APO ships with, working on Win10/11) does not
// require it -- the engine CoCreates our CLSID from the endpoint's FxProperties
// and talks to IAudioProcessingObject. If the engine ever refuses to load us,
// implementing IAudioSystemEffects is the first thing to try.

#pragma once

#include <windows.h>
#include <audioenginebaseapo.h>

#include <string>

#include "dsp.h"
#include "../shared/settings_channel.h"

class CEqApo : public IAudioProcessingObject,
               public IAudioProcessingObjectRT,
               public IAudioProcessingObjectConfiguration {
public:
    CEqApo();
    virtual ~CEqApo();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IAudioProcessingObject
    STDMETHODIMP Initialize(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHODIMP IsInputFormatSupported(IAudioMediaType* pOppositeFormat,
                                       IAudioMediaType* pRequestedInputFormat,
                                       IAudioMediaType** ppSupportedInputFormat) override;
    STDMETHODIMP IsOutputFormatSupported(IAudioMediaType* pOppositeFormat,
                                        IAudioMediaType* pRequestedOutputFormat,
                                        IAudioMediaType** ppSupportedOutputFormat) override;
    STDMETHODIMP GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps) override;
    STDMETHODIMP GetInputChannelCount(UINT32* pu32ChannelCount) override;
    STDMETHODIMP GetLatency(HNSTIME* pTime) override;

    // IAudioProcessingObjectRT
    STDMETHODIMP_(void) APOProcess(UINT32 u32NumInputConnections,
                                  APO_CONNECTION_PROPERTY** ppInputConnections,
                                  UINT32 u32NumOutputConnections,
                                  APO_CONNECTION_PROPERTY** ppOutputConnections) override;

    // IAudioProcessingObjectConfiguration
    STDMETHODIMP LockForProcess(UINT32 u32NumInputConnections,
                               APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                               UINT32 u32NumOutputConnections,
                               APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHODIMP UnlockForProcess() override;

private:
    // Returns true for the one format we process: interleaved IEEE float32.
    static bool IsFloat32Format(const WAVEFORMATEX* wfx);

    volatile LONG  m_refCount = 1;
    bool           m_initialized = false;
    bool           m_locked = false;

    std::wstring   m_endpointId;              // our render endpoint
    wchar_t        m_mappingName[128] = {};   // MMF name derived from endpoint id

    EqDsp          m_dsp;
    uint32_t       m_channels = 0;

    HANDLE         m_hMap = nullptr;          // shared-memory handle (non-RT only)
    const EqSettings* m_pSettings = nullptr;  // mapped view (read-only)
    int64_t        m_lastSequence = 0;        // last applied settings version
    EqSettings     m_localCopy;               // RT-side working copy
};
