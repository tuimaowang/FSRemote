#include "webrtc_session.h"

#include "native_webrtc_runtime.h"
#include "parsec_vdd_session.h"
#include "sdp_guard.h"
#include "uu_profile.h"

#include <api/media_stream_interface.h>
#include <api/create_peerconnection_factory.h>
#include <api/data_channel_interface.h>
#include <api/field_trials.h>
#include <api/rtp_transceiver_interface.h>
#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <api/video_codecs/sdp_video_format.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <media/base/adapted_video_track_source.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <modules/desktop_capture/desktop_capturer.h>
#include <modules/desktop_capture/desktop_frame.h>
#include <rtc_base/time_utils.h>
#include <rtc_base/ref_counted_object.h>
#include <libyuv/convert.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <thread>
#include <vector>

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

std::string to_lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

class DesktopVideoSource : public webrtc::AdaptedVideoTrackSource,
                           public webrtc::DesktopCapturer::Callback {
public:
    DesktopVideoSource(uint32_t fps, int64_t preferredSourceId, std::string preferredDeviceName, bool preferVirtualDisplayPath)
        : fps_(fps ? fps : 60)
        , preferred_source_id_(preferredSourceId)
        , preferred_device_name_(to_lower_copy(std::move(preferredDeviceName)))
        , prefer_virtual_display_path_(preferVirtualDisplayPath)
    {
    }

    ~DesktopVideoSource() override { stop(); }

    bool start(std::string* error)
    {
        auto options = webrtc::DesktopCaptureOptions::CreateDefault();
#if defined(WEBRTC_WIN)
        options.set_allow_directx_capturer(true);
        options.set_allow_wgc_screen_capturer(!prefer_virtual_display_path_);
        options.set_allow_wgc_using_texture(false);
#endif
        capturer_ = webrtc::DesktopCapturer::CreateScreenCapturer(options);
        if (!capturer_) {
            if (error) *error = "DesktopCapturer::CreateScreenCapturer failed";
            return false;
        }
        webrtc::DesktopCapturer::SourceList sources;
        if (capturer_->GetSourceList(&sources) && !sources.empty()) {
            std::cout << "desktop sources:";
            for (const auto& source : sources) {
                std::cout << " [" << source.id << " '" << source.title << "' display=" << source.display_id << "]";
            }
            std::cout << "\n";

            auto chosen = sources.front();
            const auto choose_parsec_fallback = [&sources]() -> webrtc::DesktopCapturer::Source {
                for (const auto& source : sources) {
                    const std::string title = to_lower_copy(source.title);
                    if (title.find("parsec") != std::string::npos || title.find("psccdd0") != std::string::npos) {
                        return source;
                    }
                }
                return sources.front();
            };

            bool matched = false;
            if (preferred_source_id_ != 0) {
                for (const auto& source : sources) {
                    if (static_cast<int64_t>(source.id) == preferred_source_id_
                        || static_cast<int64_t>(source.display_id) == preferred_source_id_) {
                        chosen = source;
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched && !preferred_device_name_.empty()) {
                for (const auto& source : sources) {
                    const std::string title = to_lower_copy(source.title);
                    if (title == preferred_device_name_ || title.find(preferred_device_name_) != std::string::npos) {
                        chosen = source;
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched && (preferred_source_id_ != 0 || !preferred_device_name_.empty())) {
                chosen = choose_parsec_fallback();
            }

            if (!capturer_->SelectSource(chosen.id)) {
                if (error) *error = "DesktopCapturer::SelectSource failed";
                return false;
            }
            std::cout << "desktop selected source id=" << chosen.id
                      << " preferred=" << preferred_source_id_
                      << " name='" << preferred_device_name_ << "'\n";
        }
        capturer_->SetMaxFrameRate(fps_);
        capturer_->Start(this);
        running_ = true;
        thread_ = std::thread([this] { capture_loop(); });
        return true;
    }

    void stop()
    {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        capturer_.reset();
    }

    webrtc::MediaSourceInterface::SourceState state() const override
    {
        return running_ ? kLive : kEnded;
    }

    bool remote() const override { return false; }
    bool is_screencast() const override { return true; }
    std::optional<bool> needs_denoising() const override { return false; }

    void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                         std::unique_ptr<webrtc::DesktopFrame> frame) override
    {
        if (result != webrtc::DesktopCapturer::Result::SUCCESS || !frame || !frame->data()) {
            OnFrameDropped();
            return;
        }

        const int width = frame->size().width();
        const int height = frame->size().height();
        int out_width = width;
        int out_height = height;
        int crop_width = width;
        int crop_height = height;
        int crop_x = 0;
        int crop_y = 0;
        const int64_t now_us = webrtc::TimeMicros();
        if (!AdaptFrame(width, height, now_us, &out_width, &out_height,
                        &crop_width, &crop_height, &crop_x, &crop_y)) {
            return;
        }

        auto buffer = webrtc::I420Buffer::Create(width, height);
        const int converted = libyuv::ARGBToI420(
            frame->data(), frame->stride(),
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            width, height);
        if (converted != 0) {
            OnFrameDropped();
            return;
        }

        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> final_buffer = buffer;
        if (out_width != width || out_height != height || crop_width != width || crop_height != height) {
            final_buffer = buffer->CropAndScale(crop_x, crop_y, crop_width, crop_height, out_width, out_height);
        }

        auto video_frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(final_buffer)
            .set_timestamp_us(now_us)
            .set_rotation(webrtc::kVideoRotation_0)
            .set_content_type(webrtc::VideoContentType::SCREENSHARE)
            .build();
        OnFrame(video_frame);
    }

private:
    void capture_loop()
    {
        const auto interval = std::chrono::microseconds(1000000 / fps_);
        auto next = std::chrono::steady_clock::now();
        while (running_) {
            next += interval;
            if (capturer_) capturer_->CaptureFrame();
            std::this_thread::sleep_until(next);
        }
    }

    uint32_t fps_ = 60;
    int64_t preferred_source_id_ = 0;
    std::string preferred_device_name_;
    bool prefer_virtual_display_path_ = false;
    std::atomic_bool running_ = false;
    std::unique_ptr<webrtc::DesktopCapturer> capturer_;
    std::thread thread_;
};

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

void apply_sender_rate(webrtc::RtpSenderInterface* sender, uint32_t bitrate_kbps, uint32_t fps)
{
    if (!sender) return;
    auto params = sender->GetParameters();
    if (params.encodings.empty()) params.encodings.emplace_back();
    for (auto& encoding : params.encodings) {
        encoding.active = true;
        encoding.max_bitrate_bps = static_cast<int>(bitrate_kbps) * 1000;
        encoding.min_bitrate_bps = static_cast<int>(bitrate_kbps) * 1000;
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
}

WebrtcSession::~WebrtcSession()
{
    if (control_channel_) {
        control_channel_->UnregisterObserver();
        control_channel_ = nullptr;
    }
    remote_video_sink_.reset();
    local_video_track_ = nullptr;
    local_video_source_ = nullptr;
    host_virtual_display_.reset();
    if (pc_) {
        pc_->Close();
    }
    pc_ = nullptr;
}

bool WebrtcSession::initialize(std::string* error)
{
    std::cout << "session: initialize role=" << (config_.role == SessionRole::Host ? "host" : "viewer") << "\n";
    if (!runtime_) {
        if (error) *error = "NativeWebrtcRuntime is null";
        return false;
    }
    std::cout << "session: create peer connection\n";
    if (!create_peer_connection(error)) return false;
    std::cout << "session: configure media\n";
    return config_.role == SessionRole::Host ? configure_host_media(error) : configure_viewer_media(error);
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
    if (!pc_) {
        if (error) *error = "PeerConnection not initialized";
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
        return false;
    }

    pc_->SetRemoteDescription(new_set_description_observer(), desc.release());

    if (kind == "offer") {
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        pc_->CreateAnswer(new_create_description_observer(this), options);
    }
    return true;
}

bool WebrtcSession::add_remote_candidate(const std::string& mid, int mline_index, const std::string& candidate, std::string* error)
{
    if (!pc_) {
        if (error) *error = "PeerConnection not initialized";
        return false;
    }
    webrtc::SdpParseError parse_error;
    std::unique_ptr<webrtc::IceCandidateInterface> ice(
        webrtc::CreateIceCandidate(mid, mline_index, candidate, &parse_error));
    if (!ice) {
        if (error) *error = "CreateIceCandidate failed: " + parse_error.description;
        return false;
    }
    if (!pc_->AddIceCandidate(ice.get())) {
        if (error) *error = "AddIceCandidate failed";
        return false;
    }
    return true;
}

bool WebrtcSession::create_peer_connection(std::string* error)
{
    std::cout << "session: factory lookup\n";
    auto factory = runtime_->factory();
    if (!factory) {
        if (error) *error = "PeerConnectionFactory is not initialized";
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
        return false;
    }
    pc_ = result.MoveValue();
    if (!pc_) {
        if (error) *error = "CreatePeerConnection returned null";
        return false;
    }
    std::cout << "session: peer connection ready\n";

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

    host_virtual_display_ = std::make_unique<ParsecVddSession>();
    std::string vdd_error;
    if (!host_virtual_display_->start(&vdd_error)) {
        std::cout << "host media: parsec-vdd unavailable: " << vdd_error << "\n";
        host_virtual_display_.reset();
    } else {
        std::cout << "host media: parsec-vdd source prepared id="
                  << host_virtual_display_->preferred_source_id()
                  << " name='" << host_virtual_display_->preferred_device_name() << "'\n";
    }

    std::cout << "host media: create desktop source\n";
    auto source = webrtc::make_ref_counted<DesktopVideoSource>(
        config_.fps,
        host_virtual_display_ ? host_virtual_display_->preferred_source_id() : 0,
        host_virtual_display_ ? host_virtual_display_->preferred_device_name() : std::string(),
        host_virtual_display_ != nullptr);
    std::cout << "host media: start desktop source\n";
    if (!source->start(error)) return false;

    auto factory = runtime_->factory();
    const auto send_caps = factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
    log_video_capabilities("host sender", send_caps);
    local_video_source_ = source;
    std::cout << "host media: create video track\n";
    local_video_track_ = factory->CreateVideoTrack(local_video_source_, "video0");
    std::cout << "host media: AddTrack\n";
    auto result = pc_->AddTrack(local_video_track_, {"stream0"});
    if (!result.ok()) {
        if (error) *error = result.error().message();
        source->stop();
        local_video_source_ = nullptr;
        local_video_track_ = nullptr;
        return false;
    }
    auto sender = result.MoveValue();
    auto transceivers = pc_->GetTransceivers();
    if (!transceivers.empty()) {
        if (!apply_uu_codec_preferences(transceivers.back().get(), send_caps, "host", error)) {
            source->stop();
            local_video_source_ = nullptr;
            local_video_track_ = nullptr;
            return false;
        }
    }
    apply_sender_rate(sender.get(), config_.target_bitrate_kbps, config_.fps);
    std::cout << "host media: native WebRTC desktop source enabled; transport recovery is WebRTC-owned\n";
    std::cout << "host media: custom H265 NVENC codec factory is active\n";
    return true;
}

bool WebrtcSession::configure_viewer_media(std::string* error)
{
    if (!pc_) {
        if (error) *error = "PeerConnection not initialized";
        return false;
    }

    webrtc::RtpTransceiverInit init;
    init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
    auto result = pc_->AddTransceiver(webrtc::MediaType::VIDEO, init);
    if (!result.ok()) {
        if (error) *error = result.error().message();
        return false;
    }
    auto factory = runtime_->factory();
    const auto recv_caps = factory->GetRtpReceiverCapabilities(webrtc::MediaType::VIDEO);
    log_video_capabilities("viewer receiver", recv_caps);
    auto transceiver = result.MoveValue();
    if (!apply_uu_codec_preferences(transceiver.get(), recv_caps, "viewer", error)) {
        return false;
    }
    std::cout << "viewer media: recvonly WebRTC video transceiver enabled\n";
    std::cout << "viewer media: custom H265 D3D11 decoder factory is active\n";
    return true;
}

void WebrtcSession::emit_description(webrtc::SessionDescriptionInterface* desc)
{
    std::string sdp;
    desc->ToString(&sdp);

    const auto guard = validate_uu_video_sdp(sdp);
    if (!guard.ok) {
        std::cerr << format_sdp_guard_result(guard) << "\n";
    }

    pc_->SetLocalDescription(
        new_set_description_observer(),
        desc);

    SignalCallback callback;
    {
        std::lock_guard lock(callback_mutex_);
        callback = signal_callback_;
    }
    if (callback) callback(desc->type(), sdp);
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
}
void WebrtcSession::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state)
{
    std::cout << "ice gathering state=" << static_cast<int>(state) << "\n";
}
void WebrtcSession::OnIceCandidate(const webrtc::IceCandidateInterface* candidate)
{
    std::string text;
    candidate->ToString(&text);
    SignalCallback callback;
    {
        std::lock_guard lock(callback_mutex_);
        callback = signal_callback_;
    }
    if (callback) callback("candidate", candidate->sdp_mid() + "\n" + std::to_string(candidate->sdp_mline_index()) + "\n" + text);
}
void WebrtcSession::OnIceConnectionReceivingChange(bool) {}
void WebrtcSession::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>)
{
    std::cout << "remote video track received\n";
    auto receiver = pc_ ? pc_->GetReceivers() : std::vector<webrtc::scoped_refptr<webrtc::RtpReceiverInterface>>{};
    for (const auto& r : receiver) {
        auto track = r->track();
        if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
            auto* video = static_cast<webrtc::VideoTrackInterface*>(track.get());
            remote_video_sink_ = std::make_unique<CallbackVideoSink>(this);
            webrtc::VideoSinkWants wants;
            wants.rotation_applied = true;
            video->AddOrUpdateSink(remote_video_sink_.get(), wants);
            break;
        }
    }
}

void WebrtcSession::on_description_failure(webrtc::RTCError error)
{
    std::cerr << "session description failed: " << error.message() << "\n";
}

} // namespace uu
