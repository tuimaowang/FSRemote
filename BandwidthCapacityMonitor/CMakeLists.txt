cmake_minimum_required(VERSION 3.24)

project(BandwidthCapacityMonitor LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "BandwidthCapacityMonitor currently supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =====wjy====
add_executable(BandwidthCapacityMonitor
    src/main.cpp
    src/MonitorMath.h
) # wjy: 独立控制台程序只读取 Windows 网卡与进程计数，不链接 FSRemote 或 Qt，也不会生成测试流量。
target_link_libraries(BandwidthCapacityMonitor PRIVATE iphlpapi psapi) # wjy: IP Helper 提供物理网卡计数，Psapi 提供 FSRemote 工作集与私有内存统计。

add_executable(BandwidthCapacityMonitorTests
    tests/monitor_math_tests.cpp
    src/MonitorMath.h
) # wjy: 纯数学测试锁定 Mbps、余量和风险等级计算，不读取或修改真实网卡状态。
target_include_directories(BandwidthCapacityMonitorTests PRIVATE src)

enable_testing()
add_test(NAME BandwidthCapacityMonitorTests COMMAND BandwidthCapacityMonitorTests) # wjy: CTest 通过退出码验证监控计算边界。

if(MSVC)
    target_compile_options(BandwidthCapacityMonitor PRIVATE /utf-8 /W4 /permissive-) # wjy: 中文诊断文字按 UTF-8 编译，并启用较严格的 Windows 编译检查。
    target_compile_options(BandwidthCapacityMonitorTests PRIVATE /utf-8 /W4 /permissive-)
    set_property(TARGET BandwidthCapacityMonitor PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # wjy: 静态链接 MSVC 运行库，生成的监控器可直接复制到控制电脑运行。
    set_property(TARGET BandwidthCapacityMonitorTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
# ===end====
