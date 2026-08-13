#include "stream/RemoteVideoDiagnostics.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace stream {
namespace {

// =====wjy====
std::uint64_t nowMonotonicUs() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()); // wjy: 所有阶段耗时使用单调时钟，避免系统时间回拨破坏延迟分析。
}

std::uint32_t currentThreadId() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::GetCurrentThreadId()); // wjy: Windows线程ID可直接与D3D和WebRTC诊断日志关联。
#else
    return static_cast<std::uint32_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id())); // wjy: 非Windows构建保留稳定的进程内线程标识。
#endif
}

std::string escapeJson(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

std::string eventToJson(const RemoteVideoLogEvent& event)
{
    std::ostringstream output;
    output << "{\"monotonic_us\":" << event.monotonicUs
           << ",\"level\":\"" << remoteVideoLogLevelName(event.level)
           << "\",\"event\":\"" << escapeJson(event.event)
           << "\",\"result\":\"" << escapeJson(event.result)
           << "\",\"reason\":\"" << escapeJson(event.reason)
           << "\",\"run_id\":" << event.context.runId
           << ",\"session_id\":" << event.context.sessionId
           << ",\"window_id\":" << event.context.windowId
           << ",\"viewer_generation\":" << event.context.viewerGeneration
           << ",\"frame_id\":" << event.context.frameId
           << ",\"render_job_id\":" << event.context.renderJobId
           << ",\"adapter_index\":" << event.context.adapterIndex
           << ",\"thread_id\":" << event.context.threadId
           << ",\"fields\":{";

    for (std::size_t index = 0; index < event.fields.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << '"' << escapeJson(event.fields[index].key) << "\":\""
               << escapeJson(event.fields[index].value) << '"';
    }
    output << "}}\n";
    return output.str();
}

std::string safeFileToken(std::string_view value)
{
    std::string token;
    token.reserve(std::min<std::size_t>(value.size(), 48));
    for (const char character : value) {
        if (token.size() >= 48) break;
        const bool alphaNumeric = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9');
        token += alphaNumeric ? character : '_'; // wjy: dump文件名只使用安全ASCII，诊断原因原文仍保留在JSON事件内。
    }
    return token.empty() ? "unknown" : token;
}
// ===end====

} // namespace

std::string_view remoteVideoLogLevelName(RemoteVideoLogLevel level) noexcept
{
    switch (level) {
    case RemoteVideoLogLevel::Trace: return "trace";
    case RemoteVideoLogLevel::Debug: return "debug";
    case RemoteVideoLogLevel::Info: return "info";
    case RemoteVideoLogLevel::Warning: return "warning";
    case RemoteVideoLogLevel::Error: return "error";
    }
    return "unknown";
}

std::string_view remoteVideoStageName(RemoteVideoStage stage) noexcept
{
    switch (stage) {
    case RemoteVideoStage::Decode: return "decode";
    case RemoteVideoStage::Queue: return "queue";
    case RemoteVideoStage::Synchronization: return "synchronization";
    case RemoteVideoStage::Processing: return "processing";
    case RemoteVideoStage::Present: return "present";
    case RemoteVideoStage::Release: return "release";
    case RemoteVideoStage::Profile: return "profile";
    case RemoteVideoStage::Fallback: return "fallback";
    case RemoteVideoStage::Recovery: return "recovery";
    }
    return "unknown";
}

std::string_view remoteVideoStageResultName(RemoteVideoStageResult result) noexcept
{
    switch (result) {
    case RemoteVideoStageResult::Success: return "success";
    case RemoteVideoStageResult::Accepted: return "accepted";
    case RemoteVideoStageResult::Dropped: return "dropped";
    case RemoteVideoStageResult::Retry: return "retry";
    case RemoteVideoStageResult::Failed: return "failed";
    case RemoteVideoStageResult::Fallback: return "fallback";
    case RemoteVideoStageResult::Applied: return "applied";
    case RemoteVideoStageResult::Recovered: return "recovered";
    }
    return "unknown";
}

std::string_view remoteVideoStageReasonName(RemoteVideoStageReason reason) noexcept
{
    switch (reason) {
    case RemoteVideoStageReason::None: return "none";
    case RemoteVideoStageReason::NativeFrame: return "native_frame";
    case RemoteVideoStageReason::LatestSlot: return "latest_slot";
    case RemoteVideoStageReason::PendingReplaced: return "pending_replaced";
    case RemoteVideoStageReason::StaleGeneration: return "stale_generation";
    case RemoteVideoStageReason::Deadline: return "deadline";
    case RemoteVideoStageReason::SynchronizationBusy: return "synchronization_busy";
    case RemoteVideoStageReason::DeviceLost: return "device_lost";
    case RemoteVideoStageReason::SurfaceFailure: return "surface_failure";
    case RemoteVideoStageReason::Presented: return "presented";
    case RemoteVideoStageReason::ProducerKeyReturned: return "producer_key_returned";
    case RemoteVideoStageReason::FocusedProfile: return "focused_profile";
    case RemoteVideoStageReason::BackgroundProfile: return "background_profile";
    case RemoteVideoStageReason::MinimizedProfile: return "minimized_profile";
    case RemoteVideoStageReason::SoftwareFallback: return "software_fallback";
    case RemoteVideoStageReason::RecoveryProbe: return "recovery_probe";
    case RemoteVideoStageReason::Shutdown: return "shutdown";
    }
    return "unknown";
}

std::string_view remoteVideoSummaryScopeName(RemoteVideoSummaryScope scope) noexcept
{
    return scope == RemoteVideoSummaryScope::Adapter ? "adapter" : "session";
}

RemoteVideoDiagnostics::RemoteVideoDiagnostics(
    std::size_t queueLimit,
    std::size_t traceCapacity,
    std::uint64_t anomalyPostWindowUs,
    std::uint64_t anomalyRateLimitUs)
    : queueLimit_(std::max<std::size_t>(1, queueLimit))
    , traceCapacity_(std::max<std::size_t>(1, traceCapacity))
    , anomalyEventLimit_(std::max<std::size_t>(2, traceCapacity_) * 2) // wjy: 构造时固定单快照事件上限，运行期不受异常持续时间或帧率影响。
    , anomalyPostWindowUs_(std::max<std::uint64_t>(1000, anomalyPostWindowUs))
    , anomalyRateLimitUs_(std::max<std::uint64_t>(1000, anomalyRateLimitUs))
{
}

RemoteVideoDiagnostics::~RemoteVideoDiagnostics()
{
    stop();
}

bool RemoteVideoDiagnostics::start(
    const std::filesystem::path& directory,
    std::uint64_t maxFileBytes,
    std::size_t maxFiles)
{
    if (running_.load(std::memory_order_acquire)) {
        return true; // wjy: 重复初始化不再创建第二个日志线程，避免同一模块多写文件。
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error); // wjy: 日志目录创建失败不影响后续内存追踪和视频运行。
    if (error) {
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        directory_ = directory;
        logPath_ = directory_ / "video_pipeline.jsonl";
        maxFileBytes_ = std::max<std::uint64_t>(1024, maxFileBytes);
        maxFiles_ = std::max<std::size_t>(1, maxFiles);
        currentFileBytes_ = 0;
        stopRequested_.store(false, std::memory_order_release);
    }

    try {
        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { workerLoop(); }); // wjy: 所有持久化IO集中在单独线程，解码和RenderWorker只投递事件。
    } catch (...) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void RemoteVideoDiagnostics::stop() noexcept
{
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    stopRequested_.store(true, std::memory_order_release); // wjy: 请求线程先排空队列再退出，避免丢失最后的设备/会话边界日志。
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join(); // wjy: 关闭阶段等待LoggerThread完成文件刷新，保证诊断文件末尾完整。
    }
    running_.store(false, std::memory_order_release);
}

bool RemoteVideoDiagnostics::submit(RemoteVideoLogEvent event) noexcept
{
    try {
    if (event.monotonicUs == 0) {
        event.monotonicUs = nowMonotonicUs(); // wjy: 调用方未提供时间时由日志入口补齐统一单调时间。
    }
    if (event.context.threadId == 0) {
        event.context.threadId = currentThreadId(); // wjy: 自动补齐真实产生事件的线程，跨线程追踪无需调用方重复填充。
    }

    std::lock_guard lock(mutex_);
    if (trace_.size() >= traceCapacity_) {
        trace_.pop_front(); // wjy: 环形缓冲只保留最近现场，禁止诊断数据无限增长。
    }
    trace_.push_back(event); // wjy: 即使磁盘暂时不可用，也保留内存现场供异常快照读取。

    for (auto& capture : activeAnomalies_) {
        if (event.monotonicUs >= capture.startedUs
            && capture.events.size() < anomalyEventLimit_) {
            capture.events.push_back(event); // wjy: 触发后的短窗口继续在内存追加，磁盘写入仍只发生在LoggerThread。
        } else if (event.monotonicUs >= capture.startedUs) {
            suppressedAnomalies_.fetch_add(1, std::memory_order_relaxed); // wjy: 单快照达到硬上限后只累计抑制计数，不继续复制高频事件。
        }
    }
    finalizeReadyAnomaliesLocked(event.monotonicUs, false);
    if (!event.durable) {
        return true; // wjy: 帧级Trace/Debug只保留在环形现场和活动异常窗口，正常运行不逐帧写盘。
    }
    if (!running_.load(std::memory_order_acquire)) {
        return false; // wjy: 未启动持久化线程时不伪造写入成功，但不丢弃内存事件。
    }
    if (queue_.size() >= queueLimit_) {
        queue_.pop_front(); // wjy: 日志队列满时丢弃最旧事件，优先保留最新故障现场。
        droppedEvents_.fetch_add(1, std::memory_order_relaxed);
    }
    queue_.push_back(std::move(event)); // wjy: 事件入队后立刻释放业务线程栈上的字符串和字段所有权。
    condition_.notify_one();
    return true;
    } catch (...) {
        droppedEvents_.fetch_add(1, std::memory_order_relaxed); // wjy: 热路径分配失败转换为计数，不允许noexcept入口终止视频线程。
        return false;
    }
}

bool RemoteVideoDiagnostics::submitStage(const RemoteVideoStageSample& sample) noexcept
{
    RemoteVideoLogEvent event;
    event.level = sample.level;
    event.durable = sample.level >= RemoteVideoLogLevel::Info; // wjy: 只有Info以上阶段事件进入主JSONL，逐帧阶段仍可在异常dump中重建。
    event.context = sample.context;
    event.event = std::string("stage_") + std::string(remoteVideoStageName(sample.stage));
    event.result = remoteVideoStageResultName(sample.result);
    event.reason = remoteVideoStageReasonName(sample.reason); // wjy: 强类型结果/原因只在统一序列化边界转换为稳定文本。
    event.fields = {
        {"duration_us", std::to_string(sample.durationUs)},
        {"queue_depth", std::to_string(sample.queueDepth)},
        {"frame_age_us", std::to_string(sample.frameAgeUs)},
    };
    return submit(std::move(event));
}

bool RemoteVideoDiagnostics::submitSummary(const RemoteVideoSummary& summary) noexcept
{
    RemoteVideoLogEvent event;
    event.level = RemoteVideoLogLevel::Info;
    event.context = summary.context;
    event.event = "summary";
    event.result = "interval";
    event.reason = remoteVideoSummaryScopeName(summary.scope);
    event.fields = {
        {"interval_ms", std::to_string(summary.intervalMs)},
        {"received", std::to_string(summary.received)},
        {"presented", std::to_string(summary.presented)},
        {"dropped", std::to_string(summary.dropped)},
        {"stale", std::to_string(summary.stale)},
        {"presented_fps", std::to_string(summary.presentedFps)},
        {"drop_ratio", std::to_string(summary.dropRatio)},
        {"frame_age_p95_ms", std::to_string(summary.frameAgeP95Ms)},
        {"average_render_ms", std::to_string(summary.averageRenderMs)},
        {"worker_utilization", std::to_string(summary.workerUtilization)},
        {"queue_depth", std::to_string(summary.queueDepth)},
        {"worker_backlog", std::to_string(summary.workerBacklog)},
        {"active_sessions", std::to_string(summary.activeSessions)},
        {"synchronization_busy", std::to_string(summary.synchronizationBusy)},
        {"cache_hits", std::to_string(summary.cacheHits)},
        {"cache_misses", std::to_string(summary.cacheMisses)},
    };
    return submit(std::move(event));
}

bool RemoteVideoDiagnostics::captureAnomaly(
    const RemoteVideoLogContext& context,
    std::string reason,
    double frameAgeP95Ms,
    double dropRatio,
    std::uint64_t workerBacklog) noexcept
{
    try {
        const std::uint64_t capturedAtUs = nowMonotonicUs();
        const std::string rateKey = std::to_string(context.adapterIndex) + ":"
            + std::to_string(context.sessionId) + ":" + reason;
        {
            std::lock_guard lock(mutex_);
            const auto previous = lastAnomalyUsByKey_.find(rateKey);
            if (previous != lastAnomalyUsByKey_.end()
                && capturedAtUs - previous->second < anomalyRateLimitUs_) {
                suppressedAnomalies_.fetch_add(1, std::memory_order_relaxed);
                return false; // wjy: 同会话同原因在限频窗口内只计数，不重复复制环形现场或制造磁盘风暴。
            }
            lastAnomalyUsByKey_[rateKey] = capturedAtUs;
            AnomalyCapture capture;
            capture.id = nextAnomalyId_++;
            capture.startedUs = capturedAtUs;
            capture.completeAtUs = capturedAtUs + anomalyPostWindowUs_;
            capture.context = context;
            capture.reason = reason;
            capture.events.assign(trace_.begin(), trace_.end()); // wjy: 触发瞬间复制前序现场；锁外不再读取可能滚动的主环形缓冲。
            if (activeAnomalies_.size() >= kActiveAnomalyLimit) {
                activeAnomalies_.pop_front(); // wjy: 新异常优先保留，最旧活动窗口被有界淘汰且不会继续复制后序事件。
                suppressedAnomalies_.fetch_add(1, std::memory_order_relaxed);
            }
            activeAnomalies_.push_back(std::move(capture));
        }

    RemoteVideoLogEvent event;
    event.monotonicUs = capturedAtUs;
    event.level = RemoteVideoLogLevel::Warning;
    event.context = context;
    event.event = "anomaly";
    event.result = "capture_started";
    event.reason = std::move(reason);
    event.fields = {
        {"frame_age_p95_ms", std::to_string(frameAgeP95Ms)},
        {"drop_ratio", std::to_string(dropRatio)},
        {"worker_backlog", std::to_string(workerBacklog)},
        {"trace_capacity", std::to_string(traceCapacity_)},
    };
        condition_.notify_one();
        return submit(std::move(event));
    } catch (...) {
        suppressedAnomalies_.fetch_add(1, std::memory_order_relaxed); // wjy: 异常快照分配失败只增加计数，视频热路径保持运行。
        return false;
    }
}

bool RemoteVideoDiagnostics::snapshot(const std::filesystem::path& output) const noexcept
{
    std::vector<RemoteVideoLogEvent> events;
    {
        std::lock_guard lock(mutex_);
        events.assign(trace_.begin(), trace_.end()); // wjy: 复制环形现场后在锁外写盘，避免阻塞日志提交者。
    }

    std::error_code error;
    if (!output.parent_path().empty()) {
        std::filesystem::create_directories(output.parent_path(), error);
        if (error) {
            return false;
        }
    }
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    for (const auto& event : events) {
        file << eventToJson(event); // wjy: 快照使用同一JSONL格式，便于与正常日志拼接分析。
    }
    return file.good();
}

std::vector<RemoteVideoLogEvent> RemoteVideoDiagnostics::traceSnapshot() const
{
    std::lock_guard lock(mutex_);
    return {trace_.begin(), trace_.end()}; // wjy: 测试和UI诊断面板可读取最近现场而无需访问日志文件。
}

std::uint64_t RemoteVideoDiagnostics::droppedEventCount() const noexcept
{
    return droppedEvents_.load(std::memory_order_relaxed);
}

std::size_t RemoteVideoDiagnostics::pendingEventCount() const noexcept
{
    std::lock_guard lock(mutex_);
    return queue_.size();
}

std::uint64_t RemoteVideoDiagnostics::anomalyDumpCount() const noexcept
{
    return anomalyDumps_.load(std::memory_order_relaxed);
}

std::uint64_t RemoteVideoDiagnostics::suppressedAnomalyCount() const noexcept
{
    return suppressedAnomalies_.load(std::memory_order_relaxed);
}

std::uint32_t RemoteVideoDiagnostics::writerThreadId() const noexcept
{
    return writerThreadId_.load(std::memory_order_acquire); // wjy: 测试可证明文件写入线程与提交帧事件的解码/RenderWorker线程不同。
}

void RemoteVideoDiagnostics::finalizeReadyAnomaliesLocked(
    std::uint64_t nowUs,
    bool force) noexcept
{
    try {
        for (auto iterator = activeAnomalies_.begin(); iterator != activeAnomalies_.end();) {
            if (!force && nowUs < iterator->completeAtUs) {
                ++iterator;
                continue;
            }
            if (completedAnomalies_.size() >= kCompletedAnomalyLimit) {
                completedAnomalies_.pop_front(); // wjy: LoggerThread落后时丢弃最旧完整快照，队列字节数不再随持续故障无限增长。
                suppressedAnomalies_.fetch_add(1, std::memory_order_relaxed);
            }
            completedAnomalies_.push_back(std::move(*iterator)); // wjy: 完整前后窗口转交LoggerThread有界队列，业务线程不打开文件。
            iterator = activeAnomalies_.erase(iterator);
        }
        if (!completedAnomalies_.empty()) {
            condition_.notify_one();
        }
    } catch (...) {
        suppressedAnomalies_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool RemoteVideoDiagnostics::rotateIfNeeded(std::size_t incomingBytes) noexcept
{
    if (!file_.is_open()) {
        file_.open(logPath_, std::ios::binary | std::ios::app); // wjy: 文件暂时关闭时惰性恢复，日志故障不影响视频线程。
        if (!file_.is_open()) {
            return false;
        }
        std::error_code sizeError;
        currentFileBytes_ = std::filesystem::file_size(logPath_, sizeError);
        if (sizeError) {
            currentFileBytes_ = 0;
        }
    }

    if (currentFileBytes_ == 0 || currentFileBytes_ + incomingBytes <= maxFileBytes_) {
        return true;
    }

    file_.close();
    std::error_code error;
    if (maxFiles_ > 1) {
        for (std::size_t index = maxFiles_ - 1; index > 0; --index) {
            const auto from = index == 1
                ? logPath_
                : std::filesystem::path(logPath_.string() + "." + std::to_string(index - 1));
            const auto to = std::filesystem::path(logPath_.string() + "." + std::to_string(index));
            std::filesystem::remove(to, error);
            error.clear();
            if (std::filesystem::exists(from, error)) {
                std::filesystem::rename(from, to, error); // wjy: 轮转旧文件而不是删除全部历史，保留最近几次故障现场。
                error.clear();
            }
        }
    } else {
        std::filesystem::remove(logPath_, error);
        error.clear();
    }

    file_.open(logPath_, std::ios::binary | std::ios::trunc);
    currentFileBytes_ = 0;
    return file_.is_open();
}

void RemoteVideoDiagnostics::writeEvent(const RemoteVideoLogEvent& event) noexcept
{
    try {
        const std::string line = eventToJson(event);
        if (!rotateIfNeeded(line.size())) {
            return; // wjy: 目录或权限故障只影响持久化，不把异常传播到LoggerThread外。
        }
        file_.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (!file_.good()) {
            file_.clear();
            file_.close(); // wjy: 写入失败时关闭坏文件，后续事件可以重新尝试打开。
            return;
        }
        currentFileBytes_ += static_cast<std::uint64_t>(line.size());
        if (event.level == RemoteVideoLogLevel::Error) {
            file_.flush(); // wjy: 关键错误立即落盘，普通帧事件交给批量写入减少IO压力。
        }
    } catch (...) {
        // wjy: 日志持久化绝不能终止视频、WebRTC或RenderWorker线程。
    }
}

void RemoteVideoDiagnostics::writeAnomalyDump(const AnomalyCapture& capture) noexcept
{
    try {
        const auto path = directory_ / (
            "anomaly_" + std::to_string(capture.id) + "_"
            + safeFileToken(capture.reason) + ".jsonl");
        std::ofstream dump(path, std::ios::binary | std::ios::trunc);
        if (!dump.is_open()) {
            return; // wjy: dump目录或权限故障不影响主日志、内存环形现场和视频链路。
        }
        for (const auto& event : capture.events) {
            dump << eventToJson(event);
        }
        dump.flush();
        if (dump.good()) {
            anomalyDumps_.fetch_add(1, std::memory_order_relaxed); // wjy: 只有完整刷新成功才计入可用异常快照。
        }
    } catch (...) {
        // wjy: 异常dump属于辅助诊断，任何文件或分配异常都不得逃出LoggerThread。
    }
}

void RemoteVideoDiagnostics::workerLoop() noexcept
{
    writerThreadId_.store(currentThreadId(), std::memory_order_release); // wjy: 持久化线程身份公开给测试和诊断快照，证明热路径不执行文件IO。
    for (;;) {
        RemoteVideoLogEvent event;
        AnomalyCapture anomaly;
        bool hasEvent = false;
        bool hasAnomaly = false;
        {
            std::unique_lock lock(mutex_);
            condition_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return stopRequested_.load(std::memory_order_acquire)
                    || !queue_.empty() || !completedAnomalies_.empty();
            });
            finalizeReadyAnomaliesLocked(
                nowMonotonicUs(), stopRequested_.load(std::memory_order_acquire)); // wjy: 即使异常后没有新帧，LoggerThread也会按时间完成后序窗口；关闭时立即收尾。
            if (queue_.empty() && completedAnomalies_.empty()
                && stopRequested_.load(std::memory_order_acquire)) {
                break;
            }
            if (!queue_.empty()) {
                event = std::move(queue_.front()); // wjy: 取出事件后立刻释放队列锁，文件IO不阻塞生产者。
                queue_.pop_front();
                hasEvent = true;
            } else if (!completedAnomalies_.empty()) {
                anomaly = std::move(completedAnomalies_.front());
                completedAnomalies_.pop_front();
                hasAnomaly = true;
            }
        }
        if (hasEvent) {
            writeEvent(event);
        }
        if (hasAnomaly) {
            writeAnomalyDump(anomaly); // wjy: 异常文件与主JSONL共用唯一写盘线程，RenderWorker只负责内存提交。
        }
    }

    if (file_.is_open()) {
        file_.flush();
        file_.close(); // wjy: LoggerThread退出前刷新最后一批事件，保证应用关闭日志可读。
    }
}

} // namespace stream
