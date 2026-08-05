#include "system/NetworkInterfacePolicy.h"

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAILED: " << message << '\n';
    return condition;
}

// =====wjy====
bool virtualAdaptersAreRejectedWithoutHidingPhysicalLanAdapters()
{
    return expect(platform::isVirtualLanInterface(
                      QNetworkInterface::Virtual,
                      QStringLiteral("virtual0"),
                      QStringLiteral("Virtual Adapter")),
                  "Qt virtual interface type must be rejected")
        && expect(platform::isVirtualLanInterface(
                      QNetworkInterface::Ethernet,
                      QStringLiteral("ethernet_2"),
                      QStringLiteral("VMware Network Adapter VMnet8")),
                  "VMware Ethernet-style adapter must be rejected")
        && expect(platform::isVirtualLanInterface(
                      QNetworkInterface::Ethernet,
                      QStringLiteral("vEthernet (Default Switch)"),
                      QStringLiteral("Hyper-V Virtual Ethernet Adapter")),
                  "Hyper-V switch adapter must be rejected")
        && expect(!platform::isVirtualLanInterface(
                       QNetworkInterface::Ethernet,
                       QStringLiteral("ethernet_1"),
                       QStringLiteral("Realtek PCIe GbE Family Controller")),
                   "physical Realtek adapter must remain eligible")
        && expect(!platform::isVirtualLanInterface(
                       QNetworkInterface::Wifi,
                       QStringLiteral("wlan_1"),
                       QStringLiteral("Intel(R) Wi-Fi 6 AX201")),
                   "physical Wi-Fi adapter must remain eligible"); // wjy: 回归覆盖现场 VMnet8 冲突，同时保护常见真实有线和无线接口。
}
// ===end====

} // namespace

int main()
{
    return virtualAdaptersAreRejectedWithoutHidingPhysicalLanAdapters() ? 0 : 1; // wjy: 纯策略测试不枚举或修改开发机真实网卡。
}
