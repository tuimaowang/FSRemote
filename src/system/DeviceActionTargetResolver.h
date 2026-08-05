#pragma once

#include <QString>
#include <QVector>

namespace platform {

class DeviceCatalog;

// =====wjy====
class DeviceActionTargetResolver final {
public:
    static QVector<QString> normalizeDeviceIds(
        const DeviceCatalog& catalog,
        const QVector<QString>& requestedIds,
        const QString& fallbackDeviceId = {});

    static QVector<int> indexesForDeviceIds(
        const DeviceCatalog& catalog,
        const QVector<QString>& deviceIds);

    static QVector<QString> deviceIdsForGroup(
        const DeviceCatalog& catalog,
        const QString& groupId);
};
// ===end====

} // namespace platform
