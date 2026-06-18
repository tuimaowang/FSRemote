#include "ffmpeg_decoder.h"

#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
}

namespace lsp {
namespace {

AVPixelFormat get_hw_format(AVCodecContext*, const AVPixelFormat* formats)
{
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

std::string fferr(const char* call, int ret)
{
    return std::string(call) + " failed: " + std::to_string(ret);
}

} // namespace

H264Decoder::~H264Decoder()
{
    shutdown();
}

bool H264Decoder::initialize_d3d11(ID3D11Device* device, ID3D11DeviceContext* context, std::string* error)
{
    shutdown();
    if (!device || !context) {
        if (error) *error = "invalid D3D11 device/context";
        return false;
    }
    device_ = device;
    context_ = context;
    device_->AddRef();
    context_->AddRef();

    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&video_device_))) ||
        FAILED(context_->QueryInterface(IID_PPV_ARGS(&video_context_)))) {
        if (error) *error = "D3D11 video interfaces are not available";
        shutdown();
        return false;
    }
    return ensure(error);
}

bool H264Decoder::decode(const std::vector<uint8_t>& h264, DecodedFrame* frame, std::string* error)
{
    if (h264.empty() || !ensure(error)) {
        return false;
    }
    av_packet_unref(packet_);
    packet_->data = const_cast<uint8_t*>(h264.data());
    packet_->size = static_cast<int>(h264.size());
    const int sent = avcodec_send_packet(codec_, packet_);
    if (sent == AVERROR(EAGAIN)) {
        return receive(frame, error);
    }
    if (sent < 0) {
        if (error) *error = fferr("avcodec_send_packet", sent);
        return false;
    }
    return receive(frame, error);
}

void H264Decoder::reset()
{
    shutdown();
}

bool H264Decoder::ensure(std::string* error)
{
    if (codec_) {
        return true;
    }
    if (!device_ || !context_) {
        if (error) *error = "D3D11 decoder was not initialized";
        return false;
    }

    AVBufferRef* device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!device_ref) {
        if (error) {
            std::string types;
            AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
            while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
                if (!types.empty()) types += ",";
                types += av_hwdevice_get_type_name(type);
            }
            *error = "av_hwdevice_ctx_alloc(D3D11VA) failed; available_hw_types=[" + types + "]";
        }
        return false;
    }
    auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
    auto* d3d11 = reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
    d3d11->device = device_;
    d3d11->device_context = context_;
    d3d11->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    device_->AddRef();
    context_->AddRef();

    const int hw_init = av_hwdevice_ctx_init(device_ref);
    if (hw_init < 0) {
        if (error) *error = fferr("av_hwdevice_ctx_init(D3D11VA)", hw_init);
        av_buffer_unref(&device_ref);
        return false;
    }
    hw_device_ = device_ref;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) {
        if (error) *error = "HEVC decoder not found";
        shutdown();
        return false;
    }
    bool supports_d3d11 = false;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            break;
        }
        if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
            config->pix_fmt == AV_PIX_FMT_D3D11 &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            supports_d3d11 = true;
            break;
        }
    }
    if (!supports_d3d11) {
        if (error) *error = "HEVC decoder has no D3D11VA hardware config";
        shutdown();
        return false;
    }
    codec_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!codec_ || !frame_ || !packet_) {
        if (error) *error = "FFmpeg decoder allocation failed";
        shutdown();
        return false;
    }

    codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_->thread_count = 1;
    codec_->get_format = get_hw_format;
    codec_->hw_device_ctx = av_buffer_ref(hw_device_);
    if (!codec_->hw_device_ctx) {
        if (error) *error = "av_buffer_ref(hw_device) failed";
        shutdown();
        return false;
    }

    const int opened = avcodec_open2(codec_, codec, nullptr);
    if (opened < 0) {
        if (error) *error = fferr("avcodec_open2(D3D11VA)", opened);
        shutdown();
        return false;
    }
    return true;
}

bool H264Decoder::receive(DecodedFrame* decoded, std::string* error)
{
    const int ret = avcodec_receive_frame(codec_, frame_);
    if (ret == AVERROR(EAGAIN)) {
        return false;
    }
    if (ret < 0) {
        if (error) *error = fferr("avcodec_receive_frame", ret);
        return false;
    }

    if (frame_->format != AV_PIX_FMT_D3D11) {
        if (error) *error = "decoder did not return a D3D11 frame";
        av_frame_unref(frame_);
        return false;
    }
    const bool ok = convert_d3d11_frame(decoded, error);
    av_frame_unref(frame_);
    return ok;
}

bool H264Decoder::convert_d3d11_frame(DecodedFrame* decoded, std::string* error)
{
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
    const UINT array_slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame_->data[1]));
    if (!texture) {
        if (error) *error = "D3D11 decoded texture is null";
        return false;
    }
    if (!ensure_video_processor(frame_->width, frame_->height, error)) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.ArraySlice = array_slice;
    input_desc.Texture2D.MipSlice = 0;
    HRESULT hr = video_device_->CreateVideoProcessorInputView(texture, processor_enum_.Get(), &input_desc, &input_view);
    if (FAILED(hr)) {
        if (error) *error = "CreateVideoProcessorInputView failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc = {};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_desc.Texture2D.MipSlice = 0;
    const int write_index = output_index_ ^ 1;
    hr = video_device_->CreateVideoProcessorOutputView(output_textures_[write_index].Get(), processor_enum_.Get(), &output_desc, &output_view);
    if (FAILED(hr)) {
        if (error) *error = "CreateVideoProcessorOutputView failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
        return false;
    }

    RECT rect = {0, 0, frame_->width, frame_->height};
    video_context_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &rect);
    video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &rect);
    video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    hr = video_context_->VideoProcessorBlt(processor_.Get(), output_view.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        if (error) *error = "VideoProcessorBlt failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
        return false;
    }

    decoded->size = {static_cast<uint32_t>(frame_->width), static_cast<uint32_t>(frame_->height)};
    decoded->bgra.clear();
    output_index_ = write_index;
    decoded->srv = output_srvs_[output_index_];
    return true;
}

bool H264Decoder::ensure_video_processor(int width, int height, std::string* error)
{
    if (processor_ && processor_width_ == width && processor_height_ == height) {
        return true;
    }

    processor_.Reset();
    processor_enum_.Reset();
    output_srvs_[0].Reset();
    output_srvs_[1].Reset();
    output_textures_[0].Reset();
    output_textures_[1].Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&content, &processor_enum_);
    if (FAILED(hr)) {
        if (error) *error = "CreateVideoProcessorEnumerator failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
        return false;
    }
    hr = video_device_->CreateVideoProcessor(processor_enum_.Get(), 0, &processor_);
    if (FAILED(hr)) {
        if (error) *error = "CreateVideoProcessor failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2; ++i) {
        hr = device_->CreateTexture2D(&desc, nullptr, &output_textures_[i]);
        if (FAILED(hr)) {
            if (error) *error = "CreateTexture2D(BGRA output) failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        hr = device_->CreateShaderResourceView(output_textures_[i].Get(), &srv_desc, &output_srvs_[i]);
        if (FAILED(hr)) {
            if (error) *error = "CreateShaderResourceView(BGRA output) failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
            return false;
        }
    }

    output_index_ = 0;
    processor_width_ = width;
    processor_height_ = height;
    return true;
}

void H264Decoder::shutdown()
{
    processor_.Reset();
    processor_enum_.Reset();
    output_srvs_[0].Reset();
    output_srvs_[1].Reset();
    output_textures_[0].Reset();
    output_textures_[1].Reset();
    output_index_ = 0;
    video_context_.Reset();
    video_device_.Reset();

    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&codec_);
    av_buffer_unref(&hw_device_);

    if (context_) {
        context_->Release();
        context_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    processor_width_ = 0;
    processor_height_ = 0;
}

} // namespace lsp
