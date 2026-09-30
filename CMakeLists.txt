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

add_subdirectory(apo)
add_subdirectory(app)
