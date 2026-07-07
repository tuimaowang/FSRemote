#include "FsRemoteStreamApi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "native_webrtc_runtime.h"
#include "signaling.h"
#include "system_audio_stream.h"
#include "uu_codec_factory.h"
#include "webrtc_session.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <exception>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

thread_local std::string g_last_error;
std::mutex g_log_mutex;
std::once_flag g_crash_handler_once;

void append_log_to_file(const char* file_name, const std::string& line)
{
    char exePath[MAX_PATH] = {};
    if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) {
        return;
    }

    std::string path(exePath);
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) {
        return;
    }
    path.resize(slash + 1);
    path += "data\\";
    ::CreateDirectoryA(path.c_str(), nullptr);
    path += file_name;

    std::lock_guard lock(g_log_mutex);
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "ab") != 0 || !file) {
        return;
    }
    fwrite(line.data(), 1, line.size(), file);
    fwrite("\r\n", 1, 2, file);
    fclose(file);
}

// =====wjy====
void reset_log_file(const char* file_name)
{
    char exePath[MAX_PATH] = {}; // wjy: clear the log under the running FSRemote.exe directory.
    if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) {
        return;
    }

    std::string path(exePath);
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) {
        return;
    }
    path.resize(slash + 1);
    path += "data\\";
    ::CreateDirectoryA(path.c_str(), nullptr);
    path += file_name;

    std::lock_guard lock(g_log_mutex);
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") == 0 && file) {
        fclose(file); // wjy: opening with wb truncates old content once per process start.
    }
}
// ===end====

void append_log(const std::string& line)
{
    append_log_to_file("stream_host.log", line);
}

void append_input_debug_log(const std::string& line)
{
    char tempPath[MAX_PATH] = {};
    if (::GetTempPathA(MAX_PATH, tempPath) == 0) {
        return;
    }

    std::string path(tempPath);
    path += "fsremote_input_debug.log";

    SYSTEMTIME time = {};
    ::GetLocalTime(&time);
    char prefix[96] = {};
    std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u tid=%lu native ",
                  time.wYear, time.wMonth, time.wDay,
                  time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
                  static_cast<unsigned long>(::GetCurrentThreadId()));

    std::lock_guard lock(g_log_mutex);
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "ab") != 0 || !file) {
        return;
    }
    const std::string text = std::string(prefix) + line;
    fwrite(text.data(), 1, text.size(), file);
    fwrite("\r\n", 1, 2, file);
    fclose(file);
}

bool should_log_input_message(const std::string& message)
{
    if (message.rfind("m ", 0) != 0) {
        return true;
    }

    static ULONGLONG last_mouse_move_log_ms = 0;
    const ULONGLONG now_ms = ::GetTickCount64();
    if (now_ms - last_mouse_move_log_ms < 500) {
        return false;
    }
    last_mouse_move_log_ms = now_ms;
    return true;
}

std::string cursor_lock_probe_text()
{
    POINT cursor = {};
    const BOOL cursor_ok = ::GetCursorPos(&cursor);
    const int screen_x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int screen_y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int screen_w = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int screen_h = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int center_x = screen_x + screen_w / 2;
    const int center_y = screen_y + screen_h / 2;
    const int dx = cursor_ok ? cursor.x - center_x : 0;
    const int dy = cursor_ok ? cursor.y - center_y : 0;

    char text[256] = {};
    std::snprintf(text, sizeof(text),
                  " cursor_ok=%d cursor=%ld,%ld vdesk=%d,%d,%d,%d center=%d,%d dist=%d,%d",
                  cursor_ok ? 1 : 0,
                  static_cast<long>(cursor.x),
                  static_cast<long>(cursor.y),
                  screen_x,
                  screen_y,
                  screen_w,
                  screen_h,
                  center_x,
                  center_y,
                  dx,
                  dy);
    return text;
}

// =====wjy====
void append_viewer_log(const std::string& line)
{
    (void)line;
    return; // wjy: disable native viewer diagnostics completely while testing stream smoothness.

    SYSTEMTIME time = {}; // wjy: capture wall-clock time so the last line can be matched with the crash moment.
    ::GetLocalTime(&time);
    char prefix[96] = {};
    std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u tid=%lu ",
                  time.wYear, time.wMonth, time.wDay,
                  time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
                  static_cast<unsigned long>(::GetCurrentThreadId()));
    append_log_to_file("stream_viewer_debug.log", std::string(prefix) + line); // wjy: write one flushed line per diagnostic event.
}

LONG WINAPI fsremote_crash_filter(EXCEPTION_POINTERS* pointers)
{
    const auto* record = pointers ? pointers->ExceptionRecord : nullptr;
    // =====wjy====
    const void* crash_address = record ? record->ExceptionAddress : nullptr; // wjy: keep the raw fault address from SEH.
    MEMORY_BASIC_INFORMATION memory_info = {}; // wjy: VirtualQuery tells which loaded module owns the crash address.
    const bool has_module = crash_address
        && ::VirtualQuery(crash_address, &memory_info, sizeof(memory_info)) == sizeof(memory_info)
        && memory_info.AllocationBase;
    char module_path[MAX_PATH] = {};
    if (has_module) {
        ::GetModuleFileNameA(static_cast<HMODULE>(memory_info.AllocationBase), module_path, MAX_PATH);
    }
    const auto module_base = has_module ? reinterpret_cast<uintptr_t>(memory_info.AllocationBase) : 0;
    const auto fault = reinterpret_cast<uintptr_t>(crash_address);
    char line[1024] = {};
    std::snprintf(line, sizeof(line),
                  "crash exception=0x%08lX address=%p module=%s base=%p offset=0x%llX",
                  record ? static_cast<unsigned long>(record->ExceptionCode) : 0ul,
                  crash_address,
                  module_path[0] ? module_path : "<unknown>",
                  has_module ? memory_info.AllocationBase : nullptr,
                  static_cast<unsigned long long>(has_module ? fault - module_base : 0));
    // ===end====
    append_viewer_log(line); // wjy: preserve the native exception code/address even when the process terminates immediately.
    return EXCEPTION_EXECUTE_HANDLER;
}

void install_crash_logger()
{
    std::call_once(g_crash_handler_once, [] {
        // =====wjy====
        reset_log_file("stream_viewer_debug.log"); // wjy: each FSRemote process starts with a fresh viewer diagnostic log.
        // ===end====
        ::SetUnhandledExceptionFilter(fsremote_crash_filter); // wjy: install once for both host and viewer runs in this process.
        append_viewer_log("crash logger installed");
    });
}
// ===end====

void set_error(const std::string& error)
{
    g_last_error = error;
}

bool ensure_wsa()
{
    static bool initialized = [] {
        WSADATA data = {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

void report_status(FsRemoteStatusCallback callback, void* user, int code, const char* message)
{
    if (callback) {
        callback(user, code, message);
    }
}

bool handle_message(uu::WebrtcSession& session, const std::string& message)
{
    const auto split = message.find('\n');
    if (split == std::string::npos) {
        append_viewer_log("signaling malformed message no-split size=" + std::to_string(message.size()));
        return true;
    }

    const std::string kind = message.substr(0, split);
    const std::string body = message.substr(split + 1);
    std::string error;
    if (kind == "offer" || kind == "answer" || kind == "pranswer") {
        // =====wjy====
        append_viewer_log("signaling recv " + kind + " sdp_size=" + std::to_string(body.size())); // wjy: mark before SetRemoteDescription.
        session.accept_remote_description(kind, body, &error);
        if (!error.empty()) {
            append_viewer_log("signaling " + kind + " error=" + error); // wjy: keep parse/set errors outside Qt Creator output.
        }
        // ===end====
    } else if (kind == "candidate") {
        const auto split2 = body.find('\n');
        const auto split3 = body.find('\n', split2 == std::string::npos ? 0 : split2 + 1);
        if (split2 != std::string::npos && split3 != std::string::npos) {
            const std::string mid = body.substr(0, split2);
            const int mline = std::stoi(body.substr(split2 + 1, split3 - split2 - 1));
            const std::string candidate = body.substr(split3 + 1);
            // =====wjy====
            append_viewer_log("signaling recv candidate mid=" + mid + " mline=" + std::to_string(mline)); // wjy: identify whether ICE reaches native WebRTC before the crash.
            session.add_remote_candidate(mid, mline, candidate, &error);
            if (!error.empty()) {
                append_viewer_log("signaling candidate error=" + error); // wjy: capture candidate add failures in the durable log.
            }
            // ===end====
        }
    }
    return true;
}

void move_mouse_absolute(int x, int y, bool log_result)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    ::SetLastError(ERROR_SUCCESS);
    const UINT sent = SendInput(1, &input, sizeof(input));
    const DWORD error = ::GetLastError();
    if (log_result) {
        append_input_debug_log("host SendInput abs x=" + std::to_string(x)
            + " y=" + std::to_string(y)
            + " sent=" + std::to_string(sent)
            + " error=" + std::to_string(error)
            + cursor_lock_probe_text());
    }
}

void move_mouse_relative(int dx, int dy, bool log_result)
{
    if (dx == 0 && dy == 0) {
        return;
    }

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    ::SetLastError(ERROR_SUCCESS);
    const UINT sent = SendInput(1, &input, sizeof(input));
    const DWORD error = ::GetLastError();
    if (log_result) {
        append_input_debug_log("host SendInput rel dx=" + std::to_string(dx)
            + " dy=" + std::to_string(dy)
            + " sent=" + std::to_string(sent)
            + " error=" + std::to_string(error)
            + cursor_lock_probe_text());
    }
}

struct MouseInputModeState {
    bool game_relative_mode = false;
    bool has_last_viewer_pos = false;
    int last_viewer_x = 0;
    int last_viewer_y = 0;
    int lock_score = 0;
    int unlock_score = 0;
};

MouseInputModeState g_mouse_input_mode;

bool get_cursor_center_distance(int& dx, int& dy, int& screen_w, int& screen_h)
{
    POINT cursor = {};
    if (!::GetCursorPos(&cursor)) {
        dx = 0;
        dy = 0;
        screen_w = 0;
        screen_h = 0;
        return false;
    }

    const int screen_x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int screen_y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    screen_w = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    screen_h = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int center_x = screen_x + screen_w / 2;
    const int center_y = screen_y + screen_h / 2;
    dx = cursor.x - center_x;
    dy = cursor.y - center_y;
    return true;
}

bool normalized_point_far_from_center(int x, int y, int screen_w, int screen_h)
{
    if (screen_w <= 0 || screen_h <= 0) {
        return false;
    }

    const int px = (x * screen_w) / 65535;
    const int py = (y * screen_h) / 65535;
    const int dx = px - screen_w / 2;
    const int dy = py - screen_h / 2;
    return std::abs(dx) > 48 || std::abs(dy) > 48;
}

void publish_mouse_mode(uu::WebrtcSession* session, bool relative, const char* reason)
{
    if (!session) {
        return;
    }

    const char* mode = relative ? "relative" : "desktop";
    const bool ok = session->send_control_message(std::string("__fsremote_mouse_mode ") + mode);
    append_input_debug_log(std::string("host mouse mode notify mode=") + mode
        + " ok=" + std::to_string(ok ? 1 : 0)
        + " reason=" + reason);
}

void reset_mouse_relative_mode(const char* reason, bool log_result, uu::WebrtcSession* session)
{
    (void)log_result;
    const bool was_relative = g_mouse_input_mode.game_relative_mode;
    g_mouse_input_mode = {};
    if (was_relative) {
        append_input_debug_log(std::string("host mouse mode desktop reason=") + reason + cursor_lock_probe_text());
        publish_mouse_mode(session, false, reason);
    }
}

void update_relative_unlock_probe(bool log_result, uu::WebrtcSession* session)
{
    if (!g_mouse_input_mode.game_relative_mode) {
        return;
    }

    int center_dx = 0;
    int center_dy = 0;
    int screen_w = 0;
    int screen_h = 0;
    const bool cursor_ok = get_cursor_center_distance(center_dx, center_dy, screen_w, screen_h);
    const bool cursor_near_center = cursor_ok && std::abs(center_dx) <= 12 && std::abs(center_dy) <= 12;
    if (cursor_near_center) {
        g_mouse_input_mode.unlock_score = 0;
        return;
    }

    g_mouse_input_mode.unlock_score = std::min(g_mouse_input_mode.unlock_score + 1, 10);
    if (g_mouse_input_mode.unlock_score >= 10) {
        reset_mouse_relative_mode("cursor-left-center", log_result, session);
    }
}

bool should_use_relative_mouse_for_move(int x, int y, bool log_result, uu::WebrtcSession* session)
{
    (void)x;
    (void)y;
    int center_dx = 0;
    int center_dy = 0;
    int screen_w = 0;
    int screen_h = 0;
#if 1
    const bool cursor_ok = get_cursor_center_distance(center_dx, center_dy, screen_w, screen_h);
    const bool cursor_near_center = cursor_ok && std::abs(center_dx) <= 3 && std::abs(center_dy) <= 3;
    const bool target_far = normalized_point_far_from_center(x, y, screen_w, screen_h);

    if (cursor_near_center && target_far) {
        g_mouse_input_mode.lock_score = std::min(g_mouse_input_mode.lock_score + 1, 4);
        g_mouse_input_mode.unlock_score = 0;
    } else if (g_mouse_input_mode.game_relative_mode) {
        g_mouse_input_mode.unlock_score = std::min(g_mouse_input_mode.unlock_score + 1, 10);
    } else {
        g_mouse_input_mode.lock_score = 0;
    }

    if (!g_mouse_input_mode.game_relative_mode && g_mouse_input_mode.lock_score >= 3) {
        g_mouse_input_mode.game_relative_mode = true;
        g_mouse_input_mode.has_last_viewer_pos = false;
        g_mouse_input_mode.unlock_score = 0;
        append_input_debug_log("host mouse mode relative reason=center-lock" + cursor_lock_probe_text());
        publish_mouse_mode(session, true, "center-lock");
    }

    if (g_mouse_input_mode.game_relative_mode && g_mouse_input_mode.unlock_score >= 10) {
        reset_mouse_relative_mode("cursor-unlocked", log_result, session);
    }
#else
    (void)log_result;
    (void)session;
    (void)center_dx;
    (void)center_dy;
    (void)screen_w;
    (void)screen_h;
#endif

    return g_mouse_input_mode.game_relative_mode;
}

void move_mouse_auto(int x, int y, bool log_result, uu::WebrtcSession* session)
{
    if (!should_use_relative_mouse_for_move(x, y, log_result, session)) {
        g_mouse_input_mode.has_last_viewer_pos = true;
        g_mouse_input_mode.last_viewer_x = x;
        g_mouse_input_mode.last_viewer_y = y;
        move_mouse_absolute(x, y, log_result);
        return;
    }

    if (!g_mouse_input_mode.has_last_viewer_pos) {
        g_mouse_input_mode.has_last_viewer_pos = true;
        g_mouse_input_mode.last_viewer_x = x;
        g_mouse_input_mode.last_viewer_y = y;
        if (log_result) {
            append_input_debug_log("host SendInput rel skipped reason=prime-last-viewer-pos" + cursor_lock_probe_text());
        }
        return;
    }

    int screen_w = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screen_h = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (screen_w <= 0) {
        screen_w = 1920;
    }
    if (screen_h <= 0) {
        screen_h = 1080;
    }

    const int raw_dx = x - g_mouse_input_mode.last_viewer_x;
    const int raw_dy = y - g_mouse_input_mode.last_viewer_y;
    g_mouse_input_mode.last_viewer_x = x;
    g_mouse_input_mode.last_viewer_y = y;

    int rel_dx = (raw_dx * screen_w) / 65535;
    int rel_dy = (raw_dy * screen_h) / 65535;
    rel_dx = std::clamp(rel_dx, -200, 200);
    rel_dy = std::clamp(rel_dy, -200, 200);
    move_mouse_relative(rel_dx, rel_dy, log_result);
}

void send_mouse_button(DWORD flag)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    SendInput(1, &input, sizeof(input));
}

void send_mouse_wheel(int delta)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.mouseData = static_cast<DWORD>(delta);
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &input, sizeof(input));
}

void send_key(int vk, bool down)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}

void inject_input_message(const std::string& message, uu::WebrtcSession* session)
{
    const bool log_message = should_log_input_message(message);
    if (log_message) {
        append_input_debug_log("host recv msg=\"" + message + "\"" + cursor_lock_probe_text());
    }

    std::istringstream input(message);
    std::string kind;
    input >> kind;
    if (kind == "m") {
        int x = 0;
        int y = 0;
        int buttons = 0;
        if (input >> x >> y >> buttons) {
            move_mouse_auto(x, y, log_message, session);
        }
        return;
    }
    if (kind == "r") {
        int dx = 0;
        int dy = 0;
        int buttons = 0;
        if (input >> dx >> dy >> buttons) {
            update_relative_unlock_probe(log_message, session);
            move_mouse_relative(std::clamp(dx, -200, 200), std::clamp(dy, -200, 200), log_message);
        }
        return;
    }
    if (kind == "c") {
        int active = 0;
        if (input >> active) {
            if (active == 0) {
                reset_mouse_relative_mode("viewer-release", log_message, session);
            }
        }
        return;
    }
    if (kind == "d" || kind == "u") {
        int button = 0;
        int x = 0;
        int y = 0;
        if (!(input >> button >> x >> y)) return;
        if (g_mouse_input_mode.game_relative_mode) {
            g_mouse_input_mode.has_last_viewer_pos = false;
        } else {
            move_mouse_absolute(x, y, log_message);
        }
        const bool down = kind == "d";
        if (button == 1) send_mouse_button(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
        if (button == 2) send_mouse_button(down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
        if (button == 4) send_mouse_button(down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP);
        return;
    }
    if (kind == "w") {
        int delta = 0;
        int x = 0;
        int y = 0;
        if (input >> delta >> x >> y) {
            if (!g_mouse_input_mode.game_relative_mode) {
                move_mouse_absolute(x, y, log_message);
            }
            send_mouse_wheel(delta);
        }
        return;
    }
    if (kind == "k") {
        int vk = 0;
        int down = 0;
        if (input >> vk >> down) {
            if (vk == VK_ESCAPE && down != 0) {
                reset_mouse_relative_mode("escape", log_message, session);
            }
            send_key(vk, down != 0);
        }
    }
}

class StreamInstance {
public:
    virtual ~StreamInstance() { stop(); }

    void stop()
    {
        running_ = false;
        const uintptr_t serverSocket = server_socket_.exchange(0);
        if (serverSocket) {
            uu::close_socket(serverSocket);
        }
        const uintptr_t socket = socket_.exchange(0);
        if (socket) {
            uu::close_socket(socket);
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    virtual bool sendInput(const char*) { return false; }
    virtual bool isBusy() const { return false; }

protected:
    std::atomic_bool running_ = true;
    std::atomic_uintptr_t server_socket_ = 0;
    std::atomic_uintptr_t socket_ = 0;
    std::thread worker_;
};

class HostInstance final : public StreamInstance {
public:
    explicit HostInstance(uint16_t port)
    {
        worker_ = std::thread([this, port] { run(port); });
    }

    ~HostInstance() override
    {
        // =====wjy====
        stop(); // wjy: stop and join the worker before HostInstance members are destroyed.
        // ===end====
        if (audio_streamer_) {
            audio_streamer_->stop();
        }
    }

    bool isBusy() const override
    {
        return socket_.load() != 0;
    }

private:
    uintptr_t acceptTcp(uint16_t port, std::string* error)
    {
        if (!ensure_wsa()) {
            if (error) *error = "WSAStartup failed";
            return 0;
        }

        SOCKET server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server == INVALID_SOCKET) {
            if (error) *error = "socket failed";
            return 0;
        }

        BOOL reuse = TRUE;
        ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
            ::listen(server, 1) == SOCKET_ERROR) {
            if (error) *error = "bind/listen failed";
            closesocket(server);
            return 0;
        }

        server_socket_ = static_cast<uintptr_t>(server);
        SOCKET client = ::accept(server, nullptr, nullptr);
        const uintptr_t published = server_socket_.exchange(0);
        if (published) {
            closesocket(server);
        }

        if (client == INVALID_SOCKET) {
            if (running_ && error) *error = "accept failed";
            return 0;
        }
        return static_cast<uintptr_t>(client);
    }

    void run(uint16_t port)
    {
        uu::NativeWebrtcRuntime runtime;
        std::string runtime_error;
        if (!runtime.initialize(&runtime_error)) {
            append_log("host runtime init failed: " + runtime_error);
            return;
        }

        while (running_) {
            std::string error;
            append_log("host waiting accept");
            const uintptr_t socket = acceptTcp(port, &error);
            if (!socket || !running_) {
                if (socket) uu::close_socket(socket);
                if (!error.empty()) {
                    append_log("host accept failed: " + error);
                }
                continue;
            }
            socket_ = socket;
            append_log("host accepted client");
            {
                std::string audio_error;
                audio_streamer_ = std::make_unique<uu::HostAudioStreamer>();
                audio_streamer_->start(49105, &audio_error);
            }

            {
                uu::SessionConfig config;
                config.role = uu::SessionRole::Host;
                uu::WebrtcSession session(&runtime, config);
                session.set_signal_callback([socket](const std::string& kind, const std::string& body) {
                    uu::send_message(socket, kind + "\n" + body);
                });
                session.set_control_callback([&session](const std::string& message) {
                    inject_input_message(message, &session);
                });
                append_log("host session initialize begin");
                if (session.initialize(&error) && session.start_offer(&error)) {
                    append_log("host session running");
                    std::string message;
                    while (running_ && uu::recv_message(socket, &message)) {
                        handle_message(session, message);
                    }
                    append_log("host session recv loop ended");
                } else {
                    append_log("host session init/start failed: " + error);
                }
            }

            const uintptr_t current = socket_.exchange(0);
            if (current) {
                uu::close_socket(current);
            }
            if (audio_streamer_) {
                audio_streamer_->resetClient();
                audio_streamer_->stop();
                audio_streamer_.reset();
            }
            append_log("host client socket closed");
        }
    }

    std::unique_ptr<uu::HostAudioStreamer> audio_streamer_;
};

class ViewerInstance final : public StreamInstance {
public:
    ViewerInstance(
        std::string hostIp,
        uint16_t port,
        FsRemoteFrameCallback frameCallback,
        FsRemoteTextureFrameCallback textureCallback,
        FsRemoteStatusCallback statusCallback,
        void* user)
        : host_ip_(std::move(hostIp))
        , port_(port)
        , frame_callback_(frameCallback)
        , texture_callback_(textureCallback)
        , status_callback_(statusCallback)
        , user_(user)
    {
        worker_ = std::thread([this] { run(); });
    }

    ~ViewerInstance() override
    {
        // =====wjy====
        stop(); // wjy: close sockets and join the worker before viewer members can be freed.
        // ===end====
        if (audio_player_) {
            audio_player_->stop();
        }
    }

    bool sendInput(const char* message) override
    {
        if (!message || !*message) {
            return false;
        }
        std::lock_guard lock(session_mutex_);
        return active_session_ ? active_session_->send_control_message(message) : false;
    }

private:
    uintptr_t connectTcp(std::string* error)
    {
        if (!ensure_wsa()) {
            if (error) *error = "WSAStartup failed";
            return 0;
        }

        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) {
            if (error) *error = "socket failed";
            return 0;
        }

        socket_ = static_cast<uintptr_t>(socket);

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        if (::inet_pton(AF_INET, host_ip_.c_str(), &addr.sin_addr) != 1 ||
            ::connect(socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            const uintptr_t current = socket_.exchange(0);
            if (current) {
                closesocket(static_cast<SOCKET>(current));
            }
            if (running_ && error) *error = "connect failed";
            return 0;
        }

        return static_cast<uintptr_t>(socket);
    }

    void run()
    {
        // =====wjy====
        append_viewer_log("viewer worker begin ip=" + host_ip_ + " port=" + std::to_string(port_)); // wjy: first durable line from the viewer thread.
        // ===end====
        std::string error;
        report_status(status_callback_, user_, 10, "Connecting TCP");
        const uintptr_t socket = connectTcp(&error);
        if (!socket || !running_) {
            append_viewer_log("viewer tcp failed running=" + std::to_string(running_.load()) + " error=" + error); // wjy: separate TCP failure from WebRTC/media crashes.
            if (socket) uu::close_socket(socket);
            report_status(status_callback_, user_, 90, error.empty() ? "TCP connection failed" : error.c_str());
            return;
        }

        append_viewer_log("viewer tcp connected socket=" + std::to_string(socket)); // wjy: mark successful TCP signaling connection before audio/WebRTC start.
        report_status(status_callback_, user_, 20, "TCP connected");
        {
            std::string audio_error;
            audio_player_ = std::make_unique<uu::ViewerAudioPlayer>();
            audio_player_->start(host_ip_, 49105, &audio_error);
            append_viewer_log("viewer audio start error=" + audio_error); // wjy: record audio startup because it runs parallel to video.
        }
        report_status(status_callback_, user_, 30, "Initializing WebRTC");
        uu::NativeWebrtcRuntime runtime;
        std::atomic_bool frameReported = false;
        runtime.set_decoded_texture_callback([this, &frameReported](int width, int height, void* shared_handle, uint64_t frame_id, double encoded_mbps) {
            if (!texture_callback_ || !running_ || !shared_handle) {
                return false;
            }
            bool expected = false;
            if (frameReported.compare_exchange_strong(expected, true)) {
                report_status(status_callback_, user_, 50, "Receiving video");
            }
            const uint64_t now = GetTickCount64();
            if (status_callback_ && now - last_stats_status_ms_ >= 500) {
                last_stats_status_ms_ = now;
                char stats[64] = {};
                std::snprintf(stats, sizeof(stats), "ENC %.2f", encoded_mbps);
                report_status(status_callback_, user_, 60, stats);
            }
            return texture_callback_(user_, width, height, shared_handle, frame_id, encoded_mbps) != 0;
        });
        // =====wjy====
        runtime.set_decoded_bgra_callback([this, &frameReported](int width, int height, const uint8_t* bgra, size_t size, double encoded_mbps) {
            bool expected = false;
            if (frameReported.compare_exchange_strong(expected, true)) {
                report_status(status_callback_, user_, 50, "Receiving video"); // wjy: the first BGRA frame from this runtime means this viewer is receiving video.
            }
            const uint64_t now = GetTickCount64();
            if (status_callback_ && now - last_stats_status_ms_ >= 500) {
                last_stats_status_ms_ = now;
                char stats[64] = {};
                std::snprintf(stats, sizeof(stats), "ENC %.2f", encoded_mbps);
                report_status(status_callback_, user_, 60, stats); // wjy: code 60 carries viewer-side compressed video bitrate for the Qt overlay.
            }
            if (frame_callback_ && running_) {
                frame_callback_(user_, width, height, bgra, static_cast<uint32_t>(size)); // wjy: send only this runtime/decoder's frame to this RemoteDesktopWindow.
            }
        });
        // ===end====
        if (!runtime.initialize(&error)) {
            append_viewer_log("viewer runtime init failed error=" + error); // wjy: durable error if PeerConnectionFactory cannot initialize.
            report_status(status_callback_, user_, 90, error.empty() ? "WebRTC runtime initialization failed" : error.c_str());
            return;
        }
        append_viewer_log("viewer runtime ready"); // wjy: mark native WebRTC runtime creation success.

        uu::SessionConfig config;
        config.role = uu::SessionRole::Viewer;
        uu::WebrtcSession session(&runtime, config);
        {
            std::lock_guard lock(session_mutex_);
            active_session_ = &session;
        }
        session.set_signal_callback([socket](const std::string& kind, const std::string& body) {
            append_viewer_log("viewer send signaling kind=" + kind + " body_size=" + std::to_string(body.size())); // wjy: show local offer/answer/candidate egress.
            uu::send_message(socket, kind + "\n" + body);
        });
        session.set_control_callback([this](const std::string& message) {
            if (message == "__fsremote_mouse_mode relative") {
                report_status(status_callback_, user_, 61, "MOUSE relative");
            } else if (message == "__fsremote_mouse_mode desktop") {
                report_status(status_callback_, user_, 61, "MOUSE desktop");
            }
        });
        if (!session.initialize(&error)) {
            append_viewer_log("viewer session init failed error=" + error); // wjy: durable failure before waiting for remote media.
            {
                std::lock_guard lock(session_mutex_);
                active_session_ = nullptr;
            }
            report_status(status_callback_, user_, 90, error.empty() ? "WebRTC session initialization failed" : error.c_str());
            return;
        }

        append_viewer_log("viewer session initialized waiting stream"); // wjy: last checkpoint before remote signaling/media loop.
        report_status(status_callback_, user_, 40, "Waiting for remote stream");
        std::string message;
        while (running_ && uu::recv_message(socket, &message)) {
            append_viewer_log("viewer recv raw signaling size=" + std::to_string(message.size())); // wjy: each inbound signaling packet before dispatch.
            handle_message(session, message);
        }
        append_viewer_log("viewer recv loop end running=" + std::to_string(running_.load())); // wjy: distinguish clean remote close from native crash.
        {
            std::lock_guard lock(session_mutex_);
            active_session_ = nullptr;
        }
        if (running_) {
            report_status(status_callback_, user_, 80, "Remote connection closed");
        }
        append_viewer_log("viewer worker end"); // wjy: proves the thread exited cleanly.
    }

    std::string host_ip_;
    uint16_t port_ = 49100;
    FsRemoteFrameCallback frame_callback_ = nullptr;
    FsRemoteTextureFrameCallback texture_callback_ = nullptr;
    FsRemoteStatusCallback status_callback_ = nullptr;
    void* user_ = nullptr;
    std::mutex session_mutex_;
    uu::WebrtcSession* active_session_ = nullptr;
    uint64_t last_stats_status_ms_ = 0;
    std::unique_ptr<uu::ViewerAudioPlayer> audio_player_;
};

} // namespace

FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_host(uint16_t port)
{
    // =====wjy====
    install_crash_logger(); // wjy: host also installs the crash logger because FSRemote can host and view in one process.
    append_viewer_log("api start_host port=" + std::to_string(port));
    // ===end====
    try {
        return new HostInstance(port);
    } catch (const std::exception& ex) {
        set_error(ex.what());
        return nullptr;
    }
}

FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_viewer(
    const char* host_ip,
    uint16_t port,
    FsRemoteFrameCallback callback,
    void* user)
{
    // =====wjy====
    install_crash_logger(); // wjy: make sure a viewer-side native crash records exception code/address.
    append_viewer_log(std::string("api start_viewer ip=") + (host_ip ? host_ip : "<null>")
        + " port=" + std::to_string(port));
    // ===end====
    if (!host_ip || !*host_ip || !callback) {
        set_error("invalid viewer arguments");
        return nullptr;
    }

    try {
        return new ViewerInstance(host_ip, port, callback, nullptr, nullptr, user);
    } catch (const std::exception& ex) {
        set_error(ex.what());
        return nullptr;
    }
}

FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_viewer_with_status(
    const char* host_ip,
    uint16_t port,
    FsRemoteFrameCallback frame_callback,
    FsRemoteStatusCallback status_callback,
    void* user)
{
    // =====wjy====
    install_crash_logger(); // wjy: status-enabled viewer is the path used by RemoteDesktopWindow.
    append_viewer_log(std::string("api start_viewer_with_status ip=") + (host_ip ? host_ip : "<null>")
        + " port=" + std::to_string(port));
    // ===end====
    if (!host_ip || !*host_ip || !frame_callback) {
        set_error("invalid viewer arguments");
        report_status(status_callback, user, 90, "Invalid viewer arguments");
        return nullptr;
    }

    try {
        return new ViewerInstance(host_ip, port, frame_callback, nullptr, status_callback, user);
    } catch (const std::exception& ex) {
        set_error(ex.what());
        report_status(status_callback, user, 90, ex.what());
        return nullptr;
    }
}

FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_viewer_with_texture(
    const char* host_ip,
    uint16_t port,
    FsRemoteFrameCallback frame_callback,
    FsRemoteTextureFrameCallback texture_callback,
    FsRemoteStatusCallback status_callback,
    void* user)
{
    install_crash_logger();
    append_viewer_log(std::string("api start_viewer_with_texture ip=") + (host_ip ? host_ip : "<null>")
        + " port=" + std::to_string(port));
    if (!host_ip || !*host_ip || !frame_callback) {
        set_error("invalid texture viewer arguments");
        report_status(status_callback, user, 90, "Invalid texture viewer arguments");
        return nullptr;
    }

    try {
        return new ViewerInstance(host_ip, port, frame_callback, texture_callback, status_callback, user);
    } catch (const std::exception& ex) {
        set_error(ex.what());
        report_status(status_callback, user, 90, ex.what());
        return nullptr;
    }
}

void FSREMOTE_STREAM_CALL fsremote_stream_stop(FsRemoteStreamHandle handle)
{
    // =====wjy====
    append_viewer_log("api stop handle=" + std::to_string(reinterpret_cast<uintptr_t>(handle))); // wjy: show whether the crash happens during explicit stop/close.
    // ===end====
    delete static_cast<StreamInstance*>(handle);
}

int FSREMOTE_STREAM_CALL fsremote_stream_send_input(FsRemoteStreamHandle handle, const char* message)
{
    if (!handle || !message) {
        return 0;
    }
    return static_cast<StreamInstance*>(handle)->sendInput(message) ? 1 : 0;
}

int FSREMOTE_STREAM_CALL fsremote_stream_is_busy(FsRemoteStreamHandle handle)
{
    if (!handle) {
        return 0;
    }
    return static_cast<StreamInstance*>(handle)->isBusy() ? 1 : 0;
}

const char* FSREMOTE_STREAM_CALL fsremote_stream_last_error(void)
{
    return g_last_error.c_str();
}
