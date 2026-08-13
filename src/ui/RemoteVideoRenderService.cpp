#include "ui/RemoteVideoRenderService.h"

#include <QCoreApplication>
#include <QDir>

#include <filesystem>
#include <utility>

namespace ui {

// =====wjy====
RemoteVideoRenderService& RemoteVideoRenderService::instance()
{
    static RemoteVideoRenderService service;
    return service;
}

RemoteVideoRenderService::RemoteVideoRenderService()
{
    diagnostics_ = std::make_shared<stream::RemoteVideoDiagnostics>();
    const QString directory = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("data/video_pipeline"));
    diagnostics_->start(std::filesystem::path(directory.toStdWString())); // wjy: 日志线程独立写盘，RenderWorker只提交结构化事件。
    adapter_ = std::make_shared<stream::RemoteVideoD3D11Adapter>(0);
    worker_ = std::make_unique<stream::RemoteVideoRenderWorker>(
        0,
        diagnostics_,
        [this](const stream::RemoteVideoRenderNotification& notification) {
            dispatch(notification); // wjy: RenderWorker回调只查找观察者，具体Qt更新由窗口投递到主线程。
        });
    worker_->start();
}

RemoteVideoRenderService::~RemoteVideoRenderService()
{
    if (worker_) {
        worker_->stop();
    }
    if (diagnostics_) {
        diagnostics_->stop();
    }
}

bool RemoteVideoRenderService::registerWindow(
    const stream::RemoteVideoSurfaceState& state,
    Observer observer)
{
    if (!worker_ || !adapter_ || state.windowId == 0 || !state.nativeWindow || !observer) {
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        observers_[state.windowId] = std::move(observer); // wjy: 先登记观察者，再按命令顺序创建Surface，首帧结果不会丢失。
    }
    if (!worker_->registerSurface(state.windowId, state, adapter_->createSurface())) {
        std::lock_guard lock(mutex_);
        observers_.erase(state.windowId);
        return false;
    }
    return true;
}

bool RemoteVideoRenderService::updateWindow(const stream::RemoteVideoSurfaceState& state)
{
    return worker_ && worker_->updateSurface(state);
}

bool RemoteVideoRenderService::unregisterWindow(std::uint64_t windowId)
{
    {
        std::lock_guard lock(mutex_);
        observers_.erase(windowId); // wjy: 先阻断工作线程回调，再排队释放pending帧和SwapChain。
    }
    return worker_ && worker_->unregisterSurface(windowId);
}

bool RemoteVideoRenderService::submitFrame(std::uint64_t windowId, stream::NativeVideoFrame frame)
{
    {
        std::lock_guard lock(mutex_);
        if (!observers_.contains(windowId)) {
            return false;
        }
    }
    return worker_ && worker_->submitFrame(windowId, std::move(frame));
}

bool RemoteVideoRenderService::invalidateGeneration(std::uint64_t windowId, std::uint64_t generation)
{
    return worker_ && worker_->invalidateGeneration(windowId, generation);
}

bool RemoteVideoRenderService::recordStage(
    const stream::RemoteVideoStageSample& sample) const noexcept
{
    return diagnostics_ && diagnostics_->submitStage(sample); // wjy: 调用方只构造POD式事件，文件IO仍由Diagnostics工作线程完成。
}

bool RemoteVideoRenderService::snapshotDiagnostics(const char* fileName) const
{
    if (!diagnostics_ || !fileName || !*fileName) {
        return false;
    }
    const QString path = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("data/video_pipeline/") + QString::fromUtf8(fileName));
    return diagnostics_->snapshot(std::filesystem::path(path.toStdWString()));
}

void RemoteVideoRenderService::dispatch(const stream::RemoteVideoRenderNotification& notification)
{
    Observer observer;
    {
        std::lock_guard lock(mutex_);
        const auto it = observers_.find(notification.windowId);
        if (it == observers_.end()) {
            return;
        }
        observer = it->second;
    }
    if (observer) {
        observer(notification);
    }
}
// ===end====

} // namespace ui
