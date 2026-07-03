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
        "${_webrtc_source_root}"
        "${_webrtc_source_root}/third_party/abseil-cpp"
        "${_webrtc_source_root}/third_party/libyuv/include"
        "${_webrtc_source_root}/third_party/jsoncpp/source/include"
        "${_webrtc_source_root}/third_party/boringssl/src/include"
    )
    if(NOT MSVC)
        list(APPEND WEBRTC_INCLUDE_DIRS
            "${_webrtc_source_root}/buildtools/third_party/libc++"
            "${_webrtc_source_root}/third_party/libc++/src/include"
        )
    endif()
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

# =====wjy====
# wjy: Prefer a single aggregate WebRTC library. Mixing obj/webrtc.lib with raw GN
# wjy: target objects can duplicate internal WebRTC state and crash on RTP receive.
set(_webrtc_monolithic_candidates
    "${WEBRTC_ROOT}/lib/webrtc.lib"
    "${WEBRTC_ROOT}/lib/libwebrtc.lib"
    "${WEBRTC_ROOT}/src/out/uu_release/obj/webrtc.lib"
    "${WEBRTC_ROOT}/out/uu_release/obj/webrtc.lib"
    "${WEBRTC_ROOT}/out/Release/obj/webrtc.lib"
    "${WEBRTC_ROOT}/out/Default/obj/webrtc.lib"
)

set(WEBRTC_LIBRARIES "")
foreach(_candidate IN LISTS _webrtc_monolithic_candidates)
    if(EXISTS "${_candidate}")
        list(APPEND WEBRTC_LIBRARIES "${_candidate}")
        break()
    endif()
endforeach()

if(WEBRTC_LIBRARIES)
    set(_webrtc_required_supplements
        "${WEBRTC_ROOT}/src/out/uu_release/obj/api/field_trials/field_trials.obj"
        "${WEBRTC_ROOT}/src/out/uu_release/obj/api/enable_media_with_defaults/enable_media_with_defaults.obj"
        "${WEBRTC_ROOT}/src/out/uu_release/obj/api/video_codecs/builtin_video_encoder_factory/builtin_video_encoder_factory.obj"
        "${WEBRTC_ROOT}/src/out/uu_release/obj/api/video_codecs/builtin_video_decoder_factory/builtin_video_decoder_factory.obj"
        "${WEBRTC_ROOT}/src/out/uu_release/obj/media/rtc_simulcast_encoder_adapter/simulcast_encoder_adapter.obj"
        "${WEBRTC_ROOT}/src/out/uu_release/obj/media/rtc_internal_video_codecs/internal_encoder_factory.obj"
        "${WEBRTC_ROOT}/src/out/uu_release/obj/media/rtc_internal_video_codecs/internal_decoder_factory.obj"
        "${WEBRTC_ROOT}/src/out/uu_release/obj/api/video_codecs/rtc_software_fallback_wrappers/video_encoder_software_fallback_wrapper.obj"
    )
    foreach(_candidate IN LISTS _webrtc_required_supplements)
        if(EXISTS "${_candidate}")
            list(APPEND WEBRTC_LIBRARIES "${_candidate}") # wjy: add only missing symbols required by our modular factory path.
        endif()
    endforeach()
endif()

if(NOT WEBRTC_LIBRARIES)
    set(_webrtc_component_candidates
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
    foreach(_candidate IN LISTS _webrtc_component_candidates)
        if(EXISTS "${_candidate}")
            list(APPEND WEBRTC_LIBRARIES "${_candidate}")
        endif()
    endforeach()
endif()

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
# ===end====

if(NOT WEBRTC_LIBRARIES)
    message(FATAL_ERROR
        "No WebRTC .lib files found under ${WEBRTC_ROOT}. Provide a native WebRTC build.")
endif()

message(STATUS "Using WebRTC include dir: ${WEBRTC_INCLUDE_DIRS}")
message(STATUS "Using WebRTC libraries: ${WEBRTC_LIBRARIES}")
if(WEBRTC_LIBCXX_OBJECTS)
    message(STATUS "Using WebRTC libc++ objects: ${WEBRTC_LIBCXX_OBJECTS}")
endif()
