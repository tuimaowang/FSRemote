set(WEBRTC_ROOT "" CACHE PATH "Path to a native WebRTC build/install root")

if(NOT WEBRTC_ROOT)
    message(FATAL_ERROR
        "WEBRTC_ROOT is required. This project intentionally requires native WebRTC; "
        "libdatachannel/raw UDP are not accepted for the UU-quality target. "
        "Expected layout: WEBRTC_ROOT/include plus WEBRTC_ROOT/lib/webrtc.lib or obj/*.lib.")
endif()

if(EXISTS "${WEBRTC_ROOT}/src/api/peer_connection_interface.h")
    set(_webrtc_source_root "${WEBRTC_ROOT}/src")
elseif(EXISTS "${WEBRTC_ROOT}/api/peer_connection_interface.h")
    set(_webrtc_source_root "${WEBRTC_ROOT}")
else()
    set(_webrtc_source_root "")
endif()

if(_webrtc_source_root)
    set(WEBRTC_INCLUDE_DIRS
        "${WEBRTC_ROOT}/src/out/uu_release/gen"
        "${_webrtc_source_root}/buildtools/third_party/libc++"
        "${_webrtc_source_root}/third_party/libc++/src/include"
        "${_webrtc_source_root}"
        "${_webrtc_source_root}/third_party/abseil-cpp"
        "${_webrtc_source_root}/third_party/libyuv/include"
        "${_webrtc_source_root}/third_party/jsoncpp/source/include"
        "${_webrtc_source_root}/third_party/boringssl/src/include"
    )
elseif(EXISTS "${WEBRTC_ROOT}/include/api/peer_connection_interface.h")
    set(WEBRTC_INCLUDE_DIRS "${WEBRTC_ROOT}/include")
else()
    message(FATAL_ERROR
        "WEBRTC_ROOT does not look like a native WebRTC install: "
        "missing api/peer_connection_interface.h under ${WEBRTC_ROOT}, ${WEBRTC_ROOT}/src, or ${WEBRTC_ROOT}/include")
endif()

set(WEBRTC_LIBCXX_OBJECTS "")
file(GLOB _webrtc_libcxx_objects
    "${WEBRTC_ROOT}/src/out/uu_release/obj/buildtools/third_party/libc++/libc++/*.obj"
    "${WEBRTC_ROOT}/out/uu_release/obj/buildtools/third_party/libc++/libc++/*.obj"
)
list(APPEND WEBRTC_LIBCXX_OBJECTS ${_webrtc_libcxx_objects})

set(_webrtc_candidates
    "${WEBRTC_ROOT}/lib/webrtc.lib"
    "${WEBRTC_ROOT}/lib/libwebrtc.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/webrtc.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/api/field_trials.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/api/environment/force_test_environment.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/api/enable_media_with_defaults.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/api/enable_media.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/api/video_codecs/builtin_video_encoder_factory.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/api/video_codecs/builtin_video_decoder_factory.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/api/video_codecs/rtc_software_fallback_wrappers.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/media/rtc_internal_video_codecs.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/media/rtc_simulcast_encoder_adapter.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/test/fake_video_codecs.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/api/libjingle_peerconnection_api.lib"
    "${WEBRTC_ROOT}/out/Default/obj/webrtc.lib"
    "${WEBRTC_ROOT}/out/Release/obj/webrtc.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/webrtc.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/api/field_trials.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/api/environment/force_test_environment.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/api/enable_media_with_defaults.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/api/enable_media.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/api/video_codecs/builtin_video_encoder_factory.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/api/video_codecs/builtin_video_decoder_factory.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/api/video_codecs/rtc_software_fallback_wrappers.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/media/rtc_internal_video_codecs.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/media/rtc_simulcast_encoder_adapter.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/test/fake_video_codecs.lib"
)

set(WEBRTC_LIBRARIES "")
foreach(_candidate IN LISTS _webrtc_candidates)
    if(EXISTS "${_candidate}")
        list(APPEND WEBRTC_LIBRARIES "${_candidate}")
    endif()
endforeach()

if(NOT WEBRTC_LIBRARIES)
    file(GLOB_RECURSE _webrtc_libs
        "${WEBRTC_ROOT}/lib/*.lib"
        "${WEBRTC_ROOT}/src/out/uu_release/obj/*.lib"
        "${WEBRTC_ROOT}/out/Default/obj/*.lib"
        "${WEBRTC_ROOT}/out/Release/obj/*.lib"
        "${WEBRTC_ROOT}/out/uu_release/obj/*.lib"
    )
    list(APPEND WEBRTC_LIBRARIES ${_webrtc_libs})
endif()

if(NOT WEBRTC_LIBRARIES)
    message(FATAL_ERROR
        "No WebRTC .lib files found under ${WEBRTC_ROOT}. Provide a native WebRTC build.")
endif()

message(STATUS "Using WebRTC include dir: ${WEBRTC_INCLUDE_DIRS}")
message(STATUS "Using WebRTC libraries: ${WEBRTC_LIBRARIES}")
if(WEBRTC_LIBCXX_OBJECTS)
    message(STATUS "Using WebRTC libc++ objects: ${WEBRTC_LIBCXX_OBJECTS}")
endif()
