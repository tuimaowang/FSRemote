#pragma once

// =====wjy====
#ifndef NOMINMAX
#define NOMINMAX //  VK 映射头可被业务代码单独包含，必须在 Windows.h 前关闭全局 min/max 宏污染。
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN //  独立交付头只需要基础虚拟键定义，不额外扩大 Windows 头文件依赖面。
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace uu {

struct FakerInputKeyboardReport {
    std::uint8_t modifiers = 0;
    std::array<std::uint8_t, 6> usages{};
}; //  FakerInput 键盘使用标准 boot-keyboard 快照，一字节修饰键加最多六个普通 USB HID usage。

enum class FakerInputKeyboardUpdate {
    Changed,
    Unchanged,
    Unsupported,
    Rollover,
}; //  调用方必须区分状态变化、按键重复、未映射按键和六键上限，禁止把失败误当成有效 HID 报告。

constexpr std::uint8_t fakerInputModifierBit(int virtualKey) noexcept
{
    switch (virtualKey) {
    case VK_CONTROL:
    case VK_LCONTROL:
        return 1u << 0;
    case VK_SHIFT:
    case VK_LSHIFT:
        return 1u << 1;
    case VK_MENU:
    case VK_LMENU:
        return 1u << 2;
    case VK_LWIN:
        return 1u << 3;
    case VK_RCONTROL:
        return 1u << 4;
    case VK_RSHIFT:
        return 1u << 5;
    case VK_RMENU:
        return 1u << 6;
    case VK_RWIN:
        return 1u << 7;
    default:
        return 0;
    }
} //  通用 Ctrl/Shift/Alt 按现有 Viewer 语义落到左侧位，低级钩子能提供左右 VK 时仍保留真实方向。

constexpr std::uint8_t fakerInputUsageForVirtualKey(int virtualKey) noexcept
{
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        return static_cast<std::uint8_t>(0x04 + virtualKey - 'A'); //  HID 键盘字母 usage 从 A=0x04 连续排列到 Z=0x1D。
    }
    if (virtualKey >= '1' && virtualKey <= '9') {
        return static_cast<std::uint8_t>(0x1E + virtualKey - '1'); //  主键盘数字 1..9 对应 0x1E..0x26。
    }
    if (virtualKey >= VK_F1 && virtualKey <= VK_F12) {
        return static_cast<std::uint8_t>(0x3A + virtualKey - VK_F1);
    }
    if (virtualKey >= VK_F13 && virtualKey <= VK_F24) {
        return static_cast<std::uint8_t>(0x68 + virtualKey - VK_F13);
    }
    if (virtualKey >= VK_NUMPAD1 && virtualKey <= VK_NUMPAD9) {
        return static_cast<std::uint8_t>(0x59 + virtualKey - VK_NUMPAD1);
    }

    switch (virtualKey) {
    case '0': return 0x27;
    case VK_RETURN: return 0x28;
    case VK_ESCAPE: return 0x29;
    case VK_BACK: return 0x2A;
    case VK_TAB: return 0x2B;
    case VK_SPACE: return 0x2C;
    case VK_OEM_MINUS: return 0x2D;
    case VK_OEM_PLUS: return 0x2E;
    case VK_OEM_4: return 0x2F;
    case VK_OEM_6: return 0x30;
    case VK_OEM_5: return 0x31;
    case VK_OEM_1: return 0x33;
    case VK_OEM_7: return 0x34;
    case VK_OEM_3: return 0x35;
    case VK_OEM_COMMA: return 0x36;
    case VK_OEM_PERIOD: return 0x37;
    case VK_OEM_2: return 0x38;
    case VK_CAPITAL: return 0x39;
    case VK_SNAPSHOT: return 0x46;
    case VK_SCROLL: return 0x47;
    case VK_PAUSE: return 0x48;
    case VK_INSERT: return 0x49;
    case VK_HOME: return 0x4A;
    case VK_PRIOR: return 0x4B;
    case VK_DELETE: return 0x4C;
    case VK_END: return 0x4D;
    case VK_NEXT: return 0x4E;
    case VK_RIGHT: return 0x4F;
    case VK_LEFT: return 0x50;
    case VK_DOWN: return 0x51;
    case VK_UP: return 0x52;
    case VK_NUMLOCK: return 0x53;
    case VK_DIVIDE: return 0x54;
    case VK_MULTIPLY: return 0x55;
    case VK_SUBTRACT: return 0x56;
    case VK_ADD: return 0x57;
    case VK_NUMPAD0: return 0x62;
    case VK_DECIMAL: return 0x63;
    case VK_OEM_102: return 0x64;
    case VK_APPS: return 0x65;
    default: return 0; //  浏览器、音量等 Consumer Control 键不属于当前键盘 collection，继续交给系统 SendInput 兼容处理。
    }
}

class FakerInputKeyboardState final {
public:
    FakerInputKeyboardUpdate update(int virtualKey, bool down)
    {
        const std::uint8_t modifier = fakerInputModifierBit(virtualKey);
        const std::uint8_t usage = fakerInputUsageForVirtualKey(virtualKey);
        if (modifier == 0 && usage == 0) {
            return FakerInputKeyboardUpdate::Unsupported; //  无法编码的 VK 不进入驱动状态，调用方可对该键单独保留 SendInput。
        }

        const auto existing = std::find(pressed_virtual_keys_.begin(), pressed_virtual_keys_.end(), virtualKey);
        if (down) {
            if (existing != pressed_virtual_keys_.end()) return FakerInputKeyboardUpdate::Unchanged; //  相同 HID 快照不重复写管道，按住重复由目标 Windows 键盘类驱动产生。
            if (usage != 0 && ordinaryKeyCount() >= 6) {
                return FakerInputKeyboardUpdate::Rollover; //  boot-keyboard 只能表达六个普通键，第七键不污染已按住的六键快照。
            }
            pressed_virtual_keys_.push_back(virtualKey); //  保留按下顺序使每次快照稳定，抬起一个键不会无意义重排其余 usage。
            return FakerInputKeyboardUpdate::Changed;
        }

        if (existing == pressed_virtual_keys_.end()) return FakerInputKeyboardUpdate::Unchanged;
        pressed_virtual_keys_.erase(existing); //  只有真实存在于驱动快照中的键才会产生新的抬键报告。
        return FakerInputKeyboardUpdate::Changed;
    }

    FakerInputKeyboardReport report() const noexcept
    {
        FakerInputKeyboardReport result;
        std::size_t usageIndex = 0;
        for (const int virtualKey : pressed_virtual_keys_) {
            const std::uint8_t modifier = fakerInputModifierBit(virtualKey);
            if (modifier != 0) {
                result.modifiers |= modifier; //  左右修饰键合并到标准八位掩码，不占六个普通键槽位。
                continue;
            }
            const std::uint8_t usage = fakerInputUsageForVirtualKey(virtualKey);
            if (usage != 0 && usageIndex < result.usages.size()) {
                result.usages[usageIndex++] = usage;
            }
        }
        return result;
    }

    const std::vector<int>& pressedVirtualKeys() const noexcept
    {
        return pressed_virtual_keys_; //  Bridge 断管时按现有逻辑状态重建系统 key-down，避免仍按住的移动键突然丢失。
    }

    void clear() noexcept
    {
        pressed_virtual_keys_.clear(); //  后端完成 release-all 后清空本地镜像，下一次按下从干净状态重新建立。
    }

private:
    std::size_t ordinaryKeyCount() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            pressed_virtual_keys_.begin(),
            pressed_virtual_keys_.end(),
            [](int virtualKey) { return fakerInputUsageForVirtualKey(virtualKey) != 0; }));
    }

    std::vector<int> pressed_virtual_keys_;
};

} // namespace uu
// ===end====
