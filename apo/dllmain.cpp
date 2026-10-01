// dllmain.cpp -- module entry, class factory, self-registration.
//
// Exports: DllGetClassObject, DllCanUnloadNow, DllRegisterServer,
//          DllUnregisterServer (see eqapo.def).
//
// DllRegisterServer (run elevated, e.g. `regsvr32 MiniEQ_APO.dll`) writes:
//   1. HKLM\SOFTWARE\Classes\CLSID\{our-clsid}\InprocServer32  (COM class)
//   2. HKLM\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\{our-clsid}
//      (APO declaration the audio engine looks up)
// Attaching the APO to a specific output device (the FxProperties slot under
// MMDevices\...\Render\{endpoint-id}) is done separately by AttachToEndpoint()
// in registration.cpp, because it is per-device, not per-install.

#include <windows.h>

#include <new>

#include "eq_apo.h"
#include "guids.h"
#include "registration.h"
#include "trace.h"

#include <strsafe.h>

// Module lock count backing DllCanUnloadNow. The class factory's AddRef /
// Release / LockServer touch it, and -- critically -- every live APO object
// holds one count from CreateInstance until its inner refcount reaches zero
// (see CInnerUnknown::Release in eq_apo.h). Without the per-object count the
// engine releases the factory right after CreateInstance, DllCanUnloadNow
// wrongly returns S_OK while APO instances are alive, ole32 unloads our DLL
// mid-session, and the engine's CSystemEffectWrapper then calls through
// unmapped memory -- the 0xc0000005 crash seen in audiodg.exe.
volatile LONG g_MiniEQDllLockCount = 0;
static HMODULE g_hModule = nullptr;

//------------------------------------------------------------------------------
// Class factory
//------------------------------------------------------------------------------

class CEqApoFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return (ULONG)InterlockedIncrement(&g_MiniEQDllLockCount);
    }
    STDMETHODIMP_(ULONG) Release() override {
        return (ULONG)InterlockedDecrement(&g_MiniEQDllLockCount);
    }
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        MiniEQ_Trace(L"MiniEQ_APO: CreateInstance outer=%s riidIsIUnknown=%d",
                     pUnkOuter != nullptr ? L"non-null (engine aggregates)"
                                          : L"null",
                     IsEqualGUID(riid, IID_IUnknown));
        // The audio engine AGGREGATES system-effect APOs: CreateInstance
        // arrives with a non-null controlling unknown and IID_IUnknown.
        // Answering CLASS_E_NOAGGREGATION here makes the engine silently
        // skip the effect -- no error, no event log, nothing in a trace.
        // (COM rule: with a non-null outer, only IID_IUnknown may be asked.)
        if (pUnkOuter != nullptr && riid != IID_IUnknown) return E_NOINTERFACE;
        CEqApo* apo = new (std::nothrow) CEqApo(pUnkOuter);
        if (apo == nullptr) return E_OUTOFMEMORY;
        // Hold the DLL loaded for this object's whole lifetime. Balanced in
        // CInnerUnknown::Release when the inner count reaches zero (which
        // deletes the owner) -- including the QI-failure path below, where
        // NonDelegatingRelease drops the count to zero and deletes.
        InterlockedIncrement(&g_MiniEQDllLockCount);
        HRESULT hr = apo->NonDelegatingQueryInterface(riid, ppv);
        apo->NonDelegatingRelease(); // balance the initial inner ref
        MiniEQ_Trace(L"MiniEQ_APO: CreateInstance -> hr=0x%08lx", hr);
        return hr;
    }
    STDMETHODIMP LockServer(BOOL bLock) override {
        if (bLock) InterlockedIncrement(&g_MiniEQDllLockCount);
        else InterlockedDecrement(&g_MiniEQDllLockCount);
        return S_OK;
    }
};

//------------------------------------------------------------------------------
// Standard DLL exports
//------------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        // Loader-lock safe: OutputDebugString only, no file I/O here.
        wchar_t host[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, host, ARRAYSIZE(host));
        wchar_t msg[640] = {};
        StringCchPrintfW(msg, ARRAYSIZE(msg),
                         L"MiniEQ_APO: DllMain PROCESS_ATTACH host=\"%s\" pid=%lu",
                         host, GetCurrentProcessId());
        MiniEQ_TraceNoFile(msg);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
try {
    // Not under the loader lock: safe to resolve the log file here.
    MiniEQ_TraceInit();
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualGUID(rclsid, CLSID_MiniEQAPO)) {
        MiniEQ_Trace(L"MiniEQ_APO: DllGetClassObject for foreign CLSID -> CLASS_E_CLASSNOTAVAILABLE");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    static CEqApoFactory factory;
    HRESULT hr = factory.QueryInterface(riid, ppv);
    MiniEQ_Trace(L"MiniEQ_APO: DllGetClassObject (our CLSID) -> hr=0x%08lx", hr);
    return hr;
} catch (...) {
    // The engine loads this DLL in-process; never let an exception escape
    // into it from the class factory entry point.
    return E_FAIL;
}

STDAPI DllCanUnloadNow() {
    return g_MiniEQDllLockCount == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    wchar_t dllPath[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hModule, dllPath, ARRAYSIZE(dllPath)) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT hr = MiniEQ_RegisterComClass(dllPath);
    if (FAILED(hr)) return hr;
    hr = MiniEQ_RegisterApoDeclaration();
    if (FAILED(hr)) return hr;
    // The trace log directory must be writable by the audio engine
    // (service identity), not just the installing user -- otherwise the
    // APO's diagnostic trace silently fails. Best-effort: a log-dir
    // problem must not fail the install itself.
    MiniEQ_EnsureLogDirForInstall();
    return S_OK;
}

STDAPI DllUnregisterServer() {
    MiniEQ_UnregisterApoDeclaration();
    return MiniEQ_UnregisterComClass();
}
