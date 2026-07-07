#include "uu_codec_factory.h"

#include "ffmpeg_decoder.h"
#include "nvenc_h264_encoder.h"

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
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
    int InitEncode(const webrtc::VideoCodec* codec_settings,
                   const webrtc::VideoEncoder::Settings&) override
    {
        if (!codec_settings || codec_settings->width == 0 || codec_settings->height == 0) {
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        bitrate_kbps_ = std::max(10u, codec_settings->startBitrate ? codec_settings->startBitrate : 120000u);
        fps_ = std::max(1u, codec_settings->maxFramerate ? codec_settings->maxFramerate : 60u);
        target_size_ = {codec_settings->width, codec_settings->height};
        return ensure_device() ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
    }

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override
    {
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override
    {
        encoder_.shutdown();
        encoder_bitrate_kbps_ = 0; // wjy: Clear the active NVENC bitrate marker when the encoder session is released.
        texture_.Reset();
        argb_.clear();
        callback_ = nullptr;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Encode(const webrtc::VideoFrame& frame,
                   const std::vector<webrtc::VideoFrameType>* frame_types) override
    {
        if (!callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        auto i420 = frame.video_frame_buffer()->ToI420();
        if (!i420) return WEBRTC_VIDEO_CODEC_ERROR;

        const int width = i420->width();
        const int height = i420->height();
        if (width <= 0 || height <= 0) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        if (!ensure_device()) return WEBRTC_VIDEO_CODEC_ERROR;
        if (!ensure_texture(width, height)) return WEBRTC_VIDEO_CODEC_ERROR;
        if (encoder_.ready() && should_reconfigure_bitrate()) {
            encoder_.shutdown(); // wjy: WebRTC SetRates() changed the target bitrate; rebuild NVENC so the new adaptive rate takes effect.
            last_bitrate_reconfigure_ms_ = GetTickCount64(); // wjy: Throttle bitrate-driven rebuilds so small WebRTC rate changes do not cause repeated encoder stalls.
        }
        if (!encoder_.ready() || size_.width != static_cast<uint32_t>(width) || size_.height != static_cast<uint32_t>(height)) {
            std::string error;
            size_ = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
            if (!encoder_.initialize(device_.Get(), size_, bitrate_kbps_, fps_, &error)) {
                std::cerr << "NVENC init failed: " << error << "\n";
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            encoder_bitrate_kbps_ = bitrate_kbps_; // wjy: Remember which bitrate the active NVENC session was initialized with.
        }

        argb_.resize(static_cast<size_t>(width) * height * 4);
        const int converted = libyuv::I420ToARGB(
            i420->DataY(), i420->StrideY(),
            i420->DataU(), i420->StrideU(),
            i420->DataV(), i420->StrideV(),
            argb_.data(), width * 4,
            width, height);
        if (converted != 0) return WEBRTC_VIDEO_CODEC_ERROR;

        context_->UpdateSubresource(texture_.Get(), 0, nullptr, argb_.data(), width * 4, 0);

        bool force_keyframe = frame_id_ == 0;
        if (frame_types) {
            force_keyframe = force_keyframe || std::any_of(frame_types->begin(), frame_types->end(), [](webrtc::VideoFrameType type) {
                return type == webrtc::VideoFrameType::kVideoFrameKey;
            });
        }

        std::vector<uint8_t> encoded;
        bool keyframe = false;
        std::string error;
        if (!encoder_.encode(texture_.Get(), frame_id_++, force_keyframe, &encoded, &keyframe, &error)) {
            std::cerr << "NVENC encode failed: " << error << "\n";
            return WEBRTC_VIDEO_CODEC_ERROR;
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
        callback_->OnEncodedImage(image, &codec_info);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void SetRates(const RateControlParameters& parameters) override
    {
        const uint32_t bps = parameters.bitrate.get_sum_bps();
        if (bps > 0) bitrate_kbps_ = std::max(10u, bps / 1000u);
        if (parameters.framerate_fps > 0.0) fps_ = static_cast<uint32_t>(parameters.framerate_fps + 0.5);
    }

    EncoderInfo GetEncoderInfo() const override
    {
        EncoderInfo info;
        info.implementation_name = "uu-nvenc-hevc";
        info.is_hardware_accelerated = true;
        info.has_trusted_rate_controller = true;
        info.supports_native_handle = false;
        info.supports_simulcast = false;
        info.scaling_settings = ScalingSettings::kOff;
        return info;
    }

private:
    bool should_reconfigure_bitrate() const
    {
        if (encoder_bitrate_kbps_ == 0 || encoder_bitrate_kbps_ == bitrate_kbps_) {
            return false;
        }
        const uint32_t lower = std::min(encoder_bitrate_kbps_, bitrate_kbps_);
        const uint32_t upper = std::max(encoder_bitrate_kbps_, bitrate_kbps_);
        const bool changed_enough = (upper - lower) * 100 >= std::max(upper, 1u) * 15; // wjy: Ignore tiny target-rate jitter; only rebuild NVENC for meaningful adaptive bitrate movement.
        const bool cooled_down = GetTickCount64() - last_bitrate_reconfigure_ms_ >= 500; // wjy: Keep reconfiguration below roughly twice per second to protect remote-control smoothness.
        return changed_enough && cooled_down;
    }

    bool ensure_device()
    {
        if (device_ && context_) return true;
        D3D_FEATURE_LEVEL feature_level = {};
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                             levels, 2, D3D11_SDK_VERSION,
                                             &device_, &feature_level, &context_);
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
        if (FAILED(hr)) return false;
        tex_w_ = width;
        tex_h_ = height;
        encoder_.shutdown();
        encoder_bitrate_kbps_ = 0; // wjy: Force NVENC reinitialization after texture size changes.
        return true;
    }

    webrtc::EncodedImageCallback* callback_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> texture_;
    std::vector<uint8_t> argb_;
    lsp::NvencH264Encoder encoder_;
    lsp::Size size_;
    lsp::Size target_size_;
    uint32_t bitrate_kbps_ = 120000;
    uint32_t encoder_bitrate_kbps_ = 0;
    uint64_t last_bitrate_reconfigure_ms_ = 0;
    uint32_t fps_ = 60;
    uint32_t frame_id_ = 0;
    int tex_w_ = 0;
    int tex_h_ = 0;
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
        if (!decoder_.decode(data, &decoded, &error)) {
            decoder_.reset();
            decoder_ready_ = false;
            waiting_for_keyframe_ = true;
            last_keyframe_request_ms_ = GetTickCount64();
            if (++decode_errors_ <= 3 || decode_errors_ % 30 == 0) {
                std::cerr << "uu-d3d11va-hevc decode failed; reset and request keyframe: " << error << "\n";
            }
            append_viewer_log("decoder ffmpeg decode failed error=" + error); // wjy: compressed frame decode returned an error.
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        // append_viewer_log("decoder ffmpeg decode ok size=" + std::to_string(decoded.size.width)
        //     + "x" + std::to_string(decoded.size.height)
        //     + " bgra=" + std::to_string(decoded.bgra.size())); // wjy: per-frame decode-success log disabled.
        if (!decoded.size.valid()) {
            append_viewer_log("decoder invalid decoded size"); // wjy: guard invalid dimensions before allocation/conversion.
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        auto buffer = webrtc::I420Buffer::Create(static_cast<int>(decoded.size.width),
                                                 static_cast<int>(decoded.size.height));
        bool texture_presented = false;
        if (texture_callback_ && decoded.shared_handle) {
            texture_presented = texture_callback_(
                static_cast<int>(decoded.size.width),
                static_cast<int>(decoded.size.height),
                decoded.shared_handle,
                decoded_frames_ + 1,
                encoded_mbps_);
        }

        if (texture_presented) {
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
                const auto* src_pixel = src_row + static_cast<size_t>(x) * 4; // wjy: D3D11 output texture is BGRA.
                auto* dst_pixel = dst_row + static_cast<size_t>(x) * 4;
                for (int channel = 0; channel < 3; ++channel) {
                    const int value = src_pixel[channel];
                    const int expanded = (value - 16) * 255 / 219; // wjy: Restore video limited range 16-235 to desktop full range 0-255 while copying out of D3D11.
                    dst_pixel[channel] = static_cast<uint8_t>(std::clamp(expanded, 0, 255));
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
