#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <winsock2.h>

namespace lsp {

class WsaRuntime {
public:
    WsaRuntime();
    ~WsaRuntime();
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    bool open(uint16_t bind_port, std::string* error);
    bool set_nonblocking(bool enabled, std::string* error);
    bool set_send_buffer(int bytes, std::string* error);
    bool set_recv_buffer(int bytes, std::string* error);
    int send_buffer_size() const;
    int recv_buffer_size() const;
    bool send_to(const std::string& ip, uint16_t port, const std::vector<uint8_t>& packet);
    bool send_to(const sockaddr_in& addr, const std::vector<uint8_t>& packet);
    int recv(uint8_t* buffer, int capacity, sockaddr_in* from);
    void close();

private:
    SOCKET socket_ = INVALID_SOCKET;
};

} // namespace lsp
