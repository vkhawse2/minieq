// eq_apo.h -- CEqApo: our system-effect APO (SFX).
//
// Implements IAudioProcessingObject + IAudioProcessingObjectRT +
// IAudioProcessingObjectConfiguration + IAudioSystemEffects3 with plain C++
// COM (no ATL), so the DLL builds with just the Windows SDK.
//
// WHY AGGREGATION + IAudioSystemEffects3 (read before "simplifying" this):
// The Windows audio engine creates system-effect APOs by COM *aggregation*:
// IClassFactory::CreateInstance is called with a non-null controlling unknown
// and IID_IUnknown. A factory that answers CLASS_E_NOAGGREGATION is skipped in
// total silence -- no error, no event log, nothing in an ETW trace. It looks
// exactly like "Windows refuses third-party APOs" and it is not.
// The engine then queries IAudioSystemEffects3 (Windows 11) and, with
// ThreadingModel "Both", expects no apartment affinity -- so the free-threaded
// marshaler is aggregated and IAgileObject is answered. Refusing either sends
// the engine into a fruitless round of marshalling probes and the APO never
// starts. (Verified by instrumenting a probe APO against the real engine on
// Windows 11 10.0.22631.)
//
// Audio path: the engine calls APOProcess() on its real-time thread with
// interleaved IEEE-float32 frames. We process in place with the EqDsp biquad
// bank. Live band gains arrive through the shared-memory settings channel
// (shared/settings_channel.h); APOProcess only does an atomic sequence check
// plus a memcpy when settings change -- no locks, no syscalls, no COM on the
// RT thread.
//
// A background worker thread (created in LockForProcess) retries opening the
// settings mapping until the UI has created it, and services the crossfeed
// (XFeed) allocation/free requests -- heap work must never happen on the RT
// thread.

#pragma once

#include <windows.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h> // IAudioSystemEffects3 (Win11 handshake)

#include <atomic>
#include <string>

#include "dsp.h"
#include "../shared/settings_channel.h"

class CEqApo : public IAudioProcessingObject,
               public IAudioProcessingObjectRT,
               public IAudioProcessingObjectConfiguration,
               public IAudioSystemEffects3 {
public:
    explicit CEqApo(IUnknown* pUnkOuter);
    virtual ~CEqApo();

    // The non-delegating IUnknown, handed to whoever aggregates us.
    IUnknown* NonDelegatingUnknown() { return &m_inner; }
    HRESULT NonDelegatingQueryInterface(REFIID riid, void** ppv);
    ULONG NonDelegatingRelease() { return m_inner.Release(); }

    // IUnknown -- delegating: forward to the controlling unknown (or to the
    // inner one when not aggregated) so refcounts and QI identity hold.
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IAudioSystemEffects: our UI drives everything, so there is nothing to
    // enumerate. An empty list is a valid answer; refusing the call is not.
    STDMETHODIMP GetEffectsList(GUID** ppEffectsIds, UINT* pcEffects,
                               HANDLE hEvent) override;
    // IAudioSystemEffects2
    STDMETHODIMP GetControllableSystemEffectsList(AUDIO_SYSTEMEFFECT** ppEffects,
                                                 UINT* pcEffects,
                                                 HANDLE hEvent) override;
    // IAudioSystemEffects3
    STDMETHODIMP SetAudioSystemEffectState(GUID effectId,
                                          AUDIO_SYSTEMEFFECT_STATE state) override;

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
    STDMETHODIMP Reset() override;

    // IAudioProcessingObjectRT
    STDMETHODIMP_(void) APOProcess(UINT32 u32NumInputConnections,
                                  APO_CONNECTION_PROPERTY** ppInputConnections,
                                  UINT32 u32NumOutputConnections,
                                  APO_CONNECTION_PROPERTY** ppOutputConnections) override;
    STDMETHODIMP_(UINT32) CalcInputFrames(UINT32 u32OutputFrameCount) override;
    STDMETHODIMP_(UINT32) CalcOutputFrames(UINT32 u32InputFrameCount) override;

    // IAudioProcessingObjectConfiguration
    STDMETHODIMP LockForProcess(UINT32 u32NumInputConnections,
                               APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                               UINT32 u32NumOutputConnections,
                               APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHODIMP UnlockForProcess() override;

private:
    // Non-delegating IUnknown: the only thing that actually owns the object.
    // Lives inside the owner; when its count hits zero the owner is deleted.
    class CInnerUnknown : public IUnknown {
    public:
        explicit CInnerUnknown(CEqApo* pOwner) : m_pOwner(pOwner) {}
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
            return m_pOwner->NonDelegatingQueryInterface(riid, ppv);
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++m_cRef; }
        STDMETHODIMP_(ULONG) Release() override {
            const ULONG c = --m_cRef;
            if (c == 0) {
                delete m_pOwner;
            }
            return c;
        }
    private:
        CEqApo* m_pOwner;
        std::atomic<ULONG> m_cRef{1};
    };

    // Returns true for the one format we process: interleaved IEEE float32.
    static bool IsFloat32Format(const WAVEFORMATEX* wfx);

    // Worker thread helpers (non-RT): retry the settings mapping, service the
    // XFeed alloc/free requests.
    static DWORD WINAPI WorkerThreadProc(LPVOID pParam);
    void WorkerStep();
    void OpenSettingsMapping(); // best-effort; worker retries on failure
    void OpenStatusMapping();   // worker thread, best-effort with retry
    void PublishStatus();       // worker thread: heartbeat -> UI
    void CloseStatusMapping();  // UnlockForProcess / destructor
    void StopWorker();

    CInnerUnknown  m_inner;      // constructed first; outer falls back to it
    IUnknown*      m_pUnkOuter;  // controlling unknown (or &m_inner)
    IUnknown*      m_pFTM = nullptr; // aggregated free-threaded marshaler

    bool           m_initialized = false;
    bool           m_locked = false;

    std::wstring   m_endpointId;              // our render endpoint
    wchar_t        m_mappingName[128] = {};   // MMF name derived from endpoint id

    EqDsp          m_dsp;
    uint32_t       m_channels = 0;

    HANDLE         m_hMap = nullptr;          // shared-memory handle (non-RT only)
    std::atomic<const EqSettings*> m_pSettings{nullptr}; // mapped view, read-only
    int64_t        m_lastSequence = 0;        // last applied settings version
    EqSettings     m_localCopy;               // RT-side working copy

    // Heartbeat (APO -> UI): the RT thread only bumps counters; the worker
    // publishes them into the status mapping the UI created.
    wchar_t        m_statusName[160] = {};    // MMF name for the status block
    HANDLE         m_hStatusMap = nullptr;    // status mapping (worker only)
    MiniEQApoStatus* m_pStatus = nullptr;     // mapped view (worker only)
    std::atomic<uint64_t> m_rtCalls{0};       // RT thread (relaxed)
    std::atomic<int64_t>  m_rtLastQpc{0};     // RT thread (relaxed)
    uint32_t       m_rtQpcTick = 0;           // RT thread only
    int64_t        m_qpcFreq = 0;             // worker only
    uint32_t       m_sampleRate = 0;

    HANDLE         m_hWorkerThread = nullptr; // background worker (non-RT)
    HANDLE         m_hWorkerStop = nullptr;   // manual-reset stop event
};
