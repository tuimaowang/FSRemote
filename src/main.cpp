#include "ui/MainWindow.h"
#include "ui/DeviceGrid.h"

#include "stream/StreamRuntime.h"
#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "system/DeviceStatusService.h"
#include "system/ParsecVddInstaller.h"
#include "system/PowerManager.h"
#include "system/PortableOpenSshManager.h"
#include "system/StartupManager.h"
#include "system/UpdateService.h"
#include "system/WjyDiagnosticLog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QObject>
#include <QStringList>
#include <QTimer>

namespace {

void writeStartupLog(const QString& message)
{
    platform::writeWjyDiagnosticLog(message); // wjy: Keep startup diagnostics behind one helper so logging can be changed centrally.
}

// =====wjy====
constexpr const char* kSingleInstanceKey = "FSRemote_SingleInstance_v1";

bool activateExistingInstance()
{
    QLocalSocket socket;
    socket.connectToServer(QString::fromLatin1(kSingleInstanceKey));
    if (!socket.waitForConnected(300)) {
        return false;
    }
    socket.write("raise");
    socket.flush();
    socket.waitForBytesWritten(300);
    return true;
}
// ===end====

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    writeStartupLog(QStringLiteral("[wjy-main] app created"));

    QApplication::setApplicationName(QStringLiteral("FSRemote"));
    QApplication::setOrganizationName(QStringLiteral("FSRemote"));
    writeStartupLog(QStringLiteral("[wjy-main] app metadata set"));

    // =====wjy====
    // wjy: 禁止重复启动；二次启动只唤醒已有主窗口。
    if (activateExistingInstance()) {
        writeStartupLog(QStringLiteral("[wjy-main] another instance is running, activate and exit"));
        return 0;
    }
    QLocalServer::removeServer(QString::fromLatin1(kSingleInstanceKey));
    QLocalServer singleInstanceServer;
    if (!singleInstanceServer.listen(QString::fromLatin1(kSingleInstanceKey))) {
        writeStartupLog(QStringLiteral("[wjy-main] single-instance server listen failed"));
    }
    // ===end====

    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(13);
    app.setFont(font);
    writeStartupLog(QStringLiteral("[wjy-main] font set"));

    writeStartupLog(QStringLiteral("[wjy-main] before ParsecVddInstaller::ensureInstalled")); // wjy: Restore virtual display driver preparation before stream host starts.
    platform::ParsecVddInstaller::ensureInstalled(); // wjy: Ensure the Parsec VDD driver exists; if missing, the installer may request elevation.
    writeStartupLog(QStringLiteral("[wjy-main] after ParsecVddInstaller::ensureInstalled")); // wjy: Continue even if the installer did not run, matching the previous non-blocking startup style.

    // =====wjy====
    FsRemoteHostConfig hostConfig = {}; // wjy: 在 Qt 层汇总可持久化配置，再通过稳定 C ABI 一次性复制到原生 DLL。
    hostConfig.struct_size = sizeof(hostConfig);
    hostConfig.version = 1;
    hostConfig.max_sessions = static_cast<uint32_t>(platform::AppSettings::remoteHostMaxSessions()); // wjy: 默认值已按用户要求设为 3；仅当本机已有显式设置时才使用保存的 1 至 3 配置。
    hostConfig.max_aggregate_video_kbps = static_cast<uint32_t>(platform::AppSettings::remoteHostAggregateVideoKbps());
    hostConfig.handshake_timeout_ms = static_cast<uint32_t>(platform::AppSettings::remoteHostHandshakeTimeoutMs());
    hostConfig.ownership_policy = FSREMOTE_OWNERSHIP_SHARED; // wjy: 用户确认默认所有已认证控制端都可同时控制，目标端统一串行化键鼠输入。
    writeStartupLog(QStringLiteral("[wjy-main] before StreamRuntime::startHost maxSessions=%1 aggregateKbps=%2 handshakeMs=%3")
        .arg(hostConfig.max_sessions)
        .arg(hostConfig.max_aggregate_video_kbps)
        .arg(hostConfig.handshake_timeout_ms));
    FsRemoteStreamHandle hostHandle = stream::StreamRuntime::instance().startHost(49100, hostConfig); // wjy: 配置入口缺失时 StreamRuntime 会自动回退到旧版单会话启动函数。
    // ===end====
    writeStartupLog(QStringLiteral("[wjy-main] after StreamRuntime::startHost handle=%1").arg(hostHandle ? 1 : 0)); // wjy: Record whether the native stream host returned a valid handle.

    // =====wjy====
    platform::DeviceStatusServer statusServer([hostHandle] {
        platform::DeviceStatusServer::HostSessionSnapshot snapshot;
        if (!hostHandle) {
            return snapshot;
        }
        // wjy: 会话数 + 控制端设备名一并上报，驱动徽标与悬停气泡。
        snapshot.sessionCount = static_cast<int>(stream::StreamRuntime::instance().activeSessionCount(hostHandle));
        snapshot.controllerNames = stream::StreamRuntime::instance().activeControllerNames(hostHandle);
        return snapshot;
    });
    // ===end====
    writeStartupLog(QStringLiteral("[wjy-main] status server object created"));

    platform::DeviceCommandServer commandServer;
    writeStartupLog(QStringLiteral("[wjy-main] command server object created"));

    writeStartupLog(QStringLiteral("[wjy-main] before SSH server start")); // wjy: Start the bundled OpenSSH server for terminal/SFTP-related features.
    platform::PortableOpenSshManager::instance().startServer(); // wjy: Uses the openssh runtime under the application directory when present.
    writeStartupLog(QStringLiteral("[wjy-main] after SSH server start"));

    writeStartupLog(QStringLiteral("[wjy-main] before status server start")); // wjy: Start the online/busy status endpoint.
    statusServer.start(49101); // wjy: Port 49101 is queried by other FSRemote clients during refresh.
    writeStartupLog(QStringLiteral("[wjy-main] after status server start"));

    writeStartupLog(QStringLiteral("[wjy-main] before command server start")); // wjy: Start the remote command endpoint.
    commandServer.start(49102); // wjy: Port 49102 handles shutdown, restart, and wake proxy commands.
    writeStartupLog(QStringLiteral("[wjy-main] after command server start"));

    // =====wjy====
    // wjy: 确保开机自启项存在，并统一写入带 --minimized 的命令行。
    if (!platform::StartupManager::isEnabled()) {
        platform::StartupManager::setEnabled(true);
    } else {
        platform::StartupManager::setEnabled(true);
    }

    writeStartupLog(QStringLiteral("[wjy-main] before MainWindow create"));
    ui::MainWindow window;

    // =====wjy====
    QObject::connect(&platform::UpdateService::instance(), &platform::UpdateService::updateReadyToQuit,
        &app, [&app, &window] {
            if (auto* deviceGrid = qobject_cast<ui::DeviceGrid*>(window.centralWidget())) {
                deviceGrid->prepareForApplicationExit(); // wjy: 更新器等待前先取消后台任务并同步关闭所有远控窗口，让进程退出时间变得有界。
            }
            QTimer::singleShot(0, &app, &QCoreApplication::quit); // wjy: 延迟到事件循环执行退出，让启动阶段发现更新时也能进入既有的服务有序关闭流程。
        });
    // ===end====
    writeStartupLog(QStringLiteral("[wjy-main] after MainWindow create"));

    // =====wjy====
    QObject::connect(&singleInstanceServer, &QLocalServer::newConnection, &window, [&] {
        while (singleInstanceServer.hasPendingConnections()) {
            QLocalSocket* client = singleInstanceServer.nextPendingConnection();
            if (!client) continue;
            QObject::connect(client, &QLocalSocket::readyRead, &window, [client, &window] {
                client->readAll();
                window.showFromTray(); // wjy: 二次启动请求唤醒已有窗口。
                client->disconnectFromServer();
                client->deleteLater();
            });
            QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
        }
    });

    // wjy: 启动先轻量检查共享目录更新，再决定是否直接进托盘。
    platform::UpdateService::instance().checkNow(&window, false);
    platform::UpdateService::instance().startPeriodicCheck(&window);

    const QStringList args = QCoreApplication::arguments();
    // =====wjy====
    const int updatedToIndex = args.indexOf(QStringLiteral("--updated-to"));
    const int rollbackIndex = args.indexOf(QStringLiteral("--update-rollback"));
    if (updatedToIndex >= 0 && updatedToIndex + 1 < args.size()) {
        QTimer::singleShot(0, &window, [&window, version = args.at(updatedToIndex + 1)] {
            QMessageBox::information(&window, QString(), QString::fromUtf8("已成功更新到 v%1。").arg(version)); // wjy: 自动重启后向用户确认新版本已实际安装完成。
        });
    } else if (rollbackIndex >= 0) {
        QTimer::singleShot(0, &window, [&window] {
            QMessageBox::warning(&window, QString(), QString::fromUtf8("更新安装失败，已恢复并重新启动原版本。")); // wjy: 回滚重启后明确反馈，避免用户误以为已升级。
        });
    }
    // ===end====
    // wjy: 仅开机自启带 --minimized 时进托盘；手动双击启动仍显示主窗口。
    if (args.contains(QStringLiteral("--minimized"), Qt::CaseInsensitive)) {
        window.hideToTray();
        writeStartupLog(QStringLiteral("[wjy-main] started minimized to tray"));
    } else {
        window.show();
        writeStartupLog(QStringLiteral("[wjy-main] window shown before app.exec"));
    }
    // ===end====

    const int result = app.exec();
    writeStartupLog(QStringLiteral("[wjy-main] app.exec returned"));
    platform::UpdateService::instance().stopPeriodicCheck();
    singleInstanceServer.close();
    QLocalServer::removeServer(QString::fromLatin1(kSingleInstanceKey));

    platform::PowerManager::setPreventSleepEnabled(false);
    writeStartupLog(QStringLiteral("[wjy-main] prevent sleep disabled"));

    writeStartupLog(QStringLiteral("[wjy-main] before SSH server stop")); // wjy: Stop OpenSSH before application teardown so no sshd process is left behind.
    platform::PortableOpenSshManager::instance().stopServer(); // wjy: Explicit stop keeps shutdown order deterministic.
    writeStartupLog(QStringLiteral("[wjy-main] after SSH server stop"));

    writeStartupLog(QStringLiteral("[wjy-main] before command server stop")); // wjy: Stop the command TCP server before Qt object teardown.
    commandServer.stop(); // wjy: Releases the 49102 listener synchronously.
    writeStartupLog(QStringLiteral("[wjy-main] after command server stop"));

    writeStartupLog(QStringLiteral("[wjy-main] before status server stop")); // wjy: Stop the status TCP server before the stream host is released.
    statusServer.stop(); // wjy: Releases the 49101 listener synchronously.
    writeStartupLog(QStringLiteral("[wjy-main] after status server stop"));

    if (hostHandle) {
        stream::StreamRuntime::instance().stop(hostHandle); // wjy: Stop the native desktop stream host after all listeners are closed.
    }
    writeStartupLog(QStringLiteral("[wjy-main] stream host stopped"));

    return result;
}
