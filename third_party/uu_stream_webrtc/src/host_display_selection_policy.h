#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace uu {

// =====wjy====
struct HostDisplayCandidate {
    std::string device_name; // wjy: 保存 Windows 的 \\.\DISPLAYx 身份，后续 DXGI 与 DesktopCapturer 必须指向同一输出。
    int64_t monitor_id = 0; // wjy: 保存 HMONITOR 数值，兼容 WebRTC 屏幕源使用句柄作为 source/display id 的实现。
    uint32_t width = 0; // wjy: 当前模式宽度必须有效，避免把已断开但仍残留在枚举表中的输出当作真实主屏。
    uint32_t height = 0; // wjy: 当前模式高度与宽度共同构成可捕获候选的最低有效性条件。
    uint32_t refresh_hz = 0; // wjy: 记录真实目标刷新率，最终媒体策略不再只能从虚拟屏读取刷新率。
    bool active = false; // wjy: 只有已附着到 Windows 桌面的活动输出才允许绕过 VDD。
    bool primary = false; // wjy: 第一版只捕获主屏，以保持现有绝对鼠标坐标和远控画面一致。
    bool parsec = false; // wjy: 现存 Parsec 输出不作为“真实主屏”复用，防止接管其他远控软件的显示实例。
    bool remote = false; // wjy: Windows 远程会话输出不作为本机物理主屏，避免会话退出后捕获目标立即消失。
    bool mirroring = false; // wjy: 镜像驱动不是独立桌面输出，不能作为单屏远控目标。
};

inline bool is_eligible_existing_primary(const HostDisplayCandidate& candidate)
{
    return candidate.active // wjy: 输出必须真实挂载到当前桌面。
        && candidate.primary // wjy: 只允许主屏维持现有 SendInput 主屏坐标语义。
        && !candidate.parsec // wjy: Parsec 虚拟屏由 FSRemote 的 VDD 生命周期单独管理。
        && !candidate.remote // wjy: 排除随远程登录会话出现和消失的临时输出。
        && !candidate.mirroring // wjy: 排除旧式镜像驱动，避免 DXGI 无法 DuplicateOutput。
        && !candidate.device_name.empty() // wjy: 精确目标需要稳定的 Windows 设备名。
        && candidate.monitor_id != 0 // wjy: CPU 兼容回退必须能精确映射到同一 HMONITOR。
        && candidate.width > 0
        && candidate.height > 0; // wjy: 零尺寸模式代表输出当前不可用于桌面捕获。
}

inline std::optional<HostDisplayCandidate> select_existing_primary_display(
    const std::vector<HostDisplayCandidate>& candidates)
{
    for (const HostDisplayCandidate& candidate : candidates) { // wjy: 保留 Windows 枚举顺序，但只接受满足完整约束的主屏。
        if (is_eligible_existing_primary(candidate)) {
            return candidate; // wjy: Windows 同时只应存在一个主屏，首个合格候选就是本轮共享媒体目标。
        }
    }
    return std::nullopt; // wjy: 没有合格真实主屏时由上层进入 Parsec VDD 兜底，而不是误抓任意副屏。
}
// ===end====

} // namespace uu
