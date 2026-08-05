#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace uu {

// =====wjy====
constexpr uint32_t kViewerQualityProtocolVersion = 1;
constexpr uint32_t kViewerQualityProtocolVersion2 = 2;
constexpr uint32_t kViewerQualityMaximumFpsV2 = 360;
constexpr size_t kMaxViewerQualityMessageBytes = 512; // wjy: 固定数字字段消息设置硬上限，恶意data-channel文本不能制造大分配。

enum class ViewerQualityMode : uint32_t {
    Automatic = 1,
    HighQualityLocked = 2,
    Balanced = 3,
    Smooth = 4,
};

enum class ViewerQualityLimitation : uint32_t {
    None = 0,
    Unsupported = 1,
    InvalidRequest = 2,
    ApplyFailed = 3,
    Clamped = 4,
};

enum class ViewerQualityLimitReason : uint32_t {
    None = 0,
    SourceRefresh = 1,
    ViewerRefresh = 2,
    CaptureCapability = 3,
    EncoderCapability = 4,
    Bandwidth = 5,
    AggregateBudget = 6,
    LegacyProtocol = 7,
};

struct ViewerQualityRequest {
    uint64_t request_id = 0;
    ViewerQualityMode mode = ViewerQualityMode::Automatic;
    uint32_t target_width = 0; // wjy: 宽高同时为0表示原始分辨率，否则必须处于安全显示范围。
    uint32_t target_height = 0;
    uint32_t target_fps = 60;
    uint32_t max_bitrate_kbps = 80000;
    uint32_t priority = 50; // wjy: 0到100，锁定高质量窗口使用更高优先级但仍受硬资源边界限制。
};

struct ViewerQualityAcknowledgement {
    uint64_t request_id = 0;
    bool supported = true;
    ViewerQualityMode applied_mode = ViewerQualityMode::Automatic;
    uint32_t applied_width = 0;
    uint32_t applied_height = 0;
    uint32_t applied_fps = 60;
    uint32_t applied_bitrate_kbps = 80000;
    ViewerQualityLimitation limitation = ViewerQualityLimitation::None;
};

struct ViewerQualityCapabilitiesV2 {
    uint64_t request_id = 0;
    uint32_t source_refresh_hz = 60;
    uint32_t max_capture_fps = 60;
    uint32_t max_encode_fps = 60;
    bool online_fps_update = false;
};

struct ViewerQualityRequestV2 : ViewerQualityRequest {
    uint32_t viewer_refresh_hz = 60;
};

struct ViewerQualityAcknowledgementV2 : ViewerQualityAcknowledgement {
    uint32_t source_refresh_hz = 60;
    uint32_t max_capture_fps = 60;
    uint32_t max_encode_fps = 60;
    bool online_fps_update = false;
    ViewerQualityLimitReason limit_reason = ViewerQualityLimitReason::None;
};

bool is_viewer_quality_message(std::string_view wire); // wjy: Host先识别专用前缀，画质消息绝不进入键鼠注入解析器。
bool serialize_viewer_quality_request(const ViewerQualityRequest& request, std::string* output, std::string* error);
bool parse_viewer_quality_request(std::string_view wire, ViewerQualityRequest* request, std::string* error);
bool serialize_viewer_quality_acknowledgement(const ViewerQualityAcknowledgement& acknowledgement, std::string* output, std::string* error);
bool parse_viewer_quality_acknowledgement(std::string_view wire, ViewerQualityAcknowledgement* acknowledgement, std::string* error);
bool serialize_viewer_quality_capabilities_request_v2(uint64_t requestId, std::string* output, std::string* error);
bool parse_viewer_quality_capabilities_request_v2(std::string_view wire, uint64_t* requestId, std::string* error);
bool serialize_viewer_quality_capabilities_v2(const ViewerQualityCapabilitiesV2& capabilities, std::string* output, std::string* error);
bool parse_viewer_quality_capabilities_v2(std::string_view wire, ViewerQualityCapabilitiesV2* capabilities, std::string* error);
bool serialize_viewer_quality_request_v2(const ViewerQualityRequestV2& request, std::string* output, std::string* error);
bool parse_viewer_quality_request_v2(std::string_view wire, ViewerQualityRequestV2* request, std::string* error);
bool serialize_viewer_quality_acknowledgement_v2(const ViewerQualityAcknowledgementV2& acknowledgement, std::string* output, std::string* error);
bool parse_viewer_quality_acknowledgement_v2(std::string_view wire, ViewerQualityAcknowledgementV2* acknowledgement, std::string* error);
// ===end====

} // namespace uu
