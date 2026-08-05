#include "system/DeviceCatalog.h"
#include "system/DeviceCatalogRepository.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QDir>
#include <QTemporaryDir>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAILED: " << message << '\n';
    return condition;
}

QJsonObject catalogSnapshot(bool expanded)
{
    return QJsonObject{
        {QStringLiteral("revision"), 41},
        {QStringLiteral("updatedAt"), QStringLiteral("2026-07-20T01:00:00.000Z")},
        {QStringLiteral("updatedBy"), QStringLiteral("catalog-test")},
        {QStringLiteral("groups"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("11111111-1111-4111-8111-111111111111")},
            {QStringLiteral("name"), QStringLiteral("测试组")},
            {QStringLiteral("expanded"), expanded},
        }}},
        {QStringLiteral("devices"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("22222222-2222-4222-8222-222222222222")},
            {QStringLiteral("name"), QStringLiteral("设备A")},
            {QStringLiteral("ip"), QStringLiteral("192.168.1.20")},
            {QStringLiteral("groupId"), QStringLiteral("11111111-1111-4111-8111-111111111111")},
        }}},
        {QStringLiteral("tombstones"), QJsonObject{
            {QStringLiteral("devices"), QJsonArray{QJsonObject{
                {QStringLiteral("id"), QStringLiteral("33333333-3333-4333-8333-333333333333")},
                {QStringLiteral("deletedAt"), QStringLiteral("2026-07-20T00:59:00.000Z")},
            }}},
            {QStringLiteral("groups"), QJsonArray{}},
        }},
    };
}

// =====wjy====
bool snapshotAndExpansionBehavior()
{
    platform::DeviceCatalog catalog;
    catalog.applySnapshot(catalogSnapshot(false), false); // wjy: 首次加载采用本机文件中的折叠状态，建立目录迁移前的基线。
    if (!expect(catalog.groups().size() == 1 && !catalog.groups().first().expanded, "initial local expansion must load")) return false;

    QJsonObject remote = catalogSnapshot(true);
    remote.remove(QStringLiteral("updatedAt"));
    catalog.applySnapshot(remote, true); // wjy: 远端共享快照到达时保留当前本机折叠状态，不能被远端界面偏好覆盖。
    const QJsonObject localSnapshot = catalog.snapshot(true);
    const QJsonObject sharedSnapshot = catalog.snapshot(false);
    return expect(!catalog.groups().first().expanded, "remote snapshot must preserve current local expansion")
        && expect(localSnapshot.value(QStringLiteral("groups")).toArray().first().toObject().contains(QStringLiteral("expanded")), "local snapshot must contain expansion")
        && expect(!sharedSnapshot.value(QStringLiteral("groups")).toArray().first().toObject().contains(QStringLiteral("expanded")), "shared snapshot must strip expansion")
        && expect(catalog.revision() == 41, "catalog must preserve revision")
        && expect(catalog.tombstones().value(QStringLiteral("devices")).toArray().size() == 1, "catalog must preserve tombstones");
}

bool stableLookupSurvivesReorder()
{
    platform::DeviceCatalog catalog;
    platform::DeviceRecord first;
    first.id = QStringLiteral("44444444-4444-4444-8444-444444444444");
    first.name = QStringLiteral("First");
    first.ip = QStringLiteral("192.168.1.21");
    platform::DeviceRecord second;
    second.id = QStringLiteral("55555555-5555-4555-8555-555555555555");
    second.name = QStringLiteral("Second");
    second.ip = QStringLiteral("192.168.1.22");
    if (!catalog.addDevice(first) || !catalog.addDevice(second)) return false;
    const QString stableId = catalog.devices().first().id;

    QJsonObject reordered = catalog.snapshot(false);
    QJsonArray devices = reordered.value(QStringLiteral("devices")).toArray();
    QJsonArray reversed{devices.at(1), devices.at(0)};
    reordered.insert(QStringLiteral("devices"), reversed);
    catalog.applySnapshot(reordered, true); // wjy: 模拟同步排序变化，旧数组下标已指向另一台设备。
    const int index = catalog.deviceIndexForId(stableId);
    return expect(index == 1, "stable ID must resolve the original device after reorder")
        && expect(catalog.devices().at(index).ip == QStringLiteral("192.168.1.21"), "stable lookup must not target the new occupant of the old index");
}

bool mutationBehavior()
{
    platform::DeviceCatalog catalog;
    const int groupIndex = catalog.addGroup(QStringLiteral("原组"), QStringLiteral("66666666-6666-4666-8666-666666666666"), true);
    if (!expect(groupIndex == 0, "group must be added")) return false;
    platform::DeviceRecord device;
    device.id = QStringLiteral("77777777-7777-4777-8777-777777777777");
    device.name = QStringLiteral("OldName");
    device.ip = QStringLiteral("192.168.1.23");
    if (!catalog.addDevice(device)) return false;
    const QString deviceId = catalog.devices().first().id;
    const QString groupId = catalog.groups().first().id;

    const bool assigned = catalog.assignDevicesToGroup({deviceId}, groupId); // wjy: 设备归组使用稳定 ID，不暴露目录内部下标给业务调用者。
    const bool renamedDevice = catalog.renameDevice(deviceId, QStringLiteral("NewName"));
    const bool renamedGroup = catalog.renameGroup(groupId, QStringLiteral("新组"));
    if (!expect(assigned && renamedDevice && renamedGroup, "catalog mutations must succeed")) return false;
    if (!expect(catalog.devices().first().group == QStringLiteral("新组"), "group compatibility name must follow rename")) return false;
    if (!catalog.removeGroup(groupId)) return false;
    return expect(catalog.devices().size() == 1, "removing group must retain device")
        && expect(catalog.devices().first().groupId.isEmpty(), "removing group must move device to root")
        && expect(catalog.removeDevice(deviceId) && catalog.devices().isEmpty(), "device removal must use stable ID");
}

bool repositoryRoundTrip()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "temporary repository directory must be valid")) return false;

    platform::DeviceCatalog source;
    source.applySnapshot(catalogSnapshot(false), false);
    platform::DeviceCatalogRepository writer(
        source,
        QDir(directory.path()).filePath(QStringLiteral("devices.json")));
    if (!expect(writer.saveLocal(), "repository must atomically save local snapshot")) return false;

    platform::DeviceCatalog loaded;
    platform::DeviceCatalogRepository reader(
        loaded,
        QDir(directory.path()).filePath(QStringLiteral("devices.json")));
    if (!expect(reader.loadLocal(), "repository must load normalized snapshot")) return false;
    return expect(loaded.deviceIndexForIp(QStringLiteral("192.168.1.20")) == 0,
                  "repository round trip must preserve device identity")
        && expect(!loaded.groups().isEmpty() && !loaded.groups().first().expanded,
                  "repository round trip must preserve local group expansion");
}

bool repositorySynchronizationBoundary()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "sync repository directory must be valid")) return false;

    platform::DeviceCatalog catalog;
    catalog.applySnapshot(catalogSnapshot(false), false);
    platform::DeviceCatalogRepository repository(
        catalog,
        QDir(directory.path()).filePath(QStringLiteral("devices.json")));
    int startCount = 0;
    int stopCount = 0;
    int submitCount = 0;
    QJsonObject startedSnapshot;
    repository.startSynchronization(
        directory.path(),
        [&](const QString&, const QJsonObject& snapshot) {
            ++startCount;
            startedSnapshot = snapshot; // wjy: 捕获启动载荷，验证仓储不会把本机 expanded 偏好交给共享服务。
        },
        [&] { ++stopCount; },
        [&](const QJsonObject&) { ++submitCount; });

    const QString deviceId = catalog.devices().first().id;
    catalog.renameDevice(deviceId, QStringLiteral("LocalRename"));
    if (!expect(repository.saveLocal(), "local save must succeed while sync is active")) return false;

    QJsonObject remote = catalogSnapshot(true);
    QJsonArray remoteDevices = remote.value(QStringLiteral("devices")).toArray();
    QJsonObject remoteDevice = remoteDevices.first().toObject();
    remoteDevice.insert(QStringLiteral("name"), QStringLiteral("RemoteRename"));
    remoteDevices.replace(0, remoteDevice);
    remote.insert(QStringLiteral("devices"), remoteDevices);
    if (!expect(repository.applySynchronizedSnapshot(remote), "remote snapshot must apply and persist")) return false;
    repository.stopSynchronization();

    return expect(startCount == 1, "sync service must start exactly once")
        && expect(stopCount == 1, "sync service must stop exactly once")
        && expect(submitCount == 1, "remote persistence must not recursively submit a second snapshot")
        && expect(!startedSnapshot.value(QStringLiteral("groups")).toArray().first().toObject().contains(QStringLiteral("expanded")),
                  "shared start snapshot must exclude local expansion")
        && expect(catalog.devices().first().name == QStringLiteral("RemoteRename"),
                  "remote snapshot must update catalog state")
        && expect(!catalog.groups().first().expanded,
                  "remote snapshot must preserve current local expansion");
}
// ===end====

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // =====wjy====
    const bool ok = snapshotAndExpansionBehavior() // wjy: 验证本机展开态、共享快照和同步元数据边界。
        && stableLookupSurvivesReorder() // wjy: 验证异步/同步排序变化后仍能按稳定 ID 命中原设备。
        && mutationBehavior(); // wjy: 验证重命名、归组和删除都集中经过目录权威状态。
    // ===end====
    const bool repositoryOk = repositoryRoundTrip()
        && repositorySynchronizationBoundary(); // wjy: 同时验证原子读写和远端应用不回环提交的同步边界。
    if (ok && repositoryOk) std::cout << "device catalog tests passed\n";
    return (ok && repositoryOk) ? 0 : 1;
}
