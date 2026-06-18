#include "system/PowerManager.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace platform {

void PowerManager::setPreventSleepEnabled(bool enabled)
{
#if defined(_WIN32)
    SetThreadExecutionState(enabled
        ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED
        : ES_CONTINUOUS);
#else
    (void)enabled;
#endif
}

} // namespace platform
