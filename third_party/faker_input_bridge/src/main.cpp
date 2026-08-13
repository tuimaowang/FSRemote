#include "bridge_client.h"
#include "bridge_protocol.h"
#include "bridge_server.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNotFound = 3;

std::atomic<HANDLE> g_stop_event{nullptr};

BOOL WINAPI console_control_handler(DWORD control_type) {
    if (control_type != CTRL_C_EVENT && control_type != CTRL_BREAK_EVENT &&
        control_type != CTRL_CLOSE_EVENT && control_type != CTRL_SHUTDOWN_EVENT) {
        return FALSE;
    }

    const HANDLE stop_event = g_stop_event.load(std::memory_order_relaxed);
    if (stop_event == nullptr) {
        return FALSE;
    }
    SetEvent(stop_event);
    return TRUE;
}

void print_usage() {
    std::wcout
        << L"FakerInputBridge - local-only FakerInput bridge\n\n"
        << L"Usage:\n"
        << L"  FakerInputBridge.exe --server       Start the local bridge server\n"
        << L"  FakerInputBridge.exe --ping         Check server and driver status\n"
        << L"  FakerInputBridge.exe --click-test   Wait 5 seconds, then click via server\n"
        << L"  FakerInputBridge.exe --release-all  Release input state via server\n"
        << L"  FakerInputBridge.exe --help         Show this help\n\n"
        << L"The server exposes no TCP/UDP port and rejects remote pipe clients.\n";
}

std::optional<faker_bridge::BridgeClient> connect_client() {
    faker_bridge::BridgeError error;
    auto client = faker_bridge::BridgeClient::connect(&error);
    if (!client.has_value()) {
        std::wcerr << L"ERROR: " << error.describe() << L"\n"
                   << L"Start FakerInputBridge.exe --server in another PowerShell window.\n";
    }
    return client;
}

int run_server() {
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
        std::wcerr << L"ERROR: Create stop event failed with "
                   << GetLastError() << L".\n";
        return kExitFailed;
    }

    g_stop_event.store(stop_event, std::memory_order_relaxed);
    if (!SetConsoleCtrlHandler(console_control_handler, TRUE)) {
        std::wcerr << L"ERROR: Register console handler failed with "
                   << GetLastError() << L".\n";
        g_stop_event.store(nullptr, std::memory_order_relaxed);
        CloseHandle(stop_event);
        return kExitFailed;
    }

    const int result = faker_bridge::run_bridge_server(stop_event);
    SetConsoleCtrlHandler(console_control_handler, FALSE);
    g_stop_event.store(nullptr, std::memory_order_relaxed);
    CloseHandle(stop_event);
    return result;
}

int run_ping() {
    auto client = connect_client();
    if (!client.has_value()) {
        return kExitNotFound;
    }

    faker_bridge::BridgeError error;
    faker_bridge::ServerStatus status;
    if (!client->ping(&status, &error)) {
        std::wcerr << L"ERROR: " << error.describe() << L"\n";
        return kExitFailed;
    }

    std::wcout
        << L"RESULT: Bridge server responded.\n"
        << L"Driver ready: " << (status.driver_ready ? L"yes" : L"no") << L"\n"
        << L"Driver version: " << status.driver_version << L"\n";
    return status.driver_ready ? kExitOk : kExitFailed;
}

int run_release_all() {
    auto client = connect_client();
    if (!client.has_value()) {
        return kExitNotFound;
    }

    faker_bridge::BridgeError error;
    if (!client->release_all(&error)) {
        std::wcerr << L"ERROR: " << error.describe() << L"\n";
        return kExitFailed;
    }

    std::wcout << L"RESULT: Input state released through the bridge.\n";
    return kExitOk;
}

int run_click_test() {
    auto client = connect_client();
    if (!client.has_value()) {
        return kExitNotFound;
    }

    faker_bridge::BridgeError error;
    faker_bridge::ServerStatus status;
    if (!client->ping(&status, &error) || !status.driver_ready) {
        std::wcerr << L"ERROR: Bridge or driver is not ready: "
                   << error.describe() << L"\n";
        return kExitFailed;
    }

    std::wcout
        << L"Bridge click test armed. Move the pointer to a SAFE target.\n"
        << L"One left click will be sent at the current pointer position.\n";
    for (int seconds = 5; seconds > 0; --seconds) {
        std::wcout << L"  Clicking in " << seconds << L"...\n" << std::flush;
        Sleep(1000);
    }

    faker_bridge::protocol::RelativeMousePayload mouse{};
    mouse.buttons = 0x01;
    if (!client->send_relative_mouse(mouse, &error)) {
        std::wcerr << L"ERROR sending left button down: "
                   << error.describe() << L"\n";
        faker_bridge::BridgeError release_error;
        if (!client->release_all(&release_error)) {
            std::wcerr << L"WARNING: release-all also failed: "
                       << release_error.describe() << L"\n";
        }
        return kExitFailed;
    }

    Sleep(80);
    mouse.buttons = 0x00;
    if (!client->send_relative_mouse(mouse, &error)) {
        std::wcerr << L"ERROR sending left button up: "
                   << error.describe() << L"\n";
        faker_bridge::BridgeError release_error;
        if (!client->release_all(&release_error)) {
            std::wcerr << L"WARNING: release-all also failed: "
                       << release_error.describe() << L"\n";
        }
        return kExitFailed;
    }

    std::wcout
        << L"RESULT: One FakerInput left click was sent through the bridge.\n"
        << L"Confirm visually whether the target application accepted it.\n";
    return kExitOk;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc != 2) {
        print_usage();
        return kExitUsage;
    }

    const std::wstring_view command = argv[1];
    if (command == L"--server") {
        return run_server();
    }
    if (command == L"--ping") {
        return run_ping();
    }
    if (command == L"--click-test") {
        return run_click_test();
    }
    if (command == L"--release-all") {
        return run_release_all();
    }
    if (command == L"--help" || command == L"-h" || command == L"/?") {
        print_usage();
        return kExitOk;
    }

    std::wcerr << L"Unknown argument: " << command << L"\n\n";
    print_usage();
    return kExitUsage;
}

