#pragma once

#include "system/InputScriptExecutionService.h" // wjy: 命令协议直接复用独立键鼠脚本请求和状态数据结构，不接入主界面右键脚本类型。

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
    static bool authorizeTerminalKey(const QString& hostIp, const QString& publicKey, QString* errorMessage = nullptr, uint16_t port = 49102, int timeoutMs = 10000); // wjy: 密钥登记在后台等待完整回复，10秒覆盖目标端一次最长8秒验签及锁交接开销。
    // =====wjy====
    static RemoteUpdateStatus queryUpdateStatus(const QString& hostIp, QString* errorMessage = nullptr, uint16_t port = 49102, int timeoutMs = 700); // wjy: 远控窗口短轮询目标更新阶段，避免把正常重启误显示为普通断线。
    // ===end====
    // =====wjy====
    static RemoteUpdateRequestResult requestUpdate(const QString& hostIp, QString* errorMessage = nullptr,
        uint16_t port = 49102, int timeoutMs = 1500, const QString& expectedVersion = QString()); // wjy: 只请求目标机启动它自己的更新流程，控制端不直接复制或覆盖目标安装目录。
    static bool requestDeviceListSync(const QString& hostIp, QString* errorMessage = nullptr, uint16_t port = 49102, int timeoutMs = 700); // wjy: 只通知目标设备立即读取共享 revision，设备 JSON 本身不通过命令端口传输。
    static bool requestScreenshot(
        const QString& hostIp,
        const QString& groupName,
        const QString& deviceName,
        QString* filePath,
        QString* errorMessage = nullptr,
        uint16_t port = 49102,
        int timeoutMs = 60000); // wjy: 主控只发送命名信息并等待共享路径；60秒覆盖大分辨率PNG在弱网共享目录中的写入时间。
    // ===end====

    // =====wjy====
    static RemoteInputScriptCommandResult requestInputScriptStart(
        const QString& hostIp,
        const RemoteInputScriptStartRequest& request,
        QString* errorMessage = nullptr,
        uint16_t port = 49102,
        int timeoutMs = 1500); // wjy: F10只把共享脚本标识和播放参数交给目标端，目标端随后独立复制并执行本地缓存。
    static RemoteInputScriptCommandResult requestInputScriptStop(
        const QString& hostIp,
        const QString& runId,
        QString* errorMessage = nullptr,
        uint16_t port = 49102,
        int timeoutMs = 1500); // wjy: 停止命令携带runId，主控重连或多窗口监控时不会误停另一轮新脚本。
    static RemoteInputScriptRuntimeInfo queryInputScriptStatus(
        const QString& hostIp,
        QString* errorMessage = nullptr,
        uint16_t port = 49102,
        int timeoutMs = 700); // wjy: 新远控窗口可立即取得目标端脚本快照，UDP实时状态随后继续校准。
    // ===end====
};

} // namespace platform
