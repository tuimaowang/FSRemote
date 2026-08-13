include(FetchContent)

if(NOT FSREMOTE_FFMPEG_ROOT OR NOT EXISTS "${FSREMOTE_FFMPEG_ROOT}/include/libavcodec/avcodec.h") # 根项目必须先解压并校验仓库内的固定 FFmpeg 归档。
    message(FATAL_ERROR "FSREMOTE_FFMPEG_ROOT is missing or incomplete: ${FSREMOTE_FFMPEG_ROOT}") # 阻止回退到开发机绝对路径或不稳定网络下载。
endif()

FetchContent_Declare(
    ffmpeg # 将已验证的固定 FFmpeg 目录注册为现有目标继续使用的依赖名称。
    SOURCE_DIR "${FSREMOTE_FFMPEG_ROOT}" # 直接消费构建目录内解压结果，不修改源码目录。
)

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
