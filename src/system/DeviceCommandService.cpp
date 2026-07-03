#include "system/DeviceCommandService.h"

#include "system/DeviceInfoService.h"
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

} // namespace platform
