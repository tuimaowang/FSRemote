#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#endif

#include "system/LocalNetworkBandwidthMonitor.h"

#include <chrono>
#include <unordered_map>
#include <utility>
#include <vector>

namespace platform {

// =====wjy====
namespace {

struct LocalAdapterCounter {
    quint32 index = 0; // wjy: Windows 接口索引稳定关联相邻两次累计计数。
    QString name;
    bool hardware = false;
    bool wireless = false;
    quint64 receiveLinkSpeed = 0;
    quint64 transmitLinkSpeed = 0;
    quint64 inOctets = 0;
    quint64 outOctets = 0;
    quint64 inDiscards = 0;
    quint64 outDiscards = 0;
    quint64 inErrors = 0;
    quint64 outErrors = 0;
};

} // namespace

struct LocalNetworkBandwidthMonitor::Impl {
    using Clock = std::chrono::steady_clock;
    std::unordered_map<quint32, LocalAdapterCounter> previousAdapters;
    Clock::time_point previousAt{};
    quint32 activeInterfaceIndex = 0; // wjy: 保持当前主网卡，低流量抖动时不在多个物理接口之间来回切换。
    bool initialized = false;
};

namespace {

#if defined(Q_OS_WIN)
std::vector<LocalAdapterCounter> queryPhysicalAdapterCounters()
{
    PMIB_IF_TABLE2 table = nullptr;
    if (::GetIfTable2(&table) != NO_ERROR || table == nullptr) {
        return {};
    }

    std::vector<LocalAdapterCounter> adapters;
    adapters.reserve(table->NumEntries);
    bool hasHardwareAdapter = false;
    for (ULONG rowIndex = 0; rowIndex < table->NumEntries; ++rowIndex) {
        const MIB_IF_ROW2& row = table->Table[rowIndex];
        if (row.OperStatus != IfOperStatusUp
            || row.MediaConnectState != MediaConnectStateConnected
            || row.Type == IF_TYPE_SOFTWARE_LOOPBACK
            || row.Type == IF_TYPE_TUNNEL) {
            continue; // wjy: 断开、回环和隧道接口不能代表控制电脑真实局域网端口容量。
        }

        LocalAdapterCounter adapter;
        adapter.index = row.InterfaceIndex;
        adapter.name = QString::fromWCharArray(row.Alias).trimmed();
        if (adapter.name.isEmpty()) {
            adapter.name = QString::fromWCharArray(row.Description).trimmed(); // wjy: 驱动未提供友好名称时回退描述，采样仍保留可诊断接口身份。
        }
        adapter.hardware = row.InterfaceAndOperStatusFlags.HardwareInterface != 0;
        adapter.wireless = row.Type == IF_TYPE_IEEE80211;
        adapter.receiveLinkSpeed = row.ReceiveLinkSpeed;
        adapter.transmitLinkSpeed = row.TransmitLinkSpeed;
        adapter.inOctets = row.InOctets;
        adapter.outOctets = row.OutOctets;
        adapter.inDiscards = row.InDiscards;
        adapter.outDiscards = row.OutDiscards;
        adapter.inErrors = row.InErrors;
        adapter.outErrors = row.OutErrors;
        hasHardwareAdapter = hasHardwareAdapter || adapter.hardware;
        adapters.push_back(std::move(adapter));
    }
    ::FreeMibTable(table); // wjy: 复制必要字段后立即释放 IP Helper 分配的表，常驻一秒采样不会泄漏。

    if (hasHardwareAdapter) {
        std::erase_if(adapters, [](const auto& adapter) {
            return !adapter.hardware; // wjy: 存在真实网卡时排除 VMware、Hyper-V 等虚拟层，避免同一数据包重复计数。
        });
    }
    return adapters;
}

quint32 defaultRouteInterfaceIndex()
{
    DWORD interfaceIndex = 0;
    return ::GetBestInterface(0x08080808, &interfaceIndex) == NO_ERROR
        ? static_cast<quint32>(interfaceIndex)
        : 0; // wjy: 8.8.8.8 只用于查询本机路由表，不发送数据；空闲时优先显示当前默认物理网卡。
}
#else
std::vector<LocalAdapterCounter> queryPhysicalAdapterCounters()
{
    return {};
}

quint32 defaultRouteInterfaceIndex()
{
    return 0;
}
#endif

struct AdapterCandidate {
    quint32 index = 0;
    LocalNetworkBandwidthSample sample;
    double activityMbps = 0.0;
};

} // namespace

LocalNetworkBandwidthMonitor::LocalNetworkBandwidthMonitor()
    : m_impl(std::make_unique<Impl>())
{
}

LocalNetworkBandwidthMonitor::~LocalNetworkBandwidthMonitor() = default;

LocalNetworkBandwidthSample LocalNetworkBandwidthMonitor::sample()
{
    const std::vector<LocalAdapterCounter> currentAdapters = queryPhysicalAdapterCounters();
    const Impl::Clock::time_point currentAt = Impl::Clock::now();
    if (currentAdapters.empty()) {
        reset(); // wjy: 网卡查询失败或全部断开后丢弃旧基线，恢复连接时重新建立而不平均跨断线时段。
        return {};
    }

    std::unordered_map<quint32, LocalAdapterCounter> currentByIndex;
    currentByIndex.reserve(currentAdapters.size());
    for (const LocalAdapterCounter& adapter : currentAdapters) {
        currentByIndex.insert_or_assign(adapter.index, adapter);
    }

    if (!m_impl->initialized) {
        m_impl->previousAdapters = std::move(currentByIndex); // wjy: 首次只保存累计计数，必须等下一秒差值才能计算真实 Mbps。
        m_impl->previousAt = currentAt;
        m_impl->initialized = true;
        return {};
    }

    const double elapsedSeconds = std::chrono::duration<double>(currentAt - m_impl->previousAt).count();
    std::vector<AdapterCandidate> candidates;
    candidates.reserve(currentAdapters.size());
    for (const LocalAdapterCounter& current : currentAdapters) {
        const auto previous = m_impl->previousAdapters.find(current.index);
        if (previous == m_impl->previousAdapters.end()) {
            continue; // wjy: 新插入或刚重连的接口先等待一周期基线，不把开机以来累计值当成瞬时流量。
        }

        AdapterCandidate candidate;
        candidate.index = current.index;
        candidate.sample.valid = elapsedSeconds > 0.0;
        candidate.sample.interfaceName = current.name;
        candidate.sample.wireless = current.wireless;
        candidate.sample.receive = calculateLocalNetworkDirectionMetrics(
            localNetworkCounterDelta(current.inOctets, previous->second.inOctets),
            elapsedSeconds,
            current.receiveLinkSpeed); // wjy: 接收方向直接使用真实物理网卡累计字节差，覆盖所有 FSRemote 窗口的汇总入站流量。
        candidate.sample.transmit = calculateLocalNetworkDirectionMetrics(
            localNetworkCounterDelta(current.outOctets, previous->second.outOctets),
            elapsedSeconds,
            current.transmitLinkSpeed);
        candidate.sample.receiveDiscardsDelta = localNetworkCounterDelta(current.inDiscards, previous->second.inDiscards);
        candidate.sample.transmitDiscardsDelta = localNetworkCounterDelta(current.outDiscards, previous->second.outDiscards);
        candidate.sample.receiveErrorsDelta = localNetworkCounterDelta(current.inErrors, previous->second.inErrors);
        candidate.sample.transmitErrorsDelta = localNetworkCounterDelta(current.outErrors, previous->second.outErrors);
        candidate.activityMbps = candidate.sample.receive.currentMbps + candidate.sample.transmit.currentMbps;
        candidates.push_back(std::move(candidate));
    }

    m_impl->previousAdapters = std::move(currentByIndex); // wjy: 无论本轮是否有可展示候选，都推进全部物理网卡基线供下一秒计算。
    m_impl->previousAt = currentAt;
    if (candidates.empty()) {
        return {};
    }

    const AdapterCandidate* busiest = &candidates.front();
    const AdapterCandidate* previousActive = nullptr;
    const AdapterCandidate* defaultRoute = nullptr;
    const quint32 routeIndex = defaultRouteInterfaceIndex();
    for (const AdapterCandidate& candidate : candidates) {
        if (candidate.sample.receive.currentMbps > busiest->sample.receive.currentMbps
            || (candidate.sample.receive.currentMbps == busiest->sample.receive.currentMbps
                && candidate.activityMbps > busiest->activityMbps)) {
            busiest = &candidate; // wjy: 多路远控主要是入站视频，优先选择接收 Mbps 最大的物理接口作为标题栏代表。
        }
        if (candidate.index == m_impl->activeInterfaceIndex) {
            previousActive = &candidate;
        }
        if (candidate.index == routeIndex) {
            defaultRoute = &candidate;
        }
    }

    const AdapterCandidate* selected = busiest;
    if (busiest->sample.receive.currentMbps < 0.1 && defaultRoute != nullptr) {
        selected = defaultRoute; // wjy: 几乎空闲时使用默认路由接口，标题栏不会随机跳到只有广播包的其它网卡。
    } else if (previousActive != nullptr
        && previousActive->sample.receive.currentMbps >= busiest->sample.receive.currentMbps * 0.75) {
        selected = previousActive; // wjy: 原接口仍承担至少 75% 主流量时保持显示，消除相近流量导致的每秒名称和容量抖动。
    }
    m_impl->activeInterfaceIndex = selected->index;
    return selected->sample;
}

void LocalNetworkBandwidthMonitor::reset()
{
    m_impl->previousAdapters.clear();
    m_impl->previousAt = {};
    m_impl->activeInterfaceIndex = 0;
    m_impl->initialized = false; // wjy: 清除全部跨采样状态，下一次调用严格重新建立基线。
}
// ===end====

} // namespace platform
