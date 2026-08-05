#pragma once

#include "system/DeviceStatusService.h"

namespace platform {

// =====wjy====
enum class DeviceActionKind {
    Wake,
    Shutdown,
    Restart,
    Update,
    Terminal,
};

bool isDeviceActionAllowed(DeviceActionKind action, DevicePresenceState state);
// ===end====

} // namespace platform
