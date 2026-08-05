#pragma once

#include <QObject>
#include <QJsonObject>
#include <QThreadPool>
#include <QTimer>

#include <mutex>

namespace platform {

class DeviceListSyncService final : public QObject {
    Q_OBJECT

public:
    static DeviceListSyncService& instance();
    ~DeviceListSyncService() override; // wjy: 静态服务销毁前再次取消并等待共享工作线程，避免 QThreadPool 析构阶段无日志地长期占用进程。

    void start(const QString& localDataDirectory, const QJsonObject& localSnapshot);
    void stop();
    void submitLocalSnapshot(const QJsonObject& localSnapshot);
    void requestImmediateSync();

signals:
    void snapshotAvailable(const QJsonObject& snapshot);

private:
    explicit DeviceListSyncService(QObject* parent = nullptr);
    void scheduleSync();
    // =====wjy====
    void startSyncWorker(); // wjy: 只有异步 SMB 端口探测成功后才真正启动共享目录读写任务。
    void handleSharedStorageProbeFinished(bool available); // wjy: 消费本服务等待中的连接测试结果，离线时保留待同步状态但不触碰 UNC。
    void handleWorkerTimeout(); // wjy: 共享文件访问超过限制后持续请求取消 Windows 同步 I/O，避免后台线程长期占用。
    void cancelWorkerSynchronousIo(const QString& phase); // wjy: 在运行超时和程序退出阶段统一取消当前设备列表同步线程的阻塞文件调用。
    // ===end====
    void handleWorkerResult(const QJsonObject& resultEnvelope);
    QString loadOrCreateClientId() const;

    QTimer m_pollTimer;
    // =====wjy====
    QTimer m_workerTimeoutTimer; // wjy: 只负责限制单轮共享文件任务时长，不在主线程执行任何目录或文件访问。
    std::mutex m_workerThreadHandleMutex; // wjy: 保护工作线程真实句柄的登记、取消和关闭，避免退出与完成清理并发使用失效句柄。
    void* m_workerThreadHandle = nullptr; // wjy: Windows 下保存可传给 CancelSynchronousIo 的真实线程句柄；其它平台保持空值。
    bool m_probePending = false; // wjy: 一轮共享存储连接测试进行时合并定时器和本地修改触发的重复同步请求。
    // ===end====
    QString m_localDataDirectory;
    QString m_clientId;
    QJsonObject m_localSnapshot;
    quint64 m_generation = 0;
    qint64 m_retryNotBeforeMs = 0; // wjy: 共享探测或文件任务失败后进入短时冷却，定时轮询只保留 pending 而不持续创建 SMB 线程。
    bool m_started = false;
    bool m_running = false;
    bool m_localDirty = false;
    bool m_resyncRequested = false;
    QThreadPool m_workerPool; // wjy: 最后声明使其最先析构；线程池确认工作任务结束后，句柄互斥量、定时器和同步状态才允许销毁。
};

} // namespace platform
