#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace faker_bridge::protocol {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\FakerInputBridge.v1";
inline constexpr std::uint32_t kMagic = 0x31424946;  // "FIB1" on the wire.
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint16_t kResponseBit = 0x8000;
inline constexpr std::size_t kMaxMessageBytes = 256;

enum class Command : std::uint16_t {
    ping = 1,
    keyboard = 2,
    relative_mouse = 3,
    absolute_mouse = 4,
    release_all = 5,
    get_status = 6,
};

enum ServerFlags : std::uint32_t {
    server_flag_driver_ready = 1u << 0,
};

#pragma pack(push, 1)

struct MessageHeader {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    std::uint16_t command = 0;
    std::uint32_t payload_bytes = 0;
    std::uint32_t sequence = 0;
};

struct KeyboardPayload {
    std::uint8_t modifiers = 0;
    std::uint8_t reserved = 0;
    std::uint8_t usages[6]{};
};

struct RelativeMousePayload {
    std::uint8_t buttons = 0;
    std::int8_t wheel = 0;
    std::int8_t horizontal_wheel = 0;
    std::uint8_t reserved = 0;
    std::int16_t dx = 0;
    std::int16_t dy = 0;
};

struct AbsoluteMousePayload {
    std::uint8_t buttons = 0;
    std::int8_t wheel = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t reserved = 0;
};

struct ResponsePayload {
    std::uint32_t result = 0;
    std::uint32_t server_flags = 0;
    std::uint32_t driver_version = 0;
    std::uint32_t reserved = 0;
};

#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 16);
static_assert(sizeof(KeyboardPayload) == 8);
static_assert(sizeof(RelativeMousePayload) == 8);
static_assert(sizeof(AbsoluteMousePayload) == 8);
static_assert(sizeof(ResponsePayload) == 16);
static_assert(std::is_trivially_copyable_v<MessageHeader>);
static_assert(std::is_trivially_copyable_v<KeyboardPayload>);
static_assert(std::is_trivially_copyable_v<RelativeMousePayload>);
static_assert(std::is_trivially_copyable_v<AbsoluteMousePayload>);
static_assert(std::is_trivially_copyable_v<ResponsePayload>);

[[nodiscard]] constexpr std::uint16_t response_command(Command command) noexcept {
    return static_cast<std::uint16_t>(command) | kResponseBit;
}

}  // namespace faker_bridge::protocol

