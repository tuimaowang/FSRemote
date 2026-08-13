#pragma once

#include "bridge_protocol.h"

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace faker_bridge {

struct BridgeError {
    DWORD code = ERROR_SUCCESS;
    std::wstring context;

    [[nodiscard]] std::wstring describe() const;
};

struct ServerStatus {
    bool driver_ready = false;
    std::uint32_t driver_version = 0;
};

class BridgeClient final {
public:
    BridgeClient() = default;
    ~BridgeClient();

    BridgeClient(const BridgeClient&) = delete;
    BridgeClient& operator=(const BridgeClient&) = delete;
    BridgeClient(BridgeClient&& other) noexcept;
    BridgeClient& operator=(BridgeClient&& other) noexcept;

    [[nodiscard]] static std::optional<BridgeClient> connect(
        BridgeError* error = nullptr);

    [[nodiscard]] bool ping(ServerStatus* status, BridgeError* error = nullptr);
    [[nodiscard]] bool get_status(ServerStatus* status, BridgeError* error = nullptr);
    [[nodiscard]] bool send_keyboard(
        const protocol::KeyboardPayload& payload,
        BridgeError* error = nullptr);
    [[nodiscard]] bool send_relative_mouse(
        const protocol::RelativeMousePayload& payload,
        BridgeError* error = nullptr);
    [[nodiscard]] bool send_absolute_mouse(
        const protocol::AbsoluteMousePayload& payload,
        BridgeError* error = nullptr);
    [[nodiscard]] bool release_all(BridgeError* error = nullptr);

    [[nodiscard]] bool valid() const noexcept;

private:
    explicit BridgeClient(HANDLE pipe);
    [[nodiscard]] bool request(
        protocol::Command command,
        std::span<const std::byte> payload,
        protocol::ResponsePayload* response,
        BridgeError* error);
    void close() noexcept;

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::uint32_t next_sequence_ = 1;
};

}  // namespace faker_bridge

