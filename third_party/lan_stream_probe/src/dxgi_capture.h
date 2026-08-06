#pragma once

#include "common.h"
#include "dxgi_capture_policy.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lsp {

struct CapturedFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Size size;
    std::shared_ptr<void> lifetime; // wjy: 生产链路用租约保持环形纹理槽位，帧释放前采集器不会覆盖其内容。
};

// =====wjy====
enum class DxgiCaptureStatus : uint8_t {
    FreshFrame,
    NoDesktopChange,
    FrameSlotBusy,
    DuplicationRecovering,
    DeviceRecovering,
    FatalError,
}; // wjy: 上层按类型区分正常静止、轻量恢复和设备恢复，不再依赖错误字符串决定是否清空画面。

struct DxgiCaptureResult {
    DxgiCaptureStatus status = DxgiCaptureStatus::FatalError;
    long result = S_OK;
}; // wjy: HRESULT 与恢复状态成对返回，诊断可以保留原始错误码而热路径无需构造字符串做分支。
// ===end====

class DxgiCapture {
public:
    bool initialize(std::string* error);
    bool initialize(const std::wstring& preferredDeviceName, std::string* error); // wjy: 正式远控按Parsec VDD的\\.\DISPLAYx设备名选择输出，不再默认抓主屏。
    bool capture(CapturedFrame* frame, std::string* error);
    DxgiCaptureResult capture_frame(CapturedFrame* frame, std::string* error); // wjy: 正式 Host 使用带类型结果的入口，恢复期可保留最后帧并暂停重复编码。
    void reset();

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    Size size() const { return size_; }

private:
    struct FrameSlot;

    bool recreate_frame_texture(const std::shared_ptr<FrameSlot>& slot,
                                const D3D11_TEXTURE2D_DESC& source_desc,
                                std::string* error);
    bool recreate_duplication(std::string* error, long* result); // wjy: 轻量重建同时返回原始 HRESULT，失败时可判断继续等待还是升级设备恢复。
    void schedule_duplication_recovery(long result); // wjy: 仅撤销失效 Duplication 并安排有上限的重试，不碰 D3D11 Device、帧槽和 NVENC。
    void reset_duplication();
    void reset_resources();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    std::vector<std::shared_ptr<FrameSlot>> frame_slots_; // wjy: 多槽纹理环允许采集与异步编码重叠，同时由每帧租约阻止提前复用。
    std::wstring preferred_device_name_;
    Size size_;
    DxgiRecoveryFrameGate recovery_frame_gate_; // wjy: 重建后隔离第一张预热帧，第二张真实新帧到达前始终保留控制端最后成功画面。
    uint32_t consecutive_duplication_failures_ = 0;
    std::chrono::steady_clock::time_point next_duplication_retry_{};
};

} // namespace lsp
