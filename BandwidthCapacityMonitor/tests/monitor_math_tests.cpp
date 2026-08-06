#include "MonitorMath.h"

#include <cassert>
#include <cmath>

namespace {

// =====wjy====
bool near(double left, double right, double tolerance = 0.0001)
{
    return std::abs(left - right) <= tolerance; // wjy: 浮点结果使用容差比较，避免二进制小数舍入造成伪失败。
}
void testRateAndLinkHeadroom()
{
    const auto metrics = bandwidth_monitor::calculateDirection(
        50'000'000,
        1.0,
        1'000'000'000,
        0.0); // wjy: 一秒 50 MB 对应 400 Mbps，千兆链路应剩余 600 Mbps。
    assert(near(metrics.currentMbps, 400.0));
    assert(near(metrics.capacityMbps, 1000.0));
    assert(near(metrics.headroomMbps, 600.0));
    assert(near(metrics.utilizationPercent, 40.0));
    assert(metrics.risk == bandwidth_monitor::RiskLevel::Normal);
}

void testEffectiveCapacityOverride()
{
    const auto metrics = bandwidth_monitor::calculateDirection(
        100'000'000,
        2.0,
        1'000'000'000,
        500.0); // wjy: 手工输入 500 Mbps 有效容量时，400 Mbps 实际流量应按 80% 计算。
    assert(near(metrics.currentMbps, 400.0));
    assert(near(metrics.capacityMbps, 500.0));
    assert(near(metrics.headroomMbps, 100.0));
    assert(near(metrics.utilizationPercent, 80.0));
    assert(metrics.risk == bandwidth_monitor::RiskLevel::Attention);
}

void testRiskThresholdsAndSaturationClamp()
{
    assert(bandwidth_monitor::classifyRisk(69.9) == bandwidth_monitor::RiskLevel::Normal);
    assert(bandwidth_monitor::classifyRisk(70.0) == bandwidth_monitor::RiskLevel::Attention);
    assert(bandwidth_monitor::classifyRisk(85.0) == bandwidth_monitor::RiskLevel::High);
    assert(bandwidth_monitor::classifyRisk(95.0) == bandwidth_monitor::RiskLevel::Saturated);

    const auto saturated = bandwidth_monitor::calculateDirection(
        20'000'000,
        1.0,
        100'000'000,
        0.0); // wjy: 160 Mbps 超过 100 Mbps 容量时余量必须显示为零而不是负数。
    assert(near(saturated.headroomMbps, 0.0));
    assert(saturated.risk == bandwidth_monitor::RiskLevel::Saturated);
}

void testCounterResetAndZeroInterval()
{
    assert(bandwidth_monitor::safeCounterDelta(20, 10) == 10);
    assert(bandwidth_monitor::safeCounterDelta(5, 10) == 0); // wjy: 网卡计数器重置不能产生无符号回绕的大流量。
    assert(near(bandwidth_monitor::bytesToMbps(1000, 0.0), 0.0));
}
// ===end====

} // namespace

int main()
{
    // =====wjy====
    testRateAndLinkHeadroom(); // wjy: 验证默认链路速率口径。
    testEffectiveCapacityOverride(); // wjy: 验证手工有效容量优先级。
    testRiskThresholdsAndSaturationClamp(); // wjy: 验证拥塞风险边界和零余量钳制。
    testCounterResetAndZeroInterval(); // wjy: 验证计数器异常与时间边界。
    return 0;
    // ===end====
}
