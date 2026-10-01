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
// A background worker thread (created in LockForProcess) services the
// crossfeed (XFeed) allocation/free requests -- heap work must never happen
// on the RT thread -- and publishes the heartbeat. The APO itself creates
// both shared-memory channels (it must: it runs in the audio engine,
// session 0, while the UI runs in the user's session).

#pragma once

#include <windows.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h> // IAudioSystemEffects3 (Win11 handshake)

#include <atomic>
#include <string>

#include "dsp.h"
#include "../shared/settings_channel.h"

// dllmain.cpp -- module lock count backing DllCanUnloadNow. Every live APO
// object holds one count from CreateInstance until its inner refcount reaches
// zero; without it ole32 can unload the DLL while instances are alive.
extern volatile LONG g_MiniEQDllLockCount;

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
    STDMETHODIMP Initialize(UINT32 cbDataSize, BYTE* pbyData) noexcept override;
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
                                  APO_CONNECTION_PROPERTY** ppOutputConnections) noexcept override;
    STDMETHODIMP_(UINT32) CalcInputFrames(UINT32 u32OutputFrameCount) override;
    STDMETHODIMP_(UINT32) CalcOutputFrames(UINT32 u32InputFrameCount) override;

    // IAudioProcessingObjectConfiguration
    STDMETHODIMP LockForProcess(UINT32 u32NumInputConnections,
                               APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                               UINT32 u32NumOutputConnections,
                               APO_CONNECTION_DESCRIPTOR** ppOutputConnections) noexcept override;
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
                // Release the DLL lifetime lock *before* deleting the owner:
                // after this point no code in this module may run for the
                // object, but the module itself must stay mapped until the
                // engine's wrapper finishes its own Release.
                InterlockedDecrement(&g_MiniEQDllLockCount);
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

    // R1: build a float32 IAudioMediaType twin of a non-float32 format, so
    // negotiation can answer S_FALSE ("not that, but this") instead of
    // APOERR_FORMAT_NOT_SUPPORTED (which makes the engine silently drop us).
    static HRESULT SuggestFloat32MediaType(const WAVEFORMATEX* wfx,
                                          IAudioMediaType** ppOut);
    // R1: our own format verdict, ignoring any chained child. S_OK accepts
    // as-is; S_FALSE carries a float32 suggestion; APOERR is reserved for
    // channel counts our fixed RT-safe state genuinely cannot process.
    static HRESULT OwnFormatVerdict(const WAVEFORMATEX* wfx,
                                    IAudioMediaType* pRequested,
                                    IAudioMediaType** ppOut);
    // R2: release the chained child APO, if any. Idempotent.
    void ReleaseChild();

    // Worker thread helpers (non-RT): (re)create the channel mappings if the
    // initial creation in LockForProcess failed, service XFeed requests.
    static DWORD WINAPI WorkerThreadProc(LPVOID pParam);
    void WorkerStep();
    void CreateSettingsMapping(); // idempotent; worker retries on failure
    void CreateStatusMapping();   // worker thread, best-effort with retry
    void PublishStatus();       // worker thread: heartbeat -> UI
    void CloseStatusMapping();  // UnlockForProcess / destructor
    void CreateGlobalMapping(); // idempotent; worker retries on failure
    void CloseGlobalMapping();  // UnlockForProcess / destructor
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
    // publishes them into the status mapping the APO created.
    wchar_t        m_statusName[160] = {};    // MMF name for the status block
    HANDLE         m_hStatusMap = nullptr;    // status mapping (worker only)
    MiniEQApoStatus* m_pStatus = nullptr;     // mapped view (worker only)
    std::atomic<uint64_t> m_rtCalls{0};       // RT thread (relaxed)
    // Deferred RT diagnostics: the audio thread only records facts into
    // these relaxed atomics -- no QPC, no file tracing there. The worker
    // thread samples the clock and writes the trace lines (PublishStatus).
    std::atomic<uint32_t> m_rtFirstCallFrames{0};   // validFrames, 1st call
    std::atomic<uint32_t> m_rtFirstCallChannels{0}; // m_channels, 1st call
    std::atomic<int32_t>  m_rtFirstCallBypass{0};    // bypass flag, 1st call
    std::atomic<float>    m_rtFirstCallGain0{0.0f};  // band 0 gain, 1st call
    std::atomic<bool>     m_rtFirstCallPending{false};
    std::atomic<int32_t>  m_rtGlobalChangeValue{0};
    std::atomic<int64_t>  m_rtGlobalChangeSeq{0};
    std::atomic<bool>     m_rtGlobalChangePending{false};
    std::atomic<bool>     m_rtExceptionPending{false};
    uint64_t       m_workerLastCalls = 0;     // worker only
    int64_t        m_qpcFreq = 0;             // worker only
    uint32_t       m_sampleRate = 0;

    // Global on/off (UI -> APO): one flag shared by every endpoint.
    // The APO is the creator (only session 0 holds SeCreateGlobalPrivilege);
    // the UI opens the same name and writes. Fail-open: no channel yet
    // means enabled, so audio keeps working with older UI builds.
    wchar_t        m_globalName[64] = {};     // "Global\MiniEQ__Enabled"
    HANDLE         m_hGlobalMap = nullptr;    // global-state mapping (non-RT only)
    std::atomic<const MiniEQGlobalState*> m_pGlobal{nullptr}; // mapped view, read-only
    int64_t        m_lastGlobalSeq = 0;       // last adopted global version
    bool           m_globalEnabled = true;    // RT-side cache; default enabled

    // R2: chained child APO -- the incumbent this APO displaced from the
    // endpoint's SFX slot (e.g. a vendor effect), stashed at attach time by
    // MiniEQ_AttachToEndpoint under HKLM\SOFTWARE\MiniEQ\ChildAPO. Created
    // in Initialize; every call is delegated (negotiate -> lock -> process
    // -> unlock) and any failure drops the child for the stream instead of
    // failing the user's audio.
    IAudioProcessingObject* m_childAPO = nullptr;
    IAudioProcessingObjectRT* m_childRT = nullptr;
    IAudioProcessingObjectConfiguration* m_childConfig = nullptr;
    bool           m_childDroppedForStream = false;

    HANDLE         m_hWorkerThread = nullptr; // background worker (non-RT)
    HANDLE         m_hWorkerStop = nullptr;   // manual-reset stop event
};
