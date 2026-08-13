#pragma once

// =====wjy====
//  从独立 FakerInputBridge 项目收录底层 HID 访问接口，供安装 MSI 后直接集成到自己的 Windows C++ 程序。
#ifndef NOMINMAX
#define NOMINMAX //  独立源码不再依赖原项目的编译定义，避免 Windows 的 min/max 宏破坏 std::min、std::max 和数值边界代码。
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN //  只引入驱动访问需要的 Win32 声明，减少不同宿主工程头文件之间的宏和类型冲突。
#endif
// ===end====

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace faker_bridge {

struct DeviceError {
    DWORD code = ERROR_SUCCESS;
    std::wstring context;

    [[nodiscard]] std::wstring describe() const;
};

class FakerInputDevice final {
public:
    FakerInputDevice() = default;
    ~FakerInputDevice();

    FakerInputDevice(const FakerInputDevice&) = delete;
    FakerInputDevice& operator=(const FakerInputDevice&) = delete;
    FakerInputDevice(FakerInputDevice&& other) noexcept;
    FakerInputDevice& operator=(FakerInputDevice&& other) noexcept;

    [[nodiscard]] static std::optional<FakerInputDevice> open(
        DeviceError* error = nullptr);

    [[nodiscard]] bool send_keyboard(
        std::uint8_t modifiers,
        std::span<const std::uint8_t> usages,
        DeviceError* error = nullptr) const;
    [[nodiscard]] bool send_relative_mouse(
        std::uint8_t buttons,
        std::int16_t dx,
        std::int16_t dy,
        std::int8_t wheel,
        std::int8_t horizontal_wheel,
        DeviceError* error = nullptr) const;
    [[nodiscard]] bool send_absolute_mouse(
        std::uint8_t buttons,
        std::uint16_t x,
        std::uint16_t y,
        std::int8_t wheel,
        DeviceError* error = nullptr) const;
    [[nodiscard]] bool release_all(DeviceError* error = nullptr) const;

    [[nodiscard]] std::uint16_t driver_version() const noexcept;
    [[nodiscard]] bool valid() const noexcept;

private:
    FakerInputDevice(
        HANDLE handle,
        std::uint16_t output_report_bytes,
        std::uint16_t driver_version);
    [[nodiscard]] bool send_inner_report(
        std::span<const std::uint8_t> report,
        DeviceError* error) const;
    void close() noexcept;

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::uint16_t output_report_bytes_ = 0;
    std::uint16_t driver_version_ = 0;
    // =====wjy====
    mutable std::uint8_t absolute_buttons_ = 0; // wjy: 只记录本实例最后成功送达的绝对鼠标按钮快照，纯相对鼠标释放时无需发送绝对坐标。
    mutable std::uint16_t absolute_x_ = 0; // wjy: 绝对按钮仍按住时在原报告位置抬起，避免清理操作重新映射当前光标。
    mutable std::uint16_t absolute_y_ = 0;
    // ===end====
};

}  // namespace faker_bridge
