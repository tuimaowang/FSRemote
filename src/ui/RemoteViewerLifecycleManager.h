#pragma once

#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ui {

class RemoteDesktopWindow;

// =====wjy====
class RemoteViewerLifecycleManager final : public QObject {
public:
    struct Diagnostics {
        int activeViewerStarts = 0;
        int waitingViewerStarts = 0;
        std::size_t cleanupTasks = 0;
        std::size_t backgroundTasks = 0;
        std::size_t workerThreads = 0;
        bool shuttingDown = false;
    };

    enum class LifecycleTaskPriority {
        Background,
        Cleanup,
    }; // wjy: stop清理必须越过排队中的更新查询，避免20窗口退出被低优先级网络探测拖死。

    explicit RemoteViewerLifecycleManager(
        int maximumConcurrentViewerStarts = 4,
        int lifecycleWorkerCount = 4,
        QObject* parent = nullptr); // wjy: 默认仅允许4路同时做昂贵初始化，并用固定4线程处理停止/查询任务，20窗口不会再各自创建detach线程。
    ~RemoteViewerLifecycleManager() override;

    bool requestViewerStart(RemoteDesktopWindow* window); // wjy: 所有窗口立即进入等待队列，但同一时刻只有固定数量获得原生Viewer初始化许可。
    void completeViewerStart(RemoteDesktopWindow* window); // wjy: 连接成功、失败或停止完成时释放初始化名额并补位下一窗口。
    void cancelViewerStart(RemoteDesktopWindow* window, bool keepActiveUntilStop); // wjy: 关闭排队窗口时立即移除；已启动窗口可把名额保留到原生stop真正返回。

    bool submitLifecycleTask(
        std::function<void()> task,
        LifecycleTaskPriority priority = LifecycleTaskPriority::Background); // wjy: stop和更新查询进入固定工作池，清理任务拥有更高出队优先级。
    void beginApplicationShutdown(); // wjy: 第一阶段只关闭新Viewer准入，仍允许窗口提交必要的stop清理任务。
    void shutdownAndWait(); // wjy: 第二阶段停止接收任务、排空队列并join固定工作线程，保证销毁窗口前没有原生生命周期任务遗留。
    bool isApplicationShuttingDown() const;
    Diagnostics diagnostics() const; // wjy: 低频资源日志读取有界队列和固定线程数，不暴露任务对象或窗口所有权。

private:
    void grantWaitingViewerStarts(); // wjy: 每释放一个初始化名额就按FIFO补位，避免后打开的窗口长期插队。
    void lifecycleWorkerLoop(); // wjy: 工作线程持续消费有界来源的生命周期任务，异常被隔离在单个任务边界。

    const int m_maximumConcurrentViewerStarts;
    bool m_acceptingViewerStarts = true;
    QQueue<QPointer<RemoteDesktopWindow>> m_waitingViewerStarts;
    QSet<RemoteDesktopWindow*> m_activeViewerStarts;

    mutable std::mutex m_lifecycleMutex;
    std::condition_variable m_lifecycleCondition;
    std::deque<std::function<void()>> m_cleanupLifecycleTasks; // wjy: Viewer stop等释放资源任务始终优先消费。
    std::deque<std::function<void()>> m_backgroundLifecycleTasks; // wjy: 更新查询等可丢弃后台任务与退出关键路径隔离。
    std::vector<std::thread> m_lifecycleWorkers;
    bool m_acceptingLifecycleTasks = true;
    bool m_lifecycleWorkersJoined = false;
};
// ===end====

} // namespace ui
