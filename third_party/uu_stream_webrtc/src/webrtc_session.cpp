#include "webrtc_session.h"

#include "native_webrtc_runtime.h"
#include "sdp_guard.h"
#include "uu_profile.h"

#include <api/media_stream_interface.h>
#include <api/make_ref_counted.h>
#include <api/rtp_parameters.h>
#include <api/stats/rtc_stats_collector_callback.h>
#include <api/stats/rtc_stats_report.h>
#include <api/stats/rtcstats_objects.h>
#include <api/transport/bitrate_settings.h> // wjy: 同时更新 Call 级带宽估计边界，不能只改 RtpSender 后仍被全局默认上限截断。
#include <api/create_peerconnection_factory.h>
#include <api/data_channel_interface.h>
#include <api/field_trials.h>
#include <api/rtp_transceiver_interface.h>
#include <api/video/adapted_video_track_source.h>
#include <api/video/video_frame.h>
#include <api/video/video_frame_buffer.h>
#include <api/video_codecs/sdp_video_format.h>
#include <rtc_base/ref_counted_object.h>

#include <algorithm>
#include <atomic>
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

// =====wjy====
struct ReceiverStatsRequestState {
    std::atomic_bool alive = true; // wjy: WebrtcSession 析构先清除此标志，迟到的统计回调只归还 pending，不访问上层对象。
    std::atomic_bool pending = false; // wjy: 每个 PeerConnection 最多允许一个在途统计请求，避免 60 FPS 回调堆积异步任务。
};
// ===end====

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
class SessionVideoSource : public webrtc::AdaptedVideoTrackSource,
                           public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit SessionVideoSource(
        webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> sharedSource)
        : shared_source_(std::move(sharedSource))
    {
        if (!shared_source_) return;
        webrtc::VideoSinkWants sharedWants;
        sharedWants.is_active = true; // wjy: 中继始终按活动且无限制的消费者订阅共享源，任一会话的1 FPS/低分辨率请求都不能反向降低公共采集节奏。
        shared_source_->AddOrUpdateSink(this, sharedWants); // wjy: 共享源只向本会话中继投递原始帧；真正的sender wants在中继自己的VideoAdapter中独立生效。
    }

    ~SessionVideoSource() override
    {
        if (shared_source_) {
            shared_source_->RemoveSink(this); // wjy: RemoveSink返回后保证不再有采集线程回调，避免会话关闭时访问已经析构的中继。
        }
    }

    webrtc::MediaSourceInterface::SourceState state() const override
    {
        return shared_source_ ? shared_source_->state() : kEnded; // wjy: 中继不拥有采集生命周期，只透传HostMediaPipeline共享源的真实状态。
    }

    bool remote() const override
    {
        return shared_source_ && shared_source_->remote(); // wjy: 保留上游源属性，当前Host桌面源仍会返回false。
    }

    bool is_screencast() const override
    {
        return !shared_source_ || shared_source_->is_screencast(); // wjy: 缺少上游时仍按桌面流处理，避免错误启用摄像头类优化。
    }

    std::optional<bool> needs_denoising() const override
    {
        return shared_source_ ? shared_source_->needs_denoising() : std::optional<bool>(false); // wjy: 沿用共享桌面源的降噪声明，不改变现有编码画质策略。
    }

    bool allow_zero_hertz_video() const override
    {
        return shared_source_ && shared_source_->allow_zero_hertz_video(); // wjy: 只隔离会话适配参数，其它WebRTC源能力保持与共享源一致。
    }

    void OnFrame(const webrtc::VideoFrame& frame) override
    {
        const auto sourceBuffer = frame.video_frame_buffer();
        if (!sourceBuffer) {
            AdaptedVideoTrackSource::OnFrameDropped();
            return; // wjy: 异常空帧只通知当前会话丢帧，不能影响共享源和其它控制端。
        }

        const int width = frame.width();
        const int height = frame.height();
        int outWidth = width;
        int outHeight = height;
        int cropWidth = width;
        int cropHeight = height;
        int cropX = 0;
        int cropY = 0;
        if (!AdaptFrame(
                width,
                height,
                frame.timestamp_us(),
                &outWidth,
                &outHeight,
                &cropWidth,
                &cropHeight,
                &cropX,
                &cropY)) {
            return; // wjy: 每个会话在这里独立执行max_framerate丢帧，A请求1 FPS时B的适配器仍可继续输出60 FPS。
        }

        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> outputBuffer = sourceBuffer;
        const bool requiresTransform = outWidth != width
            || outHeight != height
            || cropWidth != width
            || cropHeight != height;
        if (requiresTransform) {
            outputBuffer = sourceBuffer->CropAndScale(
                cropX,
                cropY,
                cropWidth,
                cropHeight,
                outWidth,
                outHeight); // wjy: D3D11原生帧只创建共享纹理视图，CPU回退帧才实际缩放，分辨率档位同样按会话隔离。
        }
        if (!outputBuffer) {
            AdaptedVideoTrackSource::OnFrameDropped();
            return; // wjy: 当前会话转换失败时只丢弃这一条发送链路的帧，不向公共采集源反馈降级状态。
        }

        webrtc::VideoFrame outputFrame(frame);
        outputFrame.set_video_frame_buffer(std::move(outputBuffer)); // wjy: 保留时间戳、旋转和屏幕内容类型，仅替换本会话所需的帧缓冲视图。
        if (requiresTransform) outputFrame.clear_update_rect(); // wjy: 裁剪或缩放后原始脏矩形坐标已失效，清除后让编码器按完整帧处理。
        AdaptedVideoTrackSource::OnFrame(outputFrame); // wjy: 发布到本会话track的独立broadcaster，不再与其它控制端共享sink wants。
    }

    void OnDiscardedFrame() override
    {
        AdaptedVideoTrackSource::OnFrameDropped(); // wjy: 上游偶发丢帧通知仅转发给当前会话sender，保持各会话统计链路完整。
    }

private:
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> shared_source_; // wjy: 持有公共采集源生命周期，但不把本会话的画质限制回写给它。
};
// ===end====

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

// =====wjy====
class ReceiverStatsCollector : public webrtc::RTCStatsCollectorCallback {
public:
    ReceiverStatsCollector(
        std::weak_ptr<ReceiverStatsRequestState> state,
        WebrtcSession::ReceiverStatsCallback callback)
        : state_(std::move(state)), callback_(std::move(callback))
    {
    }

    void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override
    {
        const std::shared_ptr<ReceiverStatsRequestState> state = state_.lock();
        if (!state) return;
        state->pending.store(false, std::memory_order_release); // wjy: 无论会话是否仍存活都先释放下一次采样资格。
        if (!state->alive.load(std::memory_order_acquire) || !report || !callback_) return;

        ReceiverPerformanceStats result;
        result.sample_time_ms = static_cast<uint64_t>(::GetTickCount64());
        for (const auto* inbound : report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
            if (!inbound->kind || *inbound->kind != "video") continue;
            result.frames_received += inbound->frames_received.value_or(0);
            result.frames_decoded += inbound->frames_decoded.value_or(0);
            result.frames_dropped += inbound->frames_dropped.value_or(0);
            result.freeze_count += inbound->freeze_count.value_or(0);
            result.jitter_buffer_emitted_count += inbound->jitter_buffer_emitted_count.value_or(0);
            result.packets_received += inbound->packets_received.value_or(0);
            result.packets_lost += static_cast<uint64_t>(std::max(0, inbound->packets_lost.value_or(0)));
            result.total_decode_time_ms += inbound->total_decode_time.value_or(0.0) * 1000.0;
            result.total_processing_delay_ms += inbound->total_processing_delay.value_or(0.0) * 1000.0;
            result.total_freezes_duration_ms += inbound->total_freezes_duration.value_or(0.0) * 1000.0;
            result.total_jitter_buffer_delay_ms += inbound->jitter_buffer_delay.value_or(0.0) * 1000.0;
        }
        for (const auto* pair : report->GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
            const bool usable = pair->state && *pair->state == "succeeded";
            const bool nominated = pair->nominated.value_or(false);
            if (!usable || (!nominated && result.round_trip_time_ms > 0.0)) continue;
            result.round_trip_time_ms = pair->current_round_trip_time.value_or(0.0) * 1000.0;
            result.available_incoming_bitrate_kbps = pair->available_incoming_bitrate.value_or(0.0) / 1000.0;
            if (nominated) break;
        }
        callback_(result); // wjy: 原生层只交付标准累计统计，是否降档由控制端结合显示和交互状态统一决定。
    }

private:
    std::weak_ptr<ReceiverStatsRequestState> state_;
    WebrtcSession::ReceiverStatsCallback callback_;
};
// ===end====

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
    params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE_AND_RESOLUTION; // wjy: 分辨率和 FPS 只由远控档位协议决定，禁止 WebRTC 另起一套隐藏降级造成局部模糊或帧率漂移。
    const auto result = sender->SetParameters(params);
    if (!result.ok()) {
        std::cerr << "sender SetParameters failed: " << result.message() << "\n";
    }
}

// =====wjy====
bool apply_peer_connection_bitrate(
    webrtc::PeerConnectionInterface* connection,
    uint32_t min_bitrate_kbps,
    uint32_t max_bitrate_kbps,
    std::string* error)
{
    if (!connection) {
        if (error) *error = "peer connection is unavailable";
        return false; // wjy: Sender 参数和传输带宽必须成对生效，连接不存在时不能只修改编码器一侧。
    }
    const uint32_t safeMinKbps = std::max(1u, std::min(min_bitrate_kbps, max_bitrate_kbps)); // wjy: 保证传输约束始终满足 min <= start <= max。
    const uint32_t safeMaxKbps = std::max(safeMinKbps, max_bitrate_kbps);
    const uint32_t safeStartKbps = std::clamp(60000u, safeMinKbps, safeMaxKbps); // wjy: 高质量从 60 Mbps 直接起步；较低档位自动夹到自身上限，避免先冲到高码率再回落。
    webrtc::BitrateSettings settings;
    settings.min_bitrate_bps = static_cast<int>(safeMinKbps * 1000u); // wjy: 更新拥塞控制器下限，避免仅设置 RtpEncoding 后仍被旧的 8.1 Mbps Call 上限截断。
    settings.start_bitrate_bps = static_cast<int>(safeStartKbps * 1000u);
    settings.max_bitrate_bps = static_cast<int>(safeMaxKbps * 1000u);
    const auto result = connection->SetBitrate(settings);
    if (!result.ok()) {
        if (error) *error = std::string(result.message());
        return false;
    }
    return true;
}
// ===end====

} // namespace

WebrtcSession::WebrtcSession(NativeWebrtcRuntime* runtime, SessionConfig config)
    : runtime_(runtime), config_(config), receiver_stats_state_(std::make_shared<ReceiverStatsRequestState>())
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
    if (receiver_stats_state_) {
        receiver_stats_state_->alive.store(false, std::memory_order_release); // wjy: 关闭媒体对象前让所有迟到 getStats 回调立即失效。
    }
    // =====wjy====
    {
        std::lock_guard lock(control_channel_mutex_);
        if (control_channel_) {
            control_channel_->UnregisterObserver(); // wjy: 先阻止迟到 DataChannel 回调，再释放观察者和共享通道引用。
            control_channel_ = nullptr;
        }
        control_observer_.reset();
    }
    // ===end====
    // =====wjy====
    if (remote_video_track_ && remote_video_sink_) { // wjy: WebRTC keeps a raw sink pointer on the track, so unregister before freeing it.
        remote_video_track_->RemoveSink(remote_video_sink_.get()); // wjy: prevent video worker callbacks from hitting a released CallbackVideoSink.
    }
    remote_video_track_ = nullptr; // wjy: release the saved remote track after paired sink cleanup.
    remote_video_sink_.reset();
    // ===end====
    local_video_track_ = nullptr;
    {
        std::lock_guard lock(sender_mutex_);
        local_video_sender_ = nullptr; // wjy: 关闭PeerConnection前先阻止迟到质量消息继续取得sender。
    }
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

// =====wjy====
void WebrtcSession::set_connection_state_callback(ConnectionStateCallback callback)
{
    std::lock_guard lock(callback_mutex_); // wjy: ICE 回调可能来自 WebRTC 网络线程，注册时必须和读取端串行化。
    connection_state_callback_ = std::move(callback); // wjy: 保存 Host 提供的弱引用回调，不让 WebrtcSession 持有上层会话对象。
}
// ===end====

bool WebrtcSession::send_control_message(const std::string& message)
{
    // =====wjy====
    std::lock_guard lock(control_channel_mutex_); // wjy: 与初始化、OnDataChannel 和析构串行，禁止跨线程读写同一个 scoped_refptr。
    if (!control_channel_ || control_channel_->state() != webrtc::DataChannelInterface::kOpen) {
        return false;
    }
    return control_channel_->Send(webrtc::DataBuffer(message)); // wjy: 同一锁也保持多个 Host 辅助线程发送顺序，可靠有序通道不会被并发调用打乱。
    // ===end====
}

// =====wjy====
bool WebrtcSession::apply_sender_quality(
    uint32_t target_width,
    uint32_t target_height,
    uint32_t target_fps,
    uint32_t max_bitrate_kbps,
    uint32_t priority,
    std::string* error)
{
    std::lock_guard lock(sender_mutex_);
    if (!local_video_sender_) {
        if (error) *error = "video sender is unavailable";
        return false; // wjy: 会话尚未完成媒体初始化时返回非致命失败，现有连接不被重建。
    }

    auto parameters = local_video_sender_->GetParameters();
    if (parameters.encodings.empty()) parameters.encodings.emplace_back();
    const uint32_t safe_fps = std::clamp(target_fps, 1u, 60u);
    const uint32_t safe_max_kbps = std::clamp(max_bitrate_kbps, 256u, 240000u);
    const bool high_quality_priority = priority >= 75; // wjy: 当前活动或全屏高质量窗口使用100优先级，后台流畅窗口保持普通优先级。
    const uint32_t bitrate_floor_divisor = high_quality_priority ? 2u : 4u; // wjy: 高质量最低保留上限的一半，120 Mbps配置对应60 Mbps，避免WebRTC把原始分辨率压到7 Mbps。
    const uint32_t safe_min_kbps = std::min(
        safe_max_kbps,
        std::max(256u, safe_max_kbps / bitrate_floor_divisor)); // wjy: 非高质量窗口继续使用四分之一自适应下限，后台720p不会无条件占用高码率。
    const webrtc::Priority network_priority = priority >= 75
        ? webrtc::Priority::kHigh
        : (priority >= 40 ? webrtc::Priority::kMedium : webrtc::Priority::kLow); // wjy: 高质量锁定提高发送优先级，但仍服从WebRTC拥塞控制。
    for (auto& encoding : parameters.encodings) {
        encoding.active = true;
        encoding.min_bitrate_bps = static_cast<int>(safe_min_kbps) * 1000;
        encoding.max_bitrate_bps = static_cast<int>(safe_max_kbps) * 1000;
        encoding.max_framerate = static_cast<int>(safe_fps); // wjy: 在线SetParameters改变FPS，不执行NVENC shutdown/recreate。
        encoding.network_priority = network_priority;
        encoding.scale_resolution_down_by.reset();
        if (target_width == 0 && target_height == 0) {
            encoding.scale_resolution_down_to.reset(); // wjy: 成对0恢复共享捕获源的原始分辨率，不影响其它会话sender。
        } else {
            encoding.scale_resolution_down_to = webrtc::Resolution{
                .width = static_cast<int>(target_width),
                .height = static_cast<int>(target_height),
            }; // wjy: WebRTC视频适配器在编码前按目标尺寸缩放，多个控制端继续共享同一桌面捕获源。
        }
    }
    parameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE_AND_RESOLUTION; // wjy: 高质量原始/60 和其它固定档位均由控制端显式选择，传输层只调码率，不再隐式改变画面尺寸或采样帧率。
    const auto result = local_video_sender_->SetParameters(parameters);
    if (!result.ok()) {
        if (error) *error = std::string(result.message());
        return false;
    }
    if (!apply_peer_connection_bitrate(pc_.get(), safe_min_kbps, safe_max_kbps, error)) {
        return false; // wjy: 同步解除 Call 级 8.1 Mbps 限制，确保 Sender 请求和拥塞控制器使用同一组档位边界。
    }
    return true;
}
// ===end====

// =====wjy====
bool WebrtcSession::request_receiver_performance_stats(ReceiverStatsCallback callback)
{
    const std::shared_ptr<ReceiverStatsRequestState> state = receiver_stats_state_;
    if (config_.role != SessionRole::Viewer || !pc_ || !state || !callback
        || !state->alive.load(std::memory_order_acquire)) {
        return false;
    }
    bool expected = false;
    if (!state->pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false; // wjy: 上一次统计尚未返回时跳过本轮，不创建等待队列也不影响视频解码。
    }
    auto collector = webrtc::make_ref_counted<ReceiverStatsCollector>(state, std::move(callback));
    pc_->GetStats(collector.get()); // wjy: WebRTC 持有回调引用直至异步报告完成，collector 局部引用可安全释放。
    return true;
}
// ===end====

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
    // =====wjy====
    {
        std::lock_guard lock(control_channel_mutex_);
        control_channel_ = data_result.MoveValue(); // wjy: Host 初始化发布通道时与 25ms 光标发送线程串行，未完成前发送稳定返回 false。
        control_observer_ = std::make_unique<ControlDataObserver>(this);
        control_channel_->RegisterObserver(control_observer_.get());
    }
    // ===end====

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
    local_video_source_ = webrtc::make_ref_counted<SessionVideoSource>(
        config_.host_video_source); // wjy: 每个PeerConnection使用独立中继，sender的1 FPS/分辨率请求不再进入HostMediaPipeline共享源的wants聚合。
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
    {
        std::lock_guard lock(sender_mutex_);
        local_video_sender_ = sender; // wjy: 保存本会话sender，后续质量请求只修改这一条RTP发送链路。
    }
    auto transceivers = pc_->GetTransceivers();
    if (!transceivers.empty()) {
        if (!apply_uu_codec_preferences(transceivers.back().get(), send_caps, "host", error)) {
            local_video_source_ = nullptr;
            local_video_track_ = nullptr;
            return false;
        }
    }
    apply_sender_rate(sender.get(), config_.min_bitrate_kbps, config_.target_bitrate_kbps, config_.fps);
    if (!apply_peer_connection_bitrate(pc_.get(), config_.min_bitrate_kbps, config_.target_bitrate_kbps, error)) {
        return false; // wjy: 首帧前把默认带宽估计提升到 60 Mbps 起步，质量请求尚未到达时也不会先被 8.1 Mbps 压糊。
    }
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
    // =====wjy====
    std::lock_guard lock(control_channel_mutex_); // wjy: Viewer 的 WebRTC 回调与 UI 输入发送、质量请求和析构可能并发，必须原子替换通道与观察者。
    if (control_channel_) {
        control_channel_->UnregisterObserver();
    }
    control_channel_ = data_channel;
    control_observer_ = std::make_unique<ControlDataObserver>(this);
    control_channel_->RegisterObserver(control_observer_.get());
    // ===end====
}
void WebrtcSession::OnRenegotiationNeeded() {}
void WebrtcSession::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state)
{
    std::cout << "ice connection state=" << static_cast<int>(state) << "\n";
    append_viewer_log("session ice connection state=" + std::to_string(static_cast<int>(state))); // wjy: durable ICE state sequence.
    // =====wjy====
    ConnectionStateCallback callback;
    {
        std::lock_guard lock(callback_mutex_); // wjy: 复制函数对象后再离开锁调用，防止上层取消 socket 时反向进入 WebRTC 造成锁重入。
        callback = connection_state_callback_;
    }
    if (callback) callback(state); // wjy: Host 根据 disconnected/failed/closed 立即结束僵尸会话；Viewer 未注册时不会产生额外动作。
    // ===end====
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
