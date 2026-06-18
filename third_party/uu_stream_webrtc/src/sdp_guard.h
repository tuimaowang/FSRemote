#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace uu {

struct SdpGuardResult {
    bool ok = false;
    std::vector<std::string> missing;
};

SdpGuardResult validate_uu_video_sdp(std::string_view sdp);
std::string format_sdp_guard_result(const SdpGuardResult& result);

} // namespace uu
