cmake_minimum_required(VERSION 3.20)
project(BatteryInTaskBar)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/utf-8)
endif()

enable_language(RC)

add_library(taskbar_mod SHARED
        taskbar_mod.cpp
        src/system/WindowScanner.cpp
        src/ui/Renderer.cpp
        src/ui/WidgetWindow.cpp
        src/ui/AppsEdgeProbe.cpp
        src/ui/ContextMenu.cpp
        src/ui/SlotOverlay.cpp
        src/ui/HeadsetTile.cpp
        src/airpods/AirPodsMonitor.cpp
        src/core/Application.cpp
        src/core/AppPaths.cpp
        src/core/Localization.cpp
        src/core/Diagnostics.cpp
        src/core/Report.cpp
        src/core/TelemetryCache.cpp
        src/hid/MouseProviders.cpp
        src/hid/BekenStatusProtocol.cpp
        src/hid/RedSquareProtocol.cpp
        src/core/DeviceManager.cpp
        tools/HidDump.cpp
        res/version_dll.rc
)
target_include_directories(taskbar_mod PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(taskbar_mod PRIVATE UNICODE _UNICODE)
target_link_libraries(taskbar_mod PRIVATE
        user32
        gdi32
        shell32
        d2d1
        dwrite
        setupapi
        hid
        advapi32
        dwmapi
        ole32
        uiautomationcore
        windowsapp
)

add_executable(injector WIN32
        injector.cpp
        src/core/AppPaths.cpp
        src/core/Localization.cpp
        src/core/Diagnostics.cpp
        src/hid/MouseProviders.cpp
        src/hid/BekenStatusProtocol.cpp
        src/hid/RedSquareProtocol.cpp
        tools/HidDump.cpp
        res/version_exe.rc
)
target_include_directories(injector PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(injector PRIVATE UNICODE _UNICODE)
target_link_libraries(injector PRIVATE
        user32
        advapi32
        shell32
        setupapi
        hid
)

add_executable(updater WIN32
        updater.cpp
        src/core/AppPaths.cpp
        src/core/Localization.cpp
        res/version_updater.rc
)
target_include_directories(updater PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(updater PRIVATE UNICODE _UNICODE)
target_link_libraries(updater PRIVATE
        user32
        gdi32
        advapi32
        shell32
        comctl32
        bcrypt
        winhttp
)

# Sign built binaries manually: cmake --build ... --target sign
# (close the widget first: Explorer keeps the DLL loaded).
add_custom_target(sign
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/sign.ps1"
                -Dll "$<TARGET_FILE:taskbar_mod>"
                -Exe "$<TARGET_FILE:injector>"
                -Updater "$<TARGET_FILE:updater>"
                -CreateSelfSigned
        DEPENDS taskbar_mod injector updater
        COMMENT "Sign taskbar_mod.dll, injector.exe and updater.exe"
        VERBATIM)

# Протокольные тесты без реального устройства: синтетические отчёты ATK/VXE
# проверяют разбор заряда и отсев отчётов движения, кадры Beken/Inphic —
# заряд X6/X11 и отсев ACK/DPI-отчётов.
enable_testing()
add_executable(provider_protocol_test
        tests/RedSquareProtocolTest.cpp
        src/hid/RedSquareProtocol.cpp)
target_include_directories(provider_protocol_test PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(provider_protocol_test PRIVATE UNICODE _UNICODE)
add_test(NAME provider_protocol COMMAND provider_protocol_test)

add_executable(beken_status_protocol_test
        tests/BekenStatusProtocolTest.cpp
        src/hid/BekenStatusProtocol.cpp)
target_include_directories(beken_status_protocol_test PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(beken_status_protocol_test PRIVATE UNICODE _UNICODE)
add_test(NAME beken_status_protocol COMMAND beken_status_protocol_test)
