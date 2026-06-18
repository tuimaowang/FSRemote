#include "sdp_guard.h"

#include <sstream>

namespace uu {
namespace {

bool contains(std::string_view text, std::string_view needle)
{
    return text.find(needle) != std::string_view::npos;
}

void require_line(std::string_view sdp, std::string_view line, std::vector<std::string>* missing)
{
    if (!contains(sdp, line)) missing->emplace_back(line);
}

} // namespace

SdpGuardResult validate_uu_video_sdp(std::string_view sdp)
{
    SdpGuardResult result;

    require_line(sdp, "UDP/TLS/RTP/SAVPF", &result.missing);
    require_line(sdp, "a=rtcp-mux", &result.missing);

    require_line(sdp, "H265/90000", &result.missing);
    require_line(sdp, "H264/90000", &result.missing);
    require_line(sdp, "rtx/90000", &result.missing);
    require_line(sdp, "red/90000", &result.missing);
    require_line(sdp, "ulpfec/90000", &result.missing);

    require_line(sdp, "goog-remb", &result.missing);
    require_line(sdp, "transport-cc", &result.missing);
    require_line(sdp, "ccm fir", &result.missing);
    require_line(sdp, "nack", &result.missing);
    require_line(sdp, "nack pli", &result.missing);

    require_line(sdp, "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time", &result.missing);
    require_line(sdp, "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", &result.missing);
    require_line(sdp, "urn:ietf:params:rtp-hdrext:sdes:mid", &result.missing);

    result.ok = result.missing.empty();
    return result;
}

std::string format_sdp_guard_result(const SdpGuardResult& result)
{
    if (result.ok) return "SDP matches required UU video profile";

    std::ostringstream out;
    out << "SDP is missing expected UU-style video capabilities:";
    for (const auto& item : result.missing) out << "\n  " << item;
    return out.str();
}

} // namespace uu
