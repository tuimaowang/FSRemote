#pragma once

#include <cstdint>
#include <functional>

#include <QString>
#include <QtGlobal>

class QTcpServer;

namespace platform {

enum class DevicePresenceState {
    Unknown,
    Online,
    Busy,
    Offline,
};

// =====wjy====
struct RemoteScriptRuntimeInfo {
    bool supported = false; // wjy: 区分新版目标端和未携带脚本字段的旧版目标端，避免把“没有字段”误判为“没有运行”。
    bool statusKnown = false; // wjy: 只有目标端完成清单和进程校验后才为 true，清单损坏或进程无权检查时保持未知。
    bool running = false; // wjy: 目标端确认脚本控制进程仍存活时为 true，是设备栏运行图标的远端权威来源。
    QString runId; // wjy: 标识一次唯一执行，用于正常结束或停止时避免旧任务误删新任务的活动清单。
    QString workName; // wjy: 保存目标 work 子目录名，让控制端重启后仍能定位 PID 文件并停止脚本。
    QString scriptName; // wjy: 保存正在执行的入口文件名，用于恢复脚本日志标题和提示文字。
    qint64 controllerPid = 0; // wjy: 目标 PowerShell 控制进程 PID，状态服务会先验证它再上报 running。
    qint64 startedAtEpochMs = 0; // wjy: 记录目标执行开始时间，并和 Windows 进程创建时间交叉校验，降低 PID 复用误判。
};
// ===end====

struct DeviceStatusInfo {
    DevicePresenceState state = DevicePresenceState::Offline;
    QString terminalUser;
    QString deviceName;
    QString localIp;
    QString subnetMask;
    QString broadcastIp;
    QString mac;
    RemoteScriptRuntimeInfo scriptRuntime; // wjy: 在现有设备在线状态响应中附带目标脚本运行信息，不额外增加服务端口。
    // =====wjy====
    int remoteSessionCount = 0; // wjy: 目标端当前远控会话数 0-10，驱动设备行数字徽标；旧协议缺失时保持 0。
    QString remoteControllerNames; // wjy: 逗号分隔控制端设备名，悬停远控徽标时气泡展示。
    // ===end====
};

class DeviceStatusServer final {
public:
    // =====wjy====
    struct HostSessionSnapshot {
        int sessionCount = 0;
        QString controllerNames; // wjy: UTF-8 设备名，逗号分隔。
    };
    explicit DeviceStatusServer(std::function<HostSessionSnapshot()> sessionSnapshotProvider);
    // ===end====
    ~DeviceStatusServer();

    bool start(uint16_t port = 49101);
    void stop();

private:
    // =====wjy====
    std::function<HostSessionSnapshot()> m_sessionSnapshotProvider;
    // ===end====
    QTcpServer* m_server = nullptr;
};

class DeviceStatusService final {
public:
    static DeviceStatusInfo query(const QString& hostIp, uint16_t port = 49101, int timeoutMs = 900);
    static DevicePresenceState probe(const QString& hostIp, uint16_t port = 49101, int timeoutMs = 900);
    static QString terminalUser(const QString& hostIp, uint16_t port = 49101, int timeoutMs = 900);
    // =====wjy====
    static RemoteScriptRuntimeInfo localScriptRuntime(); // wjy: 实时广播复用与 TCP 状态服务完全相同的本机清单/PID 校验，不建立第二套脚本判断逻辑。
    // ===end====
};

} // namespace platform
