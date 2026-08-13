#include "stream/RemoteVideoScheduler.h"

#include <cassert>

int main()
{
    stream::RemoteVideoScheduler scheduler;
    scheduler.upsert({1, stream::RemoteVideoWindowRole::Focused, 60, 100, 1, false}, 1000);
    scheduler.upsert({2, stream::RemoteVideoWindowRole::VisibleBackground, 30, 40, 1, false}, 1000);
    scheduler.upsert({3, stream::RemoteVideoWindowRole::VisibleBackground, 30, 40, 1, false}, 1000);

    scheduler.markFrameReady(2, 1);
    scheduler.markFrameReady(3, 1);
    auto first = scheduler.next(1000);
    assert(first.has_value());
    assert(first->windowId == 2 || first->windowId == 3);
    scheduler.markPresented(first->windowId, 1000);

    auto second = scheduler.next(1000);
    assert(second.has_value());
    assert(second->windowId != first->windowId);
    scheduler.markPresented(second->windowId, 1000);

    scheduler.markFrameReady(1, 1);
    auto focused = scheduler.next(1000);
    assert(focused.has_value());
    assert(focused->windowId == 1);
    scheduler.markPresented(1, 1000);
    assert(scheduler.size() == 3);

    // =====wjy====
    scheduler.markFrameReady(1, 1);
    assert(!scheduler.next(1005).has_value()); // wjy: 60 FPS窗口在约16ms deadline之前不能被提前重复呈现。
    assert(scheduler.next(1016).has_value()); // wjy: deadline到达后同一窗口恢复可调度。
    scheduler.markRetry(1, 1016, 2);
    assert(!scheduler.next(1017).has_value()); // wjy: 同步交接重试保留帧，但必须执行最小退避避免工作线程自旋。
    const auto retryDue = scheduler.next(1018);
    assert(retryDue.has_value());
    scheduler.markPresented(retryDue->windowId, 1018);

    scheduler.markPresented(2, 2000);
    scheduler.markFrameReady(2, 1);
    assert(!scheduler.next(2032).has_value()); // wjy: 后台30 FPS窗口必须等待完整约33ms周期。
    const auto backgroundDue = scheduler.next(2033);
    assert(backgroundDue.has_value());
    assert(backgroundDue->windowId == 2);
    // ===end====
    return 0;
}
