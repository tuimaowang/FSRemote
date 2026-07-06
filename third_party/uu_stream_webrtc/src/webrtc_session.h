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
class ParsecVddSession;

enum class SessionRole {
    Host,
    Viewer,
};

struct SessionConfig {
    SessionRole role = SessionRole::Host;
    uint32_t min_bitrate_kbps = 10; // wjy: Let WebRTC drop the compressed video target as low as 10 Kbps when the remote desktop is almost static.
    uint32_t target_bitrate_kbps = 20000; // wjy: Treat this value as the adaptive bitrate ceiling, currently 20 Mbps for 1080p remote-control testing.
    uint32_t fps = 60;
};

class WebrtcSession final : public webrtc::PeerConnectionObserver {
public:
    using SignalCallback = std::function<void(const std::string& kind, const std::string& body)>;
    using FrameCallback = std::function<void(const webrtc::VideoFrame& frame)>;
    using ControlCallback = std::function<void(const std::string& message)>;

    WebrtcSession(NativeWebrtcRuntime* runtime, SessionConfig config);
    ~WebrtcSession() override;

    WebrtcSession(const WebrtcSession&) = delete;
    WebrtcSession& operator=(const WebrtcSession&) = delete;

    bool initialize(std::string* error);
    void set_signal_callback(SignalCallback callback);
    void set_frame_callback(FrameCallback callback);
    void set_control_callback(ControlCallback callback);
    bool send_control_message(const std::string& message);

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
    std::mutex callback_mutex_;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> control_channel_;
    std::unique_ptr<ControlDataObserver> control_observer_;
    std::unique_ptr<ParsecVddSession> host_virtual_display_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> local_video_track_;
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> local_video_source_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> remote_video_track_;
    std::unique_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> remote_video_sink_;
};

} // namespace uu
