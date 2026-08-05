#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace uu {

// =====wjy====
constexpr uint32_t kSessionProtocolVersion = 1; // wjy: 第一版多控制端准入协议版本，后续升级必须通过握手协商而不是静默猜测。
constexpr size_t kMaxSessionMessageBytes = 64 * 1024; // wjy: 限制单条准入/控制消息，避免未认证客户端提交超大文本占用内存。
constexpr size_t kMaxSessionFieldCount = 32; // wjy: 限制字段数量，同时允许未来版本附加可忽略字段。
constexpr size_t kMaxSessionFieldValueBytes = 16 * 1024; // wjy: 公钥和签名字段需要一定空间，但不能无限增长。

enum class SessionMessageType {
    ClientHello,
    ServerChallenge,
    ClientProof,
    AdmissionAccepted,
    AdmissionRejected,
    ControlRequest,
    ControlRelease,
    ControlTransferRequest,
    ControlTransferApprove,
    OwnershipStatus,
    AudioSubscribe,
};

enum class SessionRequestedRole {
    View,
    Control,
};

enum class SessionOwnershipState {
    ViewOnly,
    ControlGranted,
    RequestPending,
    Revoked,
};

enum class SessionRejectionReason {
    UnsupportedVersion,
    Unauthorized,
    Capacity,
    Policy,
    Timeout,
    Malformed,
};

// =====wjy====
enum class StandardCursorShape {
    Arrow,
    IBeam,
    Wait,
    Busy,
    Cross,
    Hand,
    Forbidden,
    Help,
    UpArrow,
    SizeAll,
    SizeHorizontal,
    SizeVertical,
    SizeNorthwestSoutheast,
    SizeNortheastSouthwest,
}; // wjy: 协议只传 Windows/Qt 都有稳定等价物的标准形状，自定义位图光标安全回退箭头。

constexpr std::string_view kCursorShapeMessagePrefix = "__fsremote_cursor_v1"; // wjy: 独立版本前缀让旧 Viewer 忽略新消息，也为未来像素光标协议保留升级空间。
// ===end====

struct SessionMessage {
    uint32_t version = kSessionProtocolVersion; // wjy: 每条消息自带已协商协议版本，便于解析端拒绝不兼容数据。
    SessionMessageType type = SessionMessageType::ClientHello; // wjy: 类型决定必填字段集合和后续状态机分支。
    std::unordered_map<std::string, std::string> fields; // wjy: 使用命名字段支持未来新增字段，旧版本可安全忽略未知键。
};

std::string_view session_message_type_name(SessionMessageType type);
std::string_view session_requested_role_name(SessionRequestedRole role);
std::string_view session_ownership_state_name(SessionOwnershipState state);
std::string_view session_rejection_reason_name(SessionRejectionReason reason);

bool serialize_session_message(const SessionMessage& message, std::string* output, std::string* error);
bool parse_session_message(std::string_view wire, SessionMessage* message, std::string* error);
bool validate_session_message(const SessionMessage& message, std::string* error);
// =====wjy====
std::string_view standard_cursor_shape_name(StandardCursorShape shape);
bool is_cursor_shape_message(std::string_view wire);
bool serialize_cursor_shape_message(StandardCursorShape shape, std::string* output, std::string* error);
bool parse_cursor_shape_message(std::string_view wire, StandardCursorShape* shape, std::string* error);
// ===end====
// ===end====

} // namespace uu
