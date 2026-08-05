#include "viewer_quality_protocol.h"

#include <charconv>
#include <sstream>
#include <string>
#include <system_error>

namespace uu {
namespace {

// =====wjy====
constexpr std::string_view kQualityPrefix = "__fsremote_quality_v1";
constexpr std::string_view kQualityPrefixV2 = "__fsremote_quality_v2";

bool fail(std::string* error, std::string text)
{
    if (error) *error = std::move(text);
    return false;
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer* output)
{
    if (!output || text.empty()) return false;
    Integer value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
    *output = value;
    return true;
}

bool valid_mode(ViewerQualityMode mode)
{
    const uint32_t value = static_cast<uint32_t>(mode);
    return value >= static_cast<uint32_t>(ViewerQualityMode::Automatic)
        && value <= static_cast<uint32_t>(ViewerQualityMode::Smooth);
}

bool valid_limitation(ViewerQualityLimitation limitation)
{
    return static_cast<uint32_t>(limitation) <= static_cast<uint32_t>(ViewerQualityLimitation::Clamped);
}

bool validate_dimensions(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return width == 0 && height == 0; // wjy: 原始分辨率只能用成对0表示，禁止半个维度缺失。
    return width >= 320 && width <= 7680 && height >= 180 && height <= 4320;
}

bool validate_request(const ViewerQualityRequest& request, std::string* error)
{
    if (request.request_id == 0) return fail(error, "request id must be non-zero");
    if (!valid_mode(request.mode)) return fail(error, "invalid quality mode");
    if (!validate_dimensions(request.target_width, request.target_height)) return fail(error, "invalid target dimensions");
    if (request.target_fps < 1 || request.target_fps > 60) return fail(error, "invalid target fps");
    if (request.max_bitrate_kbps < 256 || request.max_bitrate_kbps > 240000) return fail(error, "invalid bitrate");
    if (request.priority > 100) return fail(error, "invalid priority");
    return true;
}

bool validate_acknowledgement(const ViewerQualityAcknowledgement& acknowledgement, std::string* error)
{
    if (acknowledgement.request_id == 0) return fail(error, "ack request id must be non-zero");
    if (!valid_mode(acknowledgement.applied_mode)) return fail(error, "invalid applied mode");
    if (!validate_dimensions(acknowledgement.applied_width, acknowledgement.applied_height)) return fail(error, "invalid applied dimensions");
    if (acknowledgement.applied_fps < 1 || acknowledgement.applied_fps > 60) return fail(error, "invalid applied fps");
    if (acknowledgement.applied_bitrate_kbps < 256 || acknowledgement.applied_bitrate_kbps > 240000) return fail(error, "invalid applied bitrate");
    if (!valid_limitation(acknowledgement.limitation)) return fail(error, "invalid limitation");
    return true;
}

bool validate_fps_v2(uint32_t fps)
{
    return fps >= 1 && fps <= kViewerQualityMaximumFpsV2;
}

bool validate_capabilities_v2(const ViewerQualityCapabilitiesV2& capabilities, std::string* error)
{
    if (capabilities.request_id == 0) return fail(error, "capabilities request id must be non-zero");
    if (!validate_fps_v2(capabilities.source_refresh_hz)
        || !validate_fps_v2(capabilities.max_capture_fps)
        || !validate_fps_v2(capabilities.max_encode_fps)) {
        return fail(error, "invalid v2 fps capability");
    }
    return true;
}

bool validate_request_v2(const ViewerQualityRequestV2& request, std::string* error)
{
    if (request.request_id == 0) return fail(error, "request id must be non-zero");
    if (!valid_mode(request.mode)) return fail(error, "invalid quality mode");
    if (!validate_dimensions(request.target_width, request.target_height)) return fail(error, "invalid target dimensions");
    if (!validate_fps_v2(request.target_fps) || !validate_fps_v2(request.viewer_refresh_hz)) {
        return fail(error, "invalid v2 target fps");
    }
    if (request.max_bitrate_kbps < 256 || request.max_bitrate_kbps > 240000) return fail(error, "invalid bitrate");
    if (request.priority > 100) return fail(error, "invalid priority");
    return true;
}

bool valid_limit_reason(ViewerQualityLimitReason reason)
{
    return static_cast<uint32_t>(reason) <= static_cast<uint32_t>(ViewerQualityLimitReason::LegacyProtocol);
}

bool validate_acknowledgement_v2(const ViewerQualityAcknowledgementV2& acknowledgement, std::string* error)
{
    if (acknowledgement.request_id == 0) return fail(error, "ack request id must be non-zero");
    if (!valid_mode(acknowledgement.applied_mode)) return fail(error, "invalid applied mode");
    if (!validate_dimensions(acknowledgement.applied_width, acknowledgement.applied_height)) return fail(error, "invalid applied dimensions");
    if (!validate_fps_v2(acknowledgement.applied_fps)) return fail(error, "invalid v2 applied fps");
    if (acknowledgement.applied_bitrate_kbps < 256 || acknowledgement.applied_bitrate_kbps > 240000) return fail(error, "invalid applied bitrate");
    if (!valid_limitation(acknowledgement.limitation) || !valid_limit_reason(acknowledgement.limit_reason)) {
        return fail(error, "invalid v2 limitation");
    }
    ViewerQualityCapabilitiesV2 capabilities;
    capabilities.request_id = acknowledgement.request_id;
    capabilities.source_refresh_hz = acknowledgement.source_refresh_hz;
    capabilities.max_capture_fps = acknowledgement.max_capture_fps;
    capabilities.max_encode_fps = acknowledgement.max_encode_fps;
    capabilities.online_fps_update = acknowledgement.online_fps_update;
    return validate_capabilities_v2(capabilities, error);
}

bool read_tokens(std::string_view wire, std::string* kind, std::string_view* payload, std::string* error)
{
    if (wire.empty() || wire.size() > kMaxViewerQualityMessageBytes) return fail(error, "quality message size is invalid");
    const size_t first_space = wire.find(' ');
    if (first_space == std::string_view::npos || wire.substr(0, first_space) != kQualityPrefix) return fail(error, "quality prefix is invalid");
    const size_t second_space = wire.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) return fail(error, "quality message kind is missing");
    if (kind) *kind = std::string(wire.substr(first_space + 1, second_space - first_space - 1));
    if (payload) *payload = wire.substr(second_space + 1);
    return true;
}
// ===end====

} // namespace

// =====wjy====
bool is_viewer_quality_message(std::string_view wire)
{
    return wire.starts_with(kQualityPrefix) || wire.starts_with(kQualityPrefixV2); // 命中任一保留前缀后都不得进入键鼠注入解析器。
}

bool serialize_viewer_quality_request(const ViewerQualityRequest& request, std::string* output, std::string* error)
{
    if (!output) return fail(error, "request output is null");
    if (!validate_request(request, error)) return false;
    std::ostringstream stream;
    stream << kQualityPrefix << " req "
        << request.request_id << ' '
        << static_cast<uint32_t>(request.mode) << ' '
        << request.target_width << ' '
        << request.target_height << ' '
        << request.target_fps << ' '
        << request.max_bitrate_kbps << ' '
        << request.priority;
    *output = stream.str();
    return output->size() <= kMaxViewerQualityMessageBytes;
}

bool parse_viewer_quality_request(std::string_view wire, ViewerQualityRequest* request, std::string* error)
{
    if (!request) return fail(error, "request output is null");
    std::string kind;
    std::string_view payload;
    if (!read_tokens(wire, &kind, &payload, error)) return false;
    if (kind != "req") return fail(error, "quality message is not a request");
    std::istringstream stream{std::string(payload)};
    std::string request_id;
    std::string mode;
    std::string width;
    std::string height;
    std::string fps;
    std::string bitrate;
    std::string priority;
    std::string extra;
    if (!(stream >> request_id >> mode >> width >> height >> fps >> bitrate >> priority) || (stream >> extra)) {
        return fail(error, "quality request field count is invalid");
    }
    ViewerQualityRequest parsed;
    uint32_t mode_value = 0;
    if (!parse_integer(request_id, &parsed.request_id)
        || !parse_integer(mode, &mode_value)
        || !parse_integer(width, &parsed.target_width)
        || !parse_integer(height, &parsed.target_height)
        || !parse_integer(fps, &parsed.target_fps)
        || !parse_integer(bitrate, &parsed.max_bitrate_kbps)
        || !parse_integer(priority, &parsed.priority)) {
        return fail(error, "quality request contains a non-integer field");
    }
    parsed.mode = static_cast<ViewerQualityMode>(mode_value);
    if (!validate_request(parsed, error)) return false;
    *request = parsed;
    return true;
}

bool serialize_viewer_quality_acknowledgement(const ViewerQualityAcknowledgement& acknowledgement, std::string* output, std::string* error)
{
    if (!output) return fail(error, "ack output is null");
    if (!validate_acknowledgement(acknowledgement, error)) return false;
    std::ostringstream stream;
    stream << kQualityPrefix << " ack "
        << acknowledgement.request_id << ' '
        << (acknowledgement.supported ? 1 : 0) << ' '
        << static_cast<uint32_t>(acknowledgement.applied_mode) << ' '
        << acknowledgement.applied_width << ' '
        << acknowledgement.applied_height << ' '
        << acknowledgement.applied_fps << ' '
        << acknowledgement.applied_bitrate_kbps << ' '
        << static_cast<uint32_t>(acknowledgement.limitation);
    *output = stream.str();
    return output->size() <= kMaxViewerQualityMessageBytes;
}

bool parse_viewer_quality_acknowledgement(std::string_view wire, ViewerQualityAcknowledgement* acknowledgement, std::string* error)
{
    if (!acknowledgement) return fail(error, "ack output is null");
    std::string kind;
    std::string_view payload;
    if (!read_tokens(wire, &kind, &payload, error)) return false;
    if (kind != "ack") return fail(error, "quality message is not an acknowledgement");
    std::istringstream stream{std::string(payload)};
    std::string request_id;
    std::string supported;
    std::string mode;
    std::string width;
    std::string height;
    std::string fps;
    std::string bitrate;
    std::string limitation;
    std::string extra;
    if (!(stream >> request_id >> supported >> mode >> width >> height >> fps >> bitrate >> limitation) || (stream >> extra)) {
        return fail(error, "quality acknowledgement field count is invalid");
    }
    ViewerQualityAcknowledgement parsed;
    uint32_t supported_value = 0;
    uint32_t mode_value = 0;
    uint32_t limitation_value = 0;
    if (!parse_integer(request_id, &parsed.request_id)
        || !parse_integer(supported, &supported_value)
        || !parse_integer(mode, &mode_value)
        || !parse_integer(width, &parsed.applied_width)
        || !parse_integer(height, &parsed.applied_height)
        || !parse_integer(fps, &parsed.applied_fps)
        || !parse_integer(bitrate, &parsed.applied_bitrate_kbps)
        || !parse_integer(limitation, &limitation_value)
        || supported_value > 1) {
        return fail(error, "quality acknowledgement contains an invalid integer field");
    }
    parsed.supported = supported_value != 0;
    parsed.applied_mode = static_cast<ViewerQualityMode>(mode_value);
    parsed.limitation = static_cast<ViewerQualityLimitation>(limitation_value);
    if (!validate_acknowledgement(parsed, error)) return false;
    *acknowledgement = parsed;
    return true;
}

bool serialize_viewer_quality_capabilities_request_v2(uint64_t requestId, std::string* output, std::string* error)
{
    if (!output) return fail(error, "capabilities request output is null");
    if (requestId == 0) return fail(error, "capabilities request id must be non-zero");
    *output = std::string(kQualityPrefixV2) + " caps_req " + std::to_string(requestId);
    return output->size() <= kMaxViewerQualityMessageBytes;
}

bool parse_viewer_quality_capabilities_request_v2(std::string_view wire, uint64_t* requestId, std::string* error)
{
    if (!requestId) return fail(error, "capabilities request output is null");
    const std::string prefix = std::string(kQualityPrefixV2) + " caps_req ";
    if (!wire.starts_with(prefix) || wire.size() > kMaxViewerQualityMessageBytes) {
        return fail(error, "v2 capabilities request prefix is invalid");
    }
    const std::string_view payload = wire.substr(prefix.size());
    if (!parse_integer(payload, requestId) || *requestId == 0) {
        return fail(error, "v2 capabilities request id is invalid");
    }
    return true;
}

bool serialize_viewer_quality_capabilities_v2(const ViewerQualityCapabilitiesV2& capabilities, std::string* output, std::string* error)
{
    if (!output) return fail(error, "capabilities output is null");
    if (!validate_capabilities_v2(capabilities, error)) return false;
    std::ostringstream stream;
    stream << kQualityPrefixV2 << " caps "
           << capabilities.request_id << ' '
           << capabilities.source_refresh_hz << ' '
           << capabilities.max_capture_fps << ' '
           << capabilities.max_encode_fps << ' '
           << (capabilities.online_fps_update ? 1 : 0);
    *output = stream.str();
    return output->size() <= kMaxViewerQualityMessageBytes;
}

bool parse_viewer_quality_capabilities_v2(std::string_view wire, ViewerQualityCapabilitiesV2* capabilities, std::string* error)
{
    if (!capabilities) return fail(error, "capabilities output is null");
    const std::string prefix = std::string(kQualityPrefixV2) + " caps ";
    if (!wire.starts_with(prefix) || wire.size() > kMaxViewerQualityMessageBytes) {
        return fail(error, "v2 capabilities prefix is invalid");
    }
    std::istringstream stream{std::string(wire.substr(prefix.size()))};
    std::string requestId, sourceRefresh, maxCapture, maxEncode, online, extra;
    if (!(stream >> requestId >> sourceRefresh >> maxCapture >> maxEncode >> online) || (stream >> extra)) {
        return fail(error, "v2 capabilities field count is invalid");
    }
    ViewerQualityCapabilitiesV2 parsed;
    uint32_t onlineValue = 0;
    if (!parse_integer(requestId, &parsed.request_id)
        || !parse_integer(sourceRefresh, &parsed.source_refresh_hz)
        || !parse_integer(maxCapture, &parsed.max_capture_fps)
        || !parse_integer(maxEncode, &parsed.max_encode_fps)
        || !parse_integer(online, &onlineValue)
        || onlineValue > 1) {
        return fail(error, "v2 capabilities contains invalid fields");
    }
    parsed.online_fps_update = onlineValue != 0;
    if (!validate_capabilities_v2(parsed, error)) return false;
    *capabilities = parsed;
    return true;
}

bool serialize_viewer_quality_request_v2(const ViewerQualityRequestV2& request, std::string* output, std::string* error)
{
    if (!output) return fail(error, "v2 request output is null");
    if (!validate_request_v2(request, error)) return false;
    std::ostringstream stream;
    stream << kQualityPrefixV2 << " req "
           << request.request_id << ' '
           << static_cast<uint32_t>(request.mode) << ' '
           << request.target_width << ' '
           << request.target_height << ' '
           << request.target_fps << ' '
           << request.max_bitrate_kbps << ' '
           << request.priority << ' '
           << request.viewer_refresh_hz;
    *output = stream.str();
    return output->size() <= kMaxViewerQualityMessageBytes;
}

bool parse_viewer_quality_request_v2(std::string_view wire, ViewerQualityRequestV2* request, std::string* error)
{
    if (!request) return fail(error, "v2 request output is null");
    const std::string prefix = std::string(kQualityPrefixV2) + " req ";
    if (!wire.starts_with(prefix) || wire.size() > kMaxViewerQualityMessageBytes) {
        return fail(error, "v2 request prefix is invalid");
    }
    std::istringstream stream{std::string(wire.substr(prefix.size()))};
    std::string requestId, mode, width, height, fps, bitrate, priority, viewerRefresh, extra;
    if (!(stream >> requestId >> mode >> width >> height >> fps >> bitrate >> priority >> viewerRefresh) || (stream >> extra)) {
        return fail(error, "v2 request field count is invalid");
    }
    ViewerQualityRequestV2 parsed;
    uint32_t modeValue = 0;
    if (!parse_integer(requestId, &parsed.request_id)
        || !parse_integer(mode, &modeValue)
        || !parse_integer(width, &parsed.target_width)
        || !parse_integer(height, &parsed.target_height)
        || !parse_integer(fps, &parsed.target_fps)
        || !parse_integer(bitrate, &parsed.max_bitrate_kbps)
        || !parse_integer(priority, &parsed.priority)
        || !parse_integer(viewerRefresh, &parsed.viewer_refresh_hz)) {
        return fail(error, "v2 request contains a non-integer field");
    }
    parsed.mode = static_cast<ViewerQualityMode>(modeValue);
    if (!validate_request_v2(parsed, error)) return false;
    *request = parsed;
    return true;
}

bool serialize_viewer_quality_acknowledgement_v2(const ViewerQualityAcknowledgementV2& acknowledgement, std::string* output, std::string* error)
{
    if (!output) return fail(error, "v2 acknowledgement output is null");
    if (!validate_acknowledgement_v2(acknowledgement, error)) return false;
    std::ostringstream stream;
    stream << kQualityPrefixV2 << " ack "
           << acknowledgement.request_id << ' '
           << (acknowledgement.supported ? 1 : 0) << ' '
           << static_cast<uint32_t>(acknowledgement.applied_mode) << ' '
           << acknowledgement.applied_width << ' '
           << acknowledgement.applied_height << ' '
           << acknowledgement.applied_fps << ' '
           << acknowledgement.applied_bitrate_kbps << ' '
           << static_cast<uint32_t>(acknowledgement.limitation) << ' '
           << acknowledgement.source_refresh_hz << ' '
           << acknowledgement.max_capture_fps << ' '
           << acknowledgement.max_encode_fps << ' '
           << (acknowledgement.online_fps_update ? 1 : 0) << ' '
           << static_cast<uint32_t>(acknowledgement.limit_reason);
    *output = stream.str();
    return output->size() <= kMaxViewerQualityMessageBytes;
}

bool parse_viewer_quality_acknowledgement_v2(std::string_view wire, ViewerQualityAcknowledgementV2* acknowledgement, std::string* error)
{
    if (!acknowledgement) return fail(error, "v2 acknowledgement output is null");
    const std::string prefix = std::string(kQualityPrefixV2) + " ack ";
    if (!wire.starts_with(prefix) || wire.size() > kMaxViewerQualityMessageBytes) {
        return fail(error, "v2 acknowledgement prefix is invalid");
    }
    std::istringstream stream{std::string(wire.substr(prefix.size()))};
    std::string requestId, supported, mode, width, height, fps, bitrate, limitation;
    std::string sourceRefresh, maxCapture, maxEncode, online, reason, extra;
    if (!(stream >> requestId >> supported >> mode >> width >> height >> fps >> bitrate >> limitation
          >> sourceRefresh >> maxCapture >> maxEncode >> online >> reason) || (stream >> extra)) {
        return fail(error, "v2 acknowledgement field count is invalid");
    }
    ViewerQualityAcknowledgementV2 parsed;
    uint32_t supportedValue = 0, modeValue = 0, limitationValue = 0, onlineValue = 0, reasonValue = 0;
    if (!parse_integer(requestId, &parsed.request_id)
        || !parse_integer(supported, &supportedValue)
        || !parse_integer(mode, &modeValue)
        || !parse_integer(width, &parsed.applied_width)
        || !parse_integer(height, &parsed.applied_height)
        || !parse_integer(fps, &parsed.applied_fps)
        || !parse_integer(bitrate, &parsed.applied_bitrate_kbps)
        || !parse_integer(limitation, &limitationValue)
        || !parse_integer(sourceRefresh, &parsed.source_refresh_hz)
        || !parse_integer(maxCapture, &parsed.max_capture_fps)
        || !parse_integer(maxEncode, &parsed.max_encode_fps)
        || !parse_integer(online, &onlineValue)
        || !parse_integer(reason, &reasonValue)
        || supportedValue > 1 || onlineValue > 1) {
        return fail(error, "v2 acknowledgement contains invalid fields");
    }
    parsed.supported = supportedValue != 0;
    parsed.applied_mode = static_cast<ViewerQualityMode>(modeValue);
    parsed.limitation = static_cast<ViewerQualityLimitation>(limitationValue);
    parsed.online_fps_update = onlineValue != 0;
    parsed.limit_reason = static_cast<ViewerQualityLimitReason>(reasonValue);
    if (!validate_acknowledgement_v2(parsed, error)) return false;
    *acknowledgement = parsed;
    return true;
}
// ===end====

} // namespace uu
