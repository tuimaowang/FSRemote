#include "bridge_server.h"

#include "bridge_protocol.h"
#include "faker_input_device.h"

#include <sddl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace faker_bridge {
namespace {

enum class IoState {
    completed,
    stopped,
    disconnected,
    failed,
};

struct IoResult {
    IoState state = IoState::failed;
    DWORD bytes = 0;
    DWORD error = ERROR_GEN_FAILURE;
};

class Handle final {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() {
        if (valid()) {
            CloseHandle(value_);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

class LocalMemory final {
public:
    explicit LocalMemory(HLOCAL value = nullptr) : value_(value) {}
    ~LocalMemory() {
        if (value_ != nullptr) {
            LocalFree(value_);
        }
    }

    LocalMemory(const LocalMemory&) = delete;
    LocalMemory& operator=(const LocalMemory&) = delete;

    [[nodiscard]] HLOCAL get() const noexcept { return value_; }

private:
    HLOCAL value_ = nullptr;
};

[[nodiscard]] DWORD normalized_error(DWORD code) {
    return code == ERROR_SUCCESS ? ERROR_GEN_FAILURE : code;
}

[[nodiscard]] IoResult wait_for_overlapped(
    HANDLE pipe,
    HANDLE stop_event,
    OVERLAPPED& operation) {
    const std::array<HANDLE, 2> waits{stop_event, operation.hEvent};
    const DWORD wait = WaitForMultipleObjects(
        static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        WaitForSingleObject(operation.hEvent, INFINITE);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &operation, &ignored, FALSE);
        return {IoState::stopped, 0, ERROR_OPERATION_ABORTED};
    }
    if (wait != WAIT_OBJECT_0 + 1) {
        return {IoState::failed, 0, normalized_error(GetLastError())};
    }

    DWORD bytes = 0;
    if (!GetOverlappedResult(pipe, &operation, &bytes, FALSE)) {
        const DWORD code = normalized_error(GetLastError());
        if (code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA ||
            code == ERROR_PIPE_NOT_CONNECTED) {
            return {IoState::disconnected, bytes, code};
        }
        return {IoState::failed, bytes, code};
    }
    return {IoState::completed, bytes, ERROR_SUCCESS};
}

[[nodiscard]] IoResult connect_pipe(HANDLE pipe, HANDLE stop_event) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return {IoState::failed, 0, normalized_error(GetLastError())};
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();

    if (ConnectNamedPipe(pipe, &operation)) {
        return {IoState::completed, 0, ERROR_SUCCESS};
    }
    const DWORD code = normalized_error(GetLastError());
    if (code == ERROR_PIPE_CONNECTED) {
        return {IoState::completed, 0, ERROR_SUCCESS};
    }
    if (code != ERROR_IO_PENDING) {
        return {IoState::failed, 0, code};
    }
    return wait_for_overlapped(pipe, stop_event, operation);
}

[[nodiscard]] IoResult read_pipe(
    HANDLE pipe,
    HANDLE stop_event,
    std::span<std::byte> buffer) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return {IoState::failed, 0, normalized_error(GetLastError())};
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD bytes = 0;
    if (ReadFile(
            pipe,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes,
            &operation)) {
        return {IoState::completed, bytes, ERROR_SUCCESS};
    }

    const DWORD code = normalized_error(GetLastError());
    if (code == ERROR_IO_PENDING) {
        return wait_for_overlapped(pipe, stop_event, operation);
    }
    if (code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA ||
        code == ERROR_PIPE_NOT_CONNECTED) {
        return {IoState::disconnected, bytes, code};
    }
    return {IoState::failed, bytes, code};
}

[[nodiscard]] IoResult write_pipe(
    HANDLE pipe,
    HANDLE stop_event,
    std::span<const std::byte> buffer) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return {IoState::failed, 0, normalized_error(GetLastError())};
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD bytes = 0;
    if (WriteFile(
            pipe,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes,
            &operation)) {
        return {IoState::completed, bytes, ERROR_SUCCESS};
    }

    const DWORD code = normalized_error(GetLastError());
    if (code == ERROR_IO_PENDING) {
        return wait_for_overlapped(pipe, stop_event, operation);
    }
    if (code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA ||
        code == ERROR_PIPE_NOT_CONNECTED) {
        return {IoState::disconnected, bytes, code};
    }
    return {IoState::failed, bytes, code};
}

[[nodiscard]] std::wstring current_user_sid_string(DWORD* error) {
    HANDLE raw_token = INVALID_HANDLE_VALUE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        if (error != nullptr) {
            *error = normalized_error(GetLastError());
        }
        return {};
    }
    Handle token(raw_token);

    DWORD required = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
    if (required == 0) {
        if (error != nullptr) {
            *error = normalized_error(GetLastError());
        }
        return {};
    }

    std::vector<std::byte> buffer(required);
    if (!GetTokenInformation(
            token.get(), TokenUser, buffer.data(), required, &required)) {
        if (error != nullptr) {
            *error = normalized_error(GetLastError());
        }
        return {};
    }

    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR raw_sid = nullptr;
    if (!ConvertSidToStringSidW(token_user->User.Sid, &raw_sid)) {
        if (error != nullptr) {
            *error = normalized_error(GetLastError());
        }
        return {};
    }
    LocalMemory sid_memory(raw_sid);
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }
    return raw_sid;
}

[[nodiscard]] HANDLE create_restricted_pipe(DWORD* error) {
    DWORD sid_error = ERROR_SUCCESS;
    const std::wstring user_sid = current_user_sid_string(&sid_error);
    if (user_sid.empty()) {
        if (error != nullptr) {
            *error = sid_error;
        }
        return INVALID_HANDLE_VALUE;
    }

    const std::wstring sddl =
        L"D:P(A;;GA;;;SY)(A;;GA;;;" + user_sid + L")";
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &raw_descriptor,
            nullptr)) {
        if (error != nullptr) {
            *error = normalized_error(GetLastError());
        }
        return INVALID_HANDLE_VALUE;
    }
    LocalMemory descriptor(raw_descriptor);

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = raw_descriptor;
    attributes.bInheritHandle = FALSE;

    HANDLE pipe = CreateNamedPipeW(
        protocol::kPipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1,
        4096,
        4096,
        0,
        &attributes);
    if (error != nullptr) {
        *error = pipe == INVALID_HANDLE_VALUE
            ? normalized_error(GetLastError())
            : ERROR_SUCCESS;
    }
    return pipe;
}

template <typename Payload>
[[nodiscard]] bool copy_payload(
    std::span<const std::byte> bytes,
    Payload* payload) {
    if (bytes.size() != sizeof(Payload) || payload == nullptr) {
        return false;
    }
    std::memcpy(payload, bytes.data(), sizeof(Payload));
    return true;
}

[[nodiscard]] DWORD dispatch_request(
    protocol::Command command,
    std::span<const std::byte> payload,
    FakerInputDevice& device) {
    DeviceError device_error;
    switch (command) {
    case protocol::Command::ping:
    case protocol::Command::get_status:
        return payload.empty() ? ERROR_SUCCESS : ERROR_INVALID_DATA;

    case protocol::Command::keyboard: {
        protocol::KeyboardPayload value{};
        if (!copy_payload(payload, &value) || value.reserved != 0) {
            return ERROR_INVALID_DATA;
        }
        const std::span<const std::uint8_t> usages(value.usages);
        return device.send_keyboard(value.modifiers, usages, &device_error)
            ? ERROR_SUCCESS
            : normalized_error(device_error.code);
    }

    case protocol::Command::relative_mouse: {
        protocol::RelativeMousePayload value{};
        if (!copy_payload(payload, &value) || value.reserved != 0 ||
            (value.buttons & 0xE0u) != 0) {
            return ERROR_INVALID_DATA;
        }
        return device.send_relative_mouse(
                   value.buttons,
                   value.dx,
                   value.dy,
                   value.wheel,
                   value.horizontal_wheel,
                   &device_error)
            ? ERROR_SUCCESS
            : normalized_error(device_error.code);
    }

    case protocol::Command::absolute_mouse: {
        protocol::AbsoluteMousePayload value{};
        if (!copy_payload(payload, &value) || value.reserved != 0 ||
            (value.buttons & 0xE0u) != 0 || value.x > 32767 || value.y > 32767) {
            return ERROR_INVALID_DATA;
        }
        return device.send_absolute_mouse(
                   value.buttons,
                   value.x,
                   value.y,
                   value.wheel,
                   &device_error)
            ? ERROR_SUCCESS
            : normalized_error(device_error.code);
    }

    case protocol::Command::release_all:
        if (!payload.empty()) {
            return ERROR_INVALID_DATA;
        }
        return device.release_all(&device_error)
            ? ERROR_SUCCESS
            : normalized_error(device_error.code);
    }

    return ERROR_NOT_SUPPORTED;
}

[[nodiscard]] bool send_response(
    HANDLE pipe,
    HANDLE stop_event,
    const protocol::MessageHeader& request,
    DWORD result,
    const FakerInputDevice& device) {
    protocol::MessageHeader header{};
    header.command = request.command | protocol::kResponseBit;
    header.payload_bytes = sizeof(protocol::ResponsePayload);
    header.sequence = request.sequence;

    protocol::ResponsePayload payload{};
    payload.result = result;
    payload.server_flags = protocol::server_flag_driver_ready;
    payload.driver_version = device.driver_version();

    std::array<std::byte,
               sizeof(protocol::MessageHeader) + sizeof(protocol::ResponsePayload)>
        bytes{};
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), &payload, sizeof(payload));
    const IoResult write = write_pipe(pipe, stop_event, bytes);
    return write.state == IoState::completed && write.bytes == bytes.size();
}

void release_after_client(FakerInputDevice& device) {
    DeviceError error;
    if (!device.release_all(&error)) {
        std::wcerr << L"WARNING: release after client disconnect failed: "
                   << error.describe() << L"\n";
    }
}

void serve_client(HANDLE pipe, HANDLE stop_event, FakerInputDevice& device) {
    std::array<std::byte, protocol::kMaxMessageBytes> buffer{};
    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        const IoResult read = read_pipe(pipe, stop_event, buffer);
        if (read.state == IoState::stopped || read.state == IoState::disconnected) {
            return;
        }
        if (read.state != IoState::completed) {
            std::wcerr << L"WARNING: pipe read failed with " << read.error << L".\n";
            return;
        }
        if (read.bytes < sizeof(protocol::MessageHeader)) {
            std::wcerr << L"WARNING: rejected a short bridge message.\n";
            return;
        }

        protocol::MessageHeader header{};
        std::memcpy(&header, buffer.data(), sizeof(header));
        if (header.magic != protocol::kMagic ||
            header.version != protocol::kVersion ||
            header.payload_bytes != read.bytes - sizeof(header)) {
            std::wcerr << L"WARNING: rejected an invalid bridge header.\n";
            return;
        }

        const auto payload = std::span<const std::byte>(
            buffer.data() + sizeof(header), header.payload_bytes);
        const auto command = static_cast<protocol::Command>(header.command);
        const DWORD result = dispatch_request(command, payload, device);
        if (!send_response(pipe, stop_event, header, result, device)) {
            return;
        }
    }
}

}  // namespace

int run_bridge_server(HANDLE stop_event) {
    DeviceError device_error;
    auto device = FakerInputDevice::open(&device_error);
    if (!device.has_value()) {
        std::wcerr << L"ERROR: " << device_error.describe() << L"\n";
        return 3;
    }

    std::wcout
        << L"FakerInputBridge server ready.\n"
        << L"Pipe: " << protocol::kPipeName << L"\n"
        << L"Driver version: " << device->driver_version() << L"\n"
        << L"Local clients only. Press Ctrl+C to stop.\n";

    int exit_code = 0;
    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        DWORD pipe_error = ERROR_SUCCESS;
        Handle pipe(create_restricted_pipe(&pipe_error));
        if (!pipe.valid()) {
            std::wcerr << L"ERROR: Create restricted pipe failed with "
                       << pipe_error << L".\n";
            exit_code = 1;
            break;
        }

        const IoResult connection = connect_pipe(pipe.get(), stop_event);
        if (connection.state == IoState::stopped) {
            break;
        }
        if (connection.state != IoState::completed) {
            std::wcerr << L"WARNING: pipe connection failed with "
                       << connection.error << L".\n";
            continue;
        }

        std::wcout << L"Client connected.\n";
        serve_client(pipe.get(), stop_event, *device);
        DisconnectNamedPipe(pipe.get());
        release_after_client(*device);
        std::wcout << L"Client disconnected; input state released.\n";
    }

    release_after_client(*device);
    std::wcout << L"FakerInputBridge server stopped; input state released.\n";
    return exit_code;
}

}  // namespace faker_bridge
