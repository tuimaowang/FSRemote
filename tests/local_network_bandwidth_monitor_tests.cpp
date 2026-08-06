#include "system/LocalNetworkBandwidthMonitor.h"

#include <cmath>
#include <iostream>

namespace {

// =====wjy====
bool expect(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAILED: " << message << '\n';
    return condition;
}

bool near(double left, double right, double tolerance = 0.0001)
{
    return std::abs(left - right) <= tolerance; // wjy: Mbps 和百分比用容差比较，避免浮点舍入造成伪失败。
}

bool directionMetricsUseDecimalMbpsAndClampHeadroom()
{
    const auto normal = platform::calculateLocalNetworkDirectionMetrics(
        50'000'000,
        1.0,
        1'000'000'000); // wjy: 一秒 50 MB 对应 400 Mbps，千兆网卡理论余量应为 600 Mbps。
    const auto saturated = platform::calculateLocalNetworkDirectionMetrics(
        20'000'000,
        1.0,
        100'000'000); // wjy: 实测速率超过协商容量时余量必须钳制为零并标记接近饱和。
    return expect(near(normal.currentMbps, 400.0), "50 MB/s must equal 400 Mbps")
        && expect(near(normal.headroomMbps, 600.0), "gigabit headroom must be 600 Mbps")
        && expect(near(normal.utilizationPercent, 40.0), "gigabit utilization must be 40 percent")
        && expect(normal.risk == platform::LocalNetworkBandwidthRisk::Normal, "40 percent must be normal")
        && expect(near(saturated.headroomMbps, 0.0), "negative headroom must be clamped to zero")
        && expect(saturated.risk == platform::LocalNetworkBandwidthRisk::Saturated, "over capacity must be saturated");
}

bool riskThresholdsAndCounterResetAreStable()
{
    return expect(platform::classifyLocalNetworkBandwidthRisk(69.9) == platform::LocalNetworkBandwidthRisk::Normal,
               "below 70 percent must be normal")
        && expect(platform::classifyLocalNetworkBandwidthRisk(70.0) == platform::LocalNetworkBandwidthRisk::Attention,
            "70 percent must request attention")
        && expect(platform::classifyLocalNetworkBandwidthRisk(85.0) == platform::LocalNetworkBandwidthRisk::High,
            "85 percent must be high risk")
        && expect(platform::classifyLocalNetworkBandwidthRisk(95.0) == platform::LocalNetworkBandwidthRisk::Saturated,
            "95 percent must be saturated")
        && expect(platform::localNetworkCounterDelta(20, 10) == 10, "monotonic counters must use their difference")
        && expect(platform::localNetworkCounterDelta(5, 10) == 0, "reset counters must not wrap"); // wjy: 驱动复位边界不能在标题栏制造巨量 Mbps。
}
// ===end====

} // namespace

int main()
{
    return directionMetricsUseDecimalMbpsAndClampHeadroom()
            && riskThresholdsAndCounterResetAreStable()
        ? 0
        : 1; // wjy: 纯计算测试不枚举、禁用或修改开发机真实网卡。
}
