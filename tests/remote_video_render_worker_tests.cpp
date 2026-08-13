#include "stream/RemoteVideoRenderWorker.h"

#include <cassert>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct Surface final : stream::RemoteVideoRenderSurface {
    std::mutex mutex;
    std::vector<std::uint64_t> frames;
    std::vector<std::uint64_t> discardedFrames;
    int renderRetriesRemaining = 0;
    int discardRetriesRemaining = 0;
    int renderAttempts = 0;
    int discardAttempts = 0;
    void applyState(const stream::RemoteVideoSurfaceState&) override {}
    stream::RemoteVideoRenderResult render(stream::NativeVideoFrame& frame) override
    {
        std::lock_guard lock(mutex);
        ++renderAttempts;
        if (renderRetriesRemaining > 0) {
            --renderRetriesRemaining;
            return stream::RemoteVideoRenderResult::RetrySync; // wjy: 模拟解码器尚未完成key 1交接，worker必须保留同一帧重试。
        }
        frames.push_back(frame.frameId);
        return stream::RemoteVideoRenderResult::Presented;
    }
    stream::RemoteVideoRenderResult discard(stream::NativeVideoFrame& frame) override
    {
        std::lock_guard lock(mutex);
        ++discardAttempts;
        if (discardRetriesRemaining > 0) {
            --discardRetriesRemaining;
            return stream::RemoteVideoRenderResult::RetrySync; // wjy: 模拟被替换帧暂时无法归还producer key。
        }
        discardedFrames.push_back(frame.frameId);
        return stream::RemoteVideoRenderResult::DroppedStale;
    }
    void clear() override {}
};

stream::NativeVideoFrame makeFrame(
    std::uint64_t id,
    int* releases,
    stream::RemoteVideoFrameReleaseReason* lastReason = nullptr)
{
    stream::NativeVideoFrame frame;
    frame.sessionId = 1;
    frame.viewerGeneration = 1;
    frame.frameId = id;
    frame.width = 1280;
    frame.height = 720;
    frame.sharedHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
    frame.lease = std::make_shared<stream::RemoteVideoFrameLease>(
        [releases, lastReason](stream::RemoteVideoFrameReleaseReason reason) {
            ++*releases;
            if (lastReason) *lastReason = reason; // wjy: 拒绝测试同时锁定释放次数和强类型原因。
        });
    return frame;
}

} // namespace

int main()
{
    // =====wjy====
    {
        auto surface = std::make_shared<Surface>();
        int releases = 0;
        stream::RemoteVideoRenderWorker worker;
        assert(worker.start());
        stream::RemoteVideoSurfaceState state;
        state.windowId = 1;
        state.sessionId = 1;
        state.generation = 1;
        state.visible = true;
        state.profile = {1280, 720, 30, 40, true};
        assert(worker.registerSurface(1, state, surface));
        assert(worker.submitFrame(1, makeFrame(1, &releases)));
        assert(worker.submitFrame(1, makeFrame(2, &releases)));
        assert(worker.waitForIdle());
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // wjy: 命令队列空闲不代表30 FPS呈现deadline已到，等待下一周期验证第二帧最终可见。
        worker.stop();

        std::lock_guard lock(surface->mutex);
        assert(!surface->frames.empty());
        assert(surface->frames.back() == 2);
        assert(releases == 2); // wjy: latest覆盖与最终呈现各释放一次，lease没有重复回调。
    }

    {
        auto surface = std::make_shared<Surface>();
        surface->renderRetriesRemaining = 1;
        int releases = 0;
        stream::RemoteVideoRenderWorker worker;
        assert(worker.start());
        stream::RemoteVideoSurfaceState state;
        state.windowId = 2;
        state.sessionId = 2;
        state.generation = 1;
        state.visible = true;
        state.focused = true;
        state.profile = {1920, 1080, 60, 100, true};
        assert(worker.registerSurface(2, state, surface));
        assert(worker.submitFrame(2, makeFrame(10, &releases)));
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // wjy: 覆盖2ms重试退避和测试机线程调度抖动。
        worker.stop();

        std::lock_guard lock(surface->mutex);
        assert(surface->renderAttempts >= 2);
        assert(surface->frames.size() == 1 && surface->frames.front() == 10);
        assert(releases == 1); // wjy: RetrySync阶段不得提前释放lease，成功呈现后才恰好释放一次。
    }

    {
        auto surface = std::make_shared<Surface>();
        surface->discardRetriesRemaining = 1;
        int releases = 0;
        stream::RemoteVideoRenderWorker worker;
        assert(worker.start());
        stream::RemoteVideoSurfaceState state;
        state.windowId = 3;
        state.sessionId = 3;
        state.generation = 1;
        state.visible = false; // wjy: 隐藏窗口不进入呈现调度，第二帧会直接替换latest槽中的第一帧。
        state.profile = {1280, 720, 30, 40, true};
        assert(worker.registerSurface(3, state, surface));
        assert(worker.submitFrame(3, makeFrame(20, &releases)));
        assert(worker.submitFrame(3, makeFrame(21, &releases)));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        worker.stop();

        std::lock_guard lock(surface->mutex);
        assert(surface->discardAttempts >= 3);
        assert(releases == 2); // wjy: 被替换帧重试成功和关闭时pending帧均完成归还，未因同步忙泄漏lease。
    }

    {
        auto surface = std::make_shared<Surface>();
        auto diagnostics = std::make_shared<stream::RemoteVideoDiagnostics>(128, 256);
        int releases = 0;
        stream::RemoteVideoRenderWorker worker(0, diagnostics);
        assert(worker.start());
        stream::RemoteVideoSurfaceState state;
        state.windowId = 4;
        state.sessionId = 4;
        state.generation = 1;
        state.visible = true;
        state.focused = true;
        state.profile = {1920, 1080, 60, 100, true};
        assert(worker.registerSurface(4, state, surface));
        assert(worker.submitFrame(4, makeFrame(30, &releases)));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // wjy: 等待RenderWorker自动产生一次会话与适配器周期汇总。
        worker.stop();

        const auto trace = diagnostics->traceSnapshot();
        bool foundSessionSummary = false;
        bool foundAdapterSummary = false;
        bool foundTypedQueueStage = false;
        for (const auto& event : trace) {
            foundSessionSummary = foundSessionSummary
                || (event.event == "summary" && event.reason == "session" && event.context.windowId == 4);
            foundAdapterSummary = foundAdapterSummary
                || (event.event == "summary" && event.reason == "adapter" && event.context.adapterIndex == 0);
            foundTypedQueueStage = foundTypedQueueStage
                || (event.event == "stage_queue" && event.reason == "latest_slot");
        }
        assert(foundSessionSummary);
        assert(foundAdapterSummary);
        assert(foundTypedQueueStage); // wjy: 自动汇总和强类型阶段事件均由真实worker循环产生，不依赖测试直接调用Diagnostics。
        assert(releases == 1);
    }
    // ===end====
    return 0;
}
