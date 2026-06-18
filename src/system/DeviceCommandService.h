#pragma once

#include <cstdint>

#include <QString>

namespace platform {

enum class DeviceControlAction {
    Shutdown,
    Restart,
};

class DeviceCommandServer final {
public:
    DeviceCommandServer() = default;
    ~DeviceCommandServer();

    bool start(uint16_t port = 49102);
    void stop();

private:
    class Impl;
    Impl* m_impl = nullptr;
};

class DeviceCommandService final {
public:
    static bool send(const QString& hostIp, DeviceControlAction action, uint16_t port = 49102, int timeoutMs = 1500);
    static bool sendWakeProxy(const QString& hostIp, const QString& macAddress, QString* errorMessage = nullptr, uint16_t port = 49102, int timeoutMs = 1500);
};

} // namespace platform
