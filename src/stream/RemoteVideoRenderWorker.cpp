#include "stream/RemoteVideoRenderWorker.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>

namespace stream {
namespace {

std::int64_t monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::uint64_t monotonicUs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint32_t currentThreadId()
{
    return static_cast<std::uint32_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

RemoteVideoScheduleState scheduleStateFromSurface(const RemoteVideoSurfaceState& state, bool frameReady)
{
    RemoteVideoScheduleState result;
    result.windowId = state.windowId;
    result.role = !state.visible
        ? RemoteVideoWindowRole::Hidden
        : (state.minimized
            ? RemoteVideoWindowRole::Minimized
            : (state.focused ? RemoteVideoWindowRole::Focused : RemoteVideoWindowRole::VisibleBackground));
    result.targetFps = state.profile.targetFps;
    result.priority = state.profile.priority;
    result.generation = state.generation;
    result.frameReady = frameReady;
    return result;
}

RemoteVideoStageReason profileReason(const RemoteVideoSurfaceState& state)
{
    if (state.minimized || !state.visible) {
        return RemoteVideoStageReason::MinimizedProfile;
    }
    return state.focused
        ? RemoteVideoStageReason::FocusedProfile
        : RemoteVideoStageReason::BackgroundProfile;
}

} // namespace

// =====wjy====
RemoteVideoRenderWorker::RemoteVideoRenderWorker(
    std::uint32_t adapterIndex,
    std::shared_ptr<RemoteVideoDiagnostics> diagnostics,
    ResultCallback resultCallback)
    : adapterIndex_(adapterIndex),
      diagnostics_(std::move(diagnostics)),
      resultCallback_(std::move(resultCallback))
{
}

RemoteVideoRenderWorker::~RemoteVideoRenderWorker()
{
    stop();
}

bool RemoteVideoRenderWorker::start()
{
    std::lock_guard lock(mutex_);
    if (running_) {
        return true;
    }
    stopRequested_ = false;
    workerExited_ = false;
    running_ = true;
    lastSummaryMs_ = monotonicMs();
    workerBusyUsSinceSummary_ = 0;
    worker_ = std::thread([this] { workerLoop(); });
    return true;
}

void RemoteVideoRenderWorker::stop() noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (!running_ && workerExited_) {
            return;
        }
        stopRequested_ = true;
        if (commands_.size() < kCommandQueueLimit) {
            commands_.push_back(Command{CommandType::Stop});
        }
    }
    condition_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard lock(mutex_);
    running_ = false;
    workerExited_ = true;
}

bool RemoteVideoRenderWorker::registerSurface(
    std::uint64_t windowId,
    const RemoteVideoSurfaceState& state,
    std::shared_ptr<RemoteVideoRenderSurface> surface)
{
    if (windowId == 0 || !surface || state.windowId != windowId || state.generation == 0) {
        return false;
    }
    Command command;
    command.type = CommandType::Register;
    command.windowId = windowId;
    command.state = state;
    command.surface = std::move(surface);
    return enqueue(std::move(command));
}

bool RemoteVideoRenderWorker::updateSurface(const RemoteVideoSurfaceState& state)
{
    if (state.windowId == 0 || state.generation == 0) {
        return false;
    }
    Command command;
    command.type = CommandType::Update;
    command.windowId = state.windowId;
    command.state = state;
    return enqueue(std::move(command));
}

bool RemoteVideoRenderWorker::unregisterSurface(std::uint64_t windowId)
{
    if (windowId == 0) {
        return false;
    }
    Command command;
    command.type = CommandType::Unregister;
    command.windowId = windowId;
    return enqueue(std::move(command));
}

bool RemoteVideoRenderWorker::submitFrame(std::uint64_t windowId, NativeVideoFrame frame)
{
    if (windowId == 0 || !frame.isValid()) {
        frame.release(RemoteVideoFrameReleaseReason::Invalid);
        return false;
    }
    Command command;
    command.type = CommandType::Submit;
    command.windowId = windowId;
    command.frame = std::move(frame);
    return enqueue(std::move(command));
}

bool RemoteVideoRenderWorker::invalidateGeneration(std::uint64_t windowId, std::uint64_t generation)
{
    if (windowId == 0 || generation == 0) {
        return false;
    }
    Command command;
    command.type = CommandType::Invalidate;
    command.windowId = windowId;
    command.generation = generation;
    return enqueue(std::move(command));
}

bool RemoteVideoRenderWorker::waitForIdle(std::chrono::milliseconds timeout)
{
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return true;
        }
    }
    auto barrier = std::make_shared<std::promise<void>>();
    std::future<void> future = barrier->get_future();
    Command command;
    command.type = CommandType::Barrier;
    command.barrier = std::move(barrier);
    if (!enqueue(std::move(command))) {
        return false;
    }
    return future.wait_for(timeout) == std::future_status::ready;
}

std::size_t RemoteVideoRenderWorker::registeredSurfaceCount() const noexcept
{
    return registeredSurfaceCount_.load(std::memory_order_acquire);
}

bool RemoteVideoRenderWorker::enqueue(Command command)
{
    {
        std::lock_guard lock(mutex_);
        if (!running_ || stopRequested_ || commands_.size() >= kCommandQueueLimit) {
            return false;
        }
        commands_.push_back(std::move(command));
    }
    condition_.notify_one();
    return true;
}

void RemoteVideoRenderWorker::workerLoop() noexcept
{
    for (;;) {
        Command command;
        bool hasCommand = false;
        {
            std::unique_lock lock(mutex_);
            condition_.wait_for(lock, std::chrono::milliseconds(2), [this] {
                return stopRequested_ || !commands_.empty();
            });
            if (commands_.empty() && stopRequested_) {
                break;
            }
            if (!commands_.empty()) {
                command = std::move(commands_.front());
                commands_.pop_front();
                ++activeCommands_;
                hasCommand = true;
            }
        }

        const std::uint64_t workStartedUs = monotonicUs();
        if (hasCommand) {
            processCommand(std::move(command));
        }
        {
            std::lock_guard lock(mutex_);
            if (activeCommands_ > 0) {
                --activeCommands_;
            }
            if (commands_.empty() && activeCommands_ == 0) {
                idleCondition_.notify_all();
            }
        }

        const std::int64_t nowMs = monotonicMs();
        processRetiringFrames(nowMs); // wjy: 先归还被替换/关闭帧的key 0，优先解除解码输出池背压。
        processReadyFrames(nowMs);
        workerBusyUsSinceSummary_ += monotonicUs() - workStartedUs; // wjy: 只累计命令、退役和呈现处理时间，2ms等待不计入适配器利用率。
        emitPeriodicSummaries(nowMs);
        if (hasCommand && command.type == CommandType::Stop) {
            break;
        }
    }

    for (auto& [windowId, session] : sessions_) {
        releaseSession(session, RemoteVideoFrameReleaseReason::Shutdown);
        if (session.surface) {
            session.surface->clear();
        }
        (void)windowId;
    }
    sessions_.clear();
    registeredSurfaceCount_.store(0, std::memory_order_release);
    scheduler_ = {};
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        workerExited_ = true;
        idleCondition_.notify_all();
    }
}

void RemoteVideoRenderWorker::processCommand(Command&& command)
{
    switch (command.type) {
    case CommandType::Register: {
        auto old = sessions_.find(command.windowId);
        if (old != sessions_.end()) {
            releaseSession(old->second, RemoteVideoFrameReleaseReason::Shutdown);
            if (old->second.surface) {
                old->second.surface->clear();
            }
            scheduler_.remove(command.windowId);
            sessions_.erase(old);
        }
        auto [sessionIt, inserted] = sessions_.try_emplace(command.windowId);
        (void)inserted;
        Session& session = sessionIt->second;
        session.state = command.state;
        session.surface = std::move(command.surface);
        session.surface->applyState(session.state);
        session.lastSurfaceMetrics = session.surface->diagnostics();
        registeredSurfaceCount_.store(sessions_.size(), std::memory_order_release);
        scheduler_.upsert(scheduleStateFromSurface(command.state, false), monotonicMs());
        logEvent(RemoteVideoLogLevel::Info, "surface_register", "success", "command", command.windowId,
                 command.state.sessionId, command.state.generation);
        RemoteVideoLogContext profileContext;
        profileContext.windowId = command.windowId;
        profileContext.sessionId = command.state.sessionId;
        profileContext.viewerGeneration = command.state.generation;
        profileContext.adapterIndex = adapterIndex_;
        logStage(RemoteVideoStage::Profile, RemoteVideoStageResult::Applied,
                 profileReason(command.state), profileContext); // wjy: 新Surface初始角色使用强类型profile事件记录。
        break;
    }
    case CommandType::Update: {
        const auto it = sessions_.find(command.windowId);
        if (it == sessions_.end()) {
            break;
        }
        const bool profileChanged = it->second.state.profile.resolution != command.state.profile.resolution
            || it->second.state.profile.targetFps != command.state.profile.targetFps
            || it->second.state.focused != command.state.focused
            || it->second.state.minimized != command.state.minimized
            || it->second.state.visible != command.state.visible;
        if (it->second.state.generation != command.state.generation) {
            releaseSession(it->second, RemoteVideoFrameReleaseReason::StaleGeneration); // wjy: 新Viewer代际先清理旧pending纹理，再切换Surface身份。
        }
        it->second.state = command.state;
        it->second.surface->applyState(it->second.state);
        const bool frameReady = it->second.inFlight.has_value()
            || it->second.inbox.pendingCount() != 0;
        scheduler_.upsert(scheduleStateFromSurface(command.state, frameReady), monotonicMs()); // wjy: 几何/焦点更新不能清掉已经排队的帧就绪状态。
        logEvent(RemoteVideoLogLevel::Debug, "surface_update", "success", "command", command.windowId,
                 command.state.sessionId, command.state.generation);
        if (profileChanged) {
            RemoteVideoLogContext profileContext;
            profileContext.windowId = command.windowId;
            profileContext.sessionId = command.state.sessionId;
            profileContext.viewerGeneration = command.state.generation;
            profileContext.adapterIndex = adapterIndex_;
            logStage(RemoteVideoStage::Profile, RemoteVideoStageResult::Applied,
                     profileReason(command.state), profileContext); // wjy: 焦点/最小化/档位变化只记录一次状态命令，不产生逐帧profile日志。
        }
        break;
    }
    case CommandType::Unregister: {
        const auto it = sessions_.find(command.windowId);
        if (it != sessions_.end()) {
            releaseSession(it->second, RemoteVideoFrameReleaseReason::Shutdown);
            if (it->second.surface) {
                it->second.surface->clear();
            }
            scheduler_.remove(command.windowId);
            sessions_.erase(it);
            registeredSurfaceCount_.store(sessions_.size(), std::memory_order_release);
        }
        logEvent(RemoteVideoLogLevel::Info, "surface_unregister", "success", "command", command.windowId);
        break;
    }
    case CommandType::Submit: {
        const std::uint64_t submittedFrameId = command.frame.frameId;
        const std::uint64_t submittedGeneration = command.frame.viewerGeneration;
        const auto it = sessions_.find(command.windowId);
        if (it == sessions_.end() || it->second.state.generation != command.frame.viewerGeneration) {
            if (it != sessions_.end() && it->second.surface) {
                retireFrame(it->second, std::move(command.frame), RemoteVideoFrameReleaseReason::StaleGeneration); // wjy: 旧代际帧先完成key归还，暂时同步忙则保留重试。
            } else {
                command.frame.release(RemoteVideoFrameReleaseReason::StaleGeneration);
            }
            logEvent(RemoteVideoLogLevel::Debug, "frame_queue", "drop", "stale_generation", command.windowId,
                     0, command.frame.viewerGeneration, command.frame.frameId);
            RemoteVideoLogContext queueContext;
            queueContext.windowId = command.windowId;
            queueContext.viewerGeneration = submittedGeneration;
            queueContext.frameId = submittedFrameId;
            queueContext.adapterIndex = adapterIndex_;
            logStage(RemoteVideoStage::Queue, RemoteVideoStageResult::Dropped,
                     RemoteVideoStageReason::StaleGeneration, queueContext, 0, 0, 0, RemoteVideoLogLevel::Debug);
            break;
        }
        auto result = it->second.inbox.publish(std::move(command.frame));
        if (result.replaced) {
            ++it->second.droppedSinceSummary;
            retireFrame(it->second, std::move(*result.replaced), RemoteVideoFrameReleaseReason::PendingReplaced); // wjy: latest-slot覆盖不等于直接析构，必须先把共享纹理归还解码器。
            RemoteVideoLogContext replacedContext;
            replacedContext.windowId = command.windowId;
            replacedContext.sessionId = it->second.state.sessionId;
            replacedContext.viewerGeneration = it->second.state.generation;
            replacedContext.frameId = result.replaced->frameId;
            replacedContext.adapterIndex = adapterIndex_;
            logStage(RemoteVideoStage::Queue, RemoteVideoStageResult::Dropped,
                     RemoteVideoStageReason::PendingReplaced, replacedContext, 0, 1);
        }
        if (result.accepted) {
            ++it->second.receivedSinceSummary;
            scheduler_.markFrameReady(command.windowId, it->second.state.generation);
            logEvent(RemoteVideoLogLevel::Trace, "frame_queue", "accepted", "latest_slot", command.windowId,
                     it->second.state.sessionId, it->second.state.generation);
            RemoteVideoLogContext acceptedContext;
            acceptedContext.windowId = command.windowId;
            acceptedContext.sessionId = it->second.state.sessionId;
            acceptedContext.viewerGeneration = it->second.state.generation;
            acceptedContext.frameId = submittedFrameId;
            acceptedContext.adapterIndex = adapterIndex_;
            logStage(RemoteVideoStage::Queue, RemoteVideoStageResult::Accepted,
                     RemoteVideoStageReason::LatestSlot, acceptedContext, 0,
                     it->second.inbox.pendingCount());
        }
        break;
    }
    case CommandType::Invalidate: {
        const auto it = sessions_.find(command.windowId);
        if (it != sessions_.end() && it->second.state.generation <= command.generation) {
            releaseSession(it->second, RemoteVideoFrameReleaseReason::StaleGeneration);
            it->second.state.generation = command.generation + 1;
            it->second.state.visible = false;
            scheduler_.upsert(scheduleStateFromSurface(it->second.state, false), monotonicMs());
        }
        break;
    }
    case CommandType::Barrier:
        if (command.barrier) {
            command.barrier->set_value();
        }
        break;
    case CommandType::Stop:
        break;
    }
}

void RemoteVideoRenderWorker::processRetiringFrames(std::int64_t nowMs)
{
    for (auto& [windowId, session] : sessions_) {
        const std::size_t pendingCount = session.retiringFrames.size(); // wjy: 每轮最多处理进入本轮前的数量，失败帧退避后排到队尾，避免同一次循环自旋。
        for (std::size_t index = 0; index < pendingCount; ++index) {
            RetiringFrame retiring = std::move(session.retiringFrames.front());
            session.retiringFrames.pop_front();
            if (retiring.nextRetryMs > nowMs) {
                session.retiringFrames.push_back(std::move(retiring));
                continue;
            }

            const std::uint64_t frameId = retiring.frame.frameId;
            const auto lease = retiring.frame.lease;
            const auto result = session.surface
                ? session.surface->discard(retiring.frame)
                : RemoteVideoRenderResult::Failed;
            if (result == RemoteVideoRenderResult::RetrySync) {
                ++session.synchronizationBusySinceSummary;
                ++retiring.attempts;
                const std::int64_t delayMs = std::min<std::int64_t>(16, 1LL << std::min<std::uint32_t>(retiring.attempts, 4));
                retiring.nextRetryMs = nowMs + delayMs; // wjy: 2/4/8/16ms有界退避，既等待key交接又不阻塞其它窗口。
                session.retiringFrames.push_back(std::move(retiring)); // wjy: lease与句柄继续由RenderWorker持有，成功Acquire/Release前绝不提前释放。
                logEvent(RemoteVideoLogLevel::Debug, "frame_release", "retry", "sync_busy", windowId,
                         session.state.sessionId, session.state.generation, frameId);
                continue;
            }

            if (lease) {
                lease->release(retiring.reason); // wjy: discard已成功归还key或确认设备失败后，才结束应用层帧租约。
            }
            logEvent(
                result == RemoteVideoRenderResult::DeviceLost || result == RemoteVideoRenderResult::Failed
                    ? RemoteVideoLogLevel::Error
                    : RemoteVideoLogLevel::Trace,
                "frame_release",
                result == RemoteVideoRenderResult::DroppedStale ? "success" : "failed",
                result == RemoteVideoRenderResult::DroppedStale ? "producer_key_returned" : "surface_release_failure",
                windowId,
                session.state.sessionId,
                session.state.generation,
                frameId);
            RemoteVideoLogContext releaseContext;
            releaseContext.windowId = windowId;
            releaseContext.sessionId = session.state.sessionId;
            releaseContext.viewerGeneration = session.state.generation;
            releaseContext.frameId = frameId;
            releaseContext.adapterIndex = adapterIndex_;
            logStage(RemoteVideoStage::Release,
                     result == RemoteVideoRenderResult::DroppedStale
                         ? RemoteVideoStageResult::Success
                         : RemoteVideoStageResult::Failed,
                     result == RemoteVideoRenderResult::DroppedStale
                         ? RemoteVideoStageReason::ProducerKeyReturned
                         : RemoteVideoStageReason::SurfaceFailure,
                     releaseContext,
                     0,
                     session.retiringFrames.size(),
                     0,
                     result == RemoteVideoRenderResult::DroppedStale
                         ? RemoteVideoLogLevel::Trace
                         : RemoteVideoLogLevel::Error);
        }
    }
}

void RemoteVideoRenderWorker::processReadyFrames(std::int64_t nowMs)
{
    for (;;) {
        const auto decision = scheduler_.next(nowMs);
        if (!decision) {
            return;
        }
        const auto it = sessions_.find(decision->windowId);
        if (it == sessions_.end()) {
            scheduler_.remove(decision->windowId);
            continue;
        }
        Session& session = it->second;
        if (!session.inFlight) {
            session.inFlight = session.inbox.takeLatest(); // wjy: 从latest槽取出的帧成为唯一in-flight；同步忙时保留原对象等待下一轮。
            if (!session.inFlight) {
                scheduler_.markDropped(decision->windowId, nowMs);
                continue;
            }
        }
        NativeVideoFrame& frame = *session.inFlight;
        const std::uint64_t jobId = nextRenderJobId_++;
        if (frame.viewerGeneration != session.state.generation) {
            const auto result = session.surface->discard(frame);
            if (result == RemoteVideoRenderResult::RetrySync) {
                scheduler_.markRetry(decision->windowId, nowMs); // wjy: 旧代际也必须等真实key交接后再释放，不能因身份失效泄漏输出槽。
                continue;
            }
            frame.release(RemoteVideoFrameReleaseReason::StaleGeneration);
            ++session.droppedSinceSummary;
            ++session.staleSinceSummary;
            const std::uint64_t frameId = frame.frameId;
            session.inFlight.reset();
            scheduler_.markDropped(decision->windowId, nowMs);
            logEvent(RemoteVideoLogLevel::Debug, "frame_render", "drop", "stale_generation", decision->windowId,
                     session.state.sessionId, session.state.generation, frameId, jobId);
            RemoteVideoLogContext staleContext;
            staleContext.windowId = decision->windowId;
            staleContext.sessionId = session.state.sessionId;
            staleContext.viewerGeneration = session.state.generation;
            staleContext.frameId = frameId;
            staleContext.renderJobId = jobId;
            staleContext.adapterIndex = adapterIndex_;
            logStage(RemoteVideoStage::Release, RemoteVideoStageResult::Dropped,
                     RemoteVideoStageReason::StaleGeneration, staleContext, 0,
                     session.inbox.pendingCount());
            if (session.inbox.pendingCount() != 0) {
                scheduler_.markFrameReady(decision->windowId, session.state.generation);
            }
            continue;
        }
        const std::uint64_t frameId = frame.frameId;
        const int frameWidth = frame.width;
        const int frameHeight = frame.height;
        const double encodedMbps = frame.encodedMbps;
        const auto lease = frame.lease;
        const std::uint64_t renderStartedUs = monotonicUs();
        const std::uint64_t frameAgeUs = frame.decodedAtUs > 0
            && static_cast<std::uint64_t>(frame.decodedAtUs) <= renderStartedUs
            ? renderStartedUs - static_cast<std::uint64_t>(frame.decodedAtUs)
            : 0;
        const auto result = session.surface->render(frame);
        const std::uint64_t renderDurationUs = monotonicUs() - renderStartedUs;
        if (result == RemoteVideoRenderResult::RetrySync) {
            ++session.synchronizationBusySinceSummary;
            scheduler_.markRetry(decision->windowId, nowMs); // wjy: 解码器交接或共享纹理短暂繁忙时保留帧并让出本轮worker，不计入丢帧。
            logEvent(RemoteVideoLogLevel::Debug, "frame_sync", "retry", "consumer_key_pending", decision->windowId,
                     session.state.sessionId, session.state.generation, frameId, jobId);
            RemoteVideoLogContext syncContext;
            syncContext.windowId = decision->windowId;
            syncContext.sessionId = session.state.sessionId;
            syncContext.viewerGeneration = session.state.generation;
            syncContext.frameId = frameId;
            syncContext.renderJobId = jobId;
            syncContext.adapterIndex = adapterIndex_;
            logStage(RemoteVideoStage::Synchronization, RemoteVideoStageResult::Retry,
                     RemoteVideoStageReason::SynchronizationBusy, syncContext,
                     renderDurationUs, session.inbox.pendingCount(), frameAgeUs, RemoteVideoLogLevel::Debug);
            nowMs = monotonicMs();
            continue;
        }
        if (session.frameAgeSamplesUs.size() >= 256) {
            session.frameAgeSamplesUs.pop_front();
        }
        session.frameAgeSamplesUs.push_back(frameAgeUs); // wjy: 只在本帧结束同步重试后采样一次，避免同一帧重复抬高p95。
        session.renderTimeUsSinceSummary += renderDurationUs;
        ++session.renderSamplesSinceSummary;
        RemoteVideoLogContext frameContext;
        frameContext.windowId = decision->windowId;
        frameContext.sessionId = session.state.sessionId;
        frameContext.viewerGeneration = session.state.generation;
        frameContext.frameId = frameId;
        frameContext.renderJobId = jobId;
        frameContext.adapterIndex = adapterIndex_;
        if (result == RemoteVideoRenderResult::Presented) {
            ++session.presentedSinceNotification;
            ++session.presentedSinceSummary;
            if (lease) {
                lease->release(RemoteVideoFrameReleaseReason::RenderCompleted);
            }
            scheduler_.markPresented(decision->windowId, nowMs);
            logEvent(RemoteVideoLogLevel::Trace, "frame_present", "success", "presented", decision->windowId,
                     session.state.sessionId, session.state.generation, frameId, jobId);
            logStage(RemoteVideoStage::Processing, RemoteVideoStageResult::Success,
                     RemoteVideoStageReason::NativeFrame, frameContext, renderDurationUs, 0, frameAgeUs);
            logStage(RemoteVideoStage::Present, RemoteVideoStageResult::Success,
                     RemoteVideoStageReason::Presented, frameContext, renderDurationUs, 0, frameAgeUs);
            logStage(RemoteVideoStage::Release, RemoteVideoStageResult::Success,
                     RemoteVideoStageReason::ProducerKeyReturned, frameContext);
        } else {
            ++session.droppedSinceNotification;
            ++session.droppedSinceSummary;
            if (result == RemoteVideoRenderResult::DroppedStale) {
                ++session.staleSinceSummary;
            }
            const auto reason = result == RemoteVideoRenderResult::DroppedStale
                ? RemoteVideoFrameReleaseReason::StaleGeneration
                : RemoteVideoFrameReleaseReason::ExplicitDrop;
            if (lease) {
                lease->release(reason);
            }
            scheduler_.markDropped(decision->windowId, nowMs);
            logEvent(
                result == RemoteVideoRenderResult::DeviceLost ? RemoteVideoLogLevel::Error : RemoteVideoLogLevel::Debug,
                "frame_present",
                "drop",
                result == RemoteVideoRenderResult::DeviceLost ? "device_lost" : "surface_rejected",
                decision->windowId,
                session.state.sessionId,
                session.state.generation,
                frameId,
                jobId);
            const RemoteVideoStageReason typedReason = result == RemoteVideoRenderResult::DeviceLost
                ? RemoteVideoStageReason::DeviceLost
                : RemoteVideoStageReason::SurfaceFailure;
            logStage(RemoteVideoStage::Present, RemoteVideoStageResult::Dropped,
                     typedReason, frameContext, renderDurationUs, 0, frameAgeUs,
                     result == RemoteVideoRenderResult::DeviceLost
                         ? RemoteVideoLogLevel::Error
                         : RemoteVideoLogLevel::Debug);
            logStage(RemoteVideoStage::Release, RemoteVideoStageResult::Success,
                     RemoteVideoStageReason::ProducerKeyReturned, frameContext);
        }
        session.inFlight.reset(); // wjy: 只有Presented、明确丢弃或故障结果已经结束纹理所有权后才清空in-flight。
        if (session.inbox.pendingCount() != 0) {
            scheduler_.markFrameReady(decision->windowId, session.state.generation); // wjy: 重试期间积累的最新pending帧在本帧结束后立即恢复调度。
        }
        const bool firstPresentedAfterNonPresent = result == RemoteVideoRenderResult::Presented
            && session.lastResult != RemoteVideoRenderResult::Presented;
        const bool urgentFailure = result == RemoteVideoRenderResult::DeviceLost
            || result == RemoteVideoRenderResult::Failed;
        const bool periodic = session.lastNotificationMs == 0
            || nowMs - session.lastNotificationMs >= 1000;
        if (resultCallback_ && (firstPresentedAfterNonPresent || urgentFailure || periodic)) {
            try {
                resultCallback_({
                    decision->windowId,
                    session.state.sessionId,
                    session.state.generation,
                    frameId,
                    frameWidth,
                    frameHeight,
                    encodedMbps,
                    session.presentedSinceNotification,
                    session.droppedSinceNotification,
                    result,
                }); // wjy: 只上报低频状态对象，Qt层通过QueuedConnection更新界面，RenderWorker不直接访问QWidget。
                session.presentedSinceNotification = 0;
                session.droppedSinceNotification = 0;
                session.lastNotificationMs = nowMs;
                session.lastResult = result;
            } catch (...) {
                // wjy: 观察者异常不能终止适配器RenderWorker。
            }
        }
        nowMs = monotonicMs();
    }
}

void RemoteVideoRenderWorker::emitPeriodicSummaries(std::int64_t nowMs)
{
    if (!diagnostics_ || lastSummaryMs_ == 0 || nowMs - lastSummaryMs_ < 1000) {
        return;
    }
    const std::uint64_t intervalMs = static_cast<std::uint64_t>(nowMs - lastSummaryMs_);
    std::size_t commandBacklog = 0;
    {
        std::lock_guard lock(mutex_);
        commandBacklog = commands_.size() + activeCommands_; // wjy: 只在每秒汇总时短暂读取命令队列，不在逐帧路径争用队列锁。
    }
    const double utilization = std::min(
        1.0,
        static_cast<double>(workerBusyUsSinceSummary_)
            / static_cast<double>(std::max<std::uint64_t>(1, intervalMs * 1000u)));

    RemoteVideoSummary adapterSummary;
    adapterSummary.scope = RemoteVideoSummaryScope::Adapter;
    adapterSummary.context.adapterIndex = adapterIndex_;
    adapterSummary.intervalMs = intervalMs;
    adapterSummary.workerUtilization = utilization;
    adapterSummary.workerBacklog = commandBacklog;
    adapterSummary.activeSessions = sessions_.size();
    double adapterFrameAgeP95Ms = 0.0;
    std::uint64_t adapterRenderUs = 0;
    std::uint64_t adapterRenderSamples = 0;

    for (auto& [windowId, session] : sessions_) {
        std::vector<std::uint64_t> ages(
            session.frameAgeSamplesUs.begin(), session.frameAgeSamplesUs.end());
        std::uint64_t p95Us = 0;
        if (!ages.empty()) {
            std::sort(ages.begin(), ages.end());
            const std::size_t index = std::min(
                ages.size() - 1,
                static_cast<std::size_t>((ages.size() * 95 + 99) / 100 - 1));
            p95Us = ages[index];
        }

        const std::uint64_t decisions = session.presentedSinceSummary + session.droppedSinceSummary;
        const std::size_t queueDepth = session.inbox.pendingCount()
            + (session.inFlight ? 1u : 0u) + session.retiringFrames.size();
        const RemoteVideoSurfaceMetrics metrics = session.surface
            ? session.surface->diagnostics()
            : RemoteVideoSurfaceMetrics{};
        const auto delta = [](std::uint64_t current, std::uint64_t previous) {
            return current >= previous ? current - previous : current;
        };
        const std::uint64_t cacheHits = delta(
            metrics.textureCacheHits + metrics.viewCacheHits,
            session.lastSurfaceMetrics.textureCacheHits + session.lastSurfaceMetrics.viewCacheHits);
        const std::uint64_t cacheMisses = delta(
            metrics.textureCacheMisses + metrics.viewCacheMisses,
            session.lastSurfaceMetrics.textureCacheMisses + session.lastSurfaceMetrics.viewCacheMisses);

        RemoteVideoSummary summary;
        summary.scope = RemoteVideoSummaryScope::Session;
        summary.context.windowId = windowId;
        summary.context.sessionId = session.state.sessionId;
        summary.context.viewerGeneration = session.state.generation;
        summary.context.adapterIndex = adapterIndex_;
        summary.intervalMs = intervalMs;
        summary.received = session.receivedSinceSummary;
        summary.presented = session.presentedSinceSummary;
        summary.dropped = session.droppedSinceSummary;
        summary.stale = session.staleSinceSummary;
        summary.presentedFps = static_cast<double>(summary.presented) * 1000.0 / intervalMs;
        summary.dropRatio = decisions == 0
            ? 0.0
            : static_cast<double>(summary.dropped) / static_cast<double>(decisions);
        summary.frameAgeP95Ms = static_cast<double>(p95Us) / 1000.0;
        summary.averageRenderMs = session.renderSamplesSinceSummary == 0
            ? 0.0
            : static_cast<double>(session.renderTimeUsSinceSummary)
                / static_cast<double>(session.renderSamplesSinceSummary) / 1000.0;
        summary.workerUtilization = utilization;
        summary.queueDepth = queueDepth;
        summary.workerBacklog = commandBacklog + queueDepth;
        summary.activeSessions = sessions_.size();
        summary.synchronizationBusy = session.synchronizationBusySinceSummary;
        summary.cacheHits = cacheHits;
        summary.cacheMisses = cacheMisses;
        diagnostics_->submitSummary(summary); // wjy: 每会话每秒一条聚合，正常运行不写逐帧持久化日志。

        const bool pressure = (summary.received >= 5 && summary.frameAgeP95Ms >= 150.0)
            || (decisions >= 10 && summary.dropRatio >= 0.25)
            || summary.workerBacklog >= 8
            || summary.synchronizationBusy >= 10;
        session.anomalyPressureIntervals = pressure
            ? session.anomalyPressureIntervals + 1
            : 0; // wjy: 连续两个汇总周期才触发异常dump，单次调度抖动不会制造诊断风暴。
        if (session.anomalyPressureIntervals == 2) {
            const char* reason = summary.frameAgeP95Ms >= 150.0
                ? "frame_age_p95"
                : (summary.dropRatio >= 0.25
                    ? "drop_ratio"
                    : (summary.synchronizationBusy >= 10 ? "synchronization_busy" : "worker_backlog"));
            diagnostics_->captureAnomaly(
                summary.context,
                reason,
                summary.frameAgeP95Ms,
                summary.dropRatio,
                summary.workerBacklog); // wjy: capture只复制内存前序现场并启动后序窗口，文件由LoggerThread稍后写入。
        }

        adapterSummary.received += summary.received;
        adapterSummary.presented += summary.presented;
        adapterSummary.dropped += summary.dropped;
        adapterSummary.stale += summary.stale;
        adapterSummary.synchronizationBusy += summary.synchronizationBusy;
        adapterSummary.cacheHits += summary.cacheHits;
        adapterSummary.cacheMisses += summary.cacheMisses;
        adapterSummary.queueDepth += summary.queueDepth;
        adapterSummary.workerBacklog += summary.queueDepth;
        adapterFrameAgeP95Ms = std::max(adapterFrameAgeP95Ms, summary.frameAgeP95Ms);
        adapterRenderUs += session.renderTimeUsSinceSummary;
        adapterRenderSamples += session.renderSamplesSinceSummary;

        session.receivedSinceSummary = 0;
        session.presentedSinceSummary = 0;
        session.droppedSinceSummary = 0;
        session.staleSinceSummary = 0;
        session.synchronizationBusySinceSummary = 0;
        session.renderTimeUsSinceSummary = 0;
        session.renderSamplesSinceSummary = 0;
        session.frameAgeSamplesUs.clear();
        session.lastSurfaceMetrics = metrics;
    }

    if (!sessions_.empty()) {
        const std::uint64_t adapterDecisions = adapterSummary.presented + adapterSummary.dropped;
        adapterSummary.presentedFps = static_cast<double>(adapterSummary.presented) * 1000.0 / intervalMs;
        adapterSummary.dropRatio = adapterDecisions == 0
            ? 0.0
            : static_cast<double>(adapterSummary.dropped) / static_cast<double>(adapterDecisions);
        adapterSummary.frameAgeP95Ms = adapterFrameAgeP95Ms;
        adapterSummary.averageRenderMs = adapterRenderSamples == 0
            ? 0.0
            : static_cast<double>(adapterRenderUs) / static_cast<double>(adapterRenderSamples) / 1000.0;
        diagnostics_->submitSummary(adapterSummary); // wjy: 适配器汇总提供活动窗口数、总队列、利用率和缓存命中，定位多窗口争用。
    }
    workerBusyUsSinceSummary_ = 0;
    lastSummaryMs_ = nowMs;
}

void RemoteVideoRenderWorker::retireFrame(
    Session& session,
    NativeVideoFrame frame,
    RemoteVideoFrameReleaseReason reason)
{
    const std::uint64_t frameId = frame.frameId;
    const auto lease = frame.lease;
    const auto result = session.surface
        ? session.surface->discard(frame)
        : RemoteVideoRenderResult::Failed;
    if (result == RemoteVideoRenderResult::RetrySync) {
        RetiringFrame retiring;
        retiring.frame = std::move(frame); // wjy: Surface仅借用帧对象；同步未就绪时完整保留句柄、nativeResource与lease。
        retiring.reason = reason;
        retiring.attempts = 1;
        retiring.nextRetryMs = monotonicMs() + 2;
        session.retiringFrames.push_back(std::move(retiring)); // wjy: 解码器每代三槽天然限制该队列，RenderWorker按退避逐帧归还。
        logEvent(RemoteVideoLogLevel::Debug, "frame_release", "queued", "sync_busy", session.state.windowId,
                 session.state.sessionId, session.state.generation, frameId);
        return;
    }
    if (lease) {
        lease->release(reason);
    }
}

void RemoteVideoRenderWorker::releaseSession(Session& session, RemoteVideoFrameReleaseReason reason)
{
    if (session.inFlight) {
        retireFrame(session, std::move(*session.inFlight), reason); // wjy: 关闭/换代首先归还正在重试的纹理，再销毁Surface。
        session.inFlight.reset();
    }
    if (auto frame = session.inbox.cancelPending()) {
        retireFrame(session, std::move(*frame), reason);
    }

    for (int attempt = 0; attempt < 8 && !session.retiringFrames.empty(); ++attempt) {
        processRetiringFrames(monotonicMs()); // wjy: 生命周期边界最多额外等待约16ms级别退避，尽力完成所有key归还后再销毁Surface。
        if (!session.retiringFrames.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (!session.retiringFrames.empty()) {
        logEvent(RemoteVideoLogLevel::Error, "frame_release", "failed", "retry_exhausted", session.state.windowId,
                 session.state.sessionId, session.state.generation); // wjy: 设备级异常保留明确诊断；正常交接路径不会走到此处。
    }
}

void RemoteVideoRenderWorker::logEvent(
    RemoteVideoLogLevel level,
    const char* event,
    const char* result,
    const char* reason,
    std::uint64_t windowId,
    std::uint64_t sessionId,
    std::uint64_t generation,
    std::uint64_t frameId,
    std::uint64_t renderJobId)
{
    if (!diagnostics_) {
        return;
    }
    RemoteVideoLogEvent log;
    log.monotonicUs = static_cast<std::uint64_t>(monotonicMs()) * 1000u;
    log.level = level;
    log.durable = level >= RemoteVideoLogLevel::Info; // wjy: 帧级Trace/Debug只进入内存现场，正常多窗口运行不产生逐帧磁盘IO。
    log.context.windowId = windowId;
    log.context.sessionId = sessionId;
    log.context.viewerGeneration = generation;
    log.context.frameId = frameId;
    log.context.renderJobId = renderJobId;
    log.context.adapterIndex = adapterIndex_;
    log.context.threadId = currentThreadId();
    log.event = event ? event : "";
    log.result = result ? result : "";
    log.reason = reason ? reason : "";
    diagnostics_->submit(std::move(log));
}

void RemoteVideoRenderWorker::logStage(
    RemoteVideoStage stage,
    RemoteVideoStageResult result,
    RemoteVideoStageReason reason,
    const RemoteVideoLogContext& context,
    std::uint64_t durationUs,
    std::uint64_t queueDepth,
    std::uint64_t frameAgeUs,
    RemoteVideoLogLevel level)
{
    if (!diagnostics_) {
        return;
    }
    RemoteVideoStageSample sample;
    sample.stage = stage;
    sample.level = level;
    sample.context = context;
    sample.context.adapterIndex = adapterIndex_;
    sample.context.threadId = currentThreadId();
    sample.result = result;
    sample.reason = reason;
    sample.durationUs = durationUs;
    sample.queueDepth = queueDepth;
    sample.frameAgeUs = frameAgeUs;
    diagnostics_->submitStage(sample); // wjy: 所有阶段事件统一走异步Logger队列，调用线程不打开或刷新文件。
}
// ===end====

} // namespace stream
