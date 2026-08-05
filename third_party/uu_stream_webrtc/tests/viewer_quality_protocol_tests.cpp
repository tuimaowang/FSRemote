#include "viewer_quality_protocol.h"

#include <cassert>
#include <string>

int main()
{
    // =====wjy====
    uu::ViewerQualityRequest request;
    request.request_id = 42;
    request.mode = uu::ViewerQualityMode::HighQualityLocked;
    request.target_width = 1280;
    request.target_height = 720;
    request.target_fps = 60;
    request.max_bitrate_kbps = 48000;
    request.priority = 100;
    std::string wire;
    std::string error;
    assert(uu::serialize_viewer_quality_request(request, &wire, &error));
    assert(uu::is_viewer_quality_message(wire));
    uu::ViewerQualityRequest parsed;
    assert(uu::parse_viewer_quality_request(wire, &parsed, &error));
    assert(parsed.request_id == request.request_id);
    assert(parsed.target_width == 1280 && parsed.target_height == 720);
    assert(parsed.target_fps == 60 && parsed.priority == 100); // wjy: 请求往返保持高质量优先和FPS字段不变。

    uu::ViewerQualityAcknowledgement acknowledgement;
    acknowledgement.request_id = request.request_id;
    acknowledgement.applied_mode = request.mode;
    acknowledgement.applied_width = 1280;
    acknowledgement.applied_height = 720;
    acknowledgement.applied_fps = 60;
    acknowledgement.applied_bitrate_kbps = 46000;
    acknowledgement.limitation = uu::ViewerQualityLimitation::Clamped;
    assert(uu::serialize_viewer_quality_acknowledgement(acknowledgement, &wire, &error));
    uu::ViewerQualityAcknowledgement parsed_acknowledgement;
    assert(uu::parse_viewer_quality_acknowledgement(wire, &parsed_acknowledgement, &error));
    assert(parsed_acknowledgement.limitation == uu::ViewerQualityLimitation::Clamped);

    assert(!uu::parse_viewer_quality_request("__fsremote_quality_v1 req 1 1 0 720 60 10000 50", &parsed, &error)); // wjy: 半个0分辨率必须拒绝。
    assert(!uu::parse_viewer_quality_request("__fsremote_quality_v1 req 1 1 1280 720 0 10000 50", &parsed, &error)); // wjy: 0 FPS不能被解释成暂停或断流。
    assert(!uu::parse_viewer_quality_request("__fsremote_quality_v1 req 1 1 1280 720 60 10000 50 extra", &parsed, &error));
    assert(uu::is_viewer_quality_message("__fsremote_quality_v1 malformed")); // wjy: 命中保留前缀的畸形消息仍归质量解析器，不得进入键鼠注入。

    uu::ViewerQualityCapabilitiesV2 capabilities;
    capabilities.request_id = 43;
    capabilities.source_refresh_hz = 144;
    capabilities.max_capture_fps = 144;
    capabilities.max_encode_fps = 240;
    capabilities.online_fps_update = true;
    assert(uu::serialize_viewer_quality_capabilities_v2(capabilities, &wire, &error));
    uu::ViewerQualityCapabilitiesV2 parsedCapabilities;
    assert(uu::parse_viewer_quality_capabilities_v2(wire, &parsedCapabilities, &error));
    assert(parsedCapabilities.source_refresh_hz == 144 && parsedCapabilities.online_fps_update);

    uu::ViewerQualityRequestV2 requestV2;
    requestV2.request_id = 44;
    requestV2.mode = uu::ViewerQualityMode::HighQualityLocked;
    requestV2.target_fps = 120;
    requestV2.max_bitrate_kbps = 80000;
    requestV2.priority = 100;
    requestV2.viewer_refresh_hz = 144;
    assert(uu::serialize_viewer_quality_request_v2(requestV2, &wire, &error));
    uu::ViewerQualityRequestV2 parsedRequestV2;
    assert(uu::parse_viewer_quality_request_v2(wire, &parsedRequestV2, &error));
    assert(parsedRequestV2.target_fps == 120 && parsedRequestV2.viewer_refresh_hz == 144);
    requestV2.target_fps = 360;
    requestV2.viewer_refresh_hz = 360;
    assert(uu::serialize_viewer_quality_request_v2(requestV2, &wire, &error));
    requestV2.target_fps = 361;
    assert(!uu::serialize_viewer_quality_request_v2(requestV2, &wire, &error));
    requestV2.target_fps = 120;
    requestV2.viewer_refresh_hz = 0;
    assert(!uu::serialize_viewer_quality_request_v2(requestV2, &wire, &error));
    capabilities.source_refresh_hz = 0;
    assert(!uu::serialize_viewer_quality_capabilities_v2(capabilities, &wire, &error));
    assert(!uu::parse_viewer_quality_request(
        "__fsremote_quality_v1 req 2 2 0 0 61 80000 100", &parsed, &error));
    assert(uu::is_viewer_quality_message("__fsremote_quality_v2 caps_req 1"));
    // ===end====
    return 0;
}
