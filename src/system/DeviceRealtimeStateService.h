#pragma once

#include "system/DeviceStatusService.h"

#include <functional>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>

class QTimer;
class QUdpSocket;

namespace platform {

// =====wjy====
enum class RealtimeScriptState {
    Unknown,
    Idle,
    Running,
}; // wjy: 广播脚本状态明确区分“确认空闲”和“无法确认”，离线时绝不把未知误报为空闲。

struct DeviceRealtimeHostSession {
    QString sessionId; // wjy: 一次主机远控会话的稳定标识；完整快照按唯一会话计数，不使用 +1/-1 增量事件。
    QString controllerDeviceId;
    QString controllerName;
    QString sourceIp;

    bool operator==(const DeviceRealtimeHostSession&) const = default;
};

struct DeviceRealtimeScriptRuntime {
    RealtimeScriptState state = RealtimeScriptState::Unknown;
    QString runId;
    QString workName;
    QString scriptName;
    qint64 controllerPid = 0;
    qint64 startedAtEpochMs = 0;

    bool operator==(const DeviceRealtimeScriptRuntime&) const = default;
};

struct DeviceRealtimeControllerTarget {
    QString sessionId; // wjy: 控制端窗口自己的租约 ID，只用于诊断关联，绝不加入目标设备被控人数。
    QString targetDeviceId;
    QString targetName;
    QString targetIp;

    bool operator==(const DeviceRealtimeControllerTarget&) const = default;
};

struct DeviceRealtimeUpdateState {
    QString installedVersion;
    bool runtimeRepairRequired = false;

    bool operator==(const DeviceRealtimeUpdateState&) const = default;
};

struct DeviceRealtimeLocalState {
    QList<DeviceRealtimeHostSession> hostSessions;
    DeviceRealtimeScriptRuntime script;
    RemoteInputScriptRuntimeInfo inputScript; // wjy: F9/F10目标端运行状态独立于右键脚本，所有订阅设备接收同一权威快照。
    DeviceRealtimeUpdateState update;

    bool operator==(const DeviceRealtimeLocalState&) const = default;
};

struct DeviceRealtimeSnapshot {
    int protocolVersion = 1;
    QString deviceId;
    QString bootId;
    quint64 sequence = 0;
    qint64 ttlMs = 0;
    QString deviceName;
    QString deviceIp;
    QString loginUser;
    QList<DeviceRealtimeHostSession> hostSessions;
    DeviceRealtimeScriptRuntime script;
    RemoteInputScriptRuntimeInfo inputScript; // wjy: UDP快照追加键鼠脚本状态，主控退出重连后无需依赖旧窗口内存。
    QList<DeviceRealtimeControllerTarget> controllerTargets;
    DeviceRealtimeUpdateState update;
};

struct DeviceRealtimeReducedState {
    QString deviceId;
    QString deviceName;
    QString deviceIp;
    QString loginUser;
    DevicePresenceState presence = DevicePresenceState::Unknown;
    int remoteSessionCount = 0;
    QStringList remoteControllerLabels;
    DeviceRealtimeScriptRuntime script;
    RemoteInputScriptRuntimeInfo inputScript; // wjy: 控制端归并器按目标IP保存F9/F10状态，并转交当前和后来打开的远控窗口。
    QList<DeviceRealtimeControllerTarget> controllerTargets; // wjy: 仅保留发布者自身正在控制的目标，供诊断使用，不参与 remoteSessionCount。
    DeviceRealtimeUpdateState update;
    qint64 lastSeenEpochMs = 0;
};

class DeviceRealtimeStateService final : public QObject {
    Q_OBJECT

public:
    using LocalStateProvider = std::function<DeviceRealtimeLocalState()>;

    explicit DeviceRealtimeStateService(QObject* parent = nullptr);
    ~DeviceRealtimeStateService() override;

    bool start(LocalStateProvider provider);
    void stop();
    bool isRunning() const;

    void setConfiguredDeviceIps(const QSet<QString>& deviceIps);
    QString publishControllerTarget(
        const QString& targetIp,
        const QString& targetName,
        const QString& targetDeviceId = QString(),
        const QString& sessionId = QString());
    void removeControllerTarget(const QString& sessionId);
    void notifyLocalStateChanged();
    void applyManualCalibration(const QString& deviceIp, const DeviceStatusInfo& info);

signals:
    void deviceStateChanged(const QString& deviceIp, const platform::DeviceRealtimeReducedState& state);
    void deviceExpired(const QString& deviceIp);

private:
    // =====wjy====
    struct PeerState {
        DeviceRealtimeSnapshot snapshot;
        DeviceRealtimeReducedState reduced;
        DeviceRealtimeReducedState lastEmitted;
        qint64 expiresAtMs = 0;
        bool realtime = false;
        bool expiryQueued = false;
        bool hasEmittedState = false;
    }; // wjy: 每个已配置 IP 独立保存启动身份、序号、TTL 和最后一次 UI 状态，心跳不会重复触发同值通知。

    struct SubscriberLease {
        QString deviceId;
        QString bootId;
        qint64 expiresAtMs = 0;
    }; // wjy: 跨广播域控制端只在租约有效期内接收定向状态，异常退出后目标端自动停止推送。

    struct StateEvent {
        enum class Type {
            Snapshot,
            Expire,
            ManualCalibration,
            Subscription,
        };

        Type type = Type::Snapshot;
        DeviceRealtimeSnapshot snapshot;
        DeviceStatusInfo manualInfo;
        QString subscriberDeviceId;
        QString subscriberBootId;
        QString sourceIp;
        qint64 receivedAtMs = 0;
        qint64 leaseMs = 0;
    }; // wjy: 网络、TTL 和手动校准统一进入同一有界队列，再由对象所属线程串行归并。
    // ===end====

    void refreshLocalState();
    void broadcastStateChange();
    void sendCurrentSnapshot();
    void sendSubscriptions();
    void scheduleHeartbeat();
    bool localStateIsActive() const;

    void receivePendingDatagrams();
    void enqueueEvent(StateEvent event);
    void reducePendingEvents();
    void reduceSnapshot(const DeviceRealtimeSnapshot& snapshot, const QString& sourceIp, qint64 receivedAtMs);
    void reduceExpiry(const QString& sourceIp, qint64 nowMs);
    void reduceManualCalibration(const QString& sourceIp, const DeviceStatusInfo& info, qint64 receivedAtMs);
    void reduceSubscription(const StateEvent& event);
    void scanForExpiredDevices();
    void emitReducedStateIfChanged(const QString& sourceIp, PeerState& peer, bool force = false);

    QByteArray encodeSnapshot(const DeviceRealtimeSnapshot& snapshot) const;
    bool decodeSnapshot(const QByteArray& datagram, const QString& sourceIp, DeviceRealtimeSnapshot* snapshot) const;
    bool decodeSubscription(const QByteArray& datagram, const QString& sourceIp, StateEvent* event) const;
    QString localDeviceId() const;

    LocalStateProvider m_localStateProvider;
    DeviceRealtimeLocalState m_localState;
    QHash<QString, DeviceRealtimeControllerTarget> m_controllerTargets;
    QSet<QString> m_configuredDeviceIps;
    QHash<QString, PeerState> m_peerStates;
    QHash<QString, SubscriberLease> m_subscriberLeases;
    QQueue<StateEvent> m_eventQueue;
    QUdpSocket* m_socket = nullptr;
    QTimer* m_localStateTimer = nullptr;
    QTimer* m_heartbeatTimer = nullptr;
    QTimer* m_expiryTimer = nullptr;
    QTimer* m_subscriptionTimer = nullptr;
    QString m_deviceId;
    QString m_bootId;
    quint64 m_sequence = 0;
    bool m_reduceScheduled = false;
    bool m_running = false;
};
// ===end====

} // namespace platform
