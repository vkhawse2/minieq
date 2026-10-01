# MiniEQ_APO -- the system-effect APO (DSP lives in the audio engine)

add_library(MiniEQ_APO SHARED
    dllmain.cpp
    eq_apo.cpp
    eq_apo.h
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

# ole32/uuid: COM. advapi32: registry (registration.cpp). setupapi: linked
# because registration.cpp references it (MiniEQ_RestartEndpointDevice is
# only called from the EXE's elevated helper, never from the DLL).
target_link_libraries(MiniEQ_APO PRIVATE ole32 uuid advapi32 setupapi)

# Static CRT: audiodg.exe loads this DLL with no guarantee that the VC++
# redistributable is reachable from its context. A dynamic-CRT dependency
# (VCRUNTIME140.dll) is a load failure waiting to happen.
set_property(TARGET MiniEQ_APO PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set_target_properties(MiniEQ_APO PROPERTIES OUTPUT_NAME "MiniEQ_APO")
