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
    static QString localDeviceName(); // wjy: 只读取当前 Windows 计算机名，不枚举网卡，供启动阶段快速计算设备专属默认策略。
    static DeviceInfo local();
    // =====wjy====
    static QString physicalLanIpv4ForTarget(const QString& targetIp); // wjy: 为局域网 TCP 客户端选择真实物理源地址，阻止 Meta/TUN 默认路由抢占出站连接。
    // ===end====
};

} // namespace platform
