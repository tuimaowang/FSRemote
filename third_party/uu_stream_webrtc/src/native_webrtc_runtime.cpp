#include "native_webrtc_runtime.h"

#include "uu_codec_factory.h"
#include "uu_profile.h"

#include <api/create_modular_peer_connection_factory.h>
#include <api/enable_media_with_defaults.h>
#include <api/environment/environment_factory.h>
#include <api/field_trials.h>
#include <api/peer_connection_interface.h>
#include <api/scoped_refptr.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/thread.h>

#include <exception>
#include <iostream>
#include <mutex>

namespace uu {

namespace {

// =====wjy====
struct SharedWebrtcThreads {
    std::unique_ptr<webrtc::Thread> network_thread;
    std::unique_ptr<webrtc::Thread> worker_thread;
    std::unique_ptr<webrtc::Thread> signaling_thread;
    std::mutex factory_creation_mutex; // wjy: 最多4个Viewer可同时初始化，但共享线程上的Factory创建按顺序进入，降低底层全局状态并发风险。
    bool ssl_initialized = false;

    ~SharedWebrtcThreads()
    {
        if (worker_thread) worker_thread->Stop(); // wjy: 所有引用方的Factory已先销毁，最后一个runtime释放时才停止共享线程。
        if (network_thread) network_thread->Stop();
        if (signaling_thread) signaling_thread->Stop(); // wjy: signaling最后停止，为worker/network退出回投保留依赖队列。
        if (ssl_initialized) webrtc::CleanupSSL(); // wjy: 全进程只和首次成功InitializeSSL配对一次，20个Viewer不再重复初始化/清理。
    }
};

std::mutex g_shared_webrtc_mutex;
std::weak_ptr<SharedWebrtcThreads> g_shared_webrtc_threads;

std::shared_ptr<SharedWebrtcThreads> acquire_shared_webrtc_threads(std::string* error)
{
    std::lock_guard lock(g_shared_webrtc_mutex);
    if (std::shared_ptr<SharedWebrtcThreads> existing = g_shared_webrtc_threads.lock()) {
        return existing; // wjy: Host和全部Viewer复用同一组网络/工作/信令线程，最终在线会话数量不受这里限制。
    }

    auto shared = std::make_shared<SharedWebrtcThreads>();
    if (!webrtc::InitializeSSL()) {
        if (error) *error = "webrtc::InitializeSSL failed";
        return {};
    }
    shared->ssl_initialized = true;
    shared->network_thread = webrtc::Thread::CreateWithSocketServer();
    shared->worker_thread = webrtc::Thread::Create();
    shared->signaling_thread = webrtc::Thread::Create();
    if (!shared->network_thread || !shared->worker_thread || !shared->signaling_thread) {
        if (error) *error = "failed to create shared WebRTC threads";
        return {}; // wjy: shared局部对象析构会停止已创建线程并清理SSL，不留下半初始化全局状态。
    }
    shared->network_thread->SetName("uu_webrtc_network_shared", nullptr);
    shared->worker_thread->SetName("uu_webrtc_worker_shared", nullptr);
    shared->signaling_thread->SetName("uu_webrtc_signaling_shared", nullptr);
    shared->network_thread->Start();
    shared->worker_thread->Start();
    shared->signaling_thread->Start();
    g_shared_webrtc_threads = shared;
    return shared;
}
// ===end====

} // namespace

struct NativeWebrtcRuntime::Impl {
    std::shared_ptr<SharedWebrtcThreads> shared_threads; // wjy: 每个runtime保留引用，最后一个Host/Viewer结束时才统一停止3条共享线程。
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
    DecodedBgraCallback decoded_bgra_callback;
    DecodedTextureCallback decoded_texture_callback;
};

NativeWebrtcRuntime::NativeWebrtcRuntime() = default;

NativeWebrtcRuntime::~NativeWebrtcRuntime()
{
    shutdown();
}

bool NativeWebrtcRuntime::initialize(std::string* error)
{
    try {
        DecodedBgraCallback decoded_bgra_callback = impl_ ? impl_->decoded_bgra_callback : DecodedBgraCallback{}; // wjy: keep the viewer-owned callback across initialize's shutdown reset.
        DecodedTextureCallback decoded_texture_callback = impl_ ? impl_->decoded_texture_callback : DecodedTextureCallback{};
        shutdown();
        impl_ = std::make_unique<Impl>();
        impl_->decoded_bgra_callback = std::move(decoded_bgra_callback); // wjy: pass this viewer's callback into its decoder factory.
        impl_->decoded_texture_callback = std::move(decoded_texture_callback);
        impl_->shared_threads = acquire_shared_webrtc_threads(error);
        if (!impl_->shared_threads) {
            shutdown();
            return false;
        }

        // First keep the modular factory media-free to prove the native
        // PeerConnection core is initialized correctly. Media is enabled only
        // after this path is stable.
        webrtc::PeerConnectionFactoryDependencies deps;
        deps.network_thread = impl_->shared_threads->network_thread.get();
        deps.worker_thread = impl_->shared_threads->worker_thread.get();
        deps.signaling_thread = impl_->shared_threads->signaling_thread.get(); // wjy: Factory和解码回调仍按runtime隔离，仅线程调度资源在进程内共享。
        deps.env = webrtc::CreateEnvironment(
            std::make_unique<webrtc::FieldTrials>(std::string(field_trials())));
        deps.video_encoder_factory = CreateUuVideoEncoderFactory();
        deps.video_decoder_factory = CreateUuVideoDecoderFactory(impl_->decoded_bgra_callback, impl_->decoded_texture_callback);
        std::cout << "webrtc runtime: enable media defaults\n";
        webrtc::EnableMediaWithDefaults(deps);

        std::cout << "webrtc runtime: create modular PeerConnectionFactory on shared threads\n";
        {
            std::lock_guard lock(impl_->shared_threads->factory_creation_mutex);
            impl_->factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps)); // wjy: 独立Factory保留当前Viewer专属decoder回调，不会把一台设备画面投递到另一窗口。
        }

        if (!impl_->factory) {
            if (error) *error = "CreatePeerConnectionFactory failed";
            shutdown();
            return false;
        }
        std::cout << "webrtc runtime: ready\n";
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        shutdown();
        return false;
    }
}

void NativeWebrtcRuntime::shutdown()
{
    // =====wjy====
    if (impl_) {
        impl_->factory = nullptr; // wjy: 当前Viewer先释放独立Factory，再放弃共享线程引用；其它十九路Factory和会话不受影响。
        impl_.reset(); // wjy: 只有最后一个runtime释放shared_ptr时才停止共享线程和清理SSL。
    }
    // ===end====
}

void NativeWebrtcRuntime::set_decoded_bgra_callback(DecodedBgraCallback callback)
{
    if (!impl_) {
        impl_ = std::make_unique<Impl>(); // wjy: keep the viewer callback before initialize creates the decoder factory.
    }
    impl_->decoded_bgra_callback = std::move(callback); // wjy: this callback belongs to one NativeWebrtcRuntime/viewer instance.
}

void NativeWebrtcRuntime::set_decoded_texture_callback(DecodedTextureCallback callback)
{
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->decoded_texture_callback = std::move(callback);
}

webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> NativeWebrtcRuntime::factory() const
{
    return impl_ ? impl_->factory : nullptr;
}

} // namespace uu
