#include "system/DeviceInfoService.h"

#include <QHostAddress>

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

DeviceInfo DeviceInfoService::local()
{
    return currentDeviceInfo();
}

} // namespace platform
