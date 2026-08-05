#include "d3d11_native_frame_buffer.h"

#include <api/video/i420_buffer.h>
#include <libyuv/convert.h>
#include <rtc_base/ref_counted_object.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace uu {
namespace {

int clamp_dimension(int value, int maximum)
{
    return std::clamp(value, 1, std::max(1, maximum));
}

} // namespace

// =====wjy====
D3D11I420BufferPool::D3D11I420BufferPool(std::size_t maxBuffers)
    : pool_(false, std::max<std::size_t>(1, maxBuffers)) // wjy: 关闭无意义清零；ARGBToI420会完整覆盖每个有效平面。
{
}

webrtc::scoped_refptr<webrtc::I420Buffer> D3D11I420BufferPool::acquire(int width, int height) noexcept
{
    if (width <= 0 || height <= 0) return nullptr;
    try {
        std::lock_guard lock(mutex_); // wjy: WebRTC池自身要求串行调用，归还由引用计数自动完成。
        return pool_.CreateI420Buffer(width, height);
    } catch (...) {
        return nullptr; // wjy: 内存或池内部异常统一表现为有界池暂不可用，禁止异常穿过WebRTC视频线程边界。
    }
}

D3D11NativeFrameBuffer::D3D11NativeFrameBuffer(
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    std::shared_ptr<void> textureLease,
    int cropX,
    int cropY,
    int cropWidth,
    int cropHeight,
    int outputWidth,
    int outputHeight,
    std::shared_ptr<D3D11I420BufferPool> i420Pool,
    std::uint64_t generation)
    : device_(std::move(device))
    , texture_(std::move(texture))
    , textureLease_(std::move(textureLease))
    , i420Pool_(std::move(i420Pool))
    , generation_(generation)
{
    D3D11_TEXTURE2D_DESC desc = {};
    if (texture_) texture_->GetDesc(&desc); // wjy: 以真实纹理描述作为所有裁剪边界，拒绝调用方传入越界尺寸破坏GPU视图。
    sourceWidth_ = static_cast<int>(desc.Width);
    sourceHeight_ = static_cast<int>(desc.Height);
    cropX_ = std::clamp(cropX, 0, std::max(0, sourceWidth_ - 1));
    cropY_ = std::clamp(cropY, 0, std::max(0, sourceHeight_ - 1));
    cropWidth_ = clamp_dimension(cropWidth, sourceWidth_ - cropX_);
    cropHeight_ = clamp_dimension(cropHeight, sourceHeight_ - cropY_);
    outputWidth_ = std::max(1, outputWidth);
    outputHeight_ = std::max(1, outputHeight);
}

webrtc::VideoFrameBuffer::Type D3D11NativeFrameBuffer::type() const
{
    return Type::kNative; // wjy: 通知WebRTC自定义H265编码器可直接识别纹理，软件编码器则调用ToI420兼容映射。
}

int D3D11NativeFrameBuffer::width() const
{
    return outputWidth_;
}

int D3D11NativeFrameBuffer::height() const
{
    return outputHeight_;
}

bool D3D11NativeFrameBuffer::requires_transform() const
{
    return cropX_ != 0 || cropY_ != 0
        || cropWidth_ != sourceWidth_ || cropHeight_ != sourceHeight_
        || outputWidth_ != sourceWidth_ || outputHeight_ != sourceHeight_; // wjy: 原始分辨率高质量帧返回false，编码器即可完全跳过缩放纹理。
}

webrtc::scoped_refptr<webrtc::I420BufferInterface> D3D11NativeFrameBuffer::ToI420()
{
    if (!device_ || !texture_ || sourceWidth_ <= 0 || sourceHeight_ <= 0) return nullptr;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device_->GetImmediateContext(&context);
    if (!context) return nullptr;

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    texture_->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
        && sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
        && sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM
        && sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        return nullptr; // wjy: 软件回退只读取已知四通道格式；NV12等格式由对应映射实现后再开放，避免错误颜色解释。
    }

    auto sourceI420 = i420Pool_
        ? i420Pool_->acquire(sourceWidth_, sourceHeight_)
        : webrtc::I420Buffer::Create(sourceWidth_, sourceHeight_); // wjy: 先申请有界输出，池忙时不再执行无意义GPU复制和Map。
    if (!sourceI420) return nullptr;

    D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&stagingDesc, nullptr, &staging))) return nullptr;

    context->CopyResource(staging.Get(), texture_.Get()); // wjy: 只有软件编码或非原生消费者进入这里，高质量硬编快路径不会触发GPU回读。
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return nullptr;

    const bool rgbaOrder = sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
        || sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const int converted = rgbaOrder
        ? libyuv::ABGRToI420(
            static_cast<const uint8_t*>(mapped.pData), static_cast<int>(mapped.RowPitch),
            sourceI420->MutableDataY(), sourceI420->StrideY(),
            sourceI420->MutableDataU(), sourceI420->StrideU(),
            sourceI420->MutableDataV(), sourceI420->StrideV(),
            sourceWidth_, sourceHeight_)
        : libyuv::ARGBToI420(
            static_cast<const uint8_t*>(mapped.pData), static_cast<int>(mapped.RowPitch),
            sourceI420->MutableDataY(), sourceI420->StrideY(),
            sourceI420->MutableDataU(), sourceI420->StrideU(),
            sourceI420->MutableDataV(), sourceI420->StrideV(),
            sourceWidth_, sourceHeight_);
    context->Unmap(staging.Get(), 0);
    if (converted != 0) return nullptr;

    if (!requires_transform()) return sourceI420;
    auto scaled = sourceI420->CropAndScale(
        cropX_, cropY_, cropWidth_, cropHeight_, outputWidth_, outputHeight_); // wjy: 软件回退严格复用AdaptFrame确定的裁剪和档位尺寸，不改变用户选择的画面范围。
    return scaled ? scaled->ToI420() : nullptr;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> D3D11NativeFrameBuffer::CropAndScale(
    int offsetX,
    int offsetY,
    int cropWidth,
    int cropHeight,
    int scaledWidth,
    int scaledHeight)
{
    const int logicalWidth = std::max(1, outputWidth_);
    const int logicalHeight = std::max(1, outputHeight_);
    const int safeOffsetX = std::clamp(offsetX, 0, logicalWidth - 1);
    const int safeOffsetY = std::clamp(offsetY, 0, logicalHeight - 1);
    const int safeCropWidth = clamp_dimension(cropWidth, logicalWidth - safeOffsetX);
    const int safeCropHeight = clamp_dimension(cropHeight, logicalHeight - safeOffsetY);

    const int mappedX = cropX_ + static_cast<int>(static_cast<int64_t>(safeOffsetX) * cropWidth_ / logicalWidth);
    const int mappedY = cropY_ + static_cast<int>(static_cast<int64_t>(safeOffsetY) * cropHeight_ / logicalHeight);
    const int mappedWidth = clamp_dimension(
        static_cast<int>(static_cast<int64_t>(safeCropWidth) * cropWidth_ / logicalWidth),
        cropX_ + cropWidth_ - mappedX);
    const int mappedHeight = clamp_dimension(
        static_cast<int>(static_cast<int64_t>(safeCropHeight) * cropHeight_ / logicalHeight),
        cropY_ + cropHeight_ - mappedY);

    return CreateD3D11NativeFrameBuffer(
        device_, texture_, textureLease_, mappedX, mappedY, mappedWidth, mappedHeight,
        std::max(1, scaledWidth), std::max(1, scaledHeight), i420Pool_, generation_); // wjy: 派生视图共享纹理租约、I420池和设备代际，不发生复制或CPU转换。
}

std::string D3D11NativeFrameBuffer::storage_representation() const
{
    return "uu-d3d11-native-texture";
}

webrtc::scoped_refptr<D3D11NativeFrameBuffer> CreateD3D11NativeFrameBuffer(
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    std::shared_ptr<void> textureLease,
    int cropX,
    int cropY,
    int cropWidth,
    int cropHeight,
    int outputWidth,
    int outputHeight,
    std::shared_ptr<D3D11I420BufferPool> i420Pool,
    std::uint64_t generation)
{
    return webrtc::scoped_refptr<D3D11NativeFrameBuffer>(
        new webrtc::RefCountedObject<D3D11NativeFrameBuffer>(
            std::move(device), std::move(texture), std::move(textureLease),
            cropX, cropY, cropWidth, cropHeight, outputWidth, outputHeight,
            std::move(i420Pool), generation));
}

D3D11NativeFrameBuffer* AsD3D11NativeFrameBuffer(webrtc::VideoFrameBuffer* buffer)
{
    if (!buffer || buffer->type() != webrtc::VideoFrameBuffer::Type::kNative) return nullptr;
    return dynamic_cast<D3D11NativeFrameBuffer*>(buffer); // wjy: 仅接受本项目原生纹理类型，未知平台原生帧仍安全走ToI420兼容路径。
}

const D3D11NativeFrameBuffer* AsD3D11NativeFrameBuffer(const webrtc::VideoFrameBuffer* buffer)
{
    if (!buffer || buffer->type() != webrtc::VideoFrameBuffer::Type::kNative) return nullptr;
    return dynamic_cast<const D3D11NativeFrameBuffer*>(buffer);
}
// ===end====

} // namespace uu
