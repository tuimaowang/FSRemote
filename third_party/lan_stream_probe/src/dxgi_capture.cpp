#include "dxgi_capture.h"
#include "dxgi_capture_policy.h"
#include "stream_capture_diagnostics.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <d3d11_4.h>
#include <sstream>

namespace lsp {

namespace {

// =====wjy====
std::string utf8_from_wide(const wchar_t* value)
{
    if (!value || !*value) {
        return {};
    }
    const int character_count = static_cast<int>(std::wcslen(value));
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, value, character_count, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value, character_count, result.data(), size, nullptr, nullptr);
    return result; // wjy: DXGI设备名和显卡描述转成UTF-8后写入统一诊断文件，中文设备名也不会依赖系统代码页。
}

void log_dxgi_failure(const std::string& stage, const std::string& error)
{
    append_stream_capture_diagnostic_log_rate_limited(
        "dxgi",
        stage + " failed error=" + error,
        1000); // wjy: 初始化失败可能被60 FPS循环重复触发，同一阶段每秒最多写一条。
}
// ===end====

} // namespace

// =====wjy====
struct DxgiCapture::FrameSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    std::shared_ptr<FrameSlotLeaseState> lease = std::make_shared<FrameSlotLeaseState>(); // wjy: 纹理与纯策略租约分离，既能安全复用也能在无显卡测试中验证生命周期。
};
// ===end====

std::string win32_error(const char* what, long hr)
{
    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "%s failed: 0x%08lx", what, static_cast<unsigned long>(hr));
    return buffer;
}

bool DxgiCapture::initialize(std::string* error)
{
    return initialize(preferred_device_name_, error);
}

// =====wjy====
bool DxgiCapture::initialize(const std::wstring& preferredDeviceName, std::string* error)
{
    preferred_device_name_ = preferredDeviceName;
    reset_resources(); // wjy: 重建DXGI设备和duplication时保留目标输出身份，ACCESS_LOST后仍回到同一Parsec屏幕。
    append_stream_capture_diagnostic_log_rate_limited(
        "dxgi",
        "initialize begin preferred_device='" + utf8_from_wide(preferred_device_name_.c_str()) + "'",
        1000); // wjy: 每次首次初始化或ACCESS_LOST重建都记录目标设备名，确认是否仍指向同一个Parsec输出。

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) {
        if (error) *error = win32_error("CreateDXGIFactory1", hr);
        log_dxgi_failure("CreateDXGIFactory1", error ? *error : win32_error("CreateDXGIFactory1", hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> selected_adapter;
    UINT selected_output = 0;
    LONG best_area = -1;
    bool found_preferred = false;
    bool found_primary = false;

    for (UINT adapter_index = 0;; ++adapter_index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 adapter_desc = {};
        adapter->GetDesc1(&adapter_desc);
        append_stream_capture_diagnostic_log_rate_limited(
            "dxgi",
            "adapter index=" + std::to_string(adapter_index)
                + " description='" + utf8_from_wide(adapter_desc.Description) + "'",
            1000); // wjy: 输出枚举顺序和显卡名称，定位虚拟显卡或外部远控改变适配器拓扑后的选择差异。

        for (UINT output_index = 0;; ++output_index) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC desc = {};
            output->GetDesc(&desc);
            {
                std::ostringstream output_line;
                output_line << "output adapter=" << adapter_index
                            << " index=" << output_index
                            << " device='" << utf8_from_wide(desc.DeviceName) << "'"
                            << " attached=" << (desc.AttachedToDesktop ? 1 : 0)
                            << " rect=" << desc.DesktopCoordinates.left << "," << desc.DesktopCoordinates.top
                            << "-" << desc.DesktopCoordinates.right << "," << desc.DesktopCoordinates.bottom;
                append_stream_capture_diagnostic_log_rate_limited(
                    "dxgi",
                    output_line.str(),
                    1000); // wjy: 每个输出记录设备名、挂载状态和坐标，直接判断ToDesk断开后Parsec屏是否仍被DXGI看见。
            }
            if (!desc.AttachedToDesktop) {
                continue;
            }
            if (dxgi_device_name_matches(preferred_device_name_, desc.DeviceName)) {
                selected_adapter = adapter;
                selected_output = output_index;
                found_preferred = true; // wjy: 精确匹配ParsecVddSession发布的Windows显示设备名，禁止误抓物理主屏。
                break;
            }
            if (!preferred_device_name_.empty()) {
                continue;
            }
            const LONG width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            const LONG height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            const LONG area = width * height;
            const bool primary = desc.DesktopCoordinates.left <= 0 && desc.DesktopCoordinates.top <= 0
                && desc.DesktopCoordinates.right > 0 && desc.DesktopCoordinates.bottom > 0;
            if (primary || area > best_area) {
                selected_adapter = adapter;
                selected_output = output_index;
                best_area = area;
                if (primary) {
                    found_primary = true;
                    break;
                }
            }
        }
        if (found_preferred || found_primary) {
            break;
        }
    }

    if (!preferred_device_name_.empty() && !found_preferred) {
        if (error) *error = "requested DXGI output is not attached";
        log_dxgi_failure("select-output", error ? *error : "requested DXGI output is not attached");
        return false; // wjy: 指定VDD不存在时交给上层CPU DesktopCapturer回退，绝不静默泄露另一块显示器画面。
    }
    if (!selected_adapter) {
        if (error) *error = "no attached DXGI output found";
        log_dxgi_failure("select-output", error ? *error : "no attached DXGI output found");
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL created_level = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(selected_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                           &device_, &created_level, &context_);
    if (FAILED(hr)) {
        if (error) *error = win32_error("D3D11CreateDevice", hr);
        log_dxgi_failure("D3D11CreateDevice", error ? *error : win32_error("D3D11CreateDevice", hr));
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE); // wjy: 采集线程CopyResource与编码线程VideoProcessor共享同一立即上下文，开启驱动级串行保护避免并发未定义行为。
    }

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    hr = selected_adapter->EnumOutputs(selected_output, &output);
    if (FAILED(hr)) {
        if (error) *error = win32_error("EnumOutputs", hr);
        log_dxgi_failure("EnumOutputs", error ? *error : win32_error("EnumOutputs", hr));
        return false;
    }

    DXGI_OUTPUT_DESC output_desc = {};
    output->GetDesc(&output_desc);
    size_.width = static_cast<uint32_t>(output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left);
    size_.height = static_cast<uint32_t>(output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        if (error) *error = win32_error("Query IDXGIOutput1", hr);
        log_dxgi_failure("Query IDXGIOutput1", error ? *error : win32_error("Query IDXGIOutput1", hr));
        return false;
    }

    hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        if (error) *error = win32_error("DuplicateOutput", hr);
        log_dxgi_failure("DuplicateOutput", error ? *error : win32_error("DuplicateOutput", hr));
        return false;
    }
    frame_slots_.reserve(4);
    for (int index = 0; index < 4; ++index) {
        frame_slots_.push_back(std::make_shared<FrameSlot>()); // wjy: 四槽覆盖采集、WebRTC排队和NVENC工作帧，忙时仍保持最新帧优先。
    }
    append_stream_capture_diagnostic_log(
        "dxgi",
        "initialize success device='" + utf8_from_wide(output_desc.DeviceName)
            + "' size=" + std::to_string(size_.width) + "x" + std::to_string(size_.height)
            + " feature_level=" + std::to_string(static_cast<unsigned int>(created_level))); // wjy: DuplicateOutput成功边界立即落盘，后续无首帧时可排除输出创建阶段。
    return true;
}
// ===end====

bool DxgiCapture::capture(CapturedFrame* frame, std::string* error)
{
    if (!frame) {
        if (error) *error = "invalid captured-frame output";
        log_dxgi_failure("capture-argument", error ? *error : "invalid captured-frame output");
        return false;
    }
    if (!duplication_ && !initialize(error)) {
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO info = {};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (error) *error = "timeout";
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        reset_resources(); // wjy: 显示模式切换或VDD重建后丢弃全部失效COM对象，下次采集按原设备名重新初始化。
        if (error) *error = "DXGI access lost";
        append_stream_capture_diagnostic_log_rate_limited(
            "dxgi",
            "AcquireNextFrame access-lost; resources reset and next capture will reinitialize",
            500); // wjy: 显示拓扑变化的核心信号单独记录，验证ToDesk断开是否正好触发DXGI失效。
        return false;
    }
    if (FAILED(hr)) {
        if (error) *error = win32_error("AcquireNextFrame", hr);
        log_dxgi_failure("AcquireNextFrame", error ? *error : win32_error("AcquireNextFrame", hr));
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_texture;
    hr = resource.As(&desktop_texture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        if (error) *error = win32_error("Query texture", hr);
        log_dxgi_failure("Query texture", error ? *error : win32_error("Query texture", hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desktop_texture->GetDesc(&desc);
    std::shared_ptr<FrameSlot> selected_slot;
    std::shared_ptr<void> selected_lease;
    for (const auto& slot : frame_slots_) {
        if (slot && slot->lease && slot->lease->try_acquire(&selected_lease)) {
            selected_slot = slot;
            break;
        }
    }
    if (!selected_slot) {
        duplication_->ReleaseFrame();
        if (error) *error = "busy";
        append_stream_capture_diagnostic_log_rate_limited(
            "dxgi",
            "all four frame slots are leased; newest desktop frame dropped",
            2000); // wjy: 编码端长期不归还纹理时每两秒记录一次，区分采集失败和下游堵塞。
        return false; // wjy: 所有纹理仍被编码器引用时丢弃新图像，由上层复用最后安全帧，避免覆盖导致闪屏。
    }
    if (!recreate_frame_texture(selected_slot, desc, error)) {
        selected_lease.reset();
        duplication_->ReleaseFrame();
        return false;
    }

    context_->CopyResource(selected_slot->texture.Get(), desktop_texture.Get()); // wjy: GPU内复制Desktop Duplication纹理，不回读CPU内存。
    duplication_->ReleaseFrame();

    frame->texture = selected_slot->texture;
    frame->size = {desc.Width, desc.Height};
    frame->lifetime = std::move(selected_lease); // wjy: 最后一份帧租约销毁时才归还槽位，支持多PeerConnection并发持有。
    return true;
}

bool DxgiCapture::recreate_frame_texture(const std::shared_ptr<FrameSlot>& slot,
                                         const D3D11_TEXTURE2D_DESC& source_desc,
                                         std::string* error)
{
    if (!slot) return false;
    if (slot->texture) {
        D3D11_TEXTURE2D_DESC current = {};
        slot->texture->GetDesc(&current);
        if (current.Width == source_desc.Width && current.Height == source_desc.Height
            && current.Format == source_desc.Format) {
            return true;
        }
        slot->texture.Reset();
    }

    D3D11_TEXTURE2D_DESC desc = source_desc;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &slot->texture);
    if (FAILED(hr)) {
        if (error) *error = win32_error("CreateTexture2D", hr);
        log_dxgi_failure("CreateTexture2D", error ? *error : win32_error("CreateTexture2D", hr));
        return false;
    }
    return true;
}

void DxgiCapture::reset()
{
    reset_resources();
    preferred_device_name_.clear();
}

void DxgiCapture::reset_resources()
{
    if (duplication_ || device_ || context_ || !frame_slots_.empty()) {
        append_stream_capture_diagnostic_log(
            "dxgi",
            "reset resources preferred_device='" + utf8_from_wide(preferred_device_name_.c_str()) + "'"); // wjy: 明确记录主动停止和ACCESS_LOST的资源释放边界，判断旧COM对象是否真正被丢弃。
    }
    frame_slots_.clear(); // wjy: 已交付帧通过shared_ptr继续持有自己的槽和纹理，清空采集器不会制造悬空COM指针。
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
    size_ = {};
}

} // namespace lsp
