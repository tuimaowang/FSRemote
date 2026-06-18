include(FetchContent)

set(_nvenc_local_dir "${CMAKE_BINARY_DIR}/_deps/nvenc-src")

if(EXISTS "${_nvenc_local_dir}/Interface/nvEncodeAPI.h")
    FetchContent_Declare(
        nvenc_sdk
        SOURCE_DIR "${_nvenc_local_dir}"
    )
else()
    FetchContent_Declare(
        nvenc_sdk
        URL https://gamedbg.com/Video_Codec_Interface_13.0.37.zip
        URL_HASH SHA256=1b63633d911bc124d139cb982b2f6a6e5efc1db98d7877b14463c85d1effff5c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()

FetchContent_MakeAvailable(nvenc_sdk)

add_library(nvenc INTERFACE)
target_include_directories(nvenc INTERFACE "${nvenc_sdk_SOURCE_DIR}/Interface")
