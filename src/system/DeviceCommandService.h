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

// =====wjy====
enum class RemoteUpdateStatus {
    Idle, // wjy: 目标端在线，但当前没有执行更新任务。
    Preparing, // wjy: 目标端正在下载和校验更新载荷，主进程尚未退出。
    Complete, // wjy: 目标端重启后确认本地版本已经追上共享版本。
    Failed, // wjy: 目标端准备更新失败，主进程仍保持运行。
    Unreachable, // wjy: 更新替换或重启期间命令端口暂时不可访问。
    Unsupported, // wjy: 旧客户端不认识状态查询命令，控制端继续等待其完成首次滚动升级。
};
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
    static RemoteUpdateStatus queryUpdateStatus(const QString& hostIp, QString* errorMessage = nullptr, uint16_t port = 49102, int timeoutMs = 700); // wjy: 远控窗口短轮询目标更新阶段，避免把正常重启误显示为普通断线。
    // ===end====
    // =====wjy====
    static RemoteUpdateRequestResult requestUpdate(const QString& hostIp, QString* errorMessage = nullptr,
        uint16_t port = 49102, int timeoutMs = 1500, const QString& expectedVersion = QString()); // wjy: 只请求目标机启动它自己的更新流程，控制端不直接复制或覆盖目标安装目录。
    static bool requestDeviceListSync(const QString& hostIp, QString* errorMessage = nullptr, uint16_t port = 49102, int timeoutMs = 700); // wjy: 只通知目标设备立即读取共享 revision，设备 JSON 本身不通过命令端口传输。
    // ===end====
};

} // namespace platform
