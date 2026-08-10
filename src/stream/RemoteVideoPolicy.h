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
    RemoteResolutionTier resolution = RemoteResolutionTier::Native; // wjy: 只填写P1080/P720/P360等档位，宽高由统一换算逻辑生成。
    std::uint32_t targetFps = 0;
    std::uint32_t priority = 0;
    bool requestsRemoteQuality = false;
    std::uint32_t maxBitrateKbps = 0; // wjy: 角色默认码率与分辨率、FPS放在同一配置源，避免协调器再维护第二套码率表。
};

struct RemoteVideoEncodingPreset {
    RemoteResolutionTier resolution = RemoteResolutionTier::Native; // wjy: 编码档案只描述画面尺寸，不把焦点或后台优先级绑定到分辨率名称上。
    std::uint32_t targetFps = 0; // wjy: 每个命名档案固定目标帧率，角色切换时不再手工拼接数值。
    std::uint32_t maxBitrateKbps = 0; // wjy: 码率与分辨率档一起切换，避免低分辨率继续沿用高清档的发送上限。
};

constexpr RemoteVideoProfile makeRemoteVideoProfile(
    const RemoteVideoEncodingPreset& preset,
    std::uint32_t priority)
{
    return {preset.resolution, preset.targetFps, priority, true, preset.maxBitrateKbps}; // wjy: 角色只补充资源优先级，分辨率、FPS和码率必须成套取自同一命名档案。
}

struct RemoteVideoDecision {
    RemoteVideoWindowRole role = RemoteVideoWindowRole::Hidden;
    RemoteVideoEmergencyTier emergency = RemoteVideoEmergencyTier::None;
    RemoteVideoProfile profile;
};

// =====wjy====
// 编码档案集中在这里，每组同时携带分辨率、FPS和对应码率上限。
inline constexpr RemoteVideoEncodingPreset kRemoteVideo1080p60Preset = {RemoteResolutionTier::P1080, 60, 48000}; // wjy: 1080p/60 FPS高清档，保留48 Mbps上限供高动态桌面使用。
inline constexpr RemoteVideoEncodingPreset kRemoteVideo1080p30Preset = {RemoteResolutionTier::P1080, 30, 48000}; // wjy: 1080p/30 FPS清晰度优先档，降帧时不同步压缩单帧质量上限。
inline constexpr RemoteVideoEncodingPreset kRemoteVideo720p60Preset = {RemoteResolutionTier::P720, 60, 24000}; // wjy: 720p/60 FPS流畅档，使用现有720p对应的24 Mbps码率上限。
inline constexpr RemoteVideoEncodingPreset kRemoteVideo720p30Preset = {RemoteResolutionTier::P720, 30, 24000}; // wjy: 720p/30 FPS平衡档，与60 FPS档共享同一清晰度预算。
inline constexpr RemoteVideoEncodingPreset kRemoteVideo540p30Preset = {RemoteResolutionTier::P540, 30, 14000}; // wjy: 540p/30 FPS当前焦点档，码率同步收敛到540p的14 Mbps上限。
inline constexpr RemoteVideoEncodingPreset kRemoteVideo540p25Preset = {RemoteResolutionTier::P540, 25, 14000}; // wjy: 540p/25 FPS当前可见后台档，减少失焦窗口的编码和呈现频率。
inline constexpr RemoteVideoEncodingPreset kRemoteVideo360p30Preset = {RemoteResolutionTier::P360, 30, 7000}; // wjy: 360p/30 FPS低清交互档，用于需要继续操作但优先节省资源的场景。
inline constexpr RemoteVideoEncodingPreset kRemoteVideo360p1Preset = {RemoteResolutionTier::P360, 1, 7000}; // wjy: 360p/1 FPS保活档，保持连接和画面更新能力以便快速恢复。

// 角色画质只选择上面的命名档案并补充优先级，以后切档不再手工改五个字段。
inline constexpr RemoteVideoProfile kFocusedRemoteVideoProfile = makeRemoteVideoProfile(kRemoteVideo540p30Preset, 100); // wjy: 当前焦点窗口选用540p/30 FPS档并获得最高优先级。
inline constexpr RemoteVideoProfile kVisibleBackgroundRemoteVideoProfile = makeRemoteVideoProfile(kRemoteVideo540p25Preset, 40); // wjy: 当前可见后台窗口选用540p/25 FPS档和中等资源优先级。
inline constexpr RemoteVideoProfile kMinimizedRemoteVideoProfile = makeRemoteVideoProfile(kRemoteVideo360p1Preset, 5); // wjy: 最小化和完全遮挡窗口选用360p/1 FPS保活档和最低优先级。
inline constexpr std::uint32_t kRemoteProfileFocusDebounceMs = 350;
inline constexpr std::uint32_t kRemotePressureEnterHoldMs = 1000;
inline constexpr std::uint32_t kRemotePressureRecoveryHoldMs = 3000;
// ===end====

class RemoteVideoPolicy final {
public:
    static constexpr RemoteVideoProfile backgroundProfile()
    {
        return kVisibleBackgroundRemoteVideoProfile; // wjy: 正常可见后台直接返回角色别名选中的完整编码档案。
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
