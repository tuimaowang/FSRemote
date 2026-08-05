# =====wjy====
# wjy: 网络安装器只封装安装逻辑，不携带程序运行文件；实际安装时从共享目录当前版本的不可变 release 复制到用户选择的位置。
if(NOT DEFINED FSREMOTE_ISS_TEMPLATE OR NOT DEFINED FSREMOTE_ISS_OUTPUT
        OR NOT DEFINED FSREMOTE_INSTALLER_OUTPUT_DIR OR NOT DEFINED FSREMOTE_SETUP_ICON)
    message(FATAL_ERROR "FSRemote network installer parameters are incomplete")
endif()

file(MAKE_DIRECTORY "${FSREMOTE_INSTALLER_OUTPUT_DIR}")
file(REMOVE
    "${FSREMOTE_INSTALLER_OUTPUT_DIR}/FSRemote安装器.exe"
    "${FSREMOTE_INSTALLER_OUTPUT_DIR}/FSRemote安装器.version"
) # wjy: 清除旧自包含安装器及其版本旁车，防止发布流程误把旧的 58MB 安装器继续复制到共享目录。
get_filename_component(FSREMOTE_INSTALLER_WORK_DIR "${FSREMOTE_ISS_OUTPUT}" DIRECTORY)
file(REMOVE_RECURSE "${FSREMOTE_INSTALLER_WORK_DIR}/payload") # wjy: 清理旧自包含载荷缓存；网络安装器不再在本地复制或压缩完整运行目录。

file(TO_NATIVE_PATH "${FSREMOTE_INSTALLER_OUTPUT_DIR}" FSREMOTE_INSTALLER_OUTPUT_DIR_NATIVE)
file(TO_NATIVE_PATH "${FSREMOTE_SETUP_ICON}" FSREMOTE_SETUP_ICON_NATIVE)
configure_file("${FSREMOTE_ISS_TEMPLATE}" "${FSREMOTE_ISS_OUTPUT}" @ONLY NEWLINE_STYLE CRLF)
message(STATUS "FSRemote network installer script configured: ${FSREMOTE_ISS_OUTPUT}")
# ===end====
