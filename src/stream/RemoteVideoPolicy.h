#pragma once

#include "stream/RemoteQualityPolicy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace stream {

// =====wjy====
enum class RemoteVideoWindowRole : std::uint8_t {
    Hidden,
    Focused,
    VisibleBackground,
    Minimized,
};

enum class RemoteVideoEmergencyTier : std::uint8_t {
    None,
    Fps15,
    Fps10,
    Fps5,
    Fps3,
    Fps1,
};

struct RemoteVideoWindowState {
    std::uint64_t windowId = 0;
    bool connected = false;
    bool visible = false;
    bool minimized = false;
    bool surfaceReady = false;
    bool focused = false;
};

struct RemoteVideoPressureSignals {
    bool workerBacklog = false; // wjy: RenderWorker排队超过有界预算。
    bool presentDrops = false; // wjy: 最近窗口呈现丢帧率超过阈值。
    bool frameAgeHigh = false; // wjy: 解码到显示的p95帧龄超过阈值。
    bool syncBusy = false; // wjy: Fence/Keyed Mutex忙等待比例超过阈值。
    bool renderUtilizationHigh = false; // wjy: Adapter RenderWorker持续接近满载。
    bool decodeQueueAgeHigh = false; // wjy: 解码线程或WebRTC渲染队列出现积压。
    bool deviceFailure = false; // wjy: 设备级故障立即进入最保守后台档位。
};

struct RemoteVideoProfile {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t targetFps = 0;
    std::uint32_t priority = 0;
    bool requestsRemoteQuality = false;
    std::uint32_t maxBitrateKbps = 0; // wjy: 角色默认码率与分辨率、FPS放在同一配置源，避免协调器再维护第二套码率表。
    RemoteResolutionTier resolution = RemoteResolutionTier::Native; // wjy: 角色分辨率档位也纳入统一配置，修改一处即可改变最终下发尺寸。
};

struct RemoteVideoDecision {
    RemoteVideoWindowRole role = RemoteVideoWindowRole::Hidden;
    RemoteVideoEmergencyTier emergency = RemoteVideoEmergencyTier::None;
    RemoteVideoProfile profile;
};

// =====wjy====
// 画质总开关集中在这里，后续只改常量即可调整所有窗口的角色画质。
inline constexpr RemoteVideoProfile kFocusedRemoteVideoProfile = {1920, 1080, 60, 100, true, 48000, RemoteResolutionTier::P1080}; // wjy: 单窗口和多窗口焦点统一使用1080p/60/48Mbps。
inline constexpr RemoteVideoProfile kVisibleBackgroundRemoteVideoProfile = {1280, 720, 30, 40, true, 24000, RemoteResolutionTier::P720}; // wjy: 可见后台固定720p/30/24Mbps，不按窗口数量降帧。
inline constexpr RemoteVideoProfile kMinimizedRemoteVideoProfile = {640, 360, 1, 5, true, 7000, RemoteResolutionTier::P360}; // wjy: 最小化只保留360p/1FPS保活。
inline constexpr std::uint32_t kRemoteProfileFocusDebounceMs = 350;
inline constexpr std::uint32_t kRemotePressureEnterHoldMs = 1000;
inline constexpr std::uint32_t kRemotePressureRecoveryHoldMs = 3000;
// ===end====

class RemoteVideoPolicy final {
public:
    static constexpr RemoteVideoProfile backgroundProfile()
    {
        return kVisibleBackgroundRemoteVideoProfile; // wjy: 正常可见后台固定720p/30，统一由顶部变量调整。
    }

    static constexpr RemoteVideoProfile minimizedProfile()
    {
        return kMinimizedRemoteVideoProfile; // wjy: 最小化保活档统一由顶部变量调整。
    }

    static RemoteVideoWindowRole resolveRole(
        const RemoteVideoWindowState& state,
        std::size_t visibleWindowCount)
    {
        (void)visibleWindowCount; // wjy: 角色本身只依赖当前窗口状态，数量由调度器用于预算和日志。
        if (!state.connected || !state.surfaceReady) {
            return RemoteVideoWindowRole::Hidden; // wjy: 未连接、无表面或不可见窗口不参与视频预算。
        }
        if (state.minimized) {
            return RemoteVideoWindowRole::Minimized; // wjy: 最小化窗口不会抢占焦点窗口的呈现时隙。
        }
        if (!state.visible) {
            return RemoteVideoWindowRole::Hidden; // wjy: 非最小化但不可见的窗口不请求正常视频档位。
        }
        return state.focused
            ? RemoteVideoWindowRole::Focused
            : RemoteVideoWindowRole::VisibleBackground; // wjy: 焦点是高质量保留资格，其他可见窗口走固定后台档位。
    }

    static bool pressureActive(const RemoteVideoPressureSignals& pressureSignals)
    {
        if (pressureSignals.deviceFailure) {
            return true; // wjy: 设备失败无需等待多信号，立即触发安全回退。
        }
        const int count = static_cast<int>(pressureSignals.workerBacklog)
            + static_cast<int>(pressureSignals.presentDrops)
            + static_cast<int>(pressureSignals.frameAgeHigh)
            + static_cast<int>(pressureSignals.syncBusy)
            + static_cast<int>(pressureSignals.renderUtilizationHigh)
            + static_cast<int>(pressureSignals.decodeQueueAgeHigh);
        return count >= 2; // wjy: 至少两个独立指标同时异常才降级，避免单一噪声造成画质抖动。
    }

    static RemoteVideoEmergencyTier emergencyTier(const RemoteVideoPressureSignals& pressureSignals)
    {
        if (pressureSignals.deviceFailure) {
            return RemoteVideoEmergencyTier::Fps1; // wjy: 设备级错误时后台只保留最小安全刷新，优先等待重建。
        }
        const int count = static_cast<int>(pressureSignals.workerBacklog)
            + static_cast<int>(pressureSignals.presentDrops)
            + static_cast<int>(pressureSignals.frameAgeHigh)
            + static_cast<int>(pressureSignals.syncBusy)
            + static_cast<int>(pressureSignals.renderUtilizationHigh)
            + static_cast<int>(pressureSignals.decodeQueueAgeHigh);
        if (count >= 5) return RemoteVideoEmergencyTier::Fps3; // wjy: 多项持续异常时进一步降低后台调度压力。
        if (count >= 4) return RemoteVideoEmergencyTier::Fps5;
        if (count >= 3) return RemoteVideoEmergencyTier::Fps10;
        if (count >= 2) return RemoteVideoEmergencyTier::Fps15;
        return RemoteVideoEmergencyTier::None; // wjy: 未达到双信号门槛时保持正常固定后台档位。
    }

    static RemoteVideoProfile profileFor(
        RemoteVideoWindowRole role,
        const RemoteVideoProfile& focusedProfile,
        RemoteVideoEmergencyTier emergency)
    {
        switch (role) {
        case RemoteVideoWindowRole::Focused:
            return focusedProfile; // wjy: 焦点窗口优先保持用户选择的尺寸和60 FPS预算。
        case RemoteVideoWindowRole::VisibleBackground:
            return backgroundProfile(); // wjy: 压力状态不再改写正常后台FPS，避免恢复旧的设备数量降帧策略。
        case RemoteVideoWindowRole::Minimized:
            return minimizedProfile();
        case RemoteVideoWindowRole::Hidden:
            return {}; // wjy: 隐藏窗口不请求远端视频，也不参与RenderWorker调度。
        }
        return {};
    }

    static std::vector<RemoteVideoDecision> resolve(
        const std::vector<RemoteVideoWindowState>& states,
        const RemoteVideoProfile& focusedProfile,
        const RemoteVideoPressureSignals& pressureSignals)
    {
        const auto emergency = pressureActive(pressureSignals)
            ? emergencyTier(pressureSignals)
            : RemoteVideoEmergencyTier::None; // wjy: 只有双信号压力成立后才把后台降级档位应用到所有决策。
        std::size_t visibleCount = 0;
        for (const auto& state : states) {
            if (state.connected && state.surfaceReady && state.visible && !state.minimized) {
                ++visibleCount;
            }
        }

        std::vector<RemoteVideoDecision> decisions;
        decisions.reserve(states.size());
        for (const auto& state : states) {
            const auto role = resolveRole(state, visibleCount);
            decisions.push_back({role, emergency, profileFor(role, focusedProfile, emergency)});
        }
        return decisions; // wjy: 返回顺序与输入窗口一致，调用方可按稳定windowId应用并记录策略变化。
    }
};

class RemoteVideoPressureController final {
public:
    RemoteVideoEmergencyTier update(const RemoteVideoPressureSignals& pressureSignals, std::int64_t nowMs)
    {
        if (pressureSignals.deviceFailure) {
            pressureSinceMs_ = nowMs;
            recoverySinceMs_ = 0;
            tier_ = RemoteVideoEmergencyTier::Fps1;
            return tier_; // wjy: 设备失败不等待双信号持有时间，立即进入1 FPS安全档。
        }

        if (RemoteVideoPolicy::pressureActive(pressureSignals)) {
            recoverySinceMs_ = 0;
            if (pressureSinceMs_ == 0) {
                pressureSinceMs_ = nowMs;
            }
            if (nowMs - pressureSinceMs_ >= static_cast<std::int64_t>(kRemotePressureEnterHoldMs)) {
                const auto requested = RemoteVideoPolicy::emergencyTier(pressureSignals);
                if (severity(requested) > severity(tier_)) {
                    tier_ = requested; // wjy: 持续压力只允许向更保守档位移动，避免信号抖动时反复升降。
                }
            }
            return tier_;
        }

        pressureSinceMs_ = 0;
        if (tier_ == RemoteVideoEmergencyTier::None) {
            recoverySinceMs_ = 0;
            return tier_;
        }
        if (recoverySinceMs_ == 0) {
            recoverySinceMs_ = nowMs;
        }
        if (nowMs - recoverySinceMs_ >= static_cast<std::int64_t>(kRemotePressureRecoveryHoldMs)) {
            tier_ = recoverOneStep(tier_); // wjy: 健康保持3秒后仅恢复一级，防止瞬间回到30 FPS再次过载。
            recoverySinceMs_ = nowMs;
        }
        return tier_;
    }

    RemoteVideoEmergencyTier tier() const noexcept { return tier_; }
    void reset() noexcept
    {
        tier_ = RemoteVideoEmergencyTier::None;
        pressureSinceMs_ = 0;
        recoverySinceMs_ = 0;
    }

private:
    static int severity(RemoteVideoEmergencyTier tier) noexcept
    {
        return static_cast<int>(tier);
    }

    static RemoteVideoEmergencyTier recoverOneStep(RemoteVideoEmergencyTier tier) noexcept
    {
        switch (tier) {
        case RemoteVideoEmergencyTier::Fps1: return RemoteVideoEmergencyTier::Fps3;
        case RemoteVideoEmergencyTier::Fps3: return RemoteVideoEmergencyTier::Fps5;
        case RemoteVideoEmergencyTier::Fps5: return RemoteVideoEmergencyTier::Fps10;
        case RemoteVideoEmergencyTier::Fps10: return RemoteVideoEmergencyTier::Fps15;
        case RemoteVideoEmergencyTier::Fps15: return RemoteVideoEmergencyTier::None;
        case RemoteVideoEmergencyTier::None: return RemoteVideoEmergencyTier::None;
        }
        return RemoteVideoEmergencyTier::None;
    }

    RemoteVideoEmergencyTier tier_ = RemoteVideoEmergencyTier::None;
    std::int64_t pressureSinceMs_ = 0;
    std::int64_t recoverySinceMs_ = 0;
};
// ===end====

} // namespace stream
