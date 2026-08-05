#pragma once

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <memory>
#include <string_view>

namespace lsp {

// =====wjy====
inline bool dxgi_device_name_matches(std::wstring_view requested, std::wstring_view actual)
{
    return !requested.empty() && requested.size() == actual.size()
        && std::equal(requested.begin(), requested.end(), actual.begin(), [](wchar_t left, wchar_t right) {
            return std::towlower(left) == std::towlower(right);
        }); // wjy: Parsec的\\.\DISPLAYx名称按不区分大小写精确匹配，禁止子串或空值误选物理显示器。
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
