#include "system/DeviceListSyncModel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <iostream>

namespace {

QJsonObject group(const QString& id, const QString& name)
{
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
    };
}

QJsonObject device(const QString& id, const QString& name, const QString& ip, const QString& groupId = {})
{
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("ip"), ip},
        {QStringLiteral("mac"), QString()},
        {QStringLiteral("broadcast_ip"), QString()},
        {QStringLiteral("remark"), QString()},
        {QStringLiteral("groupId"), groupId},
    };
}

QJsonObject snapshot(const QJsonArray& devices, const QJsonArray& groups = {}, qint64 revision = 1)
{
    return platform::normalizeDeviceListSnapshot(QJsonObject{
        {QStringLiteral("revision"), revision},
        {QStringLiteral("devices"), devices},
        {QStringLiteral("groups"), groups},
    });
}

QJsonObject objectById(const QJsonArray& array, const QString& id)
{
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("id")).toString() == id) {
            return object;
        }
    }
    return {};
}

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

bool differentEntitiesMerge()
{
    const QString a = QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QString b = QStringLiteral("22222222-2222-4222-8222-222222222222");
    const QJsonObject base = snapshot({device(a, QStringLiteral("A"), QStringLiteral("192.168.1.10")), device(b, QStringLiteral("B"), QStringLiteral("192.168.1.11"))});
    QJsonObject local = base;
    QJsonArray localDevices = local.value(QStringLiteral("devices")).toArray();
    QJsonObject localB = localDevices.at(1).toObject();
    localB.insert(QStringLiteral("name"), QStringLiteral("B-local"));
    localDevices.replace(1, localB);
    local.insert(QStringLiteral("devices"), localDevices);

    QJsonObject remote = base;
    QJsonArray remoteDevices = remote.value(QStringLiteral("devices")).toArray();
    QJsonObject remoteA = remoteDevices.at(0).toObject();
    remoteA.insert(QStringLiteral("name"), QStringLiteral("A-remote"));
    remoteDevices.replace(0, remoteA);
    remote.insert(QStringLiteral("devices"), remoteDevices);

    const QJsonObject merged = platform::mergeDeviceListSnapshots(base, local, remote, QStringLiteral("client-b"));
    const QJsonArray devices = merged.value(QStringLiteral("devices")).toArray();
    return expect(objectById(devices, a).value(QStringLiteral("name")).toString() == QStringLiteral("A-remote"), "remote change on A must survive")
        && expect(objectById(devices, b).value(QStringLiteral("name")).toString() == QStringLiteral("B-local"), "local change on B must merge");
}

bool laterSameEntityWins()
{
    const QString id = QStringLiteral("33333333-3333-4333-8333-333333333333");
    const QJsonObject base = snapshot({device(id, QStringLiteral("base"), QStringLiteral("192.168.1.12"))});
    QJsonObject local = snapshot({device(id, QStringLiteral("later"), QStringLiteral("192.168.1.12"))});
    QJsonObject remote = snapshot({device(id, QStringLiteral("earlier"), QStringLiteral("192.168.1.12"))}, {}, 2);
    const QJsonObject merged = platform::mergeDeviceListSnapshots(base, local, remote, QStringLiteral("later-client"));
    return expect(objectById(merged.value(QStringLiteral("devices")).toArray(), id).value(QStringLiteral("name")).toString() == QStringLiteral("later"), "later lock holder must win same-entity conflict");
}

bool deletedEntityDoesNotReturn()
{
    const QString removed = QStringLiteral("44444444-4444-4444-8444-444444444444");
    const QString changed = QStringLiteral("55555555-5555-4555-8555-555555555555");
    const QJsonObject base = snapshot({device(removed, QStringLiteral("removed"), QStringLiteral("192.168.1.13")), device(changed, QStringLiteral("old"), QStringLiteral("192.168.1.14"))});
    QJsonObject local = snapshot({device(removed, QStringLiteral("removed"), QStringLiteral("192.168.1.13")), device(changed, QStringLiteral("local"), QStringLiteral("192.168.1.14"))});
    QJsonObject remote = snapshot({device(changed, QStringLiteral("old"), QStringLiteral("192.168.1.14"))}, {}, 2);
    QJsonObject tombstones;
    tombstones.insert(QStringLiteral("devices"), QJsonArray{QJsonObject{{QStringLiteral("id"), removed}, {QStringLiteral("deletedAt"), QStringLiteral("2026-07-13T00:00:00Z")}}});
    remote.insert(QStringLiteral("tombstones"), tombstones);

    const QJsonObject merged = platform::mergeDeviceListSnapshots(base, local, remote, QStringLiteral("offline-client"));
    return expect(objectById(merged.value(QStringLiteral("devices")).toArray(), removed).isEmpty(), "unchanged offline record must not resurrect a tombstoned device")
        && expect(objectById(merged.value(QStringLiteral("devices")).toArray(), changed).value(QStringLiteral("name")).toString() == QStringLiteral("local"), "unrelated local change must still commit");
}

bool groupIdentitySurvivesRenameAndDelete()
{
    const QString groupId = QStringLiteral("66666666-6666-4666-8666-666666666666");
    const QString deviceId = QStringLiteral("77777777-7777-4777-8777-777777777777");
    const QJsonObject base = snapshot({device(deviceId, QStringLiteral("PC"), QStringLiteral("192.168.1.15"), groupId)}, {group(groupId, QStringLiteral("旧组"))});
    const QJsonObject renamedLocal = snapshot({device(deviceId, QStringLiteral("PC"), QStringLiteral("192.168.1.15"), groupId)}, {group(groupId, QStringLiteral("新组"))});
    const QJsonObject renamed = platform::mergeDeviceListSnapshots(base, renamedLocal, base, QStringLiteral("client"));
    const QJsonObject renamedDevice = objectById(renamed.value(QStringLiteral("devices")).toArray(), deviceId);
    if (!expect(renamedDevice.value(QStringLiteral("groupId")).toString() == groupId, "group rename must keep device groupId")) return false;

    const QJsonObject deletedLocal = snapshot({device(deviceId, QStringLiteral("PC"), QStringLiteral("192.168.1.15"), groupId)}, {});
    const QJsonObject deleted = platform::mergeDeviceListSnapshots(base, deletedLocal, base, QStringLiteral("client"));
    const QJsonObject releasedDevice = objectById(deleted.value(QStringLiteral("devices")).toArray(), deviceId);
    return expect(releasedDevice.value(QStringLiteral("groupId")).toString().isEmpty(), "deleting group must move device to root");
}

bool legacyMigrationIsDeterministic()
{
    const QJsonObject legacy{
        {QStringLiteral("groups"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("设计组")}}}},
        {QStringLiteral("devices"), QJsonArray{QJsonObject{
            {QStringLiteral("name"), QStringLiteral("PC")},
            {QStringLiteral("ip"), QStringLiteral("192.168.1.16")},
            {QStringLiteral("mac"), QStringLiteral("AA-BB-CC-DD-EE-FF")},
            {QStringLiteral("group"), QStringLiteral("设计组")},
        }}},
    };
    const QJsonObject first = platform::normalizeDeviceListSnapshot(legacy);
    const QJsonObject second = platform::normalizeDeviceListSnapshot(legacy);
    const QJsonObject firstDevice = first.value(QStringLiteral("devices")).toArray().first().toObject();
    const QJsonObject secondDevice = second.value(QStringLiteral("devices")).toArray().first().toObject();
    return expect(firstDevice.value(QStringLiteral("id")) == secondDevice.value(QStringLiteral("id")), "legacy device ID must be deterministic")
        && expect(!firstDevice.value(QStringLiteral("groupId")).toString().isEmpty(), "legacy group name must migrate to groupId")
        && expect(firstDevice.value(QStringLiteral("groupId")) == secondDevice.value(QStringLiteral("groupId")), "legacy group ID must be deterministic");
}

// =====wjy====
bool normalizedSnapshotRoundTripIsStable()
{
    const QString groupId = QStringLiteral("88888888-8888-4888-8888-888888888888");
    const QString deviceId = QStringLiteral("99999999-9999-4999-8999-999999999999");
    QJsonObject source = snapshot(
        {device(deviceId, QStringLiteral("RoundTrip"), QStringLiteral("192.168.1.17"), groupId)},
        {group(groupId, QStringLiteral("往返组"))},
        17);
    source.insert(QStringLiteral("updatedAt"), QStringLiteral("2026-07-20T00:00:00.000Z")); // wjy: 固定提交时间用于确认规范化往返不会丢失同步元数据。
    source.insert(QStringLiteral("updatedBy"), QStringLiteral("baseline-client")); // wjy: 固定客户端标识用于确认第二次规范化仍保持原提交来源。

    const QJsonObject first = platform::normalizeDeviceListSnapshot(source); // wjy: 第一次规范化模拟从本地或共享文件读取后的权威快照。
    const QJsonObject second = platform::normalizeDeviceListSnapshot(first); // wjy: 第二次规范化模拟保存、重启或同步回环后的再次读取。
    return expect(first == second, "normalized snapshot must be byte-structure stable across a second normalization")
        && expect(platform::deviceListSnapshotRevision(second) == 17, "round trip must preserve revision")
        && expect(second.value(QStringLiteral("updatedAt")).toString() == QStringLiteral("2026-07-20T00:00:00.000Z"), "round trip must preserve updatedAt")
        && expect(second.value(QStringLiteral("updatedBy")).toString() == QStringLiteral("baseline-client"), "round trip must preserve updatedBy");
}

bool tombstonesAndLocalExpansionAreCharacterized()
{
    const QString groupId = QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    QJsonObject localGroup = group(groupId, QStringLiteral("本机折叠组"));
    localGroup.insert(QStringLiteral("expanded"), false); // wjy: expanded 是本机界面状态，规范化共享载荷必须剥离，后续由目录层按稳定 groupId 单独保留。
    QJsonObject tombstones;
    tombstones.insert(QStringLiteral("groups"), QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")},
        {QStringLiteral("deletedAt"), QStringLiteral("2026-07-20T00:01:00.000Z")},
        {QStringLiteral("deletedBy"), QStringLiteral("baseline-client")},
    }});
    tombstones.insert(QStringLiteral("devices"), QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc")},
        {QStringLiteral("deletedAt"), QStringLiteral("2026-07-20T00:02:00.000Z")},
        {QStringLiteral("deletedBy"), QStringLiteral("baseline-client")},
    }});

    QJsonObject source{
        {QStringLiteral("revision"), QStringLiteral("23")}, // wjy: 使用字符串 revision 覆盖旧文件/跨版本读取路径，确认规范化仍得到数值 23。
        {QStringLiteral("groups"), QJsonArray{localGroup}},
        {QStringLiteral("devices"), QJsonArray{}},
        {QStringLiteral("tombstones"), tombstones},
    };
    const QJsonObject normalized = platform::normalizeDeviceListSnapshot(source); // wjy: 记录重构前共享模型对本机展开态和墓碑的真实处理契约。
    const QJsonObject normalizedGroup = normalized.value(QStringLiteral("groups")).toArray().first().toObject();
    const QJsonObject normalizedTombstones = normalized.value(QStringLiteral("tombstones")).toObject();
    return expect(!normalizedGroup.contains(QStringLiteral("expanded")), "shared normalization must strip local-only group expansion")
        && expect(platform::deviceListSnapshotRevision(normalized) == 23, "string revision must normalize to the same numeric revision")
        && expect(normalizedTombstones.value(QStringLiteral("groups")).toArray().size() == 1, "group tombstone must survive normalization")
        && expect(normalizedTombstones.value(QStringLiteral("devices")).toArray().size() == 1, "device tombstone must survive normalization");
}
// ===end====

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const bool ok = differentEntitiesMerge()
        && laterSameEntityWins()
        && deletedEntityDoesNotReturn()
        && groupIdentitySurvivesRenameAndDelete()
        && legacyMigrationIsDeterministic()
        // =====wjy====
        && normalizedSnapshotRoundTripIsStable() // wjy: 锁定规范化快照重复读取/保存时的数据稳定性，后续目录抽离不能改变该契约。
        && tombstonesAndLocalExpansionAreCharacterized(); // wjy: 锁定墓碑保留和本机展开态不进入共享载荷的现有行为。
        // ===end====
    if (ok) {
        std::cout << "device list sync tests passed\n";
    }
    return ok ? 0 : 1;
}
