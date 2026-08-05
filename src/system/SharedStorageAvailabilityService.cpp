#include "system/SharedStorageAvailabilityService.h"
#include "system/StartupPerformanceLog.h"

#include <QAbstractSocket>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>

namespace platform {
namespace {

// =====wjy====
constexpr const char* kSharedStorageHost = "192.168.1.100"; // wjy: 当前更新目录和桌面壁纸目录都位于该文件服务器。
constexpr quint16 kSharedStorageSmbPort = 445; // wjy: SMB 服务端口可快速判断目标是否具备继续访问 UNC 的基本条件。
constexpr int kSharedStorageProbeTimeoutMs = 1200; // wjy: 局域网连接超过 1.2 秒即视为本轮不可用，且异步等待不会冻结任何界面。
// ===end====

} // namespace

// =====wjy====
SharedStorageAvailabilityService& SharedStorageAvailabilityService::instance()
{
    static SharedStorageAvailabilityService service;
    return service;
}

SharedStorageAvailabilityService::SharedStorageAvailabilityService(QObject* parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_timeoutTimer(new QTimer(this))
{
    m_socket->setProxy(QNetworkProxy(QNetworkProxy::NoProxy)); // wjy: 192.168.1.100:445 是固定局域网门禁，禁止系统代理或 PAC 状态把互联网故障传导到共享服务器探测。
    m_timeoutTimer->setSingleShot(true); // wjy: 每轮探测只需要一个独立超时，结束后必须由下一次请求重新启动。
    connect(m_socket, &QTcpSocket::connected, this, [this] {
        finishProbe(true); // wjy: TCP 445 建立成功后才允许更新、回撤和壁纸继续访问共享目录。
    });
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        finishProbe(false); // wjy: 拒绝、无路由等错误直接结束探测，不把失败转换成长时间 UNC 等待。
    });
    connect(m_timeoutTimer, &QTimer::timeout, this, [this] {
        finishProbe(false); // wjy: 丢包或服务器离线时由短超时收口，事件循环始终保持可响应。
    });
}

QString SharedStorageAvailabilityService::hostAddress()
{
    return QString::fromLatin1(kSharedStorageHost);
}

quint16 SharedStorageAvailabilityService::smbPort()
{
    return kSharedStorageSmbPort;
}

bool SharedStorageAvailabilityService::isAvailable() const
{
    return m_available.load(std::memory_order_acquire); // wjy: 后台任务读取最近一次完整探测结果，不与主线程写入形成未定义行为。
}

bool SharedStorageAvailabilityService::probeInProgress() const
{
    return m_probeInProgress;
}

void SharedStorageAvailabilityService::requestProbe()
{
    if (m_probeInProgress) {
        return; // wjy: 更新、回撤和壁纸同时请求时共享正在进行的一轮探测，禁止创建重复套接字或线程。
    }

    m_socket->abort(); // wjy: 在打开本轮结果门禁前清理旧状态，旧套接字信号不会被误算成本轮失败。
    m_probeInProgress = true;
    StartupPerformanceLog::checkpoint(QStringLiteral("[startup-share] SMB probe begin 192.168.1.100:445")); // wjy: 启动慢时可直接看到异步服务器探测开始点。
    m_socket->connectToHost(hostAddress(), smbPort()); // wjy: connectToHost 由 Qt 事件循环异步推进，不执行 waitForConnected。
    m_timeoutTimer->start(kSharedStorageProbeTimeoutMs);
}

void SharedStorageAvailabilityService::stop()
{
    m_probeInProgress = false; // wjy: 先关闭结果门禁，abort 产生的状态信号不会被误判成一轮新失败。
    m_timeoutTimer->stop();
    m_socket->abort();
}

void SharedStorageAvailabilityService::finishProbe(bool available)
{
    if (!m_probeInProgress) {
        return; // wjy: 忽略 abort 后到达的旧错误信号，确保每轮请求只发布一次结果。
    }

    m_probeInProgress = false;
    m_timeoutTimer->stop();
    m_socket->abort(); // wjy: 探测只验证端口，不保持 SMB TCP 连接占用系统句柄。

    const bool previousAvailability = m_available.exchange(available, std::memory_order_acq_rel); // wjy: 一次原子交换同时发布新门禁并取得旧值，信号仍只在状态真正变化时发送。
    const bool availabilityChangedNow = previousAvailability != available;
    StartupPerformanceLog::checkpoint(QStringLiteral("[startup-share] SMB probe end available=%1")
        .arg(available)); // wjy: 与 begin 的 step_ms 差值就是本轮 SMB 连接耗时，失败超时也能明确显示。
    if (availabilityChangedNow) {
        emit availabilityChanged(available); // wjy: 状态未变化时不重复触发常驻界面刷新。
    }
    emit probeFinished(available); // wjy: 所有等待本轮结果的功能收到同一结论，再决定是否访问 UNC。
}
// ===end====

} // namespace platform
