#include "webrtc_session.h"

#include "native_webrtc_runtime.h"
#include "sdp_guard.h"
#include "uu_profile.h"

#include <api/media_stream_interface.h>
#include <api/create_peerconnection_factory.h>
#include <api/data_channel_interface.h>
#include <api/field_trials.h>
#include <api/rtp_transceiver_interface.h>
#include <api/video/video_frame.h>
#include <api/video_codecs/sdp_video_format.h>
#include <rtc_base/ref_counted_object.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <vector>
#include <windows.h>

namespace uu {
namespace {

webrtc::SdpType sdp_type_from_kind(const std::string& kind)
{
    if (kind == "offer") return webrtc::SdpType::kOffer;
    if (kind == "answer") return webrtc::SdpType::kAnswer;
    if (kind == "pranswer") return webrtc::SdpType::kPrAnswer;
    if (kind == "rollback") return webrtc::SdpType::kRollback;
    return webrtc::SdpType::kOffer;
}

// =====wjy====
void append_viewer_log(const std::string& line)
{
    (void)line;
    return; // wjy: disable WebRTC session diagnostics completely while testing stream smoothness.

    char exePath[MAX_PATH] = {}; // wjy: write diagnostics next to FSRemote.exe so the log follows the deployed DLL.
    if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) {
        return;
    }
    std::string path(exePath);
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) {
        return;
    }
    path.resize(slash + 1);
    path += "data\\";
    ::CreateDirectoryA(path.c_str(), nullptr);
    path += "stream_viewer_debug.log";

    SYSTEMTIME time = {};
    ::GetLocalTime(&time);
    char prefix[96] = {};
    std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u tid=%lu ",
                  time.wYear, time.wMonth, time.wDay,
                  time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
                  static_cast<unsigned long>(::GetCurrentThreadId()));

    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "ab") != 0 || !file) {
        return;
    }
    const std::string full = std::string(prefix) + line;
    fwrite(full.data(), 1, full.size(), file);
    fwrite("\r\n", 1, 2, file);
    fclose(file);
}
// ===end====

class CreateDescriptionObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    explicit CreateDescriptionObserver(WebrtcSession* session) : session_(session) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override
    {
        if (session_) session_->emit_description(desc);
    }

    void OnFailure(webrtc::RTCError error) override
    {
        if (session_) session_->on_description_failure(std::move(error));
    }

private:
    WebrtcSession* session_ = nullptr;
};

class SetDescriptionObserver : public webrtc::SetSessionDescriptionObserver {
public:
    void OnSuccess() override {}

    void OnFailure(webrtc::RTCError error) override
    {
        std::cerr << "set description failed: " << error.message() << "\n";
    }
};

webrtc::SetSessionDescriptionObserver* new_set_description_observer()
{
    return new webrtc::RefCountedObject<SetDescriptionObserver>();
}

webrtc::CreateSessionDescriptionObserver* new_create_description_observer(WebrtcSession* session)
{
    return new webrtc::RefCountedObject<CreateDescriptionObserver>(session);
}

class CallbackVideoSink final : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit CallbackVideoSink(WebrtcSession* session) : session_(session) {}

    void OnFrame(const webrtc::VideoFrame& frame) override
    {
        if (session_) session_->emit_frame(frame);
    }

private:
    WebrtcSession* session_ = nullptr;
};

} // namespace

class ControlDataObserver final : public webrtc::DataChannelObserver {
public:
    explicit ControlDataObserver(WebrtcSession* session) : session_(session) {}

    void OnStateChange() override {}

    void OnMessage(const webrtc::DataBuffer& buffer) override
    {
        if (!session_ || buffer.binary) return;
        const auto text = buffer.data.cdata<char>();
        session_->emit_control_message(std::string(text, text + buffer.data.size()));
    }

private:
    WebrtcSession* session_ = nullptr;
};

namespace {

std::string codec_label(const webrtc::RtpCodecCapability& codec)
{
    std::ostringstream oss;
    oss << codec.name;
    if (codec.preferred_payload_type) oss << "/pt=" << *codec.preferred_payload_type;
    if (!codec.parameters.empty()) {
        oss << "(";
        bool first = true;
        for (const auto& [key, value] : codec.parameters) {
            if (!first) oss << ";";
            first = false;
            oss << key << "=" << value;
        }
        oss << ")";
    }
    return oss.str();
}

void log_video_capabilities(const char* label, const webrtc::RtpCapabilities& caps)
{
    std::cout << label << " video codecs:";
    for (const auto& codec : caps.codecs) {
        std::cout << " " << codec_label(codec);
    }
    std::cout << "\n";
    std::cout << label << " video header extensions:";
    for (const auto& ext : caps.header_extensions) {
        std::cout << " " << ext.uri;
        if (ext.preferred_id) {
            std::cout << "#" << ext.preferred_id->value();
        }
    }
    std::cout << "\n";
}

bool codec_name_is(const webrtc::RtpCodecCapability& codec, const char* name)
{
    return _stricmp(codec.name.c_str(), name) == 0;
}

std::optional<webrtc::RtpCodecCapability> first_codec(
    const std::vector<webrtc::RtpCodecCapability>& caps,
    const char* name)
{
    for (auto codec : caps) {
        if (codec_name_is(codec, name)) return codec;
    }
    return std::nullopt;
}

std::optional<webrtc::RtpCodecCapability> rtx_for_apt(
    const std::vector<webrtc::RtpCodecCapability>& caps,
    int apt)
{
    for (auto codec : caps) {
        if (!codec_name_is(codec, "rtx")) continue;
        const auto it = codec.parameters.find("apt");
        if (it != codec.parameters.end() && it->second == std::to_string(apt)) return codec;
    }
    for (auto codec : caps) {
        if (!codec_name_is(codec, "rtx")) continue;
        if (codec.parameters.find("apt") == codec.parameters.end()) {
            codec.parameters["apt"] = std::to_string(apt);
            return codec;
        }
    }
    return std::nullopt;
}

std::vector<webrtc::RtpCodecCapability> uu_ordered_video_codecs(
    const std::vector<webrtc::RtpCodecCapability>& caps,
    std::vector<std::string>* missing_required,
    std::vector<std::string>* missing_optional)
{
    std::vector<webrtc::RtpCodecCapability> ordered;
    std::set<int> used_rtx_payloads;

    auto add_primary = [&](const char* name, bool required) -> std::optional<int> {
        auto codec = first_codec(caps, name);
        if (!codec) {
            if (required) {
                if (missing_required) missing_required->emplace_back(name);
            } else if (missing_optional) {
                missing_optional->emplace_back(name);
            }
            return std::nullopt;
        }
        auto payload_type = codec->preferred_payload_type;
        ordered.push_back(*codec);
        return payload_type;
    };

    auto add_rtx = [&](const char* label, std::optional<int> apt, bool required) {
        if (!apt) return;
        auto codec = rtx_for_apt(caps, *apt);
        if (codec && codec->preferred_payload_type && used_rtx_payloads.contains(*codec->preferred_payload_type)) {
            codec = std::nullopt;
        }
        if (!codec) {
            if (required) {
                if (missing_required) missing_required->emplace_back(label);
            } else if (missing_optional) {
                missing_optional->emplace_back(label);
            }
            return;
        }
        codec->parameters["apt"] = std::to_string(*apt);
        if (codec->preferred_payload_type) used_rtx_payloads.insert(*codec->preferred_payload_type);
        ordered.push_back(*codec);
    };

    const auto hevc_pt = add_primary("H265", true);
    add_rtx("rtx for H265", hevc_pt, true);
    const auto h264_pt = add_primary("H264", false);
    add_rtx("rtx for H264", h264_pt, false);
    const auto red_pt = add_primary("red", false);
    add_rtx("rtx for red", red_pt, false);
    add_primary("ulpfec", false);
    add_primary("flexfec-03", false);
    return ordered;
}

bool apply_uu_codec_preferences(webrtc::RtpTransceiverInterface* transceiver,
                                const webrtc::RtpCapabilities& caps,
                                const char* label,
                                std::string* error)
{
    if (!transceiver) return true;
    std::vector<std::string> missing_required;
    std::vector<std::string> missing_optional;
    auto ordered = uu_ordered_video_codecs(caps.codecs, &missing_required, &missing_optional);
    std::cout << label << " UU-preferred available codecs:";
    for (const auto& codec : ordered) {
        std::cout << " " << codec_label(codec);
    }
    std::cout << "\n";
    if (!missing_optional.empty()) {
        std::cerr << label << " optional UU video codec capability unavailable:";
        for (const auto& item : missing_optional) std::cerr << " " << item;
        std::cerr << "\n";
    }
    if (!missing_required.empty()) {
        std::ostringstream oss;
        oss << label << " WebRTC build is missing UU-required video codec capability:";
        for (const auto& item : missing_required) oss << " " << item;
        if (error) *error = oss.str();
        return false;
    }

    auto err = transceiver->SetCodecPreferences(std::span<webrtc::RtpCodecCapability>(ordered.data(), ordered.size()));
    if (!err.ok()) {
        if (error) *error = std::string(label) + " SetCodecPreferences failed: " + std::string(err.message());
        return false;
    }
    return true;
}

void apply_sender_rate(webrtc::RtpSenderInterface* sender, uint32_t min_bitrate_kbps, uint32_t max_bitrate_kbps, uint32_t fps)
{
    if (!sender) return;
    auto params = sender->GetParameters();
    if (params.encodings.empty()) params.encodings.emplace_back();
    const uint32_t safe_min_kbps = std::max(1u, std::min(min_bitrate_kbps, max_bitrate_kbps)); // wjy: Keep the adaptive bitrate floor valid even if callers pass reversed values.
    const uint32_t safe_max_kbps = std::max(safe_min_kbps, max_bitrate_kbps); // wjy: The ceiling is the user-facing adaptive max bitrate, e.g. 120000 Kbps.
    for (auto& encoding : params.encodings) {
        encoding.active = true;
        encoding.min_bitrate_bps = static_cast<int>(safe_min_kbps) * 1000; // wjy: Keep WebRTC's adaptive bitrate above the configured quality floor during still desktop scenes.
        encoding.max_bitrate_bps = static_cast<int>(safe_max_kbps) * 1000; // wjy: Cap moving/complex scenes at the configured upper bound.
        encoding.max_framerate = static_cast<int>(fps ? fps : 60);
        encoding.network_priority = webrtc::Priority::kHigh;
    }
    const auto result = sender->SetParameters(params);
    if (!result.ok()) {
        std::cerr << "sender SetParameters failed: " << result.message() << "\n";
    }
}

} // namespace

WebrtcSession::WebrtcSession(NativeWebrtcRuntime* runtime, SessionConfig config)
    : runtime_(runtime), config_(config)
{
    // =====wjy====
    append_viewer_log(std::string("session ctor role=") + (config_.role == SessionRole::Host ? "host" : "viewer")); // wjy: identify which PeerConnection instance is producing later callbacks.
    // ===end====
}

WebrtcSession::~WebrtcSession()
{
    // =====wjy====
    append_viewer_log("session dtor begin"); // wjy: if crash happens during close, this marks the start of teardown.
    // ===end====
    if (control_channel_) {
        control_channel_->UnregisterObserver();
        control_channel_ = nullptr;
    }
    // =====wjy====
    if (remote_video_track_ && remote_video_sink_) { // wjy: WebRTC keeps a raw sink pointer on the track, so unregister before freeing it.
        remote_video_track_->RemoveSink(remote_video_sink_.get()); // wjy: prevent video worker callbacks from hitting a released CallbackVideoSink.
    }
    remote_video_track_ = nullptr; // wjy: release the saved remote track after paired sink cleanup.
    remote_video_sink_.reset();
    // ===end====
    local_video_track_ = nullptr;
    local_video_source_ = nullptr;
    if (pc_) {
        pc_->Close();
    }
    pc_ = nullptr;
    // =====wjy====
    append_viewer_log("session dtor end"); // wjy: proves sink/channel cleanup finished without native crash.
    // ===end====
}

bool WebrtcSession::initialize(std::string* error)
{
    // =====wjy====
    append_viewer_log(std::string("session initialize begin role=") + (config_.role == SessionRole::Host ? "host" : "viewer")); // wjy: durable checkpoint before PeerConnection creation.
    // ===end====
    std::cout << "session: initialize role=" << (config_.role == SessionRole::Host ? "host" : "viewer") << "\n";
    if (!runtime_) {
        if (error) *error = "NativeWebrtcRuntime is null";
        return false;
    }
    std::cout << "session: create peer connection\n";
    if (!create_peer_connection(error)) return false;
    std::cout << "session: configure media\n";
    const bool ok = config_.role == SessionRole::Host ? configure_host_media(error) : configure_viewer_media(error);
    append_viewer_log(std::string("session initialize end ok=") + (ok ? "1" : "0") + " error=" + (error ? *error : "")); // wjy: checkpoint after transceiver/media setup.
    return ok;
}

void WebrtcSession::set_signal_callback(SignalCallback callback)
{
    std::lock_guard lock(callback_mutex_);
    signal_callback_ = std::move(callback);
}

void WebrtcSession::set_frame_callback(FrameCallback callback)
{
    std::lock_guard lock(callback_mutex_);
    frame_callback_ = std::move(callback);
}

void WebrtcSession::set_control_callback(ControlCallback callback)
{
    std::lock_guard lock(callback_mutex_);
    control_callback_ = std::move(callback);
}

bool WebrtcSession::send_control_message(const std::string& message)
{
    auto channel = control_channel_;
    if (!channel || channel->state() != webrtc::DataChannelInterface::kOpen) {
        return false;
    }
    return channel->Send(webrtc::DataBuffer(message));
}

bool WebrtcSession::start_offer(std::string* error)
{
    if (!pc_) {
        if (error) *error = "PeerConnection not initialized";
        return false;
    }
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    pc_->CreateOffer(new_create_description_observer(this), options);
    return true;
}

bool WebrtcSession::accept_remote_description(const std::string& kind, const std::string& sdp, std::string* error)
{
    // =====wjy====
    append_viewer_log("session accept_remote_description begin kind=" + kind + " sdp_size=" + std::to_string(sdp.size())); // wjy: mark before SDP parse and SetRemoteDescription.
    // ===end====
    if (!pc_) {
        if (error) *error = "PeerConnection not initialized";
        append_viewer_log("session accept_remote_description failed no_pc"); // wjy: expose unexpected null PeerConnection.
        return false;
    }

    const auto guard = validate_uu_video_sdp(sdp);
    if (!guard.ok) {
        std::cerr << format_sdp_guard_result(guard) << "\n";
    }

    webrtc::SdpParseError parse_error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc(
        webrtc::CreateSessionDescription(sdp_type_from_kind(kind), sdp, &parse_error));
    if (!desc) {
        if (error) {
            *error = "CreateSessionDescription failed at " + parse_error.line + ": " + parse_error.description;
        }
        append_viewer_log("session CreateSessionDescription failed line=" + parse_error.line
            + " description=" + parse_error.description); // wjy: record SDP parse failure details.
        return false;
    }

    append_viewer_log("session SetRemoteDescription call kind=" + kind); // wjy: if this is last, crash is inside/after WebRTC SetRemoteDescription.
    pc_->SetRemoteDescription(new_set_description_observer(), desc.release());
    append_viewer_log("session SetRemoteDescription returned kind=" + kind); // wjy: proves the call returned to our code.

    if (kind == "offer") {
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        append_viewer_log("session CreateAnswer call"); // wjy: host-side answer creation checkpoint.
        pc_->CreateAnswer(new_create_description_observer(this), options);
    }
    append_viewer_log("session accept_remote_description end kind=" + kind); // wjy: durable checkpoint after handling remote SDP.
    return true;
}

bool WebrtcSession::add_remote_candidate(const std::string& mid, int mline_index, const std::string& candidate, std::string* error)
{
    // =====wjy====
    append_viewer_log("session add_remote_candidate begin mid=" + mid + " mline=" + std::to_string(mline_index)
        + " size=" + std::to_string(candidate.size())); // wjy: mark each ICE candidate before WebRTC consumes it.
    // ===end====
    if (!pc_) {
        if (error) *error = "PeerConnection not initialized";
        append_viewer_log("session add_remote_candidate failed no_pc");
        return false;
    }
    webrtc::SdpParseError parse_error;
    std::unique_ptr<webrtc::IceCandidateInterface> ice(
        webrtc::CreateIceCandidate(mid, mline_index, candidate, &parse_error));
    if (!ice) {
        if (error) *error = "CreateIceCandidate failed: " + parse_error.description;
        append_viewer_log("session CreateIceCandidate failed description=" + parse_error.description); // wjy: record malformed ICE.
        return false;
    }
    if (!pc_->AddIceCandidate(ice.get())) {
        if (error) *error = "AddIceCandidate failed";
        append_viewer_log("session AddIceCandidate failed"); // wjy: record WebRTC candidate rejection.
        return false;
    }
    append_viewer_log("session add_remote_candidate end"); // wjy: candidate was accepted.
    return true;
}

bool WebrtcSession::create_peer_connection(std::string* error)
{
    // =====wjy====
    append_viewer_log("session create_peer_connection begin"); // wjy: distinguish factory lookup from media/transceiver failures.
    // ===end====
    std::cout << "session: factory lookup\n";
    auto factory = runtime_->factory();
    if (!factory) {
        if (error) *error = "PeerConnectionFactory is not initialized";
        append_viewer_log("session create_peer_connection failed no_factory");
        return false;
    }

    webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
    rtc_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    rtc_config.servers.clear();

    webrtc::PeerConnectionDependencies dependencies(this);
    dependencies.trials = std::make_unique<webrtc::FieldTrials>(std::string(field_trials()));
    std::cout << "session: CreatePeerConnectionOrError\n";
    auto result = factory->CreatePeerConnectionOrError(rtc_config, std::move(dependencies));
    if (!result.ok()) {
        if (error) *error = result.error().message();
        append_viewer_log(std::string("session CreatePeerConnectionOrError failed error=") + result.error().message()); // wjy: record native factory error.
        return false;
    }
    pc_ = result.MoveValue();
    if (!pc_) {
        if (error) *error = "CreatePeerConnection returned null";
        append_viewer_log("session create_peer_connection failed null_pc");
        return false;
    }
    std::cout << "session: peer connection ready\n";
    append_viewer_log("session create_peer_connection end"); // wjy: peer connection exists before media setup.

    // Codec preferences and RTP extensions must be set before offer/answer.
    // They are not guessed here: host/viewer media configuration below must derive them from
    // uu_profile and native WebRTC capabilities.
    return true;
}

bool WebrtcSession::configure_host_media(std::string* error)
{
    if (!pc_) {
        if (error) *error = "PeerConnection not initialized";
        return false;
    }

    webrtc::DataChannelInit data_init;
    data_init.ordered = true;
    auto data_result = pc_->CreateDataChannelOrError("control", &data_init);
    if (!data_result.ok()) {
        if (error) *error = std::string("CreateDataChannel failed: ") + std::string(data_result.error().message());
        return false;
    }
    control_channel_ = data_result.MoveValue();
    control_observer_ = std::make_unique<ControlDataObserver>(this);
    control_channel_->RegisterObserver(control_observer_.get());

    // =====wjy====
    if (!config_.host_video_source) {
        if (error) *error = "host video source subscription is missing";
        return false; // wjy: host 必须先从 HostMediaPipeline 取得订阅，WebrtcSession 不再隐式启动第二套捕获资源。
    }
    // ===end====

    auto factory = runtime_->factory();
    const auto send_caps = factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
    log_video_capabilities("host sender", send_caps);
    // =====wjy====
    local_video_source_ = config_.host_video_source; // wjy: 多个 host 会话保存同一 source 引用，各自 track/sender 仍完全独立。
    const std::string media_suffix = config_.media_id.empty() ? std::string("0") : config_.media_id;
    std::cout << "host media: create video track\n";
    local_video_track_ = factory->CreateVideoTrack(local_video_source_, "video-" + media_suffix); // wjy: track ID 绑定会话，便于定位单个发送端失败。
    std::cout << "host media: AddTrack\n";
    auto result = pc_->AddTrack(local_video_track_, {"stream-" + media_suffix});
    // ===end====
    if (!result.ok()) {
        if (error) *error = result.error().message();
        local_video_source_ = nullptr;
        local_video_track_ = nullptr;
        return false;
    }
    auto sender = result.MoveValue();
    auto transceivers = pc_->GetTransceivers();
    if (!transceivers.empty()) {
        if (!apply_uu_codec_preferences(transceivers.back().get(), send_caps, "host", error)) {
            local_video_source_ = nullptr;
            local_video_track_ = nullptr;
            return false;
        }
    }
    apply_sender_rate(sender.get(), config_.min_bitrate_kbps, config_.target_bitrate_kbps, config_.fps);
    std::cout << "host media: native WebRTC desktop source enabled; transport recovery is WebRTC-owned\n";
    std::cout << "host media: custom H265 NVENC codec factory is active\n";
    return true;
}

bool WebrtcSession::configure_viewer_media(std::string* error)
{
    // =====wjy====
    append_viewer_log("viewer media configure begin"); // wjy: checkpoint before recvonly transceiver creation.
    // ===end====
    if (!pc_) {
        if (error) *error = "PeerConnection not initialized";
        append_viewer_log("viewer media configure failed no_pc");
        return false;
    }

    webrtc::RtpTransceiverInit init;
    init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
    auto result = pc_->AddTransceiver(webrtc::MediaType::VIDEO, init);
    if (!result.ok()) {
        if (error) *error = result.error().message();
        append_viewer_log(std::string("viewer media AddTransceiver failed error=") + result.error().message()); // wjy: capture transceiver creation failure.
        return false;
    }
    auto factory = runtime_->factory();
    const auto recv_caps = factory->GetRtpReceiverCapabilities(webrtc::MediaType::VIDEO);
    log_video_capabilities("viewer receiver", recv_caps);
    auto transceiver = result.MoveValue();
    append_viewer_log("viewer media AddTransceiver ok"); // wjy: transceiver exists before codec preference is applied.
    if (!apply_uu_codec_preferences(transceiver.get(), recv_caps, "viewer", error)) {
        append_viewer_log("viewer media codec preference failed error=" + (error ? *error : "")); // wjy: codec preference failure should not look like a media crash.
        return false;
    }
    std::cout << "viewer media: recvonly WebRTC video transceiver enabled\n";
    std::cout << "viewer media: custom H265 D3D11 decoder factory is active\n";
    append_viewer_log("viewer media configure end"); // wjy: waiting for remote SDP/media after this point.
    return true;
}

void WebrtcSession::emit_description(webrtc::SessionDescriptionInterface* desc)
{
    // =====wjy====
    append_viewer_log(std::string("session emit_description begin type=") + (desc ? desc->type() : "<null>")); // wjy: mark before local SDP serialization.
    // ===end====
    std::string sdp;
    desc->ToString(&sdp);

    const auto guard = validate_uu_video_sdp(sdp);
    if (!guard.ok) {
        std::cerr << format_sdp_guard_result(guard) << "\n";
    }

    pc_->SetLocalDescription(
        new_set_description_observer(),
        desc);
    append_viewer_log("session SetLocalDescription returned type=" + desc->type()); // wjy: local description was handed to WebRTC.

    SignalCallback callback;
    {
        std::lock_guard lock(callback_mutex_);
        callback = signal_callback_;
    }
    if (callback) {
        callback(desc->type(), sdp);
        append_viewer_log("session emit_description callback returned type=" + desc->type()); // wjy: signaling callback completed.
    }
}

void WebrtcSession::emit_frame(const webrtc::VideoFrame& frame)
{
    FrameCallback callback;
    {
        std::lock_guard lock(callback_mutex_);
        callback = frame_callback_;
    }
    if (callback) callback(frame);
}

void WebrtcSession::emit_control_message(const std::string& message)
{
    ControlCallback callback;
    {
        std::lock_guard lock(callback_mutex_);
        callback = control_callback_;
    }
    if (callback) callback(message);
}

void WebrtcSession::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) {}
void WebrtcSession::OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) {}
void WebrtcSession::OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) {}
void WebrtcSession::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel)
{
    if (!data_channel || data_channel->label() != "control") return;
    if (control_channel_) {
        control_channel_->UnregisterObserver();
    }
    control_channel_ = data_channel;
    control_observer_ = std::make_unique<ControlDataObserver>(this);
    control_channel_->RegisterObserver(control_observer_.get());
}
void WebrtcSession::OnRenegotiationNeeded() {}
void WebrtcSession::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state)
{
    std::cout << "ice connection state=" << static_cast<int>(state) << "\n";
    append_viewer_log("session ice connection state=" + std::to_string(static_cast<int>(state))); // wjy: durable ICE state sequence.
}
void WebrtcSession::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state)
{
    std::cout << "ice gathering state=" << static_cast<int>(state) << "\n";
    append_viewer_log("session ice gathering state=" + std::to_string(static_cast<int>(state))); // wjy: durable ICE gathering sequence.
}
void WebrtcSession::OnIceCandidate(const webrtc::IceCandidateInterface* candidate)
{
    append_viewer_log("session OnIceCandidate begin"); // wjy: local candidate callback entered.
    std::string text;
    candidate->ToString(&text);
    SignalCallback callback;
    {
        std::lock_guard lock(callback_mutex_);
        callback = signal_callback_;
    }
    if (callback) {
        callback("candidate", candidate->sdp_mid() + "\n" + std::to_string(candidate->sdp_mline_index()) + "\n" + text);
        append_viewer_log("session OnIceCandidate callback returned"); // wjy: signaling send callback completed.
    }
}
void WebrtcSession::OnIceConnectionReceivingChange(bool) {}
void WebrtcSession::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>)
{
    // =====wjy====
    append_viewer_log("session OnTrack begin"); // wjy: first checkpoint after remote video track callback.
    // ===end====
    std::cout << "remote video track received\n";
    auto receiver = pc_ ? pc_->GetReceivers() : std::vector<webrtc::scoped_refptr<webrtc::RtpReceiverInterface>>{};
    append_viewer_log("session OnTrack receivers=" + std::to_string(receiver.size())); // wjy: identify whether WebRTC exposed receivers.
    for (const auto& r : receiver) {
        auto track = r->track();
        if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
            auto* video = static_cast<webrtc::VideoTrackInterface*>(track.get());
            append_viewer_log("session OnTrack video found before sink"); // wjy: if this is last, crash is in sink setup.
            webrtc::VideoSinkWants wants;
            wants.rotation_applied = true;
            // =====wjy====
            if (remote_video_track_ && remote_video_track_.get() != video && remote_video_sink_) { // wjy: detach the sink from the previous remote track before replacing it.
                append_viewer_log("session OnTrack remove previous sink"); // wjy: mark renegotiation/track replacement cleanup.
                remote_video_track_->RemoveSink(remote_video_sink_.get()); // wjy: avoid later frames calling into a sink owned by another track.
                remote_video_sink_.reset(); // wjy: destroy the old sink only after it has been unregistered.
            }
            if (!remote_video_sink_) {
                remote_video_sink_ = std::make_unique<CallbackVideoSink>(this); // wjy: create one stable frame sink for the current remote video track.
                append_viewer_log("session OnTrack sink created"); // wjy: sink allocation succeeded.
            }
            remote_video_track_ = video; // wjy: keep the track reference so the destructor can call RemoveSink later.
            append_viewer_log("session OnTrack AddOrUpdateSink call"); // wjy: last line before handing the sink to WebRTC.
            video->AddOrUpdateSink(remote_video_sink_.get(), wants);
            append_viewer_log("session OnTrack AddOrUpdateSink returned"); // wjy: sink registration returned.
            // ===end====
            break;
        }
    }
    append_viewer_log("session OnTrack end"); // wjy: track callback completed.
}

void WebrtcSession::on_description_failure(webrtc::RTCError error)
{
    std::cerr << "session description failed: " << error.message() << "\n";
}

} // namespace uu
