#pragma once

#include <cstdint>
#include <functional>

#include <QString>

class QTcpServer;

namespace platform {

enum class DevicePresenceState {
    Unknown,
    Online,
    Busy,
    Offline,
};

struct DeviceStatusInfo {
    DevicePresenceState state = DevicePresenceState::Offline;
    QString terminalUser;
    QString localIp;
    QString subnetMask;
    QString broadcastIp;
    QString mac;
};

class DeviceStatusServer final {
public:
    explicit DeviceStatusServer(std::function<bool()> busyProvider);
    ~DeviceStatusServer();

    bool start(uint16_t port = 49101);
    void stop();

private:
    std::function<bool()> m_busyProvider;
    QTcpServer* m_server = nullptr;
};

class DeviceStatusService final {
public:
    static DeviceStatusInfo query(const QString& hostIp, uint16_t port = 49101, int timeoutMs = 900);
    static DevicePresenceState probe(const QString& hostIp, uint16_t port = 49101, int timeoutMs = 900);
    static QString terminalUser(const QString& hostIp, uint16_t port = 49101, int timeoutMs = 900);
};

} // namespace platform
