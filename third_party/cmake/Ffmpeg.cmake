include(FetchContent)

set(_ffmpeg_local_dir "C:/Users/kagurayl/projects/fsAgent/build/_deps/ffmpeg-src")

if(EXISTS "${_ffmpeg_local_dir}/include/libavcodec/avcodec.h")
    FetchContent_Declare(
        ffmpeg
        SOURCE_DIR "${_ffmpeg_local_dir}"
    )
else()
    FetchContent_Declare(
        ffmpeg
        URL https://github.com/GyanD/codexffmpeg/releases/download/8.1/ffmpeg-8.1-full_build-shared.7z
        URL_HASH SHA256=e57f02cea8b22b7ff81fb0b2ec9f6d7edb6144e84e3c0026cea0fe6dfb28e03d
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()

FetchContent_MakeAvailable(ffmpeg)

add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${ffmpeg_SOURCE_DIR}/include")
target_link_directories(ffmpeg INTERFACE "${ffmpeg_SOURCE_DIR}/lib")
target_link_libraries(ffmpeg INTERFACE avcodec avutil swscale swresample)

set(FFMPEG_BIN_DIR "${ffmpeg_SOURCE_DIR}/bin")

function(apply_ffmpeg_deploy target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FFMPEG_BIN_DIR}"
            $<TARGET_FILE_DIR:${target_name}>
    )
endfunction()
