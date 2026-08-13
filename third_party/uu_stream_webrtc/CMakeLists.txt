cmake_minimum_required(VERSION 3.24)

project(UuStreamWebrtc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(UU_STREAM_WEBRTC_BUILD_HOST "Build the standalone WebRTC host test executable" OFF)
option(UU_STREAM_WEBRTC_BUILD_VIEWER "Build the standalone ImGui WebRTC viewer test executable" OFF)

include("../cmake/Nvenc.cmake")
include("../cmake/Ffmpeg.cmake")
set(UU_STREAM_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "FFmpeg runtime DLL directory for uu_stream_webrtc")
if(UU_STREAM_WEBRTC_BUILD_VIEWER)
    include("../cmake/imgui.cmake")
endif()
set(WEBRTC_ROOT "" CACHE PATH "Path to a native WebRTC build/install root") # wjy: bundled SDK root is injected by the top-level project.
include("cmake/FindWebRTCNative.cmake")

add_library(uu_stream_common STATIC
    # wjy: Register headers beside sources so Qt Creator shows them in the CMake project tree.
    ../lan_stream_probe/src/nvenc_h264_encoder.cpp
    ../lan_stream_probe/src/nvenc_h264_encoder.h
    ../lan_stream_probe/src/ffmpeg_decoder.cpp
    ../lan_stream_probe/src/ffmpeg_decoder.h
    ../lan_stream_probe/src/common.h
    src/native_webrtc_runtime.cpp
    src/native_webrtc_runtime.h
    src/parsec_vdd_session.cpp
    src/parsec_vdd_session.h
    src/sdp_guard.cpp
    src/sdp_guard.h
    src/signaling.cpp
    src/signaling.h
    src/system_audio_stream.cpp
    src/system_audio_stream.h
    src/uu_codec_factory.cpp
    src/uu_codec_factory.h
    src/uu_profile.cpp
    src/uu_profile.h
    src/webrtc_session.cpp
    src/webrtc_session.h
)

#aiÌí¼Ó
set_property(TARGET uu_stream_common PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")


target_include_directories(uu_stream_common PUBLIC
    src
    ../lan_stream_probe/src
    ../parsec_vdd
    ${WEBRTC_INCLUDE_DIRS}
)

target_compile_definitions(uu_stream_common PUBLIC
    _HAS_NODISCARD
    _CRT_NONSTDC_NO_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS
    _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE
    _LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS
    _LIBCPP_INSTRUMENTED_WITH_ASAN=0
    __STD_C
    _CRT_RAND_S
    _CRT_SECURE_NO_DEPRECATE
    _SCL_SECURE_NO_DEPRECATE
    _ATL_NO_OPENGL
    _WINDOWS
    CERT_CHAIN_PARA_HAS_EXTRA_FIELDS
    PSAPI_VERSION=2
    WIN32
    _SECURE_ATL
    WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _UNICODE
    UNICODE
    NTDDI_VERSION=NTDDI_WIN11_GE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    WINCRYPT_USE_SYMBOL_PREFIX
    NDEBUG
    NVALGRIND
    DYNAMIC_ANNOTATIONS_ENABLED=0
    WEBRTC_WIN
    WEBRTC_ENABLE_PROTOBUF=1
    WEBRTC_STRICT_FIELD_TRIALS=0
    WEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE
    RTC_ENABLE_VP9
    RTC_ENABLE_H265
    WEBRTC_HAVE_SCTP
    WEBRTC_USE_H264
    WEBRTC_ENCODER_PSNR_STATS
    PROTOBUF_ENABLE_DEBUG_LOGGING_MAY_LEAK_PII=0
    WEBRTC_ENABLE_AVX2
    RTC_ENABLE_WIN_WGC
    WEBRTC_DEPRECATE_PLAN_B
    WEBRTC_NON_STATIC_TRACE_EVENT_HANDLERS=0
    ABSL_ALLOCATOR_NOTHROW=1
    CHROMIUM
    LIBYUV_DISABLE_NEON
    LIBYUV_DISABLE_SVE
    LIBYUV_DISABLE_SME
    LIBYUV_DISABLE_LSX
    LIBYUV_DISABLE_LASX
    GOOGLE_PROTOBUF_NO_RTTI
    PROTOBUF_INTERNAL_BOUNDS_CHECK_MODE_ABORT
)

target_link_libraries(uu_stream_common PUBLIC
    nvenc
    ffmpeg
    ${WEBRTC_LIBRARIES}
    ${WEBRTC_LIBCXX_OBJECTS}
    d3d11
    dxgi
    ws2_32
    iphlpapi
    shcore
    dwmapi
    dmoguids
    msdmo
    wmcodecdspuuid
    #vcruntime
    winmm
    setupapi
    cfgmgr32
    user32
    ole32
)

set(_uu_stream_ffmpeg_runtime_dlls
        avcodec-62.dll
        avutil-60.dll
        swresample-6.dll
        swscale-9.dll
)

function(copy_uu_stream_ffmpeg_dlls target_name)
    foreach(dll_name IN LISTS _uu_stream_ffmpeg_runtime_dlls)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFMPEG_BIN_DIR}/${dll_name}"
                "$<TARGET_FILE_DIR:${target_name}>"
        )
    endforeach()

endfunction()

add_library(fsremote_stream SHARED
    src/fsremote_stream_api.cpp
    ../../include/FsRemoteStreamApi.h # wjy: Register the DLL API header so Qt Creator shows it under this target.
)

#aiÌí¼Ó
set_property(TARGET fsremote_stream PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")


target_include_directories(fsremote_stream PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../../include"
)
target_link_libraries(fsremote_stream PRIVATE uu_stream_common)
set_target_properties(fsremote_stream PROPERTIES
    OUTPUT_NAME fsremote_stream
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE
)
copy_uu_stream_ffmpeg_dlls(fsremote_stream)

if(UU_STREAM_WEBRTC_BUILD_HOST)
    add_executable(uu_webrtc_host
        src/host_main.cpp
    )
    target_link_libraries(uu_webrtc_host PRIVATE uu_stream_common winmm)
    copy_uu_stream_ffmpeg_dlls(uu_webrtc_host)
endif()

if(UU_STREAM_WEBRTC_BUILD_VIEWER)
    add_executable(uu_webrtc_viewer
        src/viewer_main.cpp
    )
    target_link_libraries(uu_webrtc_viewer PRIVATE uu_stream_common imgui d3d11 dxgi dwmapi)
    copy_uu_stream_ffmpeg_dlls(uu_webrtc_viewer)
endif()



