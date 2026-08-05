#include "system/DeviceActionTargetResolver.h"

#include "system/DeviceCatalog.h"

#include <QSet>

namespace platform {

// =====wjy====
QVector<QString> DeviceActionTargetResolver::normalizeDeviceIds(
    const DeviceCatalog& catalog,
    const QVector<QString>& requestedIds,
    const QString& fallbackDeviceId)
{
    QVector<QString> result;
    QSet<QString> seen;
    for (const QString& requestedId : requestedIds) {
        const int index = catalog.deviceIndexForId(requestedId);
        if (index < 0) {
            continue; // wjy: 菜单打开后设备可能已被删除或远端快照替换，失效 ID 必须被过滤。
        }
        const QString stableId = catalog.devices().at(index).id;
        if (!stableId.isEmpty() && !seen.contains(stableId)) {
            seen.insert(stableId);
            result.append(stableId); // wjy: 目标集合按请求顺序去重，后续批量操作不会重复执行同一设备。
        }
    }

    if (result.isEmpty() && catalog.deviceIndexForId(fallbackDeviceId) >= 0) {
        result.append(catalog.devices().at(catalog.deviceIndexForId(fallbackDeviceId)).id); // wjy: 多选状态失效时回退到触发菜单的稳定设备。
    }
    return result;
}

QVector<int> DeviceActionTargetResolver::indexesForDeviceIds(
    const DeviceCatalog& catalog,
    const QVector<QString>& deviceIds)
{
    QVector<int> result;
    QSet<int> seen;
    for (const QString& deviceId : deviceIds) {
        const int index = catalog.deviceIndexForId(deviceId);
        if (index >= 0 && !seen.contains(index)) {
            seen.insert(index);
            result.append(index); // wjy: 仅在即将调用旧 UI 操作入口时恢复瞬时下标，稳定 ID 仍是目标真实身份。
        }
    }
    return result;
}

QVector<QString> DeviceActionTargetResolver::deviceIdsForGroup(
    const DeviceCatalog& catalog,
    const QString& groupId)
{
    QVector<QString> result;
    const int groupIndex = catalog.groupIndexForId(groupId);
    if (groupIndex < 0) {
        return result;
    }
    for (const DeviceRecord& device : catalog.devices()) {
        if (device.groupId == catalog.groupIds().at(groupIndex)) {
            result.append(device.id); // wjy: 分组批量目标按稳定 groupId 收集，不依赖分组名称或当前视觉顺序。
        }
    }
    return result;
}
// ===end====

} // namespace platform
