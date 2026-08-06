#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <string_view>

#include <dxgi.h>

namespace lsp {

// =====wjy====
inline bool dxgi_device_name_matches(std::wstring_view requested, std::wstring_view actual)
{
    return !requested.empty() && requested.size() == actual.size()
        && std::equal(requested.begin(), requested.end(), actual.begin(), [](wchar_t left, wchar_t right) {
            return std::towlower(left) == std::towlower(right);
        }); // wjy: Parsec的\\.\DISPLAYx名称按不区分大小写精确匹配，禁止子串或空值误选物理显示器。
}

enum class DxgiFailureAction : uint8_t {
    KeepResources,
    RecreateDuplication,
    RecreateDevice,
}; // wjy: 把 HRESULT 到恢复层级的映射抽成无显卡纯策略，生产代码和测试共用同一套判断。

inline bool dxgi_result_is_device_lost(long result)
{
    return result == DXGI_ERROR_DEVICE_REMOVED
        || result == DXGI_ERROR_DEVICE_RESET
        || result == DXGI_ERROR_DEVICE_HUNG
        || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR; // wjy: 只有设备真正被移除、重置、挂起或驱动内部错误时才允许销毁 D3D11 Device 和 NVENC 会话。
}

inline DxgiFailureAction dxgi_failure_action(long result, long deviceRemovalReason)
{
    if (dxgi_result_is_device_lost(result) || dxgi_result_is_device_lost(deviceRemovalReason)) {
        return DxgiFailureAction::RecreateDevice; // wjy: 外层 HRESULT 或设备查询任一确认设备丢失时优先升级完整恢复，不能被 ACCESS_LOST 掩盖。
    }
    if (result == DXGI_ERROR_ACCESS_LOST || result == DXGI_ERROR_INVALID_CALL) {
        return DxgiFailureAction::RecreateDuplication; // wjy: 设备仍健康时，显示拓扑失效和无效 Acquire 调用只重建 Desktop Duplication，保留昂贵设备资源。
    }
    return DxgiFailureAction::KeepResources; // wjy: 超时、槽位忙和其它瞬时错误不能破坏当前可见帧与编码设备。
}

inline uint32_t dxgi_duplication_retry_delay_ms(uint32_t consecutiveFailures)
{
    if (consecutiveFailures <= 1) return 0;
    if (consecutiveFailures == 2) return 50;
    if (consecutiveFailures == 3) return 100;
    return 250; // wjy: 恢复枚举最多每 250ms 一次，避免 60 FPS 循环持续创建 DXGI 对象，同时保持快速恢复。
}
class FrameSlotLeaseState final : public std::enable_shared_from_this<FrameSlotLeaseState> {
public:
    bool try_acquire(std::shared_ptr<void>* token)
    {
        if (!token) return false;
        bool expected = false;
        if (!leased_.compare_exchange_strong(expected, true)) return false;
        const std::shared_ptr<FrameSlotLeaseState> self = shared_from_this();
        *token = std::shared_ptr<void>(this, [self](void*) { self->leased_ = false; }); // wjy: 所有token副本释放后才归还槽位，覆盖时机与WebRTC帧引用完全一致。
        return true;
    }

    bool leased() const { return leased_.load(); }

private:
    std::atomic_bool leased_ = false;
};
// ===end====

} // namespace lsp
