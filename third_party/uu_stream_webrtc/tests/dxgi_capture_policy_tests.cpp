#include "dxgi_capture_policy.h"

#include <cassert>
#include <memory>

int main()
{
    assert(lsp::dxgi_device_name_matches(L"\\\\.\\DISPLAY7", L"\\\\.\\display7")); // wjy: Windows显示设备名匹配不区分大小写。
    assert(!lsp::dxgi_device_name_matches(L"\\\\.\\DISPLAY7", L"\\\\.\\DISPLAY1"));
    assert(!lsp::dxgi_device_name_matches(L"", L"\\\\.\\DISPLAY1")); // wjy: 空目标不能退化为任意屏幕匹配。

    auto state = std::make_shared<lsp::FrameSlotLeaseState>();
    std::shared_ptr<void> first;
    assert(state->try_acquire(&first));
    assert(state->leased());
    std::shared_ptr<void> denied;
    assert(!state->try_acquire(&denied)); // wjy: 第一帧仍持有槽位时，采集器必须跳过该纹理而不是覆盖。

    std::shared_ptr<void> retained = first;
    first.reset();
    assert(state->leased()); // wjy: 多PeerConnection共享帧时任意一个引用仍存在，槽位都不能归还。
    retained.reset();
    assert(!state->leased());
    assert(state->try_acquire(&denied)); // wjy: 最后一份引用释放后，同一槽位才允许下一帧复用。
    return 0;
}
