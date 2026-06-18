#pragma once

#include "common.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>

namespace lsp {

struct CapturedFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Size size;
};

class DxgiCapture {
public:
    bool initialize(std::string* error);
    bool capture(CapturedFrame* frame, std::string* error);
    void reset();

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    Size size() const { return size_; }

private:
    bool recreate_frame_texture(const D3D11_TEXTURE2D_DESC& source_desc, std::string* error);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frame_texture_;
    Size size_;
};

} // namespace lsp
