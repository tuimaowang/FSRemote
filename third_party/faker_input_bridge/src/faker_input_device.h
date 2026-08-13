#pragma once

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
};

}  // namespace faker_bridge

