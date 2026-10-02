# MiniEQ_APO -- the system-effect APO (DSP lives in the audio engine)

add_library(MiniEQ_APO SHARED
    dllmain.cpp
    eq_apo.cpp
    eq_apo.h
    trace.cpp
    trace.h
    dsp.cpp
    dsp.h
    guids.cpp
    guids.h
    registration.cpp
    registration.h
    ../shared/settings_channel.c
    ../shared/settings_channel.h
    eqapo.def
)

target_include_directories(MiniEQ_APO PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/shared
)

# ole32/uuid: COM. advapi32: registry (registration.cpp). setupapi: device
# re-enumeration (registration.cpp's MiniEQ_ReenumerateEndpointDevice).
target_link_libraries(MiniEQ_APO PRIVATE ole32 uuid advapi32 setupapi)

# Diagnostic trace of the APO load path (see trace.h). Compiled out when 0.
# 0.2.3: production ships with tracing OFF -- the traced build performs a
# file open/write on nearly every COM call, including inside the audio
# engine's graph-construction window. Diagnosis uses the status-channel
# handshake (initCalls/processCalls) instead. Flip to 1 only for a
# throwaway diagnosis build.
target_compile_definitions(MiniEQ_APO PRIVATE MINIEQ_APO_TRACE=0)

# Build id for the status-channel stamp (see root CMakeLists).
target_compile_definitions(MiniEQ_APO PRIVATE "MINIEQ_BUILD_ID=\"${MINIEQ_BUILD_ID}\"")

# Static CRT: audiodg.exe loads this DLL with no guarantee that the VC++
# redistributable is reachable from its context. A dynamic-CRT dependency
# (VCRUNTIME140.dll) is a load failure waiting to happen.
set_property(TARGET MiniEQ_APO PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set_target_properties(MiniEQ_APO PROPERTIES OUTPUT_NAME "MiniEQ_APO")
