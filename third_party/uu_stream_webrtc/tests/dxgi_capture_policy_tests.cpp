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

    // =====wjy====
    assert(lsp::dxgi_failure_action(DXGI_ERROR_ACCESS_LOST, S_OK)
        == lsp::DxgiFailureAction::RecreateDuplication); // wjy: ACCESS_LOST 只重建 Duplication，禁止把一次显示拓扑抖动扩大成 NVENC 重启。
    assert(lsp::dxgi_failure_action(DXGI_ERROR_INVALID_CALL, S_OK)
        == lsp::DxgiFailureAction::RecreateDuplication); // wjy: INVALID_CALL 必须退出坏掉的 Acquire 状态，不能每秒重复同一个错误。
    assert(lsp::dxgi_failure_action(DXGI_ERROR_WAIT_TIMEOUT, S_OK)
        == lsp::DxgiFailureAction::KeepResources); // wjy: 静止桌面的正常超时不进入任何恢复流程。
    assert(lsp::dxgi_failure_action(DXGI_ERROR_ACCESS_LOST, DXGI_ERROR_DEVICE_REMOVED)
        == lsp::DxgiFailureAction::RecreateDevice); // wjy: 设备移除原因优先于表层 ACCESS_LOST，防止失效 Device 被轻量恢复永久保留。
    assert(lsp::dxgi_failure_action(E_FAIL, DXGI_ERROR_DEVICE_REMOVED)
        == lsp::DxgiFailureAction::RecreateDevice); // wjy: GetDeviceRemovedReason 确认设备丢失时才执行完整设备恢复。
    assert(lsp::dxgi_duplication_retry_delay_ms(1) == 0);
    assert(lsp::dxgi_duplication_retry_delay_ms(2) == 50);
    assert(lsp::dxgi_duplication_retry_delay_ms(3) == 100);
    assert(lsp::dxgi_duplication_retry_delay_ms(20) == 250); // wjy: 连续失败退避有固定上限，不会增长到数秒而让远控长期停在旧画面。
    // ===end====
    return 0;
}
