#pragma once

#include "stream/RemoteVideoD3D11Surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace ui {

// =====wjy====
class RemoteVideoRenderService final {
public:
    using Observer = std::function<void(const stream::RemoteVideoRenderNotification&)>;

    static RemoteVideoRenderService& instance();

    bool registerWindow(const stream::RemoteVideoSurfaceState& state, Observer observer);
    bool updateWindow(const stream::RemoteVideoSurfaceState& state);
    bool unregisterWindow(std::uint64_t windowId);
    bool submitFrame(std::uint64_t windowId, stream::NativeVideoFrame frame);
    bool invalidateGeneration(std::uint64_t windowId, std::uint64_t generation);
    bool recordStage(const stream::RemoteVideoStageSample& sample) const noexcept; // wjy: 解码回调与UI低频回退/恢复状态复用同一异步诊断入口。
    bool snapshotDiagnostics(const char* fileName = "video_pipeline_snapshot.jsonl") const;

private:
    RemoteVideoRenderService();
    ~RemoteVideoRenderService();

    void dispatch(const stream::RemoteVideoRenderNotification& notification);

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Observer> observers_;
    std::shared_ptr<stream::RemoteVideoDiagnostics> diagnostics_;
    std::shared_ptr<stream::RemoteVideoD3D11Adapter> adapter_;
    std::unique_ptr<stream::RemoteVideoRenderWorker> worker_;
};
// ===end====

} // namespace ui
