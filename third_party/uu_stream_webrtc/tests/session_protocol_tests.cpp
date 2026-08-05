#include "session_protocol.h"
// =====wjy====
#include "windows_cursor_classifier.h"
// ===end====

#include <cstdlib>
#include <array>
#include <iostream>
#include <string>
#include <utility>

namespace {

// =====wjy====
void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "session_protocol_tests failed: " << message << '\n';
        std::exit(1); // wjy: 测试目标不依赖外部框架，任一协议断言失败立即返回非零退出码。
    }
}

uu::SessionMessage valid_client_hello()
{
    uu::SessionMessage message;
    message.type = uu::SessionMessageType::ClientHello;
    message.fields = {
        {"client_id", "client-1"},
        {"device_name", "测试设备 A"},
        {"requested_role", "control"},
        {"public_key", "ssh-ed25519 AAAA test=device"},
        {"client_nonce", "nonce-1"},
        {"capabilities", "video,audio,control"},
    };
    return message;
}

void expect_parse_failure(const std::string& wire, const char* reason)
{
    uu::SessionMessage parsed;
    std::string error;
    require(!uu::parse_session_message(wire, &parsed, &error), reason); // wjy: 错误文本可以扩展，但恶意/残缺输入必须稳定拒绝。
    require(!error.empty(), "parse failure must provide an error");
}
// ===end====

} // namespace

int main()
{
    // =====wjy====
    uu::SessionMessage source = valid_client_hello();
    source.fields.emplace("future_field", "unknown=字段\n仍需往返"); // wjy: 未知字段验证向前兼容，特殊字符验证百分号转义。
    std::string wire;
    std::string error;
    require(uu::serialize_session_message(source, &wire, &error), "valid hello must serialize");
    uu::SessionMessage parsed;
    require(uu::parse_session_message(wire, &parsed, &error), "serialized hello must parse");
    require(parsed.fields.at("future_field") == source.fields.at("future_field"), "unknown field must round trip");
    require(parsed.fields.at("device_name") == source.fields.at("device_name"), "utf8 field must round trip");
    expect_parse_failure("FSREMOTE_SESSION 1 client_hello\nclient_id=x\n", "missing required fields must fail");
    expect_parse_failure("FSREMOTE_SESSION 99 client_hello\n", "unsupported version must fail");
    expect_parse_failure("FSREMOTE_SESSION 1 client_hello\nbadline\n", "missing equals must fail");
    expect_parse_failure("FSREMOTE_SESSION 1 client_hello\nclient_id=a\nclient_id=b\n", "duplicate fields must fail");
    expect_parse_failure("FSREMOTE_SESSION 1 client_hello\nclient_id=%GG\n", "invalid escape must fail");
    expect_parse_failure(std::string(uu::kMaxSessionMessageBytes + 1, 'x'), "oversized message length must fail");

    uu::SessionMessage oversized = valid_client_hello();
    oversized.fields["device_name"] = std::string(uu::kMaxSessionFieldValueBytes + 1, 'x');
    require(!uu::serialize_session_message(oversized, &wire, &error), "oversized field length must fail");
    uu::SessionMessage rejected;
    rejected.type = uu::SessionMessageType::AdmissionRejected;
    rejected.fields = {{"reason", "capacity"}, {"detail", "maximum sessions reached"}};
    require(uu::serialize_session_message(rejected, &wire, &error), "rejection must serialize");
    require(uu::parse_session_message(wire, &parsed, &error), "rejection must parse");
    uu::SessionMessage viewOnlyAccepted;
    viewOnlyAccepted.type = uu::SessionMessageType::AdmissionAccepted;
    viewOnlyAccepted.fields = {
        {"session_id", "view-session-1"}, // wjy: 模拟第二个已认证会话的唯一 ID。
        {"ownership", "view_only"}, // wjy: 第二会话没有键鼠控制权，但仍必须能完成准入。
        {"capabilities", "video"}, // wjy: 只读阶段不声明 audio，Viewer 因而不会使用随准入返回的音频令牌。
        {"audio_token", "scoped-unused-token"}, // wjy: 协议必填凭据保持非空，避免 AdmissionAccepted 在发送前被校验器拒绝。
    };
    require(uu::serialize_session_message(viewOnlyAccepted, &wire, &error), "view-only admission must serialize with scoped token");
    require(uu::parse_session_message(wire, &parsed, &error), "view-only admission must parse");
    require(parsed.fields.at("ownership") == "view_only", "view-only ownership must round trip");
    viewOnlyAccepted.fields["audio_token"].clear();
    require(!uu::serialize_session_message(viewOnlyAccepted, &wire, &error), "empty view-only audio token must fail explicitly"); // wjy: 固化本次缺陷条件，未来不能再次让主机发送空必填字段。
    uu::SessionMessage audio;
    audio.type = uu::SessionMessageType::AudioSubscribe;
    audio.fields = {{"session_id", "session-1"}, {"audio_token", "one-time-token"}};
    require(uu::serialize_session_message(audio, &wire, &error), "audio token message must serialize");
    require(uu::parse_session_message(wire, &parsed, &error), "audio token message must parse");

    // =====wjy====
    const std::array cursorShapes = {
        uu::StandardCursorShape::Arrow,
        uu::StandardCursorShape::IBeam,
        uu::StandardCursorShape::Wait,
        uu::StandardCursorShape::Busy,
        uu::StandardCursorShape::Cross,
        uu::StandardCursorShape::Hand,
        uu::StandardCursorShape::Forbidden,
        uu::StandardCursorShape::Help,
        uu::StandardCursorShape::UpArrow,
        uu::StandardCursorShape::SizeAll,
        uu::StandardCursorShape::SizeHorizontal,
        uu::StandardCursorShape::SizeVertical,
        uu::StandardCursorShape::SizeNorthwestSoutheast,
        uu::StandardCursorShape::SizeNortheastSouthwest,
    };
    for (const uu::StandardCursorShape sourceShape : cursorShapes) {
        require(uu::serialize_cursor_shape_message(sourceShape, &wire, &error), "known cursor shape must serialize");
        require(uu::is_cursor_shape_message(wire), "serialized cursor must carry the versioned prefix");
        uu::StandardCursorShape parsedShape = uu::StandardCursorShape::Arrow;
        require(uu::parse_cursor_shape_message(wire, &parsedShape, &error), "serialized cursor must parse");
        require(parsedShape == sourceShape, "cursor shape must round trip without swapping resize diagonals"); // wjy: 全枚举往返特别保护两种对角线不会在 Host 与 Viewer 间互换。
    }
    uu::StandardCursorShape ignoredShape = uu::StandardCursorShape::Arrow;
    require(!uu::parse_cursor_shape_message("__fsremote_cursor_v2 size_hor", &ignoredShape, &error), "wrong cursor protocol version must fail");
    require(!uu::parse_cursor_shape_message("__fsremote_cursor_v1 custom", &ignoredShape, &error), "unknown cursor token must fail");
    require(!uu::parse_cursor_shape_message("__fsremote_cursor_v1 size_ver extra", &ignoredShape, &error), "extra cursor field must fail");
    const std::array<std::pair<LRESULT, uu::StandardCursorShape>, 8> resizeHitTests = {{
        {HTLEFT, uu::StandardCursorShape::SizeHorizontal},
        {HTRIGHT, uu::StandardCursorShape::SizeHorizontal},
        {HTTOP, uu::StandardCursorShape::SizeVertical},
        {HTBOTTOM, uu::StandardCursorShape::SizeVertical},
        {HTTOPLEFT, uu::StandardCursorShape::SizeNorthwestSoutheast},
        {HTBOTTOMRIGHT, uu::StandardCursorShape::SizeNorthwestSoutheast},
        {HTTOPRIGHT, uu::StandardCursorShape::SizeNortheastSouthwest},
        {HTBOTTOMLEFT, uu::StandardCursorShape::SizeNortheastSouthwest},
    }}; // wjy: 八个 Windows 标准可缩放边缘必须完整覆盖，并保持两条对角线方向不互换。
    for (const auto& [hitTest, expectedShape] : resizeHitTests) {
        const auto mappedShape = uu::standard_resize_cursor_shape_from_hit_test(hitTest);
        require(mappedShape.has_value(), "standard resize hit-test must map to a cursor shape");
        require(*mappedShape == expectedShape, "resize hit-test must preserve its Windows direction");
    }
    require(!uu::standard_resize_cursor_shape_from_hit_test(HTCLIENT).has_value(), "client area must not synthesize a resize cursor");
    require(!uu::standard_resize_cursor_shape_from_hit_test(HTCAPTION).has_value(), "caption must not synthesize a resize cursor");
    // ===end====
    std::cout << "session_protocol_tests passed\n";
    // ===end====
    return 0;
}
