cmake_minimum_required(VERSION 3.24)

project(FakerInputBridge VERSION 1.0.0 LANGUAGES CXX)

add_executable(FakerInputBridge
    src/main.cpp
    src/bridge_client.cpp
    src/bridge_client.h
    src/bridge_protocol.h
    src/bridge_server.cpp
    src/bridge_server.h
    src/faker_input_device.cpp
    src/faker_input_device.h
)

target_compile_features(FakerInputBridge PRIVATE cxx_std_20)
target_compile_definitions(FakerInputBridge PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(FakerInputBridge PRIVATE /W4 /permissive- /utf-8 /EHsc)
    set_property(TARGET FakerInputBridge PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(FakerInputBridge PRIVATE
    advapi32
    hid
    setupapi
    user32
)

# FakerInputBridge 使用 CMake 默认的构建目录输出，避免在克隆后的源码树中生成 dist 文件。
