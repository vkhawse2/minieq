// registration.cpp -- registry writes that install and attach the APO.

#include "registration.h"
#include "guids.h"

#include <aclapi.h>
#include <audioenginebaseapo.h> // APO_FLAG_INPLACE
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
    // Must match what GetRegistrationProperties() reports at runtime
    // (APO_FLAG_INPLACE): the engine cross-checks the two, and a mismatch
    // can get the APO rejected.
    hr = SetDword(HKEY_LOCAL_MACHINE, key, L"Flags", APO_FLAG_INPLACE);
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

static HRESULT EndpointGuid(const wchar_t* endpointId, wchar_t* out, size_t cch) {
    // The MMDevice ID looks like "{0.0.0.00000000}.{endpoint-guid}", but the
    // MMDevices registry key is named with the bare endpoint GUID only --
    // the part after the last dot. This is also what PKEY_AudioEndpoint_GUID
    // returns, and what Equalizer APO enumerates under
    // ...\MMDevices\Audio\Render. Using the full ID builds a path Windows
    // never reads; the DACL repair then targets a nonexistent parent key
    // and surfaces as ERROR_FILE_NOT_FOUND (0x80070002).
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;
    const wchar_t* dot = wcsrchr(endpointId, L'.');
    const wchar_t* guid = (dot != nullptr) ? dot + 1 : endpointId;
    return StringCchCopyW(out, cch, guid);
}

static HRESULT EndpointKey(const wchar_t* endpointId, wchar_t* out, size_t cch) {
    // MMDevices path of the endpoint itself (no FxProperties suffix).
    wchar_t guid[64] = {};
    HRESULT hr = EndpointGuid(endpointId, guid, ARRAYSIZE(guid));
    if (FAILED(hr)) return hr;
    return StringCchPrintfW(out, cch,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\%s",
        guid);
}

static HRESULT FxPropertiesKey(const wchar_t* endpointId, wchar_t* out, size_t cch) {
    wchar_t guid[64] = {};
    HRESULT hr = EndpointGuid(endpointId, guid, ARRAYSIZE(guid));
    if (FAILED(hr)) return hr;
    return StringCchPrintfW(out, cch,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\%s\\FxProperties",
        guid);
}

static bool EnablePrivilege(const wchar_t* privilegeName) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = LookupPrivilegeValueW(nullptr, privilegeName,
                                    &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) &&
              GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok != FALSE;
}

// Grant the local Administrators group full control over the endpoint's
// registry key (inherited by subkeys). Some endpoints -- notably Bluetooth
// ones -- ship a DACL that denies even elevated administrators the right
// to create the FxProperties subkey, so attach fails with
// ERROR_ACCESS_DENIED. Existing ACEs are preserved; ours is merged in.
static HRESULT GrantAdminsKeyAllAccess(const wchar_t* endpointSubkey) {
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminSid = nullptr;
    if (!AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &adminSid)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    HKEY h = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, endpointSubkey, 0,
                            READ_CONTROL | WRITE_DAC, &h);
    if (rc == ERROR_ACCESS_DENIED) {
        // Cannot even change the DACL: take ownership first. The
        // Administrators group holds SeTakeOwnershipPrivilege when elevated.
        if (!EnablePrivilege(L"SeTakeOwnershipPrivilege")) {
            FreeSid(adminSid);
            return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        }
        rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, endpointSubkey, 0,
                           READ_CONTROL | WRITE_OWNER, &h);
        if (rc != ERROR_SUCCESS) {
            FreeSid(adminSid);
            return HRESULT_FROM_WIN32(rc);
        }
        DWORD err = SetSecurityInfo(h, SE_REGISTRY_KEY,
                                    OWNER_SECURITY_INFORMATION,
                                    adminSid, nullptr, nullptr, nullptr);
        RegCloseKey(h);
        h = nullptr;
        if (err != ERROR_SUCCESS) {
            FreeSid(adminSid);
            return HRESULT_FROM_WIN32(err);
        }
        rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, endpointSubkey, 0,
                           READ_CONTROL | WRITE_DAC, &h);
    }
    if (rc != ERROR_SUCCESS) {
        if (h) RegCloseKey(h);
        FreeSid(adminSid);
        return HRESULT_FROM_WIN32(rc);
    }

    PACL oldDacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD err = GetSecurityInfo(h, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, &oldDacl, nullptr, &sd);
    if (err != ERROR_SUCCESS || oldDacl == nullptr) {
        if (sd) LocalFree(sd);
        RegCloseKey(h);
        FreeSid(adminSid);
        return HRESULT_FROM_WIN32(err != ERROR_SUCCESS ? err
                                                      : ERROR_ACCESS_DENIED);
    }

    EXPLICIT_ACCESSW ea = {};
    ea.grfAccessPermissions = KEY_ALL_ACCESS;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)adminSid;

    PACL newDacl = nullptr;
    err = SetEntriesInAclW(1, &ea, oldDacl, &newDacl);
    if (err == ERROR_SUCCESS) {
        err = SetSecurityInfo(h, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, newDacl, nullptr);
    }
    if (newDacl) LocalFree(newDacl);
    if (sd) LocalFree(sd);
    RegCloseKey(h);
    FreeSid(adminSid);
    return err == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(err);
}

// Create the FxProperties subkey if needed, repairing a restrictive DACL on
// the endpoint key when the system denies us.
static HRESULT OpenFxPropertiesForWrite(const wchar_t* endpointId, HKEY* out) {
    wchar_t key[512] = {};
    HRESULT hr = FxPropertiesKey(endpointId, key, ARRAYSIZE(key));
    if (FAILED(hr)) return hr;

    HKEY h = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0,
                              KEY_SET_VALUE, nullptr, &h, nullptr);
    if (rc == ERROR_ACCESS_DENIED) {
        wchar_t parent[512] = {};
        hr = EndpointKey(endpointId, parent, ARRAYSIZE(parent));
        if (FAILED(hr)) return hr;
        hr = GrantAdminsKeyAllAccess(parent);
        if (FAILED(hr)) return hr;
        rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0,
                             KEY_SET_VALUE, nullptr, &h, nullptr);
    }
    if (rc != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(rc);
    }
    *out = h;
    return S_OK;
}

HRESULT MiniEQ_AttachToEndpoint(const wchar_t* endpointId) {
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;
    wchar_t clsid[64] = {};
    HRESULT hr = ClsidString(clsid, ARRAYSIZE(clsid));
    if (FAILED(hr)) return hr;

    // The FxProperties subkey often does not exist yet (fresh endpoint);
    // create it if needed, repairing a restrictive DACL when denied.
    HKEY h = nullptr;
    hr = OpenFxPropertiesForWrite(endpointId, &h);
    if (FAILED(hr)) return hr;

    LONG rc = RegSetValueExW(h, kFxSfxSlot, 0, REG_SZ, (const BYTE*)clsid,
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
    if (rc == ERROR_FILE_NOT_FOUND) {
        return S_OK; // FxProperties never created: nothing to detach.
    }
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
    if (rc == ERROR_FILE_NOT_FOUND) {
        *attached = false; // FxProperties never created: not attached.
        return S_OK;
    }
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
