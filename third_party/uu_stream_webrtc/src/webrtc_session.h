#pragma once

#include <api/jsep.h>
#include <api/peer_connection_interface.h>
#include <api/scoped_refptr.h>
#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace uu {

class NativeWebrtcRuntime;
class ControlDataObserver;

enum class SessionRole {
    Host,
    Viewer,
};

struct SessionConfig {
    SessionRole role = SessionRole::Host;
    uint32_t min_bitrate_kbps = 9000; // wjy: Keep the compressed video target above 20 Mbps so static desktop frames do not collapse into low-quality refreshes.
    uint32_t target_bitrate_kbps = 120000; // wjy: Treat this value as the adaptive bitrate ceiling, currently 120 Mbps for high-quality remote-control testing.
    uint32_t fps = 60;
    // =====wjy====
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> host_video_source; // wjy: host 会话只消费 manager 管理的共享帧源，不再创建或停止桌面捕获器。
    std::string media_id; // wjy: 每个会话用独立 ID 创建 track/stream，避免多 PeerConnection 诊断与 SDP 标识混淆。
    // ===end====
};

class WebrtcSession final : public webrtc::PeerConnectionObserver {
public:
    using SignalCallback = std::function<void(const std::string& kind, const std::string& body)>;
    using FrameCallback = std::function<void(const webrtc::VideoFrame& frame)>;
    using ControlCallback = std::function<void(const std::string& message)>;
    // =====wjy====
    using ConnectionStateCallback = std::function<void(webrtc::PeerConnectionInterface::IceConnectionState state)>; // wjy: 把 ICE 存活状态上报给 Host 会话管理器，异常断网后可立即清理人数记录。
    // ===end====

    WebrtcSession(NativeWebrtcRuntime* runtime, SessionConfig config);
    ~WebrtcSession() override;

    WebrtcSession(const WebrtcSession&) = delete;
    WebrtcSession& operator=(const WebrtcSession&) = delete;

    bool initialize(std::string* error);
    void set_signal_callback(SignalCallback callback);
    void set_frame_callback(FrameCallback callback);
    void set_control_callback(ControlCallback callback);
    // =====wjy====
    void set_connection_state_callback(ConnectionStateCallback callback); // wjy: Host 注册断线回调；Viewer 未注册时保持原有行为。
    // ===end====
    bool send_control_message(const std::string& message);
    bool apply_sender_quality(
        uint32_t target_width,
        uint32_t target_height,
        uint32_t target_fps,
        uint32_t max_bitrate_kbps,
        uint32_t priority,
        std::string* error); // wjy: Host按会话在线更新sender参数，不停止、不重建PeerConnection或媒体源。

    bool start_offer(std::string* error);
    bool accept_remote_description(const std::string& kind, const std::string& sdp, std::string* error);
    bool add_remote_candidate(const std::string& mid, int mline_index, const std::string& candidate, std::string* error);

    // PeerConnectionObserver.
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override;
    void OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override;
    void OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override;
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
    void OnRenegotiationNeeded() override;
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
    void OnIceConnectionReceivingChange(bool receiving) override;
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;

private:
    bool create_peer_connection(std::string* error);
    bool configure_host_media(std::string* error);
    bool configure_viewer_media(std::string* error);

public:
    void emit_description(webrtc::SessionDescriptionInterface* desc);
    void on_description_failure(webrtc::RTCError error);
    void emit_frame(const webrtc::VideoFrame& frame);
    void emit_control_message(const std::string& message);

private:
    NativeWebrtcRuntime* runtime_ = nullptr;
    SessionConfig config_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
    SignalCallback signal_callback_;
    FrameCallback frame_callback_;
    ControlCallback control_callback_;
    // =====wjy====
    ConnectionStateCallback connection_state_callback_; // wjy: 回调与其它会话回调共用互斥锁，避免 ICE 线程和析构线程并发读写函数对象。
    // ===end====
    std::mutex callback_mutex_;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> control_channel_;
    std::unique_ptr<ControlDataObserver> control_observer_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> local_video_track_;
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> local_video_sender_; // wjy: 保存每会话独立sender，质量消息只调整对应控制端的编码参数。
    std::mutex sender_mutex_; // wjy: data-channel质量回调和WebRTC关闭可能并发访问sender，统一串行保护。
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> local_video_source_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> remote_video_track_;
    std::unique_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> remote_video_sink_;
};

} // namespace uu
