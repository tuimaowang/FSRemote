#pragma once

#include "stream/RemoteVideoFrame.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace stream {

// =====wjy====
enum class RemoteVideoNativeSubmissionResult : std::uint8_t {
    Accepted,
    Dropped,
    SoftwareFallback,
};

struct RemoteVideoNativeCounters {
    std::uint64_t nativeFrames = 0;
    std::uint64_t acceptedFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t softwareFallbackFrames = 0;
    std::uint64_t staleGenerationFrames = 0;
};

class RemoteVideoNativeFrameSink {
public:
    virtual ~RemoteVideoNativeFrameSink() = default;
    virtual RemoteVideoNativeSubmissionResult submit(NativeVideoFrame frame) = 0;
};

class RemoteVideoNativeAdapter final {
public:
    using ReleaseCallback = RemoteVideoFrameLease::ReleaseCallback;

    RemoteVideoNativeAdapter(
        std::uint64_t sessionId,
        std::uint64_t generation,
        std::shared_ptr<RemoteVideoNativeFrameSink> sink,
        ReleaseCallback releaseCallback = {});

    RemoteVideoNativeSubmissionResult submitNativeTexture(
        int width,
        int height,
        void* sharedHandle,
        std::uint64_t frameId,
        std::int64_t rtpTimestamp,
        std::int64_t renderTimeMs,
        std::int64_t decodedAtUs,
        std::uint32_t format = 0,
        std::shared_ptr<void> nativeResource = {});

    RemoteVideoNativeSubmissionResult submitNativeTextureForGeneration(
        std::uint64_t callbackGeneration,
        int width,
        int height,
        void* sharedHandle,
        std::uint64_t frameId,
        std::int64_t rtpTimestamp,
        std::int64_t renderTimeMs,
        std::int64_t decodedAtUs,
        std::uint32_t format = 0,
        std::shared_ptr<void> nativeResource = {}); // wjy: 诊断探针显式传入回调代际，验证重连后旧回调不会进入新Sink。

    void recordSoftwareFallback() noexcept;
    void invalidateGeneration(std::uint64_t generation) noexcept;
    RemoteVideoNativeCounters counters() const noexcept;
    std::uint64_t generation() const noexcept;

private:
    const std::uint64_t sessionId_;
    std::atomic<std::uint64_t> generation_;
    const std::shared_ptr<RemoteVideoNativeFrameSink> sink_;
    const ReleaseCallback releaseCallback_;
    std::atomic<std::uint64_t> nativeFrames_ = 0;
    std::atomic<std::uint64_t> acceptedFrames_ = 0;
    std::atomic<std::uint64_t> droppedFrames_ = 0;
    std::atomic<std::uint64_t> softwareFallbackFrames_ = 0;
    std::atomic<std::uint64_t> staleGenerationFrames_ = 0;
};
// ===end====

} // namespace stream
