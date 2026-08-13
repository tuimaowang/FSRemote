#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace platform {

// =====wjy====
struct DeviceRecord {
    QString name;
    QString ip;
    QString mac;
    QString broadcastIp;
    QString remark;
    QString group; // wjy: 兼容旧界面和旧 JSON 的分组名称；跨同步和重命名关联始终以稳定 groupId 为准。
    QString id; // wjy: 设备稳定 UUID，异步结果、同步和跨窗口操作不能再依赖可能变化的数组下标。
    QString groupId; // wjy: 设备所属分组的稳定 UUID，空字符串表示设备位于“我的设备”根部。
    bool globallyHidden = false; // wjy: 由设备本机设置并随共享目录同步；为 true 时所有控制端列表都隐藏该设备，但保留实体用于恢复显示。

    QString displayName() const; // wjy: 名称为空时统一回退 IP，避免各个 UI/控制器重复实现相同显示规则。
};

struct DeviceGroupRecord {
    QString id; // wjy: 分组稳定 UUID，重命名和排序不会改变设备真实归属。
    QString name;
    bool expanded = true; // wjy: 仅属于本机界面的展开状态，生成共享快照时必须剥离。
};

class DeviceCatalog final {
public:
    const QVector<DeviceRecord>& devices() const;
    const QVector<QString>& groupNames() const;
    const QVector<QString>& groupIds() const;
    const QVector<bool>& groupExpandedStates() const;
    QVector<DeviceGroupRecord> groups() const;

    // =====wjy====
    // 兼容迁移入口：DeviceGrid 旧代码仍按数组遍历，但容器所有权已归目录统一管理。
    QVector<DeviceRecord>& devicesMutable(); // wjy: 迁移期间保留旧 UI 的顺序访问，后续业务调用逐步改为稳定 ID 方法。
    QVector<QString>& groupNamesMutable(); // wjy: 分组名称数组暂时提供兼容视图，真正的分组身份由同位置 groupIds 维护。
    QVector<QString>& groupIdsMutable(); // wjy: 让旧的排序/展开代码继续工作，同时把内存所有权从 DeviceGrid 全局移出。
    QVector<bool>& groupExpandedStatesMutable(); // wjy: 本机展开状态属于目录数据，不再由匿名命名空间全局独立拥有。
    qint64& revisionMutable(); // wjy: 快照元数据在迁移期需要被旧保存流程写入，所有权仍保持在目录对象内。
    QString& updatedAtMutable(); // wjy: 兼容旧保存路径的可写元数据桥接，完成 repository 迁移后将收窄为方法调用。
    QString& updatedByMutable(); // wjy: 兼容旧保存路径的可写元数据桥接，避免本阶段修改同步语义。
    QJsonObject& tombstonesMutable(); // wjy: 墓碑由目录统一保存，旧 UI 逻辑可先通过引用完成无行为迁移。
    // ===end====

    int deviceIndexForId(const QString& deviceId) const;
    int deviceIndexForIp(const QString& deviceIp) const;
    int groupIndexForId(const QString& groupId) const;
    int groupIndexForName(const QString& groupName) const;

    bool addDevice(DeviceRecord device);
    bool updateDevice(const QString& deviceId, DeviceRecord replacement);
    bool setDeviceGloballyHidden(const QString& deviceId, bool hidden); // wjy: 只修改指定稳定设备实体的全局可见性，调用方负责确认自己有权修改该设备。
    int addGroup(const QString& name, const QString& requestedId = {}, bool expanded = true);
    bool renameDevice(const QString& deviceId, const QString& newName);
    bool renameGroup(const QString& groupId, const QString& newName);
    bool setGroupExpanded(const QString& groupId, bool expanded);
    bool assignDevicesToGroup(const QVector<QString>& deviceIds, const QString& groupId);
    bool removeDevice(const QString& deviceId);
    bool removeGroup(const QString& groupId);
    bool moveGroup(const QString& groupId, int targetIndex);

    QJsonObject snapshot(bool includeLocalUiState) const;
    bool applySnapshot(const QJsonObject& source, bool preserveCurrentExpandedState);
    void normalizeState();
    void clear();

    qint64 revision() const;
    QString updatedAt() const;
    QString updatedBy() const;
    QJsonObject tombstones() const;

private:
    QVector<DeviceRecord> m_devices;
    QVector<QString> m_groupNames;
    QVector<QString> m_groupIds;
    QVector<bool> m_groupExpandedStates;
    qint64 m_revision = 0;
    QString m_updatedAt;
    QString m_updatedBy;
    QJsonObject m_tombstones;
};
// ===end====

} // namespace platform
