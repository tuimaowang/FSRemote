#pragma once

#include <cstdint>

#include <QString>

namespace platform {

enum class DeviceControlAction {
    Shutdown,
    Restart,
};

// =====wjy====
enum class RemoteUpdateRequestResult {
    Accepted,
    UpToDate,
    Failed,
}; // wjy: 远程更新需要区分“已受理”和“目标已是最新版”，不能像关机命令一样只返回成功/失败。
// ===end====

class DeviceCommandServer final {
public:
    DeviceCommandServer() = default;
    ~DeviceCommandServer();

    bool start(uint16_t port = 49102);
    void stop();

private:
    class Impl;
    Impl* m_impl = nullptr;
};

class DeviceCommandService final {
public:
    static bool send(const QString& hostIp, DeviceControlAction action, uint16_t port = 49102, int timeoutMs = 1500);
    static bool sendWakeProxy(const QString& hostIp, const QString& macAddress, QString* errorMessage = nullptr, uint16_t port = 49102, int timeoutMs = 1500);
    static bool renameDevice(const QString& hostIp, const QString& newName, QString* errorMessage = nullptr, uint16_t port = 49102, int timeoutMs = 1500);
    static bool authorizeTerminalKey(const QString& hostIp, const QString& publicKey, QString* errorMessage = nullptr, uint16_t port = 49102, int timeoutMs = 1500);
    // =====wjy====
    static RemoteUpdateRequestResult requestUpdate(const QString& hostIp, QString* errorMessage = nullptr, uint16_t port = 49102, int timeoutMs = 1500); // wjy: 只请求目标机启动它自己的更新流程，控制端不直接复制或覆盖目标安装目录。
    // ===end====
};

} // namespace platform
