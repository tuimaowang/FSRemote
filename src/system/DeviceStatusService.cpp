#include "system/DeviceStatusService.h"
#include "system/DeviceInfoService.h"
#include "system/PortableOpenSshManager.h"
#include "system/WjyDiagnosticLog.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

namespace platform {
namespace {

// =====wjy====
void writeStatusServerLog(const QString& message)
{
    writeWjyDiagnosticLog(message); // wjy: 状态服务关闭阶段写入统一诊断日志，用来确认 main 返回后的析构链路。
}
// ===end====

QByteArray statusPayload(bool busy)
{
    QByteArray payload = busy ? QByteArrayLiteral("busy") : QByteArrayLiteral("online");
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
    } else {
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

DeviceStatusServer::DeviceStatusServer(std::function<bool()> busyProvider)
    : m_busyProvider(std::move(busyProvider))
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
