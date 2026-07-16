#include "ui/MainWindow.h"
#include "ui/DeviceGrid.h"

#include "stream/StreamRuntime.h"
#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "system/DeviceRealtimeStateService.h"
#include "system/DeviceStatusService.h"
#include "system/ParsecVddInstaller.h"
#include "system/PowerManager.h"
#include "system/PortableOpenSshManager.h"
#include "system/StartupManager.h"
#include "system/UpdateService.h"
#include "system/WjyDiagnosticLog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFont>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <utility>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

void waitForRestartParentIfRequested()
{
    const QStringList arguments = QCoreApplication::arguments();
    const int pidArgumentIndex = arguments.indexOf(QStringLiteral("--restart-after-pid"));
    if (pidArgumentIndex < 0 || pidArgumentIndex + 1 >= arguments.size()) {
        return;
    }

    bool validPid = false;
    const qint64 parentPid = arguments.at(pidArgumentIndex + 1).toLongLong(&validPid);
    if (!validPid || parentPid <= 0 || parentPid == QCoreApplication::applicationPid()) {
        return; // wjy: 参数异常时跳过等待，后续仍由原有单实例逻辑保证不会重复运行。
    }

#if defined(Q_OS_WIN)
    const HANDLE parentProcess = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parentPid));
    if (!parentProcess) {
        return; // wjy: 旧进程已经退出时直接继续启动，不额外延迟。
    }
    writeStartupLog(QStringLiteral("[wjy-restart] waiting for parent pid=%1").arg(parentPid));
    ::WaitForSingleObject(parentProcess, 15000); // wjy: 正常清理最多 8 秒，15 秒上限同时避免异常 PID 导致新实例永久等待。
    ::CloseHandle(parentProcess);
#else
    Q_UNUSED(parentPid)
#endif
}

platform::DeviceRealtimeScriptRuntime currentRealtimeScriptRuntime()
{
    const platform::RemoteScriptRuntimeInfo runtime = platform::DeviceStatusService::localScriptRuntime();
    platform::DeviceRealtimeScriptRuntime realtime;
    if (!runtime.supported || !runtime.statusKnown) {
        realtime.state = platform::RealtimeScriptState::Unknown;
    } else {
        realtime.state = runtime.running
            ? platform::RealtimeScriptState::Running
            : platform::RealtimeScriptState::Idle;
    }
    realtime.runId = runtime.runId;
    realtime.workName = runtime.workName;
    realtime.scriptName = runtime.scriptName;
    realtime.controllerPid = runtime.controllerPid;
    realtime.startedAtEpochMs = runtime.startedAtEpochMs;
    return realtime; // wjy: 广播只转换已验证结果，不根据控制端本地脚本 UI 缓存推测目标状态。
}

platform::DeviceRealtimeLocalState currentRealtimeLocalState(FsRemoteStreamHandle hostHandle)
{
    platform::DeviceRealtimeLocalState state;
    state.script = currentRealtimeScriptRuntime();
    if (!hostHandle) {
        return state;
    }

    const int activeCount = qBound(0,
        static_cast<int>(stream::StreamRuntime::instance().activeSessionCount(hostHandle)),
        10);
    QStringList details = stream::StreamRuntime::instance()
        .activeControllerDetails(hostHandle)
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    std::sort(details.begin(), details.end(), [](const QString& left, const QString& right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });

    for (int index = 0; index < activeCount; ++index) {
        const QString detail = details.value(index).trimmed();
        const int separator = detail.indexOf(QLatin1Char('\t'));
        platform::DeviceRealtimeHostSession session;
        session.controllerName = (separator < 0 ? detail : detail.left(separator)).trimmed();
        session.sourceIp = (separator < 0 ? QString() : detail.mid(separator + 1)).trimmed();
        const QByteArray identityText = QStringLiteral("%1|%2|%3")
            .arg(session.controllerName, session.sourceIp)
            .arg(index)
            .toUtf8();
        session.sessionId = QStringLiteral("host-%1")
            .arg(QString::fromLatin1(QCryptographicHash::hash(identityText, QCryptographicHash::Sha256).toHex())); // wjy: 原生导出暂无会话 ID 时，用当前详情和槽位生成快照内唯一 ID；真实人数仍完全由主机会话表决定。
        state.hostSessions.append(std::move(session));
    }
    return state;
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
    writeStartupLog(QStringLiteral("[wjy-main] executable=%1 args=%2")
        .arg(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().join(QStringLiteral(" | ")))); // wjy: 日志明确记录实际运行路径，优先排除用户仍启动旧目录 EXE 的情况。

    // =====wjy====
    waitForRestartParentIfRequested(); // wjy: 托盘重启产生的新实例必须先等旧进程释放单实例服务、端口和 DLL 资源。
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
        // wjy: 会话数 + 控制端设备名/IP 一并上报，驱动徽标与悬停气泡。
        snapshot.sessionCount = static_cast<int>(stream::StreamRuntime::instance().activeSessionCount(hostHandle));
        const QString controllerDetails = stream::StreamRuntime::instance().activeControllerDetails(hostHandle);
        QStringList controllerLabels;
        for (const QString& detail : controllerDetails.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const int separator = detail.indexOf(QLatin1Char('\t'));
            QString deviceName = (separator < 0 ? detail : detail.left(separator)).trimmed();
            const QString deviceIp = (separator < 0 ? QString() : detail.mid(separator + 1)).trimmed();
            deviceName.replace(QLatin1Char(','), QLatin1Char(' ')); // wjy: 状态字段仍以逗号分隔多台控制端，设备名自身的逗号必须先替换以免气泡错误换行。
            if (deviceName.isEmpty()) {
                deviceName = QString::fromUtf8("未知设备");
            }
            controllerLabels.append(deviceIp.isEmpty()
                ? deviceName
                : QString::fromUtf8("%1  IP：%2").arg(deviceName, deviceIp)); // wjy: 气泡每行在控制设备名后追加来源 IP，便于区分同名设备。
        }
        snapshot.controllerNames = controllerLabels.isEmpty()
            ? stream::StreamRuntime::instance().activeControllerNames(hostHandle)
            : controllerLabels.join(QLatin1Char(',')); // wjy: 旧 DLL 没有详情导出时继续回退纯设备名，保证混合版本仍能显示气泡。
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

    // =====wjy====
    platform::DeviceRealtimeStateService realtimeStateService;
    writeStartupLog(QStringLiteral("[wjy-main] before realtime state service start"));
    const bool realtimeStateStarted = realtimeStateService.start([hostHandle] {
        return currentRealtimeLocalState(hostHandle); // wjy: 主机会话和脚本进程只在本机读取，网络层始终发送一份完整权威快照。
    });
    writeStartupLog(QStringLiteral("[wjy-main] after realtime state service start ok=%1").arg(realtimeStateStarted ? 1 : 0));
    // ===end====

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
    ui::MainWindow window(&realtimeStateService);

    // =====wjy====
    QTimer remoteControllerOverlayTimer;
    const auto refreshRemoteControllerOverlay = [&window, hostHandle] {
        QStringList controllers;
        if (hostHandle) {
            const QString details = stream::StreamRuntime::instance().activeControllerDetails(hostHandle);
            controllers = details.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            if (controllers.isEmpty()
                && stream::StreamRuntime::instance().activeSessionCount(hostHandle) > 0) {
                const QString names = stream::StreamRuntime::instance().activeControllerNames(hostHandle);
                for (const QString& name : names.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                    controllers.append(name.trimmed() + QLatin1Char('\t') + QString::fromUtf8("未知")); // wjy: 旧 DLL 没有详情接口时仍显示设备名，IP 明确标为未知。
                }
            }
        }
        window.setRemoteControllerOverlayEntries(controllers); // wjy: 非空列表显示目标端提示，全部会话结束后自动隐藏。
    };
    QObject::connect(&remoteControllerOverlayTimer, &QTimer::timeout, &window, refreshRemoteControllerOverlay);
    remoteControllerOverlayTimer.setInterval(600); // wjy: 600ms 足够及时反映连接变化，同时避免高频锁定 DLL 会话表。
    remoteControllerOverlayTimer.start();
    refreshRemoteControllerOverlay();
    // ===end====

    // =====wjy====
    QObject::connect(&platform::UpdateService::instance(), &platform::UpdateService::updateReadyToQuit,
        &app, [&window] {
            window.requestApplicationExit(); // wjy: 更新退出恢复为本机直接退出；控制端仅在设备右键菜单主动更新时维护远控等待窗口。
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
    // =====wjy====
    platform::UpdateService::instance().checkNow(); // wjy: 启动时静默检查一次，只控制标题栏更新按钮是否出现。
    platform::UpdateService::instance().startPeriodicCheck(); // wjy: 自动检查固定启用，不再受已移除的设置开关影响，也不会自动安装。
    // ===end====

    const QStringList args = QCoreApplication::arguments();
    // =====wjy====
    const int rollbackIndex = args.indexOf(QStringLiteral("--update-rollback"));
    // wjy: 更新成功后静默进入新版本，不再显示“已成功更新”弹窗；失败回滚仍保留明确警告。
    if (rollbackIndex >= 0) {
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

    // =====wjy====
    writeStartupLog(QStringLiteral("[wjy-main] before SSH client process stop"));
    platform::PortableOpenSshManager::instance().stopClientProcesses(); // wjy: main 退出路径再次兜底，确保绕过 DeviceGrid 清理时也不会残留终端 cmd/ssh 进程树。
    writeStartupLog(QStringLiteral("[wjy-main] after SSH client process stop"));
    // ===end====
    writeStartupLog(QStringLiteral("[wjy-main] before SSH server stop")); // wjy: Stop OpenSSH before application teardown so no sshd process is left behind.
    platform::PortableOpenSshManager::instance().stopServer(); // wjy: Explicit stop keeps shutdown order deterministic.
    writeStartupLog(QStringLiteral("[wjy-main] after SSH server stop"));

    writeStartupLog(QStringLiteral("[wjy-main] before command server stop")); // wjy: Stop the command TCP server before Qt object teardown.
    commandServer.stop(); // wjy: Releases the 49102 listener synchronously.
    writeStartupLog(QStringLiteral("[wjy-main] after command server stop"));

    writeStartupLog(QStringLiteral("[wjy-main] before status server stop")); // wjy: Stop the status TCP server before the stream host is released.
    statusServer.stop(); // wjy: Releases the 49101 listener synchronously.
    writeStartupLog(QStringLiteral("[wjy-main] after status server stop"));

    // =====wjy====
    writeStartupLog(QStringLiteral("[wjy-main] before realtime state service stop"));
    realtimeStateService.stop(); // wjy: 先停止本机状态采样、补发、心跳和 UDP 接收，再释放其依赖的原生流主机会话表。
    writeStartupLog(QStringLiteral("[wjy-main] after realtime state service stop"));
    // ===end====

    if (hostHandle) {
        stream::StreamRuntime::instance().stop(hostHandle); // wjy: Stop the native desktop stream host after all listeners are closed.
    }
    writeStartupLog(QStringLiteral("[wjy-main] stream host stopped"));

    return result;
}
