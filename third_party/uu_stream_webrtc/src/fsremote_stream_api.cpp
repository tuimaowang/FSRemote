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
#include "session_protocol.h"
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
    uint32_t requested_max_sessions = 1; // wjy: 保存用户配置用于日志和后续阶段，当前有效上限仍固定为 1。
    uint32_t effective_max_sessions = 1; // wjy: 单会话安全重构验证完成前禁止提前开放多连接。
    uint32_t max_aggregate_video_kbps = 120000; // wjy: 首阶段把总预算直接用于唯一 WebRTC 发送端。
    uint32_t handshake_timeout_ms = 5000; // wjy: 后续认证状态机直接复用该超时配置。
    uint32_t ownership_policy = FSREMOTE_OWNERSHIP_EXCLUSIVE; // wjy: 只接受独占控制权策略。
};

HostRuntimeConfig normalized_host_config(const FsRemoteHostConfig* config)
{
    HostRuntimeConfig normalized;
    if (!config || config->struct_size < sizeof(FsRemoteHostConfig) || config->version != 1) {
        return normalized; // wjy: 空配置、短结构或未知版本全部安全回退到原有单会话默认值。
    }
    normalized.requested_max_sessions = std::clamp(config->max_sessions, 1u, 3u);
    normalized.effective_max_sessions = 1; // wjy: OpenSpec 迁移计划要求先在 maxSessions=1 下完成回归再开放并发。
    normalized.max_aggregate_video_kbps = std::clamp(config->max_aggregate_video_kbps, 9000u, 240000u);
    normalized.handshake_timeout_ms = std::clamp(config->handshake_timeout_ms, 1000u, 30000u);
    normalized.ownership_policy = FSREMOTE_OWNERSHIP_EXCLUSIVE;
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

// =====wjy====
struct SessionAdmission {
    std::string session_id; // wjy: 每次 TCP 准入生成独立会话 ID，后续控制权和音频凭证都绑定它。
    std::string audio_token; // wjy: 单次短期音频令牌先随准入结果下发，音频中心阶段再强制消费。
    std::string client_id;
    std::string public_key;
    std::string host_id; // wjy: 保存签名上下文中的受控设备身份，供 ViewerInstance 后续状态和诊断使用。
    std::string requested_role; // wjy: 保存本次经过验证的请求角色，避免 WebRTC 阶段重新猜测客户端意图。
    std::string capabilities; // wjy: 仅保存主机与客户端都支持的能力交集。
    uint32_t protocol_version = 0; // wjy: 保存实际选中的协议版本，后续消息必须沿用同一版本。
    std::string ownership = "view_only"; // wjy: 默认无输入权限，只有明确请求 control 且认证通过才授予控制权。
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
    admission->audio_token = issue_audio_token(admission->session_id);
    admission->client_id = hello.fields.at("client_id");
    admission->public_key = public_key;
    admission->host_id = host_id;
    admission->requested_role = requested_role;
    admission->capabilities = negotiated_capabilities;
    admission->protocol_version = hello.version;
    admission->ownership = requested_role == "control" ? "control_granted" : "view_only"; // wjy: 视图请求永不隐式升级为控制请求。
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
    if (!recv_session_message(socket, &challenge, error)) return false;
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
    if (!recv_session_message(socket, &accepted, error)) return false;
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
    std::atomic_bool cancelled = false;
    std::atomic_bool completed = false;
    SessionAdmission admission;
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
        worker_ = std::thread([this, port] { runAcceptLoop(port); }); // wjy: manager 线程只负责共享运行时、持久监听和会话登记/回收。
    }

    ~HostInstance() override
    {
        shutdown(); // wjy: 派生成员析构前先完成 listener、客户端线程、音频和 WebRTC 的确定性停止顺序。
    }

    bool isBusy() const override
    {
        std::lock_guard lock(sessions_mutex_);
        return !sessions_.empty();
    }

private:
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
        while (running_) {
            append_log("host waiting accept");
            const SOCKET accepted = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
            if (accepted == INVALID_SOCKET) {
                if (running_) append_log("host accept failed error=" + std::to_string(::WSAGetLastError()));
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
            {
                std::lock_guard lock(sessions_mutex_);
                sessions_.emplace(session->admission.session_id, session); // wjy: map 只保存共享上下文，worker 和 manager 都能安全完成清理。
            }
            session->worker = std::thread([this, session] { runClientSession(session); });
            append_log("host client worker started session=" + session->admission.session_id);
        }
        const uintptr_t published = server_socket_.exchange(0);
        if (published) uu::close_socket(published);
        shutdownSessionsOnManagerThread(); // wjy: accept 退出后由同一 manager 线程 join 客户端并销毁共享 runtime。
    }

    void runClientSession(const std::shared_ptr<HostClientSession>& context)
    {
        const uintptr_t socket = context->socket.load();
        std::string error;
        if (!perform_host_admission(socket, config_, &context->admission, &error)) {
            append_log("host admission rejected session=" + context->admission.session_id + " error=" + error);
            context->cancel();
            context->completed = true;
            return; // wjy: 认证失败路径不会触碰音频、PeerConnection、桌面采集或编码器。
        }
        append_log("host admission accepted session=" + context->admission.session_id + " client=" + context->admission.client_id);
        startAudioForSingleSession();

        uu::SessionConfig sessionConfig;
        sessionConfig.role = uu::SessionRole::Host;
        sessionConfig.target_bitrate_kbps = config_.max_aggregate_video_kbps;
        context->webrtc = std::make_unique<uu::WebrtcSession>(&runtime_, sessionConfig);
        context->webrtc->set_signal_callback([weak = std::weak_ptr<HostClientSession>(context)](const std::string& kind, const std::string& body) {
            if (const auto locked = weak.lock()) locked->send(kind + "\n" + body); // wjy: 回调只持弱引用，会话销毁后不会访问悬空 socket。
        });
        uu::WebrtcSession* const webrtc = context->webrtc.get();
        context->webrtc->set_control_callback([weak = std::weak_ptr<HostClientSession>(context), webrtc](const std::string& message) {
            if (const auto locked = weak.lock(); locked && !locked->cancelled) inject_input_message(message, webrtc);
        });
        if (context->webrtc->initialize(&error) && context->webrtc->start_offer(&error)) {
            std::string message;
            while (running_ && !context->cancelled && uu::recv_message(socket, &message)) {
                handle_message(*context->webrtc, message);
            }
        } else {
            append_log("host session init/start failed session=" + context->admission.session_id + " error=" + error);
        }
        context->webrtc.reset(); // wjy: session worker 退出前销毁独占媒体对象，manager 最后才会销毁共享 runtime。
        context->cancel();
        stopAudioForSingleSession();
        context->completed = true;
        append_log("host client worker completed session=" + context->admission.session_id);
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
        audio_streamer_->start(49105, &ignored); // wjy: maxSessions=1 迁移阶段仍沿用单客户端音频，后续由 HostAudioHub 替换。
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
        stopAudioForSingleSession();
        append_log("host manager session shutdown audio stopped");
        append_log("host manager resources shutdown");
    }

    void shutdown()
    {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) return;
        const uintptr_t listener = server_socket_.exchange(0);
        if (listener) uu::close_socket(listener); // wjy: 第一步关闭 listener，阻止关闭期间登记新会话并解除 accept。
        append_log("host shutdown listener closed");
        if (worker_.joinable()) worker_.join(); // wjy: 第二步先等待 accept manager 退出，杜绝它与关闭线程同时回收并 join 同一个 worker。
        append_log("host shutdown accept worker joined");
        runtime_.shutdown(); // wjy: 全部会话对象销毁后，由 HostInstance 创建线程配对关闭共享 WebRTC/SSL runtime。
        append_log("host shutdown completed");
    }

    HostRuntimeConfig config_;
    uu::NativeWebrtcRuntime runtime_; // wjy: 全部 HostClientSession 共用一个进程内 WebRTC runtime。
    mutable std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<HostClientSession>> sessions_;
    std::mutex audio_mutex_;
    std::unique_ptr<uu::HostAudioStreamer> audio_streamer_;
};
// ===end====

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
        set_socket_timeout(socket, 0);
        append_viewer_log("viewer admitted session=" + admission_.session_id
            + " version=" + std::to_string(admission_.protocol_version)
            + " capabilities=" + admission_.capabilities
            + " ownership=" + admission_.ownership); // wjy: ViewerInstance 持久保存并记录服务端确认的身份、能力、版本与权限结果。
        // ===end====
        if (has_capability(admission_.capabilities, "audio")) {
            std::string audio_error;
            audio_player_ = std::make_unique<uu::ViewerAudioPlayer>();
            audio_player_->start(host_ip_, 49105, &audio_error); // wjy: 只有准入结果包含 audio 能力时才创建音频订阅者。
            append_viewer_log("viewer audio start error=" + audio_error); // wjy: record audio startup because it runs parallel to video.
        }
        report_status(status_callback_, user_, FSREMOTE_STATUS_INITIALIZING_WEBRTC, "Initializing WebRTC");
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
    SessionAdmission admission_; // wjy: 准入结果生命周期覆盖整个 ViewerInstance，供后续音频令牌、控制权和状态处理复用。
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
        return new HostInstance(port, normalized_host_config(nullptr)); // wjy: 旧入口继续提供稳定的单会话默认行为。
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
