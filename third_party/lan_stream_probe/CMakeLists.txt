cmake_minimum_required(VERSION 3.24)

project(LanStreamProbe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("../cmake/Nvenc.cmake")
include("../cmake/Ffmpeg.cmake")
include("../cmake/imgui.cmake")

add_library(lan_stream_common STATIC
    src/dxgi_capture.cpp
    src/ffmpeg_decoder.cpp
    src/nvenc_h264_encoder.cpp
    src/protocol.cpp
    src/stream_capture_diagnostics.cpp # wjy: LAN探针与正式远控共用同一套低开销采集诊断写入器。
    src/stream_capture_diagnostics.h
    src/udp_socket.cpp
)

target_include_directories(lan_stream_common PUBLIC src)
target_link_libraries(lan_stream_common PUBLIC
    nvenc
    ffmpeg
    d3d11
    dxgi
    ws2_32
)

target_compile_definitions(lan_stream_common PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(lan_stream_host
    src/host_main.cpp
)
target_link_libraries(lan_stream_host PRIVATE lan_stream_common)
target_link_libraries(lan_stream_host PRIVATE winmm)

add_executable(lan_stream_viewer
    src/viewer_main.cpp
)
target_link_libraries(lan_stream_viewer PRIVATE
    lan_stream_common
    imgui
    d3d11
    dxgi
    dwmapi
)

function(copy_ffmpeg_bins target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FFMPEG_BIN_DIR}"
            "$<TARGET_FILE_DIR:${target_name}>"
    )
endfunction()

copy_ffmpeg_bins(lan_stream_viewer)

add_library(lan_stream_core SHARED
    src/lan_stream_api.cpp
)
target_compile_definitions(lan_stream_core PRIVATE LAN_STREAM_CORE_EXPORTS)
target_link_libraries(lan_stream_core PRIVATE
    lan_stream_common
    winmm
)
