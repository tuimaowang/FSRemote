#include "session_protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <initializer_list>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

namespace uu {
namespace {

// =====wjy====
constexpr std::string_view kProtocolHeader = "FSREMOTE_SESSION"; // wjy: 固定头用于在进入 WebRTC 信令前识别准入协议消息。

struct MessageTypeName {
    SessionMessageType type;
    std::string_view name;
};

// =====wjy====
struct CursorShapeName {
    StandardCursorShape shape;
    std::string_view name;
};

constexpr std::array<CursorShapeName, 14> kCursorShapeNames = {{
    {StandardCursorShape::Arrow, "arrow"},
    {StandardCursorShape::IBeam, "ibeam"},
    {StandardCursorShape::Wait, "wait"},
    {StandardCursorShape::Busy, "busy"},
    {StandardCursorShape::Cross, "cross"},
    {StandardCursorShape::Hand, "hand"},
    {StandardCursorShape::Forbidden, "forbidden"},
    {StandardCursorShape::Help, "help"},
    {StandardCursorShape::UpArrow, "up_arrow"},
    {StandardCursorShape::SizeAll, "size_all"},
    {StandardCursorShape::SizeHorizontal, "size_hor"},
    {StandardCursorShape::SizeVertical, "size_ver"},
    {StandardCursorShape::SizeNorthwestSoutheast, "size_nwse"},
    {StandardCursorShape::SizeNortheastSouthwest, "size_nesw"},
}}; // wjy: 固定短 token 降低高频状态消息体积，并避免把平台数值枚举直接暴露到协议。
// ===end====

constexpr std::array<MessageTypeName, 11> kMessageTypeNames = {{
    {SessionMessageType::ClientHello, "client_hello"},
    {SessionMessageType::ServerChallenge, "server_challenge"},
    {SessionMessageType::ClientProof, "client_proof"},
    {SessionMessageType::AdmissionAccepted, "admission_accepted"},
    {SessionMessageType::AdmissionRejected, "admission_rejected"},
    {SessionMessageType::ControlRequest, "control_request"},
    {SessionMessageType::ControlRelease, "control_release"},
    {SessionMessageType::ControlTransferRequest, "control_transfer_request"},
    {SessionMessageType::ControlTransferApprove, "control_transfer_approve"},
    {SessionMessageType::OwnershipStatus, "ownership_status"},
    {SessionMessageType::AudioSubscribe, "audio_subscribe"},
}};

bool fail(std::string* error, std::string text)
{
    if (error) {
        *error = std::move(text); // wjy: 所有解析失败都返回稳定文本，便于测试和后续拒绝原因映射。
    }
    return false;
}

bool is_unreserved(unsigned char ch)
{
    return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~'; // wjy: 仅保留不会破坏逐行 key=value 格式的安全字符。
}

char hex_digit(unsigned int value)
{
    return static_cast<char>(value < 10 ? ('0' + value) : ('A' + value - 10));
}

int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string percent_encode(std::string_view value)
{
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char ch : value) {
        if (is_unreserved(ch)) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        encoded.push_back('%'); // wjy: 换行、等号、空格和非 ASCII 字节统一转义，确保消息仍可逐行解析。
        encoded.push_back(hex_digit(ch >> 4));
        encoded.push_back(hex_digit(ch & 0x0F));
    }
    return encoded;
}

bool percent_decode(std::string_view value, std::string* decoded, std::string* error)
{
    if (!decoded) return fail(error, "decoded output is null");
    decoded->clear();
    decoded->reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (ch != '%') {
            decoded->push_back(ch);
            continue;
        }
        if (index + 2 >= value.size()) return fail(error, "truncated percent escape");
        const int high = hex_value(value[index + 1]);
        const int low = hex_value(value[index + 2]);
        if (high < 0 || low < 0) return fail(error, "invalid percent escape");
        const char decoded_ch = static_cast<char>((high << 4) | low);
        if (decoded_ch == '\0') return fail(error, "nul byte is not allowed");
        decoded->push_back(decoded_ch); // wjy: 解码后的原始 UTF-8/公钥文本交给上层字段校验，不在协议层改变内容。
        index += 2;
    }
    return true;
}

bool valid_field_name(std::string_view name)
{
    if (name.empty() || name.size() > 48) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-';
    });
}

bool parse_message_type(std::string_view text, SessionMessageType* type)
{
    for (const auto& entry : kMessageTypeNames) {
        if (entry.name == text) {
            if (type) *type = entry.type;
            return true;
        }
    }
    return false;
}

bool require_field(const SessionMessage& message, std::string_view name, std::string* error)
{
    const auto iterator = message.fields.find(std::string(name));
    if (iterator == message.fields.end() || iterator->second.empty()) {
        return fail(error, "missing required field: " + std::string(name)); // wjy: 必填字段缺失在创建昂贵的 WebRTC/音频资源前立即拒绝。
    }
    return true;
}

bool require_fields(const SessionMessage& message, std::initializer_list<std::string_view> names, std::string* error)
{
    for (const std::string_view name : names) {
        if (!require_field(message, name, error)) return false;
    }
    return true;
}

std::string_view trim_cr(std::string_view line)
{
    return !line.empty() && line.back() == '\r' ? line.substr(0, line.size() - 1) : line;
}
// ===end====

} // namespace

// =====wjy====
std::string_view session_message_type_name(SessionMessageType type)
{
    for (const auto& entry : kMessageTypeNames) {
        if (entry.type == type) return entry.name;
    }
    return {};
}

std::string_view session_requested_role_name(SessionRequestedRole role)
{
    return role == SessionRequestedRole::Control ? "control" : "view";
}

std::string_view session_ownership_state_name(SessionOwnershipState state)
{
    switch (state) {
    case SessionOwnershipState::ViewOnly: return "view_only";
    case SessionOwnershipState::ControlGranted: return "control_granted";
    case SessionOwnershipState::RequestPending: return "request_pending";
    case SessionOwnershipState::Revoked: return "revoked";
    }
    return {};
}

std::string_view session_rejection_reason_name(SessionRejectionReason reason)
{
    switch (reason) {
    case SessionRejectionReason::UnsupportedVersion: return "unsupported_version";
    case SessionRejectionReason::Unauthorized: return "unauthorized";
    case SessionRejectionReason::Capacity: return "capacity";
    case SessionRejectionReason::Policy: return "policy";
    case SessionRejectionReason::Timeout: return "timeout";
    case SessionRejectionReason::Malformed: return "malformed";
    }
    return {};
}

// =====wjy====
std::string_view standard_cursor_shape_name(StandardCursorShape shape)
{
    for (const CursorShapeName& entry : kCursorShapeNames) {
        if (entry.shape == shape) return entry.name;
    }
    return {}; // wjy: 非法枚举不序列化成猜测值，调用方可明确回退 Arrow。
}

bool is_cursor_shape_message(std::string_view wire)
{
    return wire.size() > kCursorShapeMessagePrefix.size()
        && wire.compare(0, kCursorShapeMessagePrefix.size(), kCursorShapeMessagePrefix) == 0
        && wire[kCursorShapeMessagePrefix.size()] == ' '; // wjy: 只有完整版本前缀加空格才接管解析，类似名称的普通控制消息不被误吞。
}

bool serialize_cursor_shape_message(StandardCursorShape shape, std::string* output, std::string* error)
{
    if (!output) return fail(error, "cursor output is null");
    const std::string_view name = standard_cursor_shape_name(shape);
    if (name.empty()) return fail(error, "unknown cursor shape");
    *output = std::string(kCursorShapeMessagePrefix) + " " + std::string(name); // wjy: 单行两字段消息保持在可靠有序 control channel 的最小负担内。
    return true;
}

bool parse_cursor_shape_message(std::string_view wire, StandardCursorShape* shape, std::string* error)
{
    if (!shape) return fail(error, "cursor shape output is null");
    if (wire.empty() || wire.size() > 64) return fail(error, "invalid cursor message length"); // wjy: 光标消息长度固定很小，先限长避免异常 data-channel 文本进入流解析。

    std::istringstream stream{std::string(wire)};
    std::string prefix;
    std::string token;
    std::string extra;
    if (!(stream >> prefix >> token) || (stream >> extra) || prefix != kCursorShapeMessagePrefix) {
        return fail(error, "invalid cursor message format"); // wjy: 缺字段、多字段和错误版本前缀全部拒绝，不覆盖 Viewer 当前光标。
    }
    for (const CursorShapeName& entry : kCursorShapeNames) {
        if (entry.name == token) {
            *shape = entry.shape;
            return true;
        }
    }
    return fail(error, "unknown cursor shape token");
}
// ===end====

bool validate_session_message(const SessionMessage& message, std::string* error)
{
    if (message.version != kSessionProtocolVersion) return fail(error, "unsupported protocol version");
    if (session_message_type_name(message.type).empty()) return fail(error, "unknown message type");
    if (message.fields.size() > kMaxSessionFieldCount) return fail(error, "too many fields");
    for (const auto& [name, value] : message.fields) {
        if (!valid_field_name(name)) return fail(error, "invalid field name");
        if (value.size() > kMaxSessionFieldValueBytes) return fail(error, "field value is too large");
    }

    switch (message.type) {
    case SessionMessageType::ClientHello:
        if (!require_fields(message, {"client_id", "device_name", "requested_role", "public_key", "client_nonce", "capabilities"}, error)) return false;
        if (message.fields.at("requested_role") != "view" && message.fields.at("requested_role") != "control") return fail(error, "invalid requested role");
        return true;
    case SessionMessageType::ServerChallenge:
        return require_fields(message, {"host_id", "host_nonce", "selected_version", "challenge_context"}, error);
    case SessionMessageType::ClientProof:
        return require_fields(message, {"client_id", "signature"}, error);
    case SessionMessageType::AdmissionAccepted:
        return require_fields(message, {"session_id", "ownership", "capabilities", "audio_token"}, error);
    case SessionMessageType::AdmissionRejected:
        return require_fields(message, {"reason", "detail"}, error);
    case SessionMessageType::ControlRequest:
    case SessionMessageType::ControlRelease:
        return require_fields(message, {"session_id"}, error);
    case SessionMessageType::ControlTransferRequest:
    case SessionMessageType::ControlTransferApprove:
        return require_fields(message, {"session_id", "target_session_id"}, error);
    case SessionMessageType::OwnershipStatus:
        return require_fields(message, {"session_id", "state"}, error);
    case SessionMessageType::AudioSubscribe:
        return require_fields(message, {"session_id", "audio_token"}, error);
    }
    return fail(error, "unknown message type");
}

bool serialize_session_message(const SessionMessage& message, std::string* output, std::string* error)
{
    if (!output) return fail(error, "message output is null");
    if (!validate_session_message(message, error)) return false;
    std::ostringstream stream;
    stream << kProtocolHeader << ' ' << message.version << ' ' << session_message_type_name(message.type) << '\n';
    const std::map<std::string, std::string> ordered_fields(message.fields.begin(), message.fields.end()); // wjy: 固定字段顺序，保证签名上下文和测试输出可重复。
    for (const auto& [name, value] : ordered_fields) stream << name << '=' << percent_encode(value) << '\n';
    std::string wire = stream.str();
    if (wire.size() > kMaxSessionMessageBytes) return fail(error, "serialized message is too large");
    *output = std::move(wire); // wjy: 仅在全部校验成功后覆盖调用方输出，避免留下半条协议消息。
    return true;
}

bool parse_session_message(std::string_view wire, SessionMessage* message, std::string* error)
{
    if (!message) return fail(error, "message output is null");
    if (wire.empty()) return fail(error, "message is empty");
    if (wire.size() > kMaxSessionMessageBytes) return fail(error, "message is too large");
    const size_t header_end = wire.find('\n');
    if (header_end == std::string_view::npos) return fail(error, "message header is incomplete");
    const std::string header(trim_cr(wire.substr(0, header_end)));
    std::istringstream header_stream(header);
    std::string marker;
    std::string version_text;
    std::string type_text;
    std::string extra;
    if (!(header_stream >> marker >> version_text >> type_text) || (header_stream >> extra) || marker != kProtocolHeader) return fail(error, "invalid message header");

    uint32_t version = 0;
    const auto version_result = std::from_chars(version_text.data(), version_text.data() + version_text.size(), version);
    if (version_result.ec != std::errc() || version_result.ptr != version_text.data() + version_text.size()) return fail(error, "invalid protocol version");
    if (version != kSessionProtocolVersion) return fail(error, "unsupported protocol version");

    SessionMessage parsed;
    parsed.version = version;
    if (!parse_message_type(type_text, &parsed.type)) return fail(error, "unknown message type");
    size_t cursor = header_end + 1;
    while (cursor < wire.size()) {
        size_t line_end = wire.find('\n', cursor);
        if (line_end == std::string_view::npos) line_end = wire.size();
        const std::string_view line = trim_cr(wire.substr(cursor, line_end - cursor));
        cursor = line_end == wire.size() ? wire.size() : line_end + 1;
        if (line.empty()) continue;
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) return fail(error, "field is missing equals separator");
        const std::string name(line.substr(0, equals));
        if (!valid_field_name(name)) return fail(error, "invalid field name");
        if (parsed.fields.contains(name)) return fail(error, "duplicate field: " + name);
        std::string value;
        if (!percent_decode(line.substr(equals + 1), &value, error)) return false;
        if (value.size() > kMaxSessionFieldValueBytes) return fail(error, "field value is too large");
        parsed.fields.emplace(name, std::move(value)); // wjy: 未知但格式正确的字段仍保留，满足向前兼容扩展需求。
        if (parsed.fields.size() > kMaxSessionFieldCount) return fail(error, "too many fields");
    }
    if (!validate_session_message(parsed, error)) return false;
    *message = std::move(parsed); // wjy: 完整解析和必填校验通过后一次性交付结果。
    return true;
}
// ===end====

} // namespace uu
