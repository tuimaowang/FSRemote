#include "system/DeviceCommandService.h"

#include "system/DeviceInfoService.h"
#include "system/DeviceListSyncService.h"
#include "system/InputScriptExecutionService.h"
#include "system/PortableOpenSshManager.h"
#include "system/ScreenshotService.h"
#include "system/UpdateService.h"
#include "system/WakeOnLanSender.h"
#include "system/WjyDiagnosticLog.h"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QPointer>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread> // wjy: 远控密钥登记仅在后台线程对瞬态主动断开做一次短延迟重试。
#include <QTimer>
#include <QUrl>

#include <functional>
#include <string>
#include <thread>
#include <utility>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace platform {
namespace {

// =====wjy====
bool g_remoteUpdateScheduled = false; // wjy: 目标进程退出前只允许排队一个更新任务，避免连续点击启动多个独立更新器。
// ===end====

// =====wjy====
QString g_remoteUpdateFailure; // wjy: 暂存失败时保存可查询的错误，控制端无需一直等待到超时。
// ===end====

// =====wjy====
void writeCommandServerLog(const QString& message)
{
    writeWjyDiagnosticLog(message); // wjy: 命令服务关闭阶段写入统一诊断日志，用来确认 main 返回后的析构是否触发 Release 堆损坏。
}
// ===end====

// =====wjy====
bool configureLanTcpSocket(QTcpSocket& socket, const QString& hostIp, QString* errorMessage = nullptr)
{
    socket.setProxy(QNetworkProxy(QNetworkProxy::NoProxy)); // wjy: 设备命令只连接局域网目标，强制绕过 Clash、PAC 和 Qt 默认代理，避免原始 TCP 被无效代理类型拦截。
    const QString sourceIp = DeviceInfoService::physicalLanIpv4ForTarget(hostIp); // wjy: NoProxy 只绕过 Qt 代理，仍需选出真实物理 IPv4 才能绕过 Windows Meta/TUN 默认路由。
    if (sourceIp.isEmpty()) {
        return true; // wjy: 没有可识别物理地址时保留系统默认连接能力，兼容非 Windows 或特殊单网卡环境。
    }
    if (!socket.bind(QHostAddress(sourceIp), 0)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法绑定局域网源地址 %1：%2").arg(sourceIp, socket.errorString().trimmed()); // wjy: 绑定失败明确反馈，禁止静默回退到可能错误的 Meta 出口。
        }
        writeCommandServerLog(QStringLiteral("[wjy-command] bind failed source=%1 target=%2 error=%3")
            .arg(sourceIp, hostIp.trimmed(), socket.errorString().trimmed()));
        return false;
    }
    writeCommandServerLog(QStringLiteral("[wjy-command] bound source=%1 target=%2")
        .arg(sourceIp, hostIp.trimmed())); // wjy: 授权失败时日志直接显示实际源 IP，便于确认是否已避开 198.18.0.1。
    return true;
}
// ===end====

QByteArray actionCommand(DeviceControlAction action)
{
    switch (action) {
    case DeviceControlAction::Shutdown:
        return QByteArrayLiteral("shutdown\n");
    case DeviceControlAction::Restart:
        return QByteArrayLiteral("restart\n");
    default:
        return {};
    }
}

QByteArray wakeCommand(const QString& macAddress)
{
    const QString mac = macAddress.trimmed();
    if (mac.isEmpty()) {
        return {};
    }
    return QByteArrayLiteral("wake_mac|") + mac.toUtf8() + QByteArrayLiteral("\n");
}

// =====wjy====
QByteArray authorizeTerminalKeyCommand(const QString& publicKey)
{
    const QString key = publicKey.trimmed();
    if (key.isEmpty()) {
        return {}; // wjy: 没有本机 SSH 公钥时不发送授权命令，避免目标设备写入空行。
    }
    return QByteArrayLiteral("authorize_ssh_key|")
        + QUrl::toPercentEncoding(key)
        + QByteArrayLiteral("\n"); // wjy: 公钥中包含空格、斜杠和等号，放进 TCP 命令前统一做百分号编码。
}
// ===end====

QByteArray renameDeviceCommand(const QString& newName)
{
    const QString name = newName.trimmed();
    if (name.isEmpty()) {
        return {};
    }
    return QByteArrayLiteral("rename_device|")
        + QUrl::toPercentEncoding(name)
        + QByteArrayLiteral("\n"); // wjy: 设备名可能包含中文或空格，命令协议里先做百分号编码避免分隔符冲突。
}

bool renameLocalComputer(const QString& newName, QString* errorMessage)
{
    const QString name = newName.trimmed();
    if (name.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("设备名为空");
        }
        return false;
    }

#if defined(Q_OS_WIN)
    const std::wstring wideName = name.toStdWString();
    if (SetComputerNameExW(ComputerNamePhysicalDnsHostname, wideName.c_str())) {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true; // wjy: Windows 修改计算机名通常需要重启后完全生效，命令这里只负责写入目标系统设置。
    }
    if (errorMessage) {
        *errorMessage = QStringLiteral("修改目标设备名失败，错误码 %1").arg(GetLastError());
    }
    return false;
#else
    if (errorMessage) {
        *errorMessage = QStringLiteral("当前系统暂不支持远程修改设备名");
    }
    return false;
#endif
}

// =====wjy====
bool readSingleLineReply(
    QTcpSocket& socket,
    QByteArray* reply,
    QString* errorMessage,
    int timeoutMs,
    QAbstractSocket::SocketError* transportError = nullptr)
{
    if (reply) reply->clear();
    if (transportError) *transportError = QAbstractSocket::UnknownSocketError;

    QElapsedTimer replyTimer;
    replyTimer.start();
    QByteArray completeReply;
    while (true) {
        completeReply.append(socket.readAll()); // wjy: 每次等待前先排空Qt接收缓冲区，断开事件和数据同时到达时也不会漏掉最后一个包。
        const int newlineIndex = completeReply.indexOf('\n');
        if (newlineIndex >= 0) {
            if (reply) *reply = completeReply.left(newlineIndex).trimmed();
            if (errorMessage) errorMessage->clear();
            return true; // wjy: 协议只在收到完整换行结尾后成功，避免弱网分包导致只解析半条回复。
        }
        if (completeReply.size() > 64 * 1024) {
            if (errorMessage) *errorMessage = QStringLiteral("目标端回复超过允许长度。");
            return false;
        }

        const int remainingMs = timeoutMs - static_cast<int>(replyTimer.elapsed());
        if (remainingMs <= 0) {
            if (transportError) *transportError = QAbstractSocket::SocketTimeoutError;
            if (errorMessage) *errorMessage = QStringLiteral("等待目标端回复超时。");
            return false;
        }
        if (socket.waitForReadyRead(remainingMs)) {
            continue;
        }

        completeReply.append(socket.readAll()); // wjy: waitForReadyRead可能因RemoteHostClosed返回false，但目标端的ok行已经进入缓冲区，必须最后再读取一次。
        const int finalNewlineIndex = completeReply.indexOf('\n');
        if (finalNewlineIndex >= 0) {
            if (reply) *reply = completeReply.left(finalNewlineIndex).trimmed();
            if (errorMessage) errorMessage->clear();
            return true; // wjy: 接受“完整回复与主动断开同时到达”，避免把已经登记成功的远控密钥误报成失败。
        }

        QAbstractSocket::SocketError socketError = socket.error();
        if (socketError == QAbstractSocket::UnknownSocketError
            && socket.state() == QAbstractSocket::UnconnectedState) {
            socketError = QAbstractSocket::RemoteHostClosedError; // wjy: 某些Qt/Windows组合只留下断开状态，统一归类后授权入口仍能执行一次安全重试。
        }
        if (transportError) *transportError = socketError;
        if (errorMessage) {
            if (socketError == QAbstractSocket::RemoteHostClosedError
                || socket.state() == QAbstractSocket::UnconnectedState) {
                *errorMessage = QStringLiteral("目标端在完整回复前关闭了连接。"); // wjy: 确实没有完整回复时提供稳定中文原因，不再直接显示Qt英文错误文本。
            } else {
                const QString socketErrorText = socket.errorString().trimmed();
                *errorMessage = socketErrorText.isEmpty()
                    ? QStringLiteral("等待目标端回复失败。")
                    : socketErrorText;
            }
        }
        return false;
    }
}

bool sendCommandPayload(
    const QString& hostIp,
    const QByteArray& payload,
    QString* errorMessage,
    uint16_t port,
    int timeoutMs,
    QAbstractSocket::SocketError* transportError = nullptr)
{
    if (transportError) *transportError = QAbstractSocket::UnknownSocketError;
    if (hostIp.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("目标 IP 为空");
        }
        return false;
    }
    if (payload.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("命令为空");
        }
        return false;
    }

    QTcpSocket socket;
    if (!configureLanTcpSocket(socket, hostIp, errorMessage)) {
        if (transportError) *transportError = socket.error();
        return false; // wjy: 物理源地址绑定失败时不允许无绑定重试，避免命令再次被 Meta/TUN 截走。
    }
    socket.connectToHost(hostIp.trimmed(), port);
    if (!socket.waitForConnected(timeoutMs)) {
        if (transportError) *transportError = socket.error();
        if (errorMessage) {
            *errorMessage = socket.errorString().trimmed();
        }
        return false;
    }
    if (socket.write(payload) != payload.size()) {
        if (transportError) *transportError = socket.error();
        if (errorMessage) {
            *errorMessage = socket.errorString().trimmed();
        }
        socket.disconnectFromHost();
        return false;
    }
    if (!socket.waitForBytesWritten(timeoutMs)) {
        if (transportError) *transportError = socket.error();
        if (errorMessage) {
            *errorMessage = socket.errorString().trimmed();
        }
        socket.disconnectFromHost();
        return false;
    }
    QByteArray reply;
    if (!readSingleLineReply(socket, &reply, errorMessage, timeoutMs, transportError)) {
        socket.disconnectFromHost();
        return false;
    }

    socket.disconnectFromHost();
    const QList<QByteArray> parts = reply.split('|');
    const QByteArray status = parts.value(0).trimmed().toLower();
    if (status == "ok") {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }

    if (errorMessage) {
        *errorMessage = parts.size() > 1
            ? QUrl::fromPercentEncoding(parts.at(1)).trimmed()
            : QStringLiteral("远程命令执行失败");
    }
    return false;
}
// ===end====

// =====wjy====
bool sendCommandAndReadReply(
    const QString& hostIp,
    const QByteArray& payload,
    QByteArray* reply,
    QString* errorMessage,
    uint16_t port,
    int timeoutMs)
{
    if (hostIp.trimmed().isEmpty() || payload.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("目标地址或命令为空。");
        return false;
    }
    QTcpSocket socket;
    if (!configureLanTcpSocket(socket, hostIp, errorMessage)) {
        return false; // wjy: 截图和键鼠脚本命令与授权命令使用同一物理出口约束。
    }
    socket.connectToHost(hostIp.trimmed(), port);
    if (!socket.waitForConnected(timeoutMs)
        || socket.write(payload) != payload.size()
        || !socket.waitForBytesWritten(timeoutMs)) {
        if (errorMessage) *errorMessage = socket.errorString().trimmed();
        socket.disconnectFromHost();
        return false;
    }

    if (!readSingleLineReply(socket, reply, errorMessage, timeoutMs)) {
        socket.disconnectFromHost();
        return false; // wjy: 截图和键鼠脚本同样接受“完整回复后立即断开”，避免共享命令通道重复出现相同竞态。
    }
    socket.disconnectFromHost();
    return true; // wjy: F10和截图命令共用完整单行回复读取，等待只发生在调用方后台线程。
}

QByteArray inputScriptRuntimeJson(const RemoteInputScriptRuntimeInfo& runtime)
{
    QJsonObject object;
    object.insert(QStringLiteral("supported"), runtime.supported);
    object.insert(QStringLiteral("state"), remoteInputScriptStateName(runtime.state));
    object.insert(QStringLiteral("runId"), runtime.runId);
    object.insert(QStringLiteral("scriptName"), runtime.scriptName);
    object.insert(QStringLiteral("scriptHash"), runtime.scriptHash);
    object.insert(QStringLiteral("completedLoops"), runtime.completedLoops);
    object.insert(QStringLiteral("configuredLoops"), runtime.configuredLoops);
    object.insert(QStringLiteral("eventIndex"), runtime.eventIndex);
    object.insert(QStringLiteral("eventCount"), runtime.eventCount);
    object.insert(QStringLiteral("startedAtEpochMs"), QString::number(runtime.startedAtEpochMs));
    object.insert(QStringLiteral("revision"), QString::number(runtime.revision));
    object.insert(QStringLiteral("errorMessage"), runtime.errorMessage);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool inputScriptRuntimeFromJson(const QByteArray& payload, RemoteInputScriptRuntimeInfo* runtime)
{
    if (!runtime) return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject object = document.object();
    RemoteInputScriptState state;
    if (!remoteInputScriptStateFromName(object.value(QStringLiteral("state")).toString(), &state)) return false;
    runtime->supported = object.value(QStringLiteral("supported")).toBool();
    runtime->state = state;
    runtime->runId = object.value(QStringLiteral("runId")).toString().trimmed();
    runtime->scriptName = object.value(QStringLiteral("scriptName")).toString().trimmed();
    runtime->scriptHash = object.value(QStringLiteral("scriptHash")).toString().trimmed();
    runtime->completedLoops = object.value(QStringLiteral("completedLoops")).toInt();
    runtime->configuredLoops = object.value(QStringLiteral("configuredLoops")).toInt();
    runtime->eventIndex = object.value(QStringLiteral("eventIndex")).toInt();
    runtime->eventCount = object.value(QStringLiteral("eventCount")).toInt();
    runtime->startedAtEpochMs = object.value(QStringLiteral("startedAtEpochMs")).toString().toLongLong();
    runtime->revision = object.value(QStringLiteral("revision")).toString().toULongLong();
    runtime->errorMessage = object.value(QStringLiteral("errorMessage")).toString().trimmed();
    return true;
}

QByteArray inputScriptStartCommand(const RemoteInputScriptStartRequest& request)
{
    QJsonObject object;
    object.insert(QStringLiteral("runId"), request.runId.trimmed());
    object.insert(QStringLiteral("fileName"), request.fileName.trimmed());
    object.insert(QStringLiteral("fileSize"), QString::number(request.fileSize));
    object.insert(QStringLiteral("sha256"), request.sha256.trimmed().toLower());
    object.insert(QStringLiteral("loopCount"), request.loopCount);
    object.insert(QStringLiteral("loopIntervalMs"), request.loopIntervalMs);
    object.insert(QStringLiteral("speedMultiplier"), request.speedMultiplier);
    object.insert(QStringLiteral("pasteRandomSuffixEnabled"), request.pasteRandomSuffixEnabled);
    object.insert(QStringLiteral("pasteRandomSeparator"), request.pasteRandomSeparator);
    object.insert(QStringLiteral("pasteRandomLength"), request.pasteRandomLength);
    object.insert(QStringLiteral("pasteRandomMode"), request.pasteRandomMode);
    return QByteArrayLiteral("input_script_start|")
        + QJsonDocument(object).toJson(QJsonDocument::Compact).toBase64()
        + '\n'; // wjy: 49102仍保持一行短命令协议，JSON只承载元数据，脚本文件本身留在共享目录。
}

QByteArray screenshotCommand(const QString& groupName, const QString& deviceName)
{
    QJsonObject object;
    object.insert(QStringLiteral("groupName"), groupName.trimmed());
    object.insert(QStringLiteral("deviceName"), deviceName.trimmed());
    return QByteArrayLiteral("screenshot|")
        + QJsonDocument(object).toJson(QJsonDocument::Compact).toBase64()
        + '\n'; // wjy: 一行命令只携带截图命名元数据，目标屏幕像素和PNG文件绝不通过49102传输。
}
// ===end====

void schedulePowerAction(DeviceControlAction action)
{
    const QStringList args = action == DeviceControlAction::Restart
        ? QStringList{QStringLiteral("/r"), QStringLiteral("/t"), QStringLiteral("0"), QStringLiteral("/f")}
        : QStringList{QStringLiteral("/s"), QStringLiteral("/t"), QStringLiteral("0"), QStringLiteral("/f")};
    QTimer::singleShot(120, [args] {
        QProcess::startDetached(QStringLiteral("shutdown"), args);
    });
}

class CommandConnection final : public QObject {
public:
    CommandConnection(QTcpSocket* socket, std::function<void()> remoteUpdateStarter, QObject* parent = nullptr)
        : QObject(parent)
        , socket_(socket)
        , remoteUpdateStarter_(std::move(remoteUpdateStarter)) // wjy: 连接对象只负责协议应答，耗时更新准备交给命令服务器统一拥有的可汇合线程。
    {
        socket_->setParent(this);
        connect(socket_, &QTcpSocket::readyRead, this, &CommandConnection::onReadyRead);
        connect(socket_, &QTcpSocket::disconnected, this, &QObject::deleteLater);
    }

private:
    void replyAndClose(const QByteArray& payload)
    {
        socket_->write(payload);
        socket_->flush();
        socket_->disconnectFromHost();
        if (socket_->state() == QAbstractSocket::UnconnectedState) {
            deleteLater();
        }
    }

    void onReadyRead()
    {
        buffer_.append(socket_->readAll());
        const int newlineIndex = buffer_.indexOf('\n');
        if (newlineIndex < 0) {
            return;
        }

        const QList<QByteArray> parts = buffer_.left(newlineIndex).trimmed().split('|');
        const QByteArray command = parts.value(0).trimmed().toLower();
        if (command == "shutdown") {
            replyAndClose(QByteArrayLiteral("ok\n"));
            schedulePowerAction(DeviceControlAction::Shutdown);
            return;
        }
        if (command == "restart") {
            replyAndClose(QByteArrayLiteral("ok\n"));
            schedulePowerAction(DeviceControlAction::Restart);
            return;
        }
// =====wjy====
        if (command == "device_sync") {
            replyAndClose(QByteArrayLiteral("ok\n"));
            QTimer::singleShot(0, [] {
                DeviceListSyncService::instance().requestImmediateSync(); // wjy: 命令仅唤醒本机同步服务，实际数据仍从带锁和 revision 的共享快照读取。
            });
            return;
        }
// ===end====
// =====wjy====
        if (command == "screenshot") {
            const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromBase64(parts.value(1)));
            if (!document.isObject()) {
                replyAndClose(QByteArrayLiteral("invalid_request\n"));
                return;
            }
            const QJsonObject object = document.object();
            const QString groupName = object.value(QStringLiteral("groupName")).toString().trimmed().left(160);
            const QString deviceName = object.value(QStringLiteral("deviceName")).toString().trimmed().left(160); // wjy: 命令输入先限制长度，真实文件名仍由截图服务执行Windows字符过滤和80字符上限。
            const ScreenshotCaptureResult result = ScreenshotService::capturePrimaryScreen(groupName, deviceName); // wjy: 本调用发生在目标设备Qt线程，grabWindow读取目标主屏而不是主控Viewer画面。
            if (result.success) {
                replyAndClose(QByteArrayLiteral("screenshot|") + result.filePath.toUtf8().toBase64() + '\n');
            } else {
                replyAndClose(QByteArrayLiteral("error|")
                    + QUrl::toPercentEncoding(result.errorMessage.trimmed().isEmpty()
                        ? QStringLiteral("目标设备截图失败")
                        : result.errorMessage.trimmed())
                    + '\n');
            }
            return;
        }
// ===end====
// =====wjy====
        if (command == "input_script_start") {
            const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromBase64(parts.value(1)));
            if (!document.isObject()) {
                replyAndClose(QByteArrayLiteral("invalid_request\n"));
                return;
            }
            const QJsonObject object = document.object();
            RemoteInputScriptStartRequest request;
            request.runId = object.value(QStringLiteral("runId")).toString();
            request.fileName = object.value(QStringLiteral("fileName")).toString();
            request.fileSize = object.value(QStringLiteral("fileSize")).toString().toLongLong();
            request.sha256 = object.value(QStringLiteral("sha256")).toString();
            request.loopCount = object.value(QStringLiteral("loopCount")).toInt(1);
            request.loopIntervalMs = object.value(QStringLiteral("loopIntervalMs")).toInt();
            request.speedMultiplier = object.value(QStringLiteral("speedMultiplier")).toDouble(1.0);
            request.pasteRandomSuffixEnabled = object.value(QStringLiteral("pasteRandomSuffixEnabled")).toBool();
            request.pasteRandomSeparator = object.value(QStringLiteral("pasteRandomSeparator")).toString();
            request.pasteRandomLength = object.value(QStringLiteral("pasteRandomLength")).toInt(3);
            request.pasteRandomMode = object.value(QStringLiteral("pasteRandomMode")).toInt();
            QString errorMessage;
            const RemoteInputScriptCommandResult result = InputScriptExecutionService::instance().start(
                request, &errorMessage);
            if (result == RemoteInputScriptCommandResult::Accepted) {
                replyAndClose(QByteArrayLiteral("accepted|") + request.runId.toUtf8() + '\n');
            } else if (result == RemoteInputScriptCommandResult::AlreadyRunning) {
                replyAndClose(QByteArrayLiteral("already_running\n"));
            } else {
                replyAndClose(QByteArrayLiteral("error|")
                    + QUrl::toPercentEncoding(errorMessage.trimmed().isEmpty()
                        ? QStringLiteral("键鼠脚本启动失败")
                        : errorMessage.trimmed())
                    + '\n');
            }
            return; // wjy: 目标端命令服务只受理元数据，真正共享目录复制在独立执行器后台线程完成。
        }
        if (command == "input_script_stop") {
            QString errorMessage;
            const RemoteInputScriptCommandResult result = InputScriptExecutionService::instance().stop(
                QString::fromUtf8(parts.value(1)).trimmed(), &errorMessage);
            if (result == RemoteInputScriptCommandResult::Accepted
                || result == RemoteInputScriptCommandResult::NotRunning) {
                replyAndClose(QByteArrayLiteral("ok\n"));
            } else {
                replyAndClose(QByteArrayLiteral("error|")
                    + QUrl::toPercentEncoding(errorMessage.trimmed().isEmpty()
                        ? QStringLiteral("键鼠脚本停止失败")
                        : errorMessage.trimmed())
                    + '\n');
            }
            return; // wjy: F10停止只依赖目标端runId，不会因原主控窗口已经退出而失去停止能力。
        }
        if (command == "input_script_status") {
            const QByteArray status = inputScriptRuntimeJson(InputScriptExecutionService::instance().snapshot()).toBase64();
            replyAndClose(QByteArrayLiteral("status|") + status + '\n');
            return; // wjy: 新控制端可在实时UDP快照到达前主动取得目标端当前脚本状态。
        }
// ===end====
// =====wjy====
        if (command == "update_status") {
            if (g_remoteUpdateScheduled) {
                replyAndClose(QByteArrayLiteral("preparing\n"));
                return; // wjy: 仅设备右键菜单发起的远程更新向控制端返回准备阶段。
            }
            UpdateService& updateService = UpdateService::instance();
            if (!updateService.confirmedRemoteVersion().trimmed().isEmpty()
                && !updateService.isUpdateAvailable()) {
                g_remoteUpdateFailure.clear();
                replyAndClose(QByteArrayLiteral("complete\n"));
                return; // wjy: 当前已追平共享版本且依赖完整时清除旧失败，避免手动刷新重新点亮无效按钮。
            }
            if (!g_remoteUpdateFailure.trimmed().isEmpty()) {
                replyAndClose(QByteArrayLiteral("failed|")
                    + QUrl::toPercentEncoding(g_remoteUpdateFailure.trimmed())
                    + QByteArrayLiteral("\n"));
                return; // wjy: 目标端准备失败时把真实原因传给远控窗口。
            }
            replyAndClose(updateService.isUpdateAvailable()
                    ? QByteArrayLiteral("idle\n")
                    : QByteArrayLiteral("complete\n"));
            return;
        }

        if (command == "update") {
            UpdateService& updateService = UpdateService::instance();
            const QString expectedVersion = QString::fromUtf8(parts.value(1)).trimmed();
            const bool expectedUpdateAvailable = !expectedVersion.isEmpty()
                && UpdateService::remoteUpdateOrRepairAvailable(
                    expectedVersion,
                    UpdateService::localVersionText(),
                    QCoreApplication::applicationDirPath());
            if (!updateService.isUpdateAvailable() && !expectedUpdateAvailable) {
                replyAndClose(QByteArrayLiteral("up_to_date\n"));
                return; // wjy: 本机缓存和控制端确认版本都不指向可执行更新时才返回已最新。
            }
            if (g_remoteUpdateScheduled) {
                replyAndClose(QByteArrayLiteral("accepted\n"));
                return; // wjy: 重复请求视为已受理，防止用户连续点击时误报失败。
            }

            g_remoteUpdateScheduled = true;
            g_remoteUpdateFailure.clear(); // wjy: 新请求开始时清除上一轮失败，避免状态查询读到过期错误。
            replyAndClose(QByteArrayLiteral("accepted\n")); // wjy: 先把受理结果发回控制端，再开始可能耗时的大文件暂存，避免 1.5 秒命令超时。
            if (remoteUpdateStarter_) {
                remoteUpdateStarter_(); // wjy: 启动动作只创建专用后台线程并立即返回，目标端主界面和命令端口不会等待网盘复制。
            } else {
                g_remoteUpdateScheduled = false;
                g_remoteUpdateFailure = QString::fromUtf8("目标设备更新后台任务不可用。"); // wjy: 极端初始化异常时恢复可重试状态，不能永久停留在 preparing。
            }
            return;
        }
// ===end====
        if (command == "wake_mac") {
            const QString macAddress = QString::fromUtf8(parts.value(1)).trimmed();
            const DeviceInfo localInfo = DeviceInfoService::local();
            const WakeOnLanSendResult result = WakeOnLanSender::send(macAddress, localInfo.broadcastIp.trimmed());
            if (result.success) {
                replyAndClose(QByteArrayLiteral("ok\n"));
            } else {
                QByteArray payload = QByteArrayLiteral("error");
                if (!result.errorMessage.trimmed().isEmpty()) {
                    payload.append('|');
                    payload.append(result.errorMessage.trimmed().toUtf8());
                }
                payload.append('\n');
                replyAndClose(payload);
            }
            return;
        }
        if (command == "rename_device") {
            const QString newName = QUrl::fromPercentEncoding(parts.value(1)).trimmed();
            QString errorMessage;
            if (renameLocalComputer(newName, &errorMessage)) {
                replyAndClose(QByteArrayLiteral("ok\n"));
            } else {
                QByteArray payload = QByteArrayLiteral("error");
                if (!errorMessage.trimmed().isEmpty()) {
                    payload.append('|');
                    payload.append(QUrl::toPercentEncoding(errorMessage.trimmed()));
                }
                payload.append('\n');
                replyAndClose(payload);
            }
            return;
        }
// =====wjy====
        if (command == "authorize_ssh_key") {
            const QString publicKey = QUrl::fromPercentEncoding(parts.value(1)).trimmed();
            QString errorMessage;
            if (PortableOpenSshManager::instance().authorizeClientPublicKey(publicKey, &errorMessage)) {
                replyAndClose(QByteArrayLiteral("ok\n"));
            } else {
                QByteArray payload = QByteArrayLiteral("error");
                if (!errorMessage.trimmed().isEmpty()) {
                    payload.append('|');
                    payload.append(QUrl::toPercentEncoding(errorMessage.trimmed()));
                }
                payload.append('\n');
                replyAndClose(payload);
            }
            return; // wjy: 目标机收到发起方公钥后写入 authorized_keys，后续 ssh.exe 才能通过 publickey 认证。
        }
// ===end====

        replyAndClose(QByteArrayLiteral("error\n"));
    }

    QTcpSocket* socket_ = nullptr;
    QByteArray buffer_;
    std::function<void()> remoteUpdateStarter_; // wjy: 使用窄回调隔离连接协议与线程生命周期，CommandConnection 不直接管理共享文件任务。
};

} // namespace

class DeviceCommandServer::Impl final : public QObject {
public:
    explicit Impl(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    bool start(uint16_t port)
    {
        if (server_) {
            return true;
        }

        auto* server = new QTcpServer(this);
        connect(server, &QTcpServer::newConnection, this, [this, server] {
            while (server->hasPendingConnections()) {
                if (QTcpSocket* socket = server->nextPendingConnection()) {
                    new CommandConnection(socket, [this] {
                        startRemoteUpdatePreparation(); // wjy: 所有远程更新请求汇入同一个服务器线程所有者，失败、取消和退出都能确定性收口。
                    }, this);
                }
            }
        });

        if (!server->listen(QHostAddress::AnyIPv4, port)) {
            delete server;
            return false;
        }

        server_ = server;
        return true;
    }

    void stop()
    {
        // =====wjy====
        writeCommandServerLog(QStringLiteral("[wjy-command] Impl::stop begin server=%1").arg(server_ ? 1 : 0)); // wjy: 记录命令服务内部 stop 开始，判断关闭异常是否发生在 QTcpServer 清理阶段。
        cancelAndJoinRemoteUpdatePreparation(); // wjy: 关闭监听端口前先取消可能阻塞在 SMB 的更新线程，目标程序退出不会被后台网盘访问长期占用。
        // ===end====
        if (!server_) {
            // =====wjy====
            writeCommandServerLog(QStringLiteral("[wjy-command] Impl::stop skipped because server is null")); // wjy: server 已为空时直接返回，说明外层析构里的二次 stop 不会重复删除。
            // ===end====
            return;
        }
        // =====wjy====
        QTcpServer* server = server_.data();
        server_ = nullptr;
        server->close();
        delete server; // wjy: 关闭阶段事件循环已经退出，改为同步删除 QTcpServer，避免 deleteLater 延迟事件和父对象立即析构交错。
        writeCommandServerLog(QStringLiteral("[wjy-command] Impl::stop end")); // wjy: 记录命令服务内部 QTcpServer 已同步释放。
        // ===end====
    }

    ~Impl() override
    {
        // =====wjy====
        writeCommandServerLog(QStringLiteral("[wjy-command] Impl dtor begin")); // wjy: 记录命令服务实现对象开始析构，和 main 退出后的异常位置对齐。
        // ===end====
        stop();
        // =====wjy====
        writeCommandServerLog(QStringLiteral("[wjy-command] Impl dtor end")); // wjy: 记录命令服务实现对象析构完成。
        // ===end====
    }

private:
    // =====wjy====
    void startRemoteUpdatePreparation()
    {
        if (remoteUpdateThread_.joinable()) {
            return; // wjy: g_remoteUpdateScheduled 已阻止正常重复请求，这里再次防御旧线程尚未完成回收的竞态。
        }

        try {
            remoteUpdateThread_ = std::thread([this] {
                QString errorMessage;
                const bool prepared = UpdateService::instance().applyRemoteUpdate(&errorMessage); // wjy: 目标端版本读取、复制和校验全部在命令服务器专用线程执行。
                QMetaObject::invokeMethod(this, [this, prepared, errorMessage] {
                    finishRemoteUpdatePreparation(prepared, errorMessage); // wjy: 全局协议状态只回到命令服务器所属主线程更新，避免跨线程读写 QString。
                }, Qt::QueuedConnection);
            });
        } catch (...) {
            g_remoteUpdateScheduled = false;
            g_remoteUpdateFailure = QString::fromUtf8("无法创建目标设备更新后台线程。");
            writeCommandServerLog(QStringLiteral("[wjy-command] failed to create remote update worker")); // wjy: 线程资源不足时保留主进程并允许控制端读取明确失败状态。
        }
    }

    void finishRemoteUpdatePreparation(bool prepared, const QString& errorMessage)
    {
        if (remoteUpdateThread_.joinable()) {
            remoteUpdateThread_.join(); // wjy: 工作函数已经投递完成回调，此处仅回收系统线程句柄，不再等待网络。
        }
        if (prepared) {
            return; // wjy: 成功路径由 UpdateService 的 updateReadyToQuit 信号接管，保持 preparing 直到程序退出并重启。
        }

        g_remoteUpdateScheduled = false;
        g_remoteUpdateFailure = errorMessage.trimmed().isEmpty()
            ? QString::fromUtf8("目标设备准备更新失败。")
            : errorMessage.trimmed(); // wjy: 失败状态只在主线程发布，后续 update_status 查询可以安全读取完整字符串。
        writeCommandServerLog(QStringLiteral("[wjy-command] remote update prepare failed: %1")
            .arg(g_remoteUpdateFailure));
    }

    void cancelAndJoinRemoteUpdatePreparation()
    {
        if (!remoteUpdateThread_.joinable()) {
            return;
        }
#if defined(Q_OS_WIN)
        if (!CancelSynchronousIo(remoteUpdateThread_.native_handle())) {
            const DWORD errorCode = GetLastError();
            if (errorCode != ERROR_NOT_FOUND) {
                writeCommandServerLog(QStringLiteral("[wjy-command] update worker CancelSynchronousIo failed error=%1")
                    .arg(errorCode)); // wjy: ERROR_NOT_FOUND 只表示线程当前没有可取消 I/O，其它错误保留到统一诊断日志。
            }
        }
#endif
        remoteUpdateThread_.join(); // wjy: 服务对象析构前确定性汇合，工作线程不会在 QObject 和 Qt 运行库销毁后继续回调。
    }
    // ===end====

    QPointer<QTcpServer> server_;
    std::thread remoteUpdateThread_; // wjy: 目标端远程更新使用唯一可取消线程，弱网下不会阻塞 UI、命令服务或堆积任务。
};

DeviceCommandServer::~DeviceCommandServer()
{
    // =====wjy====
    writeCommandServerLog(QStringLiteral("[wjy-command] DeviceCommandServer dtor begin")); // wjy: 记录命令服务外层析构开始，确认 DeviceGrid 析构后是否继续走到这里。
    // ===end====
    stop();
    // =====wjy====
    writeCommandServerLog(QStringLiteral("[wjy-command] DeviceCommandServer dtor end")); // wjy: 记录命令服务外层析构结束。
    // ===end====
}

bool DeviceCommandServer::start(uint16_t port)
{
    if (!m_impl) {
        m_impl = new Impl();
    }
    return m_impl->start(port);
}

void DeviceCommandServer::stop()
{
    // =====wjy====
    writeCommandServerLog(QStringLiteral("[wjy-command] DeviceCommandServer::stop begin impl=%1").arg(m_impl ? 1 : 0)); // wjy: 记录外层 stop 是否还有实现对象需要释放。
    // ===end====
    if (!m_impl) {
        // =====wjy====
        writeCommandServerLog(QStringLiteral("[wjy-command] DeviceCommandServer::stop skipped because impl is null")); // wjy: impl 已为空时说明前面已经停止过。
        // ===end====
        return;
    }
    m_impl->stop();
    delete m_impl;
    m_impl = nullptr;
    // =====wjy====
    writeCommandServerLog(QStringLiteral("[wjy-command] DeviceCommandServer::stop end")); // wjy: 记录外层 stop 完成，后面如果还崩就继续看 statusServer 或 QApplication 析构。
    // ===end====
}

bool DeviceCommandService::send(const QString& hostIp, DeviceControlAction action, uint16_t port, int timeoutMs)
{
    if (hostIp.trimmed().isEmpty()) {
        return false;
    }

    const QByteArray payload = actionCommand(action);
    if (payload.isEmpty()) {
        return false;
    }

    QTcpSocket socket;
    if (!configureLanTcpSocket(socket, hostIp)) {
        return false; // wjy: 无法绑定真实局域网源地址时不发送关机或重启命令。
    }
    socket.connectToHost(hostIp.trimmed(), port);
    if (!socket.waitForConnected(timeoutMs)) {
        return false;
    }
    if (socket.write(payload) != payload.size()) {
        socket.disconnectFromHost();
        return false;
    }
    if (!socket.waitForBytesWritten(timeoutMs)) {
        socket.disconnectFromHost();
        return false;
    }
    if (!socket.waitForReadyRead(timeoutMs)) {
        socket.disconnectFromHost();
        return false;
    }

    const QByteArray reply = socket.readAll().trimmed().toLower();
    socket.disconnectFromHost();
    return reply == "ok";
}

// =====wjy====
RemoteUpdateRequestResult DeviceCommandService::requestUpdate(
    const QString& hostIp,
    QString* errorMessage,
    uint16_t port,
    int timeoutMs,
    const QString& expectedVersion)
{
    if (hostIp.trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = QString::fromUtf8("目标 IP 为空。");
        return RemoteUpdateRequestResult::Failed;
    }

    QTcpSocket socket;
    if (!configureLanTcpSocket(socket, hostIp, errorMessage)) {
        return RemoteUpdateRequestResult::Failed; // wjy: 更新请求同样禁止通过 Meta/TUN 虚拟默认路由发送。
    }
    socket.connectToHost(hostIp.trimmed(), port);
    if (!socket.waitForConnected(timeoutMs)) {
        if (errorMessage) *errorMessage = socket.errorString().trimmed();
        return RemoteUpdateRequestResult::Failed;
    }

    QByteArray payload = QByteArrayLiteral("update");
    const QString normalizedExpectedVersion = expectedVersion.trimmed();
    if (!normalizedExpectedVersion.isEmpty()) {
        payload.append('|');
        payload.append(normalizedExpectedVersion.toUtf8());
    }
    payload.append('\n');
    if (socket.write(payload) != payload.size() || !socket.waitForBytesWritten(timeoutMs)) {
        if (errorMessage) *errorMessage = socket.errorString().trimmed();
        socket.disconnectFromHost();
        return RemoteUpdateRequestResult::Failed;
    }
    if (!socket.waitForReadyRead(timeoutMs)) {
        if (errorMessage) *errorMessage = socket.errorString().trimmed();
        socket.disconnectFromHost();
        return RemoteUpdateRequestResult::Failed;
    }

    const QByteArray reply = socket.readAll().trimmed().toLower();
    socket.disconnectFromHost();
    if (reply == "accepted") {
        if (errorMessage) errorMessage->clear();
        return RemoteUpdateRequestResult::Accepted;
    }
    if (reply == "up_to_date") {
        if (errorMessage) errorMessage->clear();
        return RemoteUpdateRequestResult::UpToDate;
    }

    if (errorMessage) {
        *errorMessage = QString::fromUtf8("目标设备未受理更新请求；请确认目标设备已安装支持远程更新的版本。");
    }
    return RemoteUpdateRequestResult::Failed; // wjy: 旧客户端只会返回 error，这里给出可直接定位版本兼容性的提示。
}
// ===end====

// =====wjy====
RemoteUpdateStatus DeviceCommandService::queryUpdateStatus(const QString& hostIp, QString* errorMessage, uint16_t port, int timeoutMs)
{
    if (hostIp.trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = QString::fromUtf8("目标 IP 为空。");
        return RemoteUpdateStatus::Unreachable;
    }

    QTcpSocket socket;
    if (!configureLanTcpSocket(socket, hostIp, errorMessage)) {
        return RemoteUpdateStatus::Unreachable; // wjy: 状态查询与更新请求保持相同物理出口，避免两条路径结果不一致。
    }
    socket.connectToHost(hostIp.trimmed(), port);
    if (!socket.waitForConnected(timeoutMs)) {
        if (errorMessage) *errorMessage = socket.errorString().trimmed();
        return RemoteUpdateStatus::Unreachable; // wjy: 主程序退出及更新器替换文件期间端口不可达是正常阶段。
    }

    const QByteArray payload = QByteArrayLiteral("update_status\n");
    if (socket.write(payload) != payload.size() || !socket.waitForBytesWritten(timeoutMs)
        || !socket.waitForReadyRead(timeoutMs)) {
        if (errorMessage) *errorMessage = socket.errorString().trimmed();
        socket.disconnectFromHost();
        return RemoteUpdateStatus::Unreachable;
    }

    const QByteArray reply = socket.readAll().trimmed();
    socket.disconnectFromHost();
    const QList<QByteArray> parts = reply.split('|');
    const QByteArray status = parts.value(0).trimmed().toLower();
    if (status == "preparing") {
        if (errorMessage) errorMessage->clear();
        return RemoteUpdateStatus::Preparing;
    }
    if (status == "complete") {
        if (errorMessage) errorMessage->clear();
        return RemoteUpdateStatus::Complete;
    }
    if (status == "idle") {
        if (errorMessage) errorMessage->clear();
        return RemoteUpdateStatus::Idle;
    }
    if (status == "failed") {
        if (errorMessage) *errorMessage = QUrl::fromPercentEncoding(parts.value(1)).trimmed();
        return RemoteUpdateStatus::Failed;
    }

    if (errorMessage) *errorMessage = QString::fromUtf8("目标设备暂不支持更新状态查询。");
    return RemoteUpdateStatus::Unsupported; // wjy: 首次滚动升级时旧目标端会返回 error，继续等待其退出并升级到新协议。
}
// ===end====

bool DeviceCommandService::sendWakeProxy(const QString& hostIp, const QString& macAddress, QString* errorMessage, uint16_t port, int timeoutMs)
{
    if (hostIp.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("代理设备 IP 为空");
        }
        return false;
    }

    const QByteArray payload = wakeCommand(macAddress);
    if (payload.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("MAC 地址无效");
        }
        return false;
    }

    QTcpSocket socket;
    if (!configureLanTcpSocket(socket, hostIp, errorMessage)) {
        return false; // wjy: 代发开机包也必须从真实局域网网卡连接代理设备。
    }
    socket.connectToHost(hostIp.trimmed(), port);
    if (!socket.waitForConnected(timeoutMs)) {
        if (errorMessage) {
            *errorMessage = socket.errorString().trimmed();
        }
        return false;
    }
    if (socket.write(payload) != payload.size()) {
        if (errorMessage) {
            *errorMessage = socket.errorString().trimmed();
        }
        socket.disconnectFromHost();
        return false;
    }
    if (!socket.waitForBytesWritten(timeoutMs)) {
        if (errorMessage) {
            *errorMessage = socket.errorString().trimmed();
        }
        socket.disconnectFromHost();
        return false;
    }
    if (!socket.waitForReadyRead(timeoutMs)) {
        if (errorMessage) {
            *errorMessage = socket.errorString().trimmed();
        }
        socket.disconnectFromHost();
        return false;
    }

    const QByteArray reply = socket.readAll().trimmed();
    socket.disconnectFromHost();
    const QList<QByteArray> parts = reply.split('|');
    const QByteArray status = parts.value(0).trimmed().toLower();
    if (status == "ok") {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }

    if (errorMessage) {
        *errorMessage = parts.size() > 1
            ? QString::fromUtf8(parts.at(1)).trimmed()
            : QStringLiteral("同网段代理设备代发开机包失败");
    }
    return false;
}

bool DeviceCommandService::renameDevice(const QString& hostIp, const QString& newName, QString* errorMessage, uint16_t port, int timeoutMs)
{
    const QByteArray payload = renameDeviceCommand(newName);
    return sendCommandPayload(hostIp, payload, errorMessage, port, timeoutMs); // wjy: 复用统一 TCP 命令发送流程，把本地重命名同步到目标机器。
}

bool DeviceCommandService::authorizeTerminalKey(const QString& hostIp, const QString& publicKey, QString* errorMessage, uint16_t port, int timeoutMs)
{
// =====wjy====
    const QByteArray payload = authorizeTerminalKeyCommand(publicKey);
    for (int attempt = 0; attempt < 2; ++attempt) {
        QAbstractSocket::SocketError transportError = QAbstractSocket::UnknownSocketError;
        if (sendCommandPayload(hostIp, payload, errorMessage, port, timeoutMs, &transportError)) {
            return true; // wjy: 收到目标端完整ok行后立即结束，公钥只登记一次。
        }
        if (transportError != QAbstractSocket::RemoteHostClosedError || attempt > 0) {
            return false; // wjy: 目标端明确返回error、连接拒绝或超时都保留真实原因，只有主动断开类瞬态错误允许重试。
        }
        QThread::msleep(120); // wjy: 公钥追加操作幂等，短暂等待后重连一次可覆盖目标命令连接刚好切换或释放身份锁的窗口。
    }
    return false;
// ===end====
}

// =====wjy====
bool DeviceCommandService::requestDeviceListSync(const QString& hostIp, QString* errorMessage, uint16_t port, int timeoutMs)
{
    return sendCommandPayload(hostIp, QByteArrayLiteral("device_sync\n"), errorMessage, port, timeoutMs); // wjy: 通知包固定且幂等，重复收到只会多执行一次 revision 检查。
}

bool DeviceCommandService::requestScreenshot(
    const QString& hostIp,
    const QString& groupName,
    const QString& deviceName,
    QString* filePath,
    QString* errorMessage,
    uint16_t port,
    int timeoutMs)
{
    if (filePath) filePath->clear();
    QByteArray reply;
    if (!sendCommandAndReadReply(
            hostIp,
            screenshotCommand(groupName, deviceName),
            &reply,
            errorMessage,
            port,
            timeoutMs)) {
        return false;
    }
    const QList<QByteArray> parts = reply.split('|');
    const QByteArray status = parts.value(0).trimmed().toLower();
    if (status == "screenshot") {
        const QString returnedPath = QString::fromUtf8(QByteArray::fromBase64(parts.value(1))).trimmed();
        if (ScreenshotService::isManagedScreenshotPath(returnedPath)) {
            if (filePath) *filePath = QDir::toNativeSeparators(returnedPath);
            if (errorMessage) errorMessage->clear();
            return true; // wjy: 控制端只接受固定共享目录内的PNG路径，随后直接从网盘加载原图。
        }
    }
    if (errorMessage) {
        *errorMessage = parts.size() > 1 && status == "error"
            ? QUrl::fromPercentEncoding(parts.at(1)).trimmed()
            : QStringLiteral("目标端返回的截图结果无效。");
    }
    return false;
}
// ===end====

// =====wjy====
RemoteInputScriptCommandResult DeviceCommandService::requestInputScriptStart(
    const QString& hostIp,
    const RemoteInputScriptStartRequest& request,
    QString* errorMessage,
    uint16_t port,
    int timeoutMs)
{
    QByteArray reply;
    if (!sendCommandAndReadReply(
            hostIp,
            inputScriptStartCommand(request),
            &reply,
            errorMessage,
            port,
            timeoutMs)) {
        return RemoteInputScriptCommandResult::Failed;
    }
    const QList<QByteArray> parts = reply.split('|');
    const QByteArray status = parts.value(0).trimmed().toLower();
    if (status == "accepted") return RemoteInputScriptCommandResult::Accepted;
    if (status == "already_running") return RemoteInputScriptCommandResult::AlreadyRunning;
    if (errorMessage) {
        *errorMessage = parts.size() > 1
            ? QUrl::fromPercentEncoding(parts.at(1)).trimmed()
            : QStringLiteral("目标端未接受键鼠脚本启动请求");
    }
    return status == "invalid_request"
        ? RemoteInputScriptCommandResult::InvalidRequest
        : RemoteInputScriptCommandResult::Failed;
}

RemoteInputScriptCommandResult DeviceCommandService::requestInputScriptStop(
    const QString& hostIp,
    const QString& runId,
    QString* errorMessage,
    uint16_t port,
    int timeoutMs)
{
    const QByteArray payload = QByteArrayLiteral("input_script_stop|") + runId.trimmed().toUtf8() + '\n';
    QByteArray reply;
    if (!sendCommandAndReadReply(hostIp, payload, &reply, errorMessage, port, timeoutMs)) {
        return RemoteInputScriptCommandResult::Failed;
    }
    const QList<QByteArray> parts = reply.split('|');
    if (parts.value(0).trimmed().toLower() == "ok") {
        return RemoteInputScriptCommandResult::Accepted;
    }
    if (errorMessage) {
        *errorMessage = parts.size() > 1
            ? QUrl::fromPercentEncoding(parts.at(1)).trimmed()
            : QStringLiteral("目标端未接受键鼠脚本停止请求");
    }
    return RemoteInputScriptCommandResult::Failed;
}

RemoteInputScriptRuntimeInfo DeviceCommandService::queryInputScriptStatus(
    const QString& hostIp,
    QString* errorMessage,
    uint16_t port,
    int timeoutMs)
{
    RemoteInputScriptRuntimeInfo runtime;
    QByteArray reply;
    if (!sendCommandAndReadReply(
            hostIp,
            QByteArrayLiteral("input_script_status\n"),
            &reply,
            errorMessage,
            port,
            timeoutMs)) {
        runtime.state = RemoteInputScriptState::Unknown;
        runtime.supported = false;
        return runtime;
    }
    const QList<QByteArray> parts = reply.split('|');
    if (parts.value(0).trimmed().toLower() != "status"
        || !inputScriptRuntimeFromJson(QByteArray::fromBase64(parts.value(1)), &runtime)) {
        runtime.state = RemoteInputScriptState::Unknown;
        runtime.supported = false;
        if (errorMessage) *errorMessage = QStringLiteral("目标端返回的键鼠脚本状态无效。");
    }
    return runtime;
}
// ===end====

} // namespace platform
