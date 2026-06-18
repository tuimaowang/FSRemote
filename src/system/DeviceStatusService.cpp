#include "system/DeviceStatusService.h"
#include "system/DeviceInfoService.h"
#include "system/PortableOpenSshManager.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

namespace platform {
namespace {

QByteArray statusPayload(bool busy)
{
    QByteArray payload = busy ? QByteArrayLiteral("busy") : QByteArrayLiteral("online");
    const QString loginUser = PortableOpenSshManager::instance().loginUser();
    const DeviceInfo localInfo = DeviceInfoService::local();
    payload.append('|');
    payload.append(loginUser.toUtf8());
    payload.append('|');
    payload.append(localInfo.ip.trimmed().toUtf8());
    payload.append('|');
    payload.append(localInfo.subnetMask.trimmed().toUtf8());
    payload.append('|');
    payload.append(localInfo.broadcastIp.trimmed().toUtf8());
    payload.append('|');
    payload.append(localInfo.mac.trimmed().toUtf8());
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
        info.state = DevicePresenceState::Online;
        return info;
    }

    const QByteArray payload = socket.readAll().trimmed();
    socket.disconnectFromHost();
    const QList<QByteArray> parts = payload.split('|');
    const QByteArray statePart = parts.value(0).trimmed().toLower();
    if (statePart.startsWith("busy")) {
        info.state = DevicePresenceState::Busy;
    } else {
        info.state = DevicePresenceState::Online;
    }
    if (parts.size() > 1) {
        info.terminalUser = QString::fromUtf8(parts.at(1)).trimmed();
    }
    if (parts.size() > 2) {
        info.localIp = QString::fromUtf8(parts.at(2)).trimmed();
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
    return info;
}

} // namespace

DeviceStatusServer::DeviceStatusServer(std::function<bool()> busyProvider)
    : m_busyProvider(std::move(busyProvider))
{
}

DeviceStatusServer::~DeviceStatusServer()
{
    stop();
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

            socket->write(statusPayload(m_busyProvider && m_busyProvider()));
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
    if (!m_server) {
        return;
    }

    m_server->close();
    delete m_server;
    m_server = nullptr;
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
