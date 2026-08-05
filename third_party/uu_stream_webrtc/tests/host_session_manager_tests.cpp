#include "FsRemoteStreamApi.h"
#include "session_protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

// =====wjy====
void require(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "host_session_manager_tests failed: " << message << '\n';
    std::exit(1); // wjy: 生命周期或容量断言失败立即返回非零，避免悬挂 host 被后续用例掩盖。
}

int FSREMOTE_STREAM_CALL authorizeAnyTestKey(void*, const uint8_t*, uint32_t)
{
    return 1; // wjy: 该测试只验证 manager 生命周期，使用固定授权桩让首连接停在等待 hello 的握手阶段。
}

int FSREMOTE_STREAM_CALL verifyAnyTestProof(void*, const uint8_t*, uint32_t, const uint8_t*, uint32_t, const uint8_t*, uint32_t)
{
    return 1;
}

bool sendAll(SOCKET socket, const char* data, size_t size)
{
    while (size > 0) {
        const int sent = ::send(socket, data, static_cast<int>(size), 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool recvAll(SOCKET socket, char* data, size_t size)
{
    while (size > 0) {
        const int received = ::recv(socket, data, static_cast<int>(size), 0);
        if (received <= 0) return false;
        data += received;
        size -= static_cast<size_t>(received);
    }
    return true;
}

bool sendFramed(SOCKET socket, const std::string& message)
{
    const uint32_t size = htonl(static_cast<uint32_t>(message.size()));
    return sendAll(socket, reinterpret_cast<const char*>(&size), sizeof(size))
        && sendAll(socket, message.data(), message.size());
}

bool recvFramed(SOCKET socket, std::string* message)
{
    uint32_t networkSize = 0;
    if (!recvAll(socket, reinterpret_cast<char*>(&networkSize), sizeof(networkSize))) return false;
    const uint32_t size = ntohl(networkSize);
    if (size > uu::kMaxSessionMessageBytes) return false;
    message->assign(size, '\0');
    return recvAll(socket, message->data(), size);
}

SOCKET connectWithRetry(uint16_t port)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return socket;
        ::closesocket(socket);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return INVALID_SOCKET;
}

std::string validHelloWire()
{
    uu::SessionMessage hello;
    hello.type = uu::SessionMessageType::ClientHello;
    hello.fields = {
        {"client_id", "manager-test-client"},
        {"device_name", "manager-test"},
        {"requested_role", "control"},
        {"public_key", "ssh-ed25519 manager-test-key"},
        {"client_nonce", "manager-test-nonce"},
        {"capabilities", "video,audio,control"},
    };
    std::string wire;
    std::string error;
    require(uu::serialize_session_message(hello, &wire, &error), "serialize client hello");
    return wire;
}

uu::SessionMessage receiveSessionMessage(SOCKET socket)
{
    std::string wire;
    require(recvFramed(socket, &wire), "receive framed session message");
    uu::SessionMessage message;
    std::string error;
    require(uu::parse_session_message(wire, &message, &error), "parse session message");
    return message;
}
// ===end====

} // namespace

int main()
{
    // =====wjy====
    WSADATA winsock = {};
    require(::WSAStartup(MAKEWORD(2, 2), &winsock) == 0, "WSAStartup");
    FsRemoteIdentityCallbacks callbacks = {};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.version = 1;
    callbacks.is_public_key_authorized = &authorizeAnyTestKey;
    callbacks.verify_challenge = &verifyAnyTestProof;
    fsremote_stream_set_identity_callbacks(&callbacks);

    constexpr uint16_t port = 49210;
    FsRemoteStreamHandle host = fsremote_stream_start_host(port); // wjy: 不传配置走 DLL 默认入口，测试必须证明全新设备直接获得三路会话上限。
    require(host != nullptr, "start host manager with default three-session limit");

    SOCKET first = connectWithRetry(port);
    require(first != INVALID_SOCKET, "connect first pending handshake");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SOCKET second = connectWithRetry(port);
    require(second != INVALID_SOCKET, "connect second session within configured limit");
    require(sendFramed(second, validHelloWire()), "send second hello");
    require(receiveSessionMessage(second).type == uu::SessionMessageType::ServerChallenge,
        "second connection must reach challenge stage"); // wjy: 配置为 3 时，第二个并发 worker 不再收到旧 capacity 拒绝。

    SOCKET third = connectWithRetry(port);
    require(third != INVALID_SOCKET, "connect third session within configured limit");
    require(sendFramed(third, validHelloWire()), "send third hello");
    require(receiveSessionMessage(third).type == uu::SessionMessageType::ServerChallenge,
        "third connection must reach challenge stage"); // wjy: 第三个并发会话同样保留独立握手状态和 socket 生命周期。

    SOCKET fourth = connectWithRetry(port);
    require(fourth != INVALID_SOCKET, "connect capacity probe above configured limit");
    const uu::SessionMessage capacity = receiveSessionMessage(fourth);
    require(capacity.type == uu::SessionMessageType::AdmissionRejected, "fourth connection must be rejected");
    require(capacity.fields.at("reason") == "capacity", "fourth rejection reason must be capacity");
    ::closesocket(fourth); // wjy: 超限连接只关闭自身，三个已登记 worker 不受影响。

    ::closesocket(second); // wjy: 主动中断一个握手会话，用第一条既有连接继续收 challenge 验证故障隔离。
    require(sendFramed(first, validHelloWire()), "send first hello after second disconnect");
    require(receiveSessionMessage(first).type == uu::SessionMessageType::ServerChallenge,
        "one failed session must not close another active session");
    ::closesocket(first);
    ::closesocket(third); // wjy: 全部三个槽位释放后，后续连接应由 manager 回收旧 worker 并重新准入。
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    SOCKET reconnect = connectWithRetry(port);
    require(reconnect != INVALID_SOCKET, "connect after completed workers");
    require(sendFramed(reconnect, validHelloWire()), "send hello after reconnect");
    const uu::SessionMessage challenge = receiveSessionMessage(reconnect);
    require(challenge.type == uu::SessionMessageType::ServerChallenge, "persistent listener must admit reconnect to challenge stage");
    ::closesocket(reconnect); // wjy: 在 proof 前关闭，测试不会创建 PeerConnection、采集器或编码器。
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    SOCKET pendingShutdown = connectWithRetry(port);
    require(pendingShutdown != INVALID_SOCKET, "connect pending shutdown handshake");
    const auto shutdownBegin = std::chrono::steady_clock::now();
    fsremote_stream_stop(host); // wjy: 关闭 listener 和全部客户端 socket 后必须 join manager 与会话 worker。
    const auto shutdownMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shutdownBegin).count();
    require(shutdownMs < 3000, "host shutdown must not wait for handshake timeout");
    ::closesocket(pendingShutdown);

    constexpr uint16_t idlePort = 49211;
    FsRemoteStreamHandle idleHost = fsremote_stream_start_host(idlePort); // wjy: 单独启动一个从未收到连接的Host，复现生产更新时Accept线程永久阻塞的真实条件。
    require(idleHost != nullptr, "start idle host for bounded accept shutdown regression");
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // wjy: 等待Accept线程进入select轮询，测试不能因创建后立即stop而绕过监听循环。
    const auto idleShutdownBegin = std::chrono::steady_clock::now();
    fsremote_stream_stop(idleHost); // wjy: 没有客户端连接负责唤醒线程时，200ms轮询仍必须让Host自行关闭listener并完成join。
    const auto idleShutdownMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - idleShutdownBegin).count();
    require(idleShutdownMs < 3000, "idle host shutdown must not depend on a connection waking accept");

    fsremote_stream_set_identity_callbacks(nullptr);
    ::WSACleanup();
    std::cout << "host_session_manager_tests passed shutdown_ms=" << shutdownMs
              << " idle_shutdown_ms=" << idleShutdownMs << '\n'; // wjy: 输出活动握手与完全空闲两种关闭耗时，CI日志可以直接验证轮询上界。
    // ===end====
    return 0;
}
