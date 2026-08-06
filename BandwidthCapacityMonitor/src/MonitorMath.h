#pragma once

#include <algorithm>
#include <cstdint>

namespace bandwidth_monitor {

// =====wjy====
enum class RiskLevel {
    Normal,
    Attention,
    High,
    Saturated,
}; // wjy: 用稳定枚举表达网卡方向风险，控制台文字和单元测试共用同一判断结果。

struct DirectionMetrics {
    double currentMbps = 0.0; // wjy: 当前采样周期实际通过的兆比特每秒。
    double capacityMbps = 0.0; // wjy: 优先使用手工有效容量，否则使用 Windows 报告的协商链路速率。
    double headroomMbps = 0.0; // wjy: 容量减当前速率并限制为非负值，作为本机网卡理论余量。
    double utilizationPercent = 0.0; // wjy: 当前速率占所选容量的百分比。
    RiskLevel risk = RiskLevel::Normal; // wjy: 按 70%、85%、95% 三档给出拥塞风险提示。
};

inline std::uint64_t safeCounterDelta(std::uint64_t current, std::uint64_t previous)
{
    return current >= previous ? current - previous : 0; // wjy: 网卡重连或计数器复位时不把回绕误算成极大流量。
}

inline double bytesToMbps(std::uint64_t byteDelta, double elapsedSeconds)
{
    if (elapsedSeconds <= 0.0) {
        return 0.0; // wjy: 防止系统时钟异常或零间隔采样产生除零结果。
    }
    return static_cast<double>(byteDelta) * 8.0 / elapsedSeconds / 1'000'000.0; // wjy: 使用网络常用十进制 Mbps，而不是 MiB/s。
}

inline RiskLevel classifyRisk(double utilizationPercent)
{
    if (utilizationPercent >= 95.0) {
        return RiskLevel::Saturated; // wjy: 接近链路极限时标记饱和，提示排队和丢包风险最高。
    }
    if (utilizationPercent >= 85.0) {
        return RiskLevel::High; // wjy: 超过 85% 时保留余量很小，突发视频码率容易触发卡顿。
    }
    if (utilizationPercent >= 70.0) {
        return RiskLevel::Attention; // wjy: 70% 到 85% 提前提醒观察，不直接断言已经拥塞。
    }
    return RiskLevel::Normal;
}

inline DirectionMetrics calculateDirection(
    std::uint64_t byteDelta,
    double elapsedSeconds,
    std::uint64_t linkBitsPerSecond,
    double capacityOverrideMbps)
{
    DirectionMetrics metrics;
    metrics.currentMbps = bytesToMbps(byteDelta, elapsedSeconds); // wjy: 从网卡累计字节差计算这一周期的真实平均速率。
    metrics.capacityMbps = capacityOverrideMbps > 0.0
        ? capacityOverrideMbps // wjy: 手工实测容量比协商速率更接近 Wi-Fi、交换机或上联口的真实可用上限。
        : static_cast<double>(linkBitsPerSecond) / 1'000'000.0;
    metrics.headroomMbps = std::max(0.0, metrics.capacityMbps - metrics.currentMbps); // wjy: 当前速率超过口径容量时把余量钳制为零。
    metrics.utilizationPercent = metrics.capacityMbps > 0.0
        ? metrics.currentMbps * 100.0 / metrics.capacityMbps
        : 0.0; // wjy: Windows 未报告链路速率时保留零利用率，避免伪造容量结论。
    metrics.risk = classifyRisk(metrics.utilizationPercent); // wjy: 所有展示和测试统一复用风险阈值。
    return metrics;
}
// ===end====

} // namespace bandwidth_monitor
