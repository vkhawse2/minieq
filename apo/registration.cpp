// registration.cpp -- registry writes that install and attach the APO.

#include "registration.h"
#include "guids.h"

#include <strsafe.h>

// IID_IAudioProcessingObject -- the APO interface we implement.
static const wchar_t* kApoInterface0 = L"{FD7F2B29-24D0-4B5C-B177-592C39F9CA10}";
// PKEY_FX_StreamEffectClsid -- the SFX slot in an endpoint's FxProperties.
static const wchar_t* kFxSfxSlot = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";

static HRESULT ClsidString(wchar_t* out, size_t cch) {
    if (StringFromGUID2(CLSID_MiniEQAPO, out, (int)cch) == 0) {
        return E_FAIL;
    }
    return S_OK;
}

static HRESULT SetSz(HKEY root, const wchar_t* subkey, const wchar_t* valueName,
                     const wchar_t* data) {
    HKEY h = nullptr;
    LONG rc = RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE,
                              nullptr, &h, nullptr);
    if (rc != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(rc);
    }
    rc = RegSetValueExW(h, valueName, 0, REG_SZ, (const BYTE*)data,
                        (DWORD)((wcslen(data) + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
}

static HRESULT SetDword(HKEY root, const wchar_t* subkey, const wchar_t* valueName,
                        DWORD data) {
    HKEY h = nullptr;
    LONG rc = RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE,
                              nullptr, &h, nullptr);
    if (rc != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(rc);
    }
    rc = RegSetValueExW(h, valueName, 0, REG_DWORD, (const BYTE*)&data, sizeof(data));
    RegCloseKey(h);
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
}

HRESULT MiniEQ_RegisterComClass(const wchar_t* dllPath) {
    wchar_t clsid[64] = {};
    HRESULT hr = ClsidString(clsid, ARRAYSIZE(clsid));
    if (FAILED(hr)) return hr;

    wchar_t key[128] = {};
    StringCchPrintfW(key, ARRAYSIZE(key), L"SOFTWARE\\Classes\\CLSID\\%s", clsid);
    hr = SetSz(HKEY_LOCAL_MACHINE, key, nullptr, L"MiniEQ APO");
    if (FAILED(hr)) return hr;

    StringCchPrintfW(key, ARRAYSIZE(key),
                     L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", clsid);
    hr = SetSz(HKEY_LOCAL_MACHINE, key, nullptr, dllPath);
    if (FAILED(hr)) return hr;
    return SetSz(HKEY_LOCAL_MACHINE, key, L"ThreadingModel", L"Both");
}

HRESULT MiniEQ_UnregisterComClass() {
    wchar_t clsid[64] = {};
    if (FAILED(ClsidString(clsid, ARRAYSIZE(clsid)))) return E_FAIL;
    wchar_t key[128] = {};
    StringCchPrintfW(key, ARRAYSIZE(key),
                     L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", clsid);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    StringCchPrintfW(key, ARRAYSIZE(key), L"SOFTWARE\\Classes\\CLSID\\%s", clsid);
    LONG rc = RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
}

HRESULT MiniEQ_RegisterApoDeclaration() {
    wchar_t clsid[64] = {};
    HRESULT hr = ClsidString(clsid, ARRAYSIZE(clsid));
    if (FAILED(hr)) return hr;

    wchar_t key[160] = {};
    StringCchPrintfW(key, ARRAYSIZE(key),
                     L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\%s", clsid);

    hr = SetSz(HKEY_LOCAL_MACHINE, key, L"FriendlyName", L"MiniEQ");
    if (FAILED(hr)) return hr;
    hr = SetSz(HKEY_LOCAL_MACHINE, key, L"Copyright", L"Copyright (c) Vishal");
    if (FAILED(hr)) return hr;
    hr = SetDword(HKEY_LOCAL_MACHINE, key, L"MajorVersion", 1);
    if (FAILED(hr)) return hr;
    hr = SetDword(HKEY_LOCAL_MACHINE, key, L"MinorVersion", 0);
    if (FAILED(hr)) return hr;
    hr = SetDword(HKEY_LOCAL_MACHINE, key, L"Flags", 14); // APO_FLAG_DEFAULT
    if (FAILED(hr)) return hr;
    hr = SetDword(HKEY_LOCAL_MACHINE, key, L"MinInputConnections", 1);
    if (FAILED(hr)) return hr;
    hr = SetDword(HKEY_LOCAL_MACHINE, key, L"MaxInputConnections", 1);
    if (FAILED(hr)) return hr;
    hr = SetDword(HKEY_LOCAL_MACHINE, key, L"MinOutputConnections", 1);
    if (FAILED(hr)) return hr;
    hr = SetDword(HKEY_LOCAL_MACHINE, key, L"MaxOutputConnections", 1);
    if (FAILED(hr)) return hr;
    hr = SetDword(HKEY_LOCAL_MACHINE, key, L"MaxInstances", 0xFFFFFFFF);
    if (FAILED(hr)) return hr;
    hr = SetDword(HKEY_LOCAL_MACHINE, key, L"NumAPOInterfaces", 1);
    if (FAILED(hr)) return hr;
    return SetSz(HKEY_LOCAL_MACHINE, key, L"APOInterface0", kApoInterface0);
}

HRESULT MiniEQ_UnregisterApoDeclaration() {
    wchar_t clsid[64] = {};
    if (FAILED(ClsidString(clsid, ARRAYSIZE(clsid)))) return E_FAIL;
    wchar_t key[160] = {};
    StringCchPrintfW(key, ARRAYSIZE(key),
                     L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\%s", clsid);
    LONG rc = RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
}

static HRESULT FxPropertiesKey(const wchar_t* endpointId, wchar_t* out, size_t cch) {
    return StringCchPrintfW(out, cch,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\%s\\FxProperties",
        endpointId);
}

HRESULT MiniEQ_AttachToEndpoint(const wchar_t* endpointId) {
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;
    wchar_t clsid[64] = {};
    HRESULT hr = ClsidString(clsid, ARRAYSIZE(clsid));
    if (FAILED(hr)) return hr;

    wchar_t key[512] = {};
    hr = FxPropertiesKey(endpointId, key, ARRAYSIZE(key));
    if (FAILED(hr)) return hr;

    // The FxProperties key already exists; open it and set the value in place.
    // (Admins cannot create subkeys here, only set values.)
    HKEY h = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_SET_VALUE, &h);
    if (rc != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(rc);
    }
    rc = RegSetValueExW(h, kFxSfxSlot, 0, REG_SZ, (const BYTE*)clsid,
                        (DWORD)((wcslen(clsid) + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
}

HRESULT MiniEQ_DetachFromEndpoint(const wchar_t* endpointId) {
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;
    wchar_t key[512] = {};
    HRESULT hr = FxPropertiesKey(endpointId, key, ARRAYSIZE(key));
    if (FAILED(hr)) return hr;

    HKEY h = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_SET_VALUE, &h);
    if (rc != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(rc);
    }
    rc = RegDeleteValueW(h, kFxSfxSlot);
    RegCloseKey(h);
    // Deleting a value that isn't there is fine.
    if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
}

HRESULT MiniEQ_IsAttachedToEndpoint(const wchar_t* endpointId, bool* attached) {
    if (attached == nullptr) return E_POINTER;
    *attached = false;
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;

    wchar_t clsid[64] = {};
    HRESULT hr = ClsidString(clsid, ARRAYSIZE(clsid));
    if (FAILED(hr)) return hr;

    wchar_t key[512] = {};
    hr = FxPropertiesKey(endpointId, key, ARRAYSIZE(key));
    if (FAILED(hr)) return hr;

    HKEY h = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, &h);
    if (rc != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(rc);
    }
    wchar_t value[64] = {};
    DWORD size = sizeof(value), type = 0;
    rc = RegQueryValueExW(h, kFxSfxSlot, nullptr, &type, (BYTE*)value, &size);
    RegCloseKey(h);
    if (rc == ERROR_SUCCESS && type == REG_SZ) {
        *attached = (_wcsicmp(value, clsid) == 0);
    }
    return S_OK;
}
