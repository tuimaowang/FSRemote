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
std::function<void(int, int, const uint8_t*, size_t)> g_decoded_bgra_hook;

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
        bitrate_kbps_ = std::max(1000u, codec_settings->startBitrate ? codec_settings->startBitrate : 120000u);
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
        if (!encoder_.ready() || size_.width != static_cast<uint32_t>(width) || size_.height != static_cast<uint32_t>(height)) {
            std::string error;
            size_ = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
            if (!encoder_.initialize(device_.Get(), size_, bitrate_kbps_, fps_, &error)) {
                std::cerr << "NVENC init failed: " << error << "\n";
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
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
        if (bps > 0) bitrate_kbps_ = std::max(1000u, bps / 1000u);
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
    uint32_t fps_ = 60;
    uint32_t frame_id_ = 0;
    int tex_w_ = 0;
    int tex_h_ = 0;
};

class HevcD3d11Decoder final : public webrtc::VideoDecoder {
public:
    bool Configure(const Settings&) override
    {
        std::cout << "uu-d3d11va-hevc Configure\n";
        return ensure_device();
    }

    int32_t Decode(const webrtc::EncodedImage& input_image, int64_t render_time_ms) override
    {
        if (!callback_ || !input_image.data() || input_image.size() == 0) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        if (decode_calls_ == 0) {
            std::cout << "uu-d3d11va-hevc first Decode size=" << input_image.size()
                      << " rtp=" << input_image.RtpTimestamp() << "\n";
        }
        ++decode_calls_;
        if (!ensure_device()) return WEBRTC_VIDEO_CODEC_ERROR;
        std::vector<uint8_t> data(input_image.data(), input_image.data() + input_image.size());
        const bool encoded_keyframe = input_image.IsKey() || hevc_has_random_access_frame(data.data(), data.size());
        if (waiting_for_keyframe_ && !encoded_keyframe) {
            const uint64_t now = GetTickCount64();
            if (now - last_keyframe_request_ms_ >= 50) {
                last_keyframe_request_ms_ = now;
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            return WEBRTC_VIDEO_CODEC_OK;
        }
        if (encoded_keyframe && waiting_for_keyframe_) {
            decoder_.reset();
            decoder_ready_ = false;
        }
        if (!decoder_ready_) {
            std::string error;
            if (!decoder_.initialize_d3d11(device_.Get(), context_.Get(), &error)) {
                std::cerr << "decoder init failed: " << error << "\n";
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            decoder_ready_ = true;
        }

        lsp::DecodedFrame decoded;
        std::string error;
        if (!decoder_.decode(data, &decoded, &error)) {
            decoder_.reset();
            decoder_ready_ = false;
            waiting_for_keyframe_ = true;
            last_keyframe_request_ms_ = GetTickCount64();
            if (++decode_errors_ <= 3 || decode_errors_ % 30 == 0) {
                std::cerr << "uu-d3d11va-hevc decode failed; reset and request keyframe: " << error << "\n";
            }
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (!decoded.size.valid()) return WEBRTC_VIDEO_CODEC_ERROR;

        std::vector<uint8_t> bgra;
        if (!decoded.bgra.empty()) {
            bgra = decoded.bgra;
        } else if (!copy_srv_to_bgra(decoded.srv.Get(), decoded.size.width, decoded.size.height, &bgra)) {
            std::cerr << "decoder copy_srv_to_bgra failed\n";
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        {
            std::function<void(int, int, const uint8_t*, size_t)> hook;
            {
                std::lock_guard lock(g_decoded_bgra_hook_mutex);
                hook = g_decoded_bgra_hook;
            }
            if (hook) {
                hook(static_cast<int>(decoded.size.width), static_cast<int>(decoded.size.height), bgra.data(), bgra.size());
            }
        }

        auto buffer = webrtc::I420Buffer::Create(static_cast<int>(decoded.size.width),
                                                 static_cast<int>(decoded.size.height));
        const int converted = libyuv::ARGBToI420(
            bgra.data(), static_cast<int>(decoded.size.width) * 4,
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            static_cast<int>(decoded.size.width), static_cast<int>(decoded.size.height));
        if (converted != 0) return WEBRTC_VIDEO_CODEC_ERROR;

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
            if (hook) hook(frame);
        }
        callback_->Decoded(frame);
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
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override
    {
        decoder_.reset();
        callback_ = nullptr;
        decoder_ready_ = false;
        waiting_for_keyframe_ = true;
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
            std::memcpy(dst + static_cast<size_t>(y) * row_bytes,
                        src + static_cast<size_t>(y) * mapped.RowPitch,
                        row_bytes);
        }
        context_->Unmap(staging_.Get(), 0);
        return true;
    }

    webrtc::DecodedImageCallback* callback_ = nullptr;
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
        if (codec_is(format, "H265")) {
            std::cout << "CreateUuVideoEncoderFactory: create H265 NVENC encoder\n";
            return std::make_unique<NvencHevcEncoder>();
        }
        return nullptr;
    }
};

class UuVideoDecoderFactory final : public webrtc::VideoDecoderFactory {
public:
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
        if (codec_is(format, "H265")) {
            std::cout << "CreateUuVideoDecoderFactory: create H265 D3D11 decoder\n";
            return std::make_unique<HevcD3d11Decoder>();
        }
        return nullptr;
    }
};

} // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> CreateUuVideoEncoderFactory()
{
    return std::make_unique<UuVideoEncoderFactory>();
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreateUuVideoDecoderFactory()
{
    return std::make_unique<UuVideoDecoderFactory>();
}

void SetUuDecodedFrameHook(std::function<void(const webrtc::VideoFrame&)> hook)
{
    std::lock_guard lock(g_decoded_hook_mutex);
    g_decoded_hook = std::move(hook);
}

void SetUuDecodedBgraHook(std::function<void(int width, int height, const uint8_t* bgra, size_t size)> hook)
{
    std::lock_guard lock(g_decoded_bgra_hook_mutex);
    g_decoded_bgra_hook = std::move(hook);
}

} // namespace uu
