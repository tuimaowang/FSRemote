#include "ui/RemoteViewerLifecycleManager.h"

#include "system/WjyDiagnosticLog.h"
#include "ui/RemoteDesktopWindow.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace ui {

// =====wjy====
RemoteViewerLifecycleManager::RemoteViewerLifecycleManager(
    int maximumConcurrentViewerStarts,
    int lifecycleWorkerCount,
    QObject* parent)
    : QObject(parent)
    , m_maximumConcurrentViewerStarts(std::max(1, maximumConcurrentViewerStarts)) // wjy: 即使配置异常也至少保留1个初始化名额，避免全部窗口永久停在等待状态。
{
    const int workerCount = std::max(1, lifecycleWorkerCount); // wjy: 固定工作线程数有硬下限且不随窗口数量增长，20路关闭不会瞬间创建20个线程。
    m_lifecycleWorkers.reserve(static_cast<std::size_t>(workerCount));
    for (int workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        m_lifecycleWorkers.emplace_back([this] {
            lifecycleWorkerLoop(); // wjy: 每个线程只消费管理器队列，析构和应用退出都能确定性join。
        });
    }
}

RemoteViewerLifecycleManager::~RemoteViewerLifecycleManager()
{
    beginApplicationShutdown(); // wjy: 防御性禁止析构期间又从窗口回调申请新的Viewer初始化。
    shutdownAndWait(); // wjy: 即使上层漏调退出准备，管理器自身也绝不遗留可访问已释放Qt对象的工作线程。
}

bool RemoteViewerLifecycleManager::requestViewerStart(RemoteDesktopWindow* window)
{
    if (!window || !m_acceptingViewerStarts || m_lifecycleWorkersJoined) {
        return false; // wjy: 应用退出后拒绝新连接，但现有连接的stop仍由第二阶段清理。
    }
    if (m_activeViewerStarts.contains(window)) {
        return true; // wjy: 同一窗口已经占有初始化名额时不重复计数。
    }
    for (const QPointer<RemoteDesktopWindow>& waitingWindow : std::as_const(m_waitingViewerStarts)) {
        if (waitingWindow.data() == window) {
            return true; // wjy: 同一窗口已经排队时不重复插入，避免一次重连消耗多个后续名额。
        }
    }

    if (m_activeViewerStarts.size() < m_maximumConcurrentViewerStarts) {
        m_activeViewerStarts.insert(window); // wjy: 先登记再回调窗口，窗口同步失败并释放时计数仍然一致。
        window->startViewerConnectionWithAdmission(); // wjy: 原生Viewer创建仍在Qt线程触发，但最多只有4个会话同时处于昂贵初始化阶段。
    } else {
        m_waitingViewerStarts.enqueue(QPointer<RemoteDesktopWindow>(window)); // wjy: 超出并发上限只延后初始化，不关闭、不拒绝窗口，也不限制最终在线总数。
    }
    return true;
}

void RemoteViewerLifecycleManager::completeViewerStart(RemoteDesktopWindow* window)
{
    if (!window) {
        return;
    }
    m_activeViewerStarts.remove(window); // wjy: 成功、失败和停止都统一释放一次名额，QSet让重复状态回调保持幂等。
    grantWaitingViewerStarts(); // wjy: 当前窗口不再占用初始化预算后立即补位下一台设备。
}

void RemoteViewerLifecycleManager::cancelViewerStart(RemoteDesktopWindow* window, bool keepActiveUntilStop)
{
    if (!window) {
        return;
    }

    for (int index = m_waitingViewerStarts.size() - 1; index >= 0; --index) {
        if (!m_waitingViewerStarts.at(index) || m_waitingViewerStarts.at(index).data() == window) {
            m_waitingViewerStarts.removeAt(index); // wjy: 窗口关闭时从等待队列精确移除，同时顺手清理已销毁窗口留下的空QPointer。
        }
    }
    if (!keepActiveUntilStop) {
        m_activeViewerStarts.remove(window); // wjy: 尚未创建句柄或初始化已结束时可立即释放；仍在原生stop中的窗口由完成回调再释放。
        grantWaitingViewerStarts();
    }
}

bool RemoteViewerLifecycleManager::submitLifecycleTask(
    std::function<void()> task,
    LifecycleTaskPriority priority)
{
    if (!task) {
        return false;
    }

    {
        std::lock_guard lock(m_lifecycleMutex);
        if (!m_acceptingLifecycleTasks || m_lifecycleWorkersJoined) {
            return false; // wjy: join阶段开始后不再接收新任务，调用方可同步兜底，避免任务静默丢失。
        }
        if (priority == LifecycleTaskPriority::Cleanup) {
            m_cleanupLifecycleTasks.emplace_back(std::move(task)); // wjy: stop进入高优先级队列，应用退出时不会排在最多20个状态查询之后。
        } else {
            m_backgroundLifecycleTasks.emplace_back(std::move(task)); // wjy: 普通查询仍共用固定线程，线程数不随窗口数量增长。
        }
    }
    m_lifecycleCondition.notify_one(); // wjy: 每次只唤醒一个工作线程消费一个任务，减少20窗口同时关闭时的无效唤醒。
    return true;
}

void RemoteViewerLifecycleManager::beginApplicationShutdown()
{
    if (!m_acceptingViewerStarts) {
        return;
    }
    m_acceptingViewerStarts = false; // wjy: 退出第一步关闭新连接准入，防止清理过程中队列又补启动下一台设备。
    m_waitingViewerStarts.clear(); // wjy: 未开始的窗口无需再建立会话，随后由DeviceGrid统一执行窗口级退出清理。
    {
        std::lock_guard lock(m_lifecycleMutex);
        m_backgroundLifecycleTasks.clear(); // wjy: 退出时丢弃尚未执行的更新查询，只保留正在运行任务和后续高优先级stop，缩短20窗口汇合时间。
    }
}

void RemoteViewerLifecycleManager::shutdownAndWait()
{
    {
        std::lock_guard lock(m_lifecycleMutex);
        if (m_lifecycleWorkersJoined) {
            return; // wjy: MainWindow退出准备和DeviceGrid析构可能重复调用，join流程必须幂等。
        }
        m_acceptingLifecycleTasks = false; // wjy: 所有窗口提交stop后封闭队列，工作线程会先排空已有任务再退出。
    }
    m_lifecycleCondition.notify_all();

    for (std::thread& worker : m_lifecycleWorkers) {
        if (worker.joinable()) {
            worker.join(); // wjy: 确认原生stop/更新查询全部返回后，DeviceGrid才允许删除远控窗口和Presenter。
        }
    }
    m_lifecycleWorkers.clear();

    {
        std::lock_guard lock(m_lifecycleMutex);
        m_lifecycleWorkersJoined = true; // wjy: 记录不可逆的已汇合状态，后续析构重复进入不会再次join。
    }
}

bool RemoteViewerLifecycleManager::isApplicationShuttingDown() const
{
    return !m_acceptingViewerStarts;
}

RemoteViewerLifecycleManager::Diagnostics RemoteViewerLifecycleManager::diagnostics() const
{
    Diagnostics result;
    result.activeViewerStarts = m_activeViewerStarts.size();
    result.waitingViewerStarts = m_waitingViewerStarts.size();
    result.workerThreads = m_lifecycleWorkers.size();
    result.shuttingDown = !m_acceptingViewerStarts;
    {
        std::lock_guard lock(m_lifecycleMutex);
        result.cleanupTasks = m_cleanupLifecycleTasks.size();
        result.backgroundTasks = m_backgroundLifecycleTasks.size(); // wjy: 两类任务队列始终来自固定窗口/查询来源，日志可确认退出时是否持续积压。
    }
    return result;
}

void RemoteViewerLifecycleManager::grantWaitingViewerStarts()
{
    if (!m_acceptingViewerStarts) {
        return; // wjy: 应用退出后即使旧连接回调迟到释放名额，也绝不启动排队窗口。
    }

    while (m_activeViewerStarts.size() < m_maximumConcurrentViewerStarts
        && !m_waitingViewerStarts.isEmpty()) {
        const QPointer<RemoteDesktopWindow> waitingWindow = m_waitingViewerStarts.dequeue();
        if (!waitingWindow || waitingWindow->isClosingConnection()) {
            continue; // wjy: 排队期间已经关闭的窗口直接跳过，不占用初始化名额。
        }
        RemoteDesktopWindow* window = waitingWindow.data();
        m_activeViewerStarts.insert(window); // wjy: FIFO补位仍先登记再启动，保持并发计数在同步回调中也正确。
        window->startViewerConnectionWithAdmission();
    }
}

void RemoteViewerLifecycleManager::lifecycleWorkerLoop()
{
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_lifecycleMutex);
            m_lifecycleCondition.wait(lock, [this] {
                return !m_cleanupLifecycleTasks.empty()
                    || !m_backgroundLifecycleTasks.empty()
                    || !m_acceptingLifecycleTasks; // wjy: 任一优先级有任务即唤醒；退出阶段两类队列排空后结束线程。
            });
            if (m_cleanupLifecycleTasks.empty() && m_backgroundLifecycleTasks.empty()) {
                return; // wjy: 队列已封闭且没有遗留任务，当前固定工作线程可以安全退出。
            }
            if (!m_cleanupLifecycleTasks.empty()) {
                task = std::move(m_cleanupLifecycleTasks.front()); // wjy: 无论提交顺序如何，资源释放都先于普通网络查询执行。
                m_cleanupLifecycleTasks.pop_front();
            } else {
                task = std::move(m_backgroundLifecycleTasks.front());
                m_backgroundLifecycleTasks.pop_front();
            }
        }

        try {
            platform::writeWjyDiagnosticLog(
                QStringLiteral("[wjy-viewer-lifecycle] task begin tid=%1")
                    .arg(static_cast<qulonglong>(std::hash<std::thread::id>{}(std::this_thread::get_id()))));
            task(); // wjy: 单个Viewer停止或查询异常只终止本任务，不允许异常越过线程入口触发std::terminate退出整个程序。
            platform::writeWjyDiagnosticLog(
                QStringLiteral("[wjy-viewer-lifecycle] task end tid=%1")
                    .arg(static_cast<qulonglong>(std::hash<std::thread::id>{}(std::this_thread::get_id()))));
        } catch (const std::exception& exception) {
            platform::writeWjyDiagnosticLog(
                QStringLiteral("[wjy-viewer-lifecycle] task exception=%1")
                    .arg(QString::fromUtf8(exception.what()))); // wjy: 低分配文本记录具体异常，便于定位20窗口压力下的失败点。
        } catch (...) {
            platform::writeWjyDiagnosticLog(
                QStringLiteral("[wjy-viewer-lifecycle] task unknown exception")); // wjy: 未知异常也被隔离，不能让工作线程异常退出带走整个进程。
        }
    }
}
// ===end====

} // namespace ui
