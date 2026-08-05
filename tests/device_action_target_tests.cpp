#include "system/DeviceActionTargetResolver.h"
#include "system/DeviceActionPolicy.h"
#include "system/DeviceCatalog.h"
#include "system/DeviceStatusRefreshResult.h"

#include <QCoreApplication>
#include <QJsonArray>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAILED: " << message << '\n';
    return condition;
}

bool stableTargetResolution()
{
    platform::DeviceCatalog catalog;
    platform::DeviceRecord first;
    first.id = QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    first.ip = QStringLiteral("192.168.1.31");
    first.name = QStringLiteral("First");
    platform::DeviceRecord second;
    second.id = QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    second.ip = QStringLiteral("192.168.1.32");
    second.name = QStringLiteral("Second");
    if (!catalog.addDevice(first) || !catalog.addDevice(second)) return false;

    const QVector<QString> ids = platform::DeviceActionTargetResolver::normalizeDeviceIds(
        catalog,
        {first.id, first.id, QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc")},
        second.id); // wjy: 模拟多选重复、过期 ID 和单设备回退，确认目标集合只保留有效稳定身份。
    if (!expect(ids == QVector<QString>{first.id}, "resolver must deduplicate and reject stale IDs")) return false;

    QJsonObject reordered = catalog.snapshot(false);
    QJsonArray devices = reordered.value(QStringLiteral("devices")).toArray();
    reordered.insert(QStringLiteral("devices"), QJsonArray{devices.at(1), devices.at(0)});
    catalog.applySnapshot(reordered, true); // wjy: 模拟同步重排后重新解析同一稳定 ID。
    const QVector<int> indexes = platform::DeviceActionTargetResolver::indexesForDeviceIds(catalog, ids);
    return expect(indexes.size() == 1 && indexes.first() == 1,
                  "resolver must resolve the original device after reorder");
}

bool actionEligibilityPreservesExistingSemantics()
{
    using platform::DeviceActionKind;
    using platform::DevicePresenceState;
    return expect(platform::isDeviceActionAllowed(DeviceActionKind::Wake, DevicePresenceState::Offline),
                  "wake must be allowed for offline devices")
        && expect(!platform::isDeviceActionAllowed(DeviceActionKind::Wake, DevicePresenceState::Online),
                  "wake must be rejected for online devices")
        && expect(!platform::isDeviceActionAllowed(DeviceActionKind::Shutdown, DevicePresenceState::Offline),
                  "shutdown must be rejected for explicit offline state")
        && expect(platform::isDeviceActionAllowed(DeviceActionKind::Shutdown, DevicePresenceState::Unknown),
                  "shutdown must preserve unknown-state probing behavior")
        && expect(platform::isDeviceActionAllowed(DeviceActionKind::Restart, DevicePresenceState::Busy),
                  "restart must remain available for busy devices")
        && expect(platform::isDeviceActionAllowed(DeviceActionKind::Terminal, DevicePresenceState::Busy),
                  "terminal must remain available for busy devices")
        && expect(platform::isDeviceActionAllowed(DeviceActionKind::Update, DevicePresenceState::Offline),
                  "update must still probe the target despite stale offline cache");
}

// =====wjy====
bool statusResultsUseTheRealtimeReducerWhenAvailable()
{
    using platform::DeviceStatusResultSink;
    return expect(platform::deviceStatusResultSink(true) == DeviceStatusResultSink::RealtimeReducer,
                  "status results must use the realtime reducer when the service is running")
        && expect(platform::deviceStatusResultSink(false) == DeviceStatusResultSink::DirectCache,
                  "status results must use the legacy cache only when realtime is unavailable"); // wjy: 锁定批量扫描不再绕过实时 TTL，同时保留实时服务启动失败时的兼容回退。
}
// ===end====

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    return (stableTargetResolution()
        && actionEligibilityPreservesExistingSemantics()
        && statusResultsUseTheRealtimeReducerWhenAvailable()) ? 0 : 1; // wjy: 设备动作回归同时验证状态查询结果的统一归并入口。
}
