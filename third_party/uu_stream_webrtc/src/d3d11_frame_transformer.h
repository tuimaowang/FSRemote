#pragma once

#include "d3d11_native_frame_buffer.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace uu {

// =====wjy====
class D3D11FrameTransformer final {
public:
    bool transform(const D3D11NativeFrameBuffer& frame,
                   Microsoft::WRL::ComPtr<ID3D11Texture2D>* output,
                   uint64_t* transformTimeUs,
                   std::string* error); // wjy: 所有原生D3D11帧都经VideoProcessor裁剪/缩放并优先输出NV12，兼容旧代NVENC对DXGI BGRA纹理的限制。
    void reset();

private:
    bool ensure_resources(const D3D11NativeFrameBuffer& frame, std::string* error);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_texture_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view_;
    UINT source_width_ = 0;
    UINT source_height_ = 0;
    UINT output_width_ = 0;
    UINT output_height_ = 0;
    DXGI_FORMAT source_format_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT output_format_ = DXGI_FORMAT_UNKNOWN;
};
// ===end====

} // namespace uu
