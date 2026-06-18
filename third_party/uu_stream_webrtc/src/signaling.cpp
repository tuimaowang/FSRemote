#include "signaling.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace uu {
namespace {

bool ensure_wsa()
{
    static bool initialized = [] {
        WSADATA data = {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

bool send_all(SOCKET socket, const uint8_t* data, size_t size)
{
    while (size) {
        const int sent = ::send(socket, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool recv_all(SOCKET socket, uint8_t* data, size_t size)
{
    while (size) {
        const int got = ::recv(socket, reinterpret_cast<char*>(data), static_cast<int>(size), 0);
        if (got <= 0) return false;
        data += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

} // namespace

bool send_message(uintptr_t socket_value, const std::string& message)
{
    const SOCKET socket = static_cast<SOCKET>(socket_value);
    const uint32_t size = htonl(static_cast<uint32_t>(message.size()));
    return send_all(socket, reinterpret_cast<const uint8_t*>(&size), sizeof(size)) &&
           send_all(socket, reinterpret_cast<const uint8_t*>(message.data()), message.size());
}

bool recv_message(uintptr_t socket_value, std::string* message)
{
    const SOCKET socket = static_cast<SOCKET>(socket_value);
    uint32_t net_size = 0;
    if (!recv_all(socket, reinterpret_cast<uint8_t*>(&net_size), sizeof(net_size))) return false;
    const uint32_t size = ntohl(net_size);
    if (size > 4 * 1024 * 1024) return false;
    message->assign(size, '\0');
    return recv_all(socket, reinterpret_cast<uint8_t*>(message->data()), size);
}

uintptr_t accept_tcp(uint16_t port, std::string* error)
{
    if (!ensure_wsa()) {
        if (error) *error = "WSAStartup failed";
        return 0;
    }
    SOCKET server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        if (error) *error = "socket failed";
        return 0;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(server, 1) == SOCKET_ERROR) {
        if (error) *error = "bind/listen failed";
        closesocket(server);
        return 0;
    }
    SOCKET client = ::accept(server, nullptr, nullptr);
    closesocket(server);
    if (client == INVALID_SOCKET) {
        if (error) *error = "accept failed";
        return 0;
    }
    return static_cast<uintptr_t>(client);
}

uintptr_t connect_tcp(const std::string& ip, uint16_t port, std::string* error)
{
    if (!ensure_wsa()) {
        if (error) *error = "WSAStartup failed";
        return 0;
    }
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        if (error) *error = "socket failed";
        return 0;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1 ||
        ::connect(socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        if (error) *error = "connect failed";
        closesocket(socket);
        return 0;
    }
    return static_cast<uintptr_t>(socket);
}

void close_socket(uintptr_t socket)
{
    if (!socket) {
        return;
    }

    const SOCKET s = static_cast<SOCKET>(socket);
    ::shutdown(s, SD_BOTH);
    ::closesocket(s);
}

} // namespace uu
