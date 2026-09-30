# MiniEQ -- the tiny native Win32 UI app

add_executable(MiniEQ WIN32
    main.cpp
    audio_devices.cpp
    audio_devices.h
    settings_link.cpp
    settings_link.h
    diag.cpp
    diag.h
    # Reused as-is: endpoint attach/detach registry writes.
    ../apo/registration.cpp
    ../apo/registration.h
    ../apo/guids.cpp
    ../apo/guids.h
    ../shared/settings_channel.c
    ../shared/settings_channel.h
)

target_include_directories(MiniEQ PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apo
    ${CMAKE_SOURCE_DIR}/shared
)

# ole32/uuid: COM + MMDevice. advapi32: registry. comctl32: trackbars.
# shell32: SHGetFolderPathW + elevated relaunch.
target_link_libraries(MiniEQ PRIVATE ole32 uuid advapi32 comctl32 shell32)
