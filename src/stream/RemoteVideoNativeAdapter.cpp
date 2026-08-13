#include "stream/RemoteVideoNativeAdapter.h"

namespace stream {

// =====wjy====
RemoteVideoNativeAdapter::RemoteVideoNativeAdapter(
    std::uint64_t sessionId,
    std::uint64_t generation,
    std::shared_ptr<RemoteVideoNativeFrameSink> sink,
    ReleaseCallback releaseCallback)
    : sessionId_(sessionId),
      generation_(generation),
      sink_(std::move(sink)),
      releaseCallback_(std::move(releaseCallback))
{
}

RemoteVideoNativeSubmissionResult RemoteVideoNativeAdapter::submitNativeTexture(
    int width,
    int height,
    void* sharedHandle,
    std::uint64_t frameId,
    std::int64_t rtpTimestamp,
    std::int64_t renderTimeMs,
    std::int64_t decodedAtUs,
    std::uint32_t format,
    std::shared_ptr<void> nativeResource)
{
    return submitNativeTextureForGeneration(
        generation_.load(std::memory_order_acquire),
        width,
        height,
        sharedHandle,
        frameId,
        rtpTimestamp,
        renderTimeMs,
        decodedAtUs,
        format,
        std::move(nativeResource)); // wjy: 生产入口默认使用当前代际，测试/桥接入口可显式模拟迟到旧回调。
}

RemoteVideoNativeSubmissionResult RemoteVideoNativeAdapter::submitNativeTextureForGeneration(
    std::uint64_t callbackGeneration,
    int width,
    int height,
    void* sharedHandle,
    std::uint64_t frameId,
    std::int64_t rtpTimestamp,
    std::int64_t renderTimeMs,
    std::int64_t decodedAtUs,
    std::uint32_t format,
    std::shared_ptr<void> nativeResource)
{
    ++nativeFrames_;
    const std::uint64_t currentGeneration = generation_.load(std::memory_order_acquire);
    const bool staleGeneration = callbackGeneration == 0
        || currentGeneration == 0
        || callbackGeneration != currentGeneration; // wjy: 新Viewer代际建立后，任何旧回调即使尺寸和句柄有效也不得进入Sink。
    if (staleGeneration || !sink_ || width <= 0 || height <= 0 || frameId == 0
        || (!sharedHandle && !nativeResource)) {
        ++droppedFrames_;
        if (staleGeneration) {
            ++staleGenerationFrames_;
        }
        if (releaseCallback_) {
            auto lease = std::make_shared<RemoteVideoFrameLease>(releaseCallback_);
            lease->release(staleGeneration
                ? RemoteVideoFrameReleaseReason::StaleGeneration
                : RemoteVideoFrameReleaseReason::Invalid); // wjy: 探针同时验证旧代际使用独立强类型释放原因，而不是混入普通无效帧。
        }
        return RemoteVideoNativeSubmissionResult::Dropped;
    }

    auto lease = std::make_shared<RemoteVideoFrameLease>(releaseCallback_);
    NativeVideoFrame frame;
    frame.sessionId = sessionId_;
    frame.viewerGeneration = callbackGeneration;
    frame.frameId = frameId;
    frame.rtpTimestamp = rtpTimestamp;
    frame.renderTimeMs = renderTimeMs;
    frame.decodedAtUs = decodedAtUs;
    frame.width = width;
    frame.height = height;
    frame.format = format;
    frame.sharedHandle = sharedHandle;
    frame.nativeResource = std::move(nativeResource);
    frame.lease = lease;

    const auto result = sink_->submit(std::move(frame));
    switch (result) {
    case RemoteVideoNativeSubmissionResult::Accepted:
        ++acceptedFrames_;
        break;
    case RemoteVideoNativeSubmissionResult::Dropped:
        ++droppedFrames_;
        lease->release(RemoteVideoFrameReleaseReason::ExplicitDrop);
        break;
    case RemoteVideoNativeSubmissionResult::SoftwareFallback:
        ++softwareFallbackFrames_;
        lease->release(RemoteVideoFrameReleaseReason::ExplicitDrop);
        break;
    }
    return result;
}

void RemoteVideoNativeAdapter::recordSoftwareFallback() noexcept
{
    ++softwareFallbackFrames_;
}

void RemoteVideoNativeAdapter::invalidateGeneration(std::uint64_t generation) noexcept
{
    generation_.store(generation, std::memory_order_release); // wjy: 代际切换本身不是丢帧；只有后续真实迟到回调才计入staleGenerationFrames。
}

RemoteVideoNativeCounters RemoteVideoNativeAdapter::counters() const noexcept
{
    RemoteVideoNativeCounters result;
    result.nativeFrames = nativeFrames_.load(std::memory_order_relaxed);
    result.acceptedFrames = acceptedFrames_.load(std::memory_order_relaxed);
    result.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
    result.softwareFallbackFrames = softwareFallbackFrames_.load(std::memory_order_relaxed);
    result.staleGenerationFrames = staleGenerationFrames_.load(std::memory_order_relaxed);
    return result;
}

std::uint64_t RemoteVideoNativeAdapter::generation() const noexcept
{
    return generation_.load(std::memory_order_acquire);
}
// ===end====

} // namespace stream
