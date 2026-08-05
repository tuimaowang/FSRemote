#include "host_media_pipeline.h"

#include <media/base/adapted_video_track_source.h>
#include <rtc_base/ref_counted_object.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

// =====wjy====
void require(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "host_media_pipeline_tests failed: " << message << '\n';
    std::exit(1); // wjy: 任一共享生命周期断言失败立即返回非零，避免后续计数覆盖真正原因。
}

class FakeVideoSource : public webrtc::AdaptedVideoTrackSource { // wjy: make_ref_counted 通过派生包装引用计数，测试 source 同样不能标记 final。
public:
    webrtc::MediaSourceInterface::SourceState state() const override { return kLive; }
    bool remote() const override { return false; }
    bool is_screencast() const override { return true; }
    std::optional<bool> needs_denoising() const override { return false; }
};
// ===end====

} // namespace

int main()
{
    // =====wjy====
    int starts = 0;
    int stops = 0;
    uu::HostMediaPipelineHooks hooks;
    hooks.start = [&starts](std::string*) {
        ++starts; // wjy: 假后端记录物理捕获启动次数，不依赖 Parsec 驱动或桌面会话。
        return webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>(
            webrtc::make_ref_counted<FakeVideoSource>());
    };
    hooks.stop = [&stops] { ++stops; }; // wjy: 只有最后一个订阅离开时才允许停止共享后端。

    uu::HostMediaPipeline pipeline(60, std::move(hooks));
    std::string error;
    auto first = pipeline.subscribe(&error);
    require(first != nullptr, "first subscriber starts pipeline");
    require(starts == 1 && stops == 0, "first subscriber starts backend exactly once");
    require(pipeline.subscriber_count() == 1, "first subscriber count");

    auto second = pipeline.subscribe(&error);
    require(second != nullptr, "second subscriber reuses pipeline");
    require(first->source().get() == second->source().get(), "subscribers share one video source");
    require(starts == 1 && pipeline.subscriber_count() == 2, "second subscriber must not restart backend");

    first.reset();
    require(stops == 0 && pipeline.subscriber_count() == 1, "one disconnect keeps backend alive");
    second.reset();
    require(stops == 1 && pipeline.subscriber_count() == 0, "last disconnect stops backend");

    auto restarted = pipeline.subscribe(&error);
    require(restarted != nullptr && starts == 2, "new first subscriber restarts idle pipeline");
    pipeline.shutdown();
    require(!pipeline.subscribe(&error), "shutdown rejects new subscribers");
    require(stops == 1, "shutdown waits for existing subscription release");
    restarted.reset();
    require(stops == 2, "final release after shutdown stops backend once");

    std::cout << "host_media_pipeline_tests passed starts=" << starts << " stops=" << stops << '\n';
    // ===end====
    return 0;
}
