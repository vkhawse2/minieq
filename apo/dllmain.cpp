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

static volatile LONG g_lockCount = 0;
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
        return (ULONG)InterlockedIncrement(&g_lockCount);
    }
    STDMETHODIMP_(ULONG) Release() override {
        return (ULONG)InterlockedDecrement(&g_lockCount);
    }
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;
        CEqApo* apo = new (std::nothrow) CEqApo();
        if (apo == nullptr) return E_OUTOFMEMORY;
        HRESULT hr = apo->QueryInterface(riid, ppv);
        apo->Release(); // QueryInterface took its own ref on success
        return hr;
    }
    STDMETHODIMP LockServer(BOOL bLock) override {
        if (bLock) InterlockedIncrement(&g_lockCount);
        else InterlockedDecrement(&g_lockCount);
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
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualGUID(rclsid, CLSID_MiniEQAPO)) return CLASS_E_CLASSNOTAVAILABLE;
    static CEqApoFactory factory;
    return factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return g_lockCount == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    wchar_t dllPath[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hModule, dllPath, ARRAYSIZE(dllPath)) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT hr = MiniEQ_RegisterComClass(dllPath);
    if (FAILED(hr)) return hr;
    return MiniEQ_RegisterApoDeclaration();
}

STDAPI DllUnregisterServer() {
    MiniEQ_UnregisterApoDeclaration();
    return MiniEQ_UnregisterComClass();
}
