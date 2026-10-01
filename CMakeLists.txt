# MiniEQ -- root build

cmake_minimum_required(VERSION 3.20)
project(MiniEQ LANGUAGES C CXX)

# Windows-only: both the APO and the UI target the Windows SDK via MSVC.
if(NOT WIN32)
    message(FATAL_ERROR "MiniEQ builds on Windows only (MSVC + Windows SDK).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Build id stamped into both binaries: the APO publishes it in its status
# channel and the UI compares it against its own, so the app can detect a
# stale loaded DLL and reload the audio path itself (no service restarts).
# CI passes the short commit SHA; local builds get "dev".
if(NOT DEFINED MINIEQ_BUILD_ID)
    set(MINIEQ_BUILD_ID "dev")
endif()

# Route 1 experiment: attach the APO in the EFX slot instead of SFX. EFX runs
# after all mixing at the endpoint, downstream of the spatial-sound render, so
# an EQ there should survive spatial sound / Dolby Atmos. OFF by default (SFX,
# current behavior); the CI test build enables it via -DMINIEQ_EFX_SLOT=ON.
option(MINIEQ_EFX_SLOT "Attach the APO in the EFX slot (spatial-sound experiment)" OFF)
if(MINIEQ_EFX_SLOT)
    add_compile_definitions(MINIEQ_EFX_SLOT)
endif()

add_subdirectory(apo)
add_subdirectory(app)
