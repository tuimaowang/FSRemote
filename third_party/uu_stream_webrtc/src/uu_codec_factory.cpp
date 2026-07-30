#include "uu_codec_factory.h"

#include "d3d11_frame_transformer.h"
#include "d3d11_native_frame_buffer.h"
#include "ffmpeg_decoder.h"
#include "latest_encode_frame_slot.h"
#include "nvenc_h264_encoder.h"
#include "stream_capture_diagnostics.h"

#include <api/scoped_refptr.h>
#include <api/video/resolution.h>
#include <api/video/encoded_image.h>
#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <api/video/video_codec_type.h>
#include <api/video/video_frame_type.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <api/video_codecs/sdp_video_format.h>
#include <libyuv/convert.h>
#include <libyuv/convert_from.h>
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <rtc_base/ref_counted_object.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <d3d11.h>

namespace uu {
namespace {

using Microsoft::WRL::ComPtr;

std::mutex g_decoded_hook_mutex;
std::function<void(const webrtc::VideoFrame&)> g_decoded_hook;
std::mutex g_decoded_bgra_hook_mutex;
DecodedBgraCallback g_decoded_bgra_hook;

// =====wjy====
void append_viewer_log(const std::string& line)
{
    (void)line;
    return; // wjy: disable codec diagnostics completely while testing stream smoothness.

    char exePath[MAX_PATH] = {}; // wjy: decoder diagnostics go to the same deployed log as session/API checkpoints.
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

bool codec_is(const webrtc::SdpVideoFormat& format, const char* name)
{
    return _stricmp(format.name.c_str(), name) == 0;
}

bool is_hevc_random_access_nal(uint8_t nal_type)
{
    return nal_type >= 16 && nal_type <= 21;
}

bool hevc_has_random_access_frame(const uint8_t* data, size_t size)
{
    if (!data || size < 2) return false;
    auto check_nal = [](const uint8_t* nal, size_t nal_size) {
        if (nal_size < 2) return false;
        const uint8_t nal_type = (nal[0] >> 1) & 0x3f;
        return is_hevc_random_access_nal(nal_type);
    };

    bool saw_start_code = false;
    for (size_t i = 0; i + 4 < size; ++i) {
        size_t prefix = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            prefix = 3;
        } else if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            prefix = 4;
        }
        if (!prefix) continue;
        saw_start_code = true;
        const size_t nal_start = i + prefix;
        if (nal_start + 2 <= size && check_nal(data + nal_start, size - nal_start)) return true;
        i = nal_start;
    }
    return !saw_start_code && check_nal(data, size);
}

std::vector<webrtc::SdpVideoFormat> uu_formats()
{
    return {
        webrtc::SdpVideoFormat("H265"),
        webrtc::SdpVideoFormat("H264", {
            {"level-asymmetry-allowed", "1"},
            {"packetization-mode", "1"},
            {"profile-level-id", "42e01f"},
        }),
    };
}

class NvencHevcEncoder final : public webrtc::VideoEncoder {
public:
    ~NvencHevcEncoder() override { Release(); }

    int InitEncode(const webrtc::VideoCodec* codec_settings,
                   const webrtc::VideoEncoder::Settings&) override
    {
        if (!codec_settings || codec_settings->width == 0 || codec_settings->height == 0) {
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                "InitEncode rejected invalid codec settings"); // wjy: WebRTC传入空配置或零尺寸时把参数错误写入被控端采集日志。
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        lsp::append_stream_capture_diagnostic_log(
            "encoder",
            "InitEncode begin size=" + std::to_string(codec_settings->width)
                + "x" + std::to_string(codec_settings->height)
                + " start_kbps=" + std::to_string(codec_settings->startBitrate)
                + " max_fps=" + std::to_string(codec_settings->maxFramerate)); // wjy: 编码器创建入口记录WebRTC协商出的尺寸、码率和帧率。
        stop_worker();
        encoder_.shutdown();
        transformer_.reset();
        texture_.Reset();
        argb_.clear();
        device_.Reset();
        context_.Reset();
        size_ = {};
        encoder_bitrate_kbps_ = 0;
        encoder_fps_ = 0;
        frame_id_ = 0; // wjy: InitEncode重建运行资源但保留已注册回调，兼容WebRTC先Register callback再Init的生命周期顺序。
        tex_w_ = tex_h_ = 0;
        bitrate_kbps_ = std::max(10u, codec_settings->startBitrate ? codec_settings->startBitrate : 80000u);
        fps_ = std::max(1u, codec_settings->maxFramerate ? codec_settings->maxFramerate : 60u);
        target_size_ = {codec_settings->width, codec_settings->height};
        force_keyframe_next_ = true; // wjy: 新编码器会话首帧必须输出IDR和参数集，禁止从无参考帧的P帧开始。
        first_input_enqueued_logged_ = false;
        first_worker_frame_logged_ = false;
        first_encoded_frame_logged_ = false; // wjy: 每个InitEncode代际重新记录“入队→工作线程→码流”三道首帧边界。
        if (!ensure_device(nullptr)) {
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                "InitEncode failed stage=ensure-device"); // wjy: 无法创建D3D11设备时不再只返回通用编码错误。
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (!start_worker()) {
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                "InitEncode failed stage=start-worker"); // wjy: 编码线程创建失败单独标记。
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        lsp::append_stream_capture_diagnostic_log(
            "encoder",
            "InitEncode success"); // wjy: 编码设备和最新帧工作线程都准备完成后写入成功边界。
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override
    {
        std::lock_guard lock(callback_mutex_);
        callback_ = callback;
        lsp::append_stream_capture_diagnostic_log(
            "encoder",
            std::string("RegisterEncodeCompleteCallback callback=") + (callback ? "set" : "null")); // wjy: 确认WebRTC是否在首帧前安装编码完成回调。
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override
    {
        lsp::append_stream_capture_diagnostic_log(
            "encoder",
            "Release begin frame_id=" + std::to_string(frame_id_)); // wjy: 编码器退出时记录已成功输出的帧数，判断是否从未产生首帧。
        stop_worker(); // wjy: 先停止并join最新帧工作线程，再销毁NVENC和D3D资源，杜绝后台访问悬空纹理。
        encoder_.shutdown();
        encoder_bitrate_kbps_ = 0; // wjy: Clear the active NVENC bitrate marker when the encoder session is released.
        encoder_fps_ = 0;
        texture_.Reset();
        argb_.clear();
        transformer_.reset();
        device_.Reset();
        context_.Reset();
        {
            std::lock_guard lock(callback_mutex_);
            callback_ = nullptr;
        }
        force_keyframe_next_ = true; // wjy: 下次重新初始化编码器时重新建立完整关键帧边界。
        frame_id_ = 0;
        tex_w_ = tex_h_ = 0;
        first_input_enqueued_logged_ = false;
        first_worker_frame_logged_ = false;
        first_encoded_frame_logged_ = false;
        lsp::append_stream_capture_diagnostic_log(
            "encoder",
            "Release end"); // wjy: 工作线程、NVENC和D3D资源全部释放后写入终点。
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Encode(const webrtc::VideoFrame& frame,
                   const std::vector<webrtc::VideoFrameType>* frame_types) override
    {
        {
            std::lock_guard callback_lock(callback_mutex_);
            if (!callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        }
        bool force_keyframe = false; // wjy: 首帧和尺寸切换状态只由编码工作线程读取，Encode调用线程仅传递WebRTC关键帧请求，避免数据竞争。
        if (frame_types) {
            force_keyframe = force_keyframe || std::any_of(frame_types->begin(), frame_types->end(), [](webrtc::VideoFrameType type) {
                return type == webrtc::VideoFrameType::kVideoFrameKey;
            });
        }

        {
            std::lock_guard lock(worker_mutex_);
            if (!worker_running_ || worker_stopping_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
            if (pending_frames_.push(frame, force_keyframe)) ++queue_drops_; // wjy: 单槽覆盖旧待编码帧，持续过载只降低采样密度，不累积远控操作延迟。
        }
        if (!first_input_enqueued_logged_.exchange(true)) {
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                "first source frame enqueued size="
                    + std::to_string(frame.video_frame_buffer()->width()) + "x"
                    + std::to_string(frame.video_frame_buffer()->height())); // wjy: 证明共享VideoTrackSource已经把第一帧交给自定义H265编码器。
        }
        worker_cv_.notify_one();
        return WEBRTC_VIDEO_CODEC_OK;
    }

private:
    int32_t encodeFrameSync(const webrtc::VideoFrame& frame, bool force_keyframe_requested)
    {
        webrtc::EncodedImageCallback* callback = nullptr;
        {
            std::lock_guard lock(callback_mutex_);
            callback = callback_;
        }
        if (!callback) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

        ComPtr<ID3D11Texture2D> encode_texture;
        int width = frame.video_frame_buffer()->width();
        int height = frame.video_frame_buffer()->height();
        if (width <= 0 || height <= 0) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

        uint64_t transform_time_us = 0;
        bool native_ready = false;
        auto* native = AsD3D11NativeFrameBuffer(frame.video_frame_buffer().get());
        if (!first_worker_frame_logged_) {
            first_worker_frame_logged_ = true;
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                "worker received first frame size=" + std::to_string(width) + "x" + std::to_string(height)
                    + " native_d3d11=" + std::to_string(native ? 1 : 0)); // wjy: 工作线程首帧标明是否保持原生D3D11纹理，定位CPU回退或队列未唤醒。
        }
        if (native) {
            std::string transform_error;
            if (ensure_device(native->device())
                && transformer_.transform(*native, &encode_texture, &transform_time_us, &transform_error)) {
                D3D11_TEXTURE2D_DESC desc = {};
                encode_texture->GetDesc(&desc);
                width = static_cast<int>(desc.Width);
                height = static_cast<int>(desc.Height);
                native_ready = true; // wjy: 原始高质量帧直接使用采集纹理，低分辨率档使用GPU缩放/NV12纹理，均不经过CPU颜色往返。
            } else if (!transform_error.empty() && frame_id_ % 120 == 0) {
                std::cerr << "D3D11 native transform fallback: " << transform_error << "\n";
                lsp::append_stream_capture_diagnostic_log_rate_limited(
                    "encoder",
                    "native transform fallback error=" + transform_error,
                    2000); // wjy: GPU转换失败会回退I420，限频记录原因而不改变兼容路径。
            }
        }

        if (!native_ready) {
            auto i420 = frame.video_frame_buffer()->ToI420();
            if (!i420) {
                lsp::append_stream_capture_diagnostic_log_rate_limited(
                    "encoder",
                    "ToI420 failed",
                    1000); // wjy: 原生纹理和软件缓冲都无法转换时显式记录。
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            width = i420->width();
            height = i420->height();
            if (width <= 0 || height <= 0 || !ensure_device(nullptr) || !ensure_texture(width, height)) {
                lsp::append_stream_capture_diagnostic_log_rate_limited(
                    "encoder",
                    "software fallback resource preparation failed size="
                        + std::to_string(width) + "x" + std::to_string(height),
                    1000); // wjy: CPU回退资源失败每秒记录一次，不改变返回错误逻辑。
                return WEBRTC_VIDEO_CODEC_ERROR;
            }

            argb_.resize(static_cast<size_t>(width) * height * 4);
            const int converted = libyuv::I420ToARGB(
                i420->DataY(), i420->StrideY(),
                i420->DataU(), i420->StrideU(),
                i420->DataV(), i420->StrideV(),
                argb_.data(), width * 4,
                width, height);
            if (converted != 0) {
                lsp::append_stream_capture_diagnostic_log_rate_limited(
                    "encoder",
                    "I420ToARGB failed code=" + std::to_string(converted),
                    1000); // wjy: 软件颜色转换失败与NVENC失败分开记录。
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            context_->UpdateSubresource(texture_.Get(), 0, nullptr, argb_.data(), width * 4, 0);
            encode_texture = texture_; // wjy: 非原生帧和驱动不支持VideoProcessor时保留既有I420上传回退，不改变软件兼容性。
        }

        // =====wjy====
        const uint32_t requested_bitrate_kbps = bitrate_kbps_.load(); // wjy: NVENC 严格使用 WebRTC 已分配码率；档位下限现在由 Sender 和 PeerConnection 同层保证，禁止编码器私自超发造成发送队列、丢包和清晰度反复恢复。
        const uint32_t requested_fps = fps_.load();
        // ===end====
        if (encoder_.ready() && should_reconfigure_rate(requested_bitrate_kbps, requested_fps)) {
            std::string reconfigure_error;
            if (encoder_.reconfigure(requested_bitrate_kbps, requested_fps, &reconfigure_error)) {
                encoder_bitrate_kbps_ = requested_bitrate_kbps; // wjy: NVENC会话、纹理注册和bitstream全部保留，只更新实时码率/FPS。
                encoder_fps_ = requested_fps;
            } else {
                std::cerr << "NVENC live reconfigure skipped: " << reconfigure_error << "\n"; // wjy: 驱动不支持时继续使用旧会话，绝不shutdown造成断流。
                lsp::append_stream_capture_diagnostic_log_rate_limited(
                    "encoder",
                    "NVENC reconfigure failed error=" + reconfigure_error,
                    2000); // wjy: 在线码率更新失败限频记录，现有编码会话继续运行。
            }
            last_bitrate_reconfigure_ms_ = GetTickCount64(); // wjy: 成功或失败都短暂冷却，避免每帧重复调用不支持的驱动入口。
        }
        if (!encoder_.ready() || size_.width != static_cast<uint32_t>(width) || size_.height != static_cast<uint32_t>(height)) {
            std::string error;
            size_ = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                "NVENC initialize begin size=" + std::to_string(width) + "x" + std::to_string(height)
                    + " bitrate_kbps=" + std::to_string(requested_bitrate_kbps)
                    + " fps=" + std::to_string(requested_fps)
                    + " native_path=" + std::to_string(native_ready ? 1 : 0)); // wjy: 驱动会话创建前保存全部关键参数和输入路径。
            if (!encoder_.initialize(device_.Get(), size_, requested_bitrate_kbps, requested_fps, &error)) {
                std::cerr << "NVENC init failed: " << error << "\n";
                lsp::append_stream_capture_diagnostic_log(
                    "encoder",
                    "NVENC initialize failed error=" + error); // wjy: 真实NVENC错误文本立即落盘，确认是否被ToDesk占用编码会话。
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            encoder_bitrate_kbps_ = requested_bitrate_kbps; // wjy: Remember which bitrate the active NVENC session was initialized with.
            encoder_fps_ = requested_fps;
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                "NVENC initialize success"); // wjy: 驱动编码会话可用后记录成功，后续无首码流只需查看encode阶段。
        }

        bool force_keyframe = frame_id_ == 0 || force_keyframe_next_ || force_keyframe_requested; // wjy: 首帧、尺寸切换和被覆盖帧继承的关键帧请求统一在工作线程执行。

        std::vector<uint8_t> encoded;
        bool keyframe = false;
        std::string error;
        const auto nvenc_started = std::chrono::steady_clock::now();
        const uint32_t encoded_frame_id = frame_id_;
        if (!encoder_.encode(encode_texture.Get(), encoded_frame_id, force_keyframe, &encoded, &keyframe, &error)) {
            if (force_keyframe) force_keyframe_next_ = true; // wjy: 驱动失败不能吞掉PLI/IDR请求，下一张最新帧继续承担恢复关键帧。
            std::cerr << "NVENC encode failed: " << error << "\n";
            lsp::append_stream_capture_diagnostic_log_rate_limited(
                "encoder",
                "NVENC encode failed frame_id=" + std::to_string(encoded_frame_id)
                    + " error=" + error,
                1000); // wjy: 连续驱动错误按秒记录帧号和原因，保持关键帧重试行为不变。
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        ++frame_id_; // wjy: 只有成功取得码流后才推进帧序号，驱动失败重试仍按首帧/关键帧规则恢复。
        const uint64_t nvenc_time_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - nvenc_started).count());
        if (keyframe) {
            force_keyframe_next_ = false; // wjy: 只有NVENC确认实际输出关键帧后才结束尺寸切换交接。
        }
        if (frame_id_ == 1 || frame_id_ % 120 == 0) {
            std::cout << "uu-nvenc-hevc encoded frames=" << frame_id_
                      << " size=" << width << "x" << height
                      << " bytes=" << encoded.size()
                      << " key=" << (keyframe ? 1 : 0) << "\n";
        }

        webrtc::EncodedImage image;
        image.SetEncodedData(webrtc::EncodedImageBuffer::Create(encoded.data(), encoded.size()));
        image.SetRtpTimestamp(frame.rtp_timestamp());
        image.ntp_time_ms_ = frame.ntp_time_ms();
        image.capture_time_ms_ = frame.render_time_ms();
        image._encodedWidth = static_cast<uint32_t>(width);
        image._encodedHeight = static_cast<uint32_t>(height);
        image.rotation_ = frame.rotation();
        image.set_frame_type(keyframe ? webrtc::VideoFrameType::kVideoFrameKey
                                      : webrtc::VideoFrameType::kVideoFrameDelta);
        image.SetRetransmissionAllowed(true);
        webrtc::CodecSpecificInfo codec_info;
        codec_info.codecType = webrtc::kVideoCodecH265;
        callback->OnEncodedImage(image, &codec_info);
        if (!first_encoded_frame_logged_) {
            first_encoded_frame_logged_ = true;
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                "first encoded frame delivered bytes=" + std::to_string(encoded.size())
                    + " keyframe=" + std::to_string(keyframe ? 1 : 0)
                    + " size=" + std::to_string(width) + "x" + std::to_string(height)); // wjy: 第一份H265码流交给WebRTC的边界与控制端首帧等待直接对应。
        }
        ++encoded_since_report_;
        transform_time_us_since_report_ += transform_time_us;
        nvenc_time_us_since_report_ += nvenc_time_us;
        report_encoder_telemetry(); // wjy: 编码完成后聚合GPU转换、NVENC阻塞和队列覆盖数据，直接区分39 FPS瓶颈所在阶段。
        return WEBRTC_VIDEO_CODEC_OK;
    }

    bool start_worker()
    {
        std::lock_guard lock(worker_mutex_);
        if (worker_running_) return true;
        worker_stopping_ = false;
        pending_frames_.clear();
        queue_drops_ = 0;
        encoded_since_report_ = 0;
        transform_time_us_since_report_ = 0;
        nvenc_time_us_since_report_ = 0;
        report_started_ = std::chrono::steady_clock::now();
        try {
            worker_thread_ = std::thread([this] { worker_loop(); });
        } catch (const std::exception& exception) {
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                std::string("worker start exception=") + exception.what()); // wjy: 标准线程创建异常写入诊断文件。
            return false;
        } catch (...) {
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                "worker start unknown exception"); // wjy: 未知线程创建异常也保留失败阶段。
            return false;
        }
        worker_running_ = true;
        return true;
    }

    void stop_worker()
    {
        {
            std::lock_guard lock(worker_mutex_);
            if (!worker_running_) {
                pending_frames_.clear();
                return;
            }
            worker_stopping_ = true;
            pending_frames_.clear(); // wjy: Release不等待已经过时的待编码帧，只让当前NVENC调用自然结束后退出。
        }
        worker_cv_.notify_all();
        if (worker_thread_.joinable()) worker_thread_.join();
        std::lock_guard lock(worker_mutex_);
        worker_running_ = false;
        worker_stopping_ = false;
        pending_frames_.clear();
    }

    void worker_loop()
    {
        for (;;) {
            std::optional<LatestEncodeFrameSlot<webrtc::VideoFrame>::Item> pending;
            {
                std::unique_lock lock(worker_mutex_);
                worker_cv_.wait(lock, [this] { return worker_stopping_ || !pending_frames_.empty(); });
                if (worker_stopping_) break;
                pending = pending_frames_.take(); // wjy: 工作线程一次只取走最新帧，采集/WebRTC线程可立即写入下一帧而不会被NvEncLockBitstream阻塞。
            }

            try {
                if (pending) encodeFrameSync(pending->frame, pending->forceKeyframe);
            } catch (const std::bad_alloc&) {
                std::cerr << "NVENC worker allocation failed\n";
                lsp::append_stream_capture_diagnostic_log_rate_limited(
                    "encoder",
                    "worker allocation failed",
                    1000); // wjy: 内存分配失败按秒记录，工作线程仍保持原有继续循环行为。
            } catch (const std::exception& exception) {
                std::cerr << "NVENC worker exception: " << exception.what() << "\n";
                lsp::append_stream_capture_diagnostic_log_rate_limited(
                    "encoder",
                    std::string("worker exception=") + exception.what(),
                    1000); // wjy: 编码线程标准异常限频记录。
            } catch (...) {
                std::cerr << "NVENC worker unknown exception\n";
                lsp::append_stream_capture_diagnostic_log_rate_limited(
                    "encoder",
                    "worker unknown exception",
                    1000); // wjy: 未知异常不再只输出到不可见控制台。
            }
        }
    }

    void report_encoder_telemetry()
    {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - report_started_);
        if (elapsed < std::chrono::seconds(2)) return;
        const double seconds = std::max(0.001, elapsed.count() / 1000.0);
        const double transform_ms = encoded_since_report_ > 0
            ? static_cast<double>(transform_time_us_since_report_) / encoded_since_report_ / 1000.0
            : 0.0;
        const double nvenc_ms = encoded_since_report_ > 0
            ? static_cast<double>(nvenc_time_us_since_report_) / encoded_since_report_ / 1000.0
            : 0.0;
        const uint64_t queue_drops = queue_drops_.exchange(0); // wjy: 同一统计窗口只读取并清零一次，控制台和持久日志共享完全相同的丢帧值。
        std::cout << "host native encode: output_fps=" << encoded_since_report_ / seconds
                  << " queue_drop=" << queue_drops
                  << " gpu_transform_ms=" << transform_ms
                  << " nvenc_ms=" << nvenc_ms << "\n"; // wjy: 只输出稳定窗口平均值，避免逐帧日志本身制造卡顿和磁盘抖动。
        std::ostringstream diagnostic;
        diagnostic << "telemetry output_fps=" << encoded_since_report_ / seconds
                   << " queue_drop=" << queue_drops
                   << " gpu_transform_ms=" << transform_ms
                   << " nvenc_ms=" << nvenc_ms;
        lsp::append_stream_capture_diagnostic_log(
            "encoder",
            diagnostic.str()); // wjy: 两秒聚合编码指标持久化，判断是否卡在队列、GPU转换或同步NVENC调用。
        report_started_ = now;
        encoded_since_report_ = 0;
        transform_time_us_since_report_ = 0;
        nvenc_time_us_since_report_ = 0;
    }

    void SetRates(const RateControlParameters& parameters) override
    {
        const uint32_t bps = parameters.bitrate.get_sum_bps();
        if (bps > 0) bitrate_kbps_.store(std::max(10u, bps / 1000u));
        if (parameters.framerate_fps > 0.0) fps_.store(static_cast<uint32_t>(parameters.framerate_fps + 0.5));
    }

    EncoderInfo GetEncoderInfo() const override
    {
        EncoderInfo info;
        info.implementation_name = "uu-nvenc-hevc";
        info.is_hardware_accelerated = true;
        info.has_trusted_rate_controller = true;
        info.supports_native_handle = true; // wjy: 允许WebRTC把D3D11NativeFrameBuffer原样送入自定义H265编码器，跳过默认ToI420映射。
        info.supports_simulcast = false;
        info.scaling_settings = ScalingSettings::kOff;
        return info;
    }

private:
    bool should_reconfigure_rate(uint32_t requested_bitrate, uint32_t requested_fps) const
    {
        if (encoder_bitrate_kbps_ == 0) {
            return false;
        }
        const uint32_t lower = std::min(encoder_bitrate_kbps_, requested_bitrate);
        const uint32_t upper = std::max(encoder_bitrate_kbps_, requested_bitrate);
        const bool changed_enough = (upper - lower) * 100 >= std::max(upper, 1u) * 15; // wjy: Ignore tiny target-rate jitter; only rebuild NVENC for meaningful adaptive bitrate movement.
        const bool fps_changed = encoder_fps_ != requested_fps; // wjy: 在线质量协议改变目标FPS时同步更新NVENC帧率字段，不重建编码器。
        const bool cooled_down = GetTickCount64() - last_bitrate_reconfigure_ms_ >= 500; // wjy: Keep reconfiguration below roughly twice per second to protect remote-control smoothness.
        return (changed_enough || fps_changed) && cooled_down;
    }

    bool ensure_device(ID3D11Device* preferred_device)
    {
        if (preferred_device) {
            if (device_.Get() == preferred_device && context_) return true;
            encoder_.shutdown();
            transformer_.reset();
            texture_.Reset();
            argb_.clear();
            size_ = {};
            encoder_bitrate_kbps_ = 0;
            encoder_fps_ = 0;
            tex_w_ = tex_h_ = 0;
            force_keyframe_next_ = true;
            device_ = preferred_device;
            device_->GetImmediateContext(&context_); // wjy: 原生采集纹理必须由同一D3D11设备注册给NVENC，设备切换时完整重置编码资源。
            if (!context_) {
                lsp::append_stream_capture_diagnostic_log(
                    "encoder",
                    "preferred D3D11 device returned null immediate context"); // wjy: 采集设备存在但编码上下文为空时记录驱动异常。
            }
            return context_ != nullptr;
        }
        if (device_ && context_) return true;
        D3D_FEATURE_LEVEL feature_level = {};
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                             levels, 2, D3D11_SDK_VERSION,
                                             &device_, &feature_level, &context_);
        if (FAILED(hr)) {
            char line[96] = {};
            std::snprintf(line, sizeof(line), "D3D11CreateDevice failed hr=0x%08lX", static_cast<unsigned long>(hr));
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                line); // wjy: 软件回退创建编码设备失败时保存HRESULT。
        }
        return SUCCEEDED(hr);
    }

    bool ensure_texture(int width, int height)
    {
        if (texture_ && tex_w_ == width && tex_h_ == height) return true;
        texture_.Reset();
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture_);
        if (FAILED(hr)) {
            char line[128] = {};
            std::snprintf(
                line,
                sizeof(line),
                "CreateTexture2D failed hr=0x%08lX size=%dx%d",
                static_cast<unsigned long>(hr),
                width,
                height);
            lsp::append_stream_capture_diagnostic_log(
                "encoder",
                line); // wjy: CPU回退上传纹理创建失败时保存HRESULT和尺寸。
            return false;
        }
        tex_w_ = width;
        tex_h_ = height;
        encoder_.shutdown();
        encoder_bitrate_kbps_ = 0; // wjy: Force NVENC reinitialization after texture size changes.
        encoder_fps_ = 0;
        force_keyframe_next_ = true; // wjy: 任意输入尺寸变化都要求新会话第一帧强制IDR，避免短暂模糊或参考链失效。
        return true;
    }

    mutable std::mutex callback_mutex_;
    webrtc::EncodedImageCallback* callback_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> texture_;
    std::vector<uint8_t> argb_;
    D3D11FrameTransformer transformer_; // wjy: 原生高质量帧直通，均衡/流畅档位在GPU内裁剪缩放并优先转换NV12。
    lsp::NvencH264Encoder encoder_;
    lsp::Size size_;
    lsp::Size target_size_;
    std::atomic<uint32_t> bitrate_kbps_ = 80000;
    uint32_t encoder_bitrate_kbps_ = 0;
    uint32_t encoder_fps_ = 0;
    uint64_t last_bitrate_reconfigure_ms_ = 0;
    std::atomic<uint32_t> fps_ = 60;
    uint32_t frame_id_ = 0;
    bool force_keyframe_next_ = true; // wjy: 记录新会话或尺寸切换后的关键帧交接状态，成功IDR前持续请求。
    int tex_w_ = 0;
    int tex_h_ = 0;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    LatestEncodeFrameSlot<webrtc::VideoFrame> pending_frames_; // wjy: 单槽队列始终保存尚未编码的最新帧，不允许旧画面排队堆积。
    std::thread worker_thread_;
    bool worker_running_ = false;
    bool worker_stopping_ = false;
    std::atomic<uint64_t> queue_drops_ = 0;
    std::chrono::steady_clock::time_point report_started_;
    uint64_t encoded_since_report_ = 0;
    uint64_t transform_time_us_since_report_ = 0;
    uint64_t nvenc_time_us_since_report_ = 0;
    std::atomic_bool first_input_enqueued_logged_ = false; // wjy: 跨WebRTC调用线程安全记录第一帧进入编码单槽。
    bool first_worker_frame_logged_ = false; // wjy: 仅编码工作线程访问，标记第一帧是否保持原生D3D11路径。
    bool first_encoded_frame_logged_ = false; // wjy: 仅编码工作线程访问，标记第一份H265码流已经交给WebRTC。
};

class HevcD3d11Decoder final : public webrtc::VideoDecoder {
public:
    explicit HevcD3d11Decoder(DecodedBgraCallback bgra_callback = {}, DecodedTextureCallback texture_callback = {})
        : bgra_callback_(std::move(bgra_callback)) // wjy: bind one decoder to one viewer window when the runtime provides a callback.
        , texture_callback_(std::move(texture_callback))
    {
    }

    bool Configure(const Settings&) override
    {
        // =====wjy====
        append_viewer_log("decoder Configure begin"); // wjy: first decoder lifecycle callback after WebRTC chooses H265.
        // ===end====
        std::cout << "uu-d3d11va-hevc Configure\n";
        const bool ok = ensure_device();
        append_viewer_log(std::string("decoder Configure end ok=") + (ok ? "1" : "0")); // wjy: tell whether D3D11 device creation succeeded.
        return ok;
    }

    int32_t Decode(const webrtc::EncodedImage& input_image, int64_t render_time_ms) override
    {
        try {
            return decodeFrame(input_image, render_time_ms); // wjy: 单路解码分配/FFmpeg/D3D异常统一变成可恢复错误码，其它Viewer继续工作。
        } catch (const std::bad_alloc&) {
            append_viewer_log("decoder bad_alloc");
            return WEBRTC_VIDEO_CODEC_ERROR;
        } catch (const std::exception& exception) {
            append_viewer_log(std::string("decoder exception: ") + exception.what());
            return WEBRTC_VIDEO_CODEC_ERROR;
        } catch (...) {
            append_viewer_log("decoder unknown exception");
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
    }

private:
    int32_t decodeFrame(const webrtc::EncodedImage& input_image, int64_t render_time_ms)
    {
        // =====wjy====
        // if (decode_calls_ < 5 || decode_calls_ % 120 == 0) {
        //     append_viewer_log("decoder Decode enter call=" + std::to_string(decode_calls_)
        //         + " size=" + std::to_string(input_image.size())
        //         + " key=" + std::to_string(input_image.IsKey() ? 1 : 0)
        //         + " rtp=" + std::to_string(input_image.RtpTimestamp())); // wjy: sampled decode log disabled to reduce video-thread file IO.
        // }
        // ===end====
        if (!callback_ || !input_image.data() || input_image.size() == 0) {
            append_viewer_log("decoder Decode bad parameter"); // wjy: distinguish bad WebRTC input from native decode crash.
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        if (decode_calls_ == 0) {
            std::cout << "uu-d3d11va-hevc first Decode size=" << input_image.size()
                      << " rtp=" << input_image.RtpTimestamp() << "\n";
        }
        update_encoded_stats(input_image.size()); // wjy: Measure compressed video bytes before decode so the UI can show real encoded bitrate.
        ++decode_calls_;
        if (!ensure_device()) {
            append_viewer_log("decoder ensure_device failed in Decode"); // wjy: D3D11 device could not be created/reused.
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        std::vector<uint8_t> data(input_image.data(), input_image.data() + input_image.size());
        const bool encoded_keyframe = input_image.IsKey() || hevc_has_random_access_frame(data.data(), data.size());
        if (waiting_for_keyframe_ && !encoded_keyframe) {
            const uint64_t now = GetTickCount64();
            if (now - last_keyframe_request_ms_ >= 50) {
                last_keyframe_request_ms_ = now;
                // append_viewer_log("decoder waiting keyframe request error"); // wjy: repeated keyframe-wait log disabled to avoid flooding.
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            // append_viewer_log("decoder waiting keyframe skip"); // wjy: repeated keyframe-wait log disabled to avoid flooding.
            return WEBRTC_VIDEO_CODEC_OK;
        }
        if (encoded_keyframe && waiting_for_keyframe_) {
            append_viewer_log("decoder keyframe while waiting reset"); // wjy: first usable keyframe resets FFmpeg/D3D decode state.
            decoder_.reset();
            decoder_ready_ = false;
        }
        if (!decoder_ready_) {
            std::string error;
            append_viewer_log("decoder initialize_d3d11 begin"); // wjy: last checkpoint before FFmpeg D3D11 decoder init.
            if (!decoder_.initialize_d3d11(device_.Get(), context_.Get(), &error)) {
                std::cerr << "decoder init failed: " << error << "\n";
                append_viewer_log("decoder initialize_d3d11 failed error=" + error); // wjy: record native decoder initialization failure.
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            decoder_ready_ = true;
            append_viewer_log("decoder initialize_d3d11 ok"); // wjy: native decoder is ready before compressed frame decode.
        }

        lsp::DecodedFrame decoded;
        std::string error;
        // append_viewer_log("decoder ffmpeg decode begin size=" + std::to_string(data.size())); // wjy: per-frame decode log disabled for smoother multi-view streaming.
        const lsp::DecodeResult decodeResult = decoder_.decode(data, &decoded, &error);
        if (decodeResult.status == lsp::DecodeStatus::NeedMoreInput) {
            return WEBRTC_VIDEO_CODEC_OK; // wjy: FFmpeg低延迟路径暂未产出画面属于正常节奏，保持解码实例和参考链。
        }
        if (decodeResult.status == lsp::DecodeStatus::OutputTextureBusy) {
            ++output_texture_busy_drops_; // wjy: Presenter未及时归还三槽时只丢显示输出，不把背压计入真实解码错误。
            consecutive_corrupt_decode_errors_ = 0; // wjy: 共享纹理繁忙发生在码流成功解码之后，证明参考链健康并清除旧损坏连续计数。
            waiting_for_keyframe_ = false; // wjy: 当前压缩帧已经成功进入FFmpeg参考链；即使没有可写显示纹理，也不能继续等待或请求关键帧。
            if (output_texture_busy_drops_ <= 3 || output_texture_busy_drops_ % 120 == 0) {
                std::cout << "uu-d3d11va-hevc output texture busy drops=" << output_texture_busy_drops_ << "\n";
            }
            return WEBRTC_VIDEO_CODEC_OK; // wjy: 不reset、不进入等待关键帧、不请求PLI，也不触发软件回退。
        }
        if (decodeResult.status == lsp::DecodeStatus::DeviceLost) {
            ++decode_errors_;
            decoder_.reset(); // wjy: 先释放当前Viewer持有的FFmpeg/D3D11VA对象，其他远控窗口的独立解码器不受影响。
            decoder_ready_ = false;
            staging_.Reset();
            staging_w_ = 0;
            staging_h_ = 0;
            staging_format_ = DXGI_FORMAT_UNKNOWN;
            context_.Reset();
            device_.Reset(); // wjy: 丢弃已失效的外层D3D设备，下一次Decode由ensure_device为本Viewer创建新设备代际。
            consecutive_corrupt_decode_errors_ = 0;
            waiting_for_keyframe_ = true;
            last_keyframe_request_ms_ = GetTickCount64();
            append_viewer_log("decoder device lost; rebuild current viewer device"); // wjy: 设备恢复与码流损坏分开记录，不把单窗口GPU故障扩散到整个进程。
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (!decodeResult.producedFrame()) {
            ++decode_errors_;
            if (decodeResult.status == lsp::DecodeStatus::CorruptBitstream) {
                ++consecutive_corrupt_decode_errors_; // wjy: 偶发损坏只丢当前输出；连续损坏才说明参考链需要关键帧恢复。
                if (consecutive_corrupt_decode_errors_ < kCorruptDecodeErrorsBeforeReset) {
                    if (consecutive_corrupt_decode_errors_ == 1) {
                        append_viewer_log("decoder corrupt frame dropped without reset"); // wjy: 只记录连续段首帧，避免损坏期间逐帧磁盘输出。
                    }
                    return WEBRTC_VIDEO_CODEC_OK;
                }
            } else {
                consecutive_corrupt_decode_errors_ = 0; // wjy: 致命资源或契约错误不参与码流损坏阈值，立即进入现有恢复路径。
            }
            decoder_.reset();
            decoder_ready_ = false;
            waiting_for_keyframe_ = true;
            last_keyframe_request_ms_ = GetTickCount64();
            if (decode_errors_ <= 3 || decode_errors_ % 30 == 0) {
                std::cerr << "uu-d3d11va-hevc decode failed status=" << static_cast<int>(decodeResult.status)
                          << "; reset and request keyframe: " << error << "\n";
            }
            append_viewer_log("decoder ffmpeg decode failed status="
                + std::to_string(static_cast<int>(decodeResult.status)) + " error=" + error); // wjy: 真实码流、设备或致命错误保留类别，显示背压不会进入这里。
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        consecutive_corrupt_decode_errors_ = 0; // wjy: 任意成功解码帧都结束损坏连续段，后续偶发错误重新从零计数。
        // =====wjy====
        struct SharedTextureReleaseGuard {
            lsp::H264Decoder* decoder = nullptr;
            lsp::DecodedFrame* frame = nullptr;
            bool released = false;

            ~SharedTextureReleaseGuard()
            {
                if (!released && decoder && frame) {
                    decoder->release_shared_texture(frame, false); // wjy: 任意异常或提前返回都把生产端持有的纹理归还key 0。
                }
            }

            void release(bool consumerAccepted)
            {
                if (!released && decoder && frame) {
                    decoder->release_shared_texture(frame, consumerAccepted); // wjy: GPU接管交给key 1，其余路径立即回到生产者key。
                    released = true;
                }
            }
        } sharedTextureGuard;
        sharedTextureGuard.decoder = &decoder_; // wjy: 显式绑定当前解码器，避免依赖局部守卫是否满足聚合初始化规则。
        sharedTextureGuard.frame = &decoded;
        // ===end====
        // append_viewer_log("decoder ffmpeg decode ok size=" + std::to_string(decoded.size.width)
        //     + "x" + std::to_string(decoded.size.height)
        //     + " bgra=" + std::to_string(decoded.bgra.size())); // wjy: per-frame decode-success log disabled.
        if (!decoded.size.valid()) {
            append_viewer_log("decoder invalid decoded size"); // wjy: guard invalid dimensions before allocation/conversion.
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        auto buffer = webrtc::I420Buffer::Create(static_cast<int>(decoded.size.width),
                                                 static_cast<int>(decoded.size.height));
        int texture_result = DecodedTextureFallback; // wjy: 使用编解码公共库内部三态，避免底层目标反向依赖上层FsRemoteStreamApi头文件。
        if (texture_callback_ && decoded.shared_handle) {
            texture_result = texture_callback_(
                static_cast<int>(decoded.size.width),
                static_cast<int>(decoded.size.height),
                decoded.shared_handle,
                decoded_frames_ + 1,
                encoded_mbps_);
        }

        if (texture_result != DecodedTextureFallback) {
            sharedTextureGuard.release(texture_result == DecodedTextureAccepted); // wjy: 丢弃帧直接回到生产者key，不锁死共享纹理槽。
            const int width = static_cast<int>(decoded.size.width);
            const int height = static_cast<int>(decoded.size.height);
            for (int y = 0; y < height; ++y) {
                std::memset(buffer->MutableDataY() + y * buffer->StrideY(), 16, static_cast<size_t>(width));
            }
            for (int y = 0; y < (height + 1) / 2; ++y) {
                std::memset(buffer->MutableDataU() + y * buffer->StrideU(), 128, static_cast<size_t>((width + 1) / 2));
                std::memset(buffer->MutableDataV() + y * buffer->StrideV(), 128, static_cast<size_t>((width + 1) / 2));
            }
        } else {
            std::vector<uint8_t> bgra;
            if (!decoded.bgra.empty()) {
                bgra = decoded.bgra;
            } else if (!copy_srv_to_bgra(decoded.srv.Get(), decoded.size.width, decoded.size.height, &bgra)) {
                std::cerr << "decoder copy_srv_to_bgra failed\n";
                append_viewer_log("decoder copy_srv_to_bgra failed"); // wjy: texture readback failed before Qt callback.
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            sharedTextureGuard.release(false); // wjy: GPU到CPU回读完成后归还纹理，Qt复制与I420转换不再占用共享槽。
            // append_viewer_log("decoder bgra ready size=" + std::to_string(bgra.size())); // wjy: per-frame BGRA-ready log disabled.
            {
                DecodedBgraCallback hook = bgra_callback_; // wjy: prefer this decoder's viewer callback, so tiled windows do not overwrite each other.
                if (!hook) {
                    std::lock_guard lock(g_decoded_bgra_hook_mutex);
                    hook = g_decoded_bgra_hook; // wjy: fallback keeps the old standalone viewer path working.
                }
                if (hook) {
                    // append_viewer_log("decoder bgra hook call"); // wjy: per-frame app-callback log disabled.
                    hook(static_cast<int>(decoded.size.width), static_cast<int>(decoded.size.height), bgra.data(), bgra.size(), encoded_mbps_);
                    // append_viewer_log("decoder bgra hook returned"); // wjy: per-frame app-callback log disabled.
                } else {
                    // append_viewer_log("decoder bgra hook missing"); // wjy: per-frame missing-hook log disabled.
                }
            }

            const int converted = libyuv::ARGBToI420(
                bgra.data(), static_cast<int>(decoded.size.width) * 4,
                buffer->MutableDataY(), buffer->StrideY(),
                buffer->MutableDataU(), buffer->StrideU(),
                buffer->MutableDataV(), buffer->StrideV(),
                static_cast<int>(decoded.size.width), static_cast<int>(decoded.size.height));
            if (converted != 0) {
                append_viewer_log("decoder ARGBToI420 failed code=" + std::to_string(converted)); // wjy: conversion failed after app BGRA callback.
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
        }

        auto frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(buffer)
            .set_timestamp_rtp(input_image.RtpTimestamp())
            .set_timestamp_ms(render_time_ms)
            .set_rotation(webrtc::kVideoRotation_0)
            .build();
        {
            std::function<void(const webrtc::VideoFrame&)> hook;
            {
                std::lock_guard lock(g_decoded_hook_mutex);
                hook = g_decoded_hook;
            }
            if (hook) {
                // append_viewer_log("decoder frame hook call"); // wjy: per-frame optional hook log disabled.
                hook(frame);
                // append_viewer_log("decoder frame hook returned"); // wjy: per-frame optional hook log disabled.
            }
        }
        // append_viewer_log("decoder callback Decoded call"); // wjy: per-frame WebRTC callback log disabled.
        callback_->Decoded(frame);
        // append_viewer_log("decoder callback Decoded returned"); // wjy: per-frame WebRTC callback log disabled.
        waiting_for_keyframe_ = false;
        ++decoded_frames_;
        if (decoded_frames_ == 1 || decoded_frames_ % 120 == 0) {
            std::cout << "uu-d3d11va-hevc decoded frames=" << decoded_frames_
                      << " size=" << decoded.size.width << "x" << decoded.size.height << "\n";
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override
    {
        // =====wjy====
        append_viewer_log(std::string("decoder RegisterDecodeCompleteCallback callback=") + (callback ? "set" : "null")); // wjy: confirm WebRTC installed the decoded-image callback.
        // ===end====
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override
    {
        // =====wjy====
        append_viewer_log("decoder Release begin"); // wjy: decoder teardown checkpoint.
        // ===end====
        decoder_.reset();
        callback_ = nullptr;
        decoder_ready_ = false;
        waiting_for_keyframe_ = true;
        append_viewer_log("decoder Release end"); // wjy: decoder teardown completed.
        return WEBRTC_VIDEO_CODEC_OK;
    }

    DecoderInfo GetDecoderInfo() const override
    {
        DecoderInfo info;
        info.implementation_name = "uu-d3d11va-hevc";
        info.is_hardware_accelerated = true;
        return info;
    }

private:
    void update_encoded_stats(size_t encoded_bytes)
    {
        const uint64_t now = GetTickCount64();
        if (encoded_stats_start_ms_ == 0) {
            encoded_stats_start_ms_ = now;
        }
        encoded_stats_bytes_ += encoded_bytes;
        const uint64_t elapsed_ms = now - encoded_stats_start_ms_;
        if (elapsed_ms >= 1000) {
            encoded_mbps_ = static_cast<double>(encoded_stats_bytes_) * 8.0 * 1000.0 / static_cast<double>(elapsed_ms) / 1000000.0;
            encoded_stats_bytes_ = 0;
            encoded_stats_start_ms_ = now;
        }
    }

    bool ensure_device()
    {
        if (device_ && context_) return true;
        D3D_FEATURE_LEVEL feature_level = {};
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        append_viewer_log("decoder D3D11CreateDevice begin"); // wjy: checkpoint before hardware device creation.
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                             levels, 2, D3D11_SDK_VERSION,
                                             &device_, &feature_level, &context_);
        char line[128] = {};
        std::snprintf(line, sizeof(line), "decoder D3D11CreateDevice hr=0x%08lX feature=0x%X",
                      static_cast<unsigned long>(hr), static_cast<unsigned int>(feature_level));
        append_viewer_log(line); // wjy: record HRESULT for GPU/device creation failures.
        return SUCCEEDED(hr);
    }

    bool copy_srv_to_bgra(ID3D11ShaderResourceView* srv, uint32_t width, uint32_t height, std::vector<uint8_t>* out)
    {
        if (!srv || !out || !device_ || !context_ || width == 0 || height == 0) return false;
        ComPtr<ID3D11Resource> resource;
        srv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture)) || !texture) return false;

        D3D11_TEXTURE2D_DESC src_desc = {};
        texture->GetDesc(&src_desc);
        if (!staging_ || staging_w_ != width || staging_h_ != height || staging_format_ != src_desc.Format) {
            staging_.Reset();
            D3D11_TEXTURE2D_DESC desc = src_desc;
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_))) return false;
            staging_w_ = width;
            staging_h_ = height;
            staging_format_ = src_desc.Format;
        }

        context_->CopyResource(staging_.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        out->resize(static_cast<size_t>(width) * height * 4);
        auto* dst = out->data();
        const auto* src = static_cast<const uint8_t*>(mapped.pData);
        const size_t row_bytes = static_cast<size_t>(width) * 4;
        for (uint32_t y = 0; y < height; ++y) {
            const auto* src_row = src + static_cast<size_t>(y) * mapped.RowPitch;
            auto* dst_row = dst + static_cast<size_t>(y) * row_bytes;
            // =====wjy====
            for (uint32_t x = 0; x < width; ++x) {
                const auto* src_pixel = src_row + static_cast<size_t>(x) * 4; // wjy: 解码视频处理器已经输出full-range BGRA，软件回退只需保持原值。
                auto* dst_pixel = dst_row + static_cast<size_t>(x) * 4;
                for (int channel = 0; channel < 3; ++channel) {
                    dst_pixel[channel] = src_pixel[channel]; // wjy: 禁止再次执行limited到full扩展，避免回退画面黑位压死、亮部过曝并与共享纹理路径不一致。
                }
                dst_pixel[3] = 255; // wjy: Remote desktop frames should be opaque when Qt wraps them as RGB32.
            }
            // ===end====
        }
        context_->Unmap(staging_.Get(), 0);
        return true;
    }

    webrtc::DecodedImageCallback* callback_ = nullptr;
    DecodedBgraCallback bgra_callback_;
    DecodedTextureCallback texture_callback_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> staging_;
    lsp::H264Decoder decoder_;
    bool decoder_ready_ = false;
    bool waiting_for_keyframe_ = true;
    uint64_t decode_calls_ = 0;
    uint64_t decoded_frames_ = 0;
    uint64_t decode_errors_ = 0;
    uint64_t output_texture_busy_drops_ = 0; // wjy: 独立记录共享输出槽繁忙丢帧，不能与码流损坏或网络丢包合并。
    static constexpr uint32_t kCorruptDecodeErrorsBeforeReset = 3; // wjy: 连续三次明确码流损坏才重置并请求关键帧，避免单包抖动造成恢复风暴。
    uint32_t consecutive_corrupt_decode_errors_ = 0;
    uint64_t last_keyframe_request_ms_ = 0;
    uint64_t encoded_stats_start_ms_ = 0;
    uint64_t encoded_stats_bytes_ = 0;
    double encoded_mbps_ = 0.0;
    uint32_t staging_w_ = 0;
    uint32_t staging_h_ = 0;
    DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
};

class UuVideoEncoderFactory final : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override
    {
        return {webrtc::SdpVideoFormat("H265")};
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                   std::optional<std::string> scalability_mode,
                                   std::optional<webrtc::Resolution>) const override
    {
        (void)scalability_mode;
        return codec_is(format, "H265")
            ? CodecSupport{.is_supported = true, .is_power_efficient = true}
            : CodecSupport{};
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override
    {
        (void)env;
        // =====wjy====
        append_viewer_log("encoder factory Create format=" + format.name); // wjy: record which encoder WebRTC requested.
        // ===end====
        if (codec_is(format, "H265")) {
            std::cout << "CreateUuVideoEncoderFactory: create H265 NVENC encoder\n";
            return std::make_unique<NvencHevcEncoder>();
        }
        return nullptr;
    }
};

class UuVideoDecoderFactory final : public webrtc::VideoDecoderFactory {
public:
    explicit UuVideoDecoderFactory(DecodedBgraCallback bgra_callback = {}, DecodedTextureCallback texture_callback = {})
        : bgra_callback_(std::move(bgra_callback)) // wjy: every NativeWebrtcRuntime gets its own decoder callback copy.
        , texture_callback_(std::move(texture_callback))
    {
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override
    {
        return {webrtc::SdpVideoFormat("H265")};
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                   bool,
                                   std::optional<webrtc::Resolution>) const override
    {
        return codec_is(format, "H265")
            ? CodecSupport{.is_supported = true, .is_power_efficient = true}
            : CodecSupport{};
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override
    {
        (void)env;
        // =====wjy====
        append_viewer_log("decoder factory Create format=" + format.name); // wjy: confirm WebRTC selected the custom H265 decoder.
        // ===end====
        if (codec_is(format, "H265")) {
            std::cout << "CreateUuVideoDecoderFactory: create H265 D3D11 decoder\n";
            return std::make_unique<HevcD3d11Decoder>(bgra_callback_, texture_callback_); // wjy: pass the viewer-owned callbacks into this decoder instance.
        }
        return nullptr;
    }

private:
    DecodedBgraCallback bgra_callback_;
    DecodedTextureCallback texture_callback_;
};

} // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> CreateUuVideoEncoderFactory()
{
    return std::make_unique<UuVideoEncoderFactory>();
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreateUuVideoDecoderFactory(DecodedBgraCallback bgra_callback, DecodedTextureCallback texture_callback)
{
    return std::make_unique<UuVideoDecoderFactory>(std::move(bgra_callback), std::move(texture_callback)); // wjy: create a decoder factory scoped to one runtime/viewer when provided.
}

void SetUuDecodedFrameHook(std::function<void(const webrtc::VideoFrame&)> hook)
{
    // =====wjy====
    append_viewer_log(std::string("decoder SetUuDecodedFrameHook ") + (hook ? "set" : "clear")); // wjy: track optional frame hook lifetime.
    // ===end====
    std::lock_guard lock(g_decoded_hook_mutex);
    g_decoded_hook = std::move(hook);
}

void SetUuDecodedBgraHook(DecodedBgraCallback hook)
{
    // =====wjy====
    append_viewer_log(std::string("decoder SetUuDecodedBgraHook ") + (hook ? "set" : "clear")); // wjy: track app BGRA hook lifetime.
    // ===end====
    std::lock_guard lock(g_decoded_bgra_hook_mutex);
    g_decoded_bgra_hook = std::move(hook);
}

} // namespace uu
