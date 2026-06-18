#pragma once

#include <cstdint>
#include <string_view>

namespace uu {

struct VideoPayloadProfile {
    int hevc = 96;
    int hevc_rtx = 97;
    int h264 = 98;
    int h264_rtx = 99;
    int red = 100;
    int red_rtx = 101;
    int ulpfec = 102;
    int flexfec = 35;
};

struct RtpExtensionProfile {
    int abs_send_time = 2;
    int transport_wide_cc = 3;
    int mid = 4;
    int video_orientation = 5;
    int video_content_type = 6;
    int color_space = 7;
    int playout_delay = 8;
    int toffset = 9;
    int rtp_stream_id = 10;
    int video_timing = 11;
    int video_feedback_latency = 12;
    int video_frame_is_new_frame = 13;
    int video_capture_index = 14;
    int repaired_rtp_stream_id = 15;
    int video_frame_sending_delay = 19;
};

const VideoPayloadProfile& video_payloads();
const RtpExtensionProfile& rtp_extensions();

std::string_view target_summary();
std::string_view field_trials();

} // namespace uu
