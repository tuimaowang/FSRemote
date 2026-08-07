#include "host_media_pipeline.h"

#include "d3d11_native_frame_buffer.h"
#include "host_display_selection_policy.h"
#include "parsec_vdd_session.h"
#include "dxgi_capture.h"
#include "stream_capture_diagnostics.h"

#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <libyuv/convert.h>
#include <media/base/adapted_video_track_source.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <modules/desktop_capture/desktop_capturer.h>
#include <modules/desktop_capture/desktop_frame.h>
#include <rtc_base/ref_counted_object.h>
#include <rtc_base/time_utils.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <wrl/client.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace uu {
namespace {

// =====wjy====
using lsp::append_stream_capture_diagnostic_log;
using lsp::append_stream_capture_diagnostic_log_rate_limited; // wjy: 仅把统一诊断函数引入当前实现作用域，避免改变既有媒体类和运行行为。
// ===end====

std::string to_lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::wstring widen_display_name(const std::string& value)
{
    return std::wstring(value.begin(), value.end()); // wjy: Parsec发布的是ASCII形式\\.\DISPLAYx，直接扩宽可保持与DXGI DeviceName逐字符一致。
}

uint32_t display_refresh_hz(const std::string& deviceName)
{
    const std::wstring wideName = widen_display_name(deviceName);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (wideName.empty()
        || !EnumDisplaySettingsW(wideName.c_str(), ENUM_CURRENT_SETTINGS, &mode)
        || mode.dmDisplayFrequency < 1
        || mode.dmDisplayFrequency > 360) {
        return 60;
    }
    return static_cast<uint32_t>(mode.dmDisplayFrequency);
}

// =====wjy====
std::string narrow_display_name(const wchar_t* value)
{
    if (!value || value[0] == L'\0') return {}; // wjy: 空设备名不能成为精确 DXGI 或 DesktopCapturer 目标。
    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};
    std::string result(static_cast<size_t>(required), '\0'); // wjy: 先为转换结果和终止符分配完整容量，禁止WideCharToMultiByte越界写入。
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), required, nullptr, nullptr); // wjy: 使用Win32 UTF-8转换避免宽字符直接截断警告，同时保持设备名和诊断日志可读。
    result.resize(static_cast<size_t>(required - 1)); // wjy: 转换完成后移除字符串长度中的终止符，返回标准UTF-8文本。
    return result;
}

bool is_parsec_display_adapter(const wchar_t* adapterDeviceName)
{
    if (!adapterDeviceName || adapterDeviceName[0] == L'\0') return false;
    for (DWORD monitorIndex = 0;; ++monitorIndex) {
        DISPLAY_DEVICEW monitor = {};
        monitor.cb = sizeof(monitor);
        if (!EnumDisplayDevicesW(adapterDeviceName, monitorIndex, &monitor, EDD_GET_DEVICE_INTERFACE_NAME)) break;
        if (monitor.DeviceID[0] != L'\0' && wcsstr(monitor.DeviceID, L"PSCCDD0") != nullptr) {
            return true; // wjy: Parsec VDD 的监视器硬件身份包含 PSCCDD0，不能被自动策略当成真实主屏。
        }
    }
    return false;
}

BOOL CALLBACK find_host_monitor_callback(HMONITOR monitor, HDC, LPRECT, LPARAM user)
{
    auto* context = reinterpret_cast<std::pair<const std::wstring*, HMONITOR*>*>(user);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    if (_wcsicmp(info.szDevice, context->first->c_str()) != 0) return TRUE;
    *context->second = monitor; // wjy: 把 Windows 设备名解析成同一主屏的 HMONITOR，供 CPU 捕获精确匹配 source id。
    return FALSE;
}

HMONITOR host_monitor_for_device_name(const std::wstring& deviceName)
{
    if (deviceName.empty()) return nullptr;
    HMONITOR monitor = nullptr;
    std::pair<const std::wstring*, HMONITOR*> context(&deviceName, &monitor);
    EnumDisplayMonitors(nullptr, nullptr, find_host_monitor_callback, reinterpret_cast<LPARAM>(&context));
    return monitor;
}

std::vector<HostDisplayCandidate> enumerate_host_display_candidates()
{
    std::vector<HostDisplayCandidate> candidates;
    for (DWORD adapterIndex = 0;; ++adapterIndex) {
        DISPLAY_DEVICEW adapter = {};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0)) break;

        HostDisplayCandidate candidate;
        candidate.device_name = narrow_display_name(adapter.DeviceName); // wjy: 同一设备名贯穿策略、DXGI 和刷新率查询，禁止不同阶段重新猜测目标。
        candidate.active = (adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0;
        candidate.primary = (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        candidate.remote = (adapter.StateFlags & DISPLAY_DEVICE_REMOTE) != 0;
        candidate.mirroring = (adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0;
        candidate.parsec = is_parsec_display_adapter(adapter.DeviceName);

        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
            candidate.width = mode.dmPelsWidth;
            candidate.height = mode.dmPelsHeight;
            candidate.refresh_hz = mode.dmDisplayFrequency; // wjy: 当前模式只作为候选有效性和最终刷新率快照，不修改真实屏配置。
        }
        candidate.monitor_id = reinterpret_cast<int64_t>(
            host_monitor_for_device_name(adapter.DeviceName)); // wjy: 无 HMONITOR 的残留适配器由纯策略统一排除。
        candidates.push_back(candidate);

        append_stream_capture_diagnostic_log(
            "display-select",
            "candidate device='" + candidate.device_name + "'"
                + " active=" + std::to_string(candidate.active ? 1 : 0)
                + " primary=" + std::to_string(candidate.primary ? 1 : 0)
                + " parsec=" + std::to_string(candidate.parsec ? 1 : 0)
                + " remote=" + std::to_string(candidate.remote ? 1 : 0)
                + " mirroring=" + std::to_string(candidate.mirroring ? 1 : 0)
                + " size=" + std::to_string(candidate.width) + "x" + std::to_string(candidate.height)
                + " hz=" + std::to_string(candidate.refresh_hz)
                + " monitor_id=" + std::to_string(candidate.monitor_id)); // wjy: 每轮首订阅完整记录决策输入，实机可直接解释为何创建或跳过 VDD。
    }
    return candidates;
}

enum class HostCaptureMode {
    None,
    Custom,
    PhysicalDxgi,
    PhysicalDesktop,
    VirtualDxgi,
    VirtualDesktop,
    GenericDesktop,
};

const char* host_capture_mode_name(HostCaptureMode mode)
{
    switch (mode) {
    case HostCaptureMode::Custom: return "custom";
    case HostCaptureMode::PhysicalDxgi: return "physical-dxgi";
    case HostCaptureMode::PhysicalDesktop: return "physical-desktop";
    case HostCaptureMode::VirtualDxgi: return "virtual-dxgi";
    case HostCaptureMode::VirtualDesktop: return "virtual-desktop";
    case HostCaptureMode::GenericDesktop: return "generic-desktop";
    case HostCaptureMode::None: break;
    }
    return "none"; // wjy: 统一的可读模式名同时服务启动、停止和错误诊断。
}
// ===end====

// =====wjy====
const char* desktop_capture_result_name(webrtc::DesktopCapturer::Result result)
{
    switch (result) {
    case webrtc::DesktopCapturer::Result::SUCCESS:
        return "success";
    case webrtc::DesktopCapturer::Result::ERROR_TEMPORARY:
        return "temporary-error";
    case webrtc::DesktopCapturer::Result::ERROR_PERMANENT:
        return "permanent-error";
    }
    return "unknown"; // wjy: 未知枚举仍落盘为可读文本，避免异常版本只留下数字。
}
// ===end====

// =====wjy====
class DxgiVideoSource : public webrtc::AdaptedVideoTrackSource {
public:
    DxgiVideoSource(uint32_t fps, std::string preferredDeviceName)
        : fps_(fps ? fps : 60)
        , preferred_device_name_(widen_display_name(preferredDeviceName))
    {
    }

    ~DxgiVideoSource() override { stop(); }

    void set_target_fps(uint32_t fps)
    {
        fps_.store(std::clamp(fps, 1u, 360u), std::memory_order_release);
    }

    bool start(std::string* error)
    {
        append_stream_capture_diagnostic_log(
            "capture",
            "native-source start begin preferred_device='"
                + narrow_display_name(preferred_device_name_.c_str())
                + "' fps=" + std::to_string(fps_.load(std::memory_order_acquire))); // wjy: 原生采集启动前记录目标输出和帧率，确认Host实际进入哪条媒体路径。
        if (preferred_device_name_.empty()) {
            if (error) *error = "DXGI native capture requires a selected display device";
            append_stream_capture_diagnostic_log(
                "capture",
                "native-source start failed error=missing preferred display device"); // wjy: VDD成功但没有发布设备名时留下明确失败阶段。
            return false;
        }
        if (!capture_.initialize(preferred_device_name_, error)) {
            append_stream_capture_diagnostic_log(
                "capture",
                "native-source initialize failed error=" + (error ? *error : std::string())); // wjy: 上层回退CPU前保存DXGI返回的真实HRESULT文本。
            return false; // wjy: VDD输出不能原生复制时由SharedState立即创建CPU DesktopCapturer，连接和输入协议不受影响。
        }
        running_ = true;
        {
            std::lock_guard<std::mutex> lock(first_frame_mutex_);
            first_frame_seen_ = false;
        }
        report_started_ = std::chrono::steady_clock::now();
        thread_ = std::thread([this] {
            try {
                capture_loop();
            } catch (const std::exception& exception) {
                append_stream_capture_diagnostic_log(
                    "capture",
                    std::string("native-source thread exception=") + exception.what()); // wjy: 采集线程标准异常写入持久日志但仍保持原有结束线程行为。
                running_ = false;
            } catch (...) {
                append_stream_capture_diagnostic_log(
                    "capture",
                    "native-source thread unknown exception"); // wjy: 非标准异常也留下终点，避免线程静默停止后控制端永久等待。
                running_ = false;
            }
        });
        append_stream_capture_diagnostic_log(
            "capture",
            "native-source start success"); // wjy: 线程创建完成后记录成功，后续若无first fresh即可锁定在AcquireNextFrame阶段。
        return true;
    }

    bool wait_for_first_frame(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(first_frame_mutex_);
        return first_frame_cv_.wait_for(lock, timeout, [this] {
            return first_frame_seen_ || !running_.load(std::memory_order_acquire);
        }) && first_frame_seen_;
    }

    void stop()
    {
        append_stream_capture_diagnostic_log(
            "capture",
            "native-source stop begin"); // wjy: 会话释放和应用退出都记录采集线程停止边界。
        running_ = false;
        first_frame_cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        last_frame_ = {}; // wjy: 先释放最后帧租约再销毁采集器，使环形槽位按正常生命周期归还。
        capture_.reset();
        append_stream_capture_diagnostic_log(
            "capture",
            "native-source stop end"); // wjy: join和DXGI资源释放完成后落盘，确认重连前旧采集器是否真正退出。
    }

    webrtc::MediaSourceInterface::SourceState state() const override
    {
        return running_ ? kLive : kEnded;
    }

    bool remote() const override { return false; }
    bool is_screencast() const override { return true; }
    std::optional<bool> needs_denoising() const override { return false; }

private:
    bool publish_frame(const lsp::CapturedFrame& captured)
    {
        if (!captured.texture || !captured.size.valid()) return false;
        const int width = static_cast<int>(captured.size.width);
        const int height = static_cast<int>(captured.size.height);
        int out_width = width;
        int out_height = height;
        int crop_width = width;
        int crop_height = height;
        int crop_x = 0;
        int crop_y = 0;
        const int64_t now_us = webrtc::TimeMicros();
        if (!AdaptFrame(width, height, now_us, &out_width, &out_height,
                        &crop_width, &crop_height, &crop_x, &crop_y)) {
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> device;
        captured.texture->GetDevice(&device); // wjy: 帧自己持有的纹理可在DXGI access-lost重建期间继续取得原设备并安全完成编码。
        auto native_buffer = CreateD3D11NativeFrameBuffer(
            std::move(device), captured.texture, captured.lifetime,
            crop_x, crop_y, crop_width, crop_height, out_width, out_height);
        if (!native_buffer) {
            OnFrameDropped();
            return false;
        }

        OnFrame(webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(native_buffer)
            .set_timestamp_us(now_us)
            .set_rotation(webrtc::kVideoRotation_0)
            .set_content_type(webrtc::VideoContentType::SCREENSHARE)
            .build()); // wjy: 原生纹理连同裁剪尺寸进入WebRTC，自定义H265直接编码，软件编码按需调用ToI420。
        return true;
    }

    void report_if_due(std::chrono::steady_clock::time_point now)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - report_started_);
        if (elapsed < std::chrono::seconds(2)) return;
        const double seconds = std::max(0.001, elapsed.count() / 1000.0);
        const double average_capture_ms = capture_attempts_ > 0
            ? static_cast<double>(capture_time_us_) / capture_attempts_ / 1000.0
            : 0.0;
        std::cout << "host native capture: publish_fps=" << published_frames_ / seconds
                  << " new=" << new_frames_
                  << " reused=" << reused_frames_
                  << " busy=" << busy_frames_
                  << " dropped=" << dropped_frames_
                  << " recovery_suppressed=" << recovery_suppressed_frames_
                  << " capture_ms=" << average_capture_ms << "\n"; // wjy: 两秒聚合一次定位采集瓶颈，禁止逐帧磁盘日志干扰60 FPS测量。
        std::ostringstream diagnostic;
        diagnostic << "telemetry publish_fps=" << published_frames_ / seconds
                   << " target_fps=" << fps_.load(std::memory_order_relaxed)
                   << " new=" << new_frames_
                   << " reused=" << reused_frames_
                   << " busy=" << busy_frames_
                   << " dropped=" << dropped_frames_
                   << " recovery_events=" << recovery_events_
                   << " recovery_suppressed=" << recovery_suppressed_frames_
                   << " recovering=" << (capture_recovering_ ? 1 : 0)
                   << " average_capture_ms=" << average_capture_ms;
        append_stream_capture_diagnostic_log(
            "capture",
            diagnostic.str()); // wjy: 每两秒一条聚合采集指标，复现卡死时可判断是否仍在循环、是否只有超时或槽位阻塞。
        report_started_ = now;
        capture_attempts_ = new_frames_ = reused_frames_ = busy_frames_ = dropped_frames_ = published_frames_ = 0;
        recovery_events_ = recovery_suppressed_frames_ = 0; // wjy: 恢复统计与普通帧计数使用同一个两秒窗口，避免新增逐帧日志和磁盘开销。
        capture_time_us_ = 0;
    }

    void capture_loop()
    {
        auto next = std::chrono::steady_clock::now();
        uint32_t appliedFps = 0;
        while (running_) {
            const uint32_t targetFps = std::clamp(fps_.load(std::memory_order_acquire), 1u, 360u);
            const auto interval = std::chrono::microseconds(1000000 / targetFps);
            if (appliedFps != targetFps) {
                next = std::chrono::steady_clock::now();
                appliedFps = targetFps;
            }
            next += interval;
            const auto capture_begin = std::chrono::steady_clock::now();
            lsp::CapturedFrame captured;
            std::string capture_error;
            const lsp::DxgiCaptureResult capture_result = capture_.capture_frame(&captured, &capture_error); // wjy: 类型化结果直接区分静止桌面、轻量恢复和设备恢复，删除脆弱的错误字符串分支。
            const bool fresh = capture_result.status == lsp::DxgiCaptureStatus::FreshFrame;
            bool publish_allowed = true;
            const auto capture_end = std::chrono::steady_clock::now();
            ++capture_attempts_;
            capture_time_us_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(capture_end - capture_begin).count());

            if (fresh) {
                last_frame_ = std::move(captured); // wjy: 新桌面图像替换源端最后帧，旧租约由下游引用决定何时归还。
                ++new_frames_;
                {
                    std::lock_guard<std::mutex> lock(first_frame_mutex_);
                    first_frame_seen_ = true;
                }
                first_frame_cv_.notify_all();
                if (capture_recovering_) {
                    capture_recovering_ = false;
                    append_stream_capture_diagnostic_log(
                        "capture",
                        "DXGI recovery completed; fresh frame resumed on retained pipeline"); // wjy: 首张真实恢复帧到达后一次性结束恢复态，确认旧画面保留期间没有重启会话。
                }
                if (!first_fresh_frame_logged_) {
                    first_fresh_frame_logged_ = true;
                    append_stream_capture_diagnostic_log(
                        "capture",
                        "first fresh frame size=" + std::to_string(last_frame_.size.width)
                            + "x" + std::to_string(last_frame_.size.height)); // wjy: 第一张DXGI新帧是区分“输出创建成功但Acquire无帧”的关键边界。
                }
            } else if (capture_result.status == lsp::DxgiCaptureStatus::FrameSlotBusy) {
                ++busy_frames_; // wjy: 编码端占满四槽时不覆盖纹理，继续发布最后安全帧保持控制低延迟。
            // =====wjy====
            } else if (capture_result.status == lsp::DxgiCaptureStatus::DuplicationRecovering
                       || capture_result.status == lsp::DxgiCaptureStatus::DeviceRecovering) {
                publish_allowed = false; // wjy: 恢复期不清空最后纹理，也不重复送入 NVENC；控制端 SwapChain 自然保留上一张成功画面。
                ++recovery_suppressed_frames_;
                if (!capture_recovering_) {
                    capture_recovering_ = true;
                    ++recovery_events_;
                    append_stream_capture_diagnostic_log(
                        "capture",
                        "DXGI recovery begin status=" + std::to_string(static_cast<int>(capture_result.status))
                            + " hr=" + std::to_string(static_cast<unsigned long>(capture_result.result))
                            + " retained_frame=" + std::to_string(last_frame_.texture ? 1 : 0)); // wjy: 每次恢复只记录入口，证明画面保留状态并避免五秒周期产生逐帧日志。
                }
            // ===end====
            } else if (capture_result.status == lsp::DxgiCaptureStatus::FatalError) {
                publish_allowed = false; // wjy: 未分类错误不继续反复编码可能失效的源纹理，等待下一轮采集恢复或上层会话策略处理。
                ++dropped_frames_;
            }
            if (capture_result.status == lsp::DxgiCaptureStatus::FatalError) {
                append_stream_capture_diagnostic_log_rate_limited(
                    "capture",
                    "capture returned error='" + capture_error + "' has_last_frame="
                        + std::to_string(last_frame_.texture ? 1 : 0),
                    1000); // wjy: 初始化和纹理等未分类错误每秒最多记录一次，同时标明是否仍保留最后成功帧。
            }

            if (publish_allowed && last_frame_.texture) {
                if (!fresh) ++reused_frames_; // wjy: 静止桌面也按目标节奏复用纹理，健康FPS不再被DXGI仅上报变化帧误导。
                if (publish_frame(last_frame_)) {
                    ++published_frames_;
                    if (!first_published_frame_logged_) {
                        first_published_frame_logged_ = true;
                        append_stream_capture_diagnostic_log(
                            "capture",
                            "first WebRTC source frame published"); // wjy: 证明D3D11NativeFrameBuffer已进入AdaptedVideoTrackSource，后续无画面应继续查编码/WebRTC。
                    }
                }
            } else if (publish_allowed) {
                ++dropped_frames_;
                OnFrameDropped();
                append_stream_capture_diagnostic_log_rate_limited(
                    "capture",
                    "no reusable frame available; OnFrameDropped",
                    1000); // wjy: 初始化后持续没有任何可发布纹理时限频记录，直接对应控制端“等待远程画面”。
            }

            report_if_due(capture_end);
            const auto pacingNow = std::chrono::steady_clock::now();
            if (pacingNow > next + interval) {
                next = pacingNow; // 严重落后时丢弃过期节拍，禁止连续复用旧帧追赶时钟造成编码突发。
            }
            std::this_thread::sleep_until(next); // wjy: 绝对时钟维持高质量60Hz节奏，单帧抖动不会累积成长期降帧。
        }
    }

    std::atomic<uint32_t> fps_ = 60;
    std::wstring preferred_device_name_;
    std::atomic_bool running_ = false;
    lsp::DxgiCapture capture_;
    lsp::CapturedFrame last_frame_;
    std::thread thread_;
    std::chrono::steady_clock::time_point report_started_;
    uint64_t capture_attempts_ = 0;
    uint64_t new_frames_ = 0;
    uint64_t reused_frames_ = 0;
    uint64_t busy_frames_ = 0;
    uint64_t dropped_frames_ = 0;
    uint64_t published_frames_ = 0;
    uint64_t recovery_events_ = 0;
    uint64_t recovery_suppressed_frames_ = 0; // wjy: 恢复空档只做内存计数，不提交旧帧编码，也不增加任何新线程或定时器。
    uint64_t capture_time_us_ = 0;
    bool capture_recovering_ = false;
    bool first_fresh_frame_logged_ = false; // wjy: 首张DXGI新帧只记录一次，避免正常60 FPS路径产生磁盘压力。
    bool first_published_frame_logged_ = false; // wjy: 首张送入WebRTC的源帧单独标记，用于与NVENC首帧配对。
    std::mutex first_frame_mutex_;
    std::condition_variable first_frame_cv_;
    bool first_frame_seen_ = false;
};
// ===end====

// =====wjy====
class DesktopVideoSource : public webrtc::AdaptedVideoTrackSource,
                           public webrtc::DesktopCapturer::Callback { // wjy: WebRTC 的 RefCountedObject 需要从该类型派生，因此这里不能使用 final。
public:
    DesktopVideoSource(uint32_t fps, int64_t preferredSourceId, std::string preferredDeviceName,
                       bool preferVirtualDisplayPath, bool requireExactTarget)
        : fps_(fps ? fps : 60)
        , preferred_source_id_(preferredSourceId)
        , preferred_device_name_(to_lower_copy(std::move(preferredDeviceName)))
        , prefer_virtual_display_path_(preferVirtualDisplayPath)
        , require_exact_target_(requireExactTarget) // wjy: 真实屏和VDD目标都必须精确命中，只有显式紧急兼容路径允许首项回退。
    {
    }

    ~DesktopVideoSource() override { stop(); }

    void set_target_fps(uint32_t fps)
    {
        fps_.store(std::clamp(fps, 1u, 360u), std::memory_order_release);
    }

    bool start(std::string* error)
    {
        append_stream_capture_diagnostic_log(
            "capture-fallback",
            "DesktopCapturer start begin preferred_id=" + std::to_string(preferred_source_id_)
                + " preferred_name='" + preferred_device_name_ + "'"
                + " prefer_virtual=" + std::to_string(prefer_virtual_display_path_ ? 1 : 0)
                + " require_exact=" + std::to_string(require_exact_target_ ? 1 : 0)); // wjy: 记录目标类型和精确匹配门禁，排查是否存在误抓其他屏幕。
        auto options = webrtc::DesktopCaptureOptions::CreateDefault();
#if defined(WEBRTC_WIN)
        options.set_allow_directx_capturer(true);
        options.set_allow_wgc_screen_capturer(!prefer_virtual_display_path_);
        options.set_allow_wgc_using_texture(false);
#endif
        capturer_ = webrtc::DesktopCapturer::CreateScreenCapturer(options); // wjy: HostMediaPipeline 只创建一次系统桌面捕获器，后续订阅复用其帧源。
        if (!capturer_) {
            if (error) *error = "DesktopCapturer::CreateScreenCapturer failed";
            append_stream_capture_diagnostic_log(
                "capture-fallback",
                "CreateScreenCapturer failed"); // wjy: CPU回退对象都无法创建时留下独立失败点。
            return false;
        }

        webrtc::DesktopCapturer::SourceList sources;
        if (capturer_->GetSourceList(&sources) && !sources.empty()) {
            std::cout << "desktop sources:";
            for (const auto& source : sources) {
                std::cout << " [" << source.id << " '" << source.title << "' display=" << source.display_id << "]";
                append_stream_capture_diagnostic_log(
                    "capture-fallback",
                    "source id=" + std::to_string(source.id)
                        + " title='" + source.title + "'"
                        + " display_id=" + std::to_string(source.display_id)); // wjy: 保存WebRTC实际返回的ID/title/display_id，验证VDD身份匹配是否退化为第一个屏幕。
            }
            std::cout << "\n";

            auto chosen = sources.front();
            const auto choose_parsec_fallback = [&sources]() -> std::optional<webrtc::DesktopCapturer::Source> {
                for (const auto& source : sources) {
                    const std::string title = to_lower_copy(source.title);
                    if (title.find("parsec") != std::string::npos || title.find("psccdd0") != std::string::npos) {
                        return source;
                    }
                }
                return std::nullopt; // wjy: 找不到 Parsec 身份时禁止静默退化到物理屏，避免虚拟路径泄露错误画面。
            };

            bool matched = false;
            if (preferred_source_id_ != 0) {
                for (const auto& source : sources) {
                    if (static_cast<int64_t>(source.id) == preferred_source_id_
                        || static_cast<int64_t>(source.display_id) == preferred_source_id_) {
                        chosen = source;
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched && !preferred_device_name_.empty()) {
                for (const auto& source : sources) {
                    const std::string title = to_lower_copy(source.title);
                    if (title == preferred_device_name_ || title.find(preferred_device_name_) != std::string::npos) {
                        chosen = source;
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched && prefer_virtual_display_path_) {
                const auto parsecFallback = choose_parsec_fallback();
                if (parsecFallback) {
                    chosen = *parsecFallback;
                    matched = true; // wjy: WebRTC 未暴露 HMONITOR 时仅允许通过明确的 Parsec 标题完成 VDD 兼容匹配。
                }
            }
            if (!matched && require_exact_target_) {
                if (error) *error = "DesktopCapturer target display was not found";
                append_stream_capture_diagnostic_log(
                    "capture-fallback",
                    "exact target not found preferred_id=" + std::to_string(preferred_source_id_)
                        + " preferred_name='" + preferred_device_name_ + "'"); // wjy: 精确目标缺失必须返回上层继续 VDD 回退，不能选择 sources.front。
                return false;
            }

            if (!capturer_->SelectSource(chosen.id)) {
                if (error) *error = "DesktopCapturer::SelectSource failed";
                append_stream_capture_diagnostic_log(
                    "capture-fallback",
                    "SelectSource failed chosen_id=" + std::to_string(chosen.id)); // wjy: 选屏失败不再只返回控制端等待状态。
                return false;
            }
            std::cout << "desktop selected source id=" << chosen.id
                      << " preferred=" << preferred_source_id_
                      << " name='" << preferred_device_name_ << "'\n";
            append_stream_capture_diagnostic_log(
                "capture-fallback",
                "selected id=" + std::to_string(chosen.id)
                    + " matched=" + std::to_string(matched ? 1 : 0)
                    + " preferred_id=" + std::to_string(preferred_source_id_)
                    + " preferred_name='" + preferred_device_name_ + "'"); // wjy: 明确记录是否真正命中VDD，或只是回退到sources.front。
        } else {
            append_stream_capture_diagnostic_log(
                "capture-fallback",
                "GetSourceList returned no sources"); // wjy: 无活动桌面时WebRTC可能创建捕获器成功但枚举为空，必须在启动日志中显式可见。
            if (error) *error = "DesktopCapturer returned no display sources";
            return false; // wjy: 无可选源时启动线程只会永久等待空帧，因此直接交给下一层 VDD 或会话错误处理。
        }

        capturer_->SetMaxFrameRate(fps_.load(std::memory_order_acquire));
        capturer_->Start(this);
        running_ = true;
        thread_ = std::thread([this] {
            try {
                capture_loop(); // wjy: 捕获线程异常只结束共享视频源，不允许std::terminate直接带走Host进程。
            } catch (const std::exception& exception) {
                append_stream_capture_diagnostic_log(
                    "capture-fallback",
                    std::string("thread exception=") + exception.what()); // wjy: CPU回退线程异常持久化后保持原有结束行为。
                running_ = false;
            } catch (...) {
                append_stream_capture_diagnostic_log(
                    "capture-fallback",
                    "thread unknown exception"); // wjy: 未知异常也留下线程停止原因。
                running_ = false; // wjy: 失败状态由现有会话检测并重连/报错，析构仍可安全join该线程。
            }
        });
        append_stream_capture_diagnostic_log(
            "capture-fallback",
            "DesktopCapturer start success"); // wjy: 回退线程已运行但无首帧时可从后续OnCaptureResult继续定位。
        return true;
    }

    void stop()
    {
        append_stream_capture_diagnostic_log(
            "capture-fallback",
            "DesktopCapturer stop begin"); // wjy: 回退采集器停止过程与原生路径使用相同的成对边界。
        running_ = false;
        if (thread_.joinable()) thread_.join(); // wjy: source 引用清空前先等待 CaptureFrame 循环退出，避免 VDD 被移除后仍读取屏幕。
        capturer_.reset();
        append_stream_capture_diagnostic_log(
            "capture-fallback",
            "DesktopCapturer stop end"); // wjy: 捕获器销毁完成后记录终点。
    }

    webrtc::MediaSourceInterface::SourceState state() const override
    {
        return running_ ? kLive : kEnded;
    }

    bool remote() const override { return false; }
    bool is_screencast() const override { return true; }
    std::optional<bool> needs_denoising() const override { return false; }

    void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                         std::unique_ptr<webrtc::DesktopFrame> frame) override
    {
        if (result != webrtc::DesktopCapturer::Result::SUCCESS || !frame || !frame->data()) {
            OnFrameDropped();
            append_stream_capture_diagnostic_log_rate_limited(
                "capture-fallback",
                std::string("OnCaptureResult result=") + desktop_capture_result_name(result)
                    + " frame=" + std::to_string(frame ? 1 : 0)
                    + " data=" + std::to_string(frame && frame->data() ? 1 : 0),
                1000); // wjy: 临时/永久错误及空帧按秒记录，确认CPU回退为何没有产出首帧。
            return;
        }

        const int width = frame->size().width();
        const int height = frame->size().height();
        int out_width = width;
        int out_height = height;
        int crop_width = width;
        int crop_height = height;
        int crop_x = 0;
        int crop_y = 0;
        const int64_t now_us = webrtc::TimeMicros();
        if (!AdaptFrame(width, height, now_us, &out_width, &out_height,
                        &crop_width, &crop_height, &crop_x, &crop_y)) {
            return;
        }

        auto buffer = webrtc::I420Buffer::Create(width, height);
        const int converted = libyuv::ARGBToI420(
            frame->data(), frame->stride(),
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            width, height);
        if (converted != 0) {
            OnFrameDropped();
            append_stream_capture_diagnostic_log_rate_limited(
                "capture-fallback",
                "ARGBToI420 failed code=" + std::to_string(converted),
                1000); // wjy: 颜色转换失败独立记录，避免被误判为DXGI或网络问题。
            return;
        }

        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> final_buffer = buffer;
        if (out_width != width || out_height != height || crop_width != width || crop_height != height) {
            final_buffer = buffer->CropAndScale(crop_x, crop_y, crop_width, crop_height, out_width, out_height);
        }

        OnFrame(webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(final_buffer)
            .set_timestamp_us(now_us)
            .set_rotation(webrtc::kVideoRotation_0)
            .set_content_type(webrtc::VideoContentType::SCREENSHARE)
            .build()); // wjy: 同一帧进入共享 AdaptedVideoTrackSource，所有会话 track 各自编码和拥塞控制。
        if (!first_frame_logged_) {
            first_frame_logged_ = true;
            append_stream_capture_diagnostic_log(
                "capture-fallback",
                "first WebRTC source frame published size=" + std::to_string(width)
                    + "x" + std::to_string(height)); // wjy: CPU路径首帧标记与原生路径保持一致，后续直接查编码器。
        }
    }

private:
    void capture_loop()
    {
        auto next = std::chrono::steady_clock::now();
        uint32_t appliedFps = 0;
        while (running_) {
            const uint32_t targetFps = std::clamp(fps_.load(std::memory_order_acquire), 1u, 360u);
            if (capturer_ && appliedFps != targetFps) {
                capturer_->SetMaxFrameRate(targetFps);
                next = std::chrono::steady_clock::now();
                appliedFps = targetFps;
            }
            const auto interval = std::chrono::microseconds(1000000 / targetFps);
            next += interval;
            if (capturer_) capturer_->CaptureFrame();
            const auto pacingNow = std::chrono::steady_clock::now();
            if (pacingNow > next + interval) {
                next = pacingNow;
            }
            std::this_thread::sleep_until(next);
        }
    }

    std::atomic<uint32_t> fps_ = 60;
    int64_t preferred_source_id_ = 0;
    std::string preferred_device_name_;
    bool prefer_virtual_display_path_ = false;
    bool require_exact_target_ = false; // wjy: 目标化捕获必须命中同一屏幕，通用兼容路径才允许使用枚举首项。
    std::atomic_bool running_ = false;
    std::unique_ptr<webrtc::DesktopCapturer> capturer_;
    std::thread thread_;
    bool first_frame_logged_ = false; // wjy: CPU回退只记录第一张成功源帧，正常运行不逐帧写盘。
};
// ===end====

} // namespace

// =====wjy====
struct HostMediaPipeline::SharedState {
    void set_target_fps_locked(uint32_t requestedFps)
    {
        fps = std::clamp(requestedFps, 1u, 360u);
        if (dxgi_source) dxgi_source->set_target_fps(fps);
        if (desktop_source) desktop_source->set_target_fps(fps);
        append_stream_capture_diagnostic_log(
            "pipeline",
            "target fps updated fps=" + std::to_string(fps));
    }

    // =====wjy====
    void activate_target_locked(HostCaptureMode mode, const std::string& deviceName, int64_t monitorId)
    {
        active_capture_mode = mode; // wjy: 最终模式与 source 同时提交，刷新率和停止日志不会再从 VDD 是否存在反推状态。
        active_device_name = deviceName;
        active_monitor_id = monitorId;
        append_stream_capture_diagnostic_log(
            "pipeline",
            "capture target active mode=" + std::string(host_capture_mode_name(mode))
                + " device='" + active_device_name + "'"
                + " monitor_id=" + std::to_string(active_monitor_id)); // wjy: 单行记录最终选择，现场日志可直接判断本轮是否创建了虚拟屏。
    }

    bool start_dxgi_locked(const std::string& deviceName, int64_t monitorId,
                           HostCaptureMode mode, const char* targetLabel, std::string* failure)
    {
        auto native = webrtc::make_ref_counted<DxgiVideoSource>(fps, deviceName);
        std::string nativeError;
        if (!native->start(&nativeError)) {
            if (failure) *failure = nativeError;
            append_stream_capture_diagnostic_log(
                "pipeline",
                std::string(targetLabel) + " DXGI failed device='" + deviceName
                    + "' error=" + nativeError); // wjy: 真实屏失败将继续CPU捕获，VDD失败将继续VDD CPU回退，原因保持独立。
            return false;
        }
        constexpr auto kFirstFrameProbeTimeout = std::chrono::milliseconds(2500);
        if (!native->wait_for_first_frame(kFirstFrameProbeTimeout)) {
            native->stop();
            if (failure) {
                *failure = nativeError.empty()
                    ? "DXGI source produced no first frame within probe window"
                    : nativeError + "; no first frame within probe window";
            }
            append_stream_capture_diagnostic_log(
                "display-select",
                std::string(targetLabel) + " DXGI no first frame; falling back after probe_ms="
                    + std::to_string(kFirstFrameProbeTimeout.count()));
            return false;
        }
        dxgi_source = native;
        source = native;
        activate_target_locked(mode, deviceName, monitorId); // wjy: 只有 DuplicateOutput 初始化成功后才公布目标，失败对象不会污染共享状态。
        return true;
    }

    bool start_desktop_locked(int64_t monitorId, const std::string& deviceName,
                              bool preferVirtualDisplayPath, bool requireExactTarget,
                              HostCaptureMode mode, const char* targetLabel, std::string* failure)
    {
        auto desktop = webrtc::make_ref_counted<DesktopVideoSource>(
            fps, monitorId, deviceName, preferVirtualDisplayPath, requireExactTarget);
        std::string desktopError;
        if (!desktop->start(&desktopError)) {
            if (failure) *failure = desktopError;
            append_stream_capture_diagnostic_log(
                "pipeline",
                std::string(targetLabel) + " DesktopCapturer failed device='" + deviceName
                    + "' monitor_id=" + std::to_string(monitorId)
                    + " error=" + desktopError); // wjy: 精确选屏失败向上返回，真实屏路径才会继续创建VDD。
            return false;
        }
        desktop_source = desktop;
        source = desktop;
        activate_target_locked(mode, deviceName, monitorId); // wjy: CPU兼容源成功后仍保存同一个目标身份，输入与刷新率语义不变。
        return true;
    }
    // ===end====

    bool start_locked(std::string* error)
    {
        if (source) return true;
        append_stream_capture_diagnostic_log(
            "pipeline",
            "start begin fps=" + std::to_string(fps)
                + " custom_backend=" + std::to_string(hooks.start ? 1 : 0)); // wjy: 共享媒体管线首次订阅的入口状态先于VDD和DXGI落盘。

        if (hooks.start) {
            source = hooks.start(error); // wjy: 单元测试通过假 source 验证计数，不启动真实显示驱动或捕获线程。
            custom_backend_started = source != nullptr;
            if (!source && error && error->empty()) *error = "host media test backend returned null source";
            if (source) activate_target_locked(HostCaptureMode::Custom, {}, 0); // wjy: 测试后端也进入统一状态机，停止时能验证模式被完整清空。
            return source != nullptr;
        }

        // =====wjy====
        const auto physicalTarget = select_existing_primary_display(
            enumerate_host_display_candidates()); // wjy: 每个空闲到活跃的首订阅都重新读取当前拓扑，断线期间插拔屏幕后下次连接可重新决策。
        if (physicalTarget) {
            append_stream_capture_diagnostic_log(
                "display-select",
                "selected existing primary device='" + physicalTarget->device_name
                    + "' monitor_id=" + std::to_string(physicalTarget->monitor_id));

            std::string physicalDxgiError;
            if (start_dxgi_locked(
                    physicalTarget->device_name,
                    physicalTarget->monitor_id,
                    HostCaptureMode::PhysicalDxgi,
                    "physical",
                    &physicalDxgiError)) {
                return true; // wjy: 真实主屏原生纹理成功时不创建、设置或删除任何 Parsec 显示器。
            }

            std::string physicalDesktopError;
            if (start_desktop_locked(
                    physicalTarget->monitor_id,
                    physicalTarget->device_name,
                    false,
                    true,
                    HostCaptureMode::PhysicalDesktop,
                    "physical",
                    &physicalDesktopError)) {
                return true; // wjy: 原生捕获不可用但精确CPU源可用时仍保留真实主屏，避免无必要VDD。
            }

            append_stream_capture_diagnostic_log(
                "display-select",
                "existing primary unusable device='" + physicalTarget->device_name
                    + "' dxgi_error=" + physicalDxgiError
                    + " desktop_error=" + physicalDesktopError
                    + "; fallback=virtual-display"); // wjy: 两级真实屏捕获都失败后才允许进入VDD创建阶段。
        } else {
            append_stream_capture_diagnostic_log(
                "display-select",
                "no eligible existing primary; fallback=virtual-display"); // wjy: 无头、远程、镜像或仅Parsec拓扑统一走受控VDD生命周期。
        }

        virtual_display = std::make_unique<ParsecVddSession>(); // wjy: 只有真实主屏不存在或无法捕获时才分配 VDD 会话。
        std::string vdd_error;
        if (!virtual_display->start(&vdd_error)) {
            std::cout << "host media: parsec-vdd unavailable: " << vdd_error << "\n";
            virtual_display.reset(); // wjy: VDD句柄未就绪时立即释放局部所有权，再尝试原有通用DesktopCapturer紧急回退。
            append_stream_capture_diagnostic_log(
                "pipeline",
                "VDD start failed error=" + vdd_error); // wjy: 记录紧急通用CPU回退之前的VDD真实错误。

            std::string genericDesktopError;
            if (start_desktop_locked(
                    0,
                    {},
                    false,
                    false,
                    HostCaptureMode::GenericDesktop,
                    "generic",
                    &genericDesktopError)) {
                return true; // wjy: 驱动缺失时保留旧版可用性，但该路径明确标记为紧急通用捕获而非真实主屏命中。
            }
            if (error) {
                *error = "parsec-vdd failed: " + vdd_error
                    + "; generic desktop failed: " + genericDesktopError;
            }
            return false;
        } else {
            std::cout << "host media: parsec-vdd source prepared id="
                      << virtual_display->preferred_source_id()
                      << " name='" << virtual_display->preferred_device_name() << "'\n";
            append_stream_capture_diagnostic_log(
                "pipeline",
                "VDD ready id=" + std::to_string(virtual_display->preferred_source_id())
                    + " device='" + virtual_display->preferred_device_name() + "'"); // wjy: 管线保存最终收到的VDD身份，随后可与DXGI选择结果逐行核对。
        }

        const int64_t virtualMonitorId = virtual_display->preferred_source_id();
        const std::string virtualDeviceName = virtual_display->preferred_device_name();
        std::string virtualDxgiError;
        if (start_dxgi_locked(
                virtualDeviceName,
                virtualMonitorId,
                HostCaptureMode::VirtualDxgi,
                "virtual",
                &virtualDxgiError)) {
            return true; // wjy: 无头兜底仍优先保持现有D3D11纹理到NVENC的高性能链路。
        }

        std::string virtualDesktopError;
        if (start_desktop_locked(
                virtualMonitorId,
                virtualDeviceName,
                true,
                true,
                HostCaptureMode::VirtualDesktop,
                "virtual",
                &virtualDesktopError)) {
            return true; // wjy: DXGI失败时只允许精确命中本轮VDD，不得静默捕获其他物理屏幕。
        }

        append_stream_capture_diagnostic_log(
            "pipeline",
            "virtual display capture failed dxgi_error=" + virtualDxgiError
                + " desktop_error=" + virtualDesktopError); // wjy: VDD两条捕获路径都失败时保留完整因果链后终止本次订阅。
        if (error) {
            *error = "virtual display capture failed: dxgi=" + virtualDxgiError
                + "; desktop=" + virtualDesktopError;
        }
        virtual_display->stop();
        virtual_display.reset();
        return false;
        // ===end====
    }

    void stop_locked()
    {
        append_stream_capture_diagnostic_log(
            "pipeline",
            "stop begin subscribers=" + std::to_string(subscribers)
                + " native=" + std::to_string(dxgi_source ? 1 : 0)
                + " fallback=" + std::to_string(desktop_source ? 1 : 0)
                + " vdd=" + std::to_string(virtual_display ? 1 : 0)
                + " mode=" + host_capture_mode_name(active_capture_mode)
                + " device='" + active_device_name + "'"
                + " monitor_id=" + std::to_string(active_monitor_id)); // wjy: 停止前保存最终真实/虚拟模式，确认重连是否完整释放并重新决策。
        if (dxgi_source) dxgi_source->stop(); // wjy: 先等待原生采集线程退出再移除VDD，避免DuplicateOutput继续访问已删除显示器。
        if (custom_backend_started && hooks.stop) hooks.stop(); // wjy: 测试后端与生产后端遵循相同的最后订阅者停止时机。
        custom_backend_started = false;
        if (desktop_source) desktop_source->stop(); // wjy: 必须先停捕获线程，再释放 source 和虚拟显示器。
        source = nullptr;
        dxgi_source = nullptr; // wjy: 释放共享原生source引用后，仍被PeerConnection持有的帧可通过自身COM和租约安全结束。
        desktop_source = nullptr;
        if (virtual_display) virtual_display->stop();
        virtual_display.reset();
        active_capture_mode = HostCaptureMode::None; // wjy: 最后订阅者离开后清空决策结果，下次首订阅必须重新枚举当前显示拓扑。
        active_device_name.clear();
        active_monitor_id = 0;
        append_stream_capture_diagnostic_log(
            "pipeline",
            "stop end"); // wjy: DXGI、CPU source和VDD全部释放后写入统一终点。
    }

    mutable std::mutex mutex;
    uint32_t fps = 60;
    HostMediaPipelineHooks hooks;
    bool shutting_down = false;
    bool custom_backend_started = false;
    size_t subscribers = 0;
    HostCaptureMode active_capture_mode = HostCaptureMode::None; // wjy: 保存共享管线本轮最终后端，不按单个PeerConnection重复选择。
    std::string active_device_name; // wjy: 真实屏与VDD共用的最终设备身份，供刷新率和诊断读取。
    int64_t active_monitor_id = 0; // wjy: 与最终设备配对的HMONITOR快照，停止日志可核对CPU选屏。
    std::unique_ptr<ParsecVddSession> virtual_display;
    webrtc::scoped_refptr<DxgiVideoSource> dxgi_source; // wjy: 多个会话共享同一DXGI采集源，避免每个PeerConnection重复抓屏。
    webrtc::scoped_refptr<DesktopVideoSource> desktop_source;
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source;
};

HostMediaPipeline::Subscription::Subscription(
    std::shared_ptr<SharedState> state,
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source)
    : state_(std::move(state))
    , source_(std::move(source))
{
}

HostMediaPipeline::Subscription::~Subscription()
{
    source_ = nullptr; // wjy: 先释放订阅令牌自己的 source 引用，再判断是否应停止共享后端。
    const std::shared_ptr<SharedState> state = std::move(state_);
    if (!state) return;
    std::lock_guard lock(state->mutex);
    if (state->subscribers > 0) --state->subscribers;
    append_stream_capture_diagnostic_log(
        "pipeline",
        "unsubscribe subscribers=" + std::to_string(state->subscribers)); // wjy: 会话释放令牌后记录剩余订阅数，判断最后一个退出是否真正触发采集停止。
    if (state->subscribers == 0) state->stop_locked(); // wjy: 最后一个会话离开才停止桌面捕获和 VDD，单个会话退出不影响其他订阅者。
}

webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> HostMediaPipeline::Subscription::source() const
{
    return source_;
}

HostMediaPipeline::HostMediaPipeline(uint32_t fps, HostMediaPipelineHooks hooks)
    : state_(std::make_shared<SharedState>())
{
    state_->fps = fps ? fps : 60;
    state_->hooks = std::move(hooks);
}

HostMediaPipeline::~HostMediaPipeline()
{
    shutdown();
    state_.reset(); // wjy: 已存在的订阅令牌仍持有 SharedState，可在异常析构顺序下安全完成最后释放。
}

std::unique_ptr<HostMediaPipeline::Subscription> HostMediaPipeline::subscribe(std::string* error)
{
    const std::shared_ptr<SharedState> state = state_;
    if (!state) {
        if (error) *error = "host media pipeline is unavailable";
        return nullptr;
    }
    std::lock_guard lock(state->mutex);
    if (state->shutting_down) {
        if (error) *error = "host media pipeline is shutting down";
        return nullptr;
    }
    if (state->subscribers == 0 && !state->start_locked(error)) return nullptr; // wjy: 第一个订阅者惰性启动一次共享后端。
    ++state->subscribers;
    append_stream_capture_diagnostic_log(
        "pipeline",
        "subscribe success subscribers=" + std::to_string(state->subscribers)); // wjy: 会话取得共享source后记录计数，排查busy会话是否真正持有媒体订阅。
    return std::unique_ptr<Subscription>(new Subscription(state, state->source));
}

void HostMediaPipeline::set_target_fps(uint32_t fps)
{
    const std::shared_ptr<SharedState> state = state_;
    if (!state) return;
    std::lock_guard lock(state->mutex);
    state->set_target_fps_locked(fps);
}

uint32_t HostMediaPipeline::target_fps() const
{
    const std::shared_ptr<SharedState> state = state_;
    if (!state) return 60;
    std::lock_guard lock(state->mutex);
    return state->fps;
}

uint32_t HostMediaPipeline::source_refresh_hz() const
{
    const std::shared_ptr<SharedState> state = state_;
    if (!state) return 60;
    std::lock_guard lock(state->mutex);
    return display_refresh_hz(state->active_device_name); // wjy: 真实主屏和VDD都从最终目标设备读取刷新率，通用/测试源继续安全回退60Hz。
}

void HostMediaPipeline::shutdown()
{
    const std::shared_ptr<SharedState> state = state_;
    if (!state) return;
    std::lock_guard lock(state->mutex);
    state->shutting_down = true; // wjy: manager 进入关闭后拒绝新订阅，现有会话按正常析构路径释放。
    if (state->subscribers == 0) state->stop_locked();
}

size_t HostMediaPipeline::subscriber_count() const
{
    const std::shared_ptr<SharedState> state = state_;
    if (!state) return 0;
    std::lock_guard lock(state->mutex);
    return state->subscribers;
}
// ===end====

} // namespace uu
