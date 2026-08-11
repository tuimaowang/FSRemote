#include "ui/MainWindow.h"
#include "ui/DeviceGrid.h"

#include "stream/StreamRuntime.h"
#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "system/DeviceRealtimeStateService.h"
#include "system/DeviceStatusService.h"
#include "system/InputScriptExecutionService.h" // wjy: 主进程持有目标端独立键鼠脚本执行器，并把状态接入现有实时广播生命周期。
#include "system/ParsecVddInstaller.h"
#include "system/PowerManager.h"
#include "system/PortableOpenSshManager.h"
#include "system/RuntimeLogManager.h" // wjy: 主实例确认后统一清理并定位 FSRemote.exe/data 下的运行日志。
#include "system/StartupManager.h"
#include "system/SharedStorageAvailabilityService.h"
#include "system/StartupPerformanceLog.h"
#include "system/UpdateService.h"
#include "system/WjyDiagnosticLog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
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
#include <tlhelp32.h>
#endif

namespace {

void writeStartupLog(const QString& message)
{
    platform::writeWjyDiagnosticLog(message); // wjy: Keep startup diagnostics behind one helper so logging can be changed centrally.
    platform::StartupPerformanceLog::checkpoint(message); // wjy: 启动观察窗口内额外记录本步骤和累计耗时，结束后自动停止写盘。
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

qint64 waitForRestartParentIfRequested()
{
    const QStringList arguments = QCoreApplication::arguments();
    const int pidArgumentIndex = arguments.indexOf(QStringLiteral("--restart-after-pid"));
    if (pidArgumentIndex < 0 || pidArgumentIndex + 1 >= arguments.size()) {
        return 0;
    }

    bool validPid = false;
    const qint64 parentPid = arguments.at(pidArgumentIndex + 1).toLongLong(&validPid);
    if (!validPid || parentPid <= 0 || parentPid == QCoreApplication::applicationPid()) {
        return 0; // wjy: 参数异常时跳过等待，后续仍由原有单实例逻辑保证不会重复运行。
    }

#if defined(Q_OS_WIN)
    const HANDLE parentProcess = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parentPid));
    if (!parentProcess) {
        return ::GetLastError() == ERROR_INVALID_PARAMETER
            ? parentPid
            : -parentPid; // wjy: PID 已消失才视为可继续；权限等其它错误不能冒险清理仍可能被旧进程占用的日志。
    }
    const DWORD waitResult = ::WaitForSingleObject(parentProcess, 15000); // wjy: 正常清理最多 8 秒，15 秒上限同时避免异常 PID 导致新实例永久等待。
    ::CloseHandle(parentProcess);
    if (waitResult != WAIT_OBJECT_0) {
        return -parentPid; // wjy: 超时或等待失败时不取得主实例身份，防止旧进程尚未关闭日志句柄就删除当前记录。
    }
#else
    Q_UNUSED(parentPid)
#endif
    return parentPid; // wjy: 等待阶段禁止提前打开旧日志；返回 PID 后由已完成清理的新主实例统一记录。
}

void cleanupFixedMachineNumberProcessesAtStartup()
{
#if defined(Q_OS_WIN)
    const QStringList targetProcessNames{
        QString::fromUtf8("机器号.exe"), // wjy: 目标主进程名是固定字面量，不根据当前计算机名拼接或替换。
        QString::fromUtf8("机器号_置顶.exe"), // wjy: 目标置顶辅助进程名同样固定，必须与进程列表中的文件名完全对应。
    };
    writeStartupLog(QStringLiteral("[wjy-startup-process] cleanup targets=%1")
        .arg(targetProcessNames.join(QStringLiteral(",")))); // wjy: 记录本次启动检查的固定目标名，便于确认清理规则没有发生动态替换。

    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        const DWORD errorCode = ::GetLastError();
        writeStartupLog(QStringLiteral("[wjy-startup-process] process snapshot failed error=%1").arg(errorCode)); // wjy: 枚举失败不影响 FSRemote 主程序继续启动。
        return;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    BOOL hasProcess = ::Process32FirstW(snapshot, &entry);
    while (hasProcess) {
        const DWORD processId = entry.th32ProcessID;
        const QString processName = QString::fromWCharArray(entry.szExeFile); // wjy: 使用 Windows 提供的进程映像文件名，避免模糊命令行匹配误伤其它程序。
        if (processId != 0
            && processId != ::GetCurrentProcessId()
            && targetProcessNames.contains(processName, Qt::CaseInsensitive)) {
            HANDLE process = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId);
            if (!process) {
                const DWORD errorCode = ::GetLastError();
                writeStartupLog(QStringLiteral("[wjy-startup-process] open failed name=%1 pid=%2 error=%3")
                    .arg(processName)
                    .arg(processId)
                    .arg(errorCode)); // wjy: 权限不足或进程竞态只记录，不能因为辅助程序清理失败阻断主程序。
            } else {
                if (::TerminateProcess(process, ERROR_PROCESS_ABORTED)) {
                    const DWORD waitResult = ::WaitForSingleObject(process, 2000); // wjy: 等待最多 2 秒确认结束，避免启动后仍与旧进程并行运行。
                    writeStartupLog(QStringLiteral("[wjy-startup-process] terminated name=%1 pid=%2 wait=%3")
                        .arg(processName)
                        .arg(processId)
                        .arg(waitResult)); // wjy: 记录终止结果，便于确认目标是否真正进入退出态。
                } else {
                    const DWORD errorCode = ::GetLastError();
                    writeStartupLog(QStringLiteral("[wjy-startup-process] terminate failed name=%1 pid=%2 error=%3")
                        .arg(processName)
                        .arg(processId)
                        .arg(errorCode)); // wjy: TerminateProcess 失败时释放句柄并继续检查另一个目标。
                }
                ::CloseHandle(process);
            }
        }
        hasProcess = ::Process32NextW(snapshot, &entry);
    }

    const DWORD enumerationError = ::GetLastError();
    ::CloseHandle(snapshot);
    if (enumerationError != ERROR_NO_MORE_FILES) {
        writeStartupLog(QStringLiteral("[wjy-startup-process] process enumeration ended error=%1").arg(enumerationError)); // wjy: 记录遍历中途异常，但不让启动流程失败。
    }
#else
    writeStartupLog(QStringLiteral("[wjy-startup-process] non-Windows build, skip fixed process cleanup")); // wjy: 非 Windows 构建没有对应的进程枚举和终止 API，不伪装清理成功。
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

platform::DeviceRealtimeLocalState currentRealtimeLocalState(
    FsRemoteStreamHandle hostHandle,
    const platform::DeviceRealtimeUpdateState& updateState)
{
    platform::DeviceRealtimeLocalState state;
    state.script = currentRealtimeScriptRuntime();
    state.inputScript = platform::InputScriptExecutionService::instance().snapshot(); // wjy: F9/F10 状态始终取被控端独立执行器快照，Viewer 是否存在不影响广播结果。
    state.update = updateState;
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
    QElapsedTimer applicationCreationTimer;
    applicationCreationTimer.start(); // wjy: 性能日志依赖 QApplication 获取可执行目录，因此用独立计时器补记 QApplication 构造耗时。
    QApplication app(argc, argv);
    const qint64 qApplicationCreationMs = applicationCreationTimer.elapsed(); // wjy: 单实例确认前只保留内存计时，禁止二次启动清空或续写正在运行实例的日志。

    QApplication::setApplicationName(QStringLiteral("FSRemote"));
    QApplication::setOrganizationName(QStringLiteral("FSRemote"));

    // =====wjy====
    const qint64 restartParentPid = waitForRestartParentIfRequested(); // wjy: 托盘或更新器重启先等待旧进程释放文件句柄，但此阶段不能触碰上一轮日志。
    // wjy: 禁止重复启动；二次启动只唤醒已有主窗口。
    if (activateExistingInstance()) {
        return 0; // wjy: 二次启动不写文件也不清日志，避免干扰真正主实例的完整运行记录。
    }
    if (restartParentPid < 0) {
        return 1; // wjy: 指定父进程未确认退出且无法激活时安全终止，新进程不能接管端口或清理 data。
    }
    QLocalServer::removeServer(QString::fromLatin1(kSingleInstanceKey));
    QLocalServer singleInstanceServer;
    if (!singleInstanceServer.listen(QString::fromLatin1(kSingleInstanceKey))) {
        return 1; // wjy: 未取得单实例服务时不能继续启动，更不能删除可能属于其它实例的日志。
    }

    QElapsedTimer logResetTimer;
    logResetTimer.start();
    const platform::RuntimeLogResetResult logReset =
        platform::RuntimeLogManager::resetForPrimaryProcessStart(); // wjy: 仅成功成为主实例后删除上一轮日志，并清理旧版 Temp/AppData/work 路径。
    const qint64 logResetMs = logResetTimer.elapsed();
    writeStartupLog(QStringLiteral("[wjy-main] app created qapplication_ms=%1")
        .arg(qApplicationCreationMs)); // wjy: 新日志的第一条记录仍保留 QApplication 构造耗时，但落盘发生在旧日志完成清理之后。
    writeStartupLog(QStringLiteral("[wjy-main] app metadata set"));
    writeStartupLog(QStringLiteral("[wjy-main] executable=%1 args=%2")
        .arg(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().join(QStringLiteral(" | ")))); // wjy: 日志明确记录实际运行路径，优先排除用户仍启动旧目录 EXE 的情况。
    if (restartParentPid > 0) {
        writeStartupLog(QStringLiteral("[wjy-restart] parent wait completed pid=%1").arg(restartParentPid)); // wjy: 更新器或旧主进程已经退出，当前日志不会再与上一进程共享文件句柄。
    }
    writeStartupLog(QStringLiteral("[wjy-main] runtime logs reset data=%1 ready=%2 removed=%3 failed=%4 elapsed_ms=%5")
        .arg(platform::RuntimeLogManager::dataDirectory())
        .arg(logReset.dataDirectoryReady ? 1 : 0)
        .arg(logReset.removedFileCount)
        .arg(logReset.failedFileCount)
        .arg(logResetMs)); // wjy: 启动记录明确显示统一目录、删除数量和失败数量，便于核对“每次重启重新记录”是否生效。
    cleanupFixedMachineNumberProcessesAtStartup(); // wjy: 单实例确认后立即清理固定名称的两个旧进程，再继续启动其它服务。
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
    platform::DeviceRealtimeUpdateState realtimeUpdateState;
    realtimeUpdateState.installedVersion = platform::UpdateService::displayVersion();
    realtimeUpdateState.runtimeRepairRequired = platform::UpdateService::runtimeDependenciesNeedRepair(
        QCoreApplication::applicationDirPath());

    platform::DeviceRealtimeStateService realtimeStateService;
    writeStartupLog(QStringLiteral("[wjy-main] before realtime state service start"));
    const bool realtimeStateStarted = realtimeStateService.start([hostHandle, &realtimeUpdateState] {
        return currentRealtimeLocalState(hostHandle, realtimeUpdateState); // wjy: 主机会话、脚本和版本状态都从本机权威来源组成一份完整快照。
    });
    platform::InputScriptExecutionService::instance().setStatusChangedCallback(
        [&realtimeStateService] { realtimeStateService.notifyLocalStateChanged(); }); // wjy: 目标端准备、执行、轮间等待和结束都立即触发同一份UDP状态广播。
    writeStartupLog(QStringLiteral("[wjy-main] after realtime state service start ok=%1").arg(realtimeStateStarted ? 1 : 0));
    QObject::connect(&platform::UpdateService::instance(), &platform::UpdateService::updateAvailabilityChanged,
        &realtimeStateService, [&realtimeStateService, &realtimeUpdateState](bool, const QString&) {
            platform::DeviceRealtimeUpdateState refreshed;
            refreshed.installedVersion = platform::UpdateService::displayVersion();
            refreshed.runtimeRepairRequired = platform::UpdateService::runtimeDependenciesNeedRepair(
                QCoreApplication::applicationDirPath());
            if (refreshed == realtimeUpdateState) {
                return;
            }
            realtimeUpdateState = std::move(refreshed);
            realtimeStateService.notifyLocalStateChanged();
        });
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
    bool updateShutdownPrepared = false; // wjy: 更新退出只允许一个调用者执行服务和媒体预清理，重复信号不能二次释放 Host 句柄。
    QObject::connect(&platform::UpdateService::instance(), &platform::UpdateService::updateReadyToQuit,
        &app, [&] {
            writeStartupLog(QStringLiteral("[wjy-update-exit] updateReadyToQuit received")); // wjy: 这是主线程真正收到后台退出信号的第一条证据，可与 emit begin/returned 判断队列投递是否成功。
            if (updateShutdownPrepared) {
                writeStartupLog(QStringLiteral("[wjy-update-exit] duplicate signal ignored"));
                return;
            }
            updateShutdownPrepared = true;
            writeStartupLog(QStringLiteral("[wjy-update-exit] deterministic cleanup begin"));

            writeStartupLog(QStringLiteral("[wjy-update-exit] overlay timer stop begin"));
            remoteControllerOverlayTimer.stop(); // wjy: Host 句柄释放前停止控制端列表轮询，后续事件不再读取已关闭的原生会话表。
            writeStartupLog(QStringLiteral("[wjy-update-exit] overlay timer stop end"));
            writeStartupLog(QStringLiteral("[wjy-update-exit] command server stop begin"));
            commandServer.stop(); // wjy: 先关闭 49102 并汇合已经完成暂存的更新线程，禁止退出期间再受理第二个更新或电源命令。
            writeStartupLog(QStringLiteral("[wjy-update-exit] command server stop end"));
            writeStartupLog(QStringLiteral("[wjy-update-exit] input script execution stop begin"));
            platform::InputScriptExecutionService::instance().shutdown(); // wjy: 应用更新退出才停止目标端本地脚本；控制端窗口退出或断线不会进入此路径。
            writeStartupLog(QStringLiteral("[wjy-update-exit] input script execution stop end"));
            writeStartupLog(QStringLiteral("[wjy-update-exit] status server stop begin"));
            statusServer.stop(); // wjy: 关闭 49101 后控制端会把本机视为预期更新离线，不再从旧 Host 句柄生成 busy 快照。
            writeStartupLog(QStringLiteral("[wjy-update-exit] status server stop end"));
            writeStartupLog(QStringLiteral("[wjy-update-exit] realtime state service stop begin"));
            realtimeStateService.stop(); // wjy: 停止 UDP 心跳和会话采样，确保下面销毁 Host 时没有并发读取会话状态。
            writeStartupLog(QStringLiteral("[wjy-update-exit] realtime state service stop end"));
            if (hostHandle) {
                writeStartupLog(QStringLiteral("[wjy-update-exit] stream host stop begin"));
                stream::StreamRuntime::instance().stop(hostHandle); // wjy: 在 UI 析构前主动关闭监听、WebRTC 会话、采集编码管线和 FakerInput Bridge 客户端。
                hostHandle = nullptr; // wjy: 主退出尾声据此跳过重复 stop，防止对已经释放的原生句柄二次调用。
                writeStartupLog(QStringLiteral("[wjy-update-exit] stream host stop end"));
            }
            writeStartupLog(QStringLiteral("[wjy-update-exit] deterministic cleanup end"));
            writeStartupLog(QStringLiteral("[wjy-update-exit] requestApplicationExit begin"));
            window.requestApplicationExit(); // wjy: 关键媒体和服务资源确认释放后再进入窗口、后台任务、SSH 和 Qt 的统一退出路径。
            writeStartupLog(QStringLiteral("[wjy-update-exit] requestApplicationExit returned")); // wjy: 若主程序仍存活但缺少本行，DeviceGrid、SSH 或窗口退出准备就是最后阻塞区间。
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
    platform::UpdateService::instance().startPeriodicCheck(); // wjy: 启动时先异步探测 192.168.1.100:445，成功后才读取版本文件；离线时不阻塞主窗口。
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
    // wjy: 仅开机自启或更新重启带 --minimized 时静默进入托盘；手动双击启动仍显示主窗口。
    if (args.contains(QStringLiteral("--minimized"), Qt::CaseInsensitive)) {
        // =====wjy====
        window.hide(); // wjy: 静默启动时主窗口保持从未创建/从未显示状态，避免 Qt 的 showMinimized 路径在更新重启后把窗口重新弹到桌面。
        writeStartupLog(QStringLiteral("[wjy-main] started hidden to tray")); // wjy: 记录本次明确走隐藏托盘而非系统最小化，便于与用户手动最小化路径区分。
        // ===end====
    } else {
        window.show();
        writeStartupLog(QStringLiteral("[wjy-main] window shown before app.exec"));
    }
    // ===end====

    writeStartupLog(QStringLiteral("[wjy-main] before app.exec"));
    QTimer::singleShot(3000, &app, [] {
        platform::StartupPerformanceLog::finish(QStringLiteral("[wjy-main] startup observation window complete")); // wjy: 覆盖首帧、500ms 延迟本机信息和 1.2 秒网盘探测，三秒后关闭计时写盘。
    });
    const int result = app.exec();
    writeStartupLog(QStringLiteral("[wjy-main] app.exec returned"));
    platform::StartupPerformanceLog::finish(QStringLiteral("[wjy-main] startup observation ended by application exit")); // wjy: 三秒内提前退出时也明确封口并关闭日志文件。
    platform::UpdateService::instance().stopPeriodicCheck();
    platform::SharedStorageAvailabilityService::instance().stop(); // wjy: 事件循环结束后中止尚未完成的 SMB 端口探测，不留下晚到网络回调。
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

    // =====wjy====
    writeStartupLog(QStringLiteral("[wjy-main] before input script execution stop"));
    platform::InputScriptExecutionService::instance().shutdown(); // wjy: 被控端自身退出时汇合共享文件线程并释放脚本持有键鼠，普通主控退出不影响这里的运行状态。
    writeStartupLog(QStringLiteral("[wjy-main] after input script execution stop"));
    // ===end====

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
