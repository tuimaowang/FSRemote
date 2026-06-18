#pragma once

#include <QString>

namespace platform {

struct WakeOnLanSendResult {
    bool success = false;
    QString errorMessage;
};

class WakeOnLanSender final {
public:
    static WakeOnLanSendResult send(const QString& macAddress, const QString& broadcastIp = QString());
};

} // namespace platform
