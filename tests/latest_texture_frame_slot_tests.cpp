#include "ui/LatestTextureFrameSlot.h"

#include <cassert>
#include <cstdint>

int main()
{
    ui::LatestTextureFrameSlot slot;

    // =====wjy====
    const auto firstResult = slot.push({1920, 1080, reinterpret_cast<void*>(std::uintptr_t{1}), 1, 100, 200, 300, 12.0, 7});
    assert(firstResult.disposition == ui::TextureFramePushDisposition::AcceptedAndScheduleDrain); // wjy: 第一帧取得唯一Qt drain投递权。
    assert(slot.pendingCount() == 1);
    assert(slot.drainScheduled());

    for (std::uint64_t frameId = 2; frameId <= 1000; ++frameId) {
        const auto droppedResult = slot.push({1920, 1080, reinterpret_cast<void*>(std::uintptr_t{frameId}), frameId, 100, 200, 300, 12.0, 7});
        assert(droppedResult.disposition == ui::TextureFramePushDisposition::DroppedBecausePending); // wjy: pending仍占用时拒绝新纹理，由生产端立即回收自己的key，不跨线程释放旧帧。
        assert(slot.pendingCount() == 1); // wjy: 无论生产者多快，待呈现描述符始终只有一个。
    }
    assert(slot.replacedFrameCount() == 999);

    const auto latest = slot.takeLatest();
    assert(latest.has_value());
    assert(latest->frameId == 1); // wjy: 已经获得消费者所有权的首帧必须保持到Qt取走，后续新帧不能覆盖它并触发跨线程释放。
    assert(latest->rtpTimestamp == 100 && latest->renderTimeMs == 200 && latest->decodedAtUs == 300); // wjy: 单槽交换不得丢失解码器提供的三组真实时间戳。
    assert(latest->viewerGeneration == 7);
    assert(slot.pendingCount() == 0);

    const auto acceptedWhileDraining = slot.push({1280, 720, reinterpret_cast<void*>(std::uintptr_t{1001}), 1001, 101, 201, 301, 8.0, 7});
    assert(acceptedWhileDraining.disposition == ui::TextureFramePushDisposition::AcceptedWhileDraining); // wjy: 当前帧呈现期间允许单槽保存下一帧，但不增加Qt任务。
    assert(slot.completeDrainAndShouldReschedule());
    const auto next = slot.takeLatest();
    assert(next.has_value());
    assert(next->frameId == 1001);
    assert(!slot.completeDrainAndShouldReschedule());
    assert(!slot.drainScheduled());

    const auto newCycle = slot.push({960, 540, reinterpret_cast<void*>(std::uintptr_t{1002}), 1002, 102, 202, 302, 5.0, 8});
    assert(newCycle.disposition == ui::TextureFramePushDisposition::AcceptedAndScheduleDrain);
    const auto cancelled = slot.cancelPending();
    assert(cancelled.has_value()); // wjy: 关闭或回退方必须取得被取消描述符，以便完成共享纹理ReleaseSync。
    assert(cancelled->frameId == 1002);
    assert(!slot.takeLatest().has_value());
    assert(!slot.completeDrainAndShouldReschedule());

    const auto failedPostCycle = slot.push({960, 540, reinterpret_cast<void*>(std::uintptr_t{1003}), 1003, 103, 203, 303, 5.0, 8});
    assert(failedPostCycle.disposition == ui::TextureFramePushDisposition::AcceptedAndScheduleDrain);
    const auto failedPostFrame = slot.cancelScheduledDrain();
    assert(failedPostFrame.has_value()); // wjy: Qt投递失败时调用方能取回尚未交给Presenter的描述符并按回调三态归还所有权。
    assert(failedPostFrame->frameId == 1003);
    assert(!slot.drainScheduled()); // wjy: 投递失败必须释放唯一调度权，下一张帧能够开启全新drain周期。
    assert(slot.pendingCount() == 0);

    const auto afterFailedPost = slot.push({960, 540, reinterpret_cast<void*>(std::uintptr_t{1004}), 1004, 104, 204, 304, 5.0, 8});
    assert(afterFailedPost.disposition == ui::TextureFramePushDisposition::AcceptedAndScheduleDrain);
    assert(slot.cancelPending().has_value());
    assert(!slot.completeDrainAndShouldReschedule());
    // ===end====
    return 0;
}
