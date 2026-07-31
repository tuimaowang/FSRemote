#include "FsRemoteStreamApi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wtsapi32.h>

#include "host_media_pipeline.h"
// =====wjy====
#include "control_admission_policy.h"
#include "faker_input_bridge_client.h" // wjy: FSRemote 仅作为已验证本机桥接服务的客户端，不把驱动访问或网络监听并入主程序。
#include "faker_input_keyboard_state.h" // wjy: 将 Viewer 虚拟键转换成稳定的 USB HID 键盘快照，并集中限制 boot-keyboard 六键上限。
#include "faker_input_runtime_provisioner.h" // wjy: Host 后台校验发布文件、按需静默安装驱动并隐藏启动独立 Bridge，输入线程不等待 MSI。
#include "shared_input_state.h"
// ===end====
#include "native_webrtc_runtime.h"
#include "session_protocol.h"
#include "signaling.h"
#include "system_audio_stream.h"
#include "stream_capture_diagnostics.h"
#include "uu_codec_factory.h"
#include "viewer_quality_protocol.h"
#include "webrtc_session.h"
// =====wjy====
#include "windows_cursor_classifier.h"
// ===end====

#include <atomic>
#include <algorithm>
#include <chrono>
#include <exception>
#include <cstdlib>
#include <cstdio>
// =====wjy====
#include <cctype>
#include <functional>
// ===end====
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

thread_local std::string g_last_error;
std::mutex g_log_mutex;
std::once_flag g_crash_handler_once;
std::mutex g_identity_callbacks_mutex;
FsRemoteIdentityCallbacks g_identity_callbacks = {};

// =====wjy====
struct HostRuntimeConfig {
    uint32_t requested_max_sessions = 3; // wjy: 用户明确要求全新设备无需注册表即可启用三路会话，因此 DLL 无配置入口默认请求 3。
    uint32_t effective_max_sessions = 3; // wjy: 无配置或旧入口直接采用三路有效上限，复制 Release 目录到其他设备即可生效。
    uint32_t max_aggregate_video_kbps = 80000; // wjy: 默认把高质量原始分辨率的单会话发送上限设为80Mbps。
    uint32_t handshake_timeout_ms = 5000; // wjy: 后续认证状态机直接复用该超时配置。
    uint32_t ownership_policy = FSREMOTE_OWNERSHIP_SHARED; // wjy: 默认授予全部已认证 control 会话协同输入权限，无需目标设备额外配置。
};

HostRuntimeConfig normalized_host_config(const FsRemoteHostConfig* config)
{
    HostRuntimeConfig normalized;
    if (!config || config->struct_size < sizeof(FsRemoteHostConfig) || config->version != 1) {
        return normalized; // wjy: 空配置、短结构或未知版本统一回退到新的三会话默认值，避免不同启动入口产生容量差异。
    }
    normalized.requested_max_sessions = std::clamp(config->max_sessions, 1u, 3u);
    normalized.effective_max_sessions = normalized.requested_max_sessions; // wjy: 3.5 实机回归通过后正式启用调用方配置的并发上限，异常值仍已被限制在 1 到 3。
    normalized.max_aggregate_video_kbps = std::clamp(config->max_aggregate_video_kbps, 9000u, 240000u);
    normalized.handshake_timeout_ms = std::clamp(config->handshake_timeout_ms, 1000u, 30000u);
    normalized.ownership_policy = config->ownership_policy == FSREMOTE_OWNERSHIP_EXCLUSIVE
        ? FSREMOTE_OWNERSHIP_EXCLUSIVE
        : FSREMOTE_OWNERSHIP_SHARED; // wjy: 旧调用方仍可显式请求独占；未知值安全归一为产品默认的协同控制策略。
    return normalized;
}
// ===end====

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
    // =====wjy====
    const int virtual_x = ::GetSystemMetrics(SM_XVIRTUALSCREEN); // wjy: 虚拟桌面范围仅保留在日志中，便于确认副屏位于主屏左侧还是右侧。
    const int virtual_y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtual_w = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtual_h = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int screen_w = ::GetSystemMetrics(SM_CXSCREEN); // wjy: 远控画面和绝对注入都使用主屏语义，相对模式探测必须使用同一块主屏的宽高。
    const int screen_h = ::GetSystemMetrics(SM_CYSCREEN);
    const int center_x = screen_w / 2; // wjy: Windows 主屏原点固定为 (0,0)，双屏虚拟桌面中心可能位于两块屏幕交界处，不能用于 FPS 锁鼠判定。
    const int center_y = screen_h / 2;
    const int dx = cursor_ok ? cursor.x - center_x : 0;
    const int dy = cursor_ok ? cursor.y - center_y : 0;

    char text[320] = {};
    std::snprintf(text, sizeof(text),
                  " cursor_ok=%d cursor=%ld,%ld vdesk=%d,%d,%d,%d primary=0,0,%d,%d center=%d,%d dist=%d,%d",
                  cursor_ok ? 1 : 0,
                  static_cast<long>(cursor.x),
                  static_cast<long>(cursor.y),
                  virtual_x,
                  virtual_y,
                  virtual_w,
                  virtual_h,
                  screen_w,
                  screen_h,
                  center_x,
                  center_y,
                  dx,
                  dy);
    // ===end====
    return text;
}

// =====wjy====
void append_viewer_log(const std::string& line)
{
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

// =====wjy====
struct SessionAdmission {
    std::string session_id; // wjy: 每次 TCP 准入生成独立会话 ID，后续控制权和音频凭证都绑定它。
    std::string audio_token; // wjy: 单次短期音频令牌先随准入结果下发，音频中心阶段再强制消费。
    std::string client_id;
    std::string client_device_name; // wjy: 控制端在 hello 中上报的设备名，用于状态气泡展示“谁在控制”。
    std::string client_ip; // wjy: TCP 已连接对端的 IPv4/IPv6 地址，供目标端右下角提示层显示控制端来源。
    std::string public_key;
    std::string host_id; // wjy: 保存签名上下文中的受控设备身份，供 ViewerInstance 后续状态和诊断使用。
    std::string requested_role; // wjy: 保存本次经过验证的请求角色，避免 WebRTC 阶段重新猜测客户端意图。
    std::string capabilities; // wjy: 仅保存主机与客户端都支持的能力交集。
    uint32_t protocol_version = 0; // wjy: 保存实际选中的协议版本，后续消息必须沿用同一版本。
    std::string ownership = "view_only"; // wjy: 默认无输入权限，只有明确请求 control 且认证通过才授予控制权。
    bool exclusive_control_slot = false; // wjy: 仅兼容独占策略时记录本会话是否取得原子槽，协同策略不会占用该槽。
    bool audio_primary = false; // wjy: 单客户端音频重构完成前独立记录首个音频会话，禁止再把音频资格与键鼠权限绑定。
};

struct AudioTokenRecord {
    std::string session_id;
    std::chrono::steady_clock::time_point expires_at;
};

std::mutex g_audio_token_mutex;
std::unordered_map<std::string, AudioTokenRecord> g_audio_tokens;

FsRemoteIdentityCallbacks identity_callbacks_snapshot()
{
    std::lock_guard lock(g_identity_callbacks_mutex);
    return g_identity_callbacks; // wjy: 工作线程使用函数表快照，注册更新不会留下半组回调。
}

std::string random_hex(size_t byte_count)
{
    static thread_local std::mt19937_64 generator(std::random_device{}());
    static constexpr char kHex[] = "0123456789abcdef";
    std::uniform_int_distribution<unsigned int> distribution(0, 255);
    std::string value;
    value.reserve(byte_count * 2);
    for (size_t index = 0; index < byte_count; ++index) {
        const unsigned int byte = distribution(generator);
        value.push_back(kHex[byte >> 4]);
        value.push_back(kHex[byte & 0x0F]);
    }
    return value;
}

std::string local_device_name()
{
    char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = static_cast<DWORD>(std::size(name));
    return ::GetComputerNameA(name, &size) ? std::string(name, size) : std::string("unknown-device");
}

std::string socket_peer_ip(uintptr_t socket)
{
    sockaddr_storage address = {};
    int address_length = sizeof(address);
    if (::getpeername(static_cast<SOCKET>(socket), reinterpret_cast<sockaddr*>(&address), &address_length) != 0) {
        return {};
    }

    char text[INET6_ADDRSTRLEN] = {};
    if (address.ss_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
        return ::InetNtopA(AF_INET, &ipv4->sin_addr, text, static_cast<DWORD>(std::size(text)))
            ? std::string(text)
            : std::string(); // wjy: 控制端通常通过 IPv4 局域网连接，直接保存系统返回的规范文本地址。
    }
    if (address.ss_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
        return ::InetNtopA(AF_INET6, &ipv6->sin6_addr, text, static_cast<DWORD>(std::size(text)))
            ? std::string(text)
            : std::string(); // wjy: 兼容 IPv6 环境；显示层不需要自行判断地址族。
    }
    return {};
}

std::string build_challenge_context(
    const std::string& client_id,
    const std::string& client_nonce,
    const std::string& host_id,
    const std::string& host_nonce,
    uint32_t protocol_version,
    const std::string& requested_role)
{
    return "FSREMOTE_SESSION_PROOF\nversion=" + std::to_string(protocol_version) + "\nclient_id=" + client_id
        + "\nclient_nonce=" + client_nonce
        + "\nhost_id=" + host_id
        + "\nhost_nonce=" + host_nonce
        + "\nrequested_role=" + requested_role + "\n"; // wjy: 签名同时绑定双方随机数、设备身份、版本和请求角色，旧证明不能跨连接重放。
}

bool has_capability(std::string_view list, std::string_view expected)
{
    size_t begin = 0;
    while (begin <= list.size()) {
        const size_t end = list.find(',', begin);
        const std::string_view value = list.substr(begin, end == std::string_view::npos ? list.size() - begin : end - begin);
        if (value == expected) return true; // wjy: 能力按完整逗号分隔项比较，禁止用子串把未知能力误认为已支持能力。
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return false;
}

std::string negotiate_capabilities(std::string_view offered)
{
    std::string negotiated;
    for (const std::string_view capability : {std::string_view("video"), std::string_view("audio"), std::string_view("control")}) {
        if (!has_capability(offered, capability)) continue;
        if (!negotiated.empty()) negotiated.push_back(',');
        negotiated.append(capability); // wjy: 主机只回传双方共同支持且顺序稳定的能力，未知扩展项保持向前兼容但不自动启用。
    }
    return negotiated;
}

bool set_socket_timeout(uintptr_t socket, uint32_t timeout_ms)
{
    const DWORD timeout = timeout_ms;
    return ::setsockopt(static_cast<SOCKET>(socket), SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != SOCKET_ERROR;
}

bool send_session_message(uintptr_t socket, const uu::SessionMessage& message, std::string* error)
{
    std::string wire;
    if (!uu::serialize_session_message(message, &wire, error)) return false;
    if (!uu::send_message(socket, wire)) {
        if (error) *error = "failed to send session message";
        return false;
    }
    return true;
}

enum class SessionReceiveFailure {
    None,
    Timeout,
    UnsupportedVersion,
    Malformed,
    Transport,
};

bool recv_session_message(
    uintptr_t socket,
    uu::SessionMessage* message,
    std::string* error,
    SessionReceiveFailure* failure = nullptr)
{
    if (failure) *failure = SessionReceiveFailure::None;
    std::string wire;
    if (!uu::recv_message(socket, &wire)) {
        const int socket_error = ::WSAGetLastError(); // wjy: 在其它 Winsock 调用覆盖错误码之前立即保存接收失败原因。
        const bool timed_out = socket_error == WSAETIMEDOUT || socket_error == WSAEWOULDBLOCK;
        if (failure) *failure = timed_out ? SessionReceiveFailure::Timeout : SessionReceiveFailure::Transport;
        if (error) *error = timed_out ? "admission handshake timed out" : "failed to receive session message";
        return false;
    }
    if (uu::parse_session_message(wire, message, error)) return true;
    if (failure) {
        *failure = error && *error == "unsupported protocol version"
            ? SessionReceiveFailure::UnsupportedVersion
            : SessionReceiveFailure::Malformed; // wjy: 协议版本错误必须与普通格式错误分开，Viewer 才能得到稳定的不兼容状态。
    }
    return false;
}

void send_admission_rejection(uintptr_t socket, uu::SessionRejectionReason reason, const std::string& detail)
{
    uu::SessionMessage rejected;
    rejected.type = uu::SessionMessageType::AdmissionRejected;
    rejected.fields = {
        {"reason", std::string(uu::session_rejection_reason_name(reason))},
        {"detail", detail.empty() ? "session rejected" : detail},
    };
    std::string ignored;
    send_session_message(socket, rejected, &ignored); // wjy: 尽力返回结构化原因；传输已断开时不覆盖真正的服务端日志错误。
}

void reject_receive_failure(uintptr_t socket, SessionReceiveFailure failure, const std::string& detail)
{
    if (failure == SessionReceiveFailure::Transport) return; // wjy: 对端已断开时没有可用传输，保留本地错误即可。
    const uu::SessionRejectionReason reason = failure == SessionReceiveFailure::Timeout
        ? uu::SessionRejectionReason::Timeout
        : failure == SessionReceiveFailure::UnsupportedVersion
            ? uu::SessionRejectionReason::UnsupportedVersion
            : uu::SessionRejectionReason::Malformed;
    send_admission_rejection(socket, reason, detail); // wjy: 握手超时、版本不兼容和格式错误分别返回稳定拒绝原因。
}

std::string issue_audio_token(const std::string& session_id)
{
    const std::string token = random_hex(32);
    std::lock_guard lock(g_audio_token_mutex);
    const auto now = std::chrono::steady_clock::now();
    for (auto iterator = g_audio_tokens.begin(); iterator != g_audio_tokens.end();) {
        iterator = iterator->second.expires_at <= now ? g_audio_tokens.erase(iterator) : std::next(iterator);
    }
    g_audio_tokens[token] = {session_id, now + std::chrono::seconds(30)}; // wjy: 音频连接必须在准入后 30 秒内使用，过期记录会在签发/消费时清理。
    return token;
}

bool consume_audio_token(const std::string& session_id, const std::string& token)
{
    std::lock_guard lock(g_audio_token_mutex);
    const auto iterator = g_audio_tokens.find(token);
    if (iterator == g_audio_tokens.end()) return false;
    const bool valid = iterator->second.session_id == session_id
        && iterator->second.expires_at > std::chrono::steady_clock::now();
    g_audio_tokens.erase(iterator); // wjy: 无论有效与否都销毁已提交令牌，阻止同一令牌反复试探或重放。
    return valid;
}

bool read_identity_public_key(const FsRemoteIdentityCallbacks& callbacks, std::string* public_key)
{
    if (!public_key || !callbacks.read_public_key) return false;
    std::vector<uint8_t> buffer(16 * 1024);
    const uint32_t size = callbacks.read_public_key(callbacks.user, buffer.data(), static_cast<uint32_t>(buffer.size()));
    if (size == 0 || size > buffer.size()) return false;
    public_key->assign(reinterpret_cast<const char*>(buffer.data()), size);
    return true;
}

bool sign_identity_challenge(const FsRemoteIdentityCallbacks& callbacks, const std::string& challenge, std::string* signature)
{
    if (!signature || !callbacks.sign_challenge) return false;
    std::vector<uint8_t> buffer(16 * 1024);
    const uint32_t size = callbacks.sign_challenge(
        callbacks.user,
        reinterpret_cast<const uint8_t*>(challenge.data()),
        static_cast<uint32_t>(challenge.size()),
        buffer.data(),
        static_cast<uint32_t>(buffer.size()));
    if (size == 0 || size > buffer.size()) return false;
    signature->assign(reinterpret_cast<const char*>(buffer.data()), size);
    return true;
}

bool perform_host_admission(
    uintptr_t socket,
    const HostRuntimeConfig& config,
    std::atomic_bool* exclusive_control_claimed,
    std::atomic_bool* audio_primary_claimed,
    SessionAdmission* admission,
    std::string* error)
{
    if (!admission) return false;
    set_socket_timeout(socket, config.handshake_timeout_ms);
    const FsRemoteIdentityCallbacks callbacks = identity_callbacks_snapshot();
    if (!callbacks.is_public_key_authorized || !callbacks.verify_challenge) {
        if (error) *error = "identity callbacks unavailable";
        send_admission_rejection(socket, uu::SessionRejectionReason::Policy, "identity callbacks unavailable");
        return false;
    }

    uu::SessionMessage hello;
    SessionReceiveFailure receive_failure = SessionReceiveFailure::None;
    if (!recv_session_message(socket, &hello, error, &receive_failure)) {
        reject_receive_failure(socket, receive_failure, error ? *error : "failed to receive client hello");
        return false;
    }
    if (hello.type != uu::SessionMessageType::ClientHello) {
        if (error) *error = "expected client hello";
        send_admission_rejection(socket, uu::SessionRejectionReason::Malformed, "expected client hello");
        return false;
    }
    const std::string& requested_role = hello.fields.at("requested_role");
    const std::string negotiated_capabilities = negotiate_capabilities(hello.fields.at("capabilities"));
    if (!has_capability(negotiated_capabilities, "video")
        || (requested_role == "control" && !has_capability(negotiated_capabilities, "control"))) {
        if (error) *error = "required session capabilities are unavailable";
        send_admission_rejection(socket, uu::SessionRejectionReason::Policy, "required session capabilities are unavailable");
        return false; // wjy: 没有视频能力或请求控制却不支持控制协议时，在授权和媒体初始化前按策略拒绝。
    }
    const std::string& public_key = hello.fields.at("public_key");
    if (!callbacks.is_public_key_authorized(callbacks.user,
            reinterpret_cast<const uint8_t*>(public_key.data()), static_cast<uint32_t>(public_key.size()))) {
        if (error) *error = "client public key is not authorized";
        send_admission_rejection(socket, uu::SessionRejectionReason::Unauthorized, "client public key is not authorized");
        return false; // wjy: 授权检查失败时尚未创建 PeerConnection、采集器、编码器或音频客户端。
    }

    const std::string host_id = local_device_name();
    const std::string host_nonce = random_hex(32);
    const std::string challenge_context = build_challenge_context(
        hello.fields.at("client_id"), hello.fields.at("client_nonce"), host_id, host_nonce, hello.version, requested_role);
    uu::SessionMessage challenge;
    challenge.type = uu::SessionMessageType::ServerChallenge;
    challenge.fields = {
        {"host_id", host_id},
        {"host_nonce", host_nonce},
        {"selected_version", std::to_string(uu::kSessionProtocolVersion)},
        {"challenge_context", challenge_context},
    };
    if (!send_session_message(socket, challenge, error)) return false;

    uu::SessionMessage proof;
    if (!recv_session_message(socket, &proof, error, &receive_failure)) {
        reject_receive_failure(socket, receive_failure, error ? *error : "failed to receive client proof");
        return false;
    }
    if (proof.type != uu::SessionMessageType::ClientProof
        || proof.fields.at("client_id") != hello.fields.at("client_id")) {
        if (error) *error = "invalid client proof";
        send_admission_rejection(socket, uu::SessionRejectionReason::Malformed, "invalid client proof");
        return false;
    }
    const std::string& signature = proof.fields.at("signature");
    if (!callbacks.verify_challenge(callbacks.user,
            reinterpret_cast<const uint8_t*>(public_key.data()), static_cast<uint32_t>(public_key.size()),
            reinterpret_cast<const uint8_t*>(challenge_context.data()), static_cast<uint32_t>(challenge_context.size()),
            reinterpret_cast<const uint8_t*>(signature.data()), static_cast<uint32_t>(signature.size()))) {
        if (error) *error = "session signature verification failed";
        send_admission_rejection(socket, uu::SessionRejectionReason::Unauthorized, "session signature verification failed");
        return false;
    }

    if (admission->session_id.empty()) admission->session_id = random_hex(16); // wjy: HostSessionManager 可在启动工作线程前预留稳定 ID，旧调用方仍由握手生成。
    // =====wjy====
    const uu::ControlAdmissionDecision control_decision = uu::decideControlAdmission(
        requested_role == "control",
        has_capability(negotiated_capabilities, "control"),
        config.ownership_policy == FSREMOTE_OWNERSHIP_EXCLUSIVE,
        exclusive_control_claimed); // wjy: 认证完成后统一由可测试策略决定本会话是协同控制还是只读。
    const bool control_granted = control_decision.granted;
    admission->exclusive_control_slot = control_decision.claimed_exclusive_slot;
    if (control_granted && has_capability(negotiated_capabilities, "audio") && audio_primary_claimed) {
        bool expected = false;
        admission->audio_primary = audio_primary_claimed->compare_exchange_strong(expected, true); // wjy: 音频仍是单客户端实现，因此单独选出首个音频会话，不再限制其他会话的控制权限。
    }
    std::string admitted_capabilities = "video";
    if (admission->audio_primary) admitted_capabilities += ",audio";
    if (control_granted) admitted_capabilities += ",control"; // wjy: 后续协同控制端得到 video,control；只有音频主会话额外得到 audio。
    admission->audio_token = issue_audio_token(admission->session_id); // wjy: AdmissionAccepted 协议要求令牌非空；只读会话也取得独立短期令牌，但因 capabilities 不含 audio，Viewer 不会连接音频端口。
    // ===end====
    admission->client_id = hello.fields.at("client_id");
    admission->client_device_name = hello.fields.count("device_name") ? hello.fields.at("device_name") : std::string();
    admission->public_key = public_key;
    admission->host_id = host_id;
    admission->requested_role = requested_role;
    admission->capabilities = admitted_capabilities; // wjy: 把服务端最终授予的能力返回给 Viewer，不能沿用客户端自行声明的能力集合。
    admission->protocol_version = hello.version;
    admission->ownership = control_granted ? "control_granted" : "view_only"; // wjy: control_granted 在共享策略中表示“本会话具有协同控制权限”，不再表示全局唯一拥有者。
    uu::SessionMessage accepted;
    accepted.type = uu::SessionMessageType::AdmissionAccepted;
    accepted.fields = {
        {"session_id", admission->session_id},
        {"ownership", admission->ownership},
        {"capabilities", admission->capabilities},
        {"audio_token", admission->audio_token},
    };
    if (!send_session_message(socket, accepted, error)) return false;
    set_socket_timeout(socket, 0); // wjy: 准入完成后恢复阻塞信令读取，避免正常长会话因握手超时值被断开。
    return true;
}

bool perform_viewer_admission(
    uintptr_t socket,
    FsRemoteStatusCallback status_callback,
    void* user,
    SessionAdmission* admission,
    std::string* error)
{
    if (!admission) return false;
    const FsRemoteIdentityCallbacks callbacks = identity_callbacks_snapshot();
    std::string public_key;
    if (!read_identity_public_key(callbacks, &public_key)) {
        if (error) *error = "failed to read client identity";
        report_status(status_callback, user, FSREMOTE_STATUS_AUTHORIZATION_REJECTED, "Client identity unavailable");
        return false;
    }
    const std::string client_id = random_hex(16);
    const std::string client_nonce = random_hex(32);
    const std::string requested_role = "control";
    const std::string offered_capabilities = "video,audio,control";
    uu::SessionMessage hello;
    hello.type = uu::SessionMessageType::ClientHello;
    hello.fields = {
        {"client_id", client_id},
        {"device_name", local_device_name()},
        {"requested_role", requested_role},
        {"public_key", public_key},
        {"client_nonce", client_nonce},
        {"capabilities", offered_capabilities},
    };
    if (!send_session_message(socket, hello, error)) return false;

    uu::SessionMessage challenge;
    if (!recv_session_message(socket, &challenge, error)) {
        const char* detail = error && !error->empty() ? error->c_str() : "Failed to receive admission challenge"; // wjy: 传输超时或主机提前关闭时提供可见错误，不再让窗口永久停在 TCP 已连接。
        report_status(status_callback, user, FSREMOTE_STATUS_ERROR, detail); // wjy: 准入第一阶段失败立即覆盖最后一次 TCP 状态，便于定位服务端握手问题。
        return false;
    }
    if (challenge.type == uu::SessionMessageType::AdmissionRejected) {
        const std::string reason = challenge.fields.at("reason");
        const int code = reason == "unsupported_version" ? FSREMOTE_STATUS_INCOMPATIBLE_PROTOCOL
            : reason == "capacity" ? FSREMOTE_STATUS_CAPACITY_REJECTED
            : FSREMOTE_STATUS_AUTHORIZATION_REJECTED;
        report_status(status_callback, user, code, challenge.fields.at("detail").c_str());
        if (error) *error = challenge.fields.at("detail");
        return false;
    }
    if (challenge.type != uu::SessionMessageType::ServerChallenge) {
        if (error) *error = "expected server challenge";
        return false;
    }
    const std::string expected_context = build_challenge_context(
        client_id,
        client_nonce,
        challenge.fields.at("host_id"),
        challenge.fields.at("host_nonce"),
        uu::kSessionProtocolVersion,
        requested_role);
    if (challenge.fields.at("selected_version") != std::to_string(uu::kSessionProtocolVersion)
        || challenge.fields.at("challenge_context") != expected_context) {
        if (error) *error = "server challenge context mismatch";
        report_status(status_callback, user, FSREMOTE_STATUS_INCOMPATIBLE_PROTOCOL, "Server challenge mismatch");
        return false; // wjy: Viewer 只签名自己可重算的上下文，恶意主机不能替换角色、客户端随机数或协议版本。
    }

    std::string signature;
    if (!sign_identity_challenge(callbacks, expected_context, &signature)) {
        if (error) *error = "failed to sign session challenge";
        report_status(status_callback, user, FSREMOTE_STATUS_AUTHORIZATION_REJECTED, "Failed to sign session challenge");
        return false;
    }
    uu::SessionMessage proof;
    proof.type = uu::SessionMessageType::ClientProof;
    proof.fields = {{"client_id", client_id}, {"signature", signature}};
    if (!send_session_message(socket, proof, error)) return false;

    uu::SessionMessage accepted;
    if (!recv_session_message(socket, &accepted, error)) {
        const char* detail = error && !error->empty() ? error->c_str() : "Failed to receive admission result"; // wjy: 已发送证明却收不到最终结果时显示真实传输错误。
        report_status(status_callback, user, FSREMOTE_STATUS_ERROR, detail); // wjy: 防止第二会话准入回包失败后 UI 仍误显示 TCP 已连接。
        return false;
    }
    if (accepted.type == uu::SessionMessageType::AdmissionRejected) {
        report_status(status_callback, user, FSREMOTE_STATUS_AUTHORIZATION_REJECTED, accepted.fields.at("detail").c_str());
        if (error) *error = accepted.fields.at("detail");
        return false;
    }
    if (accepted.type != uu::SessionMessageType::AdmissionAccepted) {
        if (error) *error = "expected admission result";
        return false;
    }
    admission->session_id = accepted.fields.at("session_id");
    admission->audio_token = accepted.fields.at("audio_token");
    admission->client_id = client_id;
    admission->public_key = public_key;
    admission->host_id = challenge.fields.at("host_id");
    admission->requested_role = requested_role;
    admission->capabilities = accepted.fields.at("capabilities");
    admission->protocol_version = uu::kSessionProtocolVersion;
    admission->ownership = accepted.fields.at("ownership");
    report_status(status_callback, user, FSREMOTE_STATUS_ADMITTED, "Session admitted");
    report_status(status_callback, user,
        admission->ownership == "control_granted" ? FSREMOTE_STATUS_CONTROL_GRANTED : FSREMOTE_STATUS_VIEW_ONLY,
        admission->ownership == "control_granted" ? "Control granted" : "View only");
    return true;
}
// ===end====

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

// =====wjy====
constexpr DWORD kCapturedPrimaryAbsoluteMouseFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE; // wjy: 远控视频只采集被设为主屏的 Parsec 显示器，绝对鼠标必须使用主屏坐标语义。
static_assert((kCapturedPrimaryAbsoluteMouseFlags & MOUSEEVENTF_VIRTUALDESK) == 0,
    "single-source remote input must not target the whole virtual desktop"); // wjy: 编译期回归保护，防止以后再次把单屏画面坐标扩展到多显示器虚拟桌面。
// ===end====

void move_mouse_absolute(int x, int y, bool log_result)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.dwFlags = kCapturedPrimaryAbsoluteMouseFlags; // wjy: 统一使用受编译期约束的主屏注入标志，避免多显示器设备把鼠标移动到远控画面之外。
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

// =====wjy====
void send_mouse_button(DWORD flag); // wjy: 后端路由器失败回退时复用下方现有 SendInput 按钮实现。
void send_mouse_wheel(int delta); // wjy: FakerInputBridge 断管后，同一个滚轮事件立即交给系统注入，避免用户操作丢失。
void send_key(int vk, bool down); // wjy: 驱动键盘不支持的 Consumer 键及运行时回退继续复用既有系统键盘实现。

enum class CurrentSessionLockState {
    Unknown,
    Locked,
    Unlocked,
}; // wjy: 锁屏自动 Enter 只接受 WTS 明确返回的锁定状态，查询失败或未知值绝不注入。

struct CurrentSessionLockQuery {
    CurrentSessionLockState state = CurrentSessionLockState::Unknown;
    DWORD session_id = 0;
    DWORD session_flags = 0;
    DWORD error = ERROR_SUCCESS;
};

CurrentSessionLockQuery query_current_session_lock_state()
{
    CurrentSessionLockQuery result;
    if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &result.session_id)) {
        result.error = ::GetLastError();
        return result; // wjy: 只查询承载 FSRemote Host 的真实会话，禁止误操作其他登录用户的锁屏桌面。
    }

    LPWSTR raw_information = nullptr;
    DWORD information_bytes = 0;
    if (!::WTSQuerySessionInformationW(
            WTS_CURRENT_SERVER_HANDLE,
            result.session_id,
            WTSSessionInfoEx,
            &raw_information,
            &information_bytes)) {
        result.error = ::GetLastError();
        return result;
    }

    if (raw_information && information_bytes >= sizeof(WTSINFOEXW)) {
        const auto* information = reinterpret_cast<const WTSINFOEXW*>(raw_information);
        if (information->Level == 1) {
            result.session_flags = information->Data.WTSInfoExLevel1.SessionFlags;
            if (result.session_flags == WTS_SESSIONSTATE_LOCK) {
                result.state = CurrentSessionLockState::Locked;
            } else if (result.session_flags == WTS_SESSIONSTATE_UNLOCK) {
                result.state = CurrentSessionLockState::Unlocked;
            }
        }
    } else {
        result.error = ERROR_INVALID_DATA;
    }
    ::WTSFreeMemory(raw_information); // wjy: WTS 查询缓冲区必须由配对 API 释放，Host 长期运行时不能按远控次数累积泄漏。
    return result;
}

enum class MouseInjectionBackend {
    System,
    Faker,
}; // wjy: 注入后端与既有 desktop/relative 捕获模式是两条独立状态轴，禁止互相覆盖。

const char* mouse_backend_token(MouseInjectionBackend backend)
{
    return backend == MouseInjectionBackend::Faker ? "faker" : "system";
}

DWORD system_mouse_button_flag(int button, bool down)
{
    if (button == 1) return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    if (button == 2) return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    if (button == 4) return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    return 0; // wjy: 当前 Viewer 协议只允许左、右、中三种按钮，未知值绝不生成系统或 HID 报告。
}

std::uint8_t faker_mouse_button_bit(int button)
{
    if (button == 1) return 0x01;
    if (button == 2) return 0x02;
    if (button == 4) return 0x04;
    return 0;
}

class MouseInputBackendRouter final {
public:
    struct OneShotEnterAttempt {
        bool completed = false;
        bool success = false;
        std::string error;
    }; // wjy: completed 区分“尚未触碰键盘、可以稍后重连”和“已经开始一次按键、绝不允许重复”。

    MouseInjectionBackend backend() const noexcept
    {
        return backend_;
    }

    bool selectFaker(std::string* error)
    {
        if (backend_ == MouseInjectionBackend::Faker) return true;
        std::string bridge_error;
        if (!bridge_.connectAndPing(&bridge_error)) {
            backend_ = MouseInjectionBackend::System; // wjy: 服务未启动、驱动未就绪或协议不匹配时，真实键鼠后端始终保持系统 SendInput。
            button_mask_ = 0;
            keyboard_state_.clear(); // wjy: 连接失败不得保留准备阶段的虚拟按键镜像，系统后端从空状态继续接收新输入。
            if (error) *error = bridge_error;
            append_input_debug_log("host FakerInputBridge enable failed error=" + bridge_error);
            return false;
        }
        backend_ = MouseInjectionBackend::Faker; // wjy: 只有本机管道完成 ping 且驱动 ready 后才对所有 Viewer 公布驱动模式。
        button_mask_ = 0;
        keyboard_state_.clear(); // wjy: 调用方已在旧后端释放共享键鼠，驱动会话必须从全零 HID 快照开始。
        return true;
    }

    void selectSystem()
    {
        if (backend_ == MouseInjectionBackend::Faker) {
            std::string bridge_error;
            if (!bridge_.releaseAll(&bridge_error)) {
                append_input_debug_log("host FakerInputBridge release before system failed error=" + bridge_error); // wjy: 断管也会触发桥接服务端 release-all，因此这里只记录而不阻止安全回退。
            }
            bridge_.close(); // wjy: 系统模式不长期占用命名管道，服务端可明确观察到客户端已退出驱动输入会话。
        }
        backend_ = MouseInjectionBackend::System;
        button_mask_ = 0;
        keyboard_state_.clear(); // wjy: Bridge 的 release-all 已清空虚拟键盘，路由器镜像同步归零以防下一次误抬旧键。
    }

    void moveAbsolute(int x, int y, bool log_result)
    {
        if (backend_ == MouseInjectionBackend::Faker) {
            const auto hid_x = static_cast<std::uint16_t>(
                (static_cast<std::int64_t>(std::clamp(x, 0, 65535)) * 32767 + 32767) / 65535); // wjy: Viewer 的 0..65535 坐标按四舍五入缩放为 FakerInput 绝对鼠标的 0..32767。
            const auto hid_y = static_cast<std::uint16_t>(
                (static_cast<std::int64_t>(std::clamp(y, 0, 65535)) * 32767 + 32767) / 65535);
            std::string bridge_error;
            if (bridge_.sendAbsoluteMouse(0, hid_x, hid_y, 0, &bridge_error)) return; // wjy: 绝对与相对鼠标是两个 HID collection，按钮只由相对 collection 持有，禁止在拖拽时形成第二份无法同步抬起的状态。
            fallbackToSystem(bridge_error); // wjy: 当前报告失败就转回 SendInput，并在本次调用内补发同一个移动事件。
        }
        move_mouse_absolute(x, y, log_result);
    }

    void moveRelative(int dx, int dy, bool log_result)
    {
        if (dx == 0 && dy == 0) return;
        if (backend_ == MouseInjectionBackend::Faker) {
            std::string bridge_error;
            if (bridge_.sendRelativeMouse(
                    button_mask_,
                    static_cast<std::int16_t>(std::clamp(dx, -32768, 32767)),
                    static_cast<std::int16_t>(std::clamp(dy, -32768, 32767)),
                    0,
                    0,
                    &bridge_error)) {
                return;
            }
            fallbackToSystem(bridge_error); // wjy: 相对视角移动也遵循同事件回退，游戏不会因桥接服务退出而完全失去鼠标。
        }
        move_mouse_relative(dx, dy, log_result);
    }

    void sendButton(int button, bool down)
    {
        const std::uint8_t bit = faker_mouse_button_bit(button);
        const DWORD system_flag = system_mouse_button_flag(button, down);
        if (bit == 0 || system_flag == 0) return;
        if (down) {
            button_mask_ |= bit; // wjy: FakerInput HID 报告携带完整按钮快照，先更新目标状态再发送本次 down。
        } else {
            button_mask_ &= static_cast<std::uint8_t>(~bit); // wjy: 抬起只清当前按钮，其他控制端仍持有的按钮继续保留在快照中。
        }
        if (backend_ == MouseInjectionBackend::Faker) {
            std::string bridge_error;
            if (bridge_.sendRelativeMouse(button_mask_, 0, 0, 0, 0, &bridge_error)) return;
            fallbackToSystem(bridge_error); // wjy: 回退函数会按最新按钮掩码在 SendInput 侧重建仍应按住的按钮，无需重复注入当前 down/up。
            return;
        }
        send_mouse_button(system_flag);
    }

    void sendKey(int virtualKey, bool down)
    {
        const uu::FakerInputKeyboardUpdate update = keyboard_state_.update(virtualKey, down); // wjy: 系统与驱动后端都维护同一逻辑镜像，断管时才能重建仍按住的键。
        if (backend_ != MouseInjectionBackend::Faker) {
            send_key(virtualKey, down); // wjy: 系统模式保持原始 SendInput 重复按键语义，不因新增 HID 状态机改变桌面行为。
            return;
        }
        if (update == uu::FakerInputKeyboardUpdate::Unsupported) {
            send_key(virtualKey, down); // wjy: 音量、浏览器等当前 FakerInput 键盘 collection 无法表达的键单独保留系统兼容路径。
            return;
        }
        if (update == uu::FakerInputKeyboardUpdate::Rollover) {
            append_input_debug_log("host FakerInput keyboard dropped reason=6-key-rollover vk=" + std::to_string(virtualKey)); // wjy: 第七个普通键不破坏已按下六键，也不触发整套后端突然回退。
            return;
        }
        if (update == uu::FakerInputKeyboardUpdate::Unchanged) {
            return; // wjy: 驱动保持键按下后由目标 Windows 产生重复字符，重复写相同快照不会形成新的物理边沿。
        }

        const uu::FakerInputKeyboardReport report = keyboard_state_.report();
        std::string bridge_error;
        if (bridge_.sendKeyboard(report.modifiers, report.usages, &bridge_error)) return; // wjy: 每次 down/up 都发送当前完整快照，断线重连不依赖服务端保存增量事件。
        fallbackToSystem(bridge_error); // wjy: 当前键已进入逻辑镜像，回退函数会一次性用 SendInput 重建所有仍按住的键。
    }

    OneShotEnterAttempt sendOneShotEnter()
    {
        OneShotEnterAttempt attempt;
        if (!keyboard_state_.pressedVirtualKeys().empty()) {
            attempt.error = "FakerInput keyboard is not idle";
            return attempt; // wjy: 其他控制端仍按住修饰键或普通键时暂缓，避免自动 Enter 变成 Alt+Enter 等组合键。
        }

        uu::FakerInputBridgeClient temporary_bridge;
        uu::FakerInputBridgeClient* bridge = &bridge_;
        const bool uses_persistent_bridge = backend_ == MouseInjectionBackend::Faker;
        std::string bridge_error;
        if (uses_persistent_bridge) {
            if (!bridge_.connectAndPing(&bridge_error)) {
                fallbackToSystem(bridge_error); // wjy: 长连接已失效时先回到系统后端，本次尚未写键盘，后续可重新连接 Bridge 再尝试。
                attempt.error = bridge_error;
                return attempt;
            }
        } else {
            bridge = &temporary_bridge; // wjy: 系统键鼠模式不改变用户选择，只为锁屏 Enter 短暂占用本机虚拟 HID 管道。
            if (!bridge->connectAndPing(&bridge_error)) {
                attempt.error = bridge_error;
                return attempt;
            }
        }

        std::array<std::uint8_t, 6> enter_down{};
        enter_down[0] = uu::fakerInputUsageForVirtualKey(VK_RETURN); // wjy: 只发送标准 USB HID Enter usage 0x28，不组合密码、SAS、鼠标或其他键。
        attempt.completed = true; // wjy: 从第一次键盘请求开始即熔断重试；即使响应丢失，也不能冒险再产生第二次 Enter。
        if (!bridge->sendKeyboard(0, enter_down, &bridge_error)) {
            if (uses_persistent_bridge) fallbackToSystem(bridge_error);
            attempt.error = bridge_error;
            return attempt;
        }

        ::Sleep(60); // wjy: 保持一个短而完整的物理按键边沿，随后无条件发送全零键盘快照抬起 Enter。
        const std::array<std::uint8_t, 6> enter_up{};
        if (!bridge->sendKeyboard(0, enter_up, &bridge_error)) {
            if (uses_persistent_bridge) fallbackToSystem(bridge_error); // wjy: 断管时服务端会 release-all，系统回退也从空键盘镜像开始。
            attempt.error = bridge_error;
            return attempt;
        }

        attempt.success = true;
        return attempt;
    }

    void sendWheel(int delta)
    {
        if (delta == 0) return;
        if (backend_ == MouseInjectionBackend::Faker) {
            int wheel_steps = delta / WHEEL_DELTA;
            if (wheel_steps == 0) wheel_steps = delta > 0 ? 1 : -1; // wjy: 高精度设备不足 120 的非零滚动仍转换成一个 HID 滚轮刻度。
            wheel_steps = std::clamp(wheel_steps, -127, 127);
            std::string bridge_error;
            if (bridge_.sendRelativeMouse(
                    button_mask_, 0, 0, static_cast<std::int8_t>(wheel_steps), 0, &bridge_error)) {
                return;
            }
            fallbackToSystem(bridge_error);
        }
        send_mouse_wheel(delta); // wjy: 系统后端保留原始 WHEEL_DELTA 数值；驱动失败时也补发原事件而不是缩放后的 HID 刻度。
    }

    bool consumeFallback(std::string* error)
    {
        if (!fallback_pending_) return false;
        fallback_pending_ = false;
        if (error) *error = fallback_error_;
        fallback_error_.clear();
        return true; // wjy: 每次断管只广播一次 system fallback，后续 SendInput 事件不会重复刷状态栏。
    }

private:
    void fallbackToSystem(const std::string& bridge_error)
    {
        bridge_.close(); // wjy: 关闭管道触发服务端 release-all，先清除虚拟 HID 键鼠状态再在系统后端重建仍按住的输入。
        backend_ = MouseInjectionBackend::System;
        for (const int virtualKey : keyboard_state_.pressedVirtualKeys()) {
            send_key(virtualKey, true); // wjy: 驱动报告失败后按原 VK 重建当前键盘 down，WASD/修饰键不会在回退瞬间全部丢失。
        }
        if ((button_mask_ & 0x01) != 0) send_mouse_button(MOUSEEVENTF_LEFTDOWN);
        if ((button_mask_ & 0x02) != 0) send_mouse_button(MOUSEEVENTF_RIGHTDOWN);
        if ((button_mask_ & 0x04) != 0) send_mouse_button(MOUSEEVENTF_MIDDLEDOWN); // wjy: 只重建仍处于逻辑按下的按钮，已经抬起的按钮不会在系统后端复活。
        fallback_pending_ = true;
        fallback_error_ = bridge_error;
        append_input_debug_log("host FakerInputBridge runtime fallback error=" + bridge_error);
    }

    MouseInjectionBackend backend_ = MouseInjectionBackend::System;
    uu::FakerInputBridgeClient bridge_; // wjy: 单个 Host 调度器长期复用一个本机管道连接，禁止为每个鼠标报告反复连接。
    uu::FakerInputKeyboardState keyboard_state_; // wjy: 与鼠标共用同一有序管道，键盘报告不会越过按钮或位移报告乱序到达驱动。
    std::uint8_t button_mask_ = 0;
    bool fallback_pending_ = false;
    std::string fallback_error_;
};
// ===end====

// =====wjy====
struct MouseInputModeState {
    bool game_relative_mode = false;
    int lock_score = 0;
    int unlock_score = 0;
    uint64_t relative_generation = 0; // wjy: 每次进入相对模式递增，保证每个控制端都先建立自己的坐标基线。
};

MouseInputModeState g_mouse_input_mode;

struct SessionPointerState {
    bool has_last_viewer_pos = false;
    int last_viewer_x = 0;
    int last_viewer_y = 0;
    uint64_t relative_generation = 0;
}; // wjy: 相对鼠标坐标必须按会话隔离，否则两个控制端交替移动会把彼此的绝对坐标误算成巨大位移。

// =====wjy====
std::optional<uu::StandardCursorShape> standard_cursor_shape_from_handle(HCURSOR cursor)
{
    if (!cursor) return std::nullopt; // wjy: 隐藏或暂不可读的光标没有句柄，交给窗口边缘命中回退。
    if (cursor == ::LoadCursorW(nullptr, IDC_ARROW)) return uu::StandardCursorShape::Arrow;
    if (cursor == ::LoadCursorW(nullptr, IDC_IBEAM)) return uu::StandardCursorShape::IBeam;
    if (cursor == ::LoadCursorW(nullptr, IDC_WAIT)) return uu::StandardCursorShape::Wait;
    if (cursor == ::LoadCursorW(nullptr, IDC_APPSTARTING)) return uu::StandardCursorShape::Busy;
    if (cursor == ::LoadCursorW(nullptr, IDC_CROSS)) return uu::StandardCursorShape::Cross;
    if (cursor == ::LoadCursorW(nullptr, IDC_HAND)) return uu::StandardCursorShape::Hand;
    if (cursor == ::LoadCursorW(nullptr, IDC_NO)) return uu::StandardCursorShape::Forbidden;
    if (cursor == ::LoadCursorW(nullptr, IDC_HELP)) return uu::StandardCursorShape::Help;
    if (cursor == ::LoadCursorW(nullptr, IDC_UPARROW)) return uu::StandardCursorShape::UpArrow;
    if (cursor == ::LoadCursorW(nullptr, IDC_SIZEALL)) return uu::StandardCursorShape::SizeAll;
    if (cursor == ::LoadCursorW(nullptr, IDC_SIZEWE)) return uu::StandardCursorShape::SizeHorizontal; // wjy: Windows 左右双箭头映射 Qt 横向缩放。
    if (cursor == ::LoadCursorW(nullptr, IDC_SIZENS)) return uu::StandardCursorShape::SizeVertical; // wjy: Windows 上下双箭头映射 Qt 纵向缩放。
    if (cursor == ::LoadCursorW(nullptr, IDC_SIZENWSE)) return uu::StandardCursorShape::SizeNorthwestSoutheast; // wjy: 左上到右下对角线单独编码，禁止与另一方向混淆。
    if (cursor == ::LoadCursorW(nullptr, IDC_SIZENESW)) return uu::StandardCursorShape::SizeNortheastSouthwest;
    return std::nullopt; // wjy: 自定义句柄不再提前降级为箭头，先尝试由窗口的标准边框语义补齐缩放形状。
}

std::optional<uu::StandardCursorShape> standard_resize_cursor_shape_at_pointer()
{
    POINT point = {};
    if (!::GetCursorPos(&point)) {
        return std::nullopt;
    }

    HWND window = ::WindowFromPoint(point);
    if (!window) {
        return std::nullopt;
    }
    if (HWND root = ::GetAncestor(window, GA_ROOT)) {
        window = root; // wjy: 子控件通常只返回 HTCLIENT，必须询问所属顶层窗口才能得到真实非客户区缩放边缘。
    }

    DWORD_PTR raw_hit_test = 0;
    const LPARAM screen_position = MAKELPARAM(
        static_cast<WORD>(point.x),
        static_cast<WORD>(point.y)); // wjy: WM_NCHITTEST 按 Windows 约定接收两个有符号 16 位屏幕坐标，负数多屏坐标保留其补码位。
    constexpr UINT kHitTestTimeoutMs = 12;
    if (!::SendMessageTimeoutW(
            window,
            WM_NCHITTEST,
            0,
            screen_position,
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
            kHitTestTimeoutMs,
            &raw_hit_test)) {
        return std::nullopt; // wjy: 目标 GUI 线程卡死、退出或被权限隔离时快速回退，不阻塞 Host 状态线程。
    }

    return uu::standard_resize_cursor_shape_from_hit_test(static_cast<LRESULT>(raw_hit_test)); // wjy: 只接受八个标准可缩放边缘，标题栏和客户区继续保持实际光标。
}

uu::StandardCursorShape current_standard_cursor_shape()
{
    CURSORINFO info = {};
    info.cbSize = sizeof(info);
    const bool has_visible_cursor = ::GetCursorInfo(&info)
        && (info.flags & CURSOR_SHOWING)
        && info.hCursor; // wjy: 隐藏、读取失败和空句柄统一进入有界窗口命中回退。
    const std::optional<uu::StandardCursorShape> handle_shape = has_visible_cursor
        ? standard_cursor_shape_from_handle(info.hCursor)
        : std::nullopt;
    if (handle_shape.has_value() && *handle_shape != uu::StandardCursorShape::Arrow) {
        return *handle_shape; // wjy: 已得到文本、手型或标准缩放句柄时直接使用，避免无意义的跨进程窗口消息。
    }
    if (const auto resize_shape = standard_resize_cursor_shape_at_pointer()) {
        return *resize_shape; // wjy: 箭头、隐藏或自定义句柄仍可依据 Windows 非客户区语义显示正确双箭头。
    }
    return handle_shape.value_or(uu::StandardCursorShape::Arrow); // wjy: 命中查询失败或位于普通客户区时安全回退普通箭头。
}
// ===end====

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

    // =====wjy====
    screen_w = ::GetSystemMetrics(SM_CXSCREEN); // wjy: 系统与 FakerInput 后端共用这里的探测结果；主屏宽高与控制端 0..65535 坐标、主屏绝对注入保持一致。
    screen_h = ::GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 0 || screen_h <= 0) {
        dx = 0;
        dy = 0;
        return false; // wjy: 系统未返回有效主屏尺寸时禁止误判相对模式，继续保留桌面绝对移动。
    }
    const int center_x = screen_w / 2; // wjy: 双屏设备不再使用整个虚拟桌面的几何中心，避免把两屏交界处误当作游戏锁鼠点。
    const int center_y = screen_h / 2;
    dx = cursor.x - center_x;
    dy = cursor.y - center_y;
    // ===end====
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
    const uint64_t generation = g_mouse_input_mode.relative_generation;
    g_mouse_input_mode = {};
    g_mouse_input_mode.relative_generation = generation; // wjy: 退出相对模式保留代次，下一次进入时继续递增并让所有会话重新校准。
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

// =====wjy====
enum class MouseMoveRoutingDecision {
    Absolute,
    SuppressFakerAbsoluteProbe,
    Relative,
}; // wjy: 将“桌面绝对移动、驱动探测静默、游戏相对移动”分开表达，避免用单个 bool 把探测阶段误当成普通桌面移动。

MouseMoveRoutingDecision decide_mouse_move_routing(
    int x,
    int y,
    bool log_result,
    uu::WebrtcSession* session,
    bool faker_backend)
{
    int center_dx = 0;
    int center_dy = 0;
    int screen_w = 0;
    int screen_h = 0;
    bool center_lock_candidate = false; // wjy: 记录“目标坐标离中心较远但系统光标仍被游戏锁在中心”的候选状态，供 FakerInput 探测帧静默使用。
#if 1
    const bool cursor_ok = get_cursor_center_distance(center_dx, center_dy, screen_w, screen_h);
    const bool cursor_near_center = cursor_ok && std::abs(center_dx) <= 3 && std::abs(center_dy) <= 3;
    const bool target_far = normalized_point_far_from_center(x, y, screen_w, screen_h);
    center_lock_candidate = cursor_near_center && target_far; // wjy: 完全复用系统模式原有的中心锁定判据，不改变锁定灵敏度和坐标阈值。

    if (center_lock_candidate) {
        g_mouse_input_mode.lock_score = std::min(g_mouse_input_mode.lock_score + 1, 4);
        g_mouse_input_mode.unlock_score = 0;
    } else if (g_mouse_input_mode.game_relative_mode) {
        g_mouse_input_mode.unlock_score = std::min(g_mouse_input_mode.unlock_score + 1, 10);
    } else {
        g_mouse_input_mode.lock_score = 0;
    }

    if (!g_mouse_input_mode.game_relative_mode && g_mouse_input_mode.lock_score >= 3) {
        g_mouse_input_mode.game_relative_mode = true;
        ++g_mouse_input_mode.relative_generation;
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

    if (g_mouse_input_mode.game_relative_mode) {
        return MouseMoveRoutingDecision::Relative; // wjy: 达到原有三次确认门槛后，系统与 FakerInput 都继续走同一套相对移动算法。
    }
    if (faker_backend && center_lock_candidate) {
        return MouseMoveRoutingDecision::SuppressFakerAbsoluteProbe; // wjy: 第一次和第二次探测只积累锁定分数，禁止绝对 HID 被 FPS Raw Input 解读成巨大视角位移。
    }
    return MouseMoveRoutingDecision::Absolute; // wjy: 系统模式以及未命中中心锁定判据的驱动桌面模式，都保留原来的绝对移动行为。
}

void move_mouse_auto(
    int x,
    int y,
    bool log_result,
    uu::WebrtcSession* session,
    SessionPointerState* pointer,
    MouseInputBackendRouter* mouse_backend)
{
    if (!pointer || !mouse_backend) return;
    const MouseMoveRoutingDecision routing = decide_mouse_move_routing(
        x,
        y,
        log_result,
        session,
        mouse_backend->backend() == MouseInjectionBackend::Faker); // wjy: 注入后端只影响探测阶段是否静默，不改变系统模式的自动锁鼠逻辑。
    if (routing != MouseMoveRoutingDecision::Relative) {
        pointer->has_last_viewer_pos = true;
        pointer->last_viewer_x = x;
        pointer->last_viewer_y = y;
        if (routing == MouseMoveRoutingDecision::SuppressFakerAbsoluteProbe) {
            if (log_result) {
                append_input_debug_log("host FakerInput abs suppressed reason=center-lock-probe" + cursor_lock_probe_text()); // wjy: 日志明确区分主动抑制与桥接失败，便于目标机复测 FPS 锁鼠切换。
            }
            return; // wjy: 候选帧仍更新 Viewer 坐标基线，但不向虚拟绝对鼠标 collection 发送任何报告。
        }
        mouse_backend->moveAbsolute(x, y, log_result); // wjy: desktop 捕获模式只决定绝对坐标语义，实际由当前系统/驱动后端发送。
        return;
    }

    if (!pointer->has_last_viewer_pos
        || pointer->relative_generation != g_mouse_input_mode.relative_generation) {
        pointer->has_last_viewer_pos = true;
        pointer->last_viewer_x = x;
        pointer->last_viewer_y = y;
        pointer->relative_generation = g_mouse_input_mode.relative_generation; // wjy: 新进入相对模式或新控制端首次移动只记录基线，不制造跨会话跳变。
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

    const int raw_dx = x - pointer->last_viewer_x;
    const int raw_dy = y - pointer->last_viewer_y;
    pointer->last_viewer_x = x;
    pointer->last_viewer_y = y;

    int rel_dx = (raw_dx * screen_w) / 65535;
    int rel_dy = (raw_dy * screen_h) / 65535;
    rel_dx = std::clamp(rel_dx, -200, 200);
    rel_dy = std::clamp(rel_dy, -200, 200);
    mouse_backend->moveRelative(rel_dx, rel_dy, log_result); // wjy: 游戏中心锁定仍使用原相对算法，但输出统一经过可回退后端。
}
// ===end====

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

// =====wjy====
std::string base64_encode_bytes(const uint8_t* data, size_t size)
{
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    size_t index = 0;
    while (index + 2 < size) {
        const unsigned int n = (static_cast<unsigned int>(data[index]) << 16)
            | (static_cast<unsigned int>(data[index + 1]) << 8)
            | static_cast<unsigned int>(data[index + 2]);
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(kTable[(n >> 6) & 63]);
        out.push_back(kTable[n & 63]);
        index += 3;
    }
    if (index < size) {
        unsigned int n = static_cast<unsigned int>(data[index]) << 16;
        out.push_back(kTable[(n >> 18) & 63]);
        if (index + 1 < size) {
            n |= static_cast<unsigned int>(data[index + 1]) << 8;
            out.push_back(kTable[(n >> 12) & 63]);
            out.push_back(kTable[(n >> 6) & 63]);
            out.push_back('=');
        } else {
            out.push_back(kTable[(n >> 12) & 63]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

int base64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

bool base64_decode_bytes(std::string_view text, std::vector<uint8_t>* output)
{
    if (!output) return false;
    output->clear();
    int values[4] = {};
    int count = 0;
    for (char ch : text) {
        if (ch == '=' || std::isspace(static_cast<unsigned char>(ch))) {
            if (ch == '=') break;
            continue;
        }
        const int value = base64_value(ch);
        if (value < 0) return false;
        values[count++] = value;
        if (count == 4) {
            output->push_back(static_cast<uint8_t>((values[0] << 2) | (values[1] >> 4)));
            output->push_back(static_cast<uint8_t>(((values[1] & 15) << 4) | (values[2] >> 2)));
            output->push_back(static_cast<uint8_t>(((values[2] & 3) << 6) | values[3]));
            count = 0;
        }
    }
    if (count == 3) {
        output->push_back(static_cast<uint8_t>((values[0] << 2) | (values[1] >> 4)));
        output->push_back(static_cast<uint8_t>(((values[1] & 15) << 4) | (values[2] >> 2)));
    } else if (count == 2) {
        output->push_back(static_cast<uint8_t>((values[0] << 2) | (values[1] >> 4)));
    } else if (count != 0) {
        return false;
    }
    return true;
}

bool set_windows_clipboard_utf8(const std::string& utf8)
{
    const int wide_count = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wide_count <= 0) return false;
    std::wstring wide(static_cast<size_t>(wide_count), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wide_count) <= 0) {
        return false;
    }
    if (!::OpenClipboard(nullptr)) return false;
    ::EmptyClipboard();
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        ::CloseClipboard();
        return false;
    }
    void* locked = ::GlobalLock(memory);
    if (!locked) {
        ::GlobalFree(memory);
        ::CloseClipboard();
        return false;
    }
    std::memcpy(locked, wide.c_str(), bytes);
    ::GlobalUnlock(memory);
    const bool ok = ::SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    if (!ok) ::GlobalFree(memory);
    ::CloseClipboard();
    return ok;
}

bool read_windows_clipboard_utf8(std::string* utf8)
{
    if (!utf8 || !::OpenClipboard(nullptr)) return false;
    HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        ::CloseClipboard();
        return false;
    }
    const wchar_t* wide = static_cast<const wchar_t*>(::GlobalLock(data));
    if (!wide) {
        ::CloseClipboard();
        return false;
    }
    const int utf8_count = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (utf8_count <= 1) {
        ::GlobalUnlock(data);
        ::CloseClipboard();
        return false;
    }
    utf8->assign(static_cast<size_t>(utf8_count - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8->data(), utf8_count, nullptr, nullptr);
    ::GlobalUnlock(data);
    ::CloseClipboard();
    return true;
}

std::atomic_uint32_t g_clipboard_ignore_sequence = 0; // wjy: 写入远端剪贴板后忽略一次本机回读，防止环回广播。

void apply_remote_clipboard_text(const std::string& encoded)
{
    std::vector<uint8_t> decoded;
    if (!base64_decode_bytes(encoded, &decoded) || decoded.empty()) return;
    if (decoded.size() > 512 * 1024) return; // wjy: 限制文本剪贴板大小，避免 data-channel 塞入过大内容。
    const std::string utf8(reinterpret_cast<const char*>(decoded.data()), decoded.size());
    if (!set_windows_clipboard_utf8(utf8)) return;
    g_clipboard_ignore_sequence.store(::GetClipboardSequenceNumber());
}
// ===end====

void inject_input_message(
    const std::string& session_id,
    const std::string& message,
    uu::WebrtcSession* session,
    SessionPointerState* pointer,
    uu::SharedInputState* held_input,
    MouseInputBackendRouter* mouse_backend)
{
    if (!pointer || !held_input || !mouse_backend) return;
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
            move_mouse_auto(x, y, log_message, session, pointer, mouse_backend);
        }
        return;
    }
    if (kind == "r") {
        int dx = 0;
        int dy = 0;
        int buttons = 0;
        if (input >> dx >> dy >> buttons) {
            update_relative_unlock_probe(log_message, session);
            mouse_backend->moveRelative(std::clamp(dx, -200, 200), std::clamp(dy, -200, 200), log_message);
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
            pointer->has_last_viewer_pos = false;
        } else {
            mouse_backend->moveAbsolute(x, y, log_message);
        }
        const bool down = kind == "d";
        const uu::SharedInputTransition transition = held_input->updateButton(session_id, button, down);
        const bool inject_down = transition == uu::SharedInputTransition::InjectDown;
        const bool inject_up = transition == uu::SharedInputTransition::InjectUp;
        if ((button == 1 || button == 2 || button == 4) && (inject_down || inject_up)) {
            mouse_backend->sendButton(button, inject_down); // wjy: 同一按钮由多人持有时只把首次 down 和最终 up 交给当前注入后端。
        }
        return;
    }
    if (kind == "w") {
        int delta = 0;
        int x = 0;
        int y = 0;
        if (input >> delta >> x >> y) {
            if (!g_mouse_input_mode.game_relative_mode) {
                mouse_backend->moveAbsolute(x, y, log_message);
            }
            mouse_backend->sendWheel(delta);
        }
        return;
    }
    if (kind == "k") {
        int vk = 0;
        int down = 0;
        if (input >> vk >> down) {
            const uu::SharedInputTransition transition = held_input->updateKey(session_id, vk, down != 0);
            if (vk == VK_ESCAPE && down != 0 && transition != uu::SharedInputTransition::None) {
                reset_mouse_relative_mode("escape", log_message, session);
            }
            if (transition == uu::SharedInputTransition::InjectDown
                || transition == uu::SharedInputTransition::InjectRepeat) {
                mouse_backend->sendKey(vk, true); // wjy: 驱动后端把首次 down 转为 HID 快照；重复 down 在状态机内交给目标系统的键盘重复机制。
            } else if (transition == uu::SharedInputTransition::InjectUp) {
                mouse_backend->sendKey(vk, false); // wjy: 非持有者的抬键被忽略，最后一个持有者才在当前系统/驱动后端真正释放按键。
            }
        }
        return;
    }
    // =====wjy====
    if (kind == "cb") {
        // wjy: Viewer -> Host 文本剪贴板：cb <base64-utf8>
        std::string encoded;
        std::getline(input, encoded);
        if (!encoded.empty() && encoded.front() == ' ') encoded.erase(encoded.begin());
        apply_remote_clipboard_text(encoded);
    }
    // ===end====
}

class InputDispatcher final {
public:
    using MouseModeCallback = std::function<void(bool relative, const char* reason)>;
    using MouseBackendCallback = std::function<void(const char* backend, const char* state)>; // wjy: Host 的全局注入后端状态通过每个控制会话的可靠 DataChannel 同步到标题栏。
    using ClipboardCallback = std::function<void(const std::string& encodedText)>;
    using CursorShapeCallback = std::function<bool(uu::StandardCursorShape shape)>; // wjy: 回调返回可靠通道是否真正接收消息，未打开的会话保留待发送状态并在下一轮重试。
    struct CursorSubscription {
        CursorShapeCallback callback;
        std::optional<uu::StandardCursorShape> last_sent_shape;
        uint64_t generation = 0;
    };
    struct PendingCursorPublish {
        std::string session_id;
        CursorShapeCallback callback;
        uint64_t generation = 0;
    };

    // =====wjy====
    InputDispatcher()
    {
        faker_provision_worker_ = std::thread([this] {
            provisionFakerInputRuntime(); // wjy: Host 启动即在独立线程准备驱动，远控视频和现有 SendInput 在安装期间继续工作。
        });
    }

    ~InputDispatcher()
    {
        if (faker_provision_worker_.joinable()) faker_provision_worker_.join(); // wjy: 禁止后台线程越过调度器生命周期访问互斥量或会话回调。
    }
    // ===end====

    void registerSession(
        const std::string& session_id,
        MouseModeCallback callback,
        ClipboardCallback clipboard_callback = {},
        MouseBackendCallback mouse_backend_callback = {})
    {
        std::lock_guard lock(mutex_);
        mouse_mode_callbacks_[session_id] = std::move(callback); // wjy: 每个已授权会话登记弱引用通知，鼠标模式变化时所有控制窗口保持一致。
        if (clipboard_callback) clipboard_callbacks_[session_id] = std::move(clipboard_callback);
        if (mouse_backend_callback) mouse_backend_callbacks_[session_id] = std::move(mouse_backend_callback); // wjy: 只登记有控制权的 Viewer，view-only 无法查询或改变注入后端。
    }

    // =====wjy====
    void requestLockScreenEnter(const std::string& session_id)
    {
        if (session_id.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex_);
        LockScreenEnterRequest request;
        request.deadline = now + std::chrono::seconds(10); // wjy: 给 Bridge 后台准备留出有限时间，超时后不再让旧连接请求影响后续桌面状态。
        request.next_probe = now;
        pending_lock_enter_requests_[session_id] = request; // wjy: 每个已认证控制会话仅登记一次；相同 session id 重入只刷新尚未执行的同一请求。
        append_input_debug_log("host lock-enter requested session=" + session_id);
    }

    void pollLockScreenEnter()
    {
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(mutex_);
            for (auto iterator = pending_lock_enter_requests_.begin(); iterator != pending_lock_enter_requests_.end();) {
                if (now >= iterator->second.deadline) {
                    append_input_debug_log("host lock-enter expired session=" + iterator->first);
                    iterator = pending_lock_enter_requests_.erase(iterator); // wjy: 过期请求只清自己，不改变其他远控会话或键鼠后端。
                } else {
                    ++iterator;
                }
            }
            if (pending_lock_enter_requests_.empty()) return;
            const bool probe_due = std::any_of(
                pending_lock_enter_requests_.begin(),
                pending_lock_enter_requests_.end(),
                [now](const auto& item) { return now >= item.second.next_probe; });
            if (!probe_due) return;
        }

        const CurrentSessionLockQuery query = query_current_session_lock_state(); // wjy: WTS 调用放在输入互斥量外，系统服务响应慢时不阻塞正常远控键鼠。
        std::lock_guard lock(mutex_);
        if (pending_lock_enter_requests_.empty()) return;
        const auto next_probe = now + std::chrono::milliseconds(250);
        for (auto& [session_id, request] : pending_lock_enter_requests_) request.next_probe = next_probe;

        if (query.state == CurrentSessionLockState::Unlocked) {
            append_input_debug_log("host lock-enter skipped state=unlocked session_id="
                + std::to_string(query.session_id)
                + " requests=" + std::to_string(pending_lock_enter_requests_.size()));
            pending_lock_enter_requests_.clear(); // wjy: 普通桌面打开远控只完成检测，绝不发送 Enter。
            return;
        }
        if (query.state == CurrentSessionLockState::Unknown) {
            if (!lock_state_query_failure_logged_) {
                append_input_debug_log("host lock-enter state=unknown session_id="
                    + std::to_string(query.session_id)
                    + " flags=" + std::to_string(query.session_flags)
                    + " error=" + std::to_string(query.error));
                lock_state_query_failure_logged_ = true; // wjy: 同一批请求只记录一次查询失败，250ms 重试不会刷爆诊断日志。
            }
            return;
        }

        lock_state_query_failure_logged_ = false;
        if (!faker_provision_complete_) {
            return; // wjy: 锁屏确认后等待现有后台线程完成 Bridge/驱动准备，只重试就绪检查，不提前使用 SendInput。
        }
        if (!held_input_.empty()) {
            return; // wjy: 存在真实控制端持有键或鼠标时等待空闲，防止自动 Enter 与正在进行的组合操作交叉。
        }

        const MouseInputBackendRouter::OneShotEnterAttempt attempt = mouse_backend_.sendOneShotEnter();
        if (!attempt.completed) {
            append_input_debug_log("host lock-enter waiting bridge error=" + attempt.error);
            return; // wjy: Ping/连接阶段失败尚未触碰键盘，可在十秒窗口内继续等待现有 Bridge 恢复。
        }

        append_input_debug_log(std::string("host lock-enter completed success=")
            + (attempt.success ? "1" : "0")
            + " session_id=" + std::to_string(query.session_id)
            + " requests=" + std::to_string(pending_lock_enter_requests_.size())
            + (attempt.error.empty() ? std::string() : " error=" + attempt.error));
        pending_lock_enter_requests_.clear(); // wjy: 一旦开始键盘请求就清除同批全部打开事件，响应丢失或多路同时连接都不会重复按 Enter。
    }
    // ===end====

    void registerCursorSession(const std::string& session_id, CursorShapeCallback callback)
    {
        if (!callback) return;
        std::lock_guard lock(mutex_);
        CursorSubscription subscription;
        subscription.callback = std::move(callback);
        subscription.generation = ++next_cursor_subscription_generation_;
        cursor_shape_callbacks_[session_id] = std::move(subscription); // wjy: 每个授权 Viewer 独立去重，新会话不能借用旧会话的“已发送”状态。
    }

    void dispatch(const std::string& session_id, const std::string& message)
    {
        std::vector<MouseModeCallback> mode_callbacks;
        std::vector<MouseBackendCallback> backend_callbacks;
        std::string backend_token;
        std::string backend_state;
        bool relative = false;
        {
            std::lock_guard lock(mutex_); // wjy: 所有 WebRTC data-channel 回调在这里汇合，SendInput 与全局鼠标模式按唯一顺序执行。
            const bool was_relative = g_mouse_input_mode.game_relative_mode;
            const bool backend_message = handleMouseBackendMessageLocked(
                session_id,
                message,
                &backend_callbacks,
                &backend_token,
                &backend_state); // wjy: 保留前缀命令在进入普通 m/r/d/u/w/k 解析前分流，绝不被当成鼠标事件。
            if (!backend_message) {
                inject_input_message(
                    session_id,
                    message,
                    nullptr,
                    &pointer_by_session_[session_id],
                    &held_input_,
                    &mouse_backend_);
            }
            relative = g_mouse_input_mode.game_relative_mode;
            if (was_relative != relative) {
                for (const auto& [id, callback] : mouse_mode_callbacks_) {
                    if (callback) mode_callbacks.push_back(callback);
                }
            }
            std::string fallback_error;
            if (mouse_backend_.consumeFallback(&fallback_error)) {
                backend_token = "system";
                backend_state = "fallback";
                collectAllMouseBackendCallbacksLocked(&backend_callbacks); // wjy: 驱动运行中断管后，全体控制窗口同时看到真实系统回退状态。
                append_input_debug_log("host mouse backend broadcast fallback error=" + fallback_error);
            }
        }
        for (const auto& callback : mode_callbacks) callback(relative, "shared-dispatcher"); // wjy: 模式通知在互斥区外发送，避免 WebRTC 回调反向阻塞其他控制端输入。
        for (const auto& callback : backend_callbacks) {
            callback(backend_token.c_str(), backend_state.c_str()); // wjy: 管道连接与输入状态已在锁内提交，锁外发送确认避免 WebRTC 回调形成互锁。
        }
    }

    void pollHostClipboard()
    {
        std::vector<ClipboardCallback> callbacks;
        std::string encoded;
        {
            std::lock_guard lock(mutex_);
            if (clipboard_callbacks_.empty()) return;
            const DWORD sequence = ::GetClipboardSequenceNumber();
            if (sequence == 0 || sequence == last_clipboard_sequence_) return;
            if (sequence == g_clipboard_ignore_sequence.load()) {
                last_clipboard_sequence_ = sequence;
                return; // wjy: 刚写入的远端剪贴板内容不回推，避免环路。
            }
            std::string utf8;
            if (!read_windows_clipboard_utf8(&utf8) || utf8.empty()) {
                last_clipboard_sequence_ = sequence;
                return;
            }
            if (utf8.size() > 512 * 1024) {
                last_clipboard_sequence_ = sequence;
                return;
            }
            last_clipboard_sequence_ = sequence;
            last_clipboard_payload_ = utf8;
            encoded = base64_encode_bytes(reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size());
            for (const auto& [id, callback] : clipboard_callbacks_) {
                if (callback) callbacks.push_back(callback);
            }
        }
        for (const auto& callback : callbacks) callback(encoded);
    }

    // =====wjy====
    void pollHostCursor()
    {
        {
            std::lock_guard lock(mutex_);
            if (cursor_shape_callbacks_.empty()) {
                return; // wjy: 没有 Viewer 订阅时不读取光标，更不会向桌面程序发送 WM_NCHITTEST。
            }
        }
        const uu::StandardCursorShape shape = current_standard_cursor_shape(); // wjy: 系统调用和有界窗口命中都放在调度器锁外，不阻塞串行 SendInput。
        std::vector<PendingCursorPublish> pending;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [session_id, subscription] : cursor_shape_callbacks_) {
                if (subscription.callback
                    && (!subscription.last_sent_shape.has_value() || *subscription.last_sent_shape != shape)) {
                    pending.push_back({session_id, subscription.callback, subscription.generation}); // wjy: 仅复制未确认该形状的会话，锁外发送避免 WebRTC 反向阻塞串行输入。
                }
            }
        }

        std::vector<std::pair<std::string, uint64_t>> succeeded;
        for (const PendingCursorPublish& publish : pending) {
            if (publish.callback(shape)) {
                succeeded.emplace_back(publish.session_id, publish.generation); // wjy: 只有 DataChannel::Send 成功才允许提交去重状态，通道尚未打开会在 25ms 后重试。
            }
        }
        if (succeeded.empty()) return;

        std::lock_guard lock(mutex_);
        for (const auto& [session_id, generation] : succeeded) {
            const auto iterator = cursor_shape_callbacks_.find(session_id);
            if (iterator != cursor_shape_callbacks_.end() && iterator->second.generation == generation) {
                iterator->second.last_sent_shape = shape; // wjy: 代际匹配防止旧发送结果覆盖同 ID 重新注册的新会话。
            }
        }
    }
    // ===end====

    void releaseSession(const std::string& session_id)
    {
        std::vector<MouseBackendCallback> fallback_callbacks;
        {
            std::lock_guard lock(mutex_);
            const uu::SharedInputReleaseBatch released = held_input_.releaseSession(session_id);
            for (const int key : released.keys) mouse_backend_.sendKey(key, false); // wjy: 会话断线在当前真实键盘后端补发最终抬键，FakerInput 不会残留移动键。
            for (const int button : released.buttons) mouse_backend_.sendButton(button, false); // wjy: 断线只在当前真实后端补发最终抬起，其他控制端的按住状态继续有效。
            pointer_by_session_.erase(session_id);
            mouse_mode_callbacks_.erase(session_id);
            mouse_backend_callbacks_.erase(session_id);
            clipboard_callbacks_.erase(session_id);
            pending_lock_enter_requests_.erase(session_id); // wjy: 会话在执行前退出时同步撤销自动 Enter，过期请求不能在无人连接后解锁设备。
            if (mouse_mode_callbacks_.empty()) reset_mouse_relative_mode("last-controller-left", false, nullptr);
            std::string fallback_error;
            if (mouse_backend_.consumeFallback(&fallback_error)) {
                collectAllMouseBackendCallbacksLocked(&fallback_callbacks); // wjy: 某会话断开时若恰好发现桥接管道失败，剩余 Viewer 仍会收到系统回退状态。
                append_input_debug_log("host mouse backend disconnect fallback error=" + fallback_error);
            }
        }
        for (const auto& callback : fallback_callbacks) callback("system", "fallback"); // wjy: WebRTC 通知继续放在调度器锁外，断线清理不会阻塞其他会话输入。
    }

    void releaseCursorSession(const std::string& session_id)
    {
        std::lock_guard lock(mutex_);
        cursor_shape_callbacks_.erase(session_id); // wjy: 查看权限与控制权限分离释放，只移除目标会话且不会改变其它 Viewer 的已确认形状。
    }

    void shutdown()
    {
        // =====wjy====
        if (faker_provision_worker_.joinable()) faker_provision_worker_.join(); // wjy: 等待 Windows Installer 自己结束；不能在驱动提交阶段强杀 msiexec。
        // ===end====
        std::lock_guard lock(mutex_);
        for (const auto& [session_id, callback] : mouse_mode_callbacks_) {
            const uu::SharedInputReleaseBatch released = held_input_.releaseSession(session_id);
            for (const int key : released.keys) mouse_backend_.sendKey(key, false); // wjy: Host 关闭先通过当前后端释放共享键盘，再让 Bridge 执行全局 release-all 兜底。
            for (const int button : released.buttons) mouse_backend_.sendButton(button, false);
        }
        mouse_backend_.selectSystem(); // wjy: Host 关闭前释放虚拟 HID 并断开本机管道，Bridge 服务端也会执行自己的 release-all 兜底。
        pointer_by_session_.clear();
        mouse_mode_callbacks_.clear();
        mouse_backend_callbacks_.clear();
        clipboard_callbacks_.clear();
        cursor_shape_callbacks_.clear();
        pending_lock_enter_requests_.clear(); // wjy: Host 关闭后不保留任何等待中的锁屏输入请求。
        reset_mouse_relative_mode("host-shutdown", false, nullptr); // wjy: Host 资源销毁前兜底清空所有会话输入与相对鼠标状态。
    }

private:
    // =====wjy====
    struct LockScreenEnterRequest {
        std::chrono::steady_clock::time_point deadline;
        std::chrono::steady_clock::time_point next_probe;
    };
    // ===end====

    bool handleMouseBackendMessageLocked(
        const std::string& session_id,
        const std::string& message,
        std::vector<MouseBackendCallback>* callbacks,
        std::string* backend_token_out,
        std::string* backend_state_out)
    {
        constexpr std::string_view kPrefix = "__fsremote_mouse_backend ";
        if (message.rfind(kPrefix, 0) != 0) return false;
        const std::string command = message.substr(kPrefix.size());
        if (command == "query") {
            const auto callback = mouse_backend_callbacks_.find(session_id);
            if (callback != mouse_backend_callbacks_.end() && callback->second) {
                callbacks->push_back(callback->second); // wjy: query 只回复请求会话，避免每次新窗口连接都让其他窗口重复刷新。
            }
            *backend_token_out = mouse_backend_token(mouse_backend_.backend());
            // =====wjy====
            *backend_state_out = faker_requested_ && !faker_provision_complete_
                ? "installing"
                : "ready"; // wjy: 新 Viewer 加入正在安装的 Host 时得到真实中间态，不把系统回退误显示为最终完成。
            // ===end====
            return true;
        }
        if (command != "system" && command != "faker") {
            append_input_debug_log("host rejected mouse backend command=" + command);
            return true; // wjy: 命中保留前缀但值非法时直接丢弃，绝不落入普通输入解析。
        }

        const MouseInjectionBackend requested = command == "faker"
            ? MouseInjectionBackend::Faker
            : MouseInjectionBackend::System;
        // =====wjy====
        faker_requested_ = requested == MouseInjectionBackend::Faker; // wjy: 后台完成时只兑现最新一次全局请求，安装期间切回系统可取消自动切换。
        if (requested == MouseInjectionBackend::Faker
            && mouse_backend_.backend() != MouseInjectionBackend::Faker
            && !faker_provision_complete_) {
            *backend_token_out = "system";
            *backend_state_out = "installing";
            collectAllMouseBackendCallbacksLocked(callbacks); // wjy: 立即通知所有控制端“系统键鼠仍可用、驱动正在准备”，不阻塞 WebRTC data-channel 线程。
            append_input_debug_log("host mouse backend request=faker applied=system state=installing");
            return true;
        }
        // ===end====
        bool ready = true;
        if (requested != mouse_backend_.backend()) {
            const uu::SharedInputReleaseBatch released = held_input_.releaseAll(); // wjy: 现在标题栏切换的是完整键鼠后端，必须同时终止旧键盘和鼠标持有关系。
            for (const int key : released.keys) {
                mouse_backend_.sendKey(key, false); // wjy: 先在旧后端释放所有共享键，禁止 key-down 在系统而 key-up 落到 FakerInput。
            }
            for (const int button : released.buttons) {
                mouse_backend_.sendButton(button, false); // wjy: 键盘释放完成后再释放共享按钮，Bridge 收到的最终快照保持有序。
            }
            std::string ignored_fallback;
            mouse_backend_.consumeFallback(&ignored_fallback); // wjy: 释放阶段若恰好断管，显式切换的最终结果覆盖这条中间状态。
            if (requested == MouseInjectionBackend::Faker) {
                std::string bridge_error;
                ready = mouse_backend_.selectFaker(&bridge_error);
                if (!ready) append_input_debug_log("host mouse backend kept system error=" + bridge_error);
            } else {
                mouse_backend_.selectSystem();
            }
        }

        *backend_token_out = mouse_backend_token(mouse_backend_.backend());
        *backend_state_out = ready ? "ready" : "fallback";
        collectAllMouseBackendCallbacksLocked(callbacks); // wjy: 后端属于 Host 全局状态，任一控制端切换后向全部控制 Viewer 广播一致结果。
        append_input_debug_log("host mouse backend request=" + command
            + " applied=" + *backend_token_out
            + " state=" + *backend_state_out);
        return true;
    }

    // =====wjy====
    void releaseSharedInputsLocked()
    {
        const uu::SharedInputReleaseBatch released = held_input_.releaseAll(); // wjy: 后台安装完成兑现驱动请求时，键盘与鼠标使用同一个原子释放屏障。
        for (const int key : released.keys) {
            mouse_backend_.sendKey(key, false); // wjy: 先在系统后端补齐键盘抬起，再连接驱动，防止安装期间按住的键跨后端卡住。
        }
        for (const int button : released.buttons) {
            mouse_backend_.sendButton(button, false);
        }
        std::string ignoredFallback;
        mouse_backend_.consumeFallback(&ignoredFallback); // wjy: 最终切换结果覆盖释放阶段偶发的旧 Bridge 断管状态。
    }

    void provisionFakerInputRuntime()
    {
        std::string provisionError;
        const bool provisioned = faker_provisioner_.ensureReady(&provisionError); // wjy: 包含哈希校验、管理员令牌检查、静默 MSI、Bridge 启动和 driver-ready ping。
        append_input_debug_log(std::string("host FakerInput runtime provision ")
            + (provisioned ? "ready" : "failed")
            + (provisionError.empty() ? std::string() : " error=" + provisionError));

        std::vector<MouseBackendCallback> callbacks;
        std::string backendToken;
        std::string backendState;
        {
            std::lock_guard lock(mutex_);
            faker_provision_complete_ = true; // wjy: 受同一互斥量保护，点击线程不会观察到半提交的安装结果。
            faker_provision_error_ = provisionError;
            if (!faker_requested_) return; // wjy: 用户未选择驱动或安装期间已切回系统时，仅保留已准备的 Bridge，不擅自改变键鼠后端。

            releaseSharedInputsLocked(); // wjy: 驱动 ready 后同时清理旧系统键盘与鼠标状态，再由下一次物理按下建立 FakerInput 快照。
            std::string bridgeError;
            const bool ready = mouse_backend_.selectFaker(&bridgeError); // wjy: 即使安装结果失败也做一次快速连接，兼容用户同期手工启动的可信 Bridge。
            if (!ready) {
                faker_provision_error_ = bridgeError.empty() ? provisionError : bridgeError;
                append_input_debug_log("host mouse backend kept system after provision error=" + faker_provision_error_);
            }
            backendToken = mouse_backend_token(mouse_backend_.backend());
            backendState = ready ? "ready" : "fallback";
            collectAllMouseBackendCallbacksLocked(&callbacks); // wjy: 安装完成只广播一次最终 ready/fallback，所有 Viewer 的标题栏保持一致。
        }
        for (const auto& callback : callbacks) {
            callback(backendToken.c_str(), backendState.c_str()); // wjy: 状态发送继续位于互斥区外，避免 Bridge 准备线程与 WebRTC 会话互锁。
        }
    }
    // ===end====

    void collectAllMouseBackendCallbacksLocked(std::vector<MouseBackendCallback>* callbacks) const
    {
        callbacks->clear(); // wjy: 同一条消息最多生成一轮最终后端通知，避免中间回退与显式切换结果重复发送。
        for (const auto& [id, callback] : mouse_backend_callbacks_) {
            if (callback) callbacks->push_back(callback);
        }
    }

    std::mutex mutex_;
    uu::SharedInputState held_input_;
    MouseInputBackendRouter mouse_backend_; // wjy: 一个 Host 只允许一个注入后端，与跨会话按钮合并状态保持同一互斥顺序。
    // =====wjy====
    uu::FakerInputRuntimeProvisioner faker_provisioner_; // wjy: 管理本进程启动的 Bridge 生命周期，手工启动的现有服务不会被误杀。
    std::thread faker_provision_worker_; // wjy: MSI 最长等待与 PnP 枚举全部脱离输入分发线程。
    bool faker_provision_complete_ = false;
    bool faker_requested_ = false;
    std::string faker_provision_error_; // wjy: 失败原因进入本机诊断日志，网络协议只暴露有限状态而不泄漏目标端路径。
    std::unordered_map<std::string, LockScreenEnterRequest> pending_lock_enter_requests_; // wjy: 只保存已认证控制会话的短期请求，不保存密码或任何用户输入内容。
    bool lock_state_query_failure_logged_ = false;
    // ===end====
    std::unordered_map<std::string, SessionPointerState> pointer_by_session_;
    std::unordered_map<std::string, MouseModeCallback> mouse_mode_callbacks_;
    std::unordered_map<std::string, MouseBackendCallback> mouse_backend_callbacks_;
    std::unordered_map<std::string, ClipboardCallback> clipboard_callbacks_;
    std::unordered_map<std::string, CursorSubscription> cursor_shape_callbacks_; // wjy: 每个 Viewer 独立保存最后成功形状，新增或暂未打开通道的会话不会被全局去重漏发。
    uint64_t next_cursor_subscription_generation_ = 0;
    DWORD last_clipboard_sequence_ = 0;
    std::string last_clipboard_payload_;
};
// ===end====

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
    virtual bool setViewerQuality(const FsRemoteViewerQualityConfig*) { return false; } // wjy: 只有Viewer覆盖在线质量更新，Host或旧句柄安全返回false。
    virtual bool setViewerAudioEnabled(bool) { return false; } // wjy: Host句柄不拥有本地播放器，错误类型安全返回false。
    virtual bool viewerQualityStatus(FsRemoteViewerQualityStatus*) const { return false; } // wjy: 非Viewer句柄没有应用状态。
    virtual bool viewerPerformanceStats(FsRemoteViewerPerformanceStats*) const { return false; } // wjy: Host 句柄不提供接收端统计，错误句柄安全返回 false。
    virtual bool isBusy() const { return false; }
    // =====wjy====
    virtual uint32_t activeSessionCount() const { return 0; } // wjy: Viewer 无会话表；Host 覆盖为当前登记会话数。
    virtual std::string activeControllerNamesCsv() const { return {}; } // wjy: 逗号分隔的控制端设备名列表。
    virtual std::string activeControllerDetailsCsv() const { return {}; } // wjy: 每行“设备名\tIP”，仅供目标端本机提示层读取。
    // ===end====

protected:
    std::atomic_bool running_ = true;
    std::atomic_uintptr_t server_socket_ = 0;
    std::atomic_uintptr_t socket_ = 0;
    std::thread worker_;
};

// =====wjy====
struct HostClientSession {
    explicit HostClientSession(uintptr_t clientSocket)
        : socket(clientSocket)
    {
        admission.session_id = random_hex(16); // wjy: 在线程启动和 map 登记前生成稳定 ID，整个会话生命周期只使用这一把键。
    }

    void cancel()
    {
        cancelled = true;
        const uintptr_t current = socket.exchange(0);
        if (current) uu::close_socket(current); // wjy: 关闭客户端 socket 同时解除握手或信令阻塞，使工作线程可以确定退出。
    }

    bool send(const std::string& message)
    {
        std::lock_guard lock(send_mutex);
        const uintptr_t current = socket.load();
        return current && !cancelled && uu::send_message(current, message); // wjy: 所有 WebRTC 回调共用一把发送锁，避免帧边界交叉。
    }

    std::atomic_uintptr_t socket = 0;
    std::mutex send_mutex;
    std::mutex webrtc_mutex; // wjy: 共享鼠标模式通知可能来自其他会话线程，销毁 PeerConnection 时用同一把锁阻止悬空访问。
    std::atomic_bool cancelled = false;
    std::atomic_bool completed = false;
    SessionAdmission admission;
    std::unique_ptr<uu::HostMediaPipeline::Subscription> media_subscription; // wjy: 会话只持订阅令牌，最后一个令牌释放时由共享管线停止捕获/VDD。
    std::unique_ptr<uu::WebrtcSession> webrtc; // wjy: 每个客户端独占 PeerConnection、track、sender 和编码器状态。
    std::thread worker; // wjy: 工作线程始终可 join，禁止 detached 生命周期越过 HostInstance。
};

class HostInstance final : public StreamInstance {
public:
    HostInstance(uint16_t port, HostRuntimeConfig config)
        : config_(config)
    {
        std::string error;
        if (!runtime_.initialize(&error)) throw std::runtime_error(error.empty() ? "host WebRTC runtime initialization failed" : error); // wjy: runtime 在 HostInstance 创建线程初始化，关闭时回到同一线程清理。
        worker_ = std::thread([this, port] {
            try {
                runAcceptLoop(port); // wjy: manager线程异常统一限制在Host实例，不能越过std::thread入口终止整个程序。
            } catch (const std::exception& exception) {
                append_log(std::string("host accept worker exception: ") + exception.what());
            } catch (...) {
                append_log("host accept worker unknown exception");
            }
        });
        // =====wjy====
        host_state_worker_ = std::thread([this] {
            try {
                runHostStatePollLoop(); // wjy: 辅助线程同时同步低延迟光标和低频剪贴板，故障不影响视频、输入或 Host 主循环。
            } catch (const std::exception& exception) {
                append_log(std::string("host state worker exception: ") + exception.what());
            } catch (...) {
                append_log("host state worker unknown exception");
            }
        });
        // ===end====
    }

    ~HostInstance() override
    {
        shutdown(); // wjy: 派生成员析构前先完成 listener、客户端线程、音频和 WebRTC 的确定性停止顺序。
    }

    bool isBusy() const override
    {
        return activeSessionCount() > 0;
    }

    // =====wjy====
    uint32_t activeSessionCount() const override
    {
        std::lock_guard lock(sessions_mutex_);
        // wjy: 只统计已取得控制权且没有进入取消/完成状态的会话，ICE 断开回调一触发，人数无需等待 worker 完全回收即可归零。
        uint32_t count = 0;
        for (const auto& [id, session] : sessions_) {
            if (session
                && !session->cancelled // wjy: socket 已被 ICE 或同设备重连取消时立即排除，消除异常退出后的僵尸人数。
                && !session->completed
                && session->admission.ownership == "control_granted") { // wjy: 握手中和只读观看会话不属于“正在控制”的人数。
                ++count;
            }
        }
        return count;
    }

    std::string activeControllerNamesCsv() const override
    {
        std::lock_guard lock(sessions_mutex_);
        std::string names;
        for (const auto& [id, session] : sessions_) {
            if (!session || session->cancelled || session->completed) continue; // wjy: 已触发断线清理的会话不再出现在控制端名称气泡中。
            // wjy: 气泡优先展示“有控制权”的端；没有设备名时回退 client_id 前 8 位。
            if (session->admission.ownership != "control_granted"
                && session->admission.requested_role != "control") {
                // still include any active session that holds video so multi-viewer is visible
            }
            std::string name = session->admission.client_device_name;
            if (name.empty()) {
                name = session->admission.client_id.empty()
                    ? std::string("unknown")
                    : session->admission.client_id.substr(0, std::min<size_t>(8, session->admission.client_id.size()));
            }
            // strip commas so CSV stays parseable
            for (char& ch : name) {
                if (ch == ',' || ch == '|' || ch == '\n' || ch == '\r') ch = ' ';
            }
            if (!names.empty()) names.push_back(',');
            names += name;
        }
        return names;
    }

    std::string activeControllerDetailsCsv() const override
    {
        std::lock_guard lock(sessions_mutex_);
        std::string details;
        for (const auto& [id, session] : sessions_) {
            if (!session || session->cancelled || session->completed) continue; // wjy: 人数和详情使用同一存活条件，避免徽标为 1 但气泡仍残留两条记录。
            if (session->admission.ownership != "control_granted") continue; // wjy: 只有真正取得控制权的认证会话才在目标机提示，握手中和只读观看不会误显示为正在控制。

            std::string name = session->admission.client_device_name;
            if (name.empty()) {
                name = session->admission.client_id.empty()
                    ? std::string("unknown")
                    : session->admission.client_id.substr(0, std::min<size_t>(8, session->admission.client_id.size()));
            }
            std::string ip = session->admission.client_ip.empty()
                ? std::string("unknown")
                : session->admission.client_ip;
            for (char& ch : name) {
                if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
            }
            for (char& ch : ip) {
                if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
            }
            if (!details.empty()) details.push_back('\n');
            details += name;
            details.push_back('\t');
            details += ip; // wjy: 一个会话一行，Qt 端可无歧义拆成设备名和来源 IP。
        }
        return details;
    }

private:
    // =====wjy====
    void cancelSupersededControllerSessions(const std::shared_ptr<HostClientSession>& current)
    {
        if (!current || current->admission.public_key.empty()) {
            return; // wjy: 没有稳定授权公钥时不能仅凭设备名误踢其它同名控制端。
        }

        std::vector<std::shared_ptr<HostClientSession>> superseded;
        {
            std::lock_guard lock(sessions_mutex_); // wjy: 在会话表锁内只收集旧会话，真正关闭 socket 放到锁外执行。
            for (const auto& [id, session] : sessions_) {
                if (!session
                    || session.get() == current.get()
                    || session->cancelled
                    || session->completed
                    || session->admission.public_key != current->admission.public_key) {
                    continue; // wjy: 公钥代表同一已授权控制设备，只替换它自己的旧连接，不影响其它控制端并发。
                }
                superseded.push_back(session); // wjy: 同一控制设备重连时，新会话接管，旧会话立即退出人数和气泡。
            }
        }
        for (const auto& session : superseded) {
            append_log("host replacing stale controller session old=" + session->admission.session_id
                + " new=" + current->admission.session_id); // wjy: 保留替换日志，实机可确认异常退出后的旧会话是否被新连接命中。
            session->cancel(); // wjy: 关闭旧信令 socket，解除其 recv 阻塞并进入原有完整资源清理流程。
        }
    }
    // ===end====

    uintptr_t createListener(uint16_t port, std::string* error)
    {
        if (!ensure_wsa()) {
            if (error) *error = "WSAStartup failed";
            return 0;
        }
        SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            if (error) *error = "socket failed";
            return 0;
        }
        BOOL reuse = TRUE;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR
            || ::listen(listener, SOMAXCONN) == SOCKET_ERROR) {
            if (error) *error = "bind/listen failed";
            closesocket(listener);
            return 0;
        }
        server_socket_ = static_cast<uintptr_t>(listener); // wjy: listener 整个 HostInstance 生命周期只创建一次，活动会话不再关闭监听端口。
        return static_cast<uintptr_t>(listener);
    }

    void runAcceptLoop(uint16_t port)
    {
        std::string error;
        const uintptr_t listener = createListener(port, &error);
        if (!listener) {
            append_log("host listener failed: " + error);
            return;
        }
        // =====wjy====
        constexpr long kAcceptPollTimeoutMicroseconds = 200 * 1000; // wjy: Accept 管理线程最长每 200ms 检查一次退出标志，不再依赖其它线程关闭 socket 来唤醒永久阻塞的 accept。
        append_log("host accept loop ready poll_ms=200");
        while (running_) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(static_cast<SOCKET>(listener), &readable); // wjy: 只有本管理线程监视和接收 listener，select 可安全作为 accept 前的可读门禁。
            timeval timeout = {};
            timeout.tv_usec = kAcceptPollTimeoutMicroseconds;

            const int ready = ::select(0, &readable, nullptr, nullptr, &timeout); // wjy: Windows 忽略第一个 nfds 参数；超时返回后循环会重新读取原子 running_。
            if (!running_) {
                break; // wjy: 关闭请求到达后由 Accept 线程自己退出并在循环后关闭 listener，消除跨线程 closesocket 与阻塞 accept 的竞态。
            }
            if (ready == 0) {
                continue; // wjy: 没有新连接只是正常轮询超时，不写高频日志也不改变现有会话。
            }
            if (ready == SOCKET_ERROR) {
                append_log("host select failed error=" + std::to_string(::WSAGetLastError()));
                break;
            }
            if (!FD_ISSET(static_cast<SOCKET>(listener), &readable)) {
                continue; // wjy: 防御异常返回；未明确标记 listener 可读时绝不进入可能阻塞的 accept。
            }

            const SOCKET accepted = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
            if (accepted == INVALID_SOCKET) {
                const int accept_error = ::WSAGetLastError();
                if (running_) append_log("host accept failed error=" + std::to_string(accept_error));
                if (accept_error == WSAEWOULDBLOCK
                    || accept_error == WSAEINTR
                    || accept_error == WSAECONNRESET) {
                    continue; // wjy: select 与 accept 之间连接被撤销属于可恢复竞态，下一轮继续监听；其它错误退出以免无休止空转刷日志。
                }
                break;
            }
            reapCompletedSessions(); // wjy: 新连接到达后先回收已结束 worker，容量判断不会被僵尸记录占用。
            if (!running_) {
                uu::close_socket(static_cast<uintptr_t>(accepted));
                break;
            }
            {
                std::lock_guard lock(sessions_mutex_);
                if (sessions_.size() >= config_.effective_max_sessions) {
                    send_admission_rejection(static_cast<uintptr_t>(accepted), uu::SessionRejectionReason::Capacity,
                        "maximum concurrent sessions reached");
                    uu::close_socket(static_cast<uintptr_t>(accepted));
                    append_log("host capacity rejected client");
                    continue; // wjy: listener 保持可用，超限客户端获得明确 capacity，现有会话完全不受影响。
                }
            }
            auto session = std::make_shared<HostClientSession>(static_cast<uintptr_t>(accepted));
            session->admission.client_ip = socket_peer_ip(static_cast<uintptr_t>(accepted)); // wjy: 在 socket 仍有效时立即记录控制端来源地址，后续 WebRTC 阶段不再依赖信令连接状态。
            {
                std::lock_guard lock(sessions_mutex_);
                sessions_.emplace(session->admission.session_id, session); // wjy: map 只保存共享上下文，worker 和 manager 都能安全完成清理。
            }
            session->worker = std::thread([this, session] {
                try {
                    runClientSession(session); // wjy: 单个控制端会话的认证/编码/信令异常不得影响其它会话或整个Host进程。
                } catch (const std::exception& exception) {
                    append_log(std::string("host client worker exception session=")
                        + session->admission.session_id + " error=" + exception.what());
                    session->cancel();
                    session->completed = true;
                } catch (...) {
                    append_log(std::string("host client worker unknown exception session=") + session->admission.session_id);
                    session->cancel();
                    session->completed = true;
                }
            });
            append_log("host client worker started session=" + session->admission.session_id);
        }
        append_log("host accept loop exit");
        const uintptr_t published = server_socket_.exchange(0);
        if (published) uu::close_socket(published); // wjy: listener 由创建并使用它的 Accept 管理线程关闭，句柄不会在 select/accept 期间被其它线程抢先回收或复用。
        append_log("host accept listener closed by manager");
        // ===end====
        shutdownSessionsOnManagerThread(); // wjy: accept 退出后由同一 manager 线程 join 客户端并销毁共享 runtime。
    }

    void runClientSession(const std::shared_ptr<HostClientSession>& context)
    {
        const uintptr_t socket = context->socket.load();
        std::string error;
        if (!perform_host_admission(socket, config_, &exclusive_control_claimed_, &audio_primary_claimed_, &context->admission, &error)) {
            append_log("host admission rejected session=" + context->admission.session_id + " error=" + error);
            releaseAdmissionClaims(context); // wjy: 最终准入回包失败时归还兼容独占槽和单路音频槽，不影响其他协同控制端。
            context->cancel();
            context->completed = true;
            return; // wjy: 认证失败路径不会触碰音频、PeerConnection、桌面采集或编码器。
        }
        append_log("host admission accepted session=" + context->admission.session_id + " client=" + context->admission.client_id);
        lsp::append_stream_capture_diagnostic_log(
            "session",
            "admission accepted session=" + context->admission.session_id
                + " client=" + context->admission.client_id
                + " capabilities='" + context->admission.capabilities + "'"
                + " ownership='" + context->admission.ownership + "'"); // wjy: 认证成功后立即写入采集日志，后续无画面可按session id串起全部媒体阶段。
        // =====wjy====
        cancelSupersededControllerSessions(context); // wjy: 同一授权设备重新连接时先踢掉旧会话，避免异常退出记录和新连接同时显示为两台相同设备。
        const auto media_subscribe_started = std::chrono::steady_clock::now(); // wjy: 记录VDD、DXGI和source创建总耗时，不改变同步订阅流程。
        lsp::append_stream_capture_diagnostic_log(
            "session",
            "media subscribe begin session=" + context->admission.session_id); // wjy: 如果日志停在此行，问题就在共享媒体管线启动而非WebRTC。
        context->media_subscription = media_pipeline_.subscribe(&error); // wjy: 认证成功后才加入共享桌面源，未授权连接不会启动或占用采集资源。
        const auto media_subscribe_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - media_subscribe_started).count();
        if (!context->media_subscription) {
            append_log("host media subscribe failed session=" + context->admission.session_id + " error=" + error);
            lsp::append_stream_capture_diagnostic_log(
                "session",
                "media subscribe failed session=" + context->admission.session_id
                    + " elapsed_ms=" + std::to_string(media_subscribe_ms)
                    + " error=" + error); // wjy: 两种采集后端均失败时保存总耗时和最终错误。
            releaseAdmissionClaims(context); // wjy: 新会话媒体创建失败只回收自己的兼容槽和音频槽，不影响现有控制与视频订阅者。
            context->cancel();
            context->completed = true;
            return;
        }
        lsp::append_stream_capture_diagnostic_log(
            "session",
            "media subscribe success session=" + context->admission.session_id
                + " elapsed_ms=" + std::to_string(media_subscribe_ms)); // wjy: 共享source取得成功后，后续等待首帧应转查WebRTC和编码器。
        // ===end====
        const bool has_control = context->admission.ownership == "control_granted";
        if (has_control) {
            input_dispatcher_.requestLockScreenEnter(context->admission.session_id); // wjy: 新控制会话取得视频源后立即检测目标锁屏；只读观看和未认证连接绝不触发按键。
        }
        const bool owns_audio = context->admission.audio_primary;
        if (owns_audio && has_capability(context->admission.capabilities, "audio")) {
            lsp::append_stream_capture_diagnostic_log(
                "session",
                "audio start begin session=" + context->admission.session_id); // wjy: 音频启动位于媒体订阅和WebRTC之间，记录边界可发现单路音频监听意外阻塞。
            startAudioForSingleSession(); // wjy: 单路音频拥有者与协同控制权限完全解耦，第二、第三控制端不会因没有音频而降为只读。
            lsp::append_stream_capture_diagnostic_log(
                "session",
                "audio start returned session=" + context->admission.session_id); // wjy: 保留现有无返回值行为，仅确认调用已返回。
        }

        uu::SessionConfig sessionConfig;
        sessionConfig.role = uu::SessionRole::Host;
        sessionConfig.target_bitrate_kbps = config_.max_aggregate_video_kbps;
        // =====wjy====
        sessionConfig.host_video_source = context->media_subscription->source(); // wjy: 每个 PeerConnection 使用同一帧源，但后续仍创建独立 track、sender 和编码器。
        sessionConfig.media_id = context->admission.session_id; // wjy: 生成会话唯一的 WebRTC track/stream 标识。
        // ===end====
        lsp::append_stream_capture_diagnostic_log(
            "session",
            "WebrtcSession construct begin session=" + context->admission.session_id); // wjy: PeerConnection对象创建前写入阶段，构造异常时仍有最后检查点。
        context->webrtc = std::make_unique<uu::WebrtcSession>(&runtime_, sessionConfig);
        lsp::append_stream_capture_diagnostic_log(
            "session",
            "WebrtcSession construct success session=" + context->admission.session_id); // wjy: 对象创建完成后再进入回调和媒体配置。
        context->webrtc->set_signal_callback([weak = std::weak_ptr<HostClientSession>(context)](const std::string& kind, const std::string& body) {
            if (const auto locked = weak.lock()) locked->send(kind + "\n" + body); // wjy: 回调只持弱引用，会话销毁后不会访问悬空 socket。
        });
        // =====wjy====
        context->webrtc->set_connection_state_callback([weak = std::weak_ptr<HostClientSession>(context)](webrtc::PeerConnectionInterface::IceConnectionState state) {
            if (const auto locked = weak.lock()) {
                lsp::append_stream_capture_diagnostic_log(
                    "webrtc",
                    "ICE state session=" + locked->admission.session_id
                        + " value=" + std::to_string(static_cast<int>(state))); // wjy: new/checking/connected/completed/disconnected/failed/closed全部持久化，不再只记录终止状态。
            }
            if (state != webrtc::PeerConnectionInterface::kIceConnectionDisconnected
                && state != webrtc::PeerConnectionInterface::kIceConnectionFailed
                && state != webrtc::PeerConnectionInterface::kIceConnectionClosed) {
                return; // wjy: new/checking/connected/completed 都是正常建连或存活状态，不能误清理有效远控。
            }
            if (const auto locked = weak.lock(); locked && !locked->cancelled) {
                append_log("host ICE ended session=" + locked->admission.session_id
                    + " state=" + std::to_string(static_cast<int>(state))); // wjy: 记录 ICE 终止原因，排查异常退出后人数归零时序。
                locked->cancel(); // wjy: ICE 已断开时立即关闭信令 socket，使会话退出计数并唤醒阻塞中的 recv 循环。
            }
        });
        // ===end====
        if (has_control) {
            // =====wjy====
            input_dispatcher_.registerSession(
                context->admission.session_id,
                [weak = std::weak_ptr<HostClientSession>(context)](bool relative, const char* reason) {
                    if (const auto locked = weak.lock(); locked && !locked->cancelled) {
                        std::lock_guard webrtc_lock(locked->webrtc_mutex);
                        publish_mouse_mode(locked->webrtc.get(), relative, reason); // wjy: 任一控制端触发鼠标模式切换后安全广播给全部仍存活的协同控制窗口。
                    }
                },
                [weak = std::weak_ptr<HostClientSession>(context)](const std::string& encoded) {
                    // wjy: Host 剪贴板变化后推送到 Viewer：cb <base64>
                    if (const auto locked = weak.lock(); locked && !locked->cancelled) {
                        std::lock_guard webrtc_lock(locked->webrtc_mutex);
                        if (locked->webrtc) {
                            locked->webrtc->send_control_message(std::string("cb ") + encoded);
                        }
                    }
                },
                [weak = std::weak_ptr<HostClientSession>(context)](const char* backend, const char* state) {
                    if (const auto locked = weak.lock(); locked && !locked->cancelled) {
                        const std::string wire = std::string("__fsremote_mouse_backend_status ")
                            + backend + " " + state; // wjy: 固定三段状态消息让旧 Viewer 忽略、新 Viewer 严格解析，且不改动公开输入函数签名。
                        std::lock_guard webrtc_lock(locked->webrtc_mutex);
                        if (locked->webrtc) locked->webrtc->send_control_message(wire); // wjy: 后端确认与普通控制消息复用可靠有序 DataChannel。
                    }
                });
            // ===end====
        } // wjy: view_only 会话不登记输入回调，即使发送 data-channel 输入也无法到达系统注入路径。
        // =====wjy====
        input_dispatcher_.registerCursorSession(
            context->admission.session_id,
            [weak = std::weak_ptr<HostClientSession>(context)](uu::StandardCursorShape shape) -> bool {
                std::string wire;
                std::string error;
                if (!uu::serialize_cursor_shape_message(shape, &wire, &error)) return false; // wjy: 非法枚举不发送，Viewer 保持上一次有效光标。
                if (const auto locked = weak.lock(); locked && !locked->cancelled) {
                    std::lock_guard webrtc_lock(locked->webrtc_mutex);
                    return locked->webrtc && locked->webrtc->send_control_message(wire); // wjy: 控制与只读授权窗口都接收光标；通道未打开时返回 false 触发后续重试。
                }
                return false;
            });
        // ===end====
        // =====wjy====
        context->webrtc->set_control_callback(
            [this, weak = std::weak_ptr<HostClientSession>(context), has_control](const std::string& message) {
                const auto locked = weak.lock();
                if (!locked || locked->cancelled) return;

                if (uu::is_viewer_quality_message(message)) {
                    uu::ViewerQualityRequest request;
                    std::string parse_error;
                    if (!uu::parse_viewer_quality_request(message, &request, &parse_error)) {
                        append_log("host rejected malformed quality message session=" + locked->admission.session_id
                            + " error=" + parse_error); // wjy: 命中质量前缀的畸形消息只记录并丢弃，绝不送入SendInput。
                        return;
                    }

                    const uint32_t applied_bitrate = std::min(
                        request.max_bitrate_kbps,
                        config_.max_aggregate_video_kbps); // wjy: 单会话请求不能突破Host总发送硬上限，高质量锁定也只获得优先级而非无界码率。
                    uu::ViewerQualityAcknowledgement acknowledgement;
                    acknowledgement.request_id = request.request_id;
                    acknowledgement.applied_mode = request.mode;
                    acknowledgement.applied_width = request.target_width;
                    acknowledgement.applied_height = request.target_height;
                    acknowledgement.applied_fps = request.target_fps;
                    acknowledgement.applied_bitrate_kbps = applied_bitrate;
                    acknowledgement.limitation = applied_bitrate == request.max_bitrate_kbps
                        ? uu::ViewerQualityLimitation::None
                        : uu::ViewerQualityLimitation::Clamped;

                    std::string apply_error;
                    bool applied = false;
                    {
                        std::lock_guard webrtc_lock(locked->webrtc_mutex);
                        applied = locked->webrtc
                            && locked->webrtc->apply_sender_quality(
                                request.target_width,
                                request.target_height,
                                request.target_fps,
                                applied_bitrate,
                                request.priority,
                                &apply_error); // wjy: 在线SetParameters只调整当前会话sender，媒体源、输入、认证和PeerConnection全部保持连续。
                        if (!applied) {
                            acknowledgement.limitation = uu::ViewerQualityLimitation::ApplyFailed;
                        }
                        std::string acknowledgement_wire;
                        std::string acknowledgement_error;
                        if (locked->webrtc
                            && uu::serialize_viewer_quality_acknowledgement(
                                acknowledgement,
                                &acknowledgement_wire,
                                &acknowledgement_error)) {
                            locked->webrtc->send_control_message(acknowledgement_wire); // wjy: 无论成功、夹紧或非致命失败都返回版本化实际结果，现有视频流不停止。
                        }
                    }
                    if (!applied) {
                        append_log("host quality apply failed session=" + locked->admission.session_id
                            + " error=" + apply_error);
                    }
                    return;
                }

                if (has_control) {
                    input_dispatcher_.dispatch(locked->admission.session_id, message); // wjy: 只有非质量消息且拥有控制权时才进入键鼠/剪贴板串行注入路径。
                }
            });
        // ===end====
        lsp::append_stream_capture_diagnostic_log(
            "session",
            "WebRTC initialize begin session=" + context->admission.session_id); // wjy: 若初始化内部卡住，日志会停在本行并与编码/VDD日志交叉定位。
        const auto webrtc_initialize_started = std::chrono::steady_clock::now();
        const bool webrtc_initialized = context->webrtc->initialize(&error);
        const auto webrtc_initialize_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - webrtc_initialize_started).count();
        lsp::append_stream_capture_diagnostic_log(
            "session",
            "WebRTC initialize end session=" + context->admission.session_id
                + " ok=" + std::to_string(webrtc_initialized ? 1 : 0)
                + " elapsed_ms=" + std::to_string(webrtc_initialize_ms)
                + " error=" + error); // wjy: CreatePeerConnection、Track、Sender和编码器初始化的整体结果持久化。

        bool offer_started = false;
        if (webrtc_initialized) {
            lsp::append_stream_capture_diagnostic_log(
                "session",
                "WebRTC offer begin session=" + context->admission.session_id); // wjy: SDP创建和媒体初始化分开计时，避免都归为“等待画面”。
            const auto offer_started_at = std::chrono::steady_clock::now();
            offer_started = context->webrtc->start_offer(&error);
            const auto offer_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - offer_started_at).count();
            lsp::append_stream_capture_diagnostic_log(
                "session",
                "WebRTC offer end session=" + context->admission.session_id
                    + " ok=" + std::to_string(offer_started ? 1 : 0)
                    + " elapsed_ms=" + std::to_string(offer_elapsed_ms)
                    + " error=" + error); // wjy: offer返回结果和耗时与ICE状态一起判断是否进入真正媒体传输。
        }

        if (webrtc_initialized && offer_started) {
            std::string message;
            while (running_ && !context->cancelled && uu::recv_message(socket, &message)) {
                handle_message(*context->webrtc, message);
            }
        } else {
            append_log("host session init/start failed session=" + context->admission.session_id + " error=" + error);
        }
        lsp::append_stream_capture_diagnostic_log(
            "session",
            "session teardown begin session=" + context->admission.session_id
                + " cancelled=" + std::to_string(context->cancelled ? 1 : 0)); // wjy: 信令循环结束后先记录会话退出原因状态，再依次释放PeerConnection和媒体订阅。
        std::unique_ptr<uu::WebrtcSession> retiring_webrtc;
        {
            std::lock_guard webrtc_lock(context->webrtc_mutex);
            retiring_webrtc = std::move(context->webrtc); // wjy: 先在锁内摘除可广播指针，让后续模式通知立即跳过该会话。
        }
        retiring_webrtc.reset(); // wjy: 在锁外执行 PeerConnection 析构，避免析构等待 data-channel 回调时与模式通知锁形成互等。
        if (has_control) input_dispatcher_.releaseSession(context->admission.session_id); // wjy: 销毁输入回调后释放该会话独有的键、按钮和相对坐标状态。
        input_dispatcher_.releaseCursorSession(context->admission.session_id); // wjy: 无论控制或只读会话都精确注销光标订阅，避免后台轮询继续尝试已关闭通道。
        context->media_subscription.reset(); // wjy: 先销毁 PeerConnection/track，再减少共享 source 订阅计数，避免捕获线程提前停止。
        lsp::append_stream_capture_diagnostic_log(
            "session",
            "media subscription released session=" + context->admission.session_id); // wjy: 确认最后订阅是否触发管线、DXGI和VDD停止日志。
        context->cancel();
        if (owns_audio) stopAudioForSingleSession(); // wjy: 非音频主会话退出不能停止首个会话仍在使用的音频连接。
        releaseAdmissionClaims(context);
        context->completed = true;
        append_log("host client worker completed session=" + context->admission.session_id);
        lsp::append_stream_capture_diagnostic_log(
            "session",
            "session teardown end session=" + context->admission.session_id); // wjy: 会话全部本地状态清理完成后写入最终终点。
    }

    void releaseAdmissionClaims(const std::shared_ptr<HostClientSession>& context)
    {
        if (!context) return;
        if (context->admission.exclusive_control_slot) {
            bool expected = true;
            exclusive_control_claimed_.compare_exchange_strong(expected, false);
            context->admission.exclusive_control_slot = false; // wjy: 只有显式独占策略会归还该槽，协同会话之间没有全局控制者竞争。
        }
        if (context->admission.audio_primary) {
            bool expected = true;
            audio_primary_claimed_.compare_exchange_strong(expected, false);
            context->admission.audio_primary = false; // wjy: 单路音频槽独立归还，不能顺带撤销其他会话的键鼠权限。
        }
        context->admission.ownership = "view_only"; // wjy: 本会话结束后本地状态降为只读，防止重复清理。
    }

    void reapCompletedSessions()
    {
        std::vector<std::shared_ptr<HostClientSession>> completed;
        {
            std::lock_guard lock(sessions_mutex_);
            for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
                if (!iterator->second->completed) {
                    ++iterator;
                    continue;
                }
                completed.push_back(iterator->second);
                iterator = sessions_.erase(iterator);
            }
        }
        for (const auto& session : completed) {
            if (session->worker.joinable()) session->worker.join(); // wjy: 只由 manager/关闭线程 join，worker 从不 join 自己。
        }
    }

    void startAudioForSingleSession()
    {
        std::lock_guard lock(audio_mutex_);
        if (audio_streamer_) return;
        std::string ignored;
        audio_streamer_ = std::make_unique<uu::HostAudioStreamer>();
        audio_streamer_->start(49105, &ignored); // wjy: 并发控制阶段仅为独立选出的音频主会话沿用单客户端音频，后续由 HostAudioHub 替换。
    }

    void stopAudioForSingleSession()
    {
        std::lock_guard lock(audio_mutex_);
        if (!audio_streamer_) return;
        audio_streamer_->resetClient();
        audio_streamer_->stop();
        audio_streamer_.reset();
    }

    void shutdownSessionsOnManagerThread()
    {
        append_log("host manager session shutdown begin");
        std::vector<std::shared_ptr<HostClientSession>> sessions;
        {
            std::lock_guard lock(sessions_mutex_);
            for (const auto& [id, session] : sessions_) sessions.push_back(session);
            sessions_.clear();
        }
        append_log("host manager session shutdown extracted count=" + std::to_string(sessions.size()));
        for (const auto& session : sessions) session->cancel();
        append_log("host manager session shutdown cancelled");
        for (const auto& session : sessions) {
            if (session->worker.joinable()) session->worker.join(); // wjy: manager 线程是剩余会话 worker 的唯一 join 所有者。
        }
        append_log("host manager session shutdown joined");
        input_dispatcher_.shutdown(); // wjy: 所有会话 worker 结束后再次兜底释放残留输入，随后才销毁媒体和 WebRTC 资源。
        append_log("host manager input dispatcher shutdown");
        stopAudioForSingleSession();
        append_log("host manager session shutdown audio stopped");
        media_pipeline_.shutdown(); // wjy: 全部会话 worker 已 join、订阅已释放后，禁止新订阅并确认共享桌面管线停止。
        append_log("host manager media pipeline shutdown");
        append_log("host manager resources shutdown");
    }

    // =====wjy====
    void runHostStatePollLoop()
    {
        int clipboardCountdown = 0;
        while (running_) {
            input_dispatcher_.pollLockScreenEnter(); // wjy: 新远控打开后的锁屏检测与 Bridge 就绪等待独立于信令线程，不让画面建立过程阻塞。
            input_dispatcher_.pollHostCursor(); // wjy: 25ms 采样使应用边缘缩放光标变化接近本地 Windows 反馈速度。
            if (clipboardCountdown <= 0) {
                input_dispatcher_.pollHostClipboard();
                clipboardCountdown = 14; // wjy: 14×25ms 保持原约 350ms 剪贴板轮询节奏，不因光标同步增加剪贴板流量。
            }
            --clipboardCountdown;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
    // ===end====

    void shutdown()
    {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) return;
        // =====wjy====
        append_log("host shutdown accept worker join begin"); // wjy: 这里只发布 running_=false；Accept 管理线程最多在 200ms select 超时后自行关闭 listener 和全部会话。
        if (worker_.joinable()) worker_.join(); // wjy: listener 单线程所有权消除跨线程关闭竞态，同时保持 manager 仍是会话 worker 的唯一 join 所有者。
        append_log("host shutdown accept worker joined");
        // ===end====
        // =====wjy====
        if (host_state_worker_.joinable()) host_state_worker_.join(); // wjy: 光标/剪贴板状态线程在 accept worker 之后 join，避免关闭竞态。
        // ===end====
        runtime_.shutdown(); // wjy: 全部会话对象销毁后，由 HostInstance 创建线程配对关闭共享 WebRTC/SSL runtime。
        append_log("host shutdown completed");
    }

    HostRuntimeConfig config_;
    uu::NativeWebrtcRuntime runtime_; // wjy: 全部 HostClientSession 共用一个进程内 WebRTC runtime。
    uu::HostMediaPipeline media_pipeline_; // wjy: manager 独占 VDD/桌面捕获启停权，会话只能通过订阅令牌引用共享 source。
    InputDispatcher input_dispatcher_; // wjy: HostInstance 唯一持有输入调度器，三个会话共享同一确定性 SendInput 顺序。
    // =====wjy====
    std::thread host_state_worker_;
    // ===end====
    mutable std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<HostClientSession>> sessions_;
    std::mutex audio_mutex_;
    std::unique_ptr<uu::HostAudioStreamer> audio_streamer_;
    std::atomic_bool exclusive_control_claimed_ = false; // wjy: 仅供显式旧独占策略兼容使用，默认协同策略不会占用。
    std::atomic_bool audio_primary_claimed_ = false; // wjy: HostAudioHub 完成前限制单路音频客户端，但不限制协同键鼠控制。
};
// ===end====

class ViewerInstance final : public StreamInstance {
    // =====wjy====
    struct PerformanceState {
        std::mutex mutex;
        FsRemoteViewerPerformanceStats stats = {};
        bool available = false;
    }; // wjy: getStats 回调只捕获独立共享状态，ViewerInstance 关闭时不会留下悬空 this 指针。
    // ===end====

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
        worker_ = std::thread([this] {
            try {
                run(); // wjy: 每个Viewer最外层捕获分配、运行和第三方库异常，20路中单路失败只反馈当前窗口。
            } catch (const std::bad_alloc&) {
                set_error("viewer allocation failed");
                append_viewer_log("viewer worker bad_alloc");
                report_status(status_callback_, user_, FSREMOTE_STATUS_ERROR, "Viewer memory allocation failed");
            } catch (const std::exception& exception) {
                set_error(exception.what());
                append_viewer_log(std::string("viewer worker exception: ") + exception.what());
                report_status(status_callback_, user_, FSREMOTE_STATUS_ERROR, exception.what());
            } catch (...) {
                set_error("viewer unknown runtime failure");
                append_viewer_log("viewer worker unknown exception");
                report_status(status_callback_, user_, FSREMOTE_STATUS_ERROR, "Viewer runtime failure");
            }
            {
                std::lock_guard lock(audio_mutex_);
                audio_capability_available_ = false;
                stopAudioPlayerLocked(); // wjy: run任意正常返回或异常出口都关闭音频，单路视频失败不会留下后台播放器持续重连。
            }
        });
    }

    ~ViewerInstance() override
    {
        // =====wjy====
        stop(); // wjy: close sockets and join the worker before viewer members can be freed.
        {
            std::lock_guard lock(audio_mutex_); // wjy: worker退出后再串行停止播放器，避免与准入完成时的延后启动并发。
            stopAudioPlayerLocked();
        }
        // ===end====
    }

    bool sendInput(const char* message) override
    {
        if (!message || !*message || !control_allowed_) { // wjy: view_only 客户端在进入 WebRTC 发送路径前直接拒绝本地键鼠消息。
            return false;
        }
        std::lock_guard lock(session_mutex_);
        return active_session_ ? active_session_->send_control_message(message) : false;
    }

    // =====wjy====
    bool setViewerQuality(const FsRemoteViewerQualityConfig* config) override
    {
        if (!config || config->struct_size < sizeof(FsRemoteViewerQualityConfig)
            || config->version != 1 || config->request_id == 0) {
            return false;
        }
        uu::ViewerQualityRequest request;
        request.request_id = config->request_id;
        request.mode = static_cast<uu::ViewerQualityMode>(config->mode);
        request.target_width = config->target_width;
        request.target_height = config->target_height;
        request.target_fps = config->target_fps;
        request.max_bitrate_kbps = config->max_bitrate_kbps;
        request.priority = config->priority;
        std::string wire;
        std::string error;
        if (!uu::serialize_viewer_quality_request(request, &wire, &error)) {
            set_error(error);
            return false; // wjy: C ABI入口复用同一协议校验，非法尺寸/FPS绝不进入data-channel。
        }
        {
            std::lock_guard lock(quality_mutex_);
            pending_quality_request_ = request; // wjy: 新请求直接覆盖尚未发送或等待确认的旧请求，队列深度始终为1。
            pending_quality_sent_ = false;
            pending_quality_sent_at_ms_ = 0;
            quality_status_available_ = false;
        }
        trySendPendingQualityRequest(); // wjy: data-channel已打开时立即在线发送；尚未打开则由首帧回调自动补发最新值。
        return true;
    }

    bool setViewerAudioEnabled(bool enabled) override
    {
        audio_enabled_requested_.store(enabled, std::memory_order_release); // wjy: UI焦点线程只发布最新意图，不积压多次焦点切换任务。
        std::lock_guard lock(audio_mutex_);
        return applyRequestedAudioStateLocked(); // wjy: 播放器start/stop严格串行，连接准入前只缓存状态不创建线程。
    }

    bool viewerQualityStatus(FsRemoteViewerQualityStatus* status) const override
    {
        if (!status || status->struct_size < sizeof(FsRemoteViewerQualityStatus) || status->version != 1) {
            return false;
        }
        std::lock_guard lock(quality_mutex_);
        if (!quality_status_available_) return false;
        *status = quality_status_; // wjy: 在互斥区内复制固定POD快照，Qt不会读到一半新一半旧的确认字段。
        return true;
    }

    bool viewerPerformanceStats(FsRemoteViewerPerformanceStats* stats) const override
    {
        if (!stats || stats->struct_size < sizeof(FsRemoteViewerPerformanceStats) || stats->version != 1) {
            return false;
        }
        const std::shared_ptr<PerformanceState> state = performance_state_;
        std::lock_guard lock(state->mutex);
        if (!state->available) return false;
        *stats = state->stats; // wjy: 固定 POD 在互斥区内一次复制，UI 不会混读两个 WebRTC 采样周期。
        return true;
    }
    // ===end====

private:
    // =====wjy====
    bool applyRequestedAudioStateLocked()
    {
        if (!audio_capability_available_) {
            return true; // wjy: 准入结果尚未确认audio能力时接受请求并延迟应用，视频初始化不受影响。
        }
        if (!audio_enabled_requested_.load(std::memory_order_acquire)) {
            stopAudioPlayerLocked();
            return true;
        }
        if (audio_playing_) {
            return true; // wjy: 相同焦点状态重复下发时不创建第二条音频线程或第二个socket。
        }
        if (!audio_player_) {
            audio_player_ = std::make_unique<uu::ViewerAudioPlayer>(); // wjy: 只有真正成为音频所有者后才分配播放器对象。
        }
        std::string audio_error;
        if (!audio_player_->start(host_ip_, 49105, &audio_error)) {
            set_error(audio_error.empty() ? "viewer audio start failed" : audio_error);
            append_viewer_log("viewer audio start failed error=" + audio_error);
            return false; // wjy: 音频失败只返回给可选ABI，现有视频和控制会话继续运行。
        }
        audio_playing_ = true;
        append_viewer_log("viewer audio enabled host=" + host_ip_); // wjy: 只记录状态跃迁，不按音频包写日志。
        return true;
    }

    void stopAudioPlayerLocked()
    {
        if (!audio_player_ || !audio_playing_) {
            return;
        }
        audio_player_->stop(); // wjy: 关闭socket后join唯一音频线程，返回时后台设备不再播放本Viewer声音。
        audio_playing_ = false;
        append_viewer_log("viewer audio disabled host=" + host_ip_); // wjy: 焦点离开只产生一条低频状态日志。
    }

    void requestPerformanceStats()
    {
        const uint64_t now = ::GetTickCount64();
        uint64_t previous = last_performance_stats_request_ms_.load(std::memory_order_acquire);
        if (now - previous < 1000
            || !last_performance_stats_request_ms_.compare_exchange_strong(
                previous, now, std::memory_order_acq_rel)) {
            return; // wjy: 纹理/BGRA 两条回调共用一秒采样门，任何时刻都不会按帧调用 getStats。
        }

        const std::shared_ptr<PerformanceState> output = performance_state_;
        std::lock_guard lock(session_mutex_);
        if (!active_session_) return;
        active_session_->request_receiver_performance_stats(
            [output](const uu::ReceiverPerformanceStats& input) {
                FsRemoteViewerPerformanceStats snapshot = {};
                snapshot.struct_size = sizeof(snapshot);
                snapshot.version = 1;
                snapshot.sample_time_ms = input.sample_time_ms;
                snapshot.frames_received = input.frames_received;
                snapshot.frames_decoded = input.frames_decoded;
                snapshot.frames_dropped = input.frames_dropped;
                snapshot.freeze_count = input.freeze_count;
                snapshot.jitter_buffer_emitted_count = input.jitter_buffer_emitted_count;
                snapshot.packets_received = input.packets_received;
                snapshot.packets_lost = input.packets_lost;
                snapshot.total_decode_time_ms = input.total_decode_time_ms;
                snapshot.total_processing_delay_ms = input.total_processing_delay_ms;
                snapshot.total_freezes_duration_ms = input.total_freezes_duration_ms;
                snapshot.total_jitter_buffer_delay_ms = input.total_jitter_buffer_delay_ms;
                snapshot.round_trip_time_ms = input.round_trip_time_ms;
                snapshot.available_incoming_bitrate_kbps = input.available_incoming_bitrate_kbps;
                std::lock_guard stats_lock(output->mutex);
                output->stats = snapshot;
                output->available = true; // wjy: 首份完整异步报告到达后才允许 UI 读取，避免把全零初始化误判为健康样本。
            });
    }

    void trySendPendingQualityRequest()
    {
        uu::ViewerQualityRequest request;
        {
            std::lock_guard lock(quality_mutex_);
            if (!pending_quality_request_ || pending_quality_sent_) return;
            request = *pending_quality_request_;
        }
        std::string wire;
        std::string error;
        if (!uu::serialize_viewer_quality_request(request, &wire, &error)) return;
        bool sent = false;
        {
            std::lock_guard lock(session_mutex_);
            sent = active_session_ && active_session_->send_control_message(wire); // wjy: 复用现有可靠有序control data-channel，不触碰TCP信令或重建会话。
        }
        if (!sent) return;
        std::lock_guard lock(quality_mutex_);
        if (pending_quality_request_ && pending_quality_request_->request_id == request.request_id) {
            pending_quality_sent_ = true;
            pending_quality_sent_at_ms_ = GetTickCount64(); // wjy: 只给当前最新请求记录超时，迟到旧请求不能把新请求标成已发送。
        }
    }

    void checkPendingQualityTimeout()
    {
        uint64_t timed_out_request_id = 0;
        {
            std::lock_guard lock(quality_mutex_);
            if (!pending_quality_request_ || !pending_quality_sent_ || pending_quality_sent_at_ms_ == 0
                || GetTickCount64() - pending_quality_sent_at_ms_ < 3000) {
                return;
            }
            const uu::ViewerQualityRequest request = *pending_quality_request_;
            quality_status_ = {};
            quality_status_.struct_size = sizeof(quality_status_);
            quality_status_.version = 1;
            quality_status_.request_id = request.request_id;
            quality_status_.supported = 0;
            quality_status_.applied_mode = static_cast<uint32_t>(request.mode);
            quality_status_.applied_width = request.target_width;
            quality_status_.applied_height = request.target_height;
            quality_status_.applied_fps = request.target_fps;
            quality_status_.applied_bitrate_kbps = request.max_bitrate_kbps;
            quality_status_.limitation = FSREMOTE_VIEWER_QUALITY_LIMIT_UNSUPPORTED; // wjy: 旧Host三秒无确认只标记不支持，视频、输入和连接全部继续。
            quality_status_available_ = true;
            timed_out_request_id = request.request_id;
            pending_quality_request_.reset();
            pending_quality_sent_ = false;
        }
        if (timed_out_request_id != 0) {
            const std::string message = "QUALITY " + std::to_string(timed_out_request_id);
            report_status(status_callback_, user_, FSREMOTE_STATUS_QUALITY_APPLIED, message.c_str());
        }
    }

    void acceptQualityAcknowledgement(const uu::ViewerQualityAcknowledgement& acknowledgement)
    {
        {
            std::lock_guard lock(quality_mutex_);
            if (!pending_quality_request_
                || pending_quality_request_->request_id != acknowledgement.request_id) {
                return; // wjy: 用户连续切换模式时，旧Host确认不能覆盖更新请求的标题栏状态。
            }
            quality_status_ = {};
            quality_status_.struct_size = sizeof(quality_status_);
            quality_status_.version = 1;
            quality_status_.request_id = acknowledgement.request_id;
            quality_status_.supported = acknowledgement.supported ? 1u : 0u;
            quality_status_.applied_mode = static_cast<uint32_t>(acknowledgement.applied_mode);
            quality_status_.applied_width = acknowledgement.applied_width;
            quality_status_.applied_height = acknowledgement.applied_height;
            quality_status_.applied_fps = acknowledgement.applied_fps;
            quality_status_.applied_bitrate_kbps = acknowledgement.applied_bitrate_kbps;
            quality_status_.limitation = static_cast<uint32_t>(acknowledgement.limitation);
            quality_status_available_ = true;
            pending_quality_request_.reset();
            pending_quality_sent_ = false;
            pending_quality_sent_at_ms_ = 0;
        }
        const std::string message = "QUALITY " + std::to_string(acknowledgement.request_id);
        report_status(status_callback_, user_, FSREMOTE_STATUS_QUALITY_APPLIED, message.c_str()); // wjy: 现有状态回调只作“有新快照”通知，实际字段通过类型化C ABI读取。
    }
    // ===end====

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
        report_status(status_callback_, user_, FSREMOTE_STATUS_CONNECTING_TCP, "Connecting TCP");
        const uintptr_t socket = connectTcp(&error);
        if (!socket || !running_) {
            append_viewer_log("viewer tcp failed running=" + std::to_string(running_.load()) + " error=" + error); // wjy: separate TCP failure from WebRTC/media crashes.
            if (socket) uu::close_socket(socket);
            report_status(status_callback_, user_, FSREMOTE_STATUS_ERROR, error.empty() ? "TCP connection failed" : error.c_str());
            return;
        }

        append_viewer_log("viewer tcp connected socket=" + std::to_string(socket)); // wjy: mark successful TCP signaling connection before audio/WebRTC start.
        report_status(status_callback_, user_, FSREMOTE_STATUS_TCP_CONNECTED, "TCP connected");
        // =====wjy====
        set_socket_timeout(socket, 5000);
        if (!perform_viewer_admission(socket, status_callback_, user_, &admission_, &error)) {
            append_viewer_log("viewer admission failed error=" + error); // wjy: 认证失败不启动音频、不初始化 WebRTC，UI 保留具体拒绝状态。
            return;
        }
        control_allowed_ = admission_.ownership == "control_granted"; // wjy: Viewer 本地同步执行准入权限，view_only 窗口不再向 data channel 转发键鼠消息。
        set_socket_timeout(socket, 0);
        append_viewer_log("viewer admitted session=" + admission_.session_id
            + " version=" + std::to_string(admission_.protocol_version)
            + " capabilities=" + admission_.capabilities
            + " ownership=" + admission_.ownership); // wjy: ViewerInstance 持久保存并记录服务端确认的身份、能力、版本与权限结果。
        // ===end====
        if (has_capability(admission_.capabilities, "audio")) {
            std::lock_guard lock(audio_mutex_);
            audio_capability_available_ = true; // wjy: Host确认能力后只应用此前缓存的最新焦点意图，默认不抢占后台音频。
            if (!applyRequestedAudioStateLocked()) {
                append_viewer_log("viewer deferred audio state apply failed"); // wjy: 音频失败保留诊断但不阻断后续WebRTC视频初始化。
            }
        }
        report_status(status_callback_, user_, FSREMOTE_STATUS_INITIALIZING_WEBRTC, "Initializing WebRTC");
        uu::NativeWebrtcRuntime runtime;
        std::atomic_bool frameReported = false;
        runtime.set_decoded_texture_callback([this, &frameReported](int width, int height, void* shared_handle, uint64_t frame_id, double encoded_mbps) -> int {
            if (!texture_callback_ || !running_ || !shared_handle) {
                return FSREMOTE_TEXTURE_FRAME_FALLBACK; // wjy: 无纹理接收方时保留原有BGRA软件回退。
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
            requestPerformanceStats(); // wjy: 低频拉取标准接收统计，为原因感知降帧提供解码、网络和冻结证据。
            trySendPendingQualityRequest(); // wjy: data-channel通常在首帧前后打开，未发送请求在每帧以单槽状态低成本重试。
            checkPendingQualityTimeout();
            return texture_callback_(user_, width, height, shared_handle, frame_id, encoded_mbps); // wjy: 原样传递接受/回退/受控丢帧，不能把结果2压缩成bool。
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
            requestPerformanceStats(); // wjy: 软件回退路径同样采集 WebRTC 接收统计，性能判断不依赖 D3D11 是否可用。
            trySendPendingQualityRequest();
            checkPendingQualityTimeout(); // wjy: BGRA回退路径和共享纹理路径使用相同质量发送/超时语义。
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
        // =====wjy====
        session.set_connection_state_callback([this](webrtc::PeerConnectionInterface::IceConnectionState state) {
            if (state == webrtc::PeerConnectionInterface::kIceConnectionDisconnected) {
                if (!running_ || ice_disconnected_.exchange(true)) {
                    return; // wjy: 同一次断网只上报一次，重复Disconnected不重启Qt侧三秒计时器。
                }
                append_viewer_log("viewer ICE temporarily disconnected");
                report_status(
                    status_callback_, user_, FSREMOTE_STATUS_NETWORK_UNSTABLE, "ICE temporarily disconnected"); // wjy: 保留PeerConnection和信令socket，让WebRTC先尝试使用现有会话自恢复。
                return;
            }
            if (state == webrtc::PeerConnectionInterface::kIceConnectionConnected
                || state == webrtc::PeerConnectionInterface::kIceConnectionCompleted) {
                if (ice_disconnected_.exchange(false) && running_) {
                    append_viewer_log("viewer ICE connectivity restored; waiting video");
                    report_status(
                        status_callback_, user_, FSREMOTE_STATUS_NETWORK_RECOVERING, ""); // wjy: ICE恢复不等于画面恢复，Qt使用统一中文状态并保留警告直到真正呈现新帧。
                }
                return;
            }
            if (state != webrtc::PeerConnectionInterface::kIceConnectionFailed
                && state != webrtc::PeerConnectionInterface::kIceConnectionClosed) {
                return; // wjy: new/checking等中间状态不改变现有网络恢复流程。
            }
            ice_disconnected_.store(false);
            bool expectedRunning = true;
            if (!running_.compare_exchange_strong(expectedRunning, false)) {
                return; // wjy: 手动关闭或更早的ICE终态已经接管清理时，不重复通知和关闭socket。
            }
            append_viewer_log(
                "viewer ICE ended state=" + std::to_string(static_cast<int>(state)));
            report_status(
                status_callback_, user_, FSREMOTE_STATUS_REMOTE_CLOSED, "Remote network connection lost"); // wjy: recv循环会因running=false跳过尾部通知，因此在ICE线程明确投递一次断线状态。
            const uintptr_t current = socket_.exchange(0);
            if (current) {
                uu::close_socket(current); // wjy: shutdown+closesocket解除recv_message阻塞，唤醒Viewer worker退出。
            }
        });
        // ===end====
        session.set_signal_callback([socket](const std::string& kind, const std::string& body) {
            append_viewer_log("viewer send signaling kind=" + kind + " body_size=" + std::to_string(body.size())); // wjy: show local offer/answer/candidate egress.
            uu::send_message(socket, kind + "\n" + body);
        });
        session.set_control_callback([this](const std::string& message) {
            if (uu::is_viewer_quality_message(message)) {
                uu::ViewerQualityAcknowledgement acknowledgement;
                std::string error;
                if (uu::parse_viewer_quality_acknowledgement(message, &acknowledgement, &error)) {
                    acceptQualityAcknowledgement(acknowledgement); // wjy: 质量确认和鼠标/剪贴板共享data-channel，但使用保留前缀严格分流。
                }
            } else if (message == "__fsremote_mouse_mode relative") {
                report_status(status_callback_, user_, 61, "MOUSE relative");
            } else if (message == "__fsremote_mouse_mode desktop") {
                report_status(status_callback_, user_, 61, "MOUSE desktop");
            } else if (message == "__fsremote_mouse_backend_status system ready"
                || message == "__fsremote_mouse_backend_status faker ready"
                || message == "__fsremote_mouse_backend_status system fallback"
                // =====wjy====
                || message == "__fsremote_mouse_backend_status system installing") {
                report_status(status_callback_, user_, FSREMOTE_STATUS_MOUSE_BACKEND, message.c_str()); // wjy: 安装中只允许 system installing，驱动未 ready 前绝不伪造 faker 状态。
                // ===end====
            } else if (uu::is_cursor_shape_message(message)) {
                uu::StandardCursorShape shape = uu::StandardCursorShape::Arrow;
                std::string error;
                if (uu::parse_cursor_shape_message(message, &shape, &error)) {
                    report_status(status_callback_, user_, FSREMOTE_STATUS_CURSOR_SHAPE, message.c_str()); // wjy: 严格解析通过后才把版本化消息交给 Qt，畸形 token 不改变当前光标。
                }
            } else if (message.rfind("cb ", 0) == 0) {
                // wjy: Host -> Viewer 文本剪贴板：通过状态回调把 payload 交给 Qt 层写入本机剪贴板。
                report_status(status_callback_, user_, 62, message.c_str());
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
        trySendPendingQualityRequest(); // wjy: 会话初始化完成立即尝试，若channel尚未open则由首帧继续补发。

        append_viewer_log("viewer session initialized waiting stream"); // wjy: last checkpoint before remote signaling/media loop.
        report_status(status_callback_, user_, 40, "Waiting for remote stream");
        std::string message;
        while (running_ && uu::recv_message(socket, &message)) {
            append_viewer_log("viewer recv raw signaling size=" + std::to_string(message.size())); // wjy: each inbound signaling packet before dispatch.
            handle_message(session, message);
        }
        append_viewer_log("viewer recv loop end running=" + std::to_string(running_.load())); // wjy: distinguish clean remote close from native crash.
        const uintptr_t remainingSocket = socket_.exchange(0);
        if (remainingSocket) {
            uu::close_socket(remainingSocket); // wjy: 远端正常关闭或recv失败后立即清除句柄，析构时不会重复关闭已经结束的连接。
        }
        {
            std::lock_guard lock(session_mutex_);
            active_session_ = nullptr;
        }
        {
            std::lock_guard lock(audio_mutex_);
            audio_capability_available_ = false; // wjy: 信令会话结束后禁止迟到焦点请求重新启动独立音频连接。
            stopAudioPlayerLocked();
        }
        const bool loopWasRunning = running_.exchange(false); // wjy: 信令循环自然结束后锁存终态，避免随后PeerConnection析构触发ICE closed重复通知。
        if (loopWasRunning) {
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
    SessionAdmission admission_; // wjy: 准入结果生命周期覆盖整个 ViewerInstance，供后续音频令牌、控制权和状态处理复用。
    std::atomic_bool control_allowed_ = false; // wjy: 原子权限位跨 UI 与 viewer worker 线程读取，避免直接并发访问 admission_ 字符串。
    std::atomic_bool ice_disconnected_ = false; // wjy: 去重同一轮ICE断开/恢复通知，避免网络线程高频回调反复重置UI计时器。
    std::mutex session_mutex_;
    uu::WebrtcSession* active_session_ = nullptr;
    mutable std::mutex quality_mutex_;
    std::optional<uu::ViewerQualityRequest> pending_quality_request_; // wjy: 只保留最新请求，连接中多次修改不会积压消息。
    bool pending_quality_sent_ = false;
    uint64_t pending_quality_sent_at_ms_ = 0;
    FsRemoteViewerQualityStatus quality_status_ = {};
    bool quality_status_available_ = false;
    std::shared_ptr<PerformanceState> performance_state_ = std::make_shared<PerformanceState>(); // wjy: 统计回调与 Viewer 生命周期解耦，停止时无需等待 UI 读取者。
    std::atomic_uint64_t last_performance_stats_request_ms_ = 0; // wjy: 两条解码回调共用无锁限频时间戳。
    uint64_t last_stats_status_ms_ = 0;
    std::atomic_bool audio_enabled_requested_ = false; // wjy: 新Viewer默认静音，协调器明确授予真实焦点后才允许启动本地播放。
    std::mutex audio_mutex_; // wjy: UI焦点切换、Viewer准入和析构三条路径共享同一播放器所有权门禁。
    bool audio_capability_available_ = false;
    bool audio_playing_ = false;
    std::unique_ptr<uu::ViewerAudioPlayer> audio_player_;
};

} // namespace

FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_host(uint16_t port)
{
    // =====wjy====
    install_crash_logger(); // wjy: host also installs the crash logger because FSRemote can host and view in one process.
    lsp::reset_stream_capture_diagnostic_log(); // wjy: 旧API启动Host时也为本轮复现创建全新的目标端采集诊断文件。
    lsp::append_stream_capture_diagnostic_log(
        "host",
        "start requested port=" + std::to_string(port)
            + " api=legacy"); // wjy: 日志首行记录Host入口和监听端口，确认部署DLL已经包含本次诊断版本。
    append_viewer_log("api start_host port=" + std::to_string(port));
    // ===end====
    try {
        return new HostInstance(port, normalized_host_config(nullptr)); // wjy: 旧入口与 Qt 配置入口保持一致，无额外配置时默认允许三路视频会话。
    } catch (const std::exception& ex) {
        set_error(ex.what());
        return nullptr;
    }
}

// =====wjy====
void FSREMOTE_STREAM_CALL fsremote_stream_set_identity_callbacks(const FsRemoteIdentityCallbacks* callbacks)
{
    std::lock_guard lock(g_identity_callbacks_mutex);
    g_identity_callbacks = {};
    if (!callbacks || callbacks->struct_size < sizeof(FsRemoteIdentityCallbacks) || callbacks->version != 1) {
        append_log("identity callbacks cleared");
        return; // wjy: 未知或截断函数表不进入工作线程，避免调用越界函数指针。
    }
    g_identity_callbacks = *callbacks; // wjy: 只复制固定大小函数表，不保存调用方临时结构体地址。
    append_log("identity callbacks registered");
}
// ===end====

// =====wjy====
FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_host_with_config(uint16_t port, const FsRemoteHostConfig* config)
{
    install_crash_logger();
    const HostRuntimeConfig normalized = normalized_host_config(config); // wjy: DLL 在返回前复制并夹紧全部字段，调用方随后可安全释放配置结构。
    lsp::reset_stream_capture_diagnostic_log(); // wjy: Qt正式配置入口启动Host时截断旧日志，只保留当前进程和本轮远控复现。
    lsp::append_stream_capture_diagnostic_log(
        "host",
        "start requested port=" + std::to_string(port)
            + " api=config"
            + " sessions=" + std::to_string(normalized.effective_max_sessions)
            + " aggregate_kbps=" + std::to_string(normalized.max_aggregate_video_kbps)); // wjy: 首行保存Host关键配置，后续编码码率可与其核对。
    append_log("host config requested_sessions=" + std::to_string(normalized.requested_max_sessions)
        + " effective_sessions=" + std::to_string(normalized.effective_max_sessions)
        + " aggregate_kbps=" + std::to_string(normalized.max_aggregate_video_kbps)
        + " handshake_ms=" + std::to_string(normalized.handshake_timeout_ms));
    try {
        return new HostInstance(port, normalized);
    } catch (const std::exception& ex) {
        set_error(ex.what());
        return nullptr;
    }
}
// ===end====

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

// =====wjy====
int FSREMOTE_STREAM_CALL fsremote_stream_set_viewer_quality(
    FsRemoteStreamHandle handle,
    const FsRemoteViewerQualityConfig* config)
{
    if (!handle || !config) return 0;
    return static_cast<StreamInstance*>(handle)->setViewerQuality(config) ? 1 : 0; // wjy: Viewer同步复制最新配置，Host或错误句柄安全拒绝且不影响现有流。
}

int FSREMOTE_STREAM_CALL fsremote_stream_set_viewer_audio_enabled(
    FsRemoteStreamHandle handle,
    int enabled)
{
    if (!handle) return 0;
    return static_cast<StreamInstance*>(handle)->setViewerAudioEnabled(enabled != 0) ? 1 : 0; // wjy: 只切换本地Viewer播放器，远端视频、输入和认证会话保持连续。
}

int FSREMOTE_STREAM_CALL fsremote_stream_get_viewer_quality_status(
    FsRemoteStreamHandle handle,
    FsRemoteViewerQualityStatus* status)
{
    if (!handle || !status) return 0;
    return static_cast<StreamInstance*>(handle)->viewerQualityStatus(status) ? 1 : 0; // wjy: 返回固定POD快照，调用方无需解析data-channel文本。
}

int FSREMOTE_STREAM_CALL fsremote_stream_get_viewer_performance_stats(
    FsRemoteStreamHandle handle,
    FsRemoteViewerPerformanceStats* stats)
{
    if (!handle || !stats) return 0;
    return static_cast<StreamInstance*>(handle)->viewerPerformanceStats(stats) ? 1 : 0; // wjy: 可选只读统计接口不改变会话状态，旧调用方完全不受影响。
}
// ===end====

int FSREMOTE_STREAM_CALL fsremote_stream_is_busy(FsRemoteStreamHandle handle)
{
    if (!handle) {
        return 0;
    }
    return static_cast<StreamInstance*>(handle)->isBusy() ? 1 : 0;
}

// =====wjy====
uint32_t FSREMOTE_STREAM_CALL fsremote_stream_active_session_count(FsRemoteStreamHandle handle)
{
    if (!handle) {
        return 0;
    }
    return static_cast<StreamInstance*>(handle)->activeSessionCount(); // wjy: 状态服务用真实会话数填充远控人数字段。
}

uint32_t FSREMOTE_STREAM_CALL fsremote_stream_active_controller_names(
    FsRemoteStreamHandle handle,
    char* output,
    uint32_t output_capacity)
{
    if (!handle) {
        return 0;
    }
    const std::string names = static_cast<StreamInstance*>(handle)->activeControllerNamesCsv();
    const uint32_t required = static_cast<uint32_t>(names.size());
    if (!output || output_capacity < required) {
        return required;
    }
    if (required > 0) {
        std::memcpy(output, names.data(), required);
    }
    return required;
}

uint32_t FSREMOTE_STREAM_CALL fsremote_stream_active_controller_details(
    FsRemoteStreamHandle handle,
    char* output,
    uint32_t output_capacity)
{
    if (!handle) {
        return 0;
    }
    const std::string details = static_cast<StreamInstance*>(handle)->activeControllerDetailsCsv();
    const uint32_t required = static_cast<uint32_t>(details.size());
    if (!output || output_capacity < required) {
        return required;
    }
    if (required > 0) {
        std::memcpy(output, details.data(), required);
    }
    return required; // wjy: 调用方先传空缓冲查询长度，再一次性复制 UTF-8“设备名\tIP”详情列表。
}
// ===end====

const char* FSREMOTE_STREAM_CALL fsremote_stream_last_error(void)
{
    return g_last_error.c_str();
}
