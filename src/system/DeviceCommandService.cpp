#include "system/DeviceCommandService.h"

#include "system/DeviceInfoService.h"
#include "system/DeviceListSyncService.h"
#include "system/PortableOpenSshManager.h"
#include "system/UpdateService.h"
#include "system/WakeOnLanSender.h"
#include "system/WjyDiagnosticLog.h"

#include <QAbstractSocket>
#include <QPointer>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <string>

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

bool sendCommandPayload(const QString& hostIp, const QByteArray& payload, QString* errorMessage, uint16_t port, int timeoutMs)
{
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
            ? QUrl::fromPercentEncoding(parts.at(1)).trimmed()
            : QStringLiteral("远程命令执行失败");
    }
    return false;
}

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
    CommandConnection(QTcpSocket* socket, QObject* parent = nullptr)
        : QObject(parent)
        , socket_(socket)
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
        if (command == "update_status") {
            if (g_remoteUpdateScheduled) {
                replyAndClose(QByteArrayLiteral("preparing\n"));
                return; // wjy: 仅设备右键菜单发起的远程更新向控制端返回准备阶段。
            }
            if (!g_remoteUpdateFailure.trimmed().isEmpty()) {
                replyAndClose(QByteArrayLiteral("failed|")
                    + QUrl::toPercentEncoding(g_remoteUpdateFailure.trimmed())
                    + QByteArrayLiteral("\n"));
                return; // wjy: 目标端准备失败时把真实原因传给远控窗口。
            }
            replyAndClose(UpdateService::instance().isUpdateAvailable()
                    ? QByteArrayLiteral("idle\n")
                    : QByteArrayLiteral("complete\n")); // wjy: 重启后本地版本已追上共享版本即视为更新完成。
            return;
        }

        if (command == "update") {
            UpdateService& updateService = UpdateService::instance();
            if (!updateService.isUpdateAvailable()) {
                replyAndClose(QByteArrayLiteral("up_to_date\n"));
                return; // wjy: 目标端以自己的本地版本和共享版本为准判断，避免控制端版本状态影响目标设备。
            }
            if (g_remoteUpdateScheduled) {
                replyAndClose(QByteArrayLiteral("accepted\n"));
                return; // wjy: 重复请求视为已受理，防止用户连续点击时误报失败。
            }

            g_remoteUpdateScheduled = true;
            g_remoteUpdateFailure.clear(); // wjy: 新请求开始时清除上一轮失败，避免状态查询读到过期错误。
            replyAndClose(QByteArrayLiteral("accepted\n")); // wjy: 先把受理结果发回控制端，再开始可能耗时的大文件暂存，避免 1.5 秒命令超时。
            QTimer::singleShot(150, [] {
                QString errorMessage;
                if (!UpdateService::instance().applyRemoteUpdate(&errorMessage)) {
                    g_remoteUpdateScheduled = false;
                    g_remoteUpdateFailure = errorMessage.trimmed().isEmpty()
                        ? QString::fromUtf8("目标设备准备更新失败。")
                        : errorMessage.trimmed(); // wjy: 主进程未退出时保留失败原因，控制端下一轮查询即可结束等待。
                    writeCommandServerLog(QStringLiteral("[wjy-command] remote update prepare failed: %1")
                        .arg(errorMessage.trimmed())); // wjy: 准备失败时保留当前进程并允许再次请求，原因写入目标端统一诊断日志。
                }
            });
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
                    new CommandConnection(socket, this);
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
    QPointer<QTcpServer> server_;
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
RemoteUpdateRequestResult DeviceCommandService::requestUpdate(const QString& hostIp, QString* errorMessage, uint16_t port, int timeoutMs)
{
    if (hostIp.trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = QString::fromUtf8("目标 IP 为空。");
        return RemoteUpdateRequestResult::Failed;
    }

    QTcpSocket socket;
    socket.connectToHost(hostIp.trimmed(), port);
    if (!socket.waitForConnected(timeoutMs)) {
        if (errorMessage) *errorMessage = socket.errorString().trimmed();
        return RemoteUpdateRequestResult::Failed;
    }

    const QByteArray payload = QByteArrayLiteral("update\n");
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
    return sendCommandPayload(hostIp, payload, errorMessage, port, timeoutMs); // wjy: 打开终端前先把本机公钥登记到目标机，解决多台设备各自生成 key 后互不信任的问题。
// ===end====
}

// =====wjy====
bool DeviceCommandService::requestDeviceListSync(const QString& hostIp, QString* errorMessage, uint16_t port, int timeoutMs)
{
    return sendCommandPayload(hostIp, QByteArrayLiteral("device_sync\n"), errorMessage, port, timeoutMs); // wjy: 通知包固定且幂等，重复收到只会多执行一次 revision 检查。
}
// ===end====

} // namespace platform
