#pragma once

#include <string>
#include <vector>

namespace platform {

struct WolSetting {
    bool wol_supported = false;
    bool cable_connected = false;
    bool allow_S5_wake_on_lan = false;
    bool allow_wake_on_magic_packet = false;
    bool allow_wake_up_this_device = false;
    std::string board_manufacturer;
    std::vector<std::string> diagnostics;
};

struct WolApplyResult {
    bool success = false;
    bool permission_denied = false;
    WolSetting setting;
};

class WolDetector final {
public:
    static WolSetting detect();
    static WolApplyResult enable();
};

} // namespace platform
