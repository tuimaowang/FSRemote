#include "system/DeviceStatusService.h"
#include "system/DeviceInfoService.h"
#include "system/PortableOpenSshManager.h"
#include "system/WjyDiagnosticLog.h"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>

// =====wjy====
#include <iterator> // wjy: 提供 std::size，安全计算 Windows 进程映像路径缓冲区长度。
#include <limits> // wjy: 校验 qint64 PID 是否能安全转换成 Windows DWORD。
// ===end====

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

// =====wjy====
void writeStatusServerLog(const QString& message)
{
    writeWjyDiagnosticLog(message); // wjy: 状态服务关闭阶段写入统一诊断日志，用来确认 main 返回后的析构链路。
}
// ===end====

// =====wjy====
constexpr auto kActiveScriptManifestName = "fsremote_active_script.json"; // wjy: 所有脚本共用一份目标端活动清单，明确当前设备只允许一个活动脚本。
constexpr auto kLegacyScriptPidName = "fsremote_script_controller_pid.txt"; // wjy: 兼容升级前已经启动、只写 work 子目录 PID 文件的脚本。

enum class ScriptProcessState {
    Running,
    Stopped,
    Unknown,
};

QString scriptWorkRootPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("work")); // wjy: 状态服务在目标机本地读取 FSRemote.exe 同目录下的 work，不通过控制端猜路径。
}

qint64 pidFromMarkerText(const QByteArray& markerText)
{
    const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("\\d+")).match(QString::fromUtf8(markerText)); // wjy: 同时兼容“PID: 123”和只写数字的旧 PID 文件格式。
    return match.hasMatch() ? match.captured(0).toLongLong() : 0;
}

ScriptProcessState validateScriptControllerProcess(qint64 pid, qint64 recordedStartMs)
{
#if defined(Q_OS_WIN)
    if (pid <= 0 || pid > static_cast<qint64>(std::numeric_limits<DWORD>::max())) {
        return ScriptProcessState::Stopped; // wjy: 无效 PID 不能作为运行依据，后续会按过期标记清理。
    }

    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)); // wjy: 只申请查询和等待权限，不向目标脚本进程写入任何数据。
    if (!process) {
        return GetLastError() == ERROR_INVALID_PARAMETER
            ? ScriptProcessState::Stopped
            : ScriptProcessState::Unknown; // wjy: PID 不存在可确认停止；访问被拒绝等情况保守返回未知，避免重复启动脚本。
    }

    const DWORD waitResult = WaitForSingleObject(process, 0);
    if (waitResult == WAIT_OBJECT_0) {
        CloseHandle(process);
        return ScriptProcessState::Stopped; // wjy: 进程句柄已发出结束信号，说明 PID 文件只是残留。
    }
    if (waitResult != WAIT_TIMEOUT) {
        CloseHandle(process);
        return ScriptProcessState::Unknown; // wjy: 无法确定等待结果时不把目标误判为空闲。
    }

    wchar_t imagePath[32768] = {};
    DWORD imagePathLength = static_cast<DWORD>(std::size(imagePath));
    if (!QueryFullProcessImageNameW(process, 0, imagePath, &imagePathLength)) {
        CloseHandle(process);
        return ScriptProcessState::Unknown; // wjy: 活跃 PID 无法读取映像名时保留清单并报告未知。
    }
    const QString imageName = QFileInfo(QString::fromWCharArray(imagePath, static_cast<int>(imagePathLength))).fileName();
    if (imageName.compare(QStringLiteral("powershell.exe"), Qt::CaseInsensitive) != 0
        && imageName.compare(QStringLiteral("pwsh.exe"), Qt::CaseInsensitive) != 0) {
        CloseHandle(process);
        return ScriptProcessState::Stopped; // wjy: PID 已被其它程序复用时视为旧清单失效，绝不显示错误运行图标。
    }

    if (recordedStartMs > 0) {
        FILETIME creationTime = {};
        FILETIME exitTime = {};
        FILETIME kernelTime = {};
        FILETIME userTime = {};
        if (!GetProcessTimes(process, &creationTime, &exitTime, &kernelTime, &userTime)) {
            CloseHandle(process);
            return ScriptProcessState::Unknown; // wjy: 新版清单要求核对进程创建时间，读取失败时不能确认它就是本次任务。
        }
        ULARGE_INTEGER creationTicks = {};
        creationTicks.LowPart = creationTime.dwLowDateTime;
        creationTicks.HighPart = creationTime.dwHighDateTime;
        constexpr quint64 kWindowsToUnixEpochMs = 11644473600000ULL;
        const qint64 creationEpochMs = static_cast<qint64>(creationTicks.QuadPart / 10000ULL - kWindowsToUnixEpochMs);
        const qint64 manifestDelayMs = recordedStartMs - creationEpochMs;
        if (manifestDelayMs < -2000 || manifestDelayMs > 120000) {
            CloseHandle(process);
            return ScriptProcessState::Stopped; // wjy: 清单时间和进程创建时间不匹配说明 PID 已复用或清单已过期。
        }
    }

    CloseHandle(process);
    return ScriptProcessState::Running; // wjy: PID 活跃、映像为 PowerShell 且时间匹配后才能确认脚本仍在运行。
#else
    Q_UNUSED(pid);
    Q_UNUSED(recordedStartMs);
    return ScriptProcessState::Unknown; // wjy: 当前产品目标为 Windows；其它平台没有可靠校验实现时保持未知。
#endif
}

QByteArray withoutUtf8Bom(QByteArray data)
{
    if (data.startsWith("\xEF\xBB\xBF")) {
        data.remove(0, 3); // wjy: Windows PowerShell 5 的 UTF8 Set-Content 会写 BOM，解析 JSON 前显式移除。
    }
    return data;
}

RemoteScriptRuntimeInfo localScriptRuntimeInfo()
{
    RemoteScriptRuntimeInfo runtime;
    runtime.supported = true; // wjy: 新版目标端始终声明支持脚本状态字段，即便当前 work 目录不存在。

    const QString workRootPath = scriptWorkRootPath();
    const QString manifestPath = QDir(workRootPath).filePath(QString::fromLatin1(kActiveScriptManifestName));
    QFile manifestFile(manifestPath);
    if (manifestFile.exists()) {
        if (!manifestFile.open(QIODevice::ReadOnly)) {
            return runtime; // wjy: 活动清单存在但无法读取时 statusKnown 保持 false，控制端会阻止重复执行。
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(withoutUtf8Bom(manifestFile.readAll()), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            return runtime; // wjy: 损坏清单不能等同空闲，保守报告未知并保留现场便于排查。
        }

        const QJsonObject object = document.object();
        runtime.runId = object.value(QStringLiteral("runId")).toString().trimmed();
        runtime.workName = object.value(QStringLiteral("workName")).toString().trimmed();
        runtime.scriptName = object.value(QStringLiteral("scriptName")).toString().trimmed();
        runtime.controllerPid = object.value(QStringLiteral("controllerPid")).toVariant().toLongLong();
        runtime.startedAtEpochMs = object.value(QStringLiteral("startedAtEpochMs")).toVariant().toLongLong();
        if (object.value(QStringLiteral("state")).toString() != QStringLiteral("running")
            || runtime.workName.isEmpty()
            || runtime.controllerPid <= 0) {
            return runtime; // wjy: 必填字段缺失时不能安全恢复停止路径，因此保持未知而不是伪造运行状态。
        }

        const ScriptProcessState processState = validateScriptControllerProcess(runtime.controllerPid, runtime.startedAtEpochMs);
        if (processState == ScriptProcessState::Running) {
            runtime.statusKnown = true;
            runtime.running = true; // wjy: 新版清单通过进程校验后，把完整运行元数据返回给控制端。
            return runtime;
        }
        if (processState == ScriptProcessState::Unknown) {
            return runtime; // wjy: 进程检查权限或系统调用异常时保留活动清单，让下一次刷新继续确认。
        }
        manifestFile.close();
        QFile::remove(manifestPath); // wjy: 已确认 PID 不存在或被复用后清除目标根目录的过期活动清单。
    }

    bool foundUnknownLegacyMarker = false;
    const QDir workRoot(workRootPath);
    const QFileInfoList workDirectories = workRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& workDirectory : workDirectories) {
        const QString pidPath = QDir(workDirectory.absoluteFilePath()).filePath(QString::fromLatin1(kLegacyScriptPidName));
        QFile pidFile(pidPath);
        if (!pidFile.exists()) {
            continue;
        }
        if (!pidFile.open(QIODevice::ReadOnly)) {
            foundUnknownLegacyMarker = true; // wjy: 旧 PID 文件存在但读不到时保守记为未知，不能继续启动第二份脚本。
            continue;
        }
        const qint64 legacyPid = pidFromMarkerText(pidFile.readAll());
        pidFile.close();
        if (legacyPid <= 0) {
            foundUnknownLegacyMarker = true; // wjy: 旧标记内容损坏时保留文件和未知状态，避免无依据删除运行记录。
            continue;
        }

        const ScriptProcessState processState = validateScriptControllerProcess(legacyPid, 0);
        if (processState == ScriptProcessState::Running) {
            runtime.statusKnown = true;
            runtime.running = true;
            runtime.workName = workDirectory.fileName();
            runtime.scriptName = workDirectory.fileName();
            runtime.controllerPid = legacyPid; // wjy: 老版本没有 runId 和脚本名时至少恢复 work/PID，让图标与停止按钮仍可使用。
            return runtime;
        }
        if (processState == ScriptProcessState::Stopped) {
            QFile::remove(pidPath); // wjy: 旧 PID 已确认死亡时清理残留，后续刷新可以稳定得到空闲状态。
        } else {
            foundUnknownLegacyMarker = true;
        }
    }

    runtime.statusKnown = !foundUnknownLegacyMarker;
    runtime.running = false; // wjy: 只有不存在未确认标记时才把目标端状态确定为“未运行”。
    return runtime;
}

QByteArray encodedStatusText(const QString& text)
{
    return text.toUtf8().toBase64(); // wjy: 脚本名和 work 名可能包含中文或分隔符，Base64 后再放入竖线协议避免字段错位。
}

QString decodedStatusText(const QByteArray& text)
{
    return QString::fromUtf8(QByteArray::fromBase64(text)).trimmed(); // wjy: 控制端对新增文本字段做对称解码，旧协议字段保持原样。
}
// ===end====

QByteArray statusPayload(int remoteSessionCount)
{
    const int sessionCount = qBound(0, remoteSessionCount, 10); // wjy: 状态协议人数夹在 0-10，与设备行徽标档位一致。
    QByteArray payload = sessionCount > 0 ? QByteArrayLiteral("busy") : QByteArrayLiteral("online");
    const QString loginUser = PortableOpenSshManager::instance().loginUser();
    const DeviceInfo localInfo = DeviceInfoService::local();
    payload.append('|');
    payload.append(loginUser.toUtf8());
    payload.append('|');
    payload.append(localInfo.name.trimmed().toUtf8()); // wjy: 追加远端设备名，批量新增时用电脑名而不是登录账户名。
    payload.append('|');
    payload.append(localInfo.ip.trimmed().toUtf8());
    payload.append('|');
    payload.append(localInfo.subnetMask.trimmed().toUtf8());
    payload.append('|');
    payload.append(localInfo.broadcastIp.trimmed().toUtf8());
    payload.append('|');
    payload.append(localInfo.mac.trimmed().toUtf8());
    // =====wjy====
    const RemoteScriptRuntimeInfo scriptRuntime = localScriptRuntimeInfo(); // wjy: 每次状态连接都读取并验证目标机当前脚本进程，响应不是控制端缓存值。
    payload.append('|');
    payload.append(scriptRuntime.supported ? '1' : '0');
    payload.append('|');
    payload.append(scriptRuntime.statusKnown ? '1' : '0');
    payload.append('|');
    payload.append(scriptRuntime.running ? '1' : '0');
    payload.append('|');
    payload.append(scriptRuntime.runId.toUtf8());
    payload.append('|');
    payload.append(encodedStatusText(scriptRuntime.workName));
    payload.append('|');
    payload.append(encodedStatusText(scriptRuntime.scriptName));
    payload.append('|');
    payload.append(QByteArray::number(scriptRuntime.controllerPid));
    payload.append('|');
    payload.append(QByteArray::number(scriptRuntime.startedAtEpochMs)); // wjy: 新字段全部追加在旧 MAC 字段之后，旧控制端会自然忽略，保持协议向后兼容。
    payload.append('|');
    payload.append(QByteArray::number(sessionCount)); // wjy: 第 16 字段为远控会话数；旧控制端忽略，新控制端驱动数字徽标。
    // ===end====
    payload.append('\n');
    return payload;
}

DeviceStatusInfo queryStatusInfo(const QString& hostIp, uint16_t port, int timeoutMs)
{
    DeviceStatusInfo info;
    if (hostIp.trimmed().isEmpty()) {
        return info;
    }

    QTcpSocket socket;
    socket.connectToHost(hostIp.trimmed(), port);
    if (!socket.waitForConnected(timeoutMs)) {
        return info;
    }

    if (!socket.waitForReadyRead(timeoutMs)) {
        socket.disconnectFromHost();
        return info; // wjy: 能连上端口但没有状态响应时不能当在线，目标进程卡住或端口残留都会造成假在线。
    }

    const QByteArray payload = socket.readAll().trimmed();
    socket.disconnectFromHost();
    if (payload.isEmpty()) {
        return info; // wjy: 空响应没有确认目标 FSRemote 状态服务正常工作，保持默认离线。
    }
    const QList<QByteArray> parts = payload.split('|');
    const QByteArray statePart = parts.value(0).trimmed().toLower();
    if (statePart.startsWith("busy")) {
        info.state = DevicePresenceState::Busy;
    } else if (statePart.startsWith("online")) {
        info.state = DevicePresenceState::Online;
    } else {
        return info; // wjy: 未识别的状态文本不再兜底在线，避免非状态服务占用端口时误显示在线。
    }
    if (parts.size() > 1) {
        info.terminalUser = QString::fromUtf8(parts.at(1)).trimmed();
    }
    if (parts.size() > 6) {
        info.deviceName = QString::fromUtf8(parts.at(2)).trimmed(); // wjy: 新协议字段，保存远端电脑名。
        info.localIp = QString::fromUtf8(parts.at(3)).trimmed();
        info.subnetMask = QString::fromUtf8(parts.at(4)).trimmed();
        info.broadcastIp = QString::fromUtf8(parts.at(5)).trimmed();
        info.mac = QString::fromUtf8(parts.at(6)).trimmed();
        // =====wjy====
        if (parts.size() > 14 && parts.at(7).trimmed() == QByteArrayLiteral("1")) {
            info.scriptRuntime.supported = true;
            info.scriptRuntime.statusKnown = parts.at(8).trimmed() == QByteArrayLiteral("1");
            info.scriptRuntime.running = parts.at(9).trimmed() == QByteArrayLiteral("1");
            info.scriptRuntime.runId = QString::fromUtf8(parts.at(10)).trimmed();
            info.scriptRuntime.workName = decodedStatusText(parts.at(11));
            info.scriptRuntime.scriptName = decodedStatusText(parts.at(12));
            info.scriptRuntime.controllerPid = parts.at(13).trimmed().toLongLong();
            info.scriptRuntime.startedAtEpochMs = parts.at(14).trimmed().toLongLong(); // wjy: 仅在完整新版字段存在时声明支持，截断响应不会被误判为远端空闲。
        }
        // wjy: 远控会话数追加在脚本字段之后；缺失时 busy 回退为 1，online 为 0。
        if (parts.size() > 15) {
            bool ok = false;
            const int parsed = parts.at(15).trimmed().toInt(&ok);
            info.remoteSessionCount = ok ? qBound(0, parsed, 10) : (info.state == DevicePresenceState::Busy ? 1 : 0);
        } else if (info.state == DevicePresenceState::Busy) {
            info.remoteSessionCount = 1;
        }
        // ===end====
    } else {
        // =====wjy====
        if (info.state == DevicePresenceState::Busy) {
            info.remoteSessionCount = 1; // wjy: 极旧协议没有会话数字段时，busy 至少显示 1 路远控。
        }
        // ===end====
        if (parts.size() > 2) {
            info.localIp = QString::fromUtf8(parts.at(2)).trimmed(); // wjy: 兼容旧状态协议，旧客户端没有 deviceName 字段。
        }
        if (parts.size() > 3) {
            info.subnetMask = QString::fromUtf8(parts.at(3)).trimmed();
        }
        if (parts.size() > 4) {
            info.broadcastIp = QString::fromUtf8(parts.at(4)).trimmed();
        }
        if (parts.size() > 5) {
            info.mac = QString::fromUtf8(parts.at(5)).trimmed();
        }
    }
    return info;
}

} // namespace

DeviceStatusServer::DeviceStatusServer(std::function<int()> sessionCountProvider)
    : m_sessionCountProvider(std::move(sessionCountProvider))
{
}

DeviceStatusServer::~DeviceStatusServer()
{
    // =====wjy====
    writeStatusServerLog(QStringLiteral("[wjy-status-server] DeviceStatusServer dtor begin")); // wjy: 记录状态服务析构开始，确认命令服务析构之后是否继续走到这里。
    // ===end====
    stop();
    // =====wjy====
    writeStatusServerLog(QStringLiteral("[wjy-status-server] DeviceStatusServer dtor end")); // wjy: 记录状态服务析构结束。
    // ===end====
}

bool DeviceStatusServer::start(uint16_t port)
{
    if (m_server) {
        return true;
    }

    auto* server = new QTcpServer();
    QObject::connect(server, &QTcpServer::newConnection, server, [this, server] {
        while (server->hasPendingConnections()) {
            QTcpSocket* socket = server->nextPendingConnection();
            if (!socket) {
                continue;
            }

            const int sessionCount = m_sessionCountProvider ? m_sessionCountProvider() : 0;
            socket->write(statusPayload(sessionCount));
            socket->flush();
            QObject::connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
            socket->disconnectFromHost();
            if (socket->state() == QAbstractSocket::UnconnectedState) {
                socket->deleteLater();
            }
        }
    });

    if (!server->listen(QHostAddress::AnyIPv4, port)) {
        delete server;
        return false;
    }

    m_server = server;
    return true;
}

void DeviceStatusServer::stop()
{
    // =====wjy====
    writeStatusServerLog(QStringLiteral("[wjy-status-server] stop begin server=%1").arg(m_server ? 1 : 0)); // wjy: 记录状态服务 stop 是否还有 QTcpServer 需要释放。
    // ===end====
    if (!m_server) {
        // =====wjy====
        writeStatusServerLog(QStringLiteral("[wjy-status-server] stop skipped because server is null")); // wjy: server 已为空时说明前面已经停止过，不会重复删除。
        // ===end====
        return;
    }

    m_server->close();
    delete m_server;
    m_server = nullptr;
    // =====wjy====
    writeStatusServerLog(QStringLiteral("[wjy-status-server] stop end")); // wjy: 记录状态服务 QTcpServer 已释放。
    // ===end====
}

DevicePresenceState DeviceStatusService::probe(const QString& hostIp, uint16_t port, int timeoutMs)
{
    return queryStatusInfo(hostIp, port, timeoutMs).state;
}

QString DeviceStatusService::terminalUser(const QString& hostIp, uint16_t port, int timeoutMs)
{
    return queryStatusInfo(hostIp, port, timeoutMs).terminalUser;
}

DeviceStatusInfo DeviceStatusService::query(const QString& hostIp, uint16_t port, int timeoutMs)
{
    return queryStatusInfo(hostIp, port, timeoutMs);
}

} // namespace platform
