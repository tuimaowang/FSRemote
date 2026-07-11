cmake_minimum_required(VERSION 3.24)

project(FSRemote LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(FSREMOTE_BUILD_UU_STREAM_WEBRTC "Build the vendored UU native WebRTC streaming tools" ON)

if(FSREMOTE_BUILD_UU_STREAM_WEBRTC)
    add_subdirectory(third_party/uu_stream_webrtc)
    # =====wjy====
    if(MSVC)
        target_compile_options(uu_stream_common PRIVATE /utf-8) # wjy: 原生公共库已有中文 WJY 注释，统一 UTF-8 可避免系统代码页把多字节尾部误判为续行反斜杠。
        target_compile_options(fsremote_stream PRIVATE /utf-8) # wjy: DLL API 和实现新增中文说明后也必须按 UTF-8 编译，否则可能破坏 extern "C" 边界。
    endif()
    add_library(uu_session_protocol STATIC
        third_party/uu_stream_webrtc/src/session_protocol.cpp
        third_party/uu_stream_webrtc/src/session_protocol.h
    ) # wjy: 协议库独立编译，避免继承 uu_stream_common 面向 WebRTC/libc++ 的特殊宏而污染 MSVC 标准库。
    target_include_directories(uu_session_protocol PUBLIC third_party/uu_stream_webrtc/src)
    target_link_libraries(fsremote_stream PRIVATE uu_session_protocol) # wjy: DLL 后续握手状态机直接复用同一协议实现，测试与运行时不会出现两套解析规则。
    if(MSVC)
        target_compile_options(uu_session_protocol PRIVATE /utf-8)
        set_property(TARGET uu_session_protocol PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()
    option(FSREMOTE_BUILD_SESSION_PROTOCOL_TESTS "Build bounded session protocol parser tests" ON)
    if(FSREMOTE_BUILD_SESSION_PROTOCOL_TESTS)
        enable_testing() # wjy: 协议解析器无需启动 WebRTC、显卡或网络即可独立运行边界测试。
        add_executable(uu_session_protocol_tests
            third_party/uu_stream_webrtc/tests/session_protocol_tests.cpp
        )
        target_link_libraries(uu_session_protocol_tests PRIVATE uu_session_protocol) # wjy: 测试链接生产协议库，确保边界用例覆盖真实运行代码。
        if(MSVC)
            target_compile_options(uu_session_protocol_tests PRIVATE /utf-8) # wjy: 测试包含中文 UTF-8 往返样例，MSVC 必须按 UTF-8 读取源文件。
            set_property(TARGET uu_session_protocol_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        endif()
        add_test(NAME uu_session_protocol_tests COMMAND uu_session_protocol_tests) # wjy: CTest 通过退出码判断全部恶意输入边界是否通过。

        add_executable(uu_host_session_manager_tests
            third_party/uu_stream_webrtc/tests/host_session_manager_tests.cpp
        ) # wjy: 用真实 DLL 和本机 TCP 验证持久 listener、容量拒绝、worker 回收与阻塞握手关闭。
        target_include_directories(uu_host_session_manager_tests PRIVATE include third_party/uu_stream_webrtc/src)
        target_link_libraries(uu_host_session_manager_tests PRIVATE fsremote_stream uu_session_protocol ws2_32)
        set_target_properties(uu_host_session_manager_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/third_party/uu_stream_webrtc"
        ) # wjy: 测试必须与刚构建的 DLL 和 FFmpeg 运行库同目录，禁止误加载构建根目录中的旧 DLL。
        if(MSVC)
            target_compile_options(uu_host_session_manager_tests PRIVATE /utf-8)
            set_property(TARGET uu_host_session_manager_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        endif()
        add_test(NAME uu_host_session_manager_tests COMMAND uu_host_session_manager_tests)
    endif()
    # ===end====
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network)

qt_add_executable(FSRemote WIN32
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/DeviceGrid.cpp
    src/ui/DeviceGrid.h
    src/ui/RemoteDesktopWindow.cpp
    src/ui/RemoteDesktopWindow.h
    src/ui/D3D11FramePresenter.cpp
    src/ui/D3D11FramePresenter.h
    src/stream/StreamRuntime.cpp
    src/stream/StreamRuntime.h
    src/system/AppSettings.cpp
    src/system/AppSettings.h
    src/system/PowerManager.cpp
    src/system/PowerManager.h
    src/system/PortableOpenSshManager.cpp
    src/system/PortableOpenSshManager.h
    src/system/StartupManager.cpp
    src/system/StartupManager.h
    src/system/ParsecVddInstaller.cpp
    src/system/ParsecVddInstaller.h
    src/system/DeviceCommandService.cpp
    src/system/DeviceCommandService.h
    src/system/DeviceInfoService.cpp
    src/system/DeviceInfoService.h
    src/system/DeviceStatusService.cpp
    src/system/DeviceStatusService.h
    src/system/WakeOnLanSender.cpp
    src/system/WakeOnLanSender.h
    src/system/WolDetector.cpp
    src/system/WolDetector.h
    src/system/WjyDiagnosticLog.cpp
    src/system/WjyDiagnosticLog.h
    resources/app.manifest
    resources/app.rc
    resources/resources.qrc
)

target_include_directories(FSRemote PRIVATE src include third_party/parsec_vdd)
target_link_libraries(FSRemote PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
if(WIN32)
    target_link_libraries(FSRemote PRIVATE wbemuuid ole32 oleaut32 iphlpapi ws2_32 advapi32 shell32 setupapi cfgmgr32 d3d11 dxgi)
endif()

if(MSVC)
    target_compile_options(FSRemote PRIVATE /utf-8)
endif()

if(FSREMOTE_BUILD_UU_STREAM_WEBRTC)
    add_dependencies(FSRemote fsremote_stream)
    add_custom_command(TARGET FSRemote POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:fsremote_stream>"
            "$<TARGET_FILE_DIR:FSRemote>"
        VERBATIM
    )
endif()

set_target_properties(FSRemote PROPERTIES
    WIN32_EXECUTABLE TRUE
)

get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}")
if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET FSRemote POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations "$<TARGET_FILE:FSRemote>"
        VERBATIM
    )
endif()

add_custom_command(TARGET FSRemote POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FSRemote>/parsec_vdd"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/parsec_vdd/parsec-vdd-0.45.0.0.exe"
        "$<TARGET_FILE_DIR:FSRemote>/parsec_vdd/parsec-vdd-0.45.0.0.exe"
    VERBATIM
)

# =====wjy====
set(FSREMOTE_OPENSSH_RUNTIME_DIR "" CACHE PATH "OpenSSH-Win64 runtime directory copied beside FSRemote.exe")
if(NOT FSREMOTE_OPENSSH_RUNTIME_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssh/OpenSSH-Win64/ssh.exe")
        set(FSREMOTE_OPENSSH_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssh/OpenSSH-Win64") # wjy: 优先使用正式随仓库/发布包放置的 OpenSSH 运行时目录。
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/webRTC/FSRemoteDll/openssh/OpenSSH-Win64/ssh.exe")
        set(FSREMOTE_OPENSSH_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/webRTC/FSRemoteDll/openssh/OpenSSH-Win64") # wjy: 兼容当前本机已有的临时 OpenSSH 目录，方便调试构建自动复制。
    endif()
endif()

if(FSREMOTE_OPENSSH_RUNTIME_DIR AND EXISTS "${FSREMOTE_OPENSSH_RUNTIME_DIR}/ssh.exe")
    add_custom_command(TARGET FSRemote POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FSRemote>/openssh"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FSREMOTE_OPENSSH_RUNTIME_DIR}"
            "$<TARGET_FILE_DIR:FSRemote>/openssh/OpenSSH-Win64"
        VERBATIM
    ) # wjy: 构建后把 ssh.exe/sshd.exe/ssh-keygen.exe/sftp-server.exe 一起复制到程序固定查找路径。
endif()
# ===end====

# =====wjy====
option(FSREMOTE_BUILD_SESSION_IDENTITY_TESTS "Build real OpenSSH session identity tests" ON)
if(FSREMOTE_BUILD_SESSION_IDENTITY_TESTS)
    enable_testing()
    add_executable(fsremote_session_identity_tests
        tests/session_identity_tests.cpp
        src/system/PortableOpenSshManager.cpp
        src/system/PortableOpenSshManager.h
    ) # wjy: 测试直接编译生产 OpenSSH 管理器，避免用假签名实现掩盖命令行或临时文件错误。
    target_include_directories(fsremote_session_identity_tests PRIVATE src)
    target_link_libraries(fsremote_session_identity_tests PRIVATE Qt6::Core advapi32) # wjy: 身份测试和生产代码都通过 Windows 进程账户 API 计算私钥 ACL 所有者。
    set_target_properties(fsremote_session_identity_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/identity_test"
    ) # wjy: 使用独立程序目录和 data/openssh，测试绝不读取或改写主程序的真实授权与私钥数据。
    if(MSVC)
        target_compile_options(fsremote_session_identity_tests PRIVATE /utf-8)
    endif()
    add_custom_command(TARGET fsremote_session_identity_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE_DIR:fsremote_session_identity_tests>"
        VERBATIM
    ) # wjy: 独立测试目录只复制运行所需的 Qt Core DLL，确保 CTest 无需依赖开发机 PATH。
    if(FSREMOTE_OPENSSH_RUNTIME_DIR AND EXISTS "${FSREMOTE_OPENSSH_RUNTIME_DIR}/ssh-keygen.exe")
        set(_fsremote_identity_runtime_dir "$<TARGET_FILE_DIR:fsremote_session_identity_tests>/openssh/OpenSSH-Win64")
        add_custom_command(TARGET fsremote_session_identity_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_fsremote_identity_runtime_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FSREMOTE_OPENSSH_RUNTIME_DIR}/ssh.exe" "${_fsremote_identity_runtime_dir}/ssh.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FSREMOTE_OPENSSH_RUNTIME_DIR}/sshd.exe" "${_fsremote_identity_runtime_dir}/sshd.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FSREMOTE_OPENSSH_RUNTIME_DIR}/ssh-keygen.exe" "${_fsremote_identity_runtime_dir}/ssh-keygen.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FSREMOTE_OPENSSH_RUNTIME_DIR}/sftp-server.exe" "${_fsremote_identity_runtime_dir}/sftp-server.exe"
            VERBATIM
        ) # wjy: 只复制四个工具程序；测试使用已存在的分发密钥或在 data 目录生成隔离密钥，绝不覆盖已收紧 ACL 的私钥。
    endif()
    add_test(NAME fsremote_session_identity_tests COMMAND fsremote_session_identity_tests)
endif()
# ===end====
