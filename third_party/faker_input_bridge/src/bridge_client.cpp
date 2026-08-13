#include "bridge_client.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace faker_bridge {
namespace {

void set_error(BridgeError* error, DWORD code, std::wstring context) {
    if (error != nullptr) {
        error->code = code == ERROR_SUCCESS ? ERROR_GEN_FAILURE : code;
        error->context = std::move(context);
    }
}

void populate_status(
    const protocol::ResponsePayload& response,
    ServerStatus* status) {
    if (status != nullptr) {
        status->driver_ready =
            (response.server_flags & protocol::server_flag_driver_ready) != 0;
        status->driver_version = response.driver_version;
    }
}

}  // namespace

std::wstring BridgeError::describe() const {
    std::wstring result = context;
    if (code == ERROR_SUCCESS) {
        return result;
    }
    if (!result.empty()) {
        result += L": ";
    }
    result += L"Windows error ";
    result += std::to_wstring(code);
    std::wostringstream suffix;
    suffix << L" (0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill(L'0') << code << L")";
    result += suffix.str();
    return result;
}

BridgeClient::BridgeClient(HANDLE pipe) : pipe_(pipe) {}

BridgeClient::~BridgeClient() {
    close();
}

BridgeClient::BridgeClient(BridgeClient&& other) noexcept
    : pipe_(std::exchange(other.pipe_, INVALID_HANDLE_VALUE)),
      next_sequence_(std::exchange(other.next_sequence_, 1)) {}

BridgeClient& BridgeClient::operator=(BridgeClient&& other) noexcept {
    if (this != &other) {
        close();
        pipe_ = std::exchange(other.pipe_, INVALID_HANDLE_VALUE);
        next_sequence_ = std::exchange(other.next_sequence_, 1);
    }
    return *this;
}

std::optional<BridgeClient> BridgeClient::connect(BridgeError* error) {
    if (error != nullptr) {
        *error = {};
    }

    if (!WaitNamedPipeW(protocol::kPipeName, 3000)) {
        set_error(error, GetLastError(), L"Wait for FakerInputBridge server");
        return std::nullopt;
    }

    HANDLE pipe = CreateFileW(
        protocol::kPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        set_error(error, GetLastError(), L"Connect to FakerInputBridge server");
        return std::nullopt;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        const DWORD code = GetLastError();
        CloseHandle(pipe);
        set_error(error, code, L"Set named-pipe message mode");
        return std::nullopt;
    }

    return BridgeClient(pipe);
}

bool BridgeClient::ping(ServerStatus* status, BridgeError* error) {
    protocol::ResponsePayload response{};
    const bool success = request(protocol::Command::ping, {}, &response, error);
    if (success) {
        populate_status(response, status);
    }
    return success;
}

bool BridgeClient::get_status(ServerStatus* status, BridgeError* error) {
    protocol::ResponsePayload response{};
    const bool success = request(protocol::Command::get_status, {}, &response, error);
    if (success) {
        populate_status(response, status);
    }
    return success;
}

bool BridgeClient::send_keyboard(
    const protocol::KeyboardPayload& payload,
    BridgeError* error) {
    return request(
        protocol::Command::keyboard,
        std::as_bytes(std::span{&payload, std::size_t{1}}),
        nullptr,
        error);
}

bool BridgeClient::send_relative_mouse(
    const protocol::RelativeMousePayload& payload,
    BridgeError* error) {
    return request(
        protocol::Command::relative_mouse,
        std::as_bytes(std::span{&payload, std::size_t{1}}),
        nullptr,
        error);
}

bool BridgeClient::send_absolute_mouse(
    const protocol::AbsoluteMousePayload& payload,
    BridgeError* error) {
    return request(
        protocol::Command::absolute_mouse,
        std::as_bytes(std::span{&payload, std::size_t{1}}),
        nullptr,
        error);
}

bool BridgeClient::release_all(BridgeError* error) {
    return request(protocol::Command::release_all, {}, nullptr, error);
}

bool BridgeClient::valid() const noexcept {
    return pipe_ != INVALID_HANDLE_VALUE;
}

bool BridgeClient::request(
    protocol::Command command,
    std::span<const std::byte> payload,
    protocol::ResponsePayload* response,
    BridgeError* error) {
    if (error != nullptr) {
        *error = {};
    }
    if (!valid()) {
        set_error(error, ERROR_INVALID_HANDLE, L"Bridge pipe is not connected");
        return false;
    }
    if (payload.size() > protocol::kMaxMessageBytes - sizeof(protocol::MessageHeader)) {
        set_error(error, ERROR_INVALID_PARAMETER, L"Bridge request is too large");
        return false;
    }

    protocol::MessageHeader header{};
    header.command = static_cast<std::uint16_t>(command);
    header.payload_bytes = static_cast<std::uint32_t>(payload.size());
    header.sequence = next_sequence_++;

    std::vector<std::byte> request_bytes(sizeof(header) + payload.size());
    std::memcpy(request_bytes.data(), &header, sizeof(header));
    if (!payload.empty()) {
        std::memcpy(
            request_bytes.data() + sizeof(header), payload.data(), payload.size());
    }

    DWORD bytes_written = 0;
    if (!WriteFile(
            pipe_,
            request_bytes.data(),
            static_cast<DWORD>(request_bytes.size()),
            &bytes_written,
            nullptr) ||
        bytes_written != request_bytes.size()) {
        set_error(error, GetLastError(), L"Write bridge request");
        return false;
    }

    std::array<std::byte,
               sizeof(protocol::MessageHeader) + sizeof(protocol::ResponsePayload)>
        response_bytes{};
    DWORD bytes_read = 0;
    if (!ReadFile(
            pipe_,
            response_bytes.data(),
            static_cast<DWORD>(response_bytes.size()),
            &bytes_read,
            nullptr)) {
        set_error(error, GetLastError(), L"Read bridge response");
        return false;
    }
    if (bytes_read != response_bytes.size()) {
        set_error(error, ERROR_INVALID_DATA, L"Bridge response has the wrong length");
        return false;
    }

    protocol::MessageHeader response_header{};
    protocol::ResponsePayload response_payload{};
    std::memcpy(&response_header, response_bytes.data(), sizeof(response_header));
    std::memcpy(
        &response_payload,
        response_bytes.data() + sizeof(response_header),
        sizeof(response_payload));

    if (response_header.magic != protocol::kMagic ||
        response_header.version != protocol::kVersion ||
        response_header.command != protocol::response_command(command) ||
        response_header.sequence != header.sequence ||
        response_header.payload_bytes != sizeof(response_payload)) {
        set_error(error, ERROR_INVALID_DATA, L"Bridge response header is invalid");
        return false;
    }
    if (response_payload.result != ERROR_SUCCESS) {
        set_error(error, response_payload.result, L"Bridge rejected the request");
        return false;
    }

    if (response != nullptr) {
        *response = response_payload;
    }
    return true;
}

void BridgeClient::close() noexcept {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

}  // namespace faker_bridge

