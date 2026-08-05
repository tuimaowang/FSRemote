#pragma once

#include <api/media_stream_interface.h>
#include <api/scoped_refptr.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace uu {

// =====wjy====
struct HostMediaPipelineHooks {
    using StartCallback = std::function<webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>(std::string* error)>;
    using StopCallback = std::function<void()>;

    StartCallback start; // wjy: 测试可注入无显卡后端，生产环境留空时创建真实 Parsec VDD 与桌面捕获源。
    StopCallback stop; // wjy: 最后一个订阅释放时调用，用于验证共享后端只停止一次。
};

class HostMediaPipeline final {
private:
    struct SharedState;

public:
    class Subscription final {
    public:
        ~Subscription();

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source() const;

    private:
        friend class HostMediaPipeline;
        Subscription(
            std::shared_ptr<SharedState> state,
            webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source);

        std::shared_ptr<SharedState> state_; // wjy: 订阅令牌可安全晚于 manager 局部关闭，最后释放者仍能完成后端清理。
        webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source_; // wjy: 每个会话持有同一 source 的强引用，但不会拥有捕获线程的启停权。
    };

    explicit HostMediaPipeline(uint32_t fps = 60, HostMediaPipelineHooks hooks = {});
    ~HostMediaPipeline();

    HostMediaPipeline(const HostMediaPipeline&) = delete;
    HostMediaPipeline& operator=(const HostMediaPipeline&) = delete;

    std::unique_ptr<Subscription> subscribe(std::string* error);
    void set_target_fps(uint32_t fps);
    uint32_t target_fps() const;
    uint32_t source_refresh_hz() const;
    void shutdown();
    size_t subscriber_count() const;

private:
    std::shared_ptr<SharedState> state_; // wjy: manager 持有共享状态入口，具体 source/VDD 生命周期由订阅计数决定。
};
// ===end====

} // namespace uu
