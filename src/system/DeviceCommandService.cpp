#include "system/DeviceCommandService.h"

#include "system/DeviceInfoService.h"
#include "system/WakeOnLanSender.h"

#include <QAbstractSocket>
#include <QPointer>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace platform {
namespace {

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
        if (!server_) {
            return;
        }
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
    }

private:
    QPointer<QTcpServer> server_;
};

DeviceCommandServer::~DeviceCommandServer()
{
    stop();
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
    if (!m_impl) {
        return;
    }
    m_impl->stop();
    delete m_impl;
    m_impl = nullptr;
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

} // namespace platform
