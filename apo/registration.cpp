// registration.cpp -- registry writes that install and attach the APO.

#include "registration.h"
#include "guids.h"

#include <aclapi.h>
#include <audioenginebaseapo.h> // APO_FLAG_INPLACE
#include <sddl.h> // ConvertStringSecurityDescriptorToSecurityDescriptorW
#include <strsafe.h>
#include <setupapi.h>  // SetupDi* for device re-enumeration
#include <devguid.h>   // GUID_DEVCLASS_MEDIA
#include <mmdeviceapi.h> // IMMDeviceEnumerator (friendly name lookup)
#include <propsys.h>   // IPropertyStore, PROPVARIANT
#include <propkey.h>   // PROPERTYKEY (must precede functiondiscoverykeys_devpkey.h)
#include <wctype.h>    // towlower

// PKEY_Device_FriendlyName -- defined TU-local (the SDK header only declares
// it, which links LNK2019; see AGENTS.md).
static const PROPERTYKEY kPkeyDeviceFriendlyName = {
    { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA1, 0xE0, 0xE0 } },
    14
};

// IID_IAudioProcessingObject -- the APO interface we implement.
static const wchar_t* kApoInterface0 = L"{FD7F2B29-24D0-4B5C-B177-592C39F9CA10}";
// PKEY_FX_StreamEffectClsid -- the SFX slot in an endpoint's FxProperties.
static const wchar_t* kFxSfxSlot = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";
// PKEY_FX_EndpointEffectClsid -- the EFX slot in an endpoint's FxProperties.
// EFX runs after all mixing at the endpoint, downstream of the spatial-sound
// render, so an EQ attached here is not bypassed when spatial sound is on.
// (Route 1 experiment: does the EQ survive spatial/Atmos from the EFX slot?)
static const wchar_t* kFxEfxSlot = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";

// Active effect slot. SFX by default; builds compiled with MINIEQ_EFX_SLOT
// default to EFX (test builds). MiniEQ_SetEffectSlot overrides at runtime --
// reserved for a future slot-choice UI; nothing calls it yet.
static const wchar_t* g_fxSlot =
#ifdef MINIEQ_EFX_SLOT
    kFxEfxSlot;
#else
    kFxSfxSlot;
#endif

void MiniEQ_SetEffectSlot(bool useEfx) {
    g_fxSlot = useEfx ? kFxEfxSlot : kFxSfxSlot;
}

const wchar_t* MiniEQ_EffectSlotShortName() {
    return g_fxSlot == kFxEfxSlot ? L"EFX" : L"SFX";
}

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

// Creates %PROGRAMDATA%\MiniEQ and grants Everyone read/write (inherited by
// the trace log file), so the audio engine -- which runs as a service
// identity, not as the installing user -- can append to the diagnostic log
// the UI tails. Without this, the log file (if created first by the UI)
// carries a user-only DACL and the APO's trace writes silently fail, which
// is exactly the "empty log, APO apparently dead" symptom. Called from
// DllRegisterServer, which always runs elevated (installer / regsvr32).
HRESULT MiniEQ_EnsureLogDirForInstall() {
    wchar_t dir[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"PROGRAMDATA", dir, ARRAYSIZE(dir));
    if (n == 0 || n >= ARRAYSIZE(dir)) {
        return E_FAIL;
    }
    if (FAILED(StringCchCatW(dir, ARRAYSIZE(dir), L"\\MiniEQ"))) {
        return E_FAIL;
    }
    CreateDirectoryW(dir, nullptr); // ERROR_ALREADY_EXISTS is fine

    // D: SY/BA full; Everyone read+write, inherited by children (OICI).
    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GRGW;;;WD)",
            SDDL_REVISION_1, &pSD, nullptr)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    PACL pDacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    HRESULT hr = S_OK;
    if (GetSecurityDescriptorDacl(pSD, &present, &pDacl, &defaulted) && present) {
        DWORD rc = SetNamedSecurityInfoW(dir, SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, pDacl, nullptr);
        if (rc != ERROR_SUCCESS) {
            hr = HRESULT_FROM_WIN32(rc);
        } else {
            // Also repair a pre-existing log file: it may have been created
            // by the UI (user-only DACL) before this ran.
            wchar_t log[MAX_PATH] = {};
            if (SUCCEEDED(StringCchPrintfW(log, ARRAYSIZE(log),
                                           L"%s\\apo-trace.log", dir))) {
                rc = SetNamedSecurityInfoW(log, SE_FILE_OBJECT,
                                           DACL_SECURITY_INFORMATION,
                                           nullptr, nullptr, pDacl, nullptr);
                // ERROR_FILE_NOT_FOUND just means no log yet; not a failure.
                if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
                    hr = HRESULT_FROM_WIN32(rc);
                }
            }
        }
    } else {
        hr = E_FAIL;
    }
    LocalFree(pSD);
    return hr;
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

// R2: registry home of the stashed child-APO CLSIDs, one value per endpoint
// (bare GUID). Written elevated at attach time; read by the APO inside the
// audio engine (world-readable under HKLM\SOFTWARE).
static HRESULT ChildApoKey(const wchar_t* endpointId, wchar_t* out, size_t cch) {
    wchar_t guid[64] = {};
    HRESULT hr = EndpointGuid(endpointId, guid, ARRAYSIZE(guid));
    if (FAILED(hr)) return hr;
    return StringCchPrintfW(out, cch, L"SOFTWARE\\MiniEQ\\ChildAPO\\%s", guid);
}

HRESULT MiniEQ_StashChildApoClsid(const wchar_t* endpointId,
                                 const wchar_t* childClsid) {
    if (endpointId == nullptr || childClsid == nullptr) return E_INVALIDARG;
    wchar_t key[256] = {};
    HRESULT hr = ChildApoKey(endpointId, key, ARRAYSIZE(key));
    if (FAILED(hr)) return hr;
    return SetSz(HKEY_LOCAL_MACHINE, key, nullptr, childClsid);
}

HRESULT MiniEQ_ReadChildApoClsid(const wchar_t* endpointId, wchar_t* out,
                                size_t cch) {
    if (out == nullptr || cch == 0) return E_POINTER;
    out[0] = L'\0';
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;
    wchar_t key[256] = {};
    HRESULT hr = ChildApoKey(endpointId, key, ARRAYSIZE(key));
    if (FAILED(hr)) return hr;

    HKEY h = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, &h);
    if (rc == ERROR_FILE_NOT_FOUND) {
        return S_FALSE; // never stashed: no child
    }
    if (rc != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(rc);
    }
    DWORD size = (DWORD)(cch * sizeof(wchar_t)), type = 0;
    rc = RegQueryValueExW(h, nullptr, nullptr, &type, (BYTE*)out, &size);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS || type != REG_SZ) {
        out[0] = L'\0';
        return S_FALSE;
    }
    // Defensive: strip a trailing ",N" slot suffix if some stash (or unusual
    // slot data) carried it -- CLSIDFromString needs the bare CLSID. Bare
    // CLSIDs pass through untouched.
    wchar_t* comma = wcsrchr(out, L',');
    if (comma != nullptr && comma > out && *(comma - 1) == L'}') {
        *comma = L'\0';
    }
    return S_OK;
}

HRESULT MiniEQ_ClearChildApoClsid(const wchar_t* endpointId) {
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;
    wchar_t key[256] = {};
    HRESULT hr = ChildApoKey(endpointId, key, ARRAYSIZE(key));
    if (FAILED(hr)) return hr;
    LONG rc = RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
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

// Forward: defined below MiniEQ_AttachToEndpoint.
static HRESULT DetachFromSlot(const wchar_t* endpointId, const wchar_t* slot);

static HRESULT AttachToSlot(const wchar_t* endpointId, const wchar_t* slot) {
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;
    wchar_t clsid[64] = {};
    HRESULT hr = ClsidString(clsid, ARRAYSIZE(clsid));
    if (FAILED(hr)) return hr;

    // The FxProperties subkey often does not exist yet (fresh endpoint);
    // create it if needed, repairing a restrictive DACL when denied.
    HKEY h = nullptr;
    hr = OpenFxPropertiesForWrite(endpointId, &h);
    if (FAILED(hr)) return hr;

    // R2: chain, don't just evict. Stash the incumbent APO (if it is a
    // real third-party CLSID and not us) so the engine keeps running it as
    // our child. TRANSACTIONAL: the slot is overwritten only after the
    // incumbent's CLSID is verified on disk. A stash failure fails the
    // attach -- silently losing a third-party APO with no way to restore
    // it is worse than not attaching.
    wchar_t incumbent[64] = {};
    DWORD qsize = sizeof(incumbent), qtype = 0;
    LONG qrc = RegQueryValueExW(h, slot, nullptr, &qtype,
                               (BYTE*)incumbent, &qsize);
    if (qrc == ERROR_SUCCESS && qtype == REG_SZ && incumbent[0] != L'\0' &&
        _wcsicmp(incumbent, clsid) != 0) {
        // Normalize the same way the restore path reads it (strip a
        // trailing ",N" slot suffix) so the verify below compares like
        // with like, and the stash holds exactly what detach restores.
        wchar_t* comma = wcsrchr(incumbent, L',');
        if (comma != nullptr && comma > incumbent && *(comma - 1) == L'}') {
            *comma = L'\0';
        }
        hr = MiniEQ_StashChildApoClsid(endpointId, incumbent);
        if (FAILED(hr)) {
            RegCloseKey(h);
            return hr; // stash failed: leave the slot untouched
        }
        wchar_t verify[64] = {};
        if (MiniEQ_ReadChildApoClsid(endpointId, verify, ARRAYSIZE(verify)) != S_OK ||
            _wcsicmp(verify, incumbent) != 0) {
            RegCloseKey(h);
            return E_FAIL; // stash didn't land: leave the slot untouched
        }
    }

    LONG rc = RegSetValueExW(h, slot, 0, REG_SZ, (const BYTE*)clsid,
                             (DWORD)((wcslen(clsid) + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
}

HRESULT MiniEQ_AttachToEndpoint(const wchar_t* endpointId) {
    return MiniEQ_AttachToEndpointEx(endpointId, /*force=*/false);
}

HRESULT MiniEQ_AttachToEndpointEx(const wchar_t* endpointId, bool force) {
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;

    // 0. Probe first: if the active slot already points at us, the registry
    //    is already correct. We still run the cheap repair layers (1-2)
    //    below, but skip the slot rewrite AND the device restart -- the
    //    restart briefly "unplugs" the endpoint and Windows fails any
    //    playing audio over to another output (e.g. laptop speakers).
    //    Skipping it also preserves a legitimate child-APO stash, which a
    //    blind rewrite would clear. Returns S_FALSE in that case so the
    //    caller can report "already attached, no restart needed".
    //    force=true overrides the skip: the caller has evidence the engine
    //    never picked up the registration, so the slot is rewritten and the
    //    device is re-enumerated regardless.
    bool alreadyAttached = false;
    const bool probeOk = SUCCEEDED(
        MiniEQ_IsAttachedToEndpoint(endpointId, &alreadyAttached));
    const bool skipRewrite = probeOk && alreadyAttached && !force;

    // 1. Repair layers 1+2 (COM class + APO declaration). The installer writes
    //    these via DllRegisterServer, but a failed/interrupted upgrade can
    //    leave them stale -- and the audio engine will not load the APO
    //    without the AudioEngine declaration key. Best-effort: the DLL sits
    //    next to the --attach helper (MiniEQ.exe).
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath)) > 0) {
            wchar_t* slash = wcsrchr(exePath, L'\\');
            if (slash != nullptr) {
                *(slash + 1) = L'\0';
                wchar_t dllPath[MAX_PATH] = {};
                if (SUCCEEDED(StringCchCopyW(dllPath, ARRAYSIZE(dllPath), exePath)) &&
                    SUCCEEDED(StringCchCatW(dllPath, ARRAYSIZE(dllPath), L"MiniEQ_APO.dll")) &&
                    GetFileAttributesW(dllPath) != INVALID_FILE_ATTRIBUTES) {
                    MiniEQ_RegisterComClass(dllPath);
                    MiniEQ_RegisterApoDeclaration();
                    MiniEQ_EnsureLogDirForInstall();
                }
            }
        }
    }

    // 2. Sweep our CLSID from the INACTIVE slot. The EFX experiment left us
    //    registered in both SFX and EFX on some machines; the engine must
    //    never see us twice.
    {
        const wchar_t* inactiveSlot = (g_fxSlot == kFxEfxSlot) ? kFxSfxSlot : kFxEfxSlot;
        DetachFromSlot(endpointId, inactiveSlot);
    }

    // 3. Clear any stale child-APO stash so we start clean; AttachToSlot
    //    re-stashes the real incumbent below. Skipped when the slot already
    //    held us (step 0): the stash may be a legitimate displaced APO --
    //    and on a forced re-attach AttachToSlot won't re-stash (the incumbent
    //    is us), so a blind clear would lose it with no way to restore.
    if (skipRewrite) {
        return S_FALSE;
    }
    if (!(force && alreadyAttached)) {
        MiniEQ_ClearChildApoClsid(endpointId);
    }

    // 4. Attach to the active slot (stashes the incumbent as our child).
    const HRESULT hr = AttachToSlot(endpointId, g_fxSlot);
    if (FAILED(hr)) return hr;

    // 5. Force the OS to re-enumerate the endpoint so the audio engine
    //    re-reads the FxProperties effect list. Raw registry writes don't
    //    send change notifications, so without this the engine keeps using
    //    the list from when the device was last connected ("attached but
    //    never loaded"). Best-effort: the attach itself already succeeded.
    //    Only reached when the slot actually changed, or when forced
    //    (see step 0).
    MiniEQ_ReenumerateEndpointDevice(endpointId);

    return S_OK;
}

static HRESULT DetachFromSlot(const wchar_t* endpointId, const wchar_t* slot) {
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;
    wchar_t key[512] = {};
    HRESULT hr = FxPropertiesKey(endpointId, key, ARRAYSIZE(key));
    if (FAILED(hr)) return hr;

    wchar_t clsid[64] = {};
    hr = ClsidString(clsid, ARRAYSIZE(clsid));
    if (FAILED(hr)) return hr;

    HKEY h = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0,
                            KEY_QUERY_VALUE | KEY_SET_VALUE, &h);
    if (rc == ERROR_FILE_NOT_FOUND) {
        MiniEQ_ClearChildApoClsid(endpointId); // tidy any orphaned stash
        return S_OK; // FxProperties never created: nothing to detach.
    }
    if (rc != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(rc);
    }
    // Surgical detach: only remove the slot value when it is ours -- never
    // clobber an APO someone else installed after us.
    wchar_t current[64] = {};
    DWORD size = sizeof(current), type = 0;
    rc = RegQueryValueExW(h, slot, nullptr, &type, (BYTE*)current,
                          &size);
    const bool ours = (rc == ERROR_SUCCESS && type == REG_SZ &&
                       _wcsicmp(current, clsid) == 0);
    if (ours) {
        rc = RegDeleteValueW(h, slot);
        // Deleting a value that isn't there is fine.
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
        if (rc == ERROR_SUCCESS) {
            // R2: restore the displaced APO we stashed at attach time.
            wchar_t stashed[64] = {};
            if (MiniEQ_ReadChildApoClsid(endpointId, stashed,
                                        ARRAYSIZE(stashed)) == S_OK &&
                stashed[0] != L'\0') {
                const DWORD wsize =
                    (DWORD)((wcslen(stashed) + 1) * sizeof(wchar_t));
                const LONG wrc = RegSetValueExW(h, slot, 0, REG_SZ,
                                               (const BYTE*)stashed, wsize);
                if (wrc == ERROR_SUCCESS) {
                    MiniEQ_ClearChildApoClsid(endpointId);
                }
                // If the restore write failed, keep the stash for a retry.
            } else {
                MiniEQ_ClearChildApoClsid(endpointId); // no stash; tidy
            }
        }
    } else {
        rc = ERROR_SUCCESS; // not ours: leave the slot alone
    }
    RegCloseKey(h);
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
}

HRESULT MiniEQ_DetachFromEndpoint(const wchar_t* endpointId) {
    HRESULT hr = DetachFromSlot(endpointId, g_fxSlot);
    // EFX test builds: also sweep the SFX slot, in case we are still there
    // from an earlier install.
    if (g_fxSlot == kFxEfxSlot) {
        const HRESULT hrSfx = DetachFromSlot(endpointId, kFxSfxSlot);
        if (SUCCEEDED(hr)) hr = hrSfx;
    }
    return hr;
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
    rc = RegQueryValueExW(h, g_fxSlot, nullptr, &type, (BYTE*)value, &size);
    RegCloseKey(h);
    if (rc == ERROR_SUCCESS && type == REG_SZ) {
        *attached = (_wcsicmp(value, clsid) == 0);
    }
    return S_OK;
}

//------------------------------------------------------------------------------
// Device re-enumeration: makes the audio engine re-read FxProperties.
//------------------------------------------------------------------------------

// Friendly name of an MMDevice endpoint (e.g. "Headphones (Airdopes 411ANC)").
static bool EndpointFriendlyName(const wchar_t* endpointId, wchar_t* out, size_t cch) {
    if (endpointId == nullptr || out == nullptr || cch == 0) return false;
    out[0] = L'\0';
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&pEnum))) || pEnum == nullptr) {
        return false;
    }
    bool ok = false;
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDevice(endpointId, &pDev)) && pDev != nullptr) {
        IPropertyStore* pProps = nullptr;
        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps != nullptr) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(pProps->GetValue(kPkeyDeviceFriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR && pv.pwszVal != nullptr) {
                StringCchCopyW(out, cch, pv.pwszVal);
                ok = (out[0] != L'\0');
            }
            PropVariantClear(&pv);
            pProps->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
    return ok;
}

// Restarts one device node (disable + enable) via the Setup API. Returns true
// if the device was found and the restart was issued.
static bool RestartDevnode(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDevInfo) {
    SP_PROPCHANGE_PARAMS params = {};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.Scope = DICS_FLAG_GLOBAL;

    params.StateChange = DICS_DISABLE;
    if (!SetupDiSetClassInstallParamsW(hDevInfo, pDevInfo,
                                       &params.ClassInstallHeader,
                                       sizeof(params)) ||
        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hDevInfo, pDevInfo)) {
        return false;
    }
    Sleep(1200); // let the stack settle before re-enabling
    params.StateChange = DICS_ENABLE;
    if (!SetupDiSetClassInstallParamsW(hDevInfo, pDevInfo,
                                       &params.ClassInstallHeader,
                                       sizeof(params)) ||
        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hDevInfo, pDevInfo)) {
        return false;
    }
    return true;
}

HRESULT MiniEQ_ReenumerateEndpointDevice(const wchar_t* endpointId) {
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;

    // Match the endpoint to its device node by friendly name. The MMDevice
    // friendly name ("Headphones (Airdopes 411ANC)") matches the devnode's
    // SPDRP_FRIENDLYNAME on typical audio devices.
    wchar_t wantName[256] = {};
    if (!EndpointFriendlyName(endpointId, wantName, ARRAYSIZE(wantName))) {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_MEDIA, nullptr, nullptr,
                                            DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    SP_DEVINFO_DATA devInfo = {};
    devInfo.cbSize = sizeof(devInfo);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfo); ++i) {
        wchar_t devName[256] = {};
        DWORD reqSize = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfo, SPDRP_FRIENDLYNAME,
                                              nullptr, (BYTE*)devName,
                                              sizeof(devName), &reqSize)) {
            continue;
        }
        // Case-insensitive containment either way handles minor naming
        // differences between the MMDevice and devnode names.
        wchar_t wantLow[256] = {}, devLow[256] = {};
        for (size_t k = 0; k < ARRAYSIZE(wantLow) - 1 && wantName[k] != L'\0'; ++k)
            wantLow[k] = towlower(wantName[k]);
        for (size_t k = 0; k < ARRAYSIZE(devLow) - 1 && devName[k] != L'\0'; ++k)
            devLow[k] = towlower(devName[k]);
        if (wcsstr(wantLow, devLow) == nullptr && wcsstr(devLow, wantLow) == nullptr) {
            continue;
        }
        hr = RestartDevnode(hDevInfo, &devInfo) ? S_OK
                                                : HRESULT_FROM_WIN32(GetLastError());
        break; // first match wins
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return hr;
}

//------------------------------------------------------------------------------
// Diagnostics queries (read-only, no elevation needed).
//------------------------------------------------------------------------------

HRESULT MiniEQ_QueryApoDeclaration(bool* present) {
    if (present == nullptr) return E_POINTER;
    *present = false;
    wchar_t clsid[64] = {};
    HRESULT hr = ClsidString(clsid, ARRAYSIZE(clsid));
    if (FAILED(hr)) return hr;
    wchar_t key[160] = {};
    hr = StringCchPrintfW(key, ARRAYSIZE(key),
                          L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\%s",
                          clsid);
    if (FAILED(hr)) return hr;
    HKEY h = nullptr;
    const LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, &h);
    if (rc == ERROR_SUCCESS) {
        // The engine cross-checks these two against GetRegistrationProperties.
        wchar_t iface[64] = {};
        DWORD size = sizeof(iface), type = 0;
        const LONG irc = RegQueryValueExW(h, L"APOInterface0", nullptr, &type,
                                         (BYTE*)iface, &size);
        DWORD flags = 0;
        DWORD fsize = sizeof(flags), ftype = 0;
        const LONG frc = RegQueryValueExW(h, L"Flags", nullptr, &ftype,
                                         (BYTE*)&flags, &fsize);
        RegCloseKey(h);
        *present = (irc == ERROR_SUCCESS && type == REG_SZ &&
                    _wcsicmp(iface, kApoInterface0) == 0 &&
                    frc == ERROR_SUCCESS && ftype == REG_DWORD);
    }
    return S_OK;
}

HRESULT MiniEQ_QuerySlotValue(const wchar_t* endpointId, bool efx,
                             wchar_t* out, size_t cch) {
    if (out == nullptr || cch == 0) return E_POINTER;
    out[0] = L'\0';
    if (endpointId == nullptr || endpointId[0] == L'\0') return E_INVALIDARG;
    wchar_t key[512] = {};
    HRESULT hr = FxPropertiesKey(endpointId, key, ARRAYSIZE(key));
    if (FAILED(hr)) return hr;
    HKEY h = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, &h);
    if (rc != ERROR_SUCCESS) {
        return rc == ERROR_FILE_NOT_FOUND ? S_FALSE : HRESULT_FROM_WIN32(rc);
    }
    const wchar_t* slot = efx ? kFxEfxSlot : kFxSfxSlot;
    DWORD size = (DWORD)(cch * sizeof(wchar_t)), type = 0;
    rc = RegQueryValueExW(h, slot, nullptr, &type, (BYTE*)out, &size);
    RegCloseKey(h);
    if (rc == ERROR_FILE_NOT_FOUND) return S_FALSE; // slot value absent
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    if (type != REG_SZ) return S_FALSE;
    return S_OK;
}
