#pragma once

#include <Windows.h>

namespace faker_bridge {

[[nodiscard]] int run_bridge_server(HANDLE stop_event);

}  // namespace faker_bridge

