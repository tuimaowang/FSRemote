#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace stream {

// =====wjy====
enum class RemoteVideoFrameReleaseReason : std::uint8_t {
    PendingReplaced,
    ExplicitDrop,
    RenderCompleted,
    StaleGeneration,
    Shutdown,
    Invalid,
}; // wjy: 用强类型原因标记帧租约释放路径，日志可以区分正常回收、覆盖和故障丢帧。

class RemoteVideoFrameLease final {
public:
    using ReleaseCallback = std::function<void(RemoteVideoFrameReleaseReason)>;

    explicit RemoteVideoFrameLease(ReleaseCallback callback = {})
        : callback_(std::move(callback)) // wjy: 回收动作由生产端注入，帧队列本身不依赖D3D11实现。
    {
    }

    ~RemoteVideoFrameLease()
    {
        release(RemoteVideoFrameReleaseReason::Shutdown); // wjy: 异常退出或漏掉显式路径时仍归还资源，避免共享纹理永久占槽。
    }

    RemoteVideoFrameLease(const RemoteVideoFrameLease&) = delete;
    RemoteVideoFrameLease& operator=(const RemoteVideoFrameLease&) = delete;

    bool release(RemoteVideoFrameReleaseReason reason) noexcept
    {
        bool expected = false;
        if (!released_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false; // wjy: 释放动作只能成功一次，防止替换、关闭和析构路径重复交接GPU所有权。
        }

        try {
            if (callback_) {
                callback_(reason); // wjy: 回调可以在解码线程、RenderWorker或关闭线程执行，但永远只触发一次。
            }
        } catch (...) {
            // wjy: 资源回收回调不能把异常穿出视频线程边界；底层诊断由调用方另行记录。
        }
        return true;
    }

    bool released() const noexcept
    {
        return released_.load(std::memory_order_acquire);
    }

private:
    ReleaseCallback callback_;
    std::atomic_bool released_ = false;
};

struct NativeVideoFrame {
    std::uint64_t sessionId = 0; // wjy: 稳定会话身份用于跨窗口和重连日志关联。
    std::uint64_t viewerGeneration = 0; // wjy: 重连代际隔离旧解码回调，避免旧帧污染新窗口。
    std::uint64_t frameId = 0; // wjy: 贯穿解码、队列、渲染和日志的单帧编号。
    std::int64_t rtpTimestamp = 0; // wjy: 保留WebRTC原始时间戳，供后续帧时序和端到端延迟诊断使用。
    std::int64_t renderTimeMs = 0; // wjy: 保留WebRTC期望呈现时间，不再丢失解码器提供的调度信息。
    std::int64_t decodedAtUs = 0; // wjy: 使用单调时钟记录解码完成点，避免墙上时钟调整影响帧龄计算。
    int width = 0; // wjy: 原生纹理源宽度，不代表窗口最终显示宽度。
    int height = 0; // wjy: 原生纹理源高度，不代表窗口最终显示高度。
    std::uint32_t format = 0; // wjy: 保留DXGI格式数值，资源缓存按格式变化重建。
    void* sharedHandle = nullptr; // wjy: 兼容旧共享纹理入口，最终由资源租约而不是裸句柄决定生命周期。
    std::shared_ptr<void> nativeResource; // wjy: 可选的ComPtr持有者封装，确保跨线程传递期间原生纹理仍然有效。
    std::shared_ptr<RemoteVideoFrameLease> lease; // wjy: 帧唯一资源释放权，替换和Present完成都通过它回收。

    double encodedMbps = 0.0; // wjy: 仅用于低频标题栏和诊断统计，不参与纹理所有权。
    bool isValid() const noexcept
    {
        return sessionId != 0
            && viewerGeneration != 0
            && frameId != 0
            && width > 0
            && height > 0
            && (sharedHandle != nullptr || nativeResource != nullptr)
            && lease != nullptr; // wjy: 禁止没有身份、尺寸或租约的描述符进入RenderWorker。
    }

    bool release(RemoteVideoFrameReleaseReason reason) const noexcept
    {
        return lease && lease->release(reason); // wjy: 统一从帧对象释放，调用方无需知道底层是Fence还是Keyed Mutex。
    }
};

struct FrameInboxPublishResult {
    bool accepted = false; // wjy: 标记本次帧是否获得了待渲染槽位。
    bool shouldWakeWorker = false; // wjy: 只有从空槽进入时才需要唤醒RenderWorker，避免重复投递任务。
    std::optional<NativeVideoFrame> replaced; // wjy: 返回被最新帧替换的旧帧，由调用方立即释放租约。
};

class FrameInbox final {
public:
    FrameInbox() = default;

    FrameInbox(const FrameInbox&) = delete;
    FrameInbox& operator=(const FrameInbox&) = delete;

    FrameInboxPublishResult publish(NativeVideoFrame frame)
    {
        std::lock_guard lock(mutex_); // wjy: 解码线程和RenderWorker只在交换描述符时短暂持锁，不在锁内执行GPU操作。
        if (!frame.isValid()) {
            ++invalidFrames_; // wjy: 记录无效帧数量，调用方仍可按ExplicitDrop释放其租约。
            return {};
        }

        FrameInboxPublishResult result;
        result.accepted = true;
        result.shouldWakeWorker = !pending_.has_value(); // wjy: 只有空槽进入第一张帧时才需要发出唤醒信号。
        if (pending_.has_value()) {
            result.replaced = std::move(pending_); // wjy: 最新帧覆盖旧待处理帧，避免积累过期画面。
            ++replacedFrames_; // wjy: 覆盖次数单独统计，区别于生产端输出槽忙导致的丢帧。
        }
        pending_ = std::move(frame); // wjy: 槽内始终只保留最新一张未开始渲染的帧。
        ++acceptedFrames_; // wjy: 记录成功进入跨线程队列的帧数量。
        return result;
    }

    std::optional<NativeVideoFrame> takeLatest()
    {
        std::lock_guard lock(mutex_); // wjy: RenderWorker只取走描述符，真正的GPU操作发生在释放锁之后。
        if (!pending_.has_value()) {
            return std::nullopt;
        }
        auto frame = std::move(pending_); // wjy: 取走后槽位立即为空，解码线程可提交下一张帧。
        pending_.reset();
        return frame;
    }

    std::optional<NativeVideoFrame> cancelPending()
    {
        std::lock_guard lock(mutex_); // wjy: 关闭、代际切换和设备回收通过同一入口清空待处理帧。
        auto frame = std::move(pending_); // wjy: 调用方拿到帧后必须显式release，避免租约悬挂。
        pending_.reset();
        return frame;
    }

    std::size_t pendingCount() const
    {
        std::lock_guard lock(mutex_);
        return pending_.has_value() ? 1u : 0u;
    }

    std::uint64_t acceptedFrames() const
    {
        std::lock_guard lock(mutex_);
        return acceptedFrames_;
    }

    std::uint64_t replacedFrames() const
    {
        std::lock_guard lock(mutex_);
        return replacedFrames_;
    }

    std::uint64_t invalidFrames() const
    {
        std::lock_guard lock(mutex_);
        return invalidFrames_;
    }

private:
    mutable std::mutex mutex_;
    std::optional<NativeVideoFrame> pending_; // wjy: 跨线程边界最多保留一张待处理帧，旧帧被新帧替换。
    std::uint64_t acceptedFrames_ = 0;
    std::uint64_t replacedFrames_ = 0;
    std::uint64_t invalidFrames_ = 0;
};
// ===end====

} // namespace stream
