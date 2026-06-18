#pragma once

#include <cstdint>
#include <string>

namespace uu {

bool send_message(uintptr_t socket, const std::string& message);
bool recv_message(uintptr_t socket, std::string* message);
uintptr_t accept_tcp(uint16_t port, std::string* error);
uintptr_t connect_tcp(const std::string& ip, uint16_t port, std::string* error);
void close_socket(uintptr_t socket);

} // namespace uu
