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

namespace uu {

struct NativeWebrtcRuntime::Impl {
    std::unique_ptr<webrtc::Thread> network_thread;
    std::unique_ptr<webrtc::Thread> worker_thread;
    std::unique_ptr<webrtc::Thread> signaling_thread;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
    DecodedBgraCallback decoded_bgra_callback;
    DecodedTextureCallback decoded_texture_callback;
    bool ssl_initialized = false; // wjy: 记录本 runtime 是否成功执行 InitializeSSL，确保 CleanupSSL 只配对调用一次。
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
        std::cout << "webrtc runtime: InitializeSSL\n";
        if (!webrtc::InitializeSSL()) {
            if (error) *error = "webrtc::InitializeSSL failed";
            return false;
        }

        impl_ = std::make_unique<Impl>();
        impl_->ssl_initialized = true; // wjy: 从此处起任一失败路径都由 shutdown 配对清理本次 SSL 初始化。
        impl_->decoded_bgra_callback = std::move(decoded_bgra_callback); // wjy: pass this viewer's callback into its decoder factory.
        impl_->decoded_texture_callback = std::move(decoded_texture_callback);
        std::cout << "webrtc runtime: create threads\n";
        impl_->network_thread = webrtc::Thread::CreateWithSocketServer();
        impl_->worker_thread = webrtc::Thread::Create();
        impl_->signaling_thread = webrtc::Thread::Create();

        if (!impl_->network_thread || !impl_->worker_thread || !impl_->signaling_thread) {
            if (error) *error = "failed to create WebRTC threads";
            shutdown();
            return false;
        }

        impl_->network_thread->SetName("uu_webrtc_network", nullptr);
        impl_->worker_thread->SetName("uu_webrtc_worker", nullptr);
        impl_->signaling_thread->SetName("uu_webrtc_signaling", nullptr);

        std::cout << "webrtc runtime: start threads\n";
        impl_->network_thread->Start();
        impl_->worker_thread->Start();
        impl_->signaling_thread->Start();

        // First keep the modular factory media-free to prove the native
        // PeerConnection core is initialized correctly. Media is enabled only
        // after this path is stable.
        webrtc::PeerConnectionFactoryDependencies deps;
        deps.network_thread = impl_->network_thread.get();
        deps.worker_thread = impl_->worker_thread.get();
        deps.signaling_thread = impl_->signaling_thread.get();
        deps.env = webrtc::CreateEnvironment(
            std::make_unique<webrtc::FieldTrials>(std::string(field_trials())));
        deps.video_encoder_factory = CreateUuVideoEncoderFactory();
        deps.video_decoder_factory = CreateUuVideoDecoderFactory(impl_->decoded_bgra_callback, impl_->decoded_texture_callback);
        std::cout << "webrtc runtime: enable media defaults\n";
        webrtc::EnableMediaWithDefaults(deps);

        std::cout << "webrtc runtime: create modular PeerConnectionFactory\n";
        impl_->factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));

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
        const bool cleanup_ssl = impl_->ssl_initialized; // wjy: 保存后再 reset，避免析构或重复 shutdown 二次清理全局 SSL。
        impl_->factory = nullptr; // wjy: 先同步销毁 factory；其代理会在 signaling 上析构内部对象，并调用仍存活的 worker 完成媒体清理。
        if (impl_->worker_thread) impl_->worker_thread->Stop(); // wjy: factory 已同步完成析构后先停 worker，期间保留 signaling 处理可能的退出回投。
        if (impl_->network_thread) impl_->network_thread->Stop(); // wjy: 网络线程排在 signaling 前停止，避免其退出路径访问已经销毁的信令队列。
        if (impl_->signaling_thread) impl_->signaling_thread->Stop(); // wjy: signaling 最后停止，为其他 WebRTC 线程的清理保留依赖线程。
        impl_.reset();
        if (cleanup_ssl) webrtc::CleanupSSL(); // wjy: 当前 WebRTC 后端的 CleanupSSL 是幂等空操作，但仍与成功初始化保持明确配对。
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
