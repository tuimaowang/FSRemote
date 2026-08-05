#pragma once

#include "system/DeviceStatusService.h"

#include <QHash>
#include <QString>

namespace platform {

// =====wjy====
enum class DeviceStatusResultSink {
    RealtimeReducer,
    DirectCache,
}; // wjy: 明确网络查询结果必须进入实时归并器或旧版缓存之一，避免调用点自行直接伪造在线状态。

inline DeviceStatusResultSink deviceStatusResultSink(bool realtimeAvailable)
{
    return realtimeAvailable
        ? DeviceStatusResultSink::RealtimeReducer // wjy: 实时服务可用时统一应用 TTL、新旧快照优先级和离线过期规则。
        : DeviceStatusResultSink::DirectCache; // wjy: 实时服务启动失败时才回退原有 UI 状态缓存，保持旧环境可用。
}

struct DeviceStatusRefreshResult {
    QHash<QString, DeviceStatusInfo> devices;
    QHash<QString, bool> updateAvailability;

    void reserve(int count)
    {
        devices.reserve(count); // wjy: 后台刷新前一次性预留设备容量，减少多线程结果归并时的重复扩容。
        updateAvailability.reserve(count); // wjy: 更新可用性与设备状态使用同一批 IP，保持两个结果表的生命周期一致。
    }
};
// ===end====

} // namespace platform
