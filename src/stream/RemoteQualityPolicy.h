#pragma once

#include <algorithm>
#include <array>

namespace stream {

// =====wjy====
enum class RemoteQualityMode : int {
    FollowGlobal = 0,
    Automatic = 1,
    HighQualityLocked = 2,
    Balanced = 3,
    Smooth = 4,
}; // wjy: FollowGlobal只用于窗口覆盖；持久化全局默认会归一化为Automatic/高质量/均衡/流畅之一。

enum class RemoteResolutionTier : int {
    Native = 0,
    P1440 = 1,
    P1080 = 2,
    P900 = 3,
    P720 = 4,
    P540 = 5,
    P360 = 6,
}; // wjy: 固定分辨率档位只用于模式预设、最小化和软件回退，持续性能压力不再遍历这些尺寸。

struct RemoteQualityConfiguration {
    RemoteQualityMode defaultMode = RemoteQualityMode::Automatic; // wjy: 全局设置只保留默认模式；各模式的可见分辨率和FPS由固定预设决定。
    int targetFps = 60; // wjy: 兼容旧配置结构；最大可见窗口的策略目标固定为60 FPS。
    int minimumVisibleFps = 30; // wjy: 兼容旧持久化字段；普通自动策略仍以30 FPS为旧可见下限，后台数量档位由协调器单独决定。
    int severePressureMinimumFps = 30; // wjy: 兼容旧严重压力字段；可见后台窗口的1 FPS安全档由协调器数量/Presenter压力策略决定。
    int minimizedFps = 1; // wjy: 所有模式最小化后统一请求1 FPS，保留快速恢复能力并把编码/网络占用压到安全档。
    RemoteResolutionTier minimumVisibleResolution = RemoteResolutionTier::P720; // wjy: 仅保留旧设置结构兼容，真实可见分辨率完全由四种模式预设决定。
    RemoteResolutionTier minimizedResolution = RemoteResolutionTier::P360; // wjy: 所有模式最小化后统一请求640x360，与RenderWorker本地安全档保持一致。
    int largestWindowHighQualityMinArea = 1024 * 576; // wjy: 最大可见窗口达到该显示面积才允许进入高清；后续直接修改此代码默认值即可。
    int degradationHoldMs = 6000; // wjy: 只供自动模式使用；相关压力持续6秒才从60/45向下一档移动。
    int recoveryHoldMs = 3000; // wjy: 只供自动模式使用；健康持续3秒后沿30/45/60逐档恢复。
    bool automaticRecoveryEnabled = true; // wjy: 保留结构兼容，新策略始终允许自动模式恢复，固定模式不进入恢复循环。
    int aggregateReceiveBudgetMbps = 0; // wjy: 保留结构兼容但不再参与质量决策，避免总预算静默改写固定模式请求。

    bool operator==(const RemoteQualityConfiguration&) const = default;
};

inline constexpr std::array<int, 5> kRemoteFpsTiers = {60, 45, 30, 24, 15}; // wjy: 自动模式只使用60/45/30；24和15分别保留给软件回退与最小化安全档。
inline constexpr std::array<RemoteResolutionTier, 7> kRemoteResolutionTiers = {
    RemoteResolutionTier::Native,
    RemoteResolutionTier::P1440,
    RemoteResolutionTier::P1080,
    RemoteResolutionTier::P900,
    RemoteResolutionTier::P720,
    RemoteResolutionTier::P540,
    RemoteResolutionTier::P360,
}; // wjy: 顺序从高到低，模式预设和后台安全档按枚举选择固定分辨率，不用于压力循环降档。

inline bool isPersistentGlobalQualityMode(RemoteQualityMode mode)
{
    return mode == RemoteQualityMode::Automatic
        || mode == RemoteQualityMode::HighQualityLocked
        || mode == RemoteQualityMode::Balanced
        || mode == RemoteQualityMode::Smooth; // wjy: FollowGlobal不能作为全局值，避免解析时形成“全局跟随全局”的无效递归。
}

inline bool isValidResolutionTier(RemoteResolutionTier tier)
{
    const int value = static_cast<int>(tier);
    return value >= static_cast<int>(RemoteResolutionTier::Native)
        && value <= static_cast<int>(RemoteResolutionTier::P360); // wjy: 持久化和协议解析都只接受固定枚举范围。
}

inline int resolutionTierHeight(RemoteResolutionTier tier)
{
    switch (tier) {
    case RemoteResolutionTier::Native: return 0; // wjy: 0表示沿用源分辨率，不把“原始”误当成0像素。
    case RemoteResolutionTier::P1440: return 1440;
    case RemoteResolutionTier::P1080: return 1080;
    case RemoteResolutionTier::P900: return 900;
    case RemoteResolutionTier::P720: return 720;
    case RemoteResolutionTier::P540: return 540;
    case RemoteResolutionTier::P360: return 360;
    }
    return 720; // wjy: 未知值回退到稳定的720p，而不是申请无界原始分辨率。
}

inline RemoteQualityConfiguration normalizedRemoteQualityConfiguration(RemoteQualityConfiguration configuration)
{
    if (!isPersistentGlobalQualityMode(configuration.defaultMode)) {
        configuration.defaultMode = RemoteQualityMode::Automatic; // wjy: 异常或旧版本全局模式统一回到自动策略。
    }
    configuration.targetFps = std::clamp(configuration.targetFps, 15, 60); // wjy: 当前远控协议最高目标固定60 FPS，异常旧配置不得请求120/200/360导致编码与调度失配。
    configuration.minimumVisibleFps = std::clamp(
        configuration.minimumVisibleFps,
        15,
        configuration.targetFps); // wjy: 普通最低FPS不能高于目标FPS，也不能低于不断流安全档15。
    configuration.severePressureMinimumFps = std::clamp(
        configuration.severePressureMinimumFps,
        5,
        configuration.minimumVisibleFps); // wjy: 旧字段继续做边界归一化但不再参与协调器可见策略，避免破坏已有结构布局。
    configuration.minimizedFps = std::clamp(
        configuration.minimizedFps,
        1,
        configuration.targetFps); // wjy: 最小化仍至少每秒接收1帧，不能用0隐式暂停或断流。
    if (!isValidResolutionTier(configuration.minimumVisibleResolution)) {
        configuration.minimumVisibleResolution = RemoteResolutionTier::P720;
    }
    if (!isValidResolutionTier(configuration.minimizedResolution)) {
        configuration.minimizedResolution = RemoteResolutionTier::P360; // wjy: 非法旧配置回退到当前确定的最小化360p档，而不是继续请求540p。
    }
    configuration.largestWindowHighQualityMinArea = std::clamp(
        configuration.largestWindowHighQualityMinArea,
        320 * 180,
        16384 * 8640); // wjy: 限制阈值范围，避免错误配置导致所有窗口永远低质或整数溢出。
    configuration.degradationHoldMs = std::clamp(configuration.degradationHoldMs, 500, 30000); // wjy: 防止0毫秒抖动降级或异常长时间拒绝保护。
    configuration.recoveryHoldMs = std::clamp(
        configuration.recoveryHoldMs,
        500,
        120000); // wjy: 恢复和降级使用独立时间窗；恢复可以更快，档位冷却仍阻止临界负载振荡。
    configuration.aggregateReceiveBudgetMbps = std::clamp(
        configuration.aggregateReceiveBudgetMbps,
        0,
        2000); // wjy: 旧总预算字段只保留安全归一化和结构兼容，新协调器明确忽略该值。
    return configuration;
}
// ===end====

} // namespace stream
