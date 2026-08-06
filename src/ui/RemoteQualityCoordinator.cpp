#include "ui/RemoteQualityCoordinator.h"

#include "stream/RemoteVideoPolicy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace ui {

bool shouldDispatchRemoteQualityDecision(
    const RemoteQualityDecision& decision,
    bool viewerAvailable,
    bool closing)
{
    return decision.requestRemoteProfile && viewerAvailable && !closing; // wjy: 焦点未稳定时仅更新本地角色，禁止Host连续切分辨率、FPS和关键帧。
}

namespace {

// =====wjy====
struct RemoteQualityPreset {
    stream::RemoteResolutionTier resolution; // wjy: 当前角色请求的固定分辨率档位，Native 表示沿用远端原始分辨率。
    int targetFps; // wjy: 当前角色请求的FPS上限，调低它可以直接减少解码、呈现和网络压力。
    int maxBitrateKbps; // wjy: 当前角色请求的码率上限，设为0时回退到按分辨率计算的默认值。
};

struct RemoteSelectionQualityProfile {
    RemoteQualityPreset foreground; // wjy: 单选或多选场景中当前焦点/唯一窗口使用的质量。
    RemoteQualityPreset background; // wjy: 多选场景中其它可见窗口使用的质量。
    std::array<int, 5> backgroundFpsTiers; // wjy: 按可见窗口数量选择后台FPS，顺序对应 <=5、<=10、<=16、<=20、>20。
};

// wjy: 单选远控质量总开关；后续只需修改这里的分辨率、FPS或码率即可调整单窗口体验。
constexpr RemoteSelectionQualityProfile kSingleSelectionQuality = {
    {stream::RemoteResolutionTier::P1080, 60, 48000},
    {stream::RemoteResolutionTier::P720, 30, 24000},
    {30, 30, 30, 30, 30},
};

// wjy: 多选远控质量总开关；焦点窗口默认1080p/45，后台窗口按数量快速降到15/10/5/3/1 FPS。
constexpr RemoteSelectionQualityProfile kMultiSelectionQuality = {
    {stream::RemoteResolutionTier::P1080, 45, 48000},
    {stream::RemoteResolutionTier::P720, 15, 24000},
    {15, 10, 5, 3, 1},
};

const RemoteSelectionQualityProfile& selectionQualityProfile(bool singleRemoteWindow)
{
    return singleRemoteWindow ? kSingleSelectionQuality : kMultiSelectionQuality; // wjy: 根据实际远控窗口数量选择单选或多选质量变量。
}

int baselineTargetFps(stream::RemoteQualityMode mode)
{
    switch (mode) {
    case stream::RemoteQualityMode::Balanced:
        return 45; // wjy: 均衡固定1080p/45 FPS，不读取旧全局目标或接收端压力。
    case stream::RemoteQualityMode::HighQualityLocked:
    case stream::RemoteQualityMode::Automatic:
    case stream::RemoteQualityMode::Smooth:
    case stream::RemoteQualityMode::FollowGlobal:
        return 60; // wjy: 高质量、自动和流畅的可见预设均从60 FPS开始；只有自动模式后续允许降档。
    }
    return 60;
}

int priorityForRole(bool highPerformance)
{
    return highPerformance ? 100 : 10; // wjy: 真实获得焦点的可见窗口获得最高资源优先级，其余窗口统一使用最低优先级保活。
}

int backgroundFpsForVisibleWindowCount(
    int visibleWindowCount,
    const std::array<int, 5>& backgroundFpsTiers)
{
    if (visibleWindowCount <= 5) return backgroundFpsTiers[0];
    if (visibleWindowCount <= 10) return backgroundFpsTiers[1];
    if (visibleWindowCount <= 16) return backgroundFpsTiers[2];
    if (visibleWindowCount <= 20) return backgroundFpsTiers[3];
    return backgroundFpsTiers[4];
}

int reduceBackgroundFpsForPresenterPressure(
    int baseFps,
    double maxPresenterDropRatio,
    const std::array<int, 5>& backgroundFpsTiers)
{
    if (maxPresenterDropRatio >= 0.20) {
        return backgroundFpsTiers.back();
    }
    if (maxPresenterDropRatio < 0.03) {
        return baseFps;
    }

    for (std::size_t index = 0; index < backgroundFpsTiers.size(); ++index) {
        if (baseFps >= backgroundFpsTiers[index]) {
            const std::size_t nextIndex = std::min(index + 1, backgroundFpsTiers.size() - 1);
            return backgroundFpsTiers[nextIndex];
        }
    }
    return backgroundFpsTiers.back();
}
// ===end====

} // namespace

// =====wjy====
RemotePerformanceSignals RemotePerformanceSignalSampler::sample(const RemotePerformanceCounters& counters)
{
    RemotePerformanceSignals signals;
    const bool monotonic = m_initialized
        && counters.sampleTimeMs > m_previous.sampleTimeMs
        && counters.framesReceived >= m_previous.framesReceived
        && counters.framesDecoded >= m_previous.framesDecoded
        && counters.framesDropped >= m_previous.framesDropped
        && counters.freezeCount >= m_previous.freezeCount
        && counters.jitterBufferEmittedCount >= m_previous.jitterBufferEmittedCount
        && counters.packetsReceived >= m_previous.packetsReceived
        && counters.packetsLost >= m_previous.packetsLost
        && counters.totalDecodeTimeMs >= m_previous.totalDecodeTimeMs
        && counters.totalProcessingDelayMs >= m_previous.totalProcessingDelayMs
        && counters.totalFreezesDurationMs >= m_previous.totalFreezesDurationMs
        && counters.totalJitterBufferDelayMs >= m_previous.totalJitterBufferDelayMs;
    if (!monotonic) {
        m_previous = counters;
        m_initialized = true;
        return signals; // wjy: 首份样本或重连后的计数器回退只建立新基线，不把整段历史累计量当成一秒压力。
    }

    const uint64_t receivedFrames = counters.framesReceived - m_previous.framesReceived;
    const uint64_t decodedFrames = counters.framesDecoded - m_previous.framesDecoded;
    const uint64_t droppedFrames = counters.framesDropped - m_previous.framesDropped;
    const uint64_t emittedFrames = counters.jitterBufferEmittedCount - m_previous.jitterBufferEmittedCount;
    const uint64_t receivedPackets = counters.packetsReceived - m_previous.packetsReceived;
    const uint64_t lostPackets = counters.packetsLost - m_previous.packetsLost;
    signals.valid = true;
    if (decodedFrames > 0) {
        signals.averageDecodeMs = (counters.totalDecodeTimeMs - m_previous.totalDecodeTimeMs) / decodedFrames;
        signals.averageProcessingDelayMs =
            (counters.totalProcessingDelayMs - m_previous.totalProcessingDelayMs) / decodedFrames;
    }
    if (receivedFrames + droppedFrames > 0) {
        signals.decoderDropRatio = static_cast<double>(droppedFrames) / (receivedFrames + droppedFrames);
    }
    signals.freezeCountDelta = counters.freezeCount - m_previous.freezeCount;
    signals.freezeDurationDeltaMs = counters.totalFreezesDurationMs - m_previous.totalFreezesDurationMs;
    if (emittedFrames > 0) {
        signals.averageJitterBufferDelayMs =
            (counters.totalJitterBufferDelayMs - m_previous.totalJitterBufferDelayMs) / emittedFrames;
    }
    if (receivedPackets + lostPackets > 0) {
        signals.packetLossRatio = static_cast<double>(lostPackets) / (receivedPackets + lostPackets);
    }
    signals.roundTripTimeMs = std::max(0.0, counters.roundTripTimeMs);
    signals.availableIncomingBitrateKbps = std::max(0.0, counters.availableIncomingBitrateKbps);
    m_previous = counters;
    return signals;
}

void RemotePerformanceSignalSampler::reset()
{
    m_previous = {};
    m_initialized = false;
}

std::vector<RemoteQualityDecision> RemoteQualityCoordinator::evaluate(
    const stream::RemoteQualityConfiguration& rawConfiguration,
    const std::vector<RemoteQualityWindowMetrics>& windows,
    int64_t nowMs)
{
    const stream::RemoteQualityConfiguration configuration =
        stream::normalizedRemoteQualityConfiguration(rawConfiguration);
    std::vector<RemoteQualityDecision> decisions;
    decisions.reserve(windows.size());

    const int validWindowCount = static_cast<int>(std::count_if(
        windows.begin(),
        windows.end(),
        [](const RemoteQualityWindowMetrics& window) { return window.windowId != 0; })); // wjy: 按实际注册的远控窗口数量判断单窗口模式，隐藏或最小化仍由后续安全档覆盖。
    const bool singleRemoteWindow = validWindowCount == 1; // wjy: 只有一个远控窗口时不要求Qt焦点，避免用户切到主界面后唯一画面被无意义降质。

    const RemoteSelectionQualityProfile& selectionQuality =
        selectionQualityProfile(singleRemoteWindow); // wjy: 单选和多选只在这里切换质量变量，后续决策逻辑复用同一套生命周期与回退规则。
    uintptr_t focusedVisibleWindowId = 0;
    for (const RemoteQualityWindowMetrics& window : windows) {
        if (window.windowId == 0 || !window.visible || window.minimized || !window.active) {
            continue;
        }
        // wjy: Qt 主线程实际只会有一个活动顶层窗口；即使异常情况下多个快照同时标记 active，
        // 也稳定保留遍历到的第一个焦点窗口，避免多个会话同时获得高质量优先级。
        focusedVisibleWindowId = window.windowId;
        break;
    }

    int visibleWindowCount = 0;
    double maxPresenterDropRatio = 0.0;
    for (const RemoteQualityWindowMetrics& window : windows) {
        if (window.windowId == 0 || !window.visible || window.minimized) {
            continue;
        }
        ++visibleWindowCount;
        maxPresenterDropRatio = std::max(maxPresenterDropRatio, window.presenterDropRatio);
    }
    const int baseBackgroundFps = backgroundFpsForVisibleWindowCount(
        visibleWindowCount,
        selectionQuality.backgroundFpsTiers); // wjy: 多选后台FPS由可见窗口数量和顶部质量变量共同决定，避免所有后台窗口长期占用30 FPS。
    const int backgroundFps = reduceBackgroundFpsForPresenterPressure(
        baseBackgroundFps,
        maxPresenterDropRatio,
        selectionQuality.backgroundFpsTiers);

    for (const RemoteQualityWindowMetrics& window : windows) {
        RemoteQualityDecision decision;
        decision.windowId = window.windowId;
        const bool eligibleVisibleWindow = window.visible && !window.minimized;
        decision.minimized = !eligibleVisibleWindow;
        decision.active = window.active && eligibleVisibleWindow; // wjy: 隐藏或最小化窗口即使收到迟到激活状态也不能保留前台资源。
        decision.fullScreen = window.fullScreen && eligibleVisibleWindow;
        decision.softwareFallback = window.softwareFallback;
        const bool focusedWindowEligible = eligibleVisibleWindow
            && window.windowId == focusedVisibleWindowId;
        const bool highPerformance = eligibleVisibleWindow
            && (singleRemoteWindow || focusedWindowEligible); // wjy: 单窗口只要可见且未最小化就使用单选质量变量，多窗口只给真实焦点窗口使用多选前台变量。
        decision.audioEnabled = false; // wjy: 音频不再由协调器控制，RemoteDesktopWindow按自己的标题栏按钮独立下发。
        decision.effectiveMode = highPerformance
            ? stream::RemoteQualityMode::HighQualityLocked
            : stream::RemoteQualityMode::Smooth; // wjy: 单窗口或多窗口真实焦点使用高质量，其余窗口进入统一后台安全档。
        decision.priority = priorityForRole(highPerformance);

        WindowState& state = m_states[window.windowId];
        const int baselineFps = fpsIndexForTarget(baselineTargetFps(decision.effectiveMode));
        const int automaticFpsFloor = fpsIndexForTarget(30); // wjy: 自动模式唯一可见下限固定30 FPS，不再读取旧严重压力或总预算参数。
        const bool automaticMode = decision.effectiveMode == stream::RemoteQualityMode::Automatic;
        if (!state.initialized) {
            state = {};
            state.fpsIndex = baselineFps;
            state.effectiveMode = decision.effectiveMode;
            state.initialized = true; // wjy: 新窗口从模式基线开始，不继承之前同设备窗口的降级历史。
        } else if (state.effectiveMode != decision.effectiveMode) {
            state.fpsIndex = baselineFps; // wjy: 焦点导致前后台模式切换时重置画质档位，但不能清空远端角色防抖状态。
            state.pressureSinceMs = 0;
            state.recoverySinceMs = 0;
            state.degradationReason = RemoteQualityDegradationReason::None;
            state.effectiveMode = decision.effectiveMode; // wjy: 保留pending/remoteHighPerformance与roleChangedAtMs，350ms稳定窗口不会被模式重置绕过。
        }
        if (!state.roleInitialized) {
            state.pendingHighPerformance = highPerformance;
            state.remoteHighPerformance = highPerformance;
            state.roleChangedAtMs = nowMs;
            state.roleInitialized = true;
            decision.requestRemoteProfile = true;
        } else {
            if (state.pendingHighPerformance != highPerformance) {
                state.pendingHighPerformance = highPerformance;
                state.roleChangedAtMs = nowMs; // wjy: 每次焦点抖动重新计时，远端不会连续触发关键帧和码率突发。
            }
            if (state.remoteHighPerformance != state.pendingHighPerformance
                && nowMs - state.roleChangedAtMs
                    >= static_cast<int64_t>(stream::kRemoteProfileFocusDebounceMs)) {
                state.remoteHighPerformance = state.pendingHighPerformance;
            }
            decision.requestRemoteProfile = state.remoteHighPerformance == highPerformance;
        }
        state.fpsIndex = std::clamp(state.fpsIndex, baselineFps, automaticFpsFloor);

        if (!highPerformance) {
            state.fpsIndex = baselineFps;
            state.pressureSinceMs = 0;
            state.recoverySinceMs = 0;
            state.degradationReason = RemoteQualityDegradationReason::None;
            decision.resolution = decision.minimized
                ? configuration.minimizedResolution
                : selectionQuality.background.resolution;
            decision.targetFps = decision.minimized
                ? configuration.minimizedFps
                : backgroundFps; // wjy: 普通可见窗口按全局数量和Presenter压力动态降帧，最小化继续使用配置的后台档。
            decision.reason = decision.minimized
                ? RemoteQualityDegradationReason::Minimized
                : RemoteQualityDegradationReason::Background; // wjy: 非焦点可见窗口统一进入后台策略，不再读取窗口面积阈值。
        } else if (window.softwareFallback) {
            state.fpsIndex = baselineFps;
            state.pressureSinceMs = 0;
            state.recoverySinceMs = 0;
            state.degradationReason = RemoteQualityDegradationReason::None;
            decision.reason = RemoteQualityDegradationReason::SoftwareFallback;
            decision.resolution = stream::RemoteResolutionTier::P540; // wjy: 最大窗口的软件回退允许540p，普通后台窗口已在上一分支固定P720并动态降帧。
            decision.targetFps = 24; // wjy: 软件Presenter回退使用540p/24安全档，退出回退后立即恢复当前单选/多选前台质量变量。
        } else if (!automaticMode) {
            state.fpsIndex = baselineFps;
            state.pressureSinceMs = 0;
            state.recoverySinceMs = 0;
            state.degradationReason = RemoteQualityDegradationReason::None;
            decision.resolution = selectionQuality.foreground.resolution;
            decision.targetFps = selectionQuality.foreground.targetFps; // wjy: 单选/多选焦点窗口直接使用顶部质量变量，不再把原始/60写死在分支里。
            if (decision.effectiveMode == stream::RemoteQualityMode::Balanced
                || decision.effectiveMode == stream::RemoteQualityMode::Smooth) {
                decision.reason = RemoteQualityDegradationReason::ModePreference; // wjy: 1080p或720p是用户主动预设，不显示成性能降级。
            }
        } else {
            const int currentFps = stream::kRemoteFpsTiers[static_cast<std::size_t>(state.fpsIndex)];
            const double frameBudgetMs = 1000.0 / std::max(1, currentFps);
            const bool lowObservedFps = window.receiveFps > 1.0
                && window.receiveFps < static_cast<double>(currentFps) * 0.80; // wjy: 低FPS只是结果信号，后面必须与真实管线压力相关才允许降档。
            const bool decoderPressure = window.performance.valid
                && (window.performance.averageDecodeMs >= frameBudgetMs * 0.70
                    || window.performance.averageProcessingDelayMs >= frameBudgetMs * 0.90
                    || window.performance.decoderDropRatio >= 0.05);
            const bool networkPressure = window.performance.valid
                && ((window.performance.packetLossRatio >= 0.03 && window.performance.roundTripTimeMs >= 80.0)
                    || window.performance.averageJitterBufferDelayMs >= 80.0);
            const bool freezePressure = window.performance.valid
                && (window.performance.freezeCountDelta > 0
                    || window.performance.freezeDurationDeltaMs >= 100.0);
            const bool presenterPressure = window.presenterDropRatio >= 0.08;
            const bool correlatedPressure = lowObservedFps
                && (decoderPressure || networkPressure || freezePressure || presenterPressure); // wjy: 静止桌面即使只产生少量帧，只要所有直接压力健康就保持当前目标。
            const bool severePressure = window.performance.valid
                && (window.performance.freezeCountDelta >= 2
                    || window.performance.freezeDurationDeltaMs >= 250.0
                    || window.performance.averageDecodeMs >= frameBudgetMs
                    || window.performance.decoderDropRatio >= 0.20
                    || window.performance.packetLossRatio >= 0.08
                    || window.performance.averageJitterBufferDelayMs >= 180.0)
                || window.presenterDropRatio >= 0.20;
            const bool pressure = severePressure || correlatedPressure; // wjy: 自动模式只响应当前窗口的真实管线证据，总预算不再跨窗口静默改写请求。
            if (pressure) {
                state.recoverySinceMs = 0;
                if (state.pressureSinceMs == 0) state.pressureSinceMs = nowMs;
                const int pressureHoldMs = severePressure
                    ? std::min(configuration.degradationHoldMs, 1500)
                    : configuration.degradationHoldMs; // wjy: 自动模式普通压力慢降，明确冻结或重丢帧可更快进入下一固定档。
                if (nowMs - state.pressureSinceMs >= pressureHoldMs) {
                    if (state.fpsIndex < automaticFpsFloor) {
                        ++state.fpsIndex;
                    }
                    state.pressureSinceMs = nowMs; // wjy: 每个持续窗口只移动一档，使自动模式严格按60→45→30变化。
                }
                state.degradationReason = severePressure
                    ? RemoteQualityDegradationReason::SeverePipelinePressure
                    : RemoteQualityDegradationReason::PipelinePressure;
                decision.reason = state.degradationReason;
            } else {
                state.pressureSinceMs = 0;
                if (configuration.automaticRecoveryEnabled && state.fpsIndex > baselineFps) {
                    if (state.recoverySinceMs == 0) state.recoverySinceMs = nowMs;
                    if (nowMs - state.recoverySinceMs >= configuration.recoveryHoldMs) {
                        --state.fpsIndex;
                        state.recoverySinceMs = nowMs;
                    }
                } else {
                    state.recoverySinceMs = 0;
                }
                if (state.fpsIndex > baselineFps) {
                    decision.reason = state.degradationReason; // wjy: 压力消失后的滞回期保留最近原因，直到FPS恢复到模式基线。
                } else {
                    state.degradationReason = RemoteQualityDegradationReason::None;
                }
            }
            decision.resolution = stream::RemoteResolutionTier::Native; // wjy: 自动模式始终保持原始分辨率，只在60/45/30三个FPS档之间适配。
            decision.targetFps = stream::kRemoteFpsTiers[static_cast<std::size_t>(state.fpsIndex)];
        }

        targetSize(decision.resolution, window.sourceWidth, window.sourceHeight, &decision.targetWidth, &decision.targetHeight);
        const RemoteQualityPreset& selectedPreset = highPerformance
            ? selectionQuality.foreground
            : selectionQuality.background;
        const bool usePresetBitrate = !window.softwareFallback
            && !decision.minimized
            && selectedPreset.maxBitrateKbps > 0; // wjy: 最小化和软件回退沿用分辨率安全码率，避免后台低帧仍保留前台码率上限。
        decision.maxBitrateKbps = usePresetBitrate
            ? selectedPreset.maxBitrateKbps
            : bitrateForDecision(decision.resolution, decision.targetFps, decision.effectiveMode); // wjy: 质量变量可直接覆盖码率；设为0时保留旧的按分辨率兜底计算。
        decisions.push_back(decision);
    }
    // =====wjy====
    // 正常后台档位固定为720p/30；只有跨组件的双信号压力成立后，才进入15/10/5/3/1 FPS梯度。
    stream::RemoteVideoPressureSignals pipelinePressure;
    for (const auto& window : windows) {
        pipelinePressure.workerBacklog = pipelinePressure.workerBacklog || window.workerBacklog;
        pipelinePressure.presentDrops = pipelinePressure.presentDrops || window.presenterDropRatio >= 0.08;
        pipelinePressure.frameAgeHigh = pipelinePressure.frameAgeHigh || window.frameAgeP95Ms >= 120.0;
        pipelinePressure.syncBusy = pipelinePressure.syncBusy || window.syncBusy;
        pipelinePressure.renderUtilizationHigh = pipelinePressure.renderUtilizationHigh || window.renderUtilizationHigh;
        pipelinePressure.decodeQueueAgeHigh = pipelinePressure.decodeQueueAgeHigh || window.decodeQueueAgeHigh;
        pipelinePressure.deviceFailure = pipelinePressure.deviceFailure || window.deviceFailure;
    }
    const auto emergency = m_pipelinePressureController.update(pipelinePressure, nowMs); // wjy: 双信号持续1秒才进入降级，健康3秒后逐级恢复。
    for (auto& decision : decisions) {
        if (decision.reason == RemoteQualityDegradationReason::Background) {
            decision.targetFps = 30;
            if (emergency != stream::RemoteVideoEmergencyTier::None) {
                const auto profile = stream::RemoteVideoPolicy::profileFor(
                    stream::RemoteVideoWindowRole::VisibleBackground,
                    {},
                    emergency);
                decision.targetFps = static_cast<int>(profile.targetFps);
                decision.reason = RemoteQualityDegradationReason::PipelinePressure;
            }
        } else if (decision.reason == RemoteQualityDegradationReason::None
            || decision.reason == RemoteQualityDegradationReason::ModePreference) {
            decision.targetFps = 60;
        }
    }
    // ===end====
    return decisions;
}

void RemoteQualityCoordinator::removeWindow(uintptr_t windowId)
{
    m_states.erase(windowId);
}

int RemoteQualityCoordinator::fpsIndexForTarget(int fps)
{
    for (int index = 0; index < static_cast<int>(stream::kRemoteFpsTiers.size()); ++index) {
        if (stream::kRemoteFpsTiers[static_cast<std::size_t>(index)] <= fps) return index;
    }
    return static_cast<int>(stream::kRemoteFpsTiers.size()) - 1;
}

int RemoteQualityCoordinator::bitrateForDecision(
    stream::RemoteResolutionTier resolution,
    int fps,
    stream::RemoteQualityMode mode)
{
    int baseKbps = 80000;
    switch (resolution) {
    case stream::RemoteResolutionTier::Native: baseKbps = 80000; break;
    case stream::RemoteResolutionTier::P1440: baseKbps = 80000; break;
    case stream::RemoteResolutionTier::P1080: baseKbps = 48000; break;
    case stream::RemoteResolutionTier::P900: baseKbps = 36000; break;
    case stream::RemoteResolutionTier::P720: baseKbps = 24000; break;
    case stream::RemoteResolutionTier::P540: baseKbps = 14000; break;
    case stream::RemoteResolutionTier::P360: baseKbps = 7000; break;
    }
    double modeScale = 1.0; // wjy: 高质量使用80Mbps原始档上限，避免原始分辨率占用过高发送预算。
    if (mode == stream::RemoteQualityMode::Smooth) modeScale = 0.85;
    (void)fps; // wjy: FPS下降用于增加每帧可用码率并保持清晰度；WebRTC拥塞控制仍可按真实带宽使用低于上限的码率。
    return std::clamp(
        static_cast<int>(std::lround(baseKbps * modeScale)),
        512,
        240000); // wjy: 固定分辨率档位使用稳定码率上限，降帧不再同时把单帧质量预算砍掉。
}

void RemoteQualityCoordinator::targetSize(
    stream::RemoteResolutionTier resolution,
    int sourceWidth,
    int sourceHeight,
    int* width,
    int* height)
{
    const int targetHeight = stream::resolutionTierHeight(resolution);
    if (targetHeight == 0) {
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }
    const int safeSourceWidth = std::max(1, sourceWidth);
    const int safeSourceHeight = std::max(1, sourceHeight);
    int targetWidth = static_cast<int>(std::lround(
        static_cast<double>(targetHeight) * safeSourceWidth / safeSourceHeight));
    targetWidth = std::clamp(targetWidth, 320, 7680);
    if ((targetWidth & 1) != 0) --targetWidth; // wjy: 编码目标保持偶数宽度，兼容H264/H265和常见GPU缩放约束。
    if (width) *width = targetWidth;
    if (height) *height = targetHeight;
}
// ===end====

} // namespace ui
