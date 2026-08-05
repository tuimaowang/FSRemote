#include "system/DeviceCatalogRepository.h"

#include "system/DeviceCatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include <utility>

namespace platform {

// =====wjy====
DeviceCatalogRepository::DeviceCatalogRepository(DeviceCatalog& catalog, QString storePath)
    : m_catalog(catalog)
    , m_storePath(std::move(storePath))
{
}

QJsonObject DeviceCatalogRepository::snapshot(bool includeLocalUiState) const
{
    // wjy: 目录是唯一业务状态来源，仓储只负责把它转换成持久化快照，不再在 UI 层拼装设备、分组和墓碑字段。
    return m_catalog.snapshot(includeLocalUiState);
}

void DeviceCatalogRepository::setStorePath(QString storePath)
{
    m_storePath = std::move(storePath); // wjy: 应用启动后再注入最终路径，避免静态对象初始化阶段依赖 QApplication 环境。
}

bool DeviceCatalogRepository::saveLocal()
{
    // wjy: 保存前先让目录补齐旧记录可能缺少的稳定 ID/groupId，确保当前内存和 devices.json 使用同一实体身份。
    m_catalog.normalizeState();
    if (!persistLocalSnapshot()) {
        return false;
    }
    if (m_syncStarted && !m_applyingSynchronizedSnapshot) {
        m_snapshotSubmitter(m_catalog.snapshot(false)); // wjy: 只有本地主文件原子落盘成功后才提交共享载荷，并明确剥离本机 expanded 状态。
    }
    return true;
}

bool DeviceCatalogRepository::persistLocalSnapshot()
{
    // =====wjy====
    const QJsonObject localRoot = m_catalog.snapshot(true);
    const QFileInfo info(m_storePath);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }

    QSaveFile file(info.filePath());
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(localRoot).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit(); // wjy: 使用 QSaveFile 原子替换，避免程序崩溃时留下半截设备配置。
    // ===end====
}

bool DeviceCatalogRepository::loadLocal()
{
    m_catalog.clear();
    QFile file(m_storePath);
    if (!file.exists()) {
        // wjy: 首次运行保留原有示例设备行为，真正的序列化仍由统一仓储完成。
        m_catalog.addDevice(DeviceRecord{
            QStringLiteral("72"), QStringLiteral("192.168.3.27"), {}, {}, {}, {}, {}, {}});
        return saveLocal();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError
        || (!document.isArray() && !document.isObject())) {
        return false;
    }

    QJsonObject rootObject;
    if (document.isArray()) {
        rootObject.insert(QStringLiteral("devices"), document.array());
    } else {
        rootObject = document.object();
    }
    if (!m_catalog.applySnapshot(rootObject, false)) {
        return false;
    }
    file.close(); // wjy: Windows 下重新原子替换同一路径前先关闭读取句柄，避免仓储误判为保存失败。
    // wjy: 启动时立即规范化旧格式，保证后续同步可以按稳定 ID 合并，而不是继续依赖数组位置。
    return saveLocal();
}

void DeviceCatalogRepository::startSynchronization(
    const QString& localDataDirectory,
    SyncStarter starter,
    SyncStopper stopper,
    SnapshotSubmitter submitter)
{
    if (m_syncStarted || !starter || !stopper || !submitter) {
        return;
    }
    // =====wjy====
    m_catalog.normalizeState(); // wjy: 同步服务启动时获得已经补齐稳定 ID 的共享快照，不能把旧兼容字段当成新实体上传。
    m_syncStopper = std::move(stopper); // wjy: 仓储保存停止入口，退出时不要求 DeviceGrid 再维护同步生命周期状态。
    m_snapshotSubmitter = std::move(submitter); // wjy: 本地保存后的提交动作注入仓储，使纯目录测试不依赖网络同步实现。
    m_syncStarted = true; // wjy: 先登记生命周期状态，后续本地保存才能通过同一个仓储入口提交 pending。
    starter(
        localDataDirectory,
        m_catalog.snapshot(false)); // wjy: 同步服务只接收共享字段，本机分组展开偏好不会传播到其它设备。
    // ===end====
}

void DeviceCatalogRepository::stopSynchronization()
{
    if (!m_syncStarted) {
        return;
    }
    // =====wjy====
    m_syncStarted = false; // wjy: 先阻止退出阶段的任何保存再次生成提交，再等待同步线程池汇合。
    m_syncStopper(); // wjy: 调用注入的同步停止入口，销毁 UI 后不再接收后台共享快照。
    m_syncStopper = {};
    m_snapshotSubmitter = {};
    // ===end====
}

bool DeviceCatalogRepository::applySynchronizedSnapshot(const QJsonObject& snapshot)
{
    if (snapshot.isEmpty()) {
        return false;
    }
    // =====wjy====
    m_applyingSynchronizedSnapshot = true; // wjy: 远端快照仍要写入本机 devices.json，但这次保存不能重新生成本地 pending。
    const bool applied = m_catalog.applySnapshot(snapshot, true); // wjy: 按稳定 ID 应用共享数据，同时保留本机独有的分组展开状态。
    const bool saved = applied && saveLocal(); // wjy: 复用同一原子保存入口；同步抑制标志使该写入不会递归提交。
    m_applyingSynchronizedSnapshot = false;
    return saved;
    // ===end====
}

const QString& DeviceCatalogRepository::storePath() const
{
    return m_storePath;
}
// ===end====

} // namespace platform
