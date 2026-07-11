#include "FsRemoteStreamApi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "host_media_pipeline.h"
// =====wjy====
#include "control_admission_policy.h"
#include "shared_input_state.h"
// ===end====
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
// =====wjy====
#include <functional>
// ===end====
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
    uint32_t requested_max_sessions = 3; // wjy: 用户明确要求全新设备无需注册表即可启用三路会话，因此 DLL 无配置入口默认请求 3。
    uint32_t effective_max_sessions = 3; // wjy: 无配置或旧入口直接采用三路有效上限，复制 Release 目录到其他设备即可生效。
    uint32_t max_aggregate_video_kbps = 120000; // wjy: 首阶段把总预算直接用于唯一 WebRTC 发送端。
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

    return g_mouse_input_mode.game_relative_mode;
}

void move_mouse_auto(int x, int y, bool log_result, uu::WebrtcSession* session, SessionPointerState* pointer)
{
    if (!pointer) return;
    if (!should_use_relative_mouse_for_move(x, y, log_result, session)) {
        pointer->has_last_viewer_pos = true;
        pointer->last_viewer_x = x;
        pointer->last_viewer_y = y;
        move_mouse_absolute(x, y, log_result);
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

void inject_input_message(
    const std::string& session_id,
    const std::string& message,
    uu::WebrtcSession* session,
    SessionPointerState* pointer,
    uu::SharedInputState* held_input)
{
    if (!pointer || !held_input) return;
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
            move_mouse_auto(x, y, log_message, session, pointer);
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
            pointer->has_last_viewer_pos = false;
        } else {
            move_mouse_absolute(x, y, log_message);
        }
        const bool down = kind == "d";
        const uu::SharedInputTransition transition = held_input->updateButton(session_id, button, down);
        const bool inject_down = transition == uu::SharedInputTransition::InjectDown;
        const bool inject_up = transition == uu::SharedInputTransition::InjectUp;
        if (button == 1 && (inject_down || inject_up)) send_mouse_button(inject_down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
        if (button == 2 && (inject_down || inject_up)) send_mouse_button(inject_down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
        if (button == 4 && (inject_down || inject_up)) send_mouse_button(inject_down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP); // wjy: 同一按钮由多人持有时只注入首次 down 和最终 up。
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
            const uu::SharedInputTransition transition = held_input->updateKey(session_id, vk, down != 0);
            if (vk == VK_ESCAPE && down != 0 && transition != uu::SharedInputTransition::None) {
                reset_mouse_relative_mode("escape", log_message, session);
            }
            if (transition == uu::SharedInputTransition::InjectDown
                || transition == uu::SharedInputTransition::InjectRepeat) {
                send_key(vk, true);
            } else if (transition == uu::SharedInputTransition::InjectUp) {
                send_key(vk, false); // wjy: 非持有者的抬键被忽略，最后一个持有者才真正释放系统按键。
            }
        }
    }
}

class InputDispatcher final {
public:
    using MouseModeCallback = std::function<void(bool relative, const char* reason)>;

    void registerSession(const std::string& session_id, MouseModeCallback callback)
    {
        std::lock_guard lock(mutex_);
        mouse_mode_callbacks_[session_id] = std::move(callback); // wjy: 每个已授权会话登记弱引用通知，鼠标模式变化时所有控制窗口保持一致。
    }

    void dispatch(const std::string& session_id, const std::string& message)
    {
        std::vector<MouseModeCallback> mode_callbacks;
        bool relative = false;
        {
            std::lock_guard lock(mutex_); // wjy: 所有 WebRTC data-channel 回调在这里汇合，SendInput 与全局鼠标模式按唯一顺序执行。
            const bool was_relative = g_mouse_input_mode.game_relative_mode;
            inject_input_message(session_id, message, nullptr, &pointer_by_session_[session_id], &held_input_);
            relative = g_mouse_input_mode.game_relative_mode;
            if (was_relative != relative) {
                for (const auto& [id, callback] : mouse_mode_callbacks_) {
                    if (callback) mode_callbacks.push_back(callback);
                }
            }
        }
        for (const auto& callback : mode_callbacks) callback(relative, "shared-dispatcher"); // wjy: 模式通知在互斥区外发送，避免 WebRTC 回调反向阻塞其他控制端输入。
    }

    void releaseSession(const std::string& session_id)
    {
        std::lock_guard lock(mutex_);
        const uu::SharedInputReleaseBatch released = held_input_.releaseSession(session_id);
        for (const int key : released.keys) send_key(key, false);
        for (const int button : released.buttons) send_button_up(button); // wjy: 断线只补发该会话最后持有的输入，其他控制端的按住状态继续有效。
        pointer_by_session_.erase(session_id);
        mouse_mode_callbacks_.erase(session_id);
        if (mouse_mode_callbacks_.empty()) reset_mouse_relative_mode("last-controller-left", false, nullptr);
    }

    void shutdown()
    {
        std::lock_guard lock(mutex_);
        for (const auto& [session_id, callback] : mouse_mode_callbacks_) {
            const uu::SharedInputReleaseBatch released = held_input_.releaseSession(session_id);
            for (const int key : released.keys) send_key(key, false);
            for (const int button : released.buttons) send_button_up(button);
        }
        pointer_by_session_.clear();
        mouse_mode_callbacks_.clear();
        reset_mouse_relative_mode("host-shutdown", false, nullptr); // wjy: Host 资源销毁前兜底清空所有会话输入与相对鼠标状态。
    }

private:
    static void send_button_up(int button)
    {
        if (button == 1) send_mouse_button(MOUSEEVENTF_LEFTUP);
        if (button == 2) send_mouse_button(MOUSEEVENTF_RIGHTUP);
        if (button == 4) send_mouse_button(MOUSEEVENTF_MIDDLEUP);
    }

    std::mutex mutex_;
    uu::SharedInputState held_input_;
    std::unordered_map<std::string, SessionPointerState> pointer_by_session_;
    std::unordered_map<std::string, MouseModeCallback> mouse_mode_callbacks_;
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
    virtual bool isBusy() const { return false; }
    // =====wjy====
    virtual uint32_t activeSessionCount() const { return 0; } // wjy: Viewer 无会话表；Host 覆盖为当前登记会话数。
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
        worker_ = std::thread([this, port] { runAcceptLoop(port); }); // wjy: manager 线程只负责共享运行时、持久监听和会话登记/回收。
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
        // wjy: 仅统计尚未完成的会话，避免已断开但未 reap 的条目把远控人数卡在旧值。
        uint32_t count = 0;
        for (const auto& [id, session] : sessions_) {
            if (session && !session->completed) {
                ++count;
            }
        }
        return count;
    }
    // ===end====

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
        if (!perform_host_admission(socket, config_, &exclusive_control_claimed_, &audio_primary_claimed_, &context->admission, &error)) {
            append_log("host admission rejected session=" + context->admission.session_id + " error=" + error);
            releaseAdmissionClaims(context); // wjy: 最终准入回包失败时归还兼容独占槽和单路音频槽，不影响其他协同控制端。
            context->cancel();
            context->completed = true;
            return; // wjy: 认证失败路径不会触碰音频、PeerConnection、桌面采集或编码器。
        }
        append_log("host admission accepted session=" + context->admission.session_id + " client=" + context->admission.client_id);
        // =====wjy====
        context->media_subscription = media_pipeline_.subscribe(&error); // wjy: 认证成功后才加入共享桌面源，未授权连接不会启动或占用采集资源。
        if (!context->media_subscription) {
            append_log("host media subscribe failed session=" + context->admission.session_id + " error=" + error);
            releaseAdmissionClaims(context); // wjy: 新会话媒体创建失败只回收自己的兼容槽和音频槽，不影响现有控制与视频订阅者。
            context->cancel();
            context->completed = true;
            return;
        }
        // ===end====
        const bool has_control = context->admission.ownership == "control_granted";
        const bool owns_audio = context->admission.audio_primary;
        if (owns_audio && has_capability(context->admission.capabilities, "audio")) {
            startAudioForSingleSession(); // wjy: 单路音频拥有者与协同控制权限完全解耦，第二、第三控制端不会因没有音频而降为只读。
        }

        uu::SessionConfig sessionConfig;
        sessionConfig.role = uu::SessionRole::Host;
        sessionConfig.target_bitrate_kbps = config_.max_aggregate_video_kbps;
        // =====wjy====
        sessionConfig.host_video_source = context->media_subscription->source(); // wjy: 每个 PeerConnection 使用同一帧源，但后续仍创建独立 track、sender 和编码器。
        sessionConfig.media_id = context->admission.session_id; // wjy: 生成会话唯一的 WebRTC track/stream 标识。
        // ===end====
        context->webrtc = std::make_unique<uu::WebrtcSession>(&runtime_, sessionConfig);
        context->webrtc->set_signal_callback([weak = std::weak_ptr<HostClientSession>(context)](const std::string& kind, const std::string& body) {
            if (const auto locked = weak.lock()) locked->send(kind + "\n" + body); // wjy: 回调只持弱引用，会话销毁后不会访问悬空 socket。
        });
        if (has_control) {
            input_dispatcher_.registerSession(context->admission.session_id, [weak = std::weak_ptr<HostClientSession>(context)](bool relative, const char* reason) {
                if (const auto locked = weak.lock(); locked && !locked->cancelled) {
                    std::lock_guard webrtc_lock(locked->webrtc_mutex);
                    publish_mouse_mode(locked->webrtc.get(), relative, reason); // wjy: 任一控制端触发鼠标模式切换后安全广播给全部仍存活的协同控制窗口。
                }
            });
            context->webrtc->set_control_callback([this, weak = std::weak_ptr<HostClientSession>(context)](const std::string& message) {
                if (const auto locked = weak.lock(); locked && !locked->cancelled) {
                    input_dispatcher_.dispatch(locked->admission.session_id, message); // wjy: 每个授权会话都可提交输入，但实际 SendInput 统一经过主机级串行调度器。
                }
            });
        } // wjy: view_only 会话不登记回调，即使发送 data-channel 输入也无法到达系统注入路径。
        if (context->webrtc->initialize(&error) && context->webrtc->start_offer(&error)) {
            std::string message;
            while (running_ && !context->cancelled && uu::recv_message(socket, &message)) {
                handle_message(*context->webrtc, message);
            }
        } else {
            append_log("host session init/start failed session=" + context->admission.session_id + " error=" + error);
        }
        std::unique_ptr<uu::WebrtcSession> retiring_webrtc;
        {
            std::lock_guard webrtc_lock(context->webrtc_mutex);
            retiring_webrtc = std::move(context->webrtc); // wjy: 先在锁内摘除可广播指针，让后续模式通知立即跳过该会话。
        }
        retiring_webrtc.reset(); // wjy: 在锁外执行 PeerConnection 析构，避免析构等待 data-channel 回调时与模式通知锁形成互等。
        if (has_control) input_dispatcher_.releaseSession(context->admission.session_id); // wjy: 销毁输入回调后释放该会话独有的键、按钮和相对坐标状态。
        context->media_subscription.reset(); // wjy: 先销毁 PeerConnection/track，再减少共享 source 订阅计数，避免捕获线程提前停止。
        context->cancel();
        if (owns_audio) stopAudioForSingleSession(); // wjy: 非音频主会话退出不能停止首个会话仍在使用的音频连接。
        releaseAdmissionClaims(context);
        context->completed = true;
        append_log("host client worker completed session=" + context->admission.session_id);
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
    uu::HostMediaPipeline media_pipeline_; // wjy: manager 独占 VDD/桌面捕获启停权，会话只能通过订阅令牌引用共享 source。
    InputDispatcher input_dispatcher_; // wjy: HostInstance 唯一持有输入调度器，三个会话共享同一确定性 SendInput 顺序。
    mutable std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<HostClientSession>> sessions_;
    std::mutex audio_mutex_;
    std::unique_ptr<uu::HostAudioStreamer> audio_streamer_;
    std::atomic_bool exclusive_control_claimed_ = false; // wjy: 仅供显式旧独占策略兼容使用，默认协同策略不会占用。
    std::atomic_bool audio_primary_claimed_ = false; // wjy: HostAudioHub 完成前限制单路音频客户端，但不限制协同键鼠控制。
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
        if (!message || !*message || !control_allowed_) { // wjy: view_only 客户端在进入 WebRTC 发送路径前直接拒绝本地键鼠消息。
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
        control_allowed_ = admission_.ownership == "control_granted"; // wjy: Viewer 本地同步执行准入权限，view_only 窗口不再向 data channel 转发键鼠消息。
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
    std::atomic_bool control_allowed_ = false; // wjy: 原子权限位跨 UI 与 viewer worker 线程读取，避免直接并发访问 admission_ 字符串。
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

// =====wjy====
uint32_t FSREMOTE_STREAM_CALL fsremote_stream_active_session_count(FsRemoteStreamHandle handle)
{
    if (!handle) {
        return 0;
    }
    return static_cast<StreamInstance*>(handle)->activeSessionCount(); // wjy: 状态服务用真实会话数填充远控人数字段。
}
// ===end====

const char* FSREMOTE_STREAM_CALL fsremote_stream_last_error(void)
{
    return g_last_error.c_str();
}
