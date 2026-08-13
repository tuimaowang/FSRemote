cmake_minimum_required(VERSION 3.24)

project(FakerInputDriverTest LANGUAGES CXX)

# =====wjy====
add_executable(FakerInputDriverTest
    test/main.cpp # wjy: 独立测试入口默认只查询驱动，真实键鼠操作必须由命令行参数显式触发。
    src/faker_input_device.cpp # wjy: 测试程序直接复用交付包中的 HID 设备访问实现，验证同事实际会集成的源码。
    src/faker_input_device.h
    src/faker_input_keyboard_state.h
)

target_compile_features(FakerInputDriverTest PRIVATE cxx_std_20) # wjy: std::span 和现有设备接口要求 C++20。
target_compile_definitions(FakerInputDriverTest PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
) # wjy: 即使其它头文件先包含 Windows.h，也保持与交付源码一致的轻量无宏污染环境。
target_include_directories(FakerInputDriverTest PRIVATE src) # wjy: 测试代码按同事复制 src 目录后的常规方式包含公开头文件。
target_link_libraries(FakerInputDriverTest PRIVATE hid setupapi user32) # wjy: 分别提供 HID 报告、设备枚举和安全鼠标位置查询所需 Win32 库。

if(MSVC)
    target_compile_options(FakerInputDriverTest PRIVATE /W4 /permissive- /utf-8 /EHsc) # wjy: 用严格 MSVC 警告检查交付源码在独立工程中的可移植性。
endif()
# ===end====
