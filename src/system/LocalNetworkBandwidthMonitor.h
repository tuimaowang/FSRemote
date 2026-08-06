#pragma once

#include <QtGlobal>
#include <QString>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace platform {

// =====wjy====
enum class LocalNetworkBandwidthRisk {
    Unknown,
    Normal,
    Attention,
    High,
    Saturated,
}; // wjy: 主标题栏按统一枚举选择普通、关注和高风险颜色，避免绘制层重新解释利用率阈值。

struct LocalNetworkDirectionMetrics {
    double currentMbps = 0.0; // wjy: 相邻采样周期内当前方向的实际平均 Mbps。
    double capacityMbps = 0.0; // wjy: Windows 报告的当前网卡协商链路速率，只作为本机端口理论容量。
    double headroomMbps = 0.0; // wjy: 理论容量减当前速率并限制为非负值，标题栏明确使用“余”表示估算余量。
    double utilizationPercent = 0.0; // wjy: 当前速率占协商容量百分比，用于判断突发视频码率是否接近端口上限。
    LocalNetworkBandwidthRisk risk = LocalNetworkBandwidthRisk::Unknown;
};

inline quint64 localNetworkCounterDelta(quint64 current, quint64 previous)
{
    return current >= previous ? current - previous : 0; // wjy: 网卡重连或驱动复位时不把无符号回绕误算成巨量流量。
}

inline LocalNetworkBandwidthRisk classifyLocalNetworkBandwidthRisk(double utilizationPercent)
{
    if (utilizationPercent >= 95.0) {
        return LocalNetworkBandwidthRisk::Saturated; // wjy: 95% 以上接近协商极限，标题栏使用最高风险提示。
    }
    if (utilizationPercent >= 85.0) {
        return LocalNetworkBandwidthRisk::High; // wjy: 85% 以上余量很小，多窗口瞬时关键帧容易形成排队。
    }
    if (utilizationPercent >= 70.0) {
        return LocalNetworkBandwidthRisk::Attention; // wjy: 70% 起提前提示关注，但不直接断言网络已经拥塞。
    }
    return LocalNetworkBandwidthRisk::Normal;
}

inline LocalNetworkDirectionMetrics calculateLocalNetworkDirectionMetrics(
    quint64 byteDelta,
    double elapsedSeconds,
    quint64 linkBitsPerSecond)
{
    LocalNetworkDirectionMetrics metrics;
    if (elapsedSeconds <= 0.0) {
        return metrics; // wjy: 首次基线或异常零间隔不生成伪造 Mbps，调用方继续显示采样中状态。
    }
    metrics.currentMbps = static_cast<double>(byteDelta) * 8.0 / elapsedSeconds / 1'000'000.0; // wjy: 使用网络设备常见的十进制 Mbps 口径。
    metrics.capacityMbps = static_cast<double>(linkBitsPerSecond) / 1'000'000.0;
    metrics.headroomMbps = std::max(0.0, metrics.capacityMbps - metrics.currentMbps); // wjy: 实际值偶尔超过协商值时显示零余量，不显示难理解的负数。
    metrics.utilizationPercent = metrics.capacityMbps > 0.0
        ? metrics.currentMbps * 100.0 / metrics.capacityMbps
        : 0.0; // wjy: 驱动没有报告链路速率时保留实际 Mbps，但不伪造利用率。
    metrics.risk = metrics.capacityMbps > 0.0
        ? classifyLocalNetworkBandwidthRisk(metrics.utilizationPercent)
        : LocalNetworkBandwidthRisk::Unknown;
    return metrics;
}

struct LocalNetworkBandwidthSample {
    bool valid = false; // wjy: 第一次调用只建立累计计数基线，标题栏在第二次采样前显示占位而非零带宽。
    QString interfaceName; // wjy: 保存当前承载主要接收流量的物理网卡名称，供诊断或后续气泡展示。
    bool wireless = false; // wjy: Wi-Fi 协商速率通常高估稳定吞吐，保留介质类型供界面明确提示口径限制。
    LocalNetworkDirectionMetrics receive; // wjy: 多路远控视频主要进入控制端，标题栏优先展示本方向。
    LocalNetworkDirectionMetrics transmit; // wjy: 同时保留控制与其它出站流量，采样器语义完整且便于后续扩展。
    quint64 receiveDiscardsDelta = 0; // wjy: 入站丢弃增长是本机接收队列或驱动压力的直接证据。
    quint64 transmitDiscardsDelta = 0;
    quint64 receiveErrorsDelta = 0;
    quint64 transmitErrorsDelta = 0;
};

class LocalNetworkBandwidthMonitor final {
public:
    LocalNetworkBandwidthMonitor();
    ~LocalNetworkBandwidthMonitor();
    LocalNetworkBandwidthMonitor(const LocalNetworkBandwidthMonitor&) = delete;
    LocalNetworkBandwidthMonitor& operator=(const LocalNetworkBandwidthMonitor&) = delete;

    LocalNetworkBandwidthSample sample(); // wjy: 被主窗口一秒定时器调用，只读网卡累计计数且绝不创建测试流量。
    void reset(); // wjy: 程序退出或重新建立采样时清除旧网卡计数基线。

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl; // wjy: Windows SDK 类型隐藏在实现文件中，DeviceGrid 头文件不会引入 winsock/windows 宏污染。
};
// ===end====

} // namespace platform
