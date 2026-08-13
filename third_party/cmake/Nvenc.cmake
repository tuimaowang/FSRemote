include(FetchContent)

set(FSREMOTE_NVENC_SDK_ROOT "${CMAKE_SOURCE_DIR}/third_party/nvenc_sdk" CACHE PATH "NVENC SDK root containing Interface/nvEncodeAPI.h") # 默认使用仓库内固定版本头文件，避免依赖失效下载地址。
if(NOT EXISTS "${FSREMOTE_NVENC_SDK_ROOT}/Interface/nvEncodeAPI.h") # 配置阶段立即验证 NVENC SDK 是否完整。
    message(FATAL_ERROR "NVENC SDK header is missing: ${FSREMOTE_NVENC_SDK_ROOT}/Interface/nvEncodeAPI.h") # 返回明确缺失路径，避免 FetchContent 在后续阶段产生模糊错误。
endif()

FetchContent_Declare(
    nvenc_sdk # 将仓库内固定版本 NVENC SDK 注册为 FetchContent 依赖名称。
    SOURCE_DIR "${FSREMOTE_NVENC_SDK_ROOT}" # 直接使用已随项目上传的头文件目录，不访问外部下载站点。
)

FetchContent_MakeAvailable(nvenc_sdk)

add_library(nvenc INTERFACE)
target_include_directories(nvenc INTERFACE "${nvenc_sdk_SOURCE_DIR}/Interface")
