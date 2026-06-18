#include "system/WakeOnLanSender.h"

#include <QByteArray>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <QUdpSocket>

namespace platform {
namespace {

QByteArray magicPacketForMac(const QString& macAddress)
{
    QString normalized = macAddress.trimmed().toUpper();
    normalized.remove(QRegularExpression(QStringLiteral("[^0-9A-F]")));
    if (normalized.size() != 12) {
        return {};
    }

    QByteArray macBytes;
    macBytes.reserve(6);
    for (int i = 0; i < 12; i += 2) {
        bool ok = false;
        const char byte = static_cast<char>(normalized.mid(i, 2).toUInt(&ok, 16));
        if (!ok) {
            return {};
        }
        macBytes.append(byte);
    }

    QByteArray packet;
    packet.reserve(6 + macBytes.size() * 16);
    packet.append(QByteArray(6, static_cast<char>(0xFF)));
    for (int i = 0; i < 16; ++i) {
        packet.append(macBytes);
    }
    return packet;
}

QList<QHostAddress> destinationAddresses(const QString& broadcastIp)
{
    QList<QHostAddress> addresses;
    QSet<QString> seen;
    const auto add = [&](const QHostAddress& address) {
        if (address.isNull()) {
            return;
        }
        const QString key = address.toString();
        if (seen.contains(key)) {
            return;
        }
        seen.insert(key);
        addresses.append(address);
    };

    if (!broadcastIp.trimmed().isEmpty()) {
        add(QHostAddress(broadcastIp.trimmed()));
    }
    add(QHostAddress::Broadcast);
    add(QHostAddress(QStringLiteral("255.255.255.255")));
    return addresses;
}

} // namespace

WakeOnLanSendResult WakeOnLanSender::send(const QString& macAddress, const QString& broadcastIp)
{
    WakeOnLanSendResult result;

    const QByteArray packet = magicPacketForMac(macAddress);
    if (packet.isEmpty()) {
        result.errorMessage = QStringLiteral("MAC 地址无效");
        return result;
    }

    QUdpSocket socket;

    bool anySuccess = false;
    QString lastError;
    const QList<QHostAddress> addresses = destinationAddresses(broadcastIp);
    const quint16 ports[] = {9, 7};
    for (const QHostAddress& address : addresses) {
        for (quint16 port : ports) {
            for (int i = 0; i < 3; ++i) {
                const qint64 written = socket.writeDatagram(packet, address, port);
                if (written == packet.size()) {
                    anySuccess = true;
                } else if (!socket.errorString().trimmed().isEmpty()) {
                    lastError = socket.errorString().trimmed();
                }
            }
        }
    }

    result.success = anySuccess;
    if (!result.success) {
        result.errorMessage = lastError.isEmpty() ? QStringLiteral("魔术包发送失败") : lastError;
    }
    return result;
}

} // namespace platform
