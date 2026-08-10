#pragma once

#include "stream/RemoteQualityPolicy.h"
#include "stream/RemoteVideoPolicy.h"

#include <cstdint>
#include <vector>

namespace ui {

// =====wjy====
enum class RemoteQualityDegradationReason {
    None,
    ModePreference,
    Background,
    Minimized,
    FullyOccluded, // wjy: 窗口仍处于普通显示状态但屏幕区域被更高层窗口完全覆盖，画质与最小化共用安全档。
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
    stream::RemoteVideoQualityPreset userQualityPreset = stream::kDefaultRemoteVideoQualityPreset; // wjy: 仅在userQualityPresetActive时生效，保存用户手选而不是临时全屏档。
    bool userQualityPresetActive = false; // wjy: false表示使用普通540/30或全屏720/30自动规则；历史高档被恢复仲裁拒绝时也保持false。
    bool visible = true;
    bool minimized = false;
    bool fullyOccluded = false; // wjy: Windows 可见区域计算结果；只要仍露出任意像素就保持 false。
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
    stream::RemoteVideoQualityPreset preset = stream::kDefaultRemoteVideoQualityPreset; // wjy: 决策保留精确档位，标题栏诊断和Host参数不再从旧模式名称反推。
    bool userSelectedPreset = false; // wjy: 标记可见窗口是否正在使用用户手选档，完全遮挡时档位临时降级但该意图仍保留在窗口中。
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
    bool fullyOccluded = false; // wjy: 保留遮挡来源供标题栏诊断显示，资源角色仍通过 minimized 复用最小化策略。
    bool softwareFallback = false;
    bool requestRemoteProfile = true; // wjy: 精确档位变化允许立即下发，相同payload由窗口层去重。
    bool audioEnabled = false; // wjy: 音频由远控窗口自身按钮独立控制，不再由画质协调器抢占。
};

bool shouldDispatchRemoteQualityDecision(
    const RemoteQualityDecision& decision,
    bool viewerAvailable,
    bool closing); // wjy: 新Viewer补发和在线评估统一经过可用性、关闭状态和请求门禁。

class RemoteQualityCoordinator final {
public:
    std::vector<RemoteQualityDecision> evaluate(
        const stream::RemoteQualityConfiguration& configuration,
        const std::vector<RemoteQualityWindowMetrics>& windows,
        int64_t nowMs); // wjy: 每次按手选、全屏、默认和遮挡优先级直接生成精确档位，不再因焦点或性能采样自动改档。
    void removeWindow(uintptr_t windowId); // wjy: 当前策略无窗口滞回状态，保留接口兼容DeviceGrid销毁清理路径。

private:
    static void targetSize(stream::RemoteResolutionTier resolution, int sourceWidth, int sourceHeight, int* width, int* height);
};
// ===end====

} // namespace ui
