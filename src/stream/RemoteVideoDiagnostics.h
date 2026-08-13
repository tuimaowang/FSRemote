#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace stream {

// =====wjy====
enum class RemoteVideoLogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
}; // wjy: 日志级别区分帧级追踪、周期统计和必须关注的错误事件。

struct RemoteVideoLogContext {
    std::uint64_t runId = 0; // wjy: 关联一次程序运行，避免多次启动的帧号相互混淆。
    std::uint64_t sessionId = 0; // wjy: 关联具体远控会话和窗口来源。
    std::uint64_t windowId = 0; // wjy: 关联本地远控窗口。
    std::uint64_t viewerGeneration = 0; // wjy: 重连代际隔离旧回调。
    std::uint64_t frameId = 0; // wjy: 关联单帧跨线程时间线。
    std::uint64_t renderJobId = 0; // wjy: 关联一次RenderWorker工作项。
    std::uint32_t adapterIndex = 0; // wjy: 区分多显卡适配器上的RenderWorker。
    std::uint32_t threadId = 0; // wjy: 记录实际产生事件的线程身份。
};

struct RemoteVideoLogField {
    std::string key; // wjy: 结构化字段名，便于外部脚本筛选。
    std::string value; // wjy: 字段值统一序列化为字符串，避免日志格式依赖业务类型。
};

struct RemoteVideoLogEvent {
    std::uint64_t monotonicUs = 0; // wjy: 单调时钟时间用于计算跨线程阶段耗时。
    RemoteVideoLogLevel level = RemoteVideoLogLevel::Info; // wjy: 控制落盘和现场关注等级。
    RemoteVideoLogContext context; // wjy: 统一关联字段贯穿解码、队列、渲染和窗口状态。
    std::string event; // wjy: 稳定事件名称，例如 decode_done、present 或 device_lost。
    std::string result; // wjy: 事件结果，例如 success、drop、fallback 或 failure。
    std::string reason; // wjy: 稳定原因枚举，避免依赖自然语言猜测丢帧原因。
    std::vector<RemoteVideoLogField> fields; // wjy: 保存阶段耗时、尺寸、FPS和资源统计等扩展信息。
    bool durable = true; // wjy: Trace/Debug帧事件只进入内存环形现场；生命周期、错误和周期汇总才进入持久化队列。
};

enum class RemoteVideoStage : std::uint8_t {
    Decode,
    Queue,
    Synchronization,
    Processing,
    Present,
    Release,
    Profile,
    Fallback,
    Recovery,
};

enum class RemoteVideoStageResult : std::uint8_t {
    Success,
    Accepted,
    Dropped,
    Retry,
    Failed,
    Fallback,
    Applied,
    Recovered,
}; // wjy: 热路径只传强类型结果，持久化线程统一转换稳定文本，避免不同模块拼写漂移。

enum class RemoteVideoStageReason : std::uint8_t {
    None,
    NativeFrame,
    LatestSlot,
    PendingReplaced,
    StaleGeneration,
    Deadline,
    SynchronizationBusy,
    DeviceLost,
    SurfaceFailure,
    Presented,
    ProducerKeyReturned,
    FocusedProfile,
    BackgroundProfile,
    MinimizedProfile,
    SoftwareFallback,
    RecoveryProbe,
    Shutdown,
}; // wjy: 解码、队列、同步、Present、回退和恢复共享一套可检索原因枚举。

struct RemoteVideoStageSample {
    RemoteVideoStage stage = RemoteVideoStage::Queue;
    RemoteVideoLogLevel level = RemoteVideoLogLevel::Trace;
    RemoteVideoLogContext context;
    RemoteVideoStageResult result = RemoteVideoStageResult::Success;
    RemoteVideoStageReason reason = RemoteVideoStageReason::None;
    std::uint64_t durationUs = 0;
    std::uint64_t queueDepth = 0;
    std::uint64_t frameAgeUs = 0;
};

enum class RemoteVideoSummaryScope : std::uint8_t {
    Session,
    Adapter,
};

struct RemoteVideoSummary {
    RemoteVideoSummaryScope scope = RemoteVideoSummaryScope::Session;
    RemoteVideoLogContext context;
    std::uint64_t intervalMs = 0;
    std::uint64_t received = 0;
    std::uint64_t presented = 0;
    std::uint64_t dropped = 0;
    std::uint64_t stale = 0;
    double presentedFps = 0.0;
    double dropRatio = 0.0;
    double frameAgeP95Ms = 0.0;
    double averageRenderMs = 0.0;
    double workerUtilization = 0.0;
    std::uint64_t queueDepth = 0;
    std::uint64_t workerBacklog = 0;
    std::uint64_t activeSessions = 0;
    std::uint64_t synchronizationBusy = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
};

std::string_view remoteVideoLogLevelName(RemoteVideoLogLevel level) noexcept;
std::string_view remoteVideoStageName(RemoteVideoStage stage) noexcept;
std::string_view remoteVideoStageResultName(RemoteVideoStageResult result) noexcept;
std::string_view remoteVideoStageReasonName(RemoteVideoStageReason reason) noexcept;
std::string_view remoteVideoSummaryScopeName(RemoteVideoSummaryScope scope) noexcept;

class RemoteVideoDiagnostics final {
public:
    explicit RemoteVideoDiagnostics(
        std::size_t queueLimit = 4096,
        std::size_t traceCapacity = 8192,
        std::uint64_t anomalyPostWindowUs = 2u * 1000u * 1000u,
        std::uint64_t anomalyRateLimitUs = 5u * 1000u * 1000u);
    ~RemoteVideoDiagnostics();

    RemoteVideoDiagnostics(const RemoteVideoDiagnostics&) = delete;
    RemoteVideoDiagnostics& operator=(const RemoteVideoDiagnostics&) = delete;

    bool start(
        const std::filesystem::path& directory,
        std::uint64_t maxFileBytes = 8u * 1024u * 1024u,
        std::size_t maxFiles = 5);
    void stop() noexcept;

    bool submit(RemoteVideoLogEvent event) noexcept;
    bool submitStage(const RemoteVideoStageSample& sample) noexcept;
    bool submitSummary(const RemoteVideoSummary& summary) noexcept;
    bool captureAnomaly(
        const RemoteVideoLogContext& context,
        std::string reason,
        double frameAgeP95Ms,
        double dropRatio,
        std::uint64_t workerBacklog) noexcept;
    bool snapshot(const std::filesystem::path& output) const noexcept;
    std::vector<RemoteVideoLogEvent> traceSnapshot() const;

    std::uint64_t droppedEventCount() const noexcept;
    std::size_t pendingEventCount() const noexcept;
    std::uint64_t anomalyDumpCount() const noexcept;
    std::uint64_t suppressedAnomalyCount() const noexcept;
    std::uint32_t writerThreadId() const noexcept;

private:
    struct AnomalyCapture {
        std::uint64_t id = 0;
        std::uint64_t startedUs = 0;
        std::uint64_t completeAtUs = 0;
        RemoteVideoLogContext context;
        std::string reason;
        std::vector<RemoteVideoLogEvent> events;
    };

    void workerLoop() noexcept;
    void writeEvent(const RemoteVideoLogEvent& event) noexcept;
    void writeAnomalyDump(const AnomalyCapture& capture) noexcept;
    void finalizeReadyAnomaliesLocked(std::uint64_t nowUs, bool force) noexcept;
    bool rotateIfNeeded(std::size_t incomingBytes) noexcept;

    const std::size_t queueLimit_;
    const std::size_t traceCapacity_;
    const std::size_t anomalyEventLimit_; // wjy: 每个异常快照最多保存两倍Trace容量，持续故障不会让单个capture无限追加字符串事件。
    static constexpr std::size_t kActiveAnomalyLimit = 4; // wjy: 同一时刻最多跟踪四个异常窗口，覆盖多窗口故障同时限制内存放大倍数。
    static constexpr std::size_t kCompletedAnomalyLimit = 8; // wjy: 写盘线程落后时最多排队八个完整快照，优先保留最新现场。
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<RemoteVideoLogEvent> queue_;
    std::deque<RemoteVideoLogEvent> trace_;
    std::deque<AnomalyCapture> activeAnomalies_; // wjy: 异常触发时复制前序环形现场，后续事件在内存中继续追加到短窗口结束。
    std::deque<AnomalyCapture> completedAnomalies_; // wjy: 完整快照交给LoggerThread写盘，解码/渲染线程绝不直接创建dump文件。
    std::unordered_map<std::string, std::uint64_t> lastAnomalyUsByKey_;
    std::filesystem::path directory_;
    std::filesystem::path logPath_;
    std::uint64_t maxFileBytes_ = 0;
    std::size_t maxFiles_ = 0;
    std::uint64_t currentFileBytes_ = 0;
    std::ofstream file_;
    std::thread worker_;
    std::atomic_bool running_ = false;
    std::atomic_bool stopRequested_ = false;
    std::atomic_uint64_t droppedEvents_ = 0;
    const std::uint64_t anomalyPostWindowUs_;
    const std::uint64_t anomalyRateLimitUs_;
    std::uint64_t nextAnomalyId_ = 1;
    std::atomic_uint64_t anomalyDumps_ = 0;
    std::atomic_uint64_t suppressedAnomalies_ = 0;
    std::atomic_uint32_t writerThreadId_ = 0;
};
// ===end====

} // namespace stream
