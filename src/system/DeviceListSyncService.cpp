#include "system/DeviceListSyncService.h"

#include "system/DeviceCommandService.h"
#include "system/DeviceListSyncModel.h"
#include "system/SharedStorageAvailabilityService.h"
#include "system/WjyDiagnosticLog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMetaObject>
#include <QRunnable>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <exception>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace platform {
namespace {

constexpr int kSyncPollIntervalMs = 5000;
constexpr int kSharedLockTimeoutMs = 2500;
constexpr int kSharedLockStaleMs = 30000;
// =====wjy====
constexpr int kSyncWorkerTimeoutMs = 3500; // wjy: 445 可连接但共享目录仍异常时，单轮文件任务最多正常等待 3.5 秒。
constexpr int kSyncWorkerCancelRetryMs = 250; // wjy: 首次取消可能早于线程进入内核文件调用，短间隔重试覆盖该竞态。
constexpr qint64 kSyncFailureCooldownMs = 30 * 1000; // wjy: 共享目录或连接失败后冷却 30 秒，避免五秒轮询在长期离线环境持续制造 SMB 压力。
// ===end====

QString sharedSyncDirectory()
{
    const QString overridePath = qEnvironmentVariable("FSREMOTE_DEVICE_SYNC_ROOT").trimmed();
    if (!overridePath.isEmpty()) {
        return QDir::cleanPath(overridePath); // wjy: 测试或维护时可临时指向本地目录，正式运行仍使用固定共享路径。
    }
    return QDir::fromNativeSeparators(QString::fromUtf8(R"(\\192.168.1.100\广告部工具\远程软件_FS\同步配置)"));
}

QString baseSnapshotPath(const QString& localDataDirectory)
{
    return QDir(localDataDirectory).filePath(QStringLiteral("devices.sync-base.json"));
}

QString pendingSnapshotPath(const QString& localDataDirectory)
{
    return QDir(localDataDirectory).filePath(QStringLiteral("devices.pending.json"));
}

bool readJsonObject(const QString& filePath, QJsonObject* object)
{
    if (!object) {
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    *object = normalizeDeviceListSnapshot(document.object());
    return true;
}

bool writeJsonObject(const QString& filePath, const QJsonObject& object)
{
    const QFileInfo info(filePath);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit(); // wjy: 临时文件写完后原子替换，进程崩溃或网络中断不会留下半截 JSON。
}

QJsonObject snapshotWithCommitMetadata(const QJsonObject& snapshot, qint64 revision, const QString& clientId)
{
    QJsonObject result = normalizeDeviceListSnapshot(snapshot);
    result.insert(QStringLiteral("revision"), revision);
    result.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    result.insert(QStringLiteral("updatedBy"), clientId);
    return result;
}

QJsonObject resultEnvelope(
    bool success,
    quint64 generation,
    bool consumedDirty,
    bool sharedCommitted,
    const QJsonObject& snapshot,
    const QString& error = {},
    bool retrySoon = false)
{
    QJsonObject result;
    result.insert(QStringLiteral("success"), success);
    result.insert(QStringLiteral("generation"), QString::number(generation));
    result.insert(QStringLiteral("consumedDirty"), consumedDirty);
    result.insert(QStringLiteral("sharedCommitted"), sharedCommitted);
    result.insert(QStringLiteral("snapshot"), snapshot);
    result.insert(QStringLiteral("error"), error);
    result.insert(QStringLiteral("retrySoon"), retrySoon); // wjy: 正常锁竞争允许下一次五秒轮询重试，真正的共享故障才进入长冷却。
    return result;
}

QJsonObject executeSync(
    const QString& localDataDirectory,
    const QString& clientId,
    const QJsonObject& localSnapshot,
    bool localDirty,
    quint64 generation)
{
    // =====wjy====
    const QString sharedDirectory = sharedSyncDirectory();
    if (!QDir().mkpath(sharedDirectory)) {
        return resultEnvelope(false, generation, false, false, {}, QStringLiteral("无法访问或创建共享同步目录：%1").arg(sharedDirectory));
    }

    const QString sharedPath = QDir(sharedDirectory).filePath(QStringLiteral("devices.json"));
    const QString backupPath = QDir(sharedDirectory).filePath(QStringLiteral("devices.json.bak"));
    const QString lockPath = QDir(sharedDirectory).filePath(QStringLiteral("devices.lock"));
    const QString basePath = baseSnapshotPath(localDataDirectory);

    QJsonObject base;
    const bool hasBase = readJsonObject(basePath, &base);
    QJsonObject remote;
    bool hasRemote = readJsonObject(sharedPath, &remote);
    if (!hasRemote) {
        hasRemote = readJsonObject(backupPath, &remote); // wjy: 主共享 JSON 损坏时先从上一次有效备份恢复视图，禁止拿空对象覆盖所有设备。
    }

    if (!localDirty && hasRemote) {
        if (!hasBase
            || deviceListSnapshotRevision(remote) > deviceListSnapshotRevision(base)
            || !deviceListSnapshotsEquivalent(remote, base)) {
            if (!writeJsonObject(basePath, remote)) {
                return resultEnvelope(false, generation, false, false, {}, QStringLiteral("无法写入本机同步基线"));
            }
            return resultEnvelope(true, generation, false, false, remote);
        }
        return resultEnvelope(true, generation, false, false, {});
    }

    QLockFile lock(lockPath);
    lock.setStaleLockTime(kSharedLockStaleMs);
    if (!lock.tryLock(kSharedLockTimeoutMs)) {
        return resultEnvelope(
            false,
            generation,
            false,
            false,
            {},
            QStringLiteral("共享设备列表正在被其它设备提交"),
            true); // wjy: 多台设备同时提交属于短暂锁竞争，不应被当成网盘故障冷却 30 秒。
    }

    QJsonObject lockedRemote;
    const bool lockedMainValid = readJsonObject(sharedPath, &lockedRemote);
    bool lockedRemoteValid = lockedMainValid;
    if (!lockedRemoteValid) {
        lockedRemoteValid = readJsonObject(backupPath, &lockedRemote);
    }

    QJsonObject committed;
    bool shouldWriteShared = false;
    if (!lockedRemoteValid) {
        committed = snapshotWithCommitMetadata(localSnapshot, 1, clientId); // wjy: 第一台上线设备负责建立 revision=1 的权威共享快照。
        shouldWriteShared = true;
    } else if (!hasBase && !localDirty) {
        committed = lockedRemote; // wjy: 新客户端第一次接入已有共享快照时先完整采用权威数据，不用旧本地副本反向覆盖。
    } else {
        const QJsonObject mergeBase = hasBase ? base : QJsonObject();
        const QJsonObject merged = mergeDeviceListSnapshots(mergeBase, localSnapshot, lockedRemote, clientId);
        if (deviceListSnapshotsEquivalent(merged, lockedRemote)) {
            committed = lockedRemote;
        } else {
            committed = merged;
            shouldWriteShared = true;
        }
    }

    if (shouldWriteShared) {
        if (lockedMainValid) {
            QFile::remove(backupPath);
            if (!QFile::copy(sharedPath, backupPath)) {
                return resultEnvelope(false, generation, false, false, {}, QStringLiteral("无法创建共享设备列表备份"));
            }
        }
        if (!writeJsonObject(sharedPath, committed)) {
            return resultEnvelope(false, generation, false, false, {}, QStringLiteral("无法原子提交共享设备列表"));
        }
    }
    if (!writeJsonObject(basePath, committed)) {
        return resultEnvelope(false, generation, false, shouldWriteShared, committed, QStringLiteral("共享提交成功，但无法更新本机同步基线"));
    }
    return resultEnvelope(true, generation, localDirty, shouldWriteShared, committed);
    // ===end====
}

QStringList snapshotDeviceIps(const QJsonObject& snapshot)
{
    QSet<QString> uniqueIps;
    for (const QJsonValue& value : snapshot.value(QStringLiteral("devices")).toArray()) {
        const QString ip = value.toObject().value(QStringLiteral("ip")).toString().trimmed();
        if (!ip.isEmpty()) {
            uniqueIps.insert(ip);
        }
    }
    return uniqueIps.values();
}

} // namespace

DeviceListSyncService& DeviceListSyncService::instance()
{
    static DeviceListSyncService service;
    return service;
}

DeviceListSyncService::DeviceListSyncService(QObject* parent)
    : QObject(parent)
{
    // =====wjy====
    m_workerPool.setMaxThreadCount(1); // wjy: 同一进程只允许一个共享同步任务，避免本机两个提交互相抢锁或倒序覆盖。
    m_workerPool.setExpiryTimeout(10000);
    m_pollTimer.setInterval(kSyncPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &DeviceListSyncService::requestImmediateSync); // wjy: 即时通知丢失或目标离线时，每 5 秒仍会按 revision 拉取一次。
    m_workerTimeoutTimer.setSingleShot(true); // wjy: 单次超时和后续取消重试复用同一定时器，主线程不会堆积多个超时回调。
    connect(&m_workerTimeoutTimer, &QTimer::timeout,
        this, &DeviceListSyncService::handleWorkerTimeout); // wjy: 超时回调只调用系统取消 API，不执行 UNC 文件操作。
    connect(&SharedStorageAvailabilityService::instance(), &SharedStorageAvailabilityService::probeFinished,
        this, &DeviceListSyncService::handleSharedStorageProbeFinished); // wjy: 设备列表同步与更新、壁纸复用同一个异步 445 门禁。
    // ===end====
}

DeviceListSyncService::~DeviceListSyncService()
{
    stop(); // wjy: 正常退出已由仓储提前停止；析构再次兜底处理第一次 5 秒内未能取消的极端 SMB 阻塞。
}

QString DeviceListSyncService::loadOrCreateClientId() const
{
    const QString path = QDir(m_localDataDirectory).filePath(QStringLiteral("sync-client-id.txt"));
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QString existing = QString::fromUtf8(file.readAll()).trimmed();
        if (!QUuid(existing).isNull()) {
            return QUuid(existing).toString(QUuid::WithoutBraces).toLower();
        }
    }

    const QString created = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    QSaveFile output(path);
    if (QDir().mkpath(QFileInfo(path).absolutePath()) && output.open(QIODevice::WriteOnly)) {
        output.write(created.toUtf8());
        output.commit();
    }
    return created; // wjy: updatedBy 使用每次安装稳定不变的客户端 ID，日志和冲突来源可追踪但不依赖计算机名。
}

void DeviceListSyncService::start(const QString& localDataDirectory, const QJsonObject& localSnapshot)
{
    // =====wjy====
    if (m_started || m_running) {
        return; // wjy: 上一生命周期的共享线程尚未完全返回时禁止重新初始化，避免旧 generation 结果误应用到新快照。
    }
    m_localDataDirectory = QDir::cleanPath(localDataDirectory);
    m_clientId = loadOrCreateClientId();
    m_localSnapshot = normalizeDeviceListSnapshot(localSnapshot);
    m_generation = 0;
    m_retryNotBeforeMs = 0; // wjy: 每次服务启动重新允许首轮连接测试，不继承上次进程生命周期中的失败冷却。
    m_localDirty = false;
    m_resyncRequested = false;
    m_probePending = false; // wjy: 新一轮服务启动不继承上次退出前尚未完成的探测等待状态。
    QJsonObject pending;
    if (readJsonObject(pendingSnapshotPath(m_localDataDirectory), &pending)) {
        m_localSnapshot = pending;
        m_localDirty = true; // wjy: 上次共享不可用时保留的 pending 在重启后继续提交，不丢用户已经完成的本地修改。
    } else {
        QJsonObject base;
        if (readJsonObject(baseSnapshotPath(m_localDataDirectory), &base)
            && !deviceListSnapshotsEquivalent(base, m_localSnapshot)) {
            m_localDirty = true;
            writeJsonObject(pendingSnapshotPath(m_localDataDirectory), m_localSnapshot); // wjy: 若进程恰好在本地 devices.json 落盘后、pending 写入前崩溃，重启时通过 base 差异自动恢复待提交状态。
        }
    }
    m_started = true;
    m_pollTimer.start();
    requestImmediateSync();
    // ===end====
}

void DeviceListSyncService::stop()
{
    if (!m_started && !m_running) {
        return;
    }
    m_started = false;
    m_pollTimer.stop();
    m_resyncRequested = false;
    // =====wjy====
    m_probePending = false; // wjy: 退出后忽略共享探测的晚到结果，不能再启动文件工作线程。
    m_workerTimeoutTimer.stop();
    m_workerPool.clear();
    cancelWorkerSynchronousIo(QStringLiteral("stop-before-wait")); // wjy: 先取消当前可能阻塞的 SMB 调用，再等待工作线程清理句柄和返回。
    bool workerFinished = m_workerPool.waitForDone(0); // wjy: 没有活动任务时立即完成，正常退出不额外停顿 250 毫秒。
    for (int attempt = 0; attempt < 20 && !workerFinished; ++attempt) {
        cancelWorkerSynchronousIo(QStringLiteral("stop-wait-retry")); // wjy: 退出的 5 秒窗口内持续覆盖“第一次取消时尚未进入文件 I/O”的竞态。
        workerFinished = m_workerPool.waitForDone(250);
    }
    cancelWorkerSynchronousIo(QStringLiteral("stop-after-wait")); // wjy: 最后一轮兜底取消交给进程退出看门狗处理极端系统驱动不响应情况。
    m_running = !workerFinished; // wjy: 首轮退出等待未成功时保留活动标志，使静态服务析构还能再执行一轮取消与汇合。
    // ===end====
}

void DeviceListSyncService::submitLocalSnapshot(const QJsonObject& localSnapshot)
{
    if (!m_started) {
        return;
    }
    // =====wjy====
    m_localSnapshot = normalizeDeviceListSnapshot(localSnapshot);
    ++m_generation;
    m_localDirty = true;
    if (!writeJsonObject(pendingSnapshotPath(m_localDataDirectory), m_localSnapshot)) {
        writeWjyDiagnosticLog(QStringLiteral("[wjy-device-sync] failed to persist pending snapshot"));
    }
    requestImmediateSync(); // wjy: UI 先完成本地保存，再异步争取共享锁；共享断开时 pending 会留在本机等待恢复。
    // ===end====
}

void DeviceListSyncService::requestImmediateSync()
{
    if (!m_started) {
        return;
    }
    m_resyncRequested = true;
    scheduleSync();
}

void DeviceListSyncService::scheduleSync()
{
    if (!m_started || m_running || !m_resyncRequested) {
        return;
    }
    // =====wjy====
    if (QDateTime::currentMSecsSinceEpoch() < m_retryNotBeforeMs) {
        return; // wjy: 冷却期间不消费 m_resyncRequested，本地修改继续留在 pending，后续定时器到期会自动重试。
    }
    if (!qEnvironmentVariable("FSREMOTE_DEVICE_SYNC_ROOT").trimmed().isEmpty()) {
        startSyncWorker(); // wjy: 显式维护/测试路径不一定位于 192.168.1.100，跳过固定主机门禁但仍受工作线程超时取消保护。
        return;
    }
    if (m_probePending) {
        return; // wjy: 更新、壁纸或上一轮同步已在探测同一服务器时等待统一结果，不重复创建连接。
    }
    m_probePending = true;
    SharedStorageAvailabilityService::instance().requestProbe(); // wjy: 先异步连接 192.168.1.100:445，失败时本轮完全不创建共享文件任务。
    // ===end====
}

// =====wjy====
void DeviceListSyncService::handleSharedStorageProbeFinished(bool available)
{
    if (!m_probePending) {
        return; // wjy: 其它功能独立发起的探测结果不应改变本服务当前任务状态。
    }
    m_probePending = false;
    if (!m_started) {
        return; // wjy: stop 之后到达的连接结果只能被丢弃，不能重新访问共享目录。
    }
    if (!available) {
        m_retryNotBeforeMs = QDateTime::currentMSecsSinceEpoch() + kSyncFailureCooldownMs; // wjy: 文件服务器不可达时延后下一轮探测，长期离线不会每五秒重复连接。
        writeWjyDiagnosticLog(QStringLiteral("[wjy-device-sync] shared storage probe failed; UNC sync skipped")); // wjy: 保留本地 pending，等待下一个周期重新探测而不是进入阻塞文件调用。
        return;
    }
    startSyncWorker(); // wjy: 只有本轮 445 探测明确成功才消费待同步标志并开始真实共享读写。
}

void DeviceListSyncService::startSyncWorker()
{
    if (!m_started || m_running || !m_resyncRequested) {
        return; // wjy: 探测期间服务停止、已有任务启动或请求已被消费时不创建重复工作线程。
    }
    m_running = true;
    m_resyncRequested = false;
    const QString localDirectory = m_localDataDirectory;
    const QString clientId = m_clientId;
    const QJsonObject snapshot = m_localSnapshot;
    const bool dirty = m_localDirty;
    const quint64 generation = m_generation;

    m_workerPool.start(QRunnable::create([this, localDirectory, clientId, snapshot, dirty, generation] {
        // =====wjy====
#if defined(Q_OS_WIN)
        HANDLE workerHandle = nullptr;
        if (DuplicateHandle(
                GetCurrentProcess(),
                GetCurrentThread(),
                GetCurrentProcess(),
                &workerHandle,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS)) {
            std::lock_guard handleLock(m_workerThreadHandleMutex);
            m_workerThreadHandle = workerHandle; // wjy: 把当前 QThreadPool 线程的伪句柄复制成可跨线程取消的真实句柄。
        }
#endif
        // ===end====
        QJsonObject result;
        try {
            result = executeSync(localDirectory, clientId, snapshot, dirty, generation); // wjy: 所有共享目录异常都必须在工作线程边界内转换为普通失败结果。
        } catch (const std::exception& exception) {
            result = resultEnvelope(
                false,
                generation,
                false,
                false,
                {},
                QStringLiteral("设备列表同步后台异常：%1").arg(QString::fromLocal8Bit(exception.what()))); // wjy: 标准异常不能越过 QRunnable 入口触发进程终止。
        } catch (...) {
            result = resultEnvelope(
                false,
                generation,
                false,
                false,
                {},
                QStringLiteral("设备列表同步后台发生未知异常")); // wjy: 非标准异常同样收口，弱网或系统文件驱动异常只影响本轮同步。
        }
        // =====wjy====
#if defined(Q_OS_WIN)
        if (workerHandle) {
            std::lock_guard handleLock(m_workerThreadHandleMutex);
            if (m_workerThreadHandle == workerHandle) {
                m_workerThreadHandle = nullptr; // wjy: 完成前先撤销共享可见句柄，超时和退出回调不会再取消已结束任务。
            }
            CloseHandle(workerHandle);
        }
#endif
        // ===end====
        QMetaObject::invokeMethod(this, [this, result] {
            handleWorkerResult(result);
        }, Qt::QueuedConnection); // wjy: UNC 文件 IO 和锁等待全部留在后台线程，最终只把纯 JSON 结果排队回 UI 线程。
    }));
    m_workerTimeoutTimer.start(kSyncWorkerTimeoutMs); // wjy: 工作线程登记完成后才启动超时，确保超时处理能找到真实任务状态。
}

void DeviceListSyncService::handleWorkerTimeout()
{
    if (!m_running) {
        return;
    }
    cancelWorkerSynchronousIo(QStringLiteral("worker-timeout")); // wjy: 共享目录操作超时后请求 Windows 使本轮 QFile/QDir 尽快失败返回。
    if (m_running) {
        m_workerTimeoutTimer.start(kSyncWorkerCancelRetryMs); // wjy: 直到结果回到主线程前持续重试，防止首次取消落在非 I/O 计算阶段。
    }
}

void DeviceListSyncService::cancelWorkerSynchronousIo(const QString& phase)
{
#if defined(Q_OS_WIN)
    std::lock_guard handleLock(m_workerThreadHandleMutex);
    HANDLE workerHandle = reinterpret_cast<HANDLE>(m_workerThreadHandle);
    if (!workerHandle) {
        return;
    }
    if (!CancelSynchronousIo(workerHandle)) {
        const DWORD errorCode = GetLastError();
        if (errorCode != ERROR_NOT_FOUND) {
            writeWjyDiagnosticLog(QStringLiteral("[wjy-device-sync] CancelSynchronousIo failed phase=%1 error=%2")
                .arg(phase)
                .arg(errorCode)); // wjy: ERROR_NOT_FOUND 仅表示当前瞬间没有同步 I/O，其它错误保留诊断信息。
        }
    }
#else
    Q_UNUSED(phase)
#endif
}
// ===end====

void DeviceListSyncService::handleWorkerResult(const QJsonObject& resultEnvelopeObject)
{
    m_workerTimeoutTimer.stop(); // wjy: 工作线程已经返回，立即停止可能排队的下一次取消重试。
    m_running = false;
    if (!m_started) {
        return;
    }

    bool generationOk = false;
    const quint64 completedGeneration = resultEnvelopeObject.value(QStringLiteral("generation")).toString().toULongLong(&generationOk);
    const bool currentGeneration = generationOk && completedGeneration == m_generation;
    const bool success = resultEnvelopeObject.value(QStringLiteral("success")).toBool();
    const QJsonObject snapshot = resultEnvelopeObject.value(QStringLiteral("snapshot")).toObject();
    if (!success) {
        const QString error = resultEnvelopeObject.value(QStringLiteral("error")).toString().trimmed();
        const bool retrySoon = resultEnvelopeObject.value(QStringLiteral("retrySoon")).toBool();
        m_retryNotBeforeMs = retrySoon
            ? 0
            : QDateTime::currentMSecsSinceEpoch() + kSyncFailureCooldownMs; // wjy: 正常共享锁竞争保留五秒重试；目录、文件或异常失败才进入 30 秒冷却。
        if (!error.isEmpty()) {
            writeWjyDiagnosticLog(QStringLiteral("[wjy-device-sync] %1").arg(error));
        }
    } else {
        m_retryNotBeforeMs = 0; // wjy: 任一完整成功轮次立即解除旧失败冷却，后续设备修改可以马上提交。
    }
    if (success && currentGeneration && !snapshot.isEmpty()) {
        m_localSnapshot = normalizeDeviceListSnapshot(snapshot);
        if (resultEnvelopeObject.value(QStringLiteral("consumedDirty")).toBool()) {
            m_localDirty = false;
            QFile::remove(pendingSnapshotPath(m_localDataDirectory)); // wjy: 只有当前 generation 已成功提交时才删 pending，提交期间产生的新修改绝不会被误清理。
        }
        emit snapshotAvailable(m_localSnapshot);
    }

    if (success && resultEnvelopeObject.value(QStringLiteral("sharedCommitted")).toBool() && !snapshot.isEmpty()) {
        for (const QString& ip : snapshotDeviceIps(snapshot)) {
            QThreadPool::globalInstance()->start(QRunnable::create([ip] {
                QString ignoredError;
                DeviceCommandService::requestDeviceListSync(ip, &ignoredError, 49102, 700); // wjy: 提交后并行通知列表内设备立即检查 revision，失败由 5 秒轮询自动兜底。
            }));
        }
    }

    if (!currentGeneration) {
        m_resyncRequested = true; // wjy: 后台任务执行期间又有本地修改时忽略旧 UI 结果，并立即用最新 pending 再合并一次。
    }
    scheduleSync();
}

} // namespace platform
