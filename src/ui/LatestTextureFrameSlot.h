#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace ui {

// =====wjy====
struct RemoteTextureFrameDescriptor {
    int width = 0; // wjy: 保存共享纹理的实际宽度，UI drain不依赖已经返回的原生回调参数。
    int height = 0; // wjy: 保存共享纹理的实际高度，分辨率变化时Presenter按本帧尺寸重建处理资源。
    void* sharedHandle = nullptr; // wjy: 当前待消费的D3D11共享纹理句柄。
    std::uint64_t frameId = 0; // wjy: 保留解码帧编号，便于诊断丢帧和迟到帧。
    std::int64_t rtpTimestamp = 0; // wjy: 保存WebRTC原始RTP时间轴，排查网络抖动时不再依赖Qt收帧时刻。
    std::int64_t renderTimeMs = 0; // wjy: 保存WebRTC期望呈现时间，后续迁移到RenderWorker时可直接复用。
    std::uint64_t decodedAtUs = 0; // wjy: 保存解码完成的单调时间戳，避免用应用回调时刻伪造帧龄。
    double encodedMbps = 0.0; // wjy: 码率只跟随真正取出的待呈现帧更新。
    std::uint64_t viewerGeneration = 0; // wjy: 纹理绑定Viewer代际，重连后拒绝旧连接回调。

    bool isValid() const
    {
        return width > 0 && height > 0 && sharedHandle != nullptr; // wjy: 无效尺寸或空句柄不进入D3D11呈现路径。
    }
};

enum class TextureFramePushDisposition {
    AcceptedAndScheduleDrain, // wjy: 单槽从空闲进入待消费状态，调用方需要投递唯一Qt drain。
    AcceptedWhileDraining, // wjy: 当前drain已取走上一帧，新帧占用空槽并由完成阶段续投一次。
    DroppedBecausePending, // wjy: 单槽仍保存一张已交给消费者key的纹理，新帧必须由生产端直接回收，不能覆盖旧描述符。
};

struct TextureFramePushResult {
    TextureFramePushDisposition disposition = TextureFramePushDisposition::AcceptedWhileDraining; // wjy: 明确本次push是否取得唯一Qt drain投递权。
};

class LatestTextureFrameSlot final {
public:
    TextureFramePushResult push(RemoteTextureFrameDescriptor frame)
    {
        std::lock_guard lock(m_mutex); // wjy: 解码线程和Qt线程通过同一把锁交换唯一待呈现纹理。
        if (m_pending.has_value()) {
            ++m_replacedFrames; // wjy: 统计因消费者仍占用单槽而被拒绝的新帧，生产端据此立即归还自己的key 0。
            return {TextureFramePushDisposition::DroppedBecausePending}; // wjy: 保留已经排队的旧描述符，避免解码线程跨设备释放其消费者所有权。
        }

        m_pending = std::move(frame); // wjy: 只有空槽才接管新纹理，接管后的句柄只允许Qt线程消费或取消。
        if (m_drainScheduled) {
            return {TextureFramePushDisposition::AcceptedWhileDraining}; // wjy: 已有drain正在执行时允许保存下一帧，但不新增Qt任务。
        }
        m_drainScheduled = true; // wjy: 第一帧取得唯一投递权，后续只由drain完成阶段决定是否续投。
        return {TextureFramePushDisposition::AcceptedAndScheduleDrain};
    }

    std::optional<RemoteTextureFrameDescriptor> takeLatest()
    {
        std::lock_guard lock(m_mutex); // wjy: Qt线程原子取走当前待呈现帧，生产者随后可写入下一轮pending。
        if (!m_pending.has_value()) {
            return std::nullopt;
        }
        auto frame = std::move(m_pending);
        m_pending.reset();
        return frame;
    }

    bool completeDrainAndShouldReschedule()
    {
        std::lock_guard lock(m_mutex); // wjy: 呈现结束后检查执行期间是否接受了一张新帧。
        if (m_pending.has_value()) {
            return true; // wjy: 保持scheduled状态并仅补投一个drain。
        }
        m_drainScheduled = false; // wjy: 单槽为空时释放投递权，下一帧可开启新周期。
        return false;
    }

    std::optional<RemoteTextureFrameDescriptor> cancelPending()
    {
        std::lock_guard lock(m_mutex); // wjy: 关闭、重连或D3D回退时清除尚未呈现的描述符。
        auto cancelled = std::move(m_pending); // wjy: 调用方取得被取消帧后必须完成keyed mutex消费者交接，不能只丢裸句柄。
        m_pending.reset();
        return cancelled;
    }

    std::optional<RemoteTextureFrameDescriptor> cancelScheduledDrain()
    {
        std::lock_guard lock(m_mutex); // wjy: Qt拒绝投递时同时清理单槽和调度标志。
        auto cancelled = std::move(m_pending); // wjy: 已经完成原生回调的帧需要由Qt失败路径继续归还keyed mutex。
        m_pending.reset();
        m_drainScheduled = false;
        return cancelled;
    }

    std::size_t pendingCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_pending.has_value() ? 1u : 0u;
    }

    bool drainScheduled() const
    {
        std::lock_guard lock(m_mutex);
        return m_drainScheduled;
    }

    std::uint64_t replacedFrameCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_replacedFrames; // wjy: 保留既有统计接口名称，但数值现在表示单槽占用时拒绝的新纹理数量。
    }

private:
    mutable std::mutex m_mutex; // wjy: 帮助类不依赖Qt，可由轻量单元测试直接验证并发交接语义。
    std::optional<RemoteTextureFrameDescriptor> m_pending; // wjy: 整个窗口唯一的待呈现共享纹理槽。
    bool m_drainScheduled = false; // wjy: 表示唯一drain已经排队或正在Qt线程执行。
    std::uint64_t m_replacedFrames = 0; // wjy: 统计由生产端安全回收的新纹理，绝不要求解码线程访问Qt Presenter。
};
// ===end====

} // namespace ui
