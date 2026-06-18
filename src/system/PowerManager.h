#pragma once

namespace platform {

class PowerManager final {
public:
    static void setPreventSleepEnabled(bool enabled);
};

} // namespace platform
