#pragma once

#include "stream/RemoteQualityPolicy.h"
#include "stream/RemoteVideoPolicy.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ui {

// =====wjy====
enum class RemoteQualityDegradationReason {
    None,
    ModePreference,
    Background,
    Minimized,
    LargestWindowBelowThreshold,
    PipelinePressure,
    SeverePipelinePressure,
    SoftwareFallback,
};

struct RemotePerformanceCounters {
    uint64_t sampleTimeMs = 0;
    uint64_t framesReceived = 0;
    uint64_t framesDecoded = 0;
    uint64_t framesDropped = 0;
    uint64_t freezeCount = 0;
    uint64_t jitterBufferEmittedCount = 0;
    uint64_t packetsReceived = 0;
    uint64_t packetsLost = 0;
    double totalDecodeTimeMs = 0.0;
    double totalProcessingDelayMs = 0.0;
    double totalFreezesDurationMs = 0.0;
    double totalJitterBufferDelayMs = 0.0;
    double roundTripTimeMs = 0.0;
    double availableIncomingBitrateKbps = 0.0;
};

struct RemotePerformanceSignals {
    bool valid = false;
    double averageDecodeMs = 0.0;
    double averageProcessingDelayMs = 0.0;
    double decoderDropRatio = 0.0;
    uint64_t freezeCountDelta = 0;
    double freezeDurationDeltaMs = 0.0;
    double averageJitterBufferDelayMs = 0.0;
    double packetLossRatio = 0.0;
    double roundTripTimeMs = 0.0;
    double availableIncomingBitrateKbps = 0.0;
};

class RemotePerformanceSignalSampler final {
public:
    RemotePerformanceSignals sample(const RemotePerformanceCounters& counters); // wjy: 对相邻累计快照求差，计数器重置或首份样本均不制造虚假压力。
    void reset();
private:
    RemotePerformanceCounters m_previous;
    bool m_initialized = false;
};

struct RemoteQualityWindowMetrics {
    uintptr_t windowId = 0;
    stream::RemoteQualityMode effectiveMode = stream::RemoteQualityMode::Automatic;
    bool visible = true;
    bool minimized = false;
    bool active = false;
    bool fullScreen = false;
    bool softwareFallback = false;
    int sourceWidth = 1920;
    int sourceHeight = 1080;
    int viewportWidth = 1920;
    int viewportHeight = 1080;
    int viewportArea = 1920 * 1080;
    double receiveFps = 0.0;
    double encodedMbps = 0.0;
    RemotePerformanceSignals performance;
    double presenterDropRatio = 0.0;
    // =====wjy====
    bool workerBacklog = false;
    bool syncBusy = false;
    bool renderUtilizationHigh = false;
    bool decodeQueueAgeHigh = false;
    bool deviceFailure = false;
    double frameAgeP95Ms = 0.0;
    // ===end====
    bool qualityV2Supported = false;
    bool onlineFpsUpdate = false;
    int sourceRefreshHz = 60;
    int localRefreshHz = 60;
    int maxCaptureFps = 60;
    int maxEncodeFps = 60;
    int appliedFps = 60;
};

struct RemoteQualityDecision {
    uintptr_t windowId = 0;
    stream::RemoteQualityMode effectiveMode = stream::RemoteQualityMode::Automatic;
    stream::RemoteResolutionTier resolution = stream::RemoteResolutionTier::Native;
    int targetWidth = 0;
    int targetHeight = 0;
    int targetFps = 60;
    int maxBitrateKbps = 80000;
    int priority = 60;
    RemoteQualityDegradationReason reason = RemoteQualityDegradationReason::None;
    bool active = false;
    bool fullScreen = false;
    bool minimized = false;
    bool softwareFallback = false;
    bool requestRemoteProfile = true; // wjy: 本地调度立即应用；快速焦点切换期间可延迟远端编码器改参。
    bool audioEnabled = false; // wjy: 音频由远控窗口自身按钮独立控制，不再由画质协调器抢占。
};

class RemoteQualityCoordinator final {
public:
    std::vector<RemoteQualityDecision> evaluate(
        const stream::RemoteQualityConfiguration& configuration,
        const std::vector<RemoteQualityWindowMetrics>& windows,
        int64_t nowMs); // wjy: 每秒评估全部窗口，输出在线质量请求但永远不返回“断开/暂停”动作。
    void removeWindow(uintptr_t windowId); // wjy: 窗口关闭时删除滞回状态，重开设备从全局默认档重新开始。

private:
    struct WindowState {
        int fpsIndex = 0;
        int64_t pressureSinceMs = 0;
        int64_t recoverySinceMs = 0;
        RemoteQualityDegradationReason degradationReason = RemoteQualityDegradationReason::None; // wjy: 记录最近一次真实降级来源，压力消失后的滞回恢复期仍能显示准确原因。
        stream::RemoteQualityMode effectiveMode = stream::RemoteQualityMode::Automatic; // wjy: 模式切换时立即丢弃旧自动档位，固定模式不会继承先前的低FPS状态。
        bool initialized = false;
        bool pendingHighPerformance = false;
        bool remoteHighPerformance = false;
        bool roleInitialized = false;
        int64_t roleChangedAtMs = 0;
    };

    static int fpsIndexForTarget(int fps);
    static int bitrateForDecision(stream::RemoteResolutionTier resolution, int fps, stream::RemoteQualityMode mode);
    static void targetSize(stream::RemoteResolutionTier resolution, int sourceWidth, int sourceHeight, int* width, int* height);
    std::unordered_map<uintptr_t, WindowState> m_states;
    stream::RemoteVideoPressureController m_pipelinePressureController; // wjy: 全部窗口共享同一适配器压力滞回，避免每个窗口各自抖动降级。
};
// ===end====

} // namespace ui
