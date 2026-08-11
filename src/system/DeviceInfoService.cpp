#include "system/DeviceInfoService.h"
#include "system/NetworkInterfacePolicy.h"

#include <QHostAddress>

#include <chrono>
#include <mutex>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

namespace platform {
namespace {

QString currentComputerName()
{
#if defined(Q_OS_WIN)
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (GetComputerNameW(buffer, &size) && size > 0) {
        return QString::fromWCharArray(buffer, static_cast<int>(size));
    }
#endif
    return qEnvironmentVariable("COMPUTERNAME").trimmed();
}

QString formatMac(const BYTE* bytes, ULONG length)
{
    if (!bytes || length == 0) {
        return {};
    }

    QStringList parts;
    parts.reserve(static_cast<int>(length));
    for (ULONG i = 0; i < length; ++i) {
        parts.append(QStringLiteral("%1").arg(bytes[i], 2, 16, QLatin1Char('0')).toUpper());
    }
    return parts.join(QLatin1Char(':'));
}

quint32 prefixMask(ULONG prefixLength)
{
    if (prefixLength >= 32) {
        return 0xFFFFFFFFu;
    }
    if (prefixLength == 0) {
        return 0;
    }
    return 0xFFFFFFFFu << (32 - prefixLength);
}

bool isPrivateIpv4(quint32 hostAddress)
{
    const quint8 a = static_cast<quint8>((hostAddress >> 24) & 0xFF);
    const quint8 b = static_cast<quint8>((hostAddress >> 16) & 0xFF);
    if (a == 10) {
        return true;
    }
    if (a == 172 && b >= 16 && b <= 31) {
        return true;
    }
    if (a == 192 && b == 168) {
        return true;
    }
    return false;
}

QString ipv4ToString(quint32 hostAddress)
{
    return QHostAddress(hostAddress).toString();
}

QString subnetMaskForPrefix(ULONG prefixLength)
{
    return ipv4ToString(prefixMask(prefixLength));
}

QString broadcastAddressFor(const SOCKADDR* socketAddress, ULONG prefixLength)
{
    if (!socketAddress || socketAddress->sa_family != AF_INET) {
        return {};
    }

    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(socketAddress);
    const quint32 hostAddress = ntohl(ipv4->sin_addr.s_addr);
    const quint32 mask = prefixMask(prefixLength);
    const quint32 broadcast = hostAddress | (~mask);
    return ipv4ToString(broadcast);
}

QString gatewayAddressFor(const IP_ADAPTER_ADDRESSES* adapter)
{
    if (!adapter) {
        return {};
    }

    for (auto* gateway = adapter->FirstGatewayAddress; gateway; gateway = gateway->Next) {
        if (!gateway->Address.lpSockaddr || gateway->Address.lpSockaddr->sa_family != AF_INET) {
            continue;
        }
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(gateway->Address.lpSockaddr);
        const quint32 hostAddress = ntohl(ipv4->sin_addr.s_addr);
        if (hostAddress == 0) {
            continue;
        }
        return ipv4ToString(hostAddress);
    }
    return {};
}

DWORD bestDefaultIpv4InterfaceIndex()
{
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = inet_addr("1.1.1.1");

    DWORD bestIfIndex = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination), &bestIfIndex) != NO_ERROR) {
        return 0;
    }
    return bestIfIndex;
}

DeviceInfo currentDeviceInfo()
{
    DeviceInfo info;
    info.name = currentComputerName();

#if !defined(Q_OS_WIN)
    return info;
#else
    ULONG size = 0;
    const ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS;
    DWORD result = GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size);
    if (result != ERROR_BUFFER_OVERFLOW || size == 0) {
        return info;
    }

    QByteArray buffer;
    buffer.resize(static_cast<int>(size));
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (result != NO_ERROR) {
        return info;
    }

    const DWORD bestIfIndex = bestDefaultIpv4InterfaceIndex();
    int bestScore = -1;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType == IF_TYPE_TUNNEL) {
            continue;
        }
        // =====wjy====
        const QString adapterSystemName = QString::fromLatin1(adapter->AdapterName ? adapter->AdapterName : ""); // wjy: Windows 内部适配器名用于识别被伪装成 Ethernet 的代理/TUN 网卡。
        const QString adapterDisplayName = adapter->FriendlyName
            ? QString::fromWCharArray(adapter->FriendlyName)
            : QString(); // wjy: 友好名称能直接识别现场名为 Meta 的虚拟出口。
        if (isVirtualLanInterface(QNetworkInterface::Unknown, adapterSystemName, adapterDisplayName)) {
            continue; // wjy: 默认路由即使指向 Meta，也禁止它获得最高分并成为本机主局域网地址。
        }
        // ===end====
        if (adapter->PhysicalAddressLength < 6) {
            continue;
        }

        QString broadcastIp;
        QString ipv4Address;
        QString subnetMask;
        QString gatewayAddress = gatewayAddressFor(adapter);
        bool hasPrivateIpv4 = false;
        bool hasIpv4 = false;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            if (ipv4->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
                continue;
            }
            const quint32 hostAddress = ntohl(ipv4->sin_addr.s_addr);
            if (hostAddress == 0 || (hostAddress >> 16) == ((169u << 8) | 254u)) {
                continue;
            }

            const bool privateIpv4 = isPrivateIpv4(hostAddress);
            if (!hasIpv4 || privateIpv4) {
                ipv4Address = ipv4ToString(hostAddress);
                subnetMask = subnetMaskForPrefix(unicast->OnLinkPrefixLength);
                broadcastIp = broadcastAddressFor(unicast->Address.lpSockaddr, unicast->OnLinkPrefixLength);
                hasIpv4 = true;
                hasPrivateIpv4 = privateIpv4;
            }
            if (privateIpv4) {
                break;
            }
        }

        int score = 0;
        if (bestIfIndex != 0 && adapter->IfIndex == bestIfIndex) {
            score += 5000;
        }
        if (adapter->OperStatus == IfOperStatusUp) {
            score += 1000;
        }
        if (hasIpv4) {
            score += 50;
        }
        if (hasPrivateIpv4) {
            score += 1000;
        }
        if (!gatewayAddress.isEmpty()) {
            score += 500;
        }

        if (score <= bestScore) {
            continue;
        }

        bestScore = score;
        info.ip = ipv4Address;
        info.subnetMask = subnetMask;
        info.gateway = gatewayAddress;
        info.mac = formatMac(adapter->PhysicalAddress, adapter->PhysicalAddressLength);
        info.broadcastIp = broadcastIp;
    }

    return info;
#endif
}
} // namespace

// =====wjy====
QString DeviceInfoService::localDeviceName()
{
    return currentComputerName(); // wjy: 复用与完整本机信息相同的宽字符计算机名来源，但跳过 GetAdaptersAddresses 等较重的网络枚举。
}
// ===end====

DeviceInfo DeviceInfoService::local()
{
    return currentDeviceInfo();
}

// =====wjy====
QString DeviceInfoService::physicalLanIpv4ForTarget(const QString& targetIp)
{
    const QHostAddress targetAddress(targetIp.trimmed()); // wjy: 当前协议传入 IPv4；解析失败时仍可回退到主物理局域网地址。
    bool targetOk = false;
    const quint32 targetValue = targetAddress.toIPv4Address(&targetOk);
    if (targetOk && targetAddress.isLoopback()) {
        return targetAddress.toString(); // wjy: 本机回环测试必须继续绑定 127.0.0.1，不能错误改走物理网卡。
    }

    struct Candidate {
        QString ip;
        quint32 address = 0;
        quint32 netmask = 0;
        int baseScore = 0;
    };
    static std::mutex cacheMutex; // wjy: 设备刷新最多并发八个线程，网卡枚举必须串行刷新同一份短期快照。
    static QList<Candidate> cachedCandidates;
    static bool cacheInitialized = false;
    static std::chrono::steady_clock::time_point cacheExpiresAt;

    std::lock_guard cacheLock(cacheMutex);
    const auto now = std::chrono::steady_clock::now();
    if (!cacheInitialized || now >= cacheExpiresAt) {
        cacheInitialized = true;
        cacheExpiresAt = now + std::chrono::seconds(3); // wjy: 一轮并发状态刷新复用一次枚举，插拔/切换网卡最迟三秒后重新选择。
        cachedCandidates.clear();
        const QString preferredIp = currentDeviceInfo().ip.trimmed(); // wjy: 快照刷新时只调用一次较重的 Windows 适配器枚举，避免每台设备重复 GetAdaptersAddresses。
        for (const QNetworkInterface& networkInterface : QNetworkInterface::allInterfaces()) {
            const auto flags = networkInterface.flags();
            if (!flags.testFlag(QNetworkInterface::IsUp)
                || !flags.testFlag(QNetworkInterface::IsRunning)
                || flags.testFlag(QNetworkInterface::IsLoopBack)
                || flags.testFlag(QNetworkInterface::IsPointToPoint)
                || isVirtualLanInterface(
                    networkInterface.type(),
                    networkInterface.name(),
                    networkInterface.humanReadableName())) {
                continue; // wjy: 只允许当前可用的真实局域网接口进入快照，排除 VPN、Meta、VMware 和 Hyper-V。
            }

            for (const QNetworkAddressEntry& entry : networkInterface.addressEntries()) {
                bool localOk = false;
                const quint32 localValue = entry.ip().toIPv4Address(&localOk);
                if (!localOk
                    || localValue == 0
                    || entry.ip().isLoopback()
                    || (localValue >> 16) == ((169u << 8) | 254u)) {
                    continue; // wjy: 跳过 IPv6、空地址、回环和 APIPA，避免缓存无法访问目标设备的临时地址。
                }

                Candidate candidate;
                candidate.ip = QHostAddress(localValue).toString();
                candidate.address = localValue;
                bool maskOk = false;
                candidate.netmask = entry.netmask().toIPv4Address(&maskOk);
                if (!maskOk) candidate.netmask = 0;
                if (!preferredIp.isEmpty() && candidate.ip == preferredIp) {
                    candidate.baseScore += 10000; // wjy: 跨网段访问 A10 等设备时优先使用 Windows 主物理局域网接口及其真实网关。
                }
                if (isPrivateIpv4(localValue)) {
                    candidate.baseScore += 1000; // wjy: FSRemote 设备地址属于 RFC1918，私有物理地址优先于其它非局域网地址。
                }
                if (flags.testFlag(QNetworkInterface::CanBroadcast)) {
                    candidate.baseScore += 100; // wjy: 可广播接口更符合有线/无线局域网特征，但不会覆盖主接口优先级。
                }
                cachedCandidates.append(candidate); // wjy: 快照仅保存选择所需字段，不持有 QNetworkInterface 系统资源。
            }
        }
    }

    QString selectedIp;
    int selectedScore = -1;
    for (const Candidate& candidate : cachedCandidates) {
        int score = candidate.baseScore;
        if (targetOk && candidate.netmask != 0
            && (candidate.address & candidate.netmask) == (targetValue & candidate.netmask)) {
            score += 100000; // wjy: 目标与某块物理网卡同网段时直接选择该接口，不受其它默认网关优先级影响。
        }
        if (score > selectedScore) {
            selectedScore = score;
            selectedIp = candidate.ip; // wjy: 每个目标只对轻量快照评分，后续 QTcpSocket 在 connectToHost 前绑定该源 IP。
        }
    }
    return selectedIp;
}
// ===end====

} // namespace platform
