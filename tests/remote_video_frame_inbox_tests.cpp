#include "stream/RemoteVideoFrame.h"

#include <cassert>
#include <cstdint>
#include <memory>

namespace {

struct ReleaseState {
    int count = 0;
    stream::RemoteVideoFrameReleaseReason lastReason = stream::RemoteVideoFrameReleaseReason::Invalid;
};

stream::NativeVideoFrame makeFrame(
    std::uint64_t sessionId,
    std::uint64_t generation,
    std::uint64_t frameId,
    const std::shared_ptr<ReleaseState>& state)
{
    stream::NativeVideoFrame frame;
    frame.sessionId = sessionId;
    frame.viewerGeneration = generation;
    frame.frameId = frameId;
    frame.width = 1280;
    frame.height = 720;
    frame.sharedHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(frameId));
    frame.lease = std::make_shared<stream::RemoteVideoFrameLease>(
        [state](stream::RemoteVideoFrameReleaseReason reason) {
            ++state->count;
            state->lastReason = reason;
        });
    return frame;
}

} // namespace

int main()
{
    stream::FrameInbox inbox;
    const auto firstState = std::make_shared<ReleaseState>();
    const auto secondState = std::make_shared<ReleaseState>();
    const auto thirdState = std::make_shared<ReleaseState>();

    auto first = makeFrame(1, 1, 1, firstState);
    const auto firstResult = inbox.publish(std::move(first));
    assert(firstResult.accepted);
    assert(firstResult.shouldWakeWorker);
    assert(!firstResult.replaced.has_value());

    auto second = makeFrame(1, 1, 2, secondState);
    auto secondResult = inbox.publish(std::move(second));
    assert(secondResult.accepted);
    assert(!secondResult.shouldWakeWorker);
    assert(secondResult.replaced.has_value());
    assert(secondResult.replaced->frameId == 1);
    assert(secondResult.replaced->release(stream::RemoteVideoFrameReleaseReason::PendingReplaced));
    assert(firstState->count == 1);
    assert(!secondResult.replaced->release(stream::RemoteVideoFrameReleaseReason::ExplicitDrop));

    const auto latest = inbox.takeLatest();
    assert(latest.has_value());
    assert(latest->frameId == 2);
    assert(inbox.pendingCount() == 0);

    auto third = makeFrame(1, 1, 3, thirdState);
    assert(inbox.publish(std::move(third)).accepted);
    const auto cancelled = inbox.cancelPending();
    assert(cancelled.has_value());
    assert(cancelled->frameId == 3);
    assert(cancelled->release(stream::RemoteVideoFrameReleaseReason::Shutdown));
    assert(thirdState->count == 1);

    assert(latest->release(stream::RemoteVideoFrameReleaseReason::RenderCompleted));
    assert(secondState->count == 1);
    assert(inbox.acceptedFrames() == 3);
    assert(inbox.replacedFrames() == 1);
    return 0;
}
