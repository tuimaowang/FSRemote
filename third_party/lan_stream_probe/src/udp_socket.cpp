#include "udp_socket.h"

#include <ws2tcpip.h>

namespace lsp {

WsaRuntime::WsaRuntime()
{
    WSADATA data = {};
    ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

WsaRuntime::~WsaRuntime()
{
    if (ok_) {
        WSACleanup();
    }
}

UdpSocket::UdpSocket() = default;

UdpSocket::~UdpSocket()
{
    close();
}

bool UdpSocket::open(uint16_t bind_port, std::string* error)
{
    close();
    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        if (error) *error = "socket failed";
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(bind_port);
    if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        if (error) *error = "bind failed";
        close();
        return false;
    }
    return true;
}

bool UdpSocket::set_nonblocking(bool enabled, std::string* error)
{
    u_long mode = enabled ? 1 : 0;
    if (ioctlsocket(socket_, FIONBIO, &mode) != 0) {
        if (error) *error = "ioctlsocket(FIONBIO) failed";
        return false;
    }
    return true;
}

bool UdpSocket::set_send_buffer(int bytes, std::string*)
{
    return setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

bool UdpSocket::set_recv_buffer(int bytes, std::string*)
{
    return setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

int UdpSocket::send_buffer_size() const
{
    int value = 0;
    int len = sizeof(value);
    if (getsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&value), &len) != 0) {
        return 0;
    }
    return value;
}

int UdpSocket::recv_buffer_size() const
{
    int value = 0;
    int len = sizeof(value);
    if (getsockopt(socket_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&value), &len) != 0) {
        return 0;
    }
    return value;
}

bool UdpSocket::send_to(const std::string& ip, uint16_t port, const std::vector<uint8_t>& packet)
{
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        return false;
    }
    const int sent = sendto(socket_, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
                            0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<int>(packet.size());
}

bool UdpSocket::send_to(const sockaddr_in& addr, const std::vector<uint8_t>& packet)
{
    const int sent = sendto(socket_, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
                            0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<int>(packet.size());
}

int UdpSocket::recv(uint8_t* buffer, int capacity, sockaddr_in* from)
{
    int from_len = sizeof(sockaddr_in);
    return recvfrom(socket_, reinterpret_cast<char*>(buffer), capacity, 0,
                    reinterpret_cast<sockaddr*>(from), &from_len);
}

void UdpSocket::close()
{
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

} // namespace lsp
