#pragma once

namespace platform {

class AppSettings final {
public:
    static bool preventSleepEnabled();
    static void setPreventSleepEnabled(bool enabled);
    static bool remoteWakeupEnabled();
    static void setRemoteWakeupEnabled(bool enabled);
    static bool statusAutoRefreshEnabled();
    static void setStatusAutoRefreshEnabled(bool enabled);
    static int statusAutoRefreshIntervalSeconds();
    static void setStatusAutoRefreshIntervalSeconds(int seconds);
};

} // namespace platform
