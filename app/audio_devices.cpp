// audio_devices.cpp -- MMDevice enumeration.

#include "audio_devices.h"

#include <mmdeviceapi.h>
#include <propsys.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>

std::vector<AudioEndpoint> MiniEQ_ListRenderEndpoints() {
    std::vector<AudioEndpoint> out;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                 CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                 (void**)&enumerator);
    if (FAILED(hr)) {
        return out;
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr)) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device))) {
                continue;
            }
            AudioEndpoint ep;
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
                ep.id = id;
                CoTaskMemFree(id);
            }
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
                    name.vt == VT_LPWSTR && name.pwszVal != nullptr) {
                    ep.name = name.pwszVal;
                }
                PropVariantClear(&name);
                props->Release();
            }
            if (ep.name.empty()) {
                ep.name = L"(unknown device)";
            }
            out.push_back(std::move(ep));
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();
    return out;
}

std::wstring MiniEQ_GetDefaultRenderEndpointId() {
    std::wstring out;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                 CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                 (void**)&enumerator);
    if (FAILED(hr)) {
        return out;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr)) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
            out = id;
            CoTaskMemFree(id);
        }
        device->Release();
    }
    enumerator->Release();
    return out;
}
