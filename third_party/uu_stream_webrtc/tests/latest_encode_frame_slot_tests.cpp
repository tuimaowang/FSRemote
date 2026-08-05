#include "latest_encode_frame_slot.h"

#include <cassert>

int main()
{
    uu::LatestEncodeFrameSlot<int> slot;
    assert(slot.empty());
    assert(!slot.push(10, true)); // wjy: 第一帧进入空槽不算覆盖，并携带关键帧请求。
    assert(slot.push(20, false)); // wjy: 第二帧覆盖旧画面，但必须继承旧帧尚未处理的关键帧请求。

    auto latest = slot.take();
    assert(latest.has_value());
    assert(latest->frame == 20); // wjy: 工作线程只取得最新帧，旧帧10不会进入编码器增加控制延迟。
    assert(latest->forceKeyframe);
    assert(slot.empty());

    assert(!slot.push(30, false));
    slot.clear();
    assert(slot.empty());
    return 0;
}
