#include "ui/RemoteQualityCoordinator.h"

#include <algorithm>
#include <cmath>

namespace ui {

bool shouldDispatchRemoteQualityDecision(
    const RemoteQualityDecision& decision,
    bool viewerAvailable,
    bool closing)
{
    return decision.requestRemoteProfile && viewerAvailable && !closing; // wjy: 精确档位变化立即下发；Viewer未创建或窗口关闭时继续由统一门禁拦截。
}

namespace {

stream::RemoteQualityMode qualityModeForPreset(stream::RemoteVideoQualityPreset preset)
{
    switch (preset) {
    case stream::RemoteVideoQualityPreset::P1080_60:
    case stream::RemoteVideoQualityPreset::P1080_30:
        return stream::RemoteQualityMode::HighQualityLocked;
    case stream::RemoteVideoQualityPreset::P720_60:
    case stream::RemoteVideoQualityPreset::P720_30:
        return stream::RemoteQualityMode::Balanced;
    case stream::RemoteVideoQualityPreset::P540_30:
    case stream::RemoteVideoQualityPreset::P540_25:
    case stream::RemoteVideoQualityPreset::P360_25:
    case stream::RemoteVideoQualityPreset::P360_1:
        return stream::RemoteQualityMode::Smooth; // wjy: 协议mode只作兼容提示，真实尺寸、FPS和码率全部由精确预设字段决定。
    }
    return stream::RemoteQualityMode::Smooth;
}

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
    (void)rawConfiguration; // wjy: 保留调用签名兼容现有设置页；八档策略不再读取旧自动模式和压力阈值。
    (void)nowMs; // wjy: 遮挡解除、全屏切换和用户手选要求立即生效，不再使用焦点防抖或性能滞回。
    std::vector<RemoteQualityDecision> decisions;
    decisions.reserve(windows.size());

    for (const RemoteQualityWindowMetrics& window : windows) {
        RemoteQualityDecision decision;
        decision.windowId = window.windowId;
        const bool eligibleVisibleWindow = window.visible && !window.minimized && !window.fullyOccluded;
        decision.minimized = !eligibleVisibleWindow;
        decision.fullyOccluded = window.visible && !window.minimized && window.fullyOccluded;
        decision.active = window.active && eligibleVisibleWindow;
        decision.fullScreen = window.fullScreen && eligibleVisibleWindow;
        decision.softwareFallback = window.softwareFallback; // wjy: 软件回退继续进入诊断，但不再成为覆盖用户手选档的第二个例外。
        decision.userSelectedPreset = window.userQualityPresetActive;

        const stream::RemoteVideoQualityPreset preferredPreset = window.userQualityPresetActive
            ? window.userQualityPreset
            : window.monitorQualityPresetActive
                ? window.monitorQualityPreset
                : decision.fullScreen
                    ? stream::kFullscreenRemoteVideoQualityPreset
                    : stream::kDefaultRemoteVideoQualityPreset; // wjy: 手选最高，其次监控模式设置；普通自动规则仍为全屏720/30、非全屏540/30。
        decision.preset = eligibleVisibleWindow
            ? preferredPreset
            : stream::kOccludedRemoteVideoQualityPreset; // wjy: 完全遮挡、最小化或隐藏统一临时降到360/1，重新可见后下一次评估立即恢复原意图。

        const stream::RemoteVideoEncodingPreset encoding = stream::remoteVideoEncodingPreset(decision.preset);
        decision.effectiveMode = qualityModeForPreset(decision.preset);
        decision.resolution = encoding.resolution;
        decision.targetFps = static_cast<int>(encoding.targetFps);
        decision.maxBitrateKbps = static_cast<int>(encoding.maxBitrateKbps);
        decision.priority = eligibleVisibleWindow ? 100 : 5; // wjy: 所有可见窗口同优先级，Host不能再按焦点把后台窗口隐式降档。
        decision.reason = decision.fullyOccluded
            ? RemoteQualityDegradationReason::FullyOccluded
            : decision.minimized
                ? RemoteQualityDegradationReason::Minimized
                : (window.userQualityPresetActive || window.monitorQualityPresetActive)
                    ? RemoteQualityDegradationReason::ModePreference
                    : RemoteQualityDegradationReason::None;
        decision.requestRemoteProfile = true; // wjy: RemoteDesktopWindow使用完整payload去重，相同档位不会每秒重复发包。
        decision.audioEnabled = false;
        targetSize(decision.resolution, window.sourceWidth, window.sourceHeight, &decision.targetWidth, &decision.targetHeight);
        decisions.push_back(decision);
    }
    return decisions;
}

void RemoteQualityCoordinator::removeWindow(uintptr_t windowId)
{
    (void)windowId; // wjy: 当前协调器无跨评估窗口状态，保留接口让既有销毁路径无需分叉。
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
