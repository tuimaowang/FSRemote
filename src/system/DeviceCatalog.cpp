#include "system/DeviceCatalog.h"

#include "system/DeviceListSyncModel.h"

#include <QAbstractSocket>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QSet>
#include <QUuid>

#include <utility>

namespace platform {
namespace {

// =====wjy====
QString normalizedUuidOrNew(const QString& value)
{
    const QUuid parsed(value.trimmed());
    return parsed.isNull()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces).toLower() // wjy: 新实体只在没有合法稳定 ID 时生成 UUID，迁移和同步传入的合法 ID 保持不变。
        : parsed.toString(QUuid::WithoutBraces).toLower();
}

bool validIpv4(const QString& value)
{
    QHostAddress address;
    return address.setAddress(value.trimmed())
        && address.protocol() == QAbstractSocket::IPv4Protocol; // wjy: 目录层沿用旧 DeviceGrid 的 IPv4 约束，抽离不能让非法地址进入设备权威集合。
}
// ===end====

} // namespace

// =====wjy====
QString DeviceRecord::displayName() const
{
    const QString trimmedName = name.trimmed();
    return trimmedName.isEmpty() ? ip.trimmed() : trimmedName; // wjy: 所有消费者统一使用相同回退，避免列表、菜单和远控标题显示不一致。
}

const QVector<DeviceRecord>& DeviceCatalog::devices() const
{
    return m_devices;
}

const QVector<QString>& DeviceCatalog::groupNames() const { return m_groupNames; }
const QVector<QString>& DeviceCatalog::groupIds() const { return m_groupIds; }
const QVector<bool>& DeviceCatalog::groupExpandedStates() const { return m_groupExpandedStates; }

QVector<DeviceGroupRecord> DeviceCatalog::groups() const
{
    QVector<DeviceGroupRecord> result;
    result.reserve(m_groupNames.size());
    for (int index = 0; index < m_groupNames.size(); ++index) {
        DeviceGroupRecord group;
        group.id = m_groupIds.value(index);
        group.name = m_groupNames.at(index);
        group.expanded = m_groupExpandedStates.value(index, true);
        result.append(std::move(group));
    }
    return result;
}

// =====wjy====
QVector<DeviceRecord>& DeviceCatalog::devicesMutable() { return m_devices; }
QVector<QString>& DeviceCatalog::groupNamesMutable() { return m_groupNames; }
QVector<QString>& DeviceCatalog::groupIdsMutable() { return m_groupIds; }
QVector<bool>& DeviceCatalog::groupExpandedStatesMutable() { return m_groupExpandedStates; }
qint64& DeviceCatalog::revisionMutable() { return m_revision; }
QString& DeviceCatalog::updatedAtMutable() { return m_updatedAt; }
QString& DeviceCatalog::updatedByMutable() { return m_updatedBy; }
QJsonObject& DeviceCatalog::tombstonesMutable() { return m_tombstones; }
// ===end====

int DeviceCatalog::deviceIndexForId(const QString& deviceId) const
{
    const QString target = QUuid(deviceId.trimmed()).toString(QUuid::WithoutBraces).toLower();
    if (target.isEmpty()) return -1;
    for (int index = 0; index < m_devices.size(); ++index) {
        if (m_devices.at(index).id.compare(target, Qt::CaseInsensitive) == 0) return index;
    }
    return -1;
}

int DeviceCatalog::deviceIndexForIp(const QString& deviceIp) const
{
    const QString target = deviceIp.trimmed();
    if (target.isEmpty()) return -1;
    for (int index = 0; index < m_devices.size(); ++index) {
        if (m_devices.at(index).ip.compare(target, Qt::CaseInsensitive) == 0) return index;
    }
    return -1;
}

int DeviceCatalog::groupIndexForId(const QString& groupId) const
{
    const QString target = QUuid(groupId.trimmed()).toString(QUuid::WithoutBraces).toLower();
    if (target.isEmpty()) return -1;
    for (int index = 0; index < m_groupIds.size(); ++index) {
        if (m_groupIds.at(index).compare(target, Qt::CaseInsensitive) == 0) return index;
    }
    return -1;
}

int DeviceCatalog::groupIndexForName(const QString& groupName) const
{
    const QString target = groupName.trimmed();
    if (target.isEmpty()) return -1;
    for (int index = 0; index < m_groupNames.size(); ++index) {
        if (m_groupNames.at(index).compare(target, Qt::CaseSensitive) == 0) return index;
    }
    return -1;
}

bool DeviceCatalog::addDevice(DeviceRecord device)
{
    device.ip = device.ip.trimmed();
    if (!validIpv4(device.ip) || deviceIndexForIp(device.ip) >= 0) {
        return false; // wjy: 权威目录拒绝非法或重复 IP，避免多个 UI 入口各自形成不同去重规则。
    }
    device.id = normalizedUuidOrNew(device.id);
    if (deviceIndexForId(device.id) >= 0) return false;
    device.name = device.name.trimmed();
    if (device.name.isEmpty()) device.name = device.ip;
    device.mac = device.mac.trimmed();
    device.broadcastIp = device.broadcastIp.trimmed();
    const int targetGroup = groupIndexForId(device.groupId);
    if (targetGroup >= 0) {
        device.groupId = m_groupIds.at(targetGroup);
        device.group = m_groupNames.at(targetGroup);
    } else {
        device.groupId.clear();
        device.group.clear();
    }
    m_devices.append(std::move(device));
    return true;
}

bool DeviceCatalog::updateDevice(const QString& deviceId, DeviceRecord replacement)
{
    const int index = deviceIndexForId(deviceId);
    if (index < 0) return false;

    replacement.id = m_devices.at(index).id; // wjy: 更新只能修改原稳定实体的字段，调用方不能借替换操作改变设备身份。
    replacement.ip = replacement.ip.trimmed();
    if (!validIpv4(replacement.ip)) return false;
    const int duplicateIpIndex = deviceIndexForIp(replacement.ip);
    if (duplicateIpIndex >= 0 && duplicateIpIndex != index) return false;
    replacement.name = replacement.name.trimmed();
    if (replacement.name.isEmpty()) replacement.name = replacement.ip;
    replacement.mac = replacement.mac.trimmed();
    replacement.broadcastIp = replacement.broadcastIp.trimmed();

    const int targetGroup = groupIndexForId(replacement.groupId);
    if (targetGroup >= 0) {
        replacement.groupId = m_groupIds.at(targetGroup);
        replacement.group = m_groupNames.at(targetGroup); // wjy: 兼容 group 名始终由稳定 groupId 反查，避免分组重命名后出现双重事实来源。
    } else {
        const int legacyGroup = groupIndexForName(replacement.group);
        if (legacyGroup >= 0) {
            replacement.groupId = m_groupIds.at(legacyGroup);
            replacement.group = m_groupNames.at(legacyGroup); // wjy: 迁移期兼容旧 UI 只修改 group 名的调用，保存前收敛为稳定分组身份。
        } else {
            replacement.groupId.clear();
            replacement.group.clear();
        }
    }
    m_devices[index] = std::move(replacement);
    return true;
}

// 设置设备是否从所有控制端列表隐藏；设备不存在或状态未变化时返回 false，避免产生无意义同步提交。
bool DeviceCatalog::setDeviceGloballyHidden(const QString& deviceId, bool hidden)
{
    const int index = deviceIndexForId(deviceId); // wjy: 通过稳定 UUID 定位设备，不能依赖同步后可能变化的数组下标。
    if (index < 0 || m_devices.at(index).globallyHidden == hidden) { // wjy: 无效设备和重复状态都不修改目录，调用方据此判断是否需要保存。
        return false;
    }
    m_devices[index].globallyHidden = hidden; // wjy: 设备实体继续保留在目录中，仅改变所有客户端共同消费的列表可见性字段。
    return true;
}

int DeviceCatalog::addGroup(const QString& name, const QString& requestedId, bool expanded)
{
    const QString normalizedName = name.trimmed();
    if (normalizedName.isEmpty() || groupIndexForName(normalizedName) >= 0) return -1;
    DeviceGroupRecord group;
    group.id = normalizedUuidOrNew(requestedId);
    if (groupIndexForId(group.id) >= 0) return -1;
    group.name = normalizedName;
    group.expanded = expanded;
    m_groupIds.append(group.id);
    m_groupNames.append(group.name);
    m_groupExpandedStates.append(group.expanded);
    return m_groupNames.size() - 1;
}

bool DeviceCatalog::renameDevice(const QString& deviceId, const QString& newName)
{
    const int index = deviceIndexForId(deviceId);
    const QString normalizedName = newName.trimmed();
    if (index < 0 || normalizedName.isEmpty()) return false;
    m_devices[index].name = normalizedName;
    return true;
}

bool DeviceCatalog::renameGroup(const QString& groupId, const QString& newName)
{
    const int index = groupIndexForId(groupId);
    const QString normalizedName = newName.trimmed();
    if (index < 0 || normalizedName.isEmpty()) return false;
    const int duplicate = groupIndexForName(normalizedName);
    if (duplicate >= 0 && duplicate != index) return false;
    const QString oldName = m_groupNames.at(index);
    m_groupNames[index] = normalizedName;
    for (DeviceRecord& device : m_devices) {
        if (device.groupId == m_groupIds.at(index) || device.group == oldName) {
            device.groupId = m_groupIds.at(index);
            device.group = normalizedName; // wjy: 分组重命名同时更新兼容名称，稳定 groupId 始终保持不变。
        }
    }
    return true;
}

bool DeviceCatalog::setGroupExpanded(const QString& groupId, bool expanded)
{
    const int index = groupIndexForId(groupId);
    if (index < 0) return false;
    m_groupExpandedStates[index] = expanded;
    return true;
}

bool DeviceCatalog::assignDevicesToGroup(const QVector<QString>& deviceIds, const QString& groupId)
{
    const int targetGroupIndex = groupId.trimmed().isEmpty() ? -1 : groupIndexForId(groupId);
    if (!groupId.trimmed().isEmpty() && targetGroupIndex < 0) return false;
    bool changed = false;
    QSet<QString> appliedIds;
    for (const QString& deviceId : deviceIds) {
        const int deviceIndex = deviceIndexForId(deviceId);
        if (deviceIndex < 0 || appliedIds.contains(m_devices.at(deviceIndex).id)) continue;
        appliedIds.insert(m_devices.at(deviceIndex).id);
        const QString targetId = targetGroupIndex < 0 ? QString() : m_groupIds.at(targetGroupIndex);
        const QString targetName = targetGroupIndex < 0 ? QString() : m_groupNames.at(targetGroupIndex);
        if (m_devices.at(deviceIndex).groupId == targetId && m_devices.at(deviceIndex).group == targetName) continue;
        m_devices[deviceIndex].groupId = targetId;
        m_devices[deviceIndex].group = targetName;
        changed = true;
    }
    return changed;
}

bool DeviceCatalog::removeDevice(const QString& deviceId)
{
    const int index = deviceIndexForId(deviceId);
    if (index < 0) return false;
    m_devices.removeAt(index);
    return true;
}

bool DeviceCatalog::removeGroup(const QString& groupId)
{
    const int index = groupIndexForId(groupId);
    if (index < 0) return false;
    const QString removedId = m_groupIds.at(index);
    const QString removedName = m_groupNames.at(index);
    m_groupIds.removeAt(index);
    m_groupNames.removeAt(index);
    m_groupExpandedStates.removeAt(index);
    for (DeviceRecord& device : m_devices) {
        if (device.groupId == removedId || device.group == removedName) {
            device.groupId.clear();
            device.group.clear(); // wjy: 删除分组只把设备移回根部，不能连带删除分组中的设备。
        }
    }
    return true;
}

bool DeviceCatalog::moveGroup(const QString& groupId, int targetIndex)
{
    const int sourceIndex = groupIndexForId(groupId);
    if (sourceIndex < 0) return false;
    const int boundedTarget = qBound(0, targetIndex, m_groupNames.size() - 1);
    if (sourceIndex == boundedTarget) return false;

    // =====wjy====
    const QString movedId = m_groupIds.takeAt(sourceIndex); // wjy: 三个同位数组必须作为同一个分组实体一起移动，不能让名称、ID、展开状态错位。
    const QString movedName = m_groupNames.takeAt(sourceIndex);
    const bool movedExpanded = m_groupExpandedStates.takeAt(sourceIndex);
    m_groupIds.insert(boundedTarget, movedId);
    m_groupNames.insert(boundedTarget, movedName);
    m_groupExpandedStates.insert(boundedTarget, movedExpanded);
    // ===end====
    return true;
}

QJsonObject DeviceCatalog::snapshot(bool includeLocalUiState) const
{
    QJsonArray groups;
    for (int index = 0; index < m_groupNames.size(); ++index) {
        groups.append(QJsonObject{
            {QStringLiteral("id"), m_groupIds.value(index)},
            {QStringLiteral("name"), m_groupNames.at(index)},
        });
    }
    QJsonArray devices;
    for (const DeviceRecord& record : m_devices) {
        devices.append(QJsonObject{
            {QStringLiteral("id"), record.id},
            {QStringLiteral("name"), record.name},
            {QStringLiteral("ip"), record.ip},
            {QStringLiteral("mac"), record.mac},
            {QStringLiteral("broadcast_ip"), record.broadcastIp},
            {QStringLiteral("remark"), record.remark},
            {QStringLiteral("group"), record.group},
            {QStringLiteral("groupId"), record.groupId},
            {QStringLiteral("globallyHidden"), record.globallyHidden}, // wjy: 全局隐藏状态进入本地和共享快照，所有客户端收到后采用同一列表过滤结果。
        });
    }
    QJsonObject raw{
        {QStringLiteral("schemaVersion"), 2},
        {QStringLiteral("revision"), m_revision},
        {QStringLiteral("updatedAt"), m_updatedAt},
        {QStringLiteral("updatedBy"), m_updatedBy},
        {QStringLiteral("devices"), devices},
        {QStringLiteral("groups"), groups},
        {QStringLiteral("tombstones"), m_tombstones},
    };
    QJsonObject normalized = normalizeDeviceListSnapshot(raw); // wjy: 复用现有规范化器保证目录抽离前后的 JSON 字段、顺序和兼容迁移完全一致。
    if (includeLocalUiState) {
        QHash<QString, bool> expandedById;
        for (int index = 0; index < m_groupNames.size(); ++index) {
            expandedById.insert(m_groupIds.value(index), m_groupExpandedStates.value(index, true));
        }
        QJsonArray localGroups = normalized.value(QStringLiteral("groups")).toArray();
        for (int index = 0; index < localGroups.size(); ++index) {
            QJsonObject group = localGroups.at(index).toObject();
            group.insert(QStringLiteral("expanded"), expandedById.value(group.value(QStringLiteral("id")).toString(), true));
            localGroups.replace(index, group); // wjy: expanded 只在本机持久化快照中恢复，共享服务仍接收规范化后剥离该字段的载荷。
        }
        normalized.insert(QStringLiteral("groups"), localGroups);
    }
    return normalized;
}

bool DeviceCatalog::applySnapshot(const QJsonObject& source, bool preserveCurrentExpandedState)
{
    QHash<QString, bool> expandedById;
    QHash<QString, bool> expandedByName;
    if (preserveCurrentExpandedState) {
        for (int index = 0; index < m_groupNames.size(); ++index) {
            expandedById.insert(m_groupIds.value(index), m_groupExpandedStates.value(index, true));
            expandedByName.insert(m_groupNames.at(index).toCaseFolded(), m_groupExpandedStates.value(index, true));
        }
    }
    for (const QJsonValue& value : source.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject group = value.toObject();
        if (!group.contains(QStringLiteral("expanded"))) continue;
        const QString id = QUuid(group.value(QStringLiteral("id")).toString()).toString(QUuid::WithoutBraces).toLower();
        const QString nameKey = group.value(QStringLiteral("name")).toString().trimmed().toCaseFolded();
        const bool expanded = group.value(QStringLiteral("expanded")).toBool(true);
        if (!preserveCurrentExpandedState || !expandedById.contains(id)) expandedById.insert(id, expanded);
        if (!preserveCurrentExpandedState || !expandedByName.contains(nameKey)) expandedByName.insert(nameKey, expanded);
    }

    const QJsonObject normalized = normalizeDeviceListSnapshot(source);
    QVector<DeviceGroupRecord> groups;
    for (const QJsonValue& value : normalized.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject object = value.toObject();
        DeviceGroupRecord record;
        record.id = object.value(QStringLiteral("id")).toString();
        record.name = object.value(QStringLiteral("name")).toString();
        record.expanded = expandedById.value(record.id, expandedByName.value(record.name.toCaseFolded(), true));
        groups.append(std::move(record));
    }

    QVector<DeviceRecord> devices;
    for (const QJsonValue& value : normalized.value(QStringLiteral("devices")).toArray()) {
        const QJsonObject object = value.toObject();
        const QString ip = object.value(QStringLiteral("ip")).toString().trimmed();
        if (!validIpv4(ip)) continue;
        DeviceRecord record;
        record.id = object.value(QStringLiteral("id")).toString();
        record.name = object.value(QStringLiteral("name")).toString().trimmed();
        if (record.name.isEmpty()) record.name = ip;
        record.ip = ip;
        record.mac = object.value(QStringLiteral("mac")).toString().trimmed();
        record.broadcastIp = object.value(QStringLiteral("broadcast_ip")).toString().trimmed();
        record.remark = object.value(QStringLiteral("remark")).toString();
        record.group = object.value(QStringLiteral("group")).toString().trimmed();
        record.groupId = object.value(QStringLiteral("groupId")).toString();
        record.globallyHidden = object.value(QStringLiteral("globallyHidden")).toBool(false); // wjy: 旧快照没有字段时默认显示，升级不会意外隐藏既有设备。
        devices.append(std::move(record));
    }

    m_devices = std::move(devices);
    m_groupNames.clear();
    m_groupIds.clear();
    m_groupExpandedStates.clear();
    for (const DeviceGroupRecord& group : groups) {
        m_groupIds.append(group.id);
        m_groupNames.append(group.name);
        m_groupExpandedStates.append(group.expanded);
    }
    m_revision = deviceListSnapshotRevision(normalized);
    m_updatedAt = normalized.value(QStringLiteral("updatedAt")).toString();
    m_updatedBy = normalized.value(QStringLiteral("updatedBy")).toString();
    m_tombstones = normalized.value(QStringLiteral("tombstones")).toObject();
    return true;
}

// =====wjy====
void DeviceCatalog::normalizeState()
{
    // wjy: 旧界面仍可能直接写入兼容字段或追加没有 ID 的记录；在落盘前统一经过一次规范化，
    // 让稳定 ID、groupId、默认名称和历史 JSON 迁移规则只在目录层维护，避免 UI 再复制一份规则。
    const QJsonObject normalized = snapshot(false);
    applySnapshot(normalized, true);
}
// ===end====

void DeviceCatalog::clear()
{
    m_devices.clear();
    m_groupNames.clear();
    m_groupIds.clear();
    m_groupExpandedStates.clear();
    m_revision = 0;
    m_updatedAt.clear();
    m_updatedBy.clear();
    m_tombstones = QJsonObject();
}

qint64 DeviceCatalog::revision() const { return m_revision; }
QString DeviceCatalog::updatedAt() const { return m_updatedAt; }
QString DeviceCatalog::updatedBy() const { return m_updatedBy; }
QJsonObject DeviceCatalog::tombstones() const { return m_tombstones; }
// ===end====

} // namespace platform
