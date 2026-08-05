#include "host_media_pipeline.h"

#include "d3d11_native_frame_buffer.h"
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
                + std::string(preferred_device_name_.begin(), preferred_device_name_.end())
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

    void stop()
    {
        append_stream_capture_diagnostic_log(
            "capture",
            "native-source stop begin"); // wjy: 会话释放和应用退出都记录采集线程停止边界。
        running_ = false;
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
                  << " capture_ms=" << average_capture_ms << "\n"; // wjy: 两秒聚合一次定位采集瓶颈，禁止逐帧磁盘日志干扰60 FPS测量。
        std::ostringstream diagnostic;
        diagnostic << "telemetry publish_fps=" << published_frames_ / seconds
                   << " target_fps=" << fps_.load(std::memory_order_relaxed)
                   << " new=" << new_frames_
                   << " reused=" << reused_frames_
                   << " busy=" << busy_frames_
                   << " dropped=" << dropped_frames_
                   << " average_capture_ms=" << average_capture_ms;
        append_stream_capture_diagnostic_log(
            "capture",
            diagnostic.str()); // wjy: 每两秒一条聚合采集指标，复现卡死时可判断是否仍在循环、是否只有超时或槽位阻塞。
        report_started_ = now;
        capture_attempts_ = new_frames_ = reused_frames_ = busy_frames_ = dropped_frames_ = published_frames_ = 0;
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
            const bool fresh = capture_.capture(&captured, &capture_error);
            const auto capture_end = std::chrono::steady_clock::now();
            ++capture_attempts_;
            capture_time_us_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(capture_end - capture_begin).count());

            if (fresh) {
                last_frame_ = std::move(captured); // wjy: 新桌面图像替换源端最后帧，旧租约由下游引用决定何时归还。
                ++new_frames_;
                if (!first_fresh_frame_logged_) {
                    first_fresh_frame_logged_ = true;
                    append_stream_capture_diagnostic_log(
                        "capture",
                        "first fresh frame size=" + std::to_string(last_frame_.size.width)
                            + "x" + std::to_string(last_frame_.size.height)); // wjy: 第一张DXGI新帧是区分“输出创建成功但Acquire无帧”的关键边界。
                }
            } else if (capture_error == "busy") {
                ++busy_frames_; // wjy: 编码端占满四槽时不覆盖纹理，继续发布最后安全帧保持控制低延迟。
            } else if (capture_error != "timeout" && capture_error != "DXGI access lost") {
                ++dropped_frames_;
            }
            if (!fresh && capture_error != "timeout" && capture_error != "busy") {
                append_stream_capture_diagnostic_log_rate_limited(
                    "capture",
                    "capture returned error='" + capture_error + "' has_last_frame="
                        + std::to_string(last_frame_.texture ? 1 : 0),
                    1000); // wjy: ACCESS_LOST、初始化和纹理错误每秒最多记录一次，同时标明能否继续复用旧帧。
            }

            if (last_frame_.texture) {
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
            } else {
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
    uint64_t capture_time_us_ = 0;
    bool first_fresh_frame_logged_ = false; // wjy: 首张DXGI新帧只记录一次，避免正常60 FPS路径产生磁盘压力。
    bool first_published_frame_logged_ = false; // wjy: 首张送入WebRTC的源帧单独标记，用于与NVENC首帧配对。
};
// ===end====

// =====wjy====
class DesktopVideoSource : public webrtc::AdaptedVideoTrackSource,
                           public webrtc::DesktopCapturer::Callback { // wjy: WebRTC 的 RefCountedObject 需要从该类型派生，因此这里不能使用 final。
public:
    DesktopVideoSource(uint32_t fps, int64_t preferredSourceId, std::string preferredDeviceName, bool preferVirtualDisplayPath)
        : fps_(fps ? fps : 60)
        , preferred_source_id_(preferredSourceId)
        , preferred_device_name_(to_lower_copy(std::move(preferredDeviceName)))
        , prefer_virtual_display_path_(preferVirtualDisplayPath)
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
                + " prefer_virtual=" + std::to_string(prefer_virtual_display_path_ ? 1 : 0)); // wjy: 仅原生DXGI失败后进入CPU回退，记录其输入身份和后端偏好。
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
            const auto choose_parsec_fallback = [&sources]() -> webrtc::DesktopCapturer::Source {
                for (const auto& source : sources) {
                    const std::string title = to_lower_copy(source.title);
                    if (title.find("parsec") != std::string::npos || title.find("psccdd0") != std::string::npos) {
                        return source;
                    }
                }
                return sources.front();
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
            if (!matched && (preferred_source_id_ != 0 || !preferred_device_name_.empty())) {
                chosen = choose_parsec_fallback();
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
            return source != nullptr;
        }

        virtual_display = std::make_unique<ParsecVddSession>(); // wjy: VDD 所有权从 WebrtcSession 上移到主机共享媒体管线。
        std::string vdd_error;
        if (!virtual_display->start(&vdd_error)) {
            std::cout << "host media: parsec-vdd unavailable: " << vdd_error << "\n";
            virtual_display.reset(); // wjy: VDD 不可用时保持原有行为，回退到系统已有屏幕源。
            append_stream_capture_diagnostic_log(
                "pipeline",
                "VDD start failed error=" + vdd_error); // wjy: 记录回退CPU桌面之前的VDD真实错误。
        } else {
            std::cout << "host media: parsec-vdd source prepared id="
                      << virtual_display->preferred_source_id()
                      << " name='" << virtual_display->preferred_device_name() << "'\n";
            append_stream_capture_diagnostic_log(
                "pipeline",
                "VDD ready id=" + std::to_string(virtual_display->preferred_source_id())
                    + " device='" + virtual_display->preferred_device_name() + "'"); // wjy: 管线保存最终收到的VDD身份，随后可与DXGI选择结果逐行核对。
        }

        if (virtual_display) {
            auto native = webrtc::make_ref_counted<DxgiVideoSource>(
                fps, virtual_display->preferred_device_name());
            std::string native_error;
            if (native->start(&native_error)) {
                dxgi_source = native;
                source = native;
                std::cout << "host media: DXGI native texture path active for '"
                          << virtual_display->preferred_device_name() << "'\n"; // wjy: 原生路径成功后不再创建CPU DesktopCapturer和ARGBToI420转换链。
                append_stream_capture_diagnostic_log(
                    "pipeline",
                    "native DXGI source active"); // wjy: 明确标记生产环境最终选择原生纹理路径。
                return true;
            }
            std::cout << "host media: DXGI native capture unavailable: " << native_error
                      << "; falling back to CPU DesktopCapturer\n";
            append_stream_capture_diagnostic_log(
                "pipeline",
                "native DXGI source failed error=" + native_error
                    + "; fallback=DesktopCapturer"); // wjy: 回退原因写入文件，避免只存在不可见控制台输出。
        }

        auto desktop = webrtc::make_ref_counted<DesktopVideoSource>(
            fps,
            virtual_display ? virtual_display->preferred_source_id() : 0,
            virtual_display ? virtual_display->preferred_device_name() : std::string(),
            virtual_display != nullptr);
        if (!desktop->start(error)) {
            append_stream_capture_diagnostic_log(
                "pipeline",
                "DesktopCapturer source failed error=" + (error ? *error : std::string())); // wjy: 两种采集后端都失败时保存最终错误。
            virtual_display.reset();
            return false;
        }
        desktop_source = desktop;
        source = desktop;
        append_stream_capture_diagnostic_log(
            "pipeline",
            "DesktopCapturer source active"); // wjy: 标记CPU回退最终成为共享source。
        return true;
    }

    void stop_locked()
    {
        append_stream_capture_diagnostic_log(
            "pipeline",
            "stop begin subscribers=" + std::to_string(subscribers)
                + " native=" + std::to_string(dxgi_source ? 1 : 0)
                + " fallback=" + std::to_string(desktop_source ? 1 : 0)
                + " vdd=" + std::to_string(virtual_display ? 1 : 0)); // wjy: 停止前记录实际活跃后端和订阅数，确认重连是否完整释放系统资源。
        if (dxgi_source) dxgi_source->stop(); // wjy: 先等待原生采集线程退出再移除VDD，避免DuplicateOutput继续访问已删除显示器。
        if (custom_backend_started && hooks.stop) hooks.stop(); // wjy: 测试后端与生产后端遵循相同的最后订阅者停止时机。
        custom_backend_started = false;
        if (desktop_source) desktop_source->stop(); // wjy: 必须先停捕获线程，再释放 source 和虚拟显示器。
        source = nullptr;
        dxgi_source = nullptr; // wjy: 释放共享原生source引用后，仍被PeerConnection持有的帧可通过自身COM和租约安全结束。
        desktop_source = nullptr;
        if (virtual_display) virtual_display->stop();
        virtual_display.reset();
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
    if (!state->virtual_display) return 60;
    return display_refresh_hz(state->virtual_display->preferred_device_name());
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
