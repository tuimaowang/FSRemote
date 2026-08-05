#pragma once

#include <api/scoped_refptr.h>
#include <api/video/video_frame_buffer.h>
#include <common_video/include/video_frame_buffer_pool.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace uu {

// =====wjy====
class D3D11I420BufferPool final {
public:
    explicit D3D11I420BufferPool(std::size_t maxBuffers = 3); // wjy: 接收端软件消费者最多同时占用固定数量I420，禁止ToI420逐帧无界分配。
    webrtc::scoped_refptr<webrtc::I420Buffer> acquire(int width, int height) noexcept;

private:
    std::mutex mutex_; // wjy: 原生帧可能在WebRTC渲染线程并发请求映射，池操作必须串行。
    webrtc::VideoFrameBufferPool pool_; // wjy: 引用计数缓冲归还后复用相同内存，分辨率变化由WebRTC池自动清理旧尺寸。
};

class D3D11NativeFrameBuffer : public webrtc::VideoFrameBuffer {
public:
    D3D11NativeFrameBuffer(
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
        std::uint64_t generation); // wjy: 原生帧保存GPU资源、裁剪几何、代际和纹理租约，最后引用释放时自动归还有界槽位。

    Type type() const override;
    int width() const override;
    int height() const override;
    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> CropAndScale(
        int offsetX,
        int offsetY,
        int cropWidth,
        int cropHeight,
        int scaledWidth,
        int scaledHeight) override;
    std::string storage_representation() const override;

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11Texture2D* texture() const { return texture_.Get(); }
    int source_width() const { return sourceWidth_; }
    int source_height() const { return sourceHeight_; }
    int crop_x() const { return cropX_; }
    int crop_y() const { return cropY_; }
    int crop_width() const { return cropWidth_; }
    int crop_height() const { return cropHeight_; }
    std::uint64_t generation() const { return generation_; } // wjy: 接收端可识别帧所属解码设备代际；发送端默认代际为0。
    bool requires_transform() const;

protected:
    ~D3D11NativeFrameBuffer() override = default;

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_; // wjy: 编码器必须在同一D3D11设备上注册采集纹理，避免跨适配器CPU回读。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_; // wjy: Desktop Duplication复制得到的GPU纹理，硬件H265快路径直接消费。
    std::shared_ptr<void> textureLease_; // wjy: 最后一份帧引用释放时归还采集环槽位，防止后续CopyResource覆盖在编码帧。
    std::shared_ptr<D3D11I420BufferPool> i420Pool_; // wjy: 只有软件消费者真实请求像素时才从有界池取得I420内存。
    std::uint64_t generation_ = 0;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    int cropX_ = 0;
    int cropY_ = 0;
    int cropWidth_ = 0;
    int cropHeight_ = 0;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
};

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
    std::shared_ptr<D3D11I420BufferPool> i420Pool = {},
    std::uint64_t generation = 0);

D3D11NativeFrameBuffer* AsD3D11NativeFrameBuffer(webrtc::VideoFrameBuffer* buffer);
const D3D11NativeFrameBuffer* AsD3D11NativeFrameBuffer(const webrtc::VideoFrameBuffer* buffer);
// ===end====

} // namespace uu
