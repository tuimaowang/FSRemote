#pragma once

#include <QNetworkInterface>
#include <QString>
#include <QStringList>

namespace platform {

// =====wjy====
inline bool isVirtualLanInterface(
    QNetworkInterface::InterfaceType type,
    const QString& systemName,
    const QString& displayName)
{
    if (type == QNetworkInterface::Virtual) {
        return true; // wjy: Qt 已明确识别为虚拟类型时直接排除，禁止它参与真实局域网广播域判断。
    }

    const QString identity = (systemName + QLatin1Char(' ') + displayName).toCaseFolded(); // wjy: Windows 上 VMware 网卡常被报告为 Ethernet，只能结合系统名和友好名称识别。
    static const QStringList virtualMarkers {
        QStringLiteral("vmware"),
        QStringLiteral("vmnet"),
        QStringLiteral("vethernet"),
        QStringLiteral("hyper-v"),
        QStringLiteral("default switch"),
        QStringLiteral("virtualbox"),
        QStringLiteral("wsl"),
        QStringLiteral("docker"),
        QStringLiteral("npcap"),
    }; // wjy: 只列出会创建本机虚拟广播域的常见适配器标记，不用宽泛的“virtual”误伤真实厂商网卡名称。
    for (const QString& marker : virtualMarkers) {
        if (identity.contains(marker)) {
            return true; // wjy: 命中任一虚拟交换/Host-Only/NAT 标记后，不再把其子网当作公司真实直连网段。
        }
    }
    return false; // wjy: 普通 Realtek/Intel 有线或无线网卡继续参与定向广播和同网段判定。
}
// ===end====

} // namespace platform
