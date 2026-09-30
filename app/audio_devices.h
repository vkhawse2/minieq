// audio_devices.h -- list active render endpoints via MMDevice API.

#pragma once

#include <string>
#include <vector>

struct AudioEndpoint {
    std::wstring id;    // endpoint ID, used for attach + settings channel
    std::wstring name;  // friendly name shown in the picker
};

std::vector<AudioEndpoint> MiniEQ_ListRenderEndpoints();

// Endpoint ID of the system default render device (eConsole role), or empty
// if there is none. Used to pre-select the device the user is actually
// listening on when the app opens.
std::wstring MiniEQ_GetDefaultRenderEndpointId();
