#include "system/DeviceActionPolicy.h"

namespace platform {

// =====wjy====
bool isDeviceActionAllowed(DeviceActionKind action, DevicePresenceState state)
{
    switch (action) {
    case DeviceActionKind::Wake:
        return state == DevicePresenceState::Offline; // wjy: 开机只对离线设备发送 Wake-on-LAN，在线/占用设备避免重复唤醒。
    case DeviceActionKind::Shutdown:
    case DeviceActionKind::Restart:
    case DeviceActionKind::Terminal:
        return state != DevicePresenceState::Offline; // wjy: 与现有 UI 语义一致，未知状态仍允许命令入口自行探测，只有明确离线才提前拦截。
    case DeviceActionKind::Update:
        return true; // wjy: 更新请求必须直达目标端确认版本，不能用过期的离线缓存提前阻断用户明确操作。
    }
    return false;
}
// ===end====

} // namespace platform
