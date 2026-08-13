#include "stream/RemoteVideoNativeAdapter.h"

#include <cassert>
#include <memory>
#include <vector>

namespace {

struct Sink final : stream::RemoteVideoNativeFrameSink {
    stream::RemoteVideoNativeSubmissionResult result = stream::RemoteVideoNativeSubmissionResult::Accepted;
    int submitted = 0;
    std::vector<stream::NativeVideoFrame> retained;
    stream::RemoteVideoNativeSubmissionResult submit(stream::NativeVideoFrame frame) override
    {
        ++submitted;
        assert(frame.isValid());
        if (result == stream::RemoteVideoNativeSubmissionResult::Accepted) {
            retained.push_back(std::move(frame));
        }
        return result;
    }
};

} // namespace

int main()
{
    int releaseCount = 0;
    std::vector<stream::RemoteVideoFrameReleaseReason> releaseReasons;
    auto sink = std::make_shared<Sink>();
    stream::RemoteVideoNativeAdapter adapter(
        7,
        3,
        sink,
        [&releaseCount, &releaseReasons](stream::RemoteVideoFrameReleaseReason reason) {
            ++releaseCount;
            releaseReasons.push_back(reason); // wjy: 探针核对普通拒绝、旧代际和最终呈现使用不同释放原因。
        });

    assert(adapter.submitNativeTexture(1280, 720, reinterpret_cast<void*>(1), 1, 10, 20, 30)
        == stream::RemoteVideoNativeSubmissionResult::Accepted);
    assert(adapter.counters().acceptedFrames == 1);

    sink->result = stream::RemoteVideoNativeSubmissionResult::Dropped;
    assert(adapter.submitNativeTexture(1280, 720, reinterpret_cast<void*>(2), 2, 11, 21, 31)
        == stream::RemoteVideoNativeSubmissionResult::Dropped);
    assert(adapter.counters().droppedFrames == 1);
    assert(releaseCount == 1);
    assert(releaseReasons.back() == stream::RemoteVideoFrameReleaseReason::ExplicitDrop);

    adapter.recordSoftwareFallback();
    assert(adapter.counters().softwareFallbackFrames == 1);

    // =====wjy====
    auto retirementSink = std::make_shared<Sink>();
    std::vector<stream::RemoteVideoFrameReleaseReason> retirementReasons;
    stream::RemoteVideoNativeAdapter retirementAdapter(
        9,
        8,
        retirementSink,
        [&retirementReasons](stream::RemoteVideoFrameReleaseReason reason) {
            retirementReasons.push_back(reason); // wjy: 独立记录代际探针，避免前一组普通drop影响断言顺序。
        });

    auto oldOutputTexture = std::make_shared<int>(42);
    std::weak_ptr<int> retiredTextureProbe = oldOutputTexture;
    assert(retirementAdapter.submitNativeTextureForGeneration(
        8, 1920, 1080, reinterpret_cast<void*>(3), 10, 100, 200, 300, 87, oldOutputTexture)
        == stream::RemoteVideoNativeSubmissionResult::Accepted);
    oldOutputTexture.reset();
    assert(!retiredTextureProbe.expired()); // wjy: 输出组切换前，已提交帧的nativeResource必须继续持有旧纹理。

    retirementAdapter.invalidateGeneration(9);
    assert(retirementAdapter.submitNativeTextureForGeneration(
        8, 1920, 1080, reinterpret_cast<void*>(4), 11, 101, 201, 301)
        == stream::RemoteVideoNativeSubmissionResult::Dropped);
    assert(retirementAdapter.counters().staleGenerationFrames == 1);
    assert(retirementReasons.back() == stream::RemoteVideoFrameReleaseReason::StaleGeneration); // wjy: 迟到旧回调明确标记stale且没有进入Sink。
    assert(retirementSink->submitted == 1);

    assert(retirementSink->retained.front().release(
        stream::RemoteVideoFrameReleaseReason::RenderCompleted));
    retirementSink->retained.clear();
    assert(retiredTextureProbe.expired()); // wjy: 最后一份帧引用与lease完成后旧输出纹理才允许真正退役。
    assert(retirementReasons.back() == stream::RemoteVideoFrameReleaseReason::RenderCompleted);
    // ===end====
    return 0;
}
