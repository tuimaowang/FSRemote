#pragma once

#include "common.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

namespace lsp {

struct CapturedFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Size size;
    std::shared_ptr<void> lifetime; // wjy: 生产链路用租约保持环形纹理槽位，帧释放前采集器不会覆盖其内容。
};

class DxgiCapture {
public:
    bool initialize(std::string* error);
    bool initialize(const std::wstring& preferredDeviceName, std::string* error); // wjy: 正式远控按Parsec VDD的\\.\DISPLAYx设备名选择输出，不再默认抓主屏。
    bool capture(CapturedFrame* frame, std::string* error);
    void reset();

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    Size size() const { return size_; }

private:
    struct FrameSlot;

    bool recreate_frame_texture(const std::shared_ptr<FrameSlot>& slot,
                                const D3D11_TEXTURE2D_DESC& source_desc,
                                std::string* error);
    void reset_resources();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    std::vector<std::shared_ptr<FrameSlot>> frame_slots_; // wjy: 多槽纹理环允许采集与异步编码重叠，同时由每帧租约阻止提前复用。
    std::wstring preferred_device_name_;
    Size size_;
};

} // namespace lsp
