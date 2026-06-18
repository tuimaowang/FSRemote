#pragma once

#include <cstdint>

#include <QString>

namespace platform {

struct DeviceInfo {
    QString name;
    QString ip;
    QString subnetMask;
    QString gateway;
    QString mac;
    QString broadcastIp;

    bool isEmpty() const
    {
        return name.trimmed().isEmpty()
            && ip.trimmed().isEmpty()
            && subnetMask.trimmed().isEmpty()
            && gateway.trimmed().isEmpty()
            && mac.trimmed().isEmpty()
            && broadcastIp.trimmed().isEmpty();
    }
};

class DeviceInfoService final {
public:
    static DeviceInfo local();
};

} // namespace platform
