#include "d3d11_frame_transformer.h"

#include <algorithm>
#include <chrono>

namespace uu {

// =====wjy====
bool D3D11FrameTransformer::transform(
    const D3D11NativeFrameBuffer& frame,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>* output,
    uint64_t* transformTimeUs,
    std::string* error)
{
    if (!output || !frame.device() || !frame.texture()) {
        if (error) *error = "invalid native D3D11 frame";
        return false;
    }
    if (transformTimeUs) *transformTimeUs = 0;
    if (!frame.requires_transform()) {
        *output = frame.texture(); // wjy: 高质量原始分辨率直接把采集纹理交给NVENC，完全跳过额外GPU复制和颜色转换。
        return true;
    }
    if (!ensure_resources(frame, error)) return false;

    const auto started = std::chrono::steady_clock::now();
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
    input_desc.FourCC = 0;
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.MipSlice = 0;
    input_desc.Texture2D.ArraySlice = 0;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    HRESULT hr = video_device_->CreateVideoProcessorInputView(
        frame.texture(), enumerator_.Get(), &input_desc, &input_view);
    if (FAILED(hr)) {
        if (error) *error = "CreateVideoProcessorInputView failed";
        return false;
    }

    RECT source_rect = {
        frame.crop_x(), frame.crop_y(),
        frame.crop_x() + frame.crop_width(),
        frame.crop_y() + frame.crop_height(),
    };
    RECT output_rect = {0, 0, frame.width(), frame.height()};
    video_context_->VideoProcessorSetStreamFrameFormat(
        processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source_rect);
    video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &output_rect);
    video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &output_rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    hr = video_context_->VideoProcessorBlt(processor_.Get(), output_view_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        if (error) *error = "VideoProcessorBlt failed";
        return false; // wjy: 上层收到失败后立即调用ToI420兼容路径，不因单个驱动能力差异中断远控。
    }

    *output = output_texture_;
    if (transformTimeUs) {
        *transformTimeUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
    }
    return true;
}

bool D3D11FrameTransformer::ensure_resources(const D3D11NativeFrameBuffer& frame, std::string* error)
{
    D3D11_TEXTURE2D_DESC source_desc = {};
    frame.texture()->GetDesc(&source_desc);
    const bool reusable = device_.Get() == frame.device()
        && source_width_ == source_desc.Width
        && source_height_ == source_desc.Height
        && output_width_ == static_cast<UINT>(frame.width())
        && output_height_ == static_cast<UINT>(frame.height())
        && source_format_ == source_desc.Format
        && output_texture_ && output_view_ && processor_;
    if (reusable) return true;

    reset();
    device_ = frame.device();
    device_->GetImmediateContext(&context_);
    if (!context_ || FAILED(device_.As(&video_device_)) || FAILED(context_.As(&video_context_))) {
        if (error) *error = "D3D11 video processor interfaces are unavailable";
        reset();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = source_desc.Width;
    content.InputHeight = source_desc.Height;
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = static_cast<UINT>(frame.width());
    content.OutputHeight = static_cast<UINT>(frame.height());
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED; // wjy: 远控缩放优先帧时延和吞吐，画面质量主要由原始采集与高码率HEVC保证。
    HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&content, &enumerator_);
    if (FAILED(hr)) {
        if (error) *error = "CreateVideoProcessorEnumerator failed";
        reset();
        return false;
    }

    UINT input_flags = 0;
    if (FAILED(enumerator_->CheckVideoProcessorFormat(source_desc.Format, &input_flags))
        || (input_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
        if (error) *error = "capture texture format is not supported by D3D11 video processor";
        reset();
        return false;
    }

    output_format_ = (frame.width() % 2 == 0 && frame.height() % 2 == 0)
        ? DXGI_FORMAT_NV12
        : DXGI_FORMAT_B8G8R8A8_UNORM; // wjy: NV12色度平面要求偶数尺寸，异常奇数档位直接使用BGRA避免创建失败。
    UINT output_flags = 0;
    if (FAILED(enumerator_->CheckVideoProcessorFormat(output_format_, &output_flags))
        || (output_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        output_format_ = DXGI_FORMAT_B8G8R8A8_UNORM; // wjy: 驱动不支持NV12输出时仍在GPU内缩放BGRA，功能与色彩保持可用。
        output_flags = 0;
        if (FAILED(enumerator_->CheckVideoProcessorFormat(output_format_, &output_flags))
            || (output_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
            if (error) *error = "D3D11 video processor has no supported output format";
            reset();
            return false;
        }
    }

    if (FAILED(video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_))) {
        if (error) *error = "CreateVideoProcessor failed";
        reset();
        return false;
    }

    D3D11_TEXTURE2D_DESC output_desc = {};
    output_desc.Width = static_cast<UINT>(frame.width());
    output_desc.Height = static_cast<UINT>(frame.height());
    output_desc.MipLevels = 1;
    output_desc.ArraySize = 1;
    output_desc.Format = output_format_;
    output_desc.SampleDesc.Count = 1;
    output_desc.Usage = D3D11_USAGE_DEFAULT;
    output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = device_->CreateTexture2D(&output_desc, nullptr, &output_texture_);
    if (FAILED(hr) && output_format_ == DXGI_FORMAT_NV12) {
        output_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
        output_flags = 0;
        if (FAILED(enumerator_->CheckVideoProcessorFormat(output_format_, &output_flags))
            || (output_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
            if (error) *error = "D3D11 video processor BGRA fallback is unsupported";
            reset();
            return false;
        }
        output_desc.Format = output_format_;
        hr = device_->CreateTexture2D(&output_desc, nullptr, &output_texture_); // wjy: 部分旧驱动宣称支持NV12但拒绝创建目标纹理，自动退回GPU BGRA而不是退回CPU。
    }
    if (FAILED(hr)) {
        if (error) *error = "Create D3D11 transform output texture failed";
        reset();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc = {};
    view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipSlice = 0;
    hr = video_device_->CreateVideoProcessorOutputView(
        output_texture_.Get(), enumerator_.Get(), &view_desc, &output_view_);
    if (FAILED(hr) && output_format_ == DXGI_FORMAT_NV12) {
        output_view_.Reset();
        output_texture_.Reset();
        output_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
        output_flags = 0;
        if (SUCCEEDED(enumerator_->CheckVideoProcessorFormat(output_format_, &output_flags))
            && (output_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) != 0) {
            output_desc.Format = output_format_;
            hr = device_->CreateTexture2D(&output_desc, nullptr, &output_texture_);
            if (SUCCEEDED(hr)) {
                hr = video_device_->CreateVideoProcessorOutputView(
                    output_texture_.Get(), enumerator_.Get(), &view_desc, &output_view_); // wjy: NV12输出视图创建失败时再尝试GPU BGRA，尽量不触发CPU回退。
            }
        }
    }
    if (FAILED(hr)) {
        if (error) *error = "CreateVideoProcessorOutputView failed";
        reset();
        return false;
    }

    source_width_ = source_desc.Width;
    source_height_ = source_desc.Height;
    output_width_ = static_cast<UINT>(frame.width());
    output_height_ = static_cast<UINT>(frame.height());
    source_format_ = source_desc.Format;
    return true;
}

void D3D11FrameTransformer::reset()
{
    output_view_.Reset();
    output_texture_.Reset();
    processor_.Reset();
    enumerator_.Reset();
    video_context_.Reset();
    video_device_.Reset();
    context_.Reset();
    device_.Reset();
    source_width_ = source_height_ = output_width_ = output_height_ = 0;
    source_format_ = output_format_ = DXGI_FORMAT_UNKNOWN;
}
// ===end====

} // namespace uu
