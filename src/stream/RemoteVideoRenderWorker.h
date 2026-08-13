#pragma once

#include "stream/RemoteVideoDiagnostics.h"
#include "stream/RemoteVideoFrame.h"
#include "stream/RemoteVideoScheduler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace stream {

// =====wjy====
enum class RemoteVideoRenderResult : std::uint8_t {
    Presented,
    DroppedStale,
    DroppedSyncBusy,
    RetrySync,
    DeviceLost,
    Failed,
};

struct RemoteVideoSurfaceState {
    std::uint64_t windowId = 0;
    std::uint64_t sessionId = 0;
    std::uint64_t generation = 0;
    std::uint32_t adapterIndex = 0;
    void* nativeWindow = nullptr;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool visible = false;
    bool focused = false;
    bool minimized = false;
    RemoteVideoProfile profile;
};

struct RemoteVideoSurfaceMetrics {
    std::uint64_t textureCacheHits = 0;
    std::uint64_t textureCacheMisses = 0;
    std::uint64_t viewCacheHits = 0;
    std::uint64_t viewCacheMisses = 0;
    std::uint64_t deviceGeneration = 0;
}; // wjy: Surface只暴露低频累计计数，RenderWorker汇总时读取，不在UI线程逐帧查询D3D状态。

class RemoteVideoRenderSurface {
public:
    virtual ~RemoteVideoRenderSurface() = default;

    virtual void applyState(const RemoteVideoSurfaceState& state) = 0;
    virtual RemoteVideoRenderResult render(NativeVideoFrame& frame) = 0; // wjy: Surface只处理纹理所有权，不搬走帧对象；RetrySync时RenderWorker必须保留原描述符。
    virtual RemoteVideoRenderResult discard(NativeVideoFrame& frame) = 0; // wjy: 显式丢弃同样使用借用引用，成功归还key后再由worker释放lease。
    virtual void clear() = 0;
    virtual RemoteVideoSurfaceMetrics diagnostics() const noexcept { return {}; } // wjy: Fake Surface无需实现即可参与测试，D3D11表面覆盖真实缓存计数。
};

struct RemoteVideoRenderNotification {
    std::uint64_t windowId = 0;
    std::uint64_t sessionId = 0;
    std::uint64_t generation = 0;
    std::uint64_t frameId = 0;
    int width = 0;
    int height = 0;
    double encodedMbps = 0.0;
    std::uint32_t presentedSinceLast = 0;
    std::uint32_t droppedSinceLast = 0;
    RemoteVideoRenderResult result = RemoteVideoRenderResult::Failed;
};

class RemoteVideoRenderWorker final {
public:
    using ResultCallback = std::function<void(const RemoteVideoRenderNotification&)>;

    explicit RemoteVideoRenderWorker(
        std::uint32_t adapterIndex = 0,
        std::shared_ptr<RemoteVideoDiagnostics> diagnostics = {},
        ResultCallback resultCallback = {});
    ~RemoteVideoRenderWorker();

    RemoteVideoRenderWorker(const RemoteVideoRenderWorker&) = delete;
    RemoteVideoRenderWorker& operator=(const RemoteVideoRenderWorker&) = delete;

    bool start();
    void stop() noexcept;

    bool registerSurface(
        std::uint64_t windowId,
        const RemoteVideoSurfaceState& state,
        std::shared_ptr<RemoteVideoRenderSurface> surface);
    bool updateSurface(const RemoteVideoSurfaceState& state);
    bool unregisterSurface(std::uint64_t windowId);
    bool submitFrame(std::uint64_t windowId, NativeVideoFrame frame);
    bool invalidateGeneration(std::uint64_t windowId, std::uint64_t generation);

    bool waitForIdle(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    std::size_t registeredSurfaceCount() const noexcept;

private:
    struct RetiringFrame {
        NativeVideoFrame frame;
        RemoteVideoFrameReleaseReason reason = RemoteVideoFrameReleaseReason::ExplicitDrop;
        std::uint32_t attempts = 0;
        std::int64_t nextRetryMs = 0;
    };

    struct Session {
        RemoteVideoSurfaceState state;
        std::shared_ptr<RemoteVideoRenderSurface> surface;
        FrameInbox inbox;
        std::optional<NativeVideoFrame> inFlight; // wjy: 同步key尚未就绪时保留唯一正在处理帧，新解码帧仍只占一个latest pending槽。
        std::deque<RetiringFrame> retiringFrames; // wjy: 被替换/关闭的纹理若暂时取不到key 1，在RenderWorker内保留后重试，绝不提前释放lease。
        std::int64_t lastNotificationMs = 0;
        RemoteVideoRenderResult lastResult = RemoteVideoRenderResult::Failed;
        std::uint32_t presentedSinceNotification = 0;
        std::uint32_t droppedSinceNotification = 0;
        std::uint64_t receivedSinceSummary = 0;
        std::uint64_t presentedSinceSummary = 0;
        std::uint64_t droppedSinceSummary = 0;
        std::uint64_t staleSinceSummary = 0;
        std::uint64_t synchronizationBusySinceSummary = 0;
        std::uint64_t renderTimeUsSinceSummary = 0;
        std::uint64_t renderSamplesSinceSummary = 0;
        std::deque<std::uint64_t> frameAgeSamplesUs; // wjy: 每会话只保留一个汇总周期内最多256个帧龄样本，用于p95而不写逐帧日志。
        RemoteVideoSurfaceMetrics lastSurfaceMetrics;
        std::uint32_t anomalyPressureIntervals = 0;
    };

    enum class CommandType : std::uint8_t {
        Register,
        Update,
        Unregister,
        Submit,
        Invalidate,
        Barrier,
        Stop,
    };

    struct Command {
        CommandType type = CommandType::Stop;
        std::uint64_t windowId = 0;
        RemoteVideoSurfaceState state;
        std::shared_ptr<RemoteVideoRenderSurface> surface;
        NativeVideoFrame frame;
        std::uint64_t generation = 0;
        std::shared_ptr<std::promise<void>> barrier;
    };

    bool enqueue(Command command);
    void workerLoop() noexcept;
    void processCommand(Command&& command);
    void processRetiringFrames(std::int64_t nowMs);
    void processReadyFrames(std::int64_t nowMs);
    void emitPeriodicSummaries(std::int64_t nowMs);
    void retireFrame(Session& session, NativeVideoFrame frame, RemoteVideoFrameReleaseReason reason);
    void releaseSession(Session& session, RemoteVideoFrameReleaseReason reason);
    void logEvent(
        RemoteVideoLogLevel level,
        const char* event,
        const char* result,
        const char* reason,
        std::uint64_t windowId = 0,
        std::uint64_t sessionId = 0,
        std::uint64_t generation = 0,
        std::uint64_t frameId = 0,
        std::uint64_t renderJobId = 0);
    void logStage(
        RemoteVideoStage stage,
        RemoteVideoStageResult result,
        RemoteVideoStageReason reason,
        const RemoteVideoLogContext& context,
        std::uint64_t durationUs = 0,
        std::uint64_t queueDepth = 0,
        std::uint64_t frameAgeUs = 0,
        RemoteVideoLogLevel level = RemoteVideoLogLevel::Trace);

    const std::uint32_t adapterIndex_;
    const std::shared_ptr<RemoteVideoDiagnostics> diagnostics_;
    const ResultCallback resultCallback_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable idleCondition_;
    std::deque<Command> commands_;
    std::unordered_map<std::uint64_t, Session> sessions_;
    RemoteVideoScheduler scheduler_;
    std::thread worker_;
    std::atomic<std::size_t> registeredSurfaceCount_ = 0;
    std::uint64_t nextRenderJobId_ = 1;
    std::int64_t lastSummaryMs_ = 0;
    std::uint64_t workerBusyUsSinceSummary_ = 0;
    std::size_t activeCommands_ = 0;
    bool running_ = false;
    bool stopRequested_ = false;
    bool workerExited_ = true;
    static constexpr std::size_t kCommandQueueLimit = 2048;
};
// ===end====

} // namespace stream
