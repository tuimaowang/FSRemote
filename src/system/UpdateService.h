#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <mutex>
#include <thread>

class QTimer;

namespace platform {

// =====wjy====
class UpdateService final : public QObject {
    Q_OBJECT

public:
    static UpdateService& instance();
    ~UpdateService() override; // wjy: 静态服务销毁前取消并汇合仍在读取 UNC 版本文件的专用线程，避免留下可连接线程触发进程终止。
    static QString updateShareRoot();
    static QString localVersionPath();
    static QString remoteVersionPath();
    static QString localVersionText();
    static QString remoteVersionText();
    static QString displayVersion(); // wjy: 标题栏只显示本机已安装版本，本地缺失时使用安全默认值。
    static bool canPublishCurrentBuild(); // wjy: 只有从 CMake 构建目录运行的开发版本可以向共享目录发布更新。
    static QString bumpPatchVersion(const QString& version);
    static int compareSemanticVersions(const QString& left, const QString& right); // wjy: 返回 -1/0/1，统一发布和检查的语义版本排序。

    static bool runtimeDependenciesNeedRepair(const QString& runtimeRoot); // wjy: 检查当前运行目录是否缺少 FakerInput Bridge/MSI，供同版本更新修复旧客户端漏下发的新增依赖。
    static bool remoteUpdateOrRepairAvailable(
        const QString& remoteVersion,
        const QString& localVersion,
        const QString& runtimeRoot); // wjy: 更高版本照常更新；版本相同仅在本地依赖残缺时开放一次修复更新。

    // =====wjy====
    static QStringList availableRollbackVersions(QString* errorMessage = nullptr); // wjy: 只返回共享不可变发布目录中低于当前安装版本、且具备完整关键运行文件的历史版本。
    // ===end====

    bool isUpdateAvailable() const; // wjy: 只返回最近一次异步连接测试和版本读取产生的缓存，不在命令查询路径同步访问网盘。
    QString confirmedRemoteVersion() const;
    bool publishCurrentBuild(QString* errorMessage = nullptr);
    bool applyRemoteUpdate(QString* errorMessage = nullptr); // wjy: 只准备暂存任务并启动独立更新器，不再覆盖运行中的程序。
    // =====wjy====
    bool applyVersionRollback(const QString& targetVersion, QString* errorMessage = nullptr); // wjy: 回撤入口只接受严格低于当前版本的目标，并复用事务更新器完成替换和重启。
    // ===end====
    void startPeriodicCheck();
    void stopPeriodicCheck();
    void checkNow();

signals:
    // =====wjy====
    void updateAvailabilityChanged(bool available, const QString& remoteVersion); // wjy: 后台检查只广播版本状态，由主窗口决定是否显示更新入口，不再弹窗或自动安装。
    // ===end====
    void updateReadyToQuit(); // wjy: 更新器准备完成后通知 main 有序退出并释放全部文件占用。

private:
    explicit UpdateService(QObject* parent = nullptr);
    // =====wjy====
    void handleSharedStorageProbeFinished(bool available); // wjy: SMB 连接测试成功后只启动后台版本读取，主界面线程不再直接触碰 UNC 文件。
    void startRemoteVersionRead(); // wjy: 为当前检查创建唯一后台任务，重复定时请求直接复用正在执行的读取。
    void finishRemoteVersionRead(
        quint64 generation,
        const QString& remoteVersion,
        const std::shared_ptr<std::thread>& worker); // wjy: 只接受当前代次且未超时的结果，晚到任务只能回收线程不能覆盖界面状态。
    void handleRemoteVersionReadTimeout(); // wjy: 超过限定时间后使结果失效，并持续请求 Windows 取消线程中的同步 SMB I/O。
    void cancelRemoteVersionReadIo(const QString& phase); // wjy: 统一封装运行期超时和退出阶段的 CancelSynchronousIo 调用。
    bool prepareRemoteVersionInstall(const QString& targetVersion, QString* errorMessage); // wjy: 升级与回撤共享同一套载荷暂存、校验、任务生成和独立更新器启动流程。
    // ===end====

    QTimer* m_timer = nullptr;
    // =====wjy====
    bool m_checkPending = false; // wjy: 一轮连接测试未返回前合并所有更新检查请求，避免重复探测服务器。
    bool m_periodicCheckRunning = false; // wjy: 停止服务后拒绝晚到探测和后台读取结果，退出阶段不再产生新任务。
    bool m_remoteVersionReadInProgress = false; // wjy: 网盘版本文件同一时间只允许一个专用线程读取，防止弱网下周期任务堆积。
    bool m_remoteVersionReadTimedOut = false; // wjy: 超时后即使底层 SMB 稍后返回，也只能回收线程而不能恢复过期结果。
    quint64 m_remoteVersionReadGeneration = 0; // wjy: 每次启动、超时和停止都推进代次，用于识别已经失效的异步结果。
    qint64 m_updateCheckCooldownUntilMs = 0; // wjy: 连接或文件读取失败后短时冷却，避免弱网状态下连续触碰共享目录。
    QTimer* m_remoteVersionReadTimeoutTimer = nullptr; // wjy: UI 线程只负责计时和取消，不参与任何 UNC 文件读取。
    std::shared_ptr<std::thread> m_remoteVersionReadThread; // wjy: 精确持有本轮工作线程，完成回调不会误等待下一轮任务。
    std::mutex m_versionTransactionMutex; // wjy: 本机按钮和远端命令可能来自不同线程，升级、回撤、发布必须在服务层保持全局互斥。
    bool m_updateAvailable = false; // wjy: 远端命令和界面直接读取缓存，不再各自触碰 UNC 版本文件。
    QString m_remoteVersion; // wjy: 保存最近一次成功连接后读取到的规范版本号，失败时立即清空。
    // ===end====
};
// ===end====

} // namespace platform
