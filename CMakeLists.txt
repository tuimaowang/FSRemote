cmake_minimum_required(VERSION 3.24)

project(FSRemote LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(FSREMOTE_BUILD_UU_STREAM_WEBRTC "Build the vendored UU native WebRTC streaming tools" OFF)

if(FSREMOTE_BUILD_UU_STREAM_WEBRTC)
    add_subdirectory(third_party/uu_stream_webrtc)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network)

qt_add_executable(FSRemote WIN32
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/DeviceGrid.cpp
    src/ui/DeviceGrid.h
    src/ui/RemoteDesktopWindow.cpp
    src/ui/RemoteDesktopWindow.h
    src/stream/StreamRuntime.cpp
    src/stream/StreamRuntime.h
    src/system/AppSettings.cpp
    src/system/AppSettings.h
    src/system/PowerManager.cpp
    src/system/PowerManager.h
    src/system/PortableOpenSshManager.cpp
    src/system/PortableOpenSshManager.h
    src/system/StartupManager.cpp
    src/system/StartupManager.h
    src/system/ParsecVddInstaller.cpp
    src/system/ParsecVddInstaller.h
    src/system/DeviceCommandService.cpp
    src/system/DeviceCommandService.h
    src/system/DeviceInfoService.cpp
    src/system/DeviceInfoService.h
    src/system/DeviceStatusService.cpp
    src/system/DeviceStatusService.h
    src/system/WakeOnLanSender.cpp
    src/system/WakeOnLanSender.h
    src/system/WolDetector.cpp
    src/system/WolDetector.h
    resources/app.manifest
    resources/app.rc
    resources/resources.qrc
)

target_include_directories(FSRemote PRIVATE src include third_party/parsec_vdd)
target_link_libraries(FSRemote PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
if(WIN32)
    target_link_libraries(FSRemote PRIVATE wbemuuid ole32 oleaut32 iphlpapi ws2_32 advapi32 shell32 setupapi cfgmgr32)
endif()

if(MSVC)
    target_compile_options(FSRemote PRIVATE /utf-8)
endif()

set_target_properties(FSRemote PROPERTIES
    WIN32_EXECUTABLE TRUE
)

get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}")
if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET FSRemote POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations "$<TARGET_FILE:FSRemote>"
        VERBATIM
    )
endif()

add_custom_command(TARGET FSRemote POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FSRemote>/parsec_vdd"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/parsec_vdd/parsec-vdd-0.45.0.0.exe"
        "$<TARGET_FILE_DIR:FSRemote>/parsec_vdd/parsec-vdd-0.45.0.0.exe"
    VERBATIM
)
