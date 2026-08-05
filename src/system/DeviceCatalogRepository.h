#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

namespace platform {

class DeviceCatalog;

// =====wjy====
class DeviceCatalogRepository final {
public:
    using SyncStarter = std::function<void(const QString&, const QJsonObject&)>;
    using SyncStopper = std::function<void()>;
    using SnapshotSubmitter = std::function<void(const QJsonObject&)>;

    DeviceCatalogRepository(DeviceCatalog& catalog, QString storePath);

    QJsonObject snapshot(bool includeLocalUiState) const;
    void setStorePath(QString storePath);
    bool saveLocal();
    bool loadLocal();
    void startSynchronization(
        const QString& localDataDirectory,
        SyncStarter starter,
        SyncStopper stopper,
        SnapshotSubmitter submitter);
    void stopSynchronization();
    bool applySynchronizedSnapshot(const QJsonObject& snapshot);
    const QString& storePath() const;

private:
    bool persistLocalSnapshot();

    DeviceCatalog& m_catalog;
    QString m_storePath;
    SyncStopper m_syncStopper;
    SnapshotSubmitter m_snapshotSubmitter;
    bool m_syncStarted = false;
    bool m_applyingSynchronizedSnapshot = false;
};
// ===end====

} // namespace platform
