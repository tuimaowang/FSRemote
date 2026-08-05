#include "system/DeviceRealtimeStateService.h"

#include "system/DeviceInfoService.h"
#include "system/NetworkInterfacePolicy.h"
#include "system/PortableOpenSshManager.h"
#include "system/WjyDiagnosticLog.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAddressEntry>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <utility>

namespace platform {
namespace {

// =====wjy====
constexpr quint16 kRealtimeStatePort = 49104; // wjy: 49103 已用于 SSH、49105 已用于音频，实时状态固定使用独立 UDP 49104。
constexpr int kProtocolVersion = 1;
constexpr qint64 kActiveTtlMs = 5000;
constexpr qint64 kIdleTtlMs = 15000;
constexpr int kActiveHeartbeatMs = 1000;
constexpr int kIdleHeartbeatMs = 5000;
constexpr int kMaxDatagramBytes = 16 * 1024;
constexpr int kMaxQueuedEvents = 128;
constexpr int kMaxHostSessions = 10;
constexpr int kMaxControllerTargets = 64;
constexpr int kMaxIdentifierLength = 160;
constexpr int kMaxTextLength = 512;
constexpr qint64 kManualCalibrationTtlMs = 15000;
constexpr qint64 kSubscriptionLeaseMs = 30000;
constexpr int kSubscriptionRenewIntervalMs = 10000;
constexpr int kMaxSubscriptionBytes = 2048;
constexpr int kMaxSubscribers = 128;

struct BroadcastEndpoint {
    QHostAddress localAddress;
    QHostAddress broadcastAddress;
    uint interfaceIndex = 0;
    quint32 networkAddress = 0;
    quint32 netmask = 0;
};

QString normalizedIpv4(const QHostAddress& address)
{
    bool ok = false;
    const quint32 ipv4 = address.toIPv4Address(&ok);
    return ok ? QHostAddress(ipv4).toString() : QString(); // wjy: IPv4-mapped 地址统一转成点分十进制，避免字符串形式不同绕过来源校验。
}

QString normalizedIpv4(const QString& text)
{
    return normalizedIpv4(QHostAddress(text.trimmed()));
}

bool boundedText(const QString& text, int maximumLength, bool required = false)
{
    const QString trimmed = text.trimmed();
    return trimmed.size() <= maximumLength && (!required || !trimmed.isEmpty());
}

bool validOptionalSemanticVersion(const QString& text)
{
    const QString normalized = text.trimmed();
    if (normalized.isEmpty()) {
        return true;
    }
    if (normalized.size() > 32) {
        return false;
    }
    const QStringList parts = normalized.split(QLatin1Char('.'));
    if (parts.size() != 3) {
        return false;
    }
    for (const QString& part : parts) {
        bool ok = false;
        const int value = part.toInt(&ok);
        if (!ok || value < 0) {
            return false;
        }
    }
    return true;
}

QString scriptStateName(RealtimeScriptState state)
{
    switch (state) {
    case RealtimeScriptState::Idle: return QStringLiteral("idle");
    case RealtimeScriptState::Running: return QStringLiteral("running");
    case RealtimeScriptState::Unknown:
    default: return QStringLiteral("unknown");
    }
}

bool parseScriptState(const QString& value, RealtimeScriptState* state)
{
    if (!state) {
        return false;
    }
    if (value == QStringLiteral("unknown")) {
        *state = RealtimeScriptState::Unknown;
        return true;
    }
    if (value == QStringLiteral("idle")) {
        *state = RealtimeScriptState::Idle;
        return true;
    }
    if (value == QStringLiteral("running")) {
        *state = RealtimeScriptState::Running;
        return true;
    }
    return false;
}

QJsonObject hostSessionObject(const DeviceRealtimeHostSession& session)
{
    return {
        {QStringLiteral("sessionId"), session.sessionId},
        {QStringLiteral("controllerDeviceId"), session.controllerDeviceId},
        {QStringLiteral("controllerName"), session.controllerName},
        {QStringLiteral("sourceIp"), session.sourceIp},
    };
}

QJsonObject controllerTargetObject(const DeviceRealtimeControllerTarget& target)
{
    return {
        {QStringLiteral("sessionId"), target.sessionId},
        {QStringLiteral("targetDeviceId"), target.targetDeviceId},
        {QStringLiteral("targetName"), target.targetName},
        {QStringLiteral("targetIp"), target.targetIp},
    };
}

QJsonObject scriptObject(const DeviceRealtimeScriptRuntime& script)
{
    return {
        {QStringLiteral("state"), scriptStateName(script.state)},
        {QStringLiteral("runId"), script.runId},
        {QStringLiteral("workName"), script.workName},
        {QStringLiteral("scriptName"), script.scriptName},
        {QStringLiteral("controllerPid"), QString::number(script.controllerPid)},
        {QStringLiteral("startedAtEpochMs"), QString::number(script.startedAtEpochMs)},
    };
}

QJsonObject updateObject(const DeviceRealtimeUpdateState& update)
{
    return {
        {QStringLiteral("installedVersion"), update.installedVersion},
        {QStringLiteral("runtimeRepairRequired"), update.runtimeRepairRequired},
    };
}

bool reducedStateMeaningfullyEqual(const DeviceRealtimeReducedState& left, const DeviceRealtimeReducedState& right)
{
    return left.deviceId == right.deviceId
        && left.deviceName == right.deviceName
        && left.deviceIp == right.deviceIp
        && left.loginUser == right.loginUser
        && left.presence == right.presence
        && left.remoteSessionCount == right.remoteSessionCount
        && left.remoteControllerLabels == right.remoteControllerLabels
        && left.script == right.script
        && left.controllerTargets == right.controllerTargets
        && left.update == right.update; // wjy: 心跳只更新 lastSeen，不让 UI 每秒重复重绘相同状态。
}

bool ipv4Value(const QHostAddress& address, quint32* value)
{
    bool ok = false;
    const quint32 ipv4 = address.toIPv4Address(&ok);
    if (ok && value) {
        *value = ipv4;
    }
    return ok;
}

bool isPrivateIpv4(const QString& ip)
{
    quint32 value = 0;
    if (!ipv4Value(QHostAddress(ip), &value)) {
        return false;
    }
    const quint8 first = static_cast<quint8>((value >> 24) & 0xFF);
    const quint8 second = static_cast<quint8>((value >> 16) & 0xFF);
    return first == 10
        || (first == 172 && second >= 16 && second <= 31)
        || (first == 192 && second == 168); // wjy: 跨网段订阅只允许 RFC1918 私有地址，禁止把状态推送租约扩散到互联网来源。
}

bool belongsToLocalSubnet(const QString& ip, const QList<BroadcastEndpoint>& endpoints)
{
    quint32 target = 0;
    if (!ipv4Value(QHostAddress(ip), &target)) {
        return false;
    }
    for (const BroadcastEndpoint& endpoint : endpoints) {
        if ((target & endpoint.netmask) == endpoint.networkAddress) {
            return true;
        }
    }
    return false;
}

qint64 writeDatagramFromEndpoint(
    QUdpSocket* socket,
    const QByteArray& payload,
    const BroadcastEndpoint& endpoint,
    const QHostAddress& destination)
{
    if (!socket || payload.isEmpty() || endpoint.localAddress.isNull() || destination.isNull()) {
        return -1;
    }

    // =====wjy====
    QNetworkDatagram datagram(payload, destination, kRealtimeStatePort); // wjy: 所有广播、订阅和订阅回包统一构造带目的地址的 UDP 数据报。
    datagram.setSender(endpoint.localAddress, kRealtimeStatePort); // wjy: 强制公布 IP 与 UDP 实际源 IP 一致，避免 VPN/TUN 自动路由选择虚拟地址后被对端校验拒绝。
    datagram.setInterfaceIndex(endpoint.interfaceIndex); // wjy: 固定从对应物理局域网接口发出，跨网段目标经真实网关路由，不被 Clash 等虚拟网卡的更高优先级路由截走。
    return socket->writeDatagram(datagram);
    // ===end====
}

QList<BroadcastEndpoint> eligibleBroadcastEndpoints()
{
    QList<BroadcastEndpoint> endpoints;
    QSet<QString> seen;
    for (const QNetworkInterface& networkInterface : QNetworkInterface::allInterfaces()) {
        const auto flags = networkInterface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || !flags.testFlag(QNetworkInterface::CanBroadcast)
            || flags.testFlag(QNetworkInterface::IsLoopBack)
            || flags.testFlag(QNetworkInterface::IsPointToPoint)
            || isVirtualLanInterface(
                networkInterface.type(),
                networkInterface.name(),
                networkInterface.humanReadableName())) {
            continue; // wjy: 排除回环、点对点/VPN、VMware 与 Hyper-V 虚拟交换网卡，防止虚拟 192.168.1.0/24 抢占真实跨网段订阅。
        }
        for (const QNetworkAddressEntry& entry : networkInterface.addressEntries()) {
            const QString localIp = normalizedIpv4(entry.ip());
            const QString broadcastIp = normalizedIpv4(entry.broadcast());
            quint32 localValue = 0;
            quint32 maskValue = 0;
            if (localIp.isEmpty() || broadcastIp.isEmpty()
                || localIp.startsWith(QStringLiteral("169.254."))
                || localIp == QStringLiteral("0.0.0.0")
                || broadcastIp == QStringLiteral("255.255.255.255")
                || !ipv4Value(entry.ip(), &localValue)
                || !ipv4Value(entry.netmask(), &maskValue)
                || maskValue == 0) {
                continue;
            }
            const QString key = QString::number(networkInterface.index())
                + QLatin1Char('|') + localIp + QLatin1Char('|') + broadcastIp;
            if (seen.contains(key)) {
                continue;
            }
            seen.insert(key);
            BroadcastEndpoint endpoint;
            endpoint.localAddress = QHostAddress(localIp);
            endpoint.broadcastAddress = QHostAddress(broadcastIp);
            endpoint.interfaceIndex = networkInterface.index(); // wjy: 保存接口索引，后续单播订阅也能明确绕过 VPN/TUN 虚拟路由。
            endpoint.netmask = maskValue;
            endpoint.networkAddress = localValue & maskValue;
            endpoints.append(endpoint); // wjy: 同时保存接口网段，用来区分同广播域目标和需要订阅的跨网段目标。
        }
    }
    return endpoints;
}
// ===end====

} // namespace

DeviceRealtimeStateService::DeviceRealtimeStateService(QObject* parent)
    : QObject(parent)
{
    // =====wjy====
    m_localStateTimer = new QTimer(this);
    m_localStateTimer->setInterval(250); // wjy: 仅在本机内存/进程中采样权威状态，不产生网络轮询；变化后才触发立即广播。
    connect(m_localStateTimer, &QTimer::timeout, this, &DeviceRealtimeStateService::refreshLocalState);

    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setSingleShot(true);
    connect(m_heartbeatTimer, &QTimer::timeout, this, [this] {
        refreshLocalState(); // wjy: 心跳发送前再次校验脚本进程和主机会话，静默结束也能随本次完整快照被纠正。
        sendCurrentSnapshot();
        scheduleHeartbeat();
    });

    m_expiryTimer = new QTimer(this);
    m_expiryTimer->setInterval(500);
    connect(m_expiryTimer, &QTimer::timeout, this, &DeviceRealtimeStateService::scanForExpiredDevices);

    m_subscriptionTimer = new QTimer(this);
    m_subscriptionTimer->setInterval(kSubscriptionRenewIntervalMs);
    connect(m_subscriptionTimer, &QTimer::timeout, this, &DeviceRealtimeStateService::sendSubscriptions); // wjy: 订阅续租只发轻量 UDP 控制面消息，不读取状态、不建立 TCP 连接。
    // ===end====
}

DeviceRealtimeStateService::~DeviceRealtimeStateService()
{
    stop();
}

bool DeviceRealtimeStateService::start(LocalStateProvider provider)
{
    if (m_running) {
        return true;
    }

    // =====wjy====
    m_deviceId = localDeviceId();
    if (m_deviceId.isEmpty()) {
        writeWjyDiagnosticLog(QStringLiteral("[wjy-realtime] start failed: local OpenSSH public key fingerprint unavailable"));
        return false; // wjy: 没有稳定公钥身份时不发送临时随机设备 ID，避免每次启动在对端形成另一台设备。
    }
    m_bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_sequence = 0;
    m_localStateProvider = std::move(provider);
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &DeviceRealtimeStateService::receivePendingDatagrams);
    if (!m_socket->bind(QHostAddress::AnyIPv4, kRealtimeStatePort,
            QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        writeWjyDiagnosticLog(QStringLiteral("[wjy-realtime] UDP bind failed port=%1 error=%2")
            .arg(kRealtimeStatePort)
            .arg(m_socket->errorString()));
        delete m_socket;
        m_socket = nullptr;
        m_localStateProvider = {};
        return false;
    }

    m_running = true;
    refreshLocalState();
    m_localStateTimer->start();
    m_expiryTimer->start();
    m_subscriptionTimer->start();
    broadcastStateChange(); // wjy: 启动成功立即发布第一份完整快照，不等待首个心跳周期。
    writeWjyDiagnosticLog(QStringLiteral("[wjy-realtime] started udp=%1 deviceId=%2 bootId=%3")
        .arg(kRealtimeStatePort)
        .arg(m_deviceId, m_bootId));
    return true;
    // ===end====
}

void DeviceRealtimeStateService::stop()
{
    if (!m_running && !m_socket) {
        return;
    }

    // =====wjy====
    m_running = false;
    m_localStateTimer->stop();
    m_heartbeatTimer->stop();
    m_expiryTimer->stop();
    m_subscriptionTimer->stop();
    m_eventQueue.clear();
    m_peerStates.clear();
    m_subscriberLeases.clear();
    m_controllerTargets.clear();
    m_reduceScheduled = false;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->close();
        delete m_socket;
        m_socket = nullptr; // wjy: 在流主机停止前同步释放 49104，防止重启时新实例无法绑定状态端口。
    }
    m_localStateProvider = {};
    writeWjyDiagnosticLog(QStringLiteral("[wjy-realtime] stopped"));
    // ===end====
}

bool DeviceRealtimeStateService::isRunning() const
{
    return m_running;
}

void DeviceRealtimeStateService::setConfiguredDeviceIps(const QSet<QString>& deviceIps)
{
    // =====wjy====
    QSet<QString> normalized;
    for (const QString& deviceIp : deviceIps) {
        const QString ip = normalizedIpv4(deviceIp);
        if (!ip.isEmpty()) {
            normalized.insert(ip);
        }
    }
    m_configuredDeviceIps = std::move(normalized); // wjy: 只有设备列表已有的 IP 才有资格进入归并器，广播不能自行创建新设备。

    for (auto it = m_peerStates.begin(); it != m_peerStates.end();) {
        if (!m_configuredDeviceIps.contains(it.key())) {
            it = m_peerStates.erase(it); // wjy: 删除设备后同步清掉其序号、会话和 TTL，迟到包也会被来源过滤拒绝。
        } else {
            ++it;
        }
    }
    if (m_running) {
        QTimer::singleShot(0, this, &DeviceRealtimeStateService::sendSubscriptions); // wjy: 设备列表加载或变更后立即订阅跨网段目标，不等待下一个 10 秒续租周期。
    }
    // ===end====
}

QString DeviceRealtimeStateService::publishControllerTarget(
    const QString& targetIp,
    const QString& targetName,
    const QString& targetDeviceId,
    const QString& sessionId)
{
    const QString ip = normalizedIpv4(targetIp);
    if (ip.isEmpty()) {
        return {};
    }

    // =====wjy====
    DeviceRealtimeControllerTarget target;
    target.sessionId = sessionId.trimmed().isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : sessionId.trimmed();
    target.targetDeviceId = targetDeviceId.trimmed().left(kMaxIdentifierLength);
    target.targetName = targetName.trimmed().left(kMaxTextLength);
    target.targetIp = ip;
    if (m_controllerTargets.size() >= kMaxControllerTargets
        && !m_controllerTargets.contains(target.sessionId)) {
        return {}; // wjy: 异常窗口风暴也不能让广播快照和本机内存无限增长。
    }
    const bool changed = m_controllerTargets.value(target.sessionId) != target;
    m_controllerTargets.insert(target.sessionId, target);
    if (changed && m_running) {
        broadcastStateChange();
    }
    return target.sessionId;
    // ===end====
}

void DeviceRealtimeStateService::removeControllerTarget(const QString& sessionId)
{
    // =====wjy====
    if (m_controllerTargets.remove(sessionId.trimmed()) > 0 && m_running) {
        broadcastStateChange(); // wjy: 窗口销毁立即发布不再控制该目标的完整快照，丢包时后续心跳仍可自愈。
    }
    // ===end====
}

void DeviceRealtimeStateService::notifyLocalStateChanged()
{
    refreshLocalState();
}

void DeviceRealtimeStateService::applyManualCalibration(const QString& deviceIp, const DeviceStatusInfo& info)
{
    const QString sourceIp = normalizedIpv4(deviceIp);
    if (sourceIp.isEmpty() || !m_configuredDeviceIps.contains(sourceIp)) {
        return;
    }

    // =====wjy====
    StateEvent event;
    event.type = StateEvent::Type::ManualCalibration;
    event.sourceIp = sourceIp;
    event.manualInfo = info;
    event.receivedAtMs = QDateTime::currentMSecsSinceEpoch();
    enqueueEvent(std::move(event)); // wjy: 用户手动 TCP 刷新也经过同一消息队列归并，不从后台线程直接改 UI 缓存。
    // ===end====
}

void DeviceRealtimeStateService::refreshLocalState()
{
    if (!m_running || !m_localStateProvider) {
        return;
    }

    // =====wjy====
    DeviceRealtimeLocalState refreshed = m_localStateProvider();
    if (refreshed.hostSessions.size() > kMaxHostSessions) {
        refreshed.hostSessions = refreshed.hostSessions.mid(0, kMaxHostSessions);
    }
    if (refreshed == m_localState) {
        return;
    }
    m_localState = std::move(refreshed);
    broadcastStateChange(); // wjy: 会话或脚本事实变化立即发包；250ms 采样不是设备网络轮询，也不会每次都广播。
    // ===end====
}

void DeviceRealtimeStateService::broadcastStateChange()
{
    if (!m_running || !m_socket) {
        return;
    }

    // =====wjy====
    sendCurrentSnapshot();
    QTimer::singleShot(100, this, [this] {
        if (m_running) {
            sendCurrentSnapshot(); // wjy: 首次短补发仍发送“当前完整快照”，若中间又变化会自然合并为最新状态。
        }
    });
    QTimer::singleShot(300, this, [this] {
        if (m_running) {
            sendCurrentSnapshot(); // wjy: 第二次短补发提高局域网瞬时丢包下的到达率，不依赖反向结束事件。
        }
    });
    scheduleHeartbeat();
    // ===end====
}

void DeviceRealtimeStateService::sendCurrentSnapshot()
{
    if (!m_running || !m_socket) {
        return;
    }

    // =====wjy====
    const DeviceInfo localInfo = DeviceInfoService::local();
    DeviceRealtimeSnapshot snapshot;
    snapshot.protocolVersion = kProtocolVersion;
    snapshot.deviceId = m_deviceId;
    snapshot.bootId = m_bootId;
    snapshot.sequence = ++m_sequence;
    snapshot.ttlMs = localStateIsActive() ? kActiveTtlMs : kIdleTtlMs;
    snapshot.deviceName = localInfo.name.trimmed().left(kMaxTextLength);
    snapshot.loginUser = PortableOpenSshManager::instance().loginUser().trimmed().left(kMaxTextLength);
    snapshot.hostSessions = m_localState.hostSessions;
    snapshot.script = m_localState.script;
    snapshot.controllerTargets = m_controllerTargets.values();
    snapshot.update = m_localState.update;
    std::sort(snapshot.controllerTargets.begin(), snapshot.controllerTargets.end(), [](const auto& left, const auto& right) {
        return left.sessionId < right.sessionId;
    });

    const auto endpoints = eligibleBroadcastEndpoints();
    for (const BroadcastEndpoint& endpoint : endpoints) {
        snapshot.deviceIp = normalizedIpv4(endpoint.localAddress); // wjy: 多网卡时每个广播报文公布实际接口 IP，使接收端来源 IP 校验保持严格相等。
        const QByteArray datagram = encodeSnapshot(snapshot);
        if (datagram.isEmpty() || datagram.size() > kMaxDatagramBytes) {
            continue;
        }
        const qint64 written = writeDatagramFromEndpoint(m_socket, datagram, endpoint, endpoint.broadcastAddress);
        if (written != datagram.size()) {
            writeWjyDiagnosticLog(QStringLiteral("[wjy-realtime] send failed local=%1 broadcast=%2 error=%3")
                .arg(snapshot.deviceIp, endpoint.broadcastAddress.toString(), m_socket->errorString()));
        }
    }

    QString primaryIp = normalizedIpv4(localInfo.ip);
    const BroadcastEndpoint* primaryEndpoint = nullptr;
    for (const BroadcastEndpoint& endpoint : endpoints) {
        if (normalizedIpv4(endpoint.localAddress) == primaryIp) {
            primaryEndpoint = &endpoint;
            break;
        }
    }
    if (!primaryEndpoint && !endpoints.isEmpty()) {
        primaryEndpoint = &endpoints.first();
        primaryIp = normalizedIpv4(primaryEndpoint->localAddress); // wjy: 主信息 IP 不属于可广播接口时，回退到真实局域网接口并同步修正报文公布 IP。
    }
    if (primaryEndpoint && !primaryIp.isEmpty()) {
        snapshot.deviceIp = primaryIp;
        const QByteArray directedDatagram = encodeSnapshot(snapshot);
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_subscriberLeases.begin(); it != m_subscriberLeases.end();) {
            if (it->expiresAtMs <= nowMs) {
                it = m_subscriberLeases.erase(it); // wjy: 控制端停止续租后不再占用目标端地址表，也不继续产生跨网段流量。
                continue;
            }
            const QString subscriberIp = it.key();
            const qint64 written = directedDatagram.isEmpty()
                ? -1
                : writeDatagramFromEndpoint(m_socket, directedDatagram, *primaryEndpoint, QHostAddress(subscriberIp));
            if (written != directedDatagram.size()) {
                writeWjyDiagnosticLog(QStringLiteral("[wjy-realtime] subscriber send failed local=%1 target=%2 error=%3")
                    .arg(primaryIp, subscriberIp, m_socket->errorString()));
            }
            ++it; // wjy: 状态变化、补发和心跳共用此路径，跨网段订阅者收到与广播域内设备相同的完整快照。
        }
    }
    // ===end====
}

void DeviceRealtimeStateService::sendSubscriptions()
{
    if (!m_running || !m_socket || m_configuredDeviceIps.isEmpty()) {
        return;
    }

    // =====wjy====
    const DeviceInfo localInfo = DeviceInfoService::local();
    const QList<BroadcastEndpoint> endpoints = eligibleBroadcastEndpoints();
    QString primaryIp = normalizedIpv4(localInfo.ip);
    const BroadcastEndpoint* primaryEndpoint = nullptr;
    for (const BroadcastEndpoint& endpoint : endpoints) {
        if (normalizedIpv4(endpoint.localAddress) == primaryIp) {
            primaryEndpoint = &endpoint;
            break;
        }
    }
    if (!primaryEndpoint && !endpoints.isEmpty()) {
        primaryEndpoint = &endpoints.first();
        primaryIp = normalizedIpv4(primaryEndpoint->localAddress); // wjy: 订阅公布地址必须来自实际可用接口，不能使用无法固定出口的缓存 IP。
    }
    if (!primaryEndpoint || primaryIp.isEmpty() || !isPrivateIpv4(primaryIp)) {
        return;
    }

    const QJsonObject subscription {
        {QStringLiteral("protocolVersion"), kProtocolVersion},
        {QStringLiteral("messageType"), QStringLiteral("subscribe")},
        {QStringLiteral("deviceId"), m_deviceId},
        {QStringLiteral("bootId"), m_bootId},
        {QStringLiteral("deviceIp"), primaryIp},
        {QStringLiteral("leaseMs"), kSubscriptionLeaseMs},
    };
    const QByteArray datagram = QJsonDocument(subscription).toJson(QJsonDocument::Compact);
    if (datagram.isEmpty() || datagram.size() > kMaxSubscriptionBytes) {
        return;
    }

    for (const QString& configuredIp : std::as_const(m_configuredDeviceIps)) {
        if (configuredIp == primaryIp || belongsToLocalSubnet(configuredIp, endpoints)) {
            continue; // wjy: 同广播域已有一次定向广播覆盖，只对其它可路由网段发送订阅，避免无意义的逐设备单播。
        }
        const qint64 written = writeDatagramFromEndpoint(m_socket, datagram, *primaryEndpoint, QHostAddress(configuredIp));
        if (written != datagram.size()) {
            writeWjyDiagnosticLog(QStringLiteral("[wjy-realtime] subscribe send failed local=%1 target=%2 error=%3")
                .arg(primaryIp, configuredIp, m_socket->errorString()));
        }
    }
    // ===end====
}

void DeviceRealtimeStateService::scheduleHeartbeat()
{
    if (!m_running) {
        return;
    }

    // =====wjy====
    const bool active = localStateIsActive();
    const int baseInterval = active ? kActiveHeartbeatMs : kIdleHeartbeatMs;
    const int jitterRange = active ? 100 : 500;
    const int jitter = QRandomGenerator::global()->bounded(jitterRange * 2 + 1) - jitterRange;
    m_heartbeatTimer->start(qMax(250, baseInterval + jitter)); // wjy: 心跳只负责崩溃/断电存活检测，业务变化不等它到时才同步。
    // ===end====
}

bool DeviceRealtimeStateService::localStateIsActive() const
{
    return !m_localState.hostSessions.isEmpty()
        || m_localState.script.state == RealtimeScriptState::Running
        || !m_controllerTargets.isEmpty();
}

void DeviceRealtimeStateService::receivePendingDatagrams()
{
    if (!m_socket) {
        return;
    }

    while (m_socket->hasPendingDatagrams()) {
        const qint64 pendingSize = m_socket->pendingDatagramSize();
        QNetworkDatagram networkDatagram = m_socket->receiveDatagram(kMaxDatagramBytes + 1);
        const QString sourceIp = normalizedIpv4(networkDatagram.senderAddress());
        if (pendingSize <= 0 || pendingSize > kMaxDatagramBytes || sourceIp.isEmpty()) {
            continue;
        }

        StateEvent subscriptionEvent;
        if (pendingSize <= kMaxSubscriptionBytes
            && decodeSubscription(networkDatagram.data(), sourceIp, &subscriptionEvent)) {
            enqueueEvent(std::move(subscriptionEvent)); // wjy: 订阅只更新有界的出站接收者租约，不创建或修改 DeviceGrid 状态。
            continue;
        }

        if (!m_configuredDeviceIps.contains(sourceIp)) {
            continue; // wjy: 只有状态快照仍要求来源 IP 已存在于设备列表；未知来源不能创建 UI 设备。
        }

        DeviceRealtimeSnapshot snapshot;
        if (!decodeSnapshot(networkDatagram.data(), sourceIp, &snapshot)) {
            continue;
        }

        StateEvent event;
        event.type = StateEvent::Type::Snapshot;
        event.snapshot = std::move(snapshot);
        event.sourceIp = sourceIp;
        event.receivedAtMs = QDateTime::currentMSecsSinceEpoch();
        enqueueEvent(std::move(event));
    }
}

void DeviceRealtimeStateService::enqueueEvent(StateEvent event)
{
    // =====wjy====
    if (event.type == StateEvent::Type::Snapshot) {
        for (int i = m_eventQueue.size() - 1; i >= 0; --i) {
            StateEvent& queued = m_eventQueue[i];
            if (queued.type != StateEvent::Type::Snapshot
                || queued.sourceIp != event.sourceIp
                || queued.snapshot.deviceId != event.snapshot.deviceId
                || queued.snapshot.bootId != event.snapshot.bootId) {
                continue;
            }
            if (event.snapshot.sequence > queued.snapshot.sequence) {
                queued = std::move(event); // wjy: 同设备同进程只保留尚未归并的最大序号快照，降低广播突发时 UI 压力。
            }
            return;
        }
    }
    if (event.type == StateEvent::Type::Subscription) {
        for (int i = m_eventQueue.size() - 1; i >= 0; --i) {
            StateEvent& queued = m_eventQueue[i];
            if (queued.type == StateEvent::Type::Subscription && queued.sourceIp == event.sourceIp) {
                queued = std::move(event); // wjy: 同一控制端连续续租只保留最后一条，防止订阅报文挤占状态事件队列。
                return;
            }
        }
    }

    while (m_eventQueue.size() >= kMaxQueuedEvents) {
        m_eventQueue.dequeue(); // wjy: 固定容量优先保证进程内存有界；完整心跳会再次送达最新状态。
    }
    m_eventQueue.enqueue(std::move(event));
    if (!m_reduceScheduled) {
        m_reduceScheduled = true;
        QTimer::singleShot(0, this, &DeviceRealtimeStateService::reducePendingEvents); // wjy: 所有来源统一在对象所属线程串行修改状态表。
    }
    // ===end====
}

void DeviceRealtimeStateService::reducePendingEvents()
{
    m_reduceScheduled = false;
    while (!m_eventQueue.isEmpty()) {
        StateEvent event = m_eventQueue.dequeue();
        switch (event.type) {
        case StateEvent::Type::Snapshot:
            reduceSnapshot(event.snapshot, event.sourceIp, event.receivedAtMs);
            break;
        case StateEvent::Type::Expire:
            reduceExpiry(event.sourceIp, event.receivedAtMs);
            break;
        case StateEvent::Type::ManualCalibration:
            reduceManualCalibration(event.sourceIp, event.manualInfo, event.receivedAtMs);
            break;
        case StateEvent::Type::Subscription:
            reduceSubscription(event);
            break;
        }
    }
}

void DeviceRealtimeStateService::reduceSnapshot(
    const DeviceRealtimeSnapshot& snapshot,
    const QString& sourceIp,
    qint64 receivedAtMs)
{
    if (!m_configuredDeviceIps.contains(sourceIp)) {
        return;
    }

    // =====wjy====
    PeerState& peer = m_peerStates[sourceIp];
    if (peer.realtime
        && peer.snapshot.deviceId == snapshot.deviceId
        && peer.snapshot.bootId == snapshot.bootId
        && snapshot.sequence <= peer.snapshot.sequence) {
        return; // wjy: 相同启动实例只接受严格递增序号，补发、乱序和延迟旧包不会回滚 UI。
    }

    const bool newIdentityOrBoot = !peer.realtime
        || peer.snapshot.deviceId != snapshot.deviceId
        || peer.snapshot.bootId != snapshot.bootId;
    if (newIdentityOrBoot) {
        peer = PeerState(); // wjy: 同 IP 更换设备或设备进程重启时先清空旧会话、脚本和控制目标，再应用新快照。
    }

    peer.realtime = true;
    peer.snapshot = snapshot;
    peer.expiresAtMs = receivedAtMs + snapshot.ttlMs;
    peer.expiryQueued = false;
    peer.reduced.deviceId = snapshot.deviceId;
    peer.reduced.deviceName = snapshot.deviceName;
    peer.reduced.deviceIp = sourceIp;
    peer.reduced.loginUser = snapshot.loginUser;
    peer.reduced.remoteSessionCount = snapshot.hostSessions.size(); // wjy: 人数只来自目标主机公布的唯一会话列表，控制端 target 租约永不相加。
    peer.reduced.remoteControllerLabels.clear();
    for (const DeviceRealtimeHostSession& session : snapshot.hostSessions) {
        QString label = session.controllerName.trimmed();
        if (label.isEmpty()) {
            label = QString::fromUtf8("未知设备");
        }
        if (!session.sourceIp.trimmed().isEmpty()) {
            label += QString::fromUtf8("  IP：%1").arg(session.sourceIp.trimmed());
        }
        peer.reduced.remoteControllerLabels.append(label);
    }
    peer.reduced.presence = peer.reduced.remoteSessionCount > 0
        ? DevicePresenceState::Busy
        : DevicePresenceState::Online;
    peer.reduced.script = snapshot.script;
    peer.reduced.controllerTargets = snapshot.controllerTargets;
    peer.reduced.update = snapshot.update;
    peer.reduced.lastSeenEpochMs = receivedAtMs;
    emitReducedStateIfChanged(sourceIp, peer, newIdentityOrBoot);
    // ===end====
}

void DeviceRealtimeStateService::reduceExpiry(const QString& sourceIp, qint64 nowMs)
{
    auto it = m_peerStates.find(sourceIp);
    if (it == m_peerStates.end()) {
        return;
    }
    PeerState& peer = it.value();
    peer.expiryQueued = false;
    if (peer.expiresAtMs <= 0 || nowMs < peer.expiresAtMs) {
        return; // wjy: 到期事件排队后若新心跳已续租，旧到期事件不能把新状态清掉。
    }

    // =====wjy====
    peer.reduced.presence = DevicePresenceState::Offline;
    peer.reduced.remoteSessionCount = 0;
    peer.reduced.remoteControllerLabels.clear();
    peer.reduced.script = DeviceRealtimeScriptRuntime(); // wjy: 离线只能得出 Unknown，不能把无法验证的脚本误写成 Idle。
    peer.reduced.controllerTargets.clear();
    peer.reduced.update = DeviceRealtimeUpdateState();
    peer.reduced.lastSeenEpochMs = nowMs;
    peer.realtime = false;
    peer.expiresAtMs = 0;
    emitReducedStateIfChanged(sourceIp, peer, true);
    emit deviceExpired(sourceIp);
    // ===end====
}

void DeviceRealtimeStateService::reduceManualCalibration(
    const QString& sourceIp,
    const DeviceStatusInfo& info,
    qint64 receivedAtMs)
{
    if (!m_configuredDeviceIps.contains(sourceIp)) {
        return;
    }

    PeerState& peer = m_peerStates[sourceIp];
    if (peer.realtime && peer.expiresAtMs > receivedAtMs) {
        return; // wjy: 新鲜实时快照优先于一次 TCP 结果，避免瞬时 TCP 超时覆盖仍在续租的广播状态。
    }

    // =====wjy====
    peer = PeerState();
    peer.snapshot.deviceIp = sourceIp;
    peer.reduced.deviceIp = sourceIp;
    peer.reduced.deviceName = info.deviceName.trimmed();
    peer.reduced.loginUser = info.terminalUser.trimmed();
    peer.reduced.presence = info.state;
    peer.reduced.remoteSessionCount = qBound(0, info.remoteSessionCount, kMaxHostSessions);
    for (const QString& label : info.remoteControllerNames.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        peer.reduced.remoteControllerLabels.append(label.trimmed());
    }
    if (info.scriptRuntime.supported && info.scriptRuntime.statusKnown) {
        peer.reduced.script.state = info.scriptRuntime.running
            ? RealtimeScriptState::Running
            : RealtimeScriptState::Idle;
        peer.reduced.script.runId = info.scriptRuntime.runId;
        peer.reduced.script.workName = info.scriptRuntime.workName;
        peer.reduced.script.scriptName = info.scriptRuntime.scriptName;
        peer.reduced.script.controllerPid = info.scriptRuntime.controllerPid;
        peer.reduced.script.startedAtEpochMs = info.scriptRuntime.startedAtEpochMs;
    }
    peer.reduced.lastSeenEpochMs = receivedAtMs;
    peer.expiresAtMs = receivedAtMs + kManualCalibrationTtlMs; // wjy: 手动校准是短期诊断租约，不让一次旧版 TCP 在线结果永久残留。
    emitReducedStateIfChanged(sourceIp, peer, true);
    // ===end====
}

void DeviceRealtimeStateService::reduceSubscription(const StateEvent& event)
{
    if (!isPrivateIpv4(event.sourceIp)
        || event.leaseMs < kSubscriptionRenewIntervalMs
        || event.leaseMs > 60000) {
        return;
    }

    // =====wjy====
    auto it = m_subscriberLeases.find(event.sourceIp);
    const bool newSubscriberProcess = it == m_subscriberLeases.end()
        || it->deviceId != event.subscriberDeviceId
        || it->bootId != event.subscriberBootId
        || it->expiresAtMs <= event.receivedAtMs;
    if (it == m_subscriberLeases.end() && m_subscriberLeases.size() >= kMaxSubscribers) {
        return; // wjy: 陌生订阅来源最多占用 128 个租约槽位，禁止恶意数据报无限增长内存和推送扇出。
    }

    SubscriberLease lease;
    lease.deviceId = event.subscriberDeviceId;
    lease.bootId = event.subscriberBootId;
    lease.expiresAtMs = event.receivedAtMs + event.leaseMs;
    m_subscriberLeases.insert(event.sourceIp, std::move(lease));
    if (newSubscriberProcess) {
        sendCurrentSnapshot(); // wjy: 控制端首次订阅或重启后立即收到当前完整状态，无需等待 1/5 秒心跳。
    }
    // ===end====
}

void DeviceRealtimeStateService::scanForExpiredDevices()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_subscriberLeases.begin(); it != m_subscriberLeases.end();) {
        if (it->expiresAtMs <= nowMs) {
            it = m_subscriberLeases.erase(it); // wjy: 订阅者异常退出后最多保留一个租约周期，之后目标端不再向该 IP 发送状态。
        } else {
            ++it;
        }
    }
    for (auto it = m_peerStates.begin(); it != m_peerStates.end(); ++it) {
        PeerState& peer = it.value();
        if (peer.expiresAtMs <= 0 || nowMs < peer.expiresAtMs || peer.expiryQueued) {
            continue;
        }
        peer.expiryQueued = true;
        StateEvent event;
        event.type = StateEvent::Type::Expire;
        event.sourceIp = it.key();
        event.receivedAtMs = nowMs;
        enqueueEvent(std::move(event));
    }
}

void DeviceRealtimeStateService::emitReducedStateIfChanged(
    const QString& sourceIp,
    PeerState& peer,
    bool force)
{
    if (!force && peer.hasEmittedState && reducedStateMeaningfullyEqual(peer.lastEmitted, peer.reduced)) {
        return;
    }
    peer.hasEmittedState = true;
    peer.lastEmitted = peer.reduced;
    emit deviceStateChanged(sourceIp, peer.reduced); // wjy: UI 只消费归并后的最终状态，不接触网络包、TTL 或队列内部事件。
}

QByteArray DeviceRealtimeStateService::encodeSnapshot(const DeviceRealtimeSnapshot& snapshot) const
{
    QJsonArray sessions;
    for (const DeviceRealtimeHostSession& session : snapshot.hostSessions) {
        sessions.append(hostSessionObject(session));
    }
    QJsonArray targets;
    for (const DeviceRealtimeControllerTarget& target : snapshot.controllerTargets) {
        targets.append(controllerTargetObject(target));
    }

    // =====wjy====
    const QJsonObject root {
        {QStringLiteral("protocolVersion"), snapshot.protocolVersion},
        {QStringLiteral("messageType"), QStringLiteral("snapshot")},
        {QStringLiteral("deviceId"), snapshot.deviceId},
        {QStringLiteral("bootId"), snapshot.bootId},
        {QStringLiteral("sequence"), QString::number(snapshot.sequence)}, // wjy: 序号用十进制字符串传输，避免 JSON double 在长时间运行后丢失整数精度。
        {QStringLiteral("ttlMs"), snapshot.ttlMs},
        {QStringLiteral("device"), QJsonObject {
            {QStringLiteral("name"), snapshot.deviceName},
            {QStringLiteral("ip"), snapshot.deviceIp},
            {QStringLiteral("loginUser"), snapshot.loginUser},
        }},
        {QStringLiteral("host"), QJsonObject {{QStringLiteral("sessions"), sessions}}},
        {QStringLiteral("script"), scriptObject(snapshot.script)},
        {QStringLiteral("controller"), QJsonObject {{QStringLiteral("targets"), targets}}},
        {QStringLiteral("update"), updateObject(snapshot.update)},
    };
    const QByteArray encoded = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return encoded.size() <= kMaxDatagramBytes ? encoded : QByteArray();
    // ===end====
}

bool DeviceRealtimeStateService::decodeSubscription(
    const QByteArray& datagram,
    const QString& sourceIp,
    StateEvent* event) const
{
    if (!event || datagram.isEmpty() || datagram.size() > kMaxSubscriptionBytes || !isPrivateIpv4(sourceIp)) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(datagram, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }

    // =====wjy====
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("protocolVersion")).toInt(-1) != kProtocolVersion
        || root.value(QStringLiteral("messageType")).toString() != QStringLiteral("subscribe")) {
        return false;
    }
    const QString deviceId = root.value(QStringLiteral("deviceId")).toString().trimmed();
    const QString bootId = root.value(QStringLiteral("bootId")).toString().trimmed();
    const QString advertisedIp = normalizedIpv4(root.value(QStringLiteral("deviceIp")).toString());
    const qint64 leaseMs = root.value(QStringLiteral("leaseMs")).toVariant().toLongLong();
    if (advertisedIp != sourceIp
        || !QRegularExpression(QStringLiteral("^SHA256:[A-Za-z0-9+/]{43}$")).match(deviceId).hasMatch()
        || !QRegularExpression(QStringLiteral("^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$")).match(bootId).hasMatch()
        || QUuid(bootId).isNull()
        || leaseMs < kSubscriptionRenewIntervalMs
        || leaseMs > 60000) {
        return false;
    }

    event->type = StateEvent::Type::Subscription;
    event->subscriberDeviceId = deviceId;
    event->subscriberBootId = bootId;
    event->sourceIp = sourceIp;
    event->receivedAtMs = QDateTime::currentMSecsSinceEpoch();
    event->leaseMs = leaseMs;
    return true;
    // ===end====
}

bool DeviceRealtimeStateService::decodeSnapshot(
    const QByteArray& datagram,
    const QString& sourceIp,
    DeviceRealtimeSnapshot* snapshot) const
{
    if (!snapshot || datagram.isEmpty() || datagram.size() > kMaxDatagramBytes) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(datagram, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }

    // =====wjy====
    const QJsonObject root = document.object();
    DeviceRealtimeSnapshot parsed;
    parsed.protocolVersion = root.value(QStringLiteral("protocolVersion")).toInt(-1);
    parsed.deviceId = root.value(QStringLiteral("deviceId")).toString().trimmed();
    parsed.bootId = root.value(QStringLiteral("bootId")).toString().trimmed();
    bool sequenceOk = false;
    parsed.sequence = root.value(QStringLiteral("sequence")).toString().toULongLong(&sequenceOk);
    parsed.ttlMs = root.value(QStringLiteral("ttlMs")).toVariant().toLongLong();
    if (parsed.protocolVersion != kProtocolVersion
        || !sequenceOk || parsed.sequence == 0
        || parsed.ttlMs < 1000 || parsed.ttlMs > 30000
        || !QRegularExpression(QStringLiteral("^SHA256:[A-Za-z0-9+/]{43}$")).match(parsed.deviceId).hasMatch()
        || !boundedText(parsed.deviceId, kMaxIdentifierLength, true)
        || !boundedText(parsed.bootId, kMaxIdentifierLength, true)
        || !QRegularExpression(QStringLiteral("^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$")).match(parsed.bootId).hasMatch()
        || QUuid(parsed.bootId).isNull()) {
        return false;
    }

    const QJsonObject device = root.value(QStringLiteral("device")).toObject();
    parsed.deviceName = device.value(QStringLiteral("name")).toString().trimmed();
    parsed.deviceIp = normalizedIpv4(device.value(QStringLiteral("ip")).toString());
    parsed.loginUser = device.value(QStringLiteral("loginUser")).toString().trimmed();
    if (parsed.deviceIp != sourceIp
        || !boundedText(parsed.deviceName, kMaxTextLength)
        || !boundedText(parsed.loginUser, kMaxTextLength)) {
        return false; // wjy: 报文公布 IP 必须与 UDP 来源严格一致，且只接受有界显示文本。
    }

    const QJsonValue sessionsValue = root.value(QStringLiteral("host")).toObject().value(QStringLiteral("sessions"));
    if (!sessionsValue.isArray() || sessionsValue.toArray().size() > kMaxHostSessions) {
        return false;
    }
    QSet<QString> sessionIds;
    for (const QJsonValue& value : sessionsValue.toArray()) {
        if (!value.isObject()) {
            return false;
        }
        const QJsonObject object = value.toObject();
        DeviceRealtimeHostSession session;
        session.sessionId = object.value(QStringLiteral("sessionId")).toString().trimmed();
        session.controllerDeviceId = object.value(QStringLiteral("controllerDeviceId")).toString().trimmed();
        session.controllerName = object.value(QStringLiteral("controllerName")).toString().trimmed();
        const QString rawSourceIp = object.value(QStringLiteral("sourceIp")).toString().trimmed();
        session.sourceIp = normalizedIpv4(rawSourceIp);
        if (!boundedText(session.sessionId, kMaxIdentifierLength, true)
            || sessionIds.contains(session.sessionId)
            || !boundedText(session.controllerDeviceId, kMaxIdentifierLength)
            || !boundedText(session.controllerName, kMaxTextLength)
            || (!rawSourceIp.isEmpty() && session.sourceIp.isEmpty())) {
            return false;
        }
        sessionIds.insert(session.sessionId);
        parsed.hostSessions.append(std::move(session));
    }

    const QJsonObject script = root.value(QStringLiteral("script")).toObject();
    if (!parseScriptState(script.value(QStringLiteral("state")).toString(), &parsed.script.state)) {
        return false;
    }
    parsed.script.runId = script.value(QStringLiteral("runId")).toString().trimmed();
    parsed.script.workName = script.value(QStringLiteral("workName")).toString().trimmed();
    parsed.script.scriptName = script.value(QStringLiteral("scriptName")).toString().trimmed();
    bool pidOk = false;
    bool startOk = false;
    parsed.script.controllerPid = script.value(QStringLiteral("controllerPid")).toString().toLongLong(&pidOk);
    parsed.script.startedAtEpochMs = script.value(QStringLiteral("startedAtEpochMs")).toString().toLongLong(&startOk);
    if (!pidOk || !startOk
        || parsed.script.controllerPid < 0 || parsed.script.startedAtEpochMs < 0
        || !boundedText(parsed.script.runId, kMaxIdentifierLength)
        || !boundedText(parsed.script.workName, kMaxTextLength)
        || !boundedText(parsed.script.scriptName, kMaxTextLength)
        || (parsed.script.state == RealtimeScriptState::Running
            && (parsed.script.workName.isEmpty() || parsed.script.controllerPid <= 0))) {
        return false;
    }

    const QJsonValue targetsValue = root.value(QStringLiteral("controller")).toObject().value(QStringLiteral("targets"));
    if (!targetsValue.isArray() || targetsValue.toArray().size() > kMaxControllerTargets) {
        return false;
    }
    QSet<QString> targetSessionIds;
    for (const QJsonValue& value : targetsValue.toArray()) {
        if (!value.isObject()) {
            return false;
        }
        const QJsonObject object = value.toObject();
        DeviceRealtimeControllerTarget target;
        target.sessionId = object.value(QStringLiteral("sessionId")).toString().trimmed();
        target.targetDeviceId = object.value(QStringLiteral("targetDeviceId")).toString().trimmed();
        target.targetName = object.value(QStringLiteral("targetName")).toString().trimmed();
        target.targetIp = normalizedIpv4(object.value(QStringLiteral("targetIp")).toString());
        if (!boundedText(target.sessionId, kMaxIdentifierLength, true)
            || targetSessionIds.contains(target.sessionId)
            || !boundedText(target.targetDeviceId, kMaxIdentifierLength)
            || !boundedText(target.targetName, kMaxTextLength)
            || target.targetIp.isEmpty()) {
            return false;
        }
        targetSessionIds.insert(target.sessionId);
        parsed.controllerTargets.append(std::move(target));
    }

    const QJsonValue updateValue = root.value(QStringLiteral("update"));
    if (!updateValue.isUndefined() && !updateValue.isObject()) {
        return false;
    }
    if (updateValue.isObject()) {
        const QJsonObject update = updateValue.toObject();
        parsed.update.installedVersion = update.value(QStringLiteral("installedVersion")).toString().trimmed();
        const QJsonValue repairValue = update.value(QStringLiteral("runtimeRepairRequired"));
        if ((!repairValue.isUndefined() && !repairValue.isBool())
            || !validOptionalSemanticVersion(parsed.update.installedVersion)) {
            return false;
        }
        parsed.update.runtimeRepairRequired = repairValue.toBool(false); // wjy: 旧客户端附带的 knownReleaseVersion 作为未知 JSON 字段自然忽略，不再触发或影响共享版本检查。
    }

    *snapshot = std::move(parsed);
    return true;
    // ===end====
}

QString DeviceRealtimeStateService::localDeviceId() const
{
    QString errorMessage;
    const QString publicKey = PortableOpenSshManager::instance().clientPublicKey(&errorMessage).trimmed();
    if (publicKey.isEmpty()) {
        writeWjyDiagnosticLog(QStringLiteral("[wjy-realtime] public key unavailable: %1").arg(errorMessage));
        return {};
    }

    // =====wjy====
    const QStringList parts = publicKey.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        return {};
    }
    const QByteArray keyBlob = QByteArray::fromBase64(parts.at(1).toLatin1());
    if (keyBlob.isEmpty()) {
        return {};
    }
    const QByteArray digest = QCryptographicHash::hash(keyBlob, QCryptographicHash::Sha256);
    return QStringLiteral("SHA256:%1")
        .arg(QString::fromLatin1(digest.toBase64(QByteArray::OmitTrailingEquals))); // wjy: 对 OpenSSH 公钥二进制主体做 SHA-256，跨程序重启保持设备身份稳定。
    // ===end====
}

} // namespace platform
