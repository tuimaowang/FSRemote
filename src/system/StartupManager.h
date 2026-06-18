#pragma once

namespace platform {

class StartupManager final {
public:
    static bool isEnabled();
    static bool setEnabled(bool enabled);
};

} // namespace platform
