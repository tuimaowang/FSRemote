#include "parsec_vdd_session.h"
#include "stream_capture_diagnostics.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "parsec-vdd.h"

#include <chrono>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace uu {
namespace {

// =====wjy====
using lsp::append_stream_capture_diagnostic_log; // wjy: 仅把统一诊断函数引入当前VDD实现作用域，不改变显示创建、复用或释放流程。
// ===end====

constexpr DWORD kDisplayEnumerateTimeoutMs = 5000;
constexpr int kTargetWidth = 1920;
constexpr int kTargetHeight = 1080;
constexpr int kTargetHz = 60;

struct DisplayInfo {
    std::wstring device_name;
    int display_index = -1;
    bool active = false;
    int64_t monitor_id = 0;
};

// =====wjy====
std::string narrow(const std::wstring& value); // wjy: 前置声明供显示枚举阶段直接把\\.\DISPLAYx写入统一UTF-8诊断日志。
// ===end====

int parse_display_address(const std::wstring& path)
{
    const size_t uid = path.find(L"UID");
    if (uid == std::wstring::npos) {
        return 0;
    }

    size_t begin = uid + 3;
    size_t end = begin;
    while (end < path.size() && path[end] >= L'0' && path[end] <= L'9') {
        ++end;
    }
    if (end <= begin) {
        return 0;
    }

    return std::stoi(path.substr(begin, end - begin));
}

BOOL CALLBACK find_monitor_callback(HMONITOR monitor, HDC, LPRECT, LPARAM user)
{
    auto* context = reinterpret_cast<std::pair<const std::wstring*, HMONITOR*>*>(user);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }

    if (_wcsicmp(info.szDevice, context->first->c_str()) == 0) {
        *context->second = monitor;
        return FALSE;
    }
    return TRUE;
}

HMONITOR monitor_for_device_name(const std::wstring& device_name)
{
    if (device_name.empty()) {
        return nullptr;
    }

    HMONITOR monitor = nullptr;
    std::pair<const std::wstring*, HMONITOR*> context(&device_name, &monitor);
    EnumDisplayMonitors(nullptr, nullptr, find_monitor_callback, reinterpret_cast<LPARAM>(&context));
    return monitor;
}

bool set_display_mode(const std::wstring& device_name)
{
    if (device_name.empty()) {
        return false;
    }

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
        return false;
    }

    mode.dmPelsWidth = kTargetWidth;
    mode.dmPelsHeight = kTargetHeight;
    mode.dmDisplayFrequency = kTargetHz;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

    const LONG result = ChangeDisplaySettingsExW(
        device_name.c_str(),
        &mode,
        nullptr,
        CDS_UPDATEREGISTRY,
        nullptr);
    return result == DISP_CHANGE_SUCCESSFUL;
}

bool commit_display_changes()
{
    return ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr) == DISP_CHANGE_SUCCESSFUL;
}

bool set_display_primary(const std::wstring& device_name)
{
    if (device_name.empty()) {
        return false;
    }

    DEVMODEW target = {};
    target.dmSize = sizeof(target);
    if (!EnumDisplaySettingsW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &target)) {
        return false;
    }

    const int offset_x = target.dmPosition.x;
    const int offset_y = target.dmPosition.y;

    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    bool target_ok = false;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i) {
        if ((adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0) {
            continue;
        }

        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
            continue;
        }

        mode.dmPosition.x -= offset_x;
        mode.dmPosition.y -= offset_y;
        mode.dmFields = DM_POSITION;

        DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
        const bool is_target = _wcsicmp(adapter.DeviceName, device_name.c_str()) == 0;
        if (is_target) {
            flags |= CDS_SET_PRIMARY;
        }

        const LONG result = ChangeDisplaySettingsExW(adapter.DeviceName, &mode, nullptr, flags, nullptr);
        if (is_target && result == DISP_CHANGE_SUCCESSFUL) {
            target_ok = true;
        }
    }

    return target_ok && commit_display_changes();
}

bool is_parsec_display_device(const DISPLAY_DEVICEW& monitor)
{
    return monitor.DeviceID[0] != L'\0'
        && wcsstr(monitor.DeviceID, L"PSCCDD0") != nullptr;
}

bool enumerate_parsec_display_by_index(int display_index, DisplayInfo* out)
{
    if (!out) {
        return false;
    }

    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i) {
        DISPLAY_DEVICEW monitor = {};
        monitor.cb = sizeof(monitor);
        for (DWORD j = 0; EnumDisplayDevicesW(adapter.DeviceName, j, &monitor, EDD_GET_DEVICE_INTERFACE_NAME); ++j) {
            if (!is_parsec_display_device(monitor)) {
                continue;
            }

            const int address = parse_display_address(monitor.DeviceID);
            if (address - 0x100 != display_index) {
                continue;
            }

            DEVMODEW mode = {};
            mode.dmSize = sizeof(mode);
            out->device_name = adapter.DeviceName;
            out->display_index = display_index;
            out->active = EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode) != FALSE;
            out->monitor_id = reinterpret_cast<int64_t>(monitor_for_device_name(adapter.DeviceName));
            return true;
        }
    }

    return false;
}

std::vector<int> enumerate_parsec_display_indexes()
{
    std::vector<int> indexes;
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i) {
        DISPLAY_DEVICEW monitor = {};
        monitor.cb = sizeof(monitor);
        for (DWORD j = 0; EnumDisplayDevicesW(adapter.DeviceName, j, &monitor, EDD_GET_DEVICE_INTERFACE_NAME); ++j) {
            if (!is_parsec_display_device(monitor)) {
                continue;
            }

            const int address = parse_display_address(monitor.DeviceID);
            if (address >= 0x100) {
                indexes.push_back(address - 0x100);
            }
        }
    }

    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    return indexes;
}

void remove_existing_parsec_displays(HANDLE handle)
{
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }

    auto indexes = enumerate_parsec_display_indexes();
    std::sort(indexes.rbegin(), indexes.rend());
    {
        std::ostringstream line;
        line << "remove-existing count=" << indexes.size() << " indexes=";
        for (size_t position = 0; position < indexes.size(); ++position) {
            if (position > 0) line << ",";
            line << indexes[position];
        }
        append_stream_capture_diagnostic_log(
            "vdd",
            line.str()); // wjy: 删除前记录驱动中全部Parsec显示索引，确认ToDesk或旧FSRemote是否留下共享VDD实例。
    }
    for (const int index : indexes) {
        append_stream_capture_diagnostic_log(
            "vdd",
            "remove-existing index=" + std::to_string(index)); // wjy: 每个实际移除动作单独记录，复现后可核对是否误删外部远控拥有的显示器。
        parsec_vdd::VddRemoveDisplay(handle, index);
    }
}

bool wait_for_display_ready(int display_index, DisplayInfo* out)
{
    append_stream_capture_diagnostic_log(
        "vdd",
        "wait-ready begin index=" + std::to_string(display_index)
            + " timeout_ms=" + std::to_string(kDisplayEnumerateTimeoutMs)); // wjy: 记录VDD添加后进入Windows显示枚举等待的明确边界。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDisplayEnumerateTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        DisplayInfo info;
        if (enumerate_parsec_display_by_index(display_index, &info) && !info.device_name.empty()) {
            for (int attempt = 0; attempt < 20; ++attempt) {
                if (set_display_mode(info.device_name)) {
                    const bool primary_ready = set_display_primary(info.device_name); // wjy: 保持原有忽略主屏失败的行为，仅把结果写入诊断文件。
                    info.active = true;
                    info.monitor_id = reinterpret_cast<int64_t>(monitor_for_device_name(info.device_name));
                    *out = info;
                    append_stream_capture_diagnostic_log(
                        "vdd",
                        "wait-ready success index=" + std::to_string(display_index)
                            + " attempt=" + std::to_string(attempt + 1)
                            + " device='" + narrow(info.device_name) + "'"
                            + " primary=" + std::to_string(primary_ready ? 1 : 0)
                            + " monitor_id=" + std::to_string(info.monitor_id)); // wjy: 模式、主屏和HMONITOR结果集中记录，直接定位“设备OK但监视器句柄无效”。
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            info.monitor_id = reinterpret_cast<int64_t>(monitor_for_device_name(info.device_name));
            if (info.monitor_id != 0) {
                *out = info;
                append_stream_capture_diagnostic_log(
                    "vdd",
                    "wait-ready monitor-fallback index=" + std::to_string(display_index)
                        + " device='" + narrow(info.device_name) + "'"
                        + " monitor_id=" + std::to_string(info.monitor_id)); // wjy: 分辨率设置连续失败但HMONITOR可用时保留原有回退，并明确标记该异常路径。
                return true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    append_stream_capture_diagnostic_log(
        "vdd",
        "wait-ready timeout index=" + std::to_string(display_index)); // wjy: 五秒枚举失败必须成为持久日志终点，避免控制端只看到“等待远程画面”。
    return false;
}

std::string narrow(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }

    // =====wjy====
    const int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr); // wjy: 按不含终止符的实际字符数分配和转换，避免写入UTF-8终止符时越过string缓冲区。
    return result;
    // ===end====
}

bool driver_ready()
{
    return parsec_vdd::QueryDeviceStatus(&parsec_vdd::VDD_CLASS_GUID, parsec_vdd::VDD_HARDWARE_ID)
        == parsec_vdd::DEVICE_OK;
}

class SharedParsecDisplay {
public:
    ~SharedParsecDisplay()
    {
        shutdown();
    }

    bool acquire(int64_t* preferred_source_id, std::string* preferred_device_name, std::string* error)
    {
        std::lock_guard lock(mutex_);
        append_stream_capture_diagnostic_log(
            "vdd",
            "acquire begin ready=" + std::to_string(ready_ ? 1 : 0)
                + " ref_count=" + std::to_string(ref_count_)
                + " display_index=" + std::to_string(display_index_)); // wjy: 每个首订阅进入VDD前记录共享状态，判断旧会话是否遗留ready或引用计数。

        if (ready_) {
            DisplayInfo info;
            if (!refresh_display_info(&info)) {
                append_stream_capture_diagnostic_log(
                    "vdd",
                    "existing display refresh failed; stopping stale display"); // wjy: 旧共享显示无法重新枚举时标记清理原因，不改变原有stop后重建流程。
                stop_locked();
            } else {
                ++ref_count_;
                publish(info, preferred_source_id, preferred_device_name);
                append_stream_capture_diagnostic_log(
                    "vdd",
                    "acquire reused index=" + std::to_string(display_index_)
                        + " ref_count=" + std::to_string(ref_count_)
                        + " device='" + preferred_device_name_ + "'"); // wjy: 多会话复用时记录已发布设备名，排除重复创建VDD。
                return true;
            }
        }

        const bool parsec_driver_ready = driver_ready();
        append_stream_capture_diagnostic_log(
            "vdd",
            "driver ready=" + std::to_string(parsec_driver_ready ? 1 : 0)); // wjy: PnP显示OK之外再记录parsec-vdd API自己的设备状态判断。
        if (!parsec_driver_ready) {
            if (error) *error = "parsec-vdd driver is not ready";
            return false;
        }

        handle_ = parsec_vdd::OpenDeviceHandle(&parsec_vdd::VDD_ADAPTER_GUID);
        if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
            handle_ = INVALID_HANDLE_VALUE;
            if (error) *error = "parsec-vdd open handle failed";
            append_stream_capture_diagnostic_log(
                "vdd",
                "open-device failed"); // wjy: 驱动状态正常但设备句柄打不开时留下独立阶段，区分权限和驱动占用问题。
            return false;
        }
        append_stream_capture_diagnostic_log(
            "vdd",
            "open-device success handle=" + std::to_string(reinterpret_cast<uintptr_t>(handle_))); // wjy: 句柄值仅用于同一日志内核对生命周期，不包含认证或用户隐私信息。

        remove_existing_parsec_displays(handle_);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        display_index_ = parsec_vdd::VddAddDisplay(handle_);
        append_stream_capture_diagnostic_log(
            "vdd",
            "add-display result index=" + std::to_string(display_index_)); // wjy: 记录驱动返回的显示索引，后续Windows枚举和停止阶段都用同一值核对。
        if (display_index_ < 0) {
            if (error) *error = "parsec-vdd add display failed";
            stop_locked();
            return false;
        }

        heartbeat_running_ = true;
        heartbeat_thread_ = std::thread([this] {
            while (heartbeat_running_) {
                parsec_vdd::VddUpdate(handle_);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        DisplayInfo info;
        if (!wait_for_display_ready(display_index_, &info)) {
            if (error) *error = "parsec-vdd display enumeration timed out";
            stop_locked();
            return false;
        }

        ready_ = true;
        ref_count_ = 1;
        publish(info, preferred_source_id, preferred_device_name);
        append_stream_capture_diagnostic_log(
            "vdd",
            "acquire success index=" + std::to_string(display_index_)
                + " device='" + preferred_device_name_ + "'"
                + " monitor_id=" + std::to_string(preferred_source_id_)); // wjy: VDD最终发布给DXGI的设备名和HMONITOR在同一行落盘。
        return true;
    }

    void release()
    {
        std::lock_guard lock(mutex_);
        // =====wjy====
        if (ref_count_ > 0) {
            --ref_count_;
        }
        append_stream_capture_diagnostic_log(
            "vdd",
            "release ref_count=" + std::to_string(ref_count_)
                + " ready=" + std::to_string(ready_ ? 1 : 0)); // wjy: 会话关闭时确认引用是否真正归零，排查僵尸订阅阻止VDD重建。
        if (ref_count_ == 0 && ready_) {
            stop_locked(); // wjy: 最后一个 HostMediaPipeline 订阅释放后立即移除虚拟显示器，不把空闲 VDD 留到进程退出。
        }
        // ===end====
    }

private:
    void shutdown()
    {
        std::lock_guard lock(mutex_);
        stop_locked();
    }

    bool refresh_display_info(DisplayInfo* out)
    {
        if (display_index_ < 0) {
            return false;
        }

        DisplayInfo info;
        if (!enumerate_parsec_display_by_index(display_index_, &info) || info.device_name.empty()) {
            append_stream_capture_diagnostic_log(
                "vdd",
                "refresh enumerate failed index=" + std::to_string(display_index_)); // wjy: 外部远控改变显示拓扑后若旧索引消失，直接记录失败索引。
            return false;
        }

        const bool mode_ready = set_display_mode(info.device_name);
        const bool primary_ready = set_display_primary(info.device_name);
        info.monitor_id = reinterpret_cast<int64_t>(monitor_for_device_name(info.device_name));
        info.active = info.monitor_id != 0;
        append_stream_capture_diagnostic_log(
            "vdd",
            "refresh index=" + std::to_string(display_index_)
                + " device='" + narrow(info.device_name) + "'"
                + " mode=" + std::to_string(mode_ready ? 1 : 0)
                + " primary=" + std::to_string(primary_ready ? 1 : 0)
                + " monitor_id=" + std::to_string(info.monitor_id)); // wjy: 复用显示时完整记录模式、主屏和句柄状态，行为仍与原来一样继续按HMONITOR判定。
        if (!info.active) {
            return false;
        }

        if (out) {
            *out = info;
        }
        return true;
    }

    void publish(const DisplayInfo& info, int64_t* preferred_source_id, std::string* preferred_device_name)
    {
        preferred_source_id_ = info.monitor_id;
        preferred_device_name_ = narrow(info.device_name);
        if (preferred_source_id) {
            *preferred_source_id = preferred_source_id_;
        }
        if (preferred_device_name) {
            *preferred_device_name = preferred_device_name_;
        }
    }

    void stop_locked()
    {
        append_stream_capture_diagnostic_log(
            "vdd",
            "stop begin index=" + std::to_string(display_index_)
                + " ready=" + std::to_string(ready_ ? 1 : 0)
                + " ref_count=" + std::to_string(ref_count_)); // wjy: VDD清理前保存最后状态，确认应用重启为何不能复原时是否真的执行移除。
        heartbeat_running_ = false;
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }

        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE && display_index_ >= 0) {
            parsec_vdd::VddRemoveDisplay(handle_, display_index_);
        }

        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            parsec_vdd::CloseDeviceHandle(handle_);
        }

        handle_ = INVALID_HANDLE_VALUE;
        display_index_ = -1;
        ready_ = false;
        ref_count_ = 0;
        preferred_source_id_ = 0;
        preferred_device_name_.clear();
        append_stream_capture_diagnostic_log(
            "vdd",
            "stop end"); // wjy: 句柄关闭和状态清零完成后写入终点，异常中断时可与stop begin成对核对。
    }

    std::mutex mutex_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    int display_index_ = -1;
    bool ready_ = false;
    int ref_count_ = 0;
    std::atomic_bool heartbeat_running_ = false;
    std::thread heartbeat_thread_;
    int64_t preferred_source_id_ = 0;
    std::string preferred_device_name_;
};

SharedParsecDisplay& shared_display()
{
    static SharedParsecDisplay instance;
    return instance;
}

} // namespace

class ParsecVddSession::Impl {
public:
    ~Impl()
    {
        stop();
    }

    bool start(std::string* error)
    {
        if (active_) {
            return true;
        }

        if (!shared_display().acquire(&preferred_source_id_, &preferred_device_name_, error)) {
            return false;
        }

        active_ = true;
        return true;
    }

    void stop()
    {
        if (active_) {
            shared_display().release();
        }
        preferred_source_id_ = 0;
        preferred_device_name_.clear();
        active_ = false;
    }

    bool active() const
    {
        return active_;
    }

    int64_t preferred_source_id() const
    {
        return preferred_source_id_;
    }

    const std::string& preferred_device_name() const
    {
        return preferred_device_name_;
    }

private:
    bool active_ = false;
    int64_t preferred_source_id_ = 0;
    std::string preferred_device_name_;
};

ParsecVddSession::ParsecVddSession()
    : impl_(std::make_unique<Impl>())
{
}

ParsecVddSession::~ParsecVddSession() = default;

bool ParsecVddSession::start(std::string* error)
{
    return impl_ && impl_->start(error);
}

void ParsecVddSession::stop()
{
    if (impl_) {
        impl_->stop();
    }
}

bool ParsecVddSession::active() const
{
    return impl_ && impl_->active();
}

int64_t ParsecVddSession::preferred_source_id() const
{
    return impl_ ? impl_->preferred_source_id() : 0;
}

const std::string& ParsecVddSession::preferred_device_name() const
{
    static const std::string empty;
    return impl_ ? impl_->preferred_device_name() : empty;
}

} // namespace uu
