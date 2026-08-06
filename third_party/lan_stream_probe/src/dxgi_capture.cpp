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
    reset_resources(); // wjy: 首次初始化或设备级恢复前释放旧设备资源，同时保留目标输出身份；普通 ACCESS_LOST 不再进入这里。
    append_stream_capture_diagnostic_log_rate_limited(
        "dxgi",
        "initialize begin preferred_device='" + utf8_from_wide(preferred_device_name_.c_str()) + "'",
        1000); // wjy: 首次初始化或设备级恢复记录目标设备名，确认完整恢复后仍指向同一个 Parsec 输出。

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
    recovery_frame_gate_.begin(); // wjy: 初次 Duplication 也先隔离一张驱动预热帧，避免打开远控窗口时把初始化黑帧编码出去。
    consecutive_duplication_failures_ = 0; // wjy: 新设备代际初始化成功后清除旧 Duplication 失败次数，避免首次异常被立即升级为再次完整重建。
    next_duplication_retry_ = {};
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
    return capture_frame(frame, error).status == DxgiCaptureStatus::FreshFrame; // wjy: 保留旧布尔接口给独立探针，正式 Host 使用下方类型化结果控制恢复期行为。
}

// =====wjy====
DxgiCaptureResult DxgiCapture::capture_frame(CapturedFrame* frame, std::string* error)
{
    if (!frame) {
        if (error) *error = "invalid captured-frame output";
        log_dxgi_failure("capture-argument", error ? *error : "invalid captured-frame output");
        return {DxgiCaptureStatus::FatalError, E_INVALIDARG};
    }

    if (!duplication_) {
        const auto now = std::chrono::steady_clock::now();
        if (next_duplication_retry_.time_since_epoch().count() > 0 && now < next_duplication_retry_) {
            if (error) *error = "DXGI duplication recovery pending";
            return {device_ ? DxgiCaptureStatus::DuplicationRecovering : DxgiCaptureStatus::DeviceRecovering, S_FALSE}; // wjy: 退避窗口只返回状态，不重复枚举输出或创建设备。
        }

        if (device_ && context_ && !frame_slots_.empty()) {
            long recreate_result = S_OK;
            if (!recreate_duplication(error, &recreate_result)) {
                const long removal_reason = device_->GetDeviceRemovedReason();
                if (dxgi_failure_action(recreate_result, removal_reason) == DxgiFailureAction::RecreateDevice) {
                    const long recovery_result = dxgi_result_is_device_lost(recreate_result)
                        ? recreate_result
                        : removal_reason; // wjy: 轻量重建的表层错误可能不是设备错误，返回真正的设备移除原因供诊断定位。
                    reset_resources(); // wjy: 只有轻量重建确认设备代际失效时才释放 D3D11 Device 和帧槽。
                    next_duplication_retry_ = now + std::chrono::milliseconds(250);
                    return {DxgiCaptureStatus::DeviceRecovering, recovery_result};
                }
                schedule_duplication_recovery(recreate_result); // wjy: 输出暂时未挂载时保留原设备和最后帧，按有界退避继续等待同名输出。
                return {DxgiCaptureStatus::DuplicationRecovering, recreate_result};
            }
        } else if (!initialize(error)) {
            next_duplication_retry_ = now + std::chrono::milliseconds(250); // wjy: 完整设备恢复失败也限制到每秒最多四次，避免初始化循环占满 CPU。
            return {DxgiCaptureStatus::DeviceRecovering, E_FAIL};
        }
    }

    DXGI_OUTDUPL_FRAME_INFO info = {};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (error) *error = "timeout";
        return {recovery_frame_gate_.awaitingFrame() ? DxgiCaptureStatus::DuplicationRecovering
                                                     : DxgiCaptureStatus::NoDesktopChange,
                hr}; // wjy: 重建后的首帧尚未到达时暂停编码；健康静止桌面仍沿用原来的最后帧策略。
    }
    if (FAILED(hr)) {
        const long removal_reason = device_ ? device_->GetDeviceRemovedReason() : E_FAIL; // wjy: 仅异常路径查询设备状态，正常新帧路径不增加任何额外 COM 调用。
        const DxgiFailureAction failure_action = dxgi_failure_action(hr, removal_reason);
        if (failure_action == DxgiFailureAction::RecreateDuplication) {
            schedule_duplication_recovery(hr); // wjy: 设备健康时 ACCESS_LOST 与 INVALID_CALL 只撤销 Duplication，同一 D3D11 Device 让 NVENC 会话保持不变。
            if (error) *error = hr == DXGI_ERROR_ACCESS_LOST
                ? "DXGI access lost"
                : win32_error("AcquireNextFrame", hr);
            append_stream_capture_diagnostic_log_rate_limited(
                "dxgi",
                "AcquireNextFrame lightweight recovery hr=" + std::to_string(static_cast<unsigned long>(hr))
                    + " failures=" + std::to_string(consecutive_duplication_failures_),
                500); // wjy: 一条低频事件同时覆盖拓扑丢失和无效调用，现场可确认是否避免了设备与编码器重建。
            if (hr == DXGI_ERROR_INVALID_CALL && consecutive_duplication_failures_ >= 3) {
                reset_resources(); // wjy: 新 Duplication 仍连续无效时升级一次完整恢复，避免永久停留在坏状态。
                next_duplication_retry_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                return {DxgiCaptureStatus::DeviceRecovering, hr};
            }
            return {DxgiCaptureStatus::DuplicationRecovering, hr};
        }
        if (failure_action == DxgiFailureAction::RecreateDevice) {
            const long recovery_result = dxgi_result_is_device_lost(hr)
                ? hr
                : removal_reason; // wjy: Acquire 表层错误与设备移除原因不同时，优先向上返回真实设备级 HRESULT。
            reset_resources(); // wjy: Device Removed/Reset/Hung 才销毁采集设备，旧 CapturedFrame 仍由自己的 COM 引用安全保留。
            recovery_frame_gate_.begin(); // wjy: 完整设备恢复同样隔离第一张驱动预热帧，禁止设备重建黑帧进入码流。
            next_duplication_retry_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            if (error) *error = win32_error("AcquireNextFrame device recovery", recovery_result);
            return {DxgiCaptureStatus::DeviceRecovering, recovery_result};
        }
        if (error) *error = win32_error("AcquireNextFrame", hr);
        log_dxgi_failure("AcquireNextFrame", error ? *error : win32_error("AcquireNextFrame", hr));
        return {DxgiCaptureStatus::FatalError, hr};
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_texture;
    hr = resource.As(&desktop_texture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        if (error) *error = win32_error("Query texture", hr);
        log_dxgi_failure("Query texture", error ? *error : win32_error("Query texture", hr));
        return {DxgiCaptureStatus::FatalError, hr};
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desktop_texture->GetDesc(&desc);
    // =====wjy====
    if (recovery_frame_gate_.discardWarmupFrame()) {
        duplication_->ReleaseFrame(); // wjy: 第一张恢复帧不复制、不租用帧槽也不编码，常态热路径没有新增 GPU 开销。
        append_stream_capture_diagnostic_log_rate_limited(
            "dxgi",
            "recovery warmup frame discarded; retained last good frame size="
                + std::to_string(desc.Width) + "x" + std::to_string(desc.Height),
            500); // wjy: 日志落在 data/stream_capture_debug.log，现场可确认黑帧已在被控端被拦截。
        return {DxgiCaptureStatus::DuplicationRecovering, S_FALSE}; // wjy: 上层继续保留最后正常纹理，等待下一张真实新帧结束恢复。
    }
    // ===end====
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
        return {DxgiCaptureStatus::FrameSlotBusy, DXGI_ERROR_WAS_STILL_DRAWING}; // wjy: 所有纹理仍被编码器引用时由上层复用最后安全帧，避免覆盖导致闪屏。
    }
    if (!recreate_frame_texture(selected_slot, desc, error)) {
        selected_lease.reset();
        duplication_->ReleaseFrame();
        return {DxgiCaptureStatus::FatalError, E_FAIL};
    }

    context_->CopyResource(selected_slot->texture.Get(), desktop_texture.Get()); // wjy: GPU内复制Desktop Duplication纹理，不回读CPU内存。
    duplication_->ReleaseFrame();

    frame->texture = selected_slot->texture;
    frame->size = {desc.Width, desc.Height};
    frame->lifetime = std::move(selected_lease); // wjy: 最后一份帧租约销毁时才归还槽位，支持多PeerConnection并发持有。
    recovery_frame_gate_.complete(); // wjy: 预热帧已隔离，第二张成功复制并交付的帧才正式结束恢复态。
    consecutive_duplication_failures_ = 0;
    next_duplication_retry_ = {};
    return {DxgiCaptureStatus::FreshFrame, S_OK};
}
// ===end====

// =====wjy====
bool DxgiCapture::recreate_duplication(std::string* error, long* result)
{
    if (result) *result = E_FAIL;
    if (!device_) {
        if (error) *error = "D3D11 device is not available for duplication recovery";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = device_.As(&dxgi_device);
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter) {
        if (result) *result = hr;
        if (error) *error = win32_error("Get capture adapter", hr);
        return false; // wjy: 原设备无法取得适配器时交给上层升级完整设备恢复。
    }

    Microsoft::WRL::ComPtr<IDXGIOutput> selected_output;
    DXGI_OUTPUT_DESC selected_desc = {};
    LONG best_area = -1;
    for (UINT output_index = 0;; ++output_index) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        const HRESULT enum_result = adapter->EnumOutputs(output_index, &output);
        if (enum_result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enum_result)) {
            if (result) *result = enum_result;
            if (error) *error = win32_error("EnumOutputs during recovery", enum_result);
            return false;
        }

        DXGI_OUTPUT_DESC desc = {};
        if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
        if (dxgi_device_name_matches(preferred_device_name_, desc.DeviceName)) {
            selected_output = output;
            selected_desc = desc;
            break; // wjy: VDD 会话始终优先精确回到原来的 \\.\DISPLAYx，禁止恢复到物理屏。
        }
        if (!preferred_device_name_.empty()) continue;

        const LONG width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        const LONG height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        const LONG area = width * height;
        const bool primary = desc.DesktopCoordinates.left <= 0 && desc.DesktopCoordinates.top <= 0
            && desc.DesktopCoordinates.right > 0 && desc.DesktopCoordinates.bottom > 0;
        if (primary || area > best_area) {
            selected_output = output;
            selected_desc = desc;
            best_area = area;
            if (primary) break;
        }
    }

    if (!selected_output) {
        if (result) *result = DXGI_ERROR_NOT_FOUND;
        if (error) *error = "requested DXGI output is not attached during recovery";
        return false; // wjy: 输出暂时消失时保留现有 Device 与最后帧，下一次退避到期后继续查找。
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = selected_output.As(&output1);
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> replacement;
    if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(device_.Get(), &replacement);
    if (FAILED(hr) || !replacement) {
        if (result) *result = FAILED(hr) ? hr : E_FAIL;
        if (error) *error = win32_error("DuplicateOutput during recovery", FAILED(hr) ? hr : E_FAIL);
        return false;
    }

    duplication_ = std::move(replacement); // wjy: 新对象完全创建成功后再原子式替换成员，失败过程不会破坏 D3D11 Device 和帧槽。
    size_.width = static_cast<uint32_t>(selected_desc.DesktopCoordinates.right - selected_desc.DesktopCoordinates.left);
    size_.height = static_cast<uint32_t>(selected_desc.DesktopCoordinates.bottom - selected_desc.DesktopCoordinates.top);
    recovery_frame_gate_.begin(); // wjy: 轻量重建成功后重新开启首帧隔离，避免 VDD 周期性 ACCESS_LOST 产生黑闪。
    if (result) *result = S_OK;
    append_stream_capture_diagnostic_log_rate_limited(
        "dxgi",
        "duplication recreated on existing device='" + utf8_from_wide(selected_desc.DeviceName)
            + "' size=" + std::to_string(size_.width) + "x" + std::to_string(size_.height),
        500); // wjy: 成功日志明确标记 existing device，实机可据此确认 NVENC 不再每五秒初始化。
    return true;
}

void DxgiCapture::schedule_duplication_recovery(long result)
{
    reset_duplication();
    ++consecutive_duplication_failures_;
    const uint32_t delay_ms = dxgi_duplication_retry_delay_ms(consecutive_duplication_failures_);
    next_duplication_retry_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
    (void)result; // wjy: HRESULT 已由调用处写入诊断，本函数只负责一致的资源边界与退避时钟。
}

void DxgiCapture::reset_duplication()
{
    duplication_.Reset(); // wjy: 轻量恢复只释放 Desktop Duplication，Device、Context、帧槽和已交付纹理全部保留。
    recovery_frame_gate_.begin(); // wjy: 在新 Duplication 到达前保持恢复门控，首张成功帧仍需预热隔离。
}
// ===end====

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
    recovery_frame_gate_.reset(); // wjy: 主动 reset 清除旧恢复轮次，下一次 initialize 会建立全新的预热门控。
    consecutive_duplication_failures_ = 0;
    next_duplication_retry_ = {};
}

void DxgiCapture::reset_resources()
{
    if (duplication_ || device_ || context_ || !frame_slots_.empty()) {
        append_stream_capture_diagnostic_log(
            "dxgi",
            "reset resources preferred_device='" + utf8_from_wide(preferred_device_name_.c_str()) + "'"); // wjy: 明确记录主动停止与设备级恢复的完整释放边界；普通 ACCESS_LOST 只释放 Duplication，不会写入此日志。
    }
    frame_slots_.clear(); // wjy: 已交付帧通过shared_ptr继续持有自己的槽和纹理，清空采集器不会制造悬空COM指针。
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
    size_ = {};
}

} // namespace lsp
