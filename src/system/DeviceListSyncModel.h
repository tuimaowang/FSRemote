#pragma once

#include <QJsonObject>

namespace platform {

// =====wjy====
QJsonObject normalizeDeviceListSnapshot(const QJsonObject& snapshot); // wjy: 将旧版设备 JSON 迁移为带稳定 ID、groupId、revision 和墓碑的新同步格式。
QJsonObject mergeDeviceListSnapshots(
    const QJsonObject& baseSnapshot,
    const QJsonObject& localSnapshot,
    const QJsonObject& remoteSnapshot,
    const QString& updatedBy); // wjy: 把 base→local 的实体级变化应用到最新 remote，同一实体由最后取得共享锁的一端生效。
bool deviceListSnapshotsEquivalent(const QJsonObject& left, const QJsonObject& right); // wjy: 比较同步有效载荷，忽略 revision、更新时间等提交元数据。
qint64 deviceListSnapshotRevision(const QJsonObject& snapshot); // wjy: 集中读取 revision，旧文件或非法值统一视为 0。
// ===end====

} // namespace platform
