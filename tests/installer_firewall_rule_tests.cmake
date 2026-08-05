# =====wjy====
if(NOT DEFINED FSREMOTE_SOURCE_DIR OR FSREMOTE_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "FSREMOTE_SOURCE_DIR is required") # wjy: 测试必须显式收到源码根目录，禁止依赖调用者当前工作目录。
endif()

set(installer_path "${FSREMOTE_SOURCE_DIR}/installer/FSRemote.iss.in") # wjy: 直接验证生产网络安装器模板，测试与最终打包读取同一份规则定义。
if(NOT EXISTS "${installer_path}")
    message(FATAL_ERROR "Installer template not found: ${installer_path}")
endif()

file(READ "${installer_path}" installer_text) # wjy: 只读模板文本，不执行 netsh，也不会修改开发机防火墙。

function(require_installer_text expected description)
    string(FIND "${installer_text}" "${expected}" match_index) # wjy: 每条安全关键参数都做精确子串匹配，端口、方向或地址范围变化会直接令测试失败。
    if(match_index EQUAL -1)
        message(FATAL_ERROR "Missing installer firewall contract: ${description}")
    endif()
endfunction()

require_installer_text([[advfirewall firewall add rule name=""FSRemote Realtime UDP 49104""]]
    "UDP 49104 inbound allow rule")
require_installer_text([[program=""{app}\FSRemote.exe"" protocol=UDP localport=49104]]
    "program-scoped UDP port")
require_installer_text([[remoteip=10.0.0.0/8,172.16.0.0/12,192.168.0.0/16 profile=any enable=yes]]
    "RFC1918-only remote scope")
require_installer_text([[advfirewall firewall delete rule name=""FSRemote Realtime UDP 49104""]]
    "rule replacement and uninstall cleanup")
require_installer_text("[UninstallRun]"
    "uninstall cleanup section")
# ===end====
