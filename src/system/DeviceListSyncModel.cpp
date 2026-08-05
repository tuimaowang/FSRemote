#include "system/DeviceListSyncModel.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonValue>
#include <QMap>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace platform {
namespace {

constexpr int kDeviceListSchemaVersion = 2;

QString normalizedUuid(const QString& value)
{
    const QUuid uuid(value.trimmed());
    return uuid.isNull() ? QString() : uuid.toString(QUuid::WithoutBraces).toLower();
}

QString deterministicId(const QByteArray& scope, const QString& key)
{
    const QByteArray normalizedKey = key.trimmed().toLower().toUtf8();
    const QByteArray digest = QCryptographicHash::hash(scope + QByteArrayLiteral("|") + normalizedKey, QCryptographicHash::Sha256);
    QByteArray uuidBytes = digest.left(16);
    uuidBytes[6] = char((quint8(uuidBytes.at(6)) & 0x0fU) | 0x50U);
    uuidBytes[8] = char((quint8(uuidBytes.at(8)) & 0x3fU) | 0x80U);
    return QUuid::fromRfc4122(uuidBytes).toString(QUuid::WithoutBraces).toLower();
}

QString groupFallbackId(const QString& name)
{
    return deterministicId(QByteArrayLiteral("fsremote-group-v1"), name);
}

QString deviceFallbackId(const QJsonObject& object, int index)
{
    QString key = object.value(QStringLiteral("mac")).toString().trimmed().toLower();
    if (key.isEmpty()) {
        key = object.value(QStringLiteral("ip")).toString().trimmed().toLower();
    }
    if (key.isEmpty()) {
        key = object.value(QStringLiteral("name")).toString().trimmed().toLower()
            + QStringLiteral("|%1").arg(index);
    }
    return deterministicId(QByteArrayLiteral("fsremote-device-v1"), key);
}

QJsonArray normalizedTombstones(const QJsonValue& value)
{
    QMap<QString, QJsonObject> byId;
    for (const QJsonValue& item : value.toArray()) {
        const QJsonObject source = item.toObject();
        const QString id = normalizedUuid(source.value(QStringLiteral("id")).toString());
        if (id.isEmpty()) {
            continue;
        }
        QJsonObject tombstone;
        tombstone.insert(QStringLiteral("id"), id);
        tombstone.insert(QStringLiteral("deletedAt"), source.value(QStringLiteral("deletedAt")).toString());
        tombstone.insert(QStringLiteral("deletedBy"), source.value(QStringLiteral("deletedBy")).toString());
        byId.insert(id, tombstone);
    }

    QJsonArray result;
    for (const QJsonObject& tombstone : byId) {
        result.append(tombstone);
    }
    return result;
}

QMap<QString, QJsonObject> objectsById(const QJsonArray& array)
{
    QMap<QString, QJsonObject> result;
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            result.insert(id, object);
        }
    }
    return result;
}

QJsonArray orderedObjects(const QMap<QString, QJsonObject>& objects)
{
    QVector<QJsonObject> values;
    values.reserve(objects.size());
    for (const QJsonObject& object : objects) {
        values.append(object);
    }
    std::sort(values.begin(), values.end(), [](const QJsonObject& left, const QJsonObject& right) {
        const int leftOrder = left.value(QStringLiteral("sortOrder")).toInt();
        const int rightOrder = right.value(QStringLiteral("sortOrder")).toInt();
        if (leftOrder != rightOrder) {
            return leftOrder < rightOrder;
        }
        return left.value(QStringLiteral("id")).toString() < right.value(QStringLiteral("id")).toString();
    });

    QJsonArray result;
    for (const QJsonObject& object : values) {
        result.append(object);
    }
    return result;
}

QJsonArray orderedTombstones(const QMap<QString, QJsonObject>& tombstones)
{
    QJsonArray result;
    for (const QJsonObject& tombstone : tombstones) {
        result.append(tombstone);
    }
    return result;
}

QJsonObject payloadOnly(const QJsonObject& snapshot)
{
    const QJsonObject normalized = normalizeDeviceListSnapshot(snapshot);
    QJsonObject payload;
    payload.insert(QStringLiteral("devices"), normalized.value(QStringLiteral("devices")));
    payload.insert(QStringLiteral("groups"), normalized.value(QStringLiteral("groups")));
    payload.insert(QStringLiteral("tombstones"), normalized.value(QStringLiteral("tombstones")));
    return payload;
}

void mergeEntityChanges(
    const QJsonArray& baseArray,
    const QJsonArray& localArray,
    QMap<QString, QJsonObject>* remoteObjects,
    QMap<QString, QJsonObject>* remoteTombstones,
    const QString& updatedBy,
    const QString& deletedAt)
{
    if (!remoteObjects || !remoteTombstones) {
        return;
    }

    const QMap<QString, QJsonObject> baseObjects = objectsById(baseArray);
    const QMap<QString, QJsonObject> localObjects = objectsById(localArray);
    QSet<QString> ids;
    for (auto it = baseObjects.cbegin(); it != baseObjects.cend(); ++it) ids.insert(it.key());
    for (auto it = localObjects.cbegin(); it != localObjects.cend(); ++it) ids.insert(it.key());

    for (const QString& id : ids) {
        const bool baseHas = baseObjects.contains(id);
        const bool localHas = localObjects.contains(id);
        if (!baseHas && localHas) {
            remoteObjects->insert(id, localObjects.value(id));
            remoteTombstones->remove(id); // wjy: 本端明确新增或重新添加实体时清掉旧墓碑，允许用户主动恢复同一设备。
            continue;
        }
        if (baseHas && !localHas) {
            remoteObjects->remove(id);
            QJsonObject tombstone;
            tombstone.insert(QStringLiteral("id"), id);
            tombstone.insert(QStringLiteral("deletedAt"), deletedAt);
            tombstone.insert(QStringLiteral("deletedBy"), updatedBy);
            remoteTombstones->insert(id, tombstone); // wjy: 删除以墓碑提交，离线设备携带旧 base 回来时不会把未修改记录复活。
            continue;
        }
        if (baseHas && localHas && baseObjects.value(id) != localObjects.value(id)) {
            remoteObjects->insert(id, localObjects.value(id));
            remoteTombstones->remove(id); // wjy: 同一实体发生本地修改时覆盖当前远端值，锁提交顺序自然形成“后提交者生效”。
        }
    }
}

} // namespace

qint64 deviceListSnapshotRevision(const QJsonObject& snapshot)
{
    const QJsonValue value = snapshot.value(QStringLiteral("revision"));
    if (value.isDouble()) {
        return qMax<qint64>(0, qRound64(value.toDouble()));
    }
    bool ok = false;
    const qint64 revision = value.toString().toLongLong(&ok);
    return ok ? qMax<qint64>(0, revision) : 0;
}

QJsonObject normalizeDeviceListSnapshot(const QJsonObject& snapshot)
{
    // =====wjy====
    QJsonArray normalizedGroups;
    QMap<QString, QString> groupIdByName;
    QSet<QString> usedGroupIds;
    int groupOrder = 0;
    for (const QJsonValue& value : snapshot.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject source = value.toObject();
        const QString name = source.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }
        QString id = normalizedUuid(source.value(QStringLiteral("id")).toString());
        if (id.isEmpty()) {
            id = groupFallbackId(name); // wjy: 旧客户端没有 group ID 时按名称生成确定性 ID，多个旧副本迁移后仍指向同一分组。
        }
        if (usedGroupIds.contains(id)) {
            continue;
        }
        usedGroupIds.insert(id);
        groupIdByName.insert(name.toCaseFolded(), id);
        QJsonObject group;
        group.insert(QStringLiteral("id"), id);
        group.insert(QStringLiteral("name"), name);
        group.insert(QStringLiteral("sortOrder"), groupOrder++); // wjy: 分组排列顺序也是同步数据，重排后其它设备保持一致。
        normalizedGroups.append(group);
    }

    QJsonArray normalizedDevices;
    QSet<QString> usedDeviceIds;
    int deviceOrder = 0;
    int sourceIndex = -1;
    for (const QJsonValue& value : snapshot.value(QStringLiteral("devices")).toArray()) {
        ++sourceIndex;
        const QJsonObject source = value.toObject();
        const QString ip = source.value(QStringLiteral("ip")).toString().trimmed();
        if (ip.isEmpty()) {
            continue;
        }
        QString id = normalizedUuid(source.value(QStringLiteral("id")).toString());
        if (id.isEmpty()) {
            id = deviceFallbackId(source, sourceIndex); // wjy: 优先按 MAC、其次按 IP 生成确定性设备 ID，兼容所有旧 devices.json。
        }
        if (usedDeviceIds.contains(id)) {
            continue;
        }
        usedDeviceIds.insert(id);

        const QString groupName = source.value(QStringLiteral("group")).toString().trimmed();
        QString groupId = normalizedUuid(source.value(QStringLiteral("groupId")).toString());
        if (!usedGroupIds.contains(groupId)) {
            groupId = groupIdByName.value(groupName.toCaseFolded()); // wjy: 旧文件只有 group 名时迁移到稳定 groupId；不存在的分组自动回到根部。
        }
        QString canonicalGroupName;
        if (!groupId.isEmpty()) {
            for (const QJsonValue& groupValue : normalizedGroups) {
                const QJsonObject group = groupValue.toObject();
                if (group.value(QStringLiteral("id")).toString() == groupId) {
                    canonicalGroupName = group.value(QStringLiteral("name")).toString();
                    break;
                }
            }
        }

        QJsonObject device;
        device.insert(QStringLiteral("id"), id);
        device.insert(QStringLiteral("name"), source.value(QStringLiteral("name")).toString().trimmed());
        device.insert(QStringLiteral("ip"), ip);
        device.insert(QStringLiteral("mac"), source.value(QStringLiteral("mac")).toString().trimmed());
        device.insert(QStringLiteral("broadcast_ip"), source.value(QStringLiteral("broadcast_ip")).toString().trimmed());
        device.insert(QStringLiteral("remark"), source.value(QStringLiteral("remark")).toString());
        device.insert(QStringLiteral("groupId"), groupId);
        device.insert(QStringLiteral("group"), canonicalGroupName); // wjy: 保留 group 名只为兼容旧代码，真正关联以不可变 groupId 为准。
        device.insert(QStringLiteral("sortOrder"), deviceOrder++);
        normalizedDevices.append(device);
    }

    const QJsonObject sourceTombstones = snapshot.value(QStringLiteral("tombstones")).toObject();
    QJsonObject tombstones;
    tombstones.insert(QStringLiteral("devices"), normalizedTombstones(sourceTombstones.value(QStringLiteral("devices"))));
    tombstones.insert(QStringLiteral("groups"), normalizedTombstones(sourceTombstones.value(QStringLiteral("groups"))));

    QJsonObject normalized;
    normalized.insert(QStringLiteral("schemaVersion"), kDeviceListSchemaVersion);
    normalized.insert(QStringLiteral("revision"), deviceListSnapshotRevision(snapshot));
    normalized.insert(QStringLiteral("updatedAt"), snapshot.value(QStringLiteral("updatedAt")).toString());
    normalized.insert(QStringLiteral("updatedBy"), snapshot.value(QStringLiteral("updatedBy")).toString());
    normalized.insert(QStringLiteral("devices"), normalizedDevices);
    normalized.insert(QStringLiteral("groups"), normalizedGroups);
    normalized.insert(QStringLiteral("tombstones"), tombstones);
    return normalized;
    // ===end====
}

bool deviceListSnapshotsEquivalent(const QJsonObject& left, const QJsonObject& right)
{
    return payloadOnly(left) == payloadOnly(right);
}

QJsonObject mergeDeviceListSnapshots(
    const QJsonObject& baseSnapshot,
    const QJsonObject& localSnapshot,
    const QJsonObject& remoteSnapshot,
    const QString& updatedBy)
{
    // =====wjy====
    const QJsonObject base = normalizeDeviceListSnapshot(baseSnapshot);
    const QJsonObject local = normalizeDeviceListSnapshot(localSnapshot);
    const QJsonObject remote = normalizeDeviceListSnapshot(remoteSnapshot);
    const QString committedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    QMap<QString, QJsonObject> remoteGroups = objectsById(remote.value(QStringLiteral("groups")).toArray());
    QMap<QString, QJsonObject> remoteDevices = objectsById(remote.value(QStringLiteral("devices")).toArray());
    const QJsonObject remoteTombstoneObject = remote.value(QStringLiteral("tombstones")).toObject();
    QMap<QString, QJsonObject> groupTombstones = objectsById(remoteTombstoneObject.value(QStringLiteral("groups")).toArray());
    QMap<QString, QJsonObject> deviceTombstones = objectsById(remoteTombstoneObject.value(QStringLiteral("devices")).toArray());

    mergeEntityChanges(
        base.value(QStringLiteral("groups")).toArray(),
        local.value(QStringLiteral("groups")).toArray(),
        &remoteGroups,
        &groupTombstones,
        updatedBy,
        committedAt);
    mergeEntityChanges(
        base.value(QStringLiteral("devices")).toArray(),
        local.value(QStringLiteral("devices")).toArray(),
        &remoteDevices,
        &deviceTombstones,
        updatedBy,
        committedAt);

    for (auto it = remoteDevices.begin(); it != remoteDevices.end(); ++it) {
        QJsonObject device = it.value();
        const QString groupId = device.value(QStringLiteral("groupId")).toString();
        if (groupId.isEmpty() || remoteGroups.contains(groupId)) {
            continue;
        }
        device.insert(QStringLiteral("groupId"), QString());
        device.insert(QStringLiteral("group"), QString());
        it.value() = device; // wjy: 删除分组后仍保留其中设备，只把它们移动回“我的设备”根部。
    }

    QJsonObject tombstones;
    tombstones.insert(QStringLiteral("devices"), orderedTombstones(deviceTombstones));
    tombstones.insert(QStringLiteral("groups"), orderedTombstones(groupTombstones));
    QJsonObject merged;
    merged.insert(QStringLiteral("schemaVersion"), kDeviceListSchemaVersion);
    merged.insert(QStringLiteral("revision"), deviceListSnapshotRevision(remote) + 1);
    merged.insert(QStringLiteral("updatedAt"), committedAt);
    merged.insert(QStringLiteral("updatedBy"), updatedBy);
    merged.insert(QStringLiteral("devices"), orderedObjects(remoteDevices));
    merged.insert(QStringLiteral("groups"), orderedObjects(remoteGroups));
    merged.insert(QStringLiteral("tombstones"), tombstones);
    return normalizeDeviceListSnapshot(merged); // wjy: 最终再规范化顺序和字段，确保不同设备写出的 JSON 结构稳定一致。
    // ===end====
}

} // namespace platform
