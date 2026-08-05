#include "stream_capture_diagnostics.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lsp {
namespace {

// =====wjy====
std::mutex g_capture_log_mutex; // wjy: VDD、DXGI、WebRTC和NVENC来自不同线程，统一串行写入避免日志行互相穿插。
std::unordered_map<std::string, uint64_t> g_capture_log_last_write_ms; // wjy: 保存每类重复诊断最近写入时刻，只影响日志频率，不改变媒体行为。

std::wstring capture_log_path()
{
    wchar_t executable_path[MAX_PATH] = {}; // wjy: 日志始终跟随实际运行的FSRemote.exe，便于从目标设备安装目录直接取回。
    if (::GetModuleFileNameW(nullptr, executable_path, MAX_PATH) == 0) {
        return {};
    }

    std::wstring path(executable_path);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    path.resize(slash + 1);
    path += L"data";
    ::CreateDirectoryW(path.c_str(), nullptr); // wjy: 新安装目录首次远控时允许诊断模块自行建立data目录。
    path += L"\\stream_capture_debug.log";
    return path;
}

void append_stream_capture_diagnostic_log_locked(
    std::string_view component,
    std::string_view message)
{
    const std::wstring path = capture_log_path();
    if (path.empty()) {
        return;
    }

    SYSTEMTIME local_time = {};
    ::GetLocalTime(&local_time);
    char prefix[192] = {};
    std::snprintf(
        prefix,
        sizeof(prefix),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u uptime_ms=%llu pid=%lu tid=%lu [%.*s] ",
        local_time.wYear,
        local_time.wMonth,
        local_time.wDay,
        local_time.wHour,
        local_time.wMinute,
        local_time.wSecond,
        local_time.wMilliseconds,
        static_cast<unsigned long long>(::GetTickCount64()),
        static_cast<unsigned long>(::GetCurrentProcessId()),
        static_cast<unsigned long>(::GetCurrentThreadId()),
        static_cast<int>(component.size()),
        component.data()); // wjy: 同时记录墙钟、开机单调时间和线程身份，便于对齐ToDesk断开与各媒体阶段。

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || !file) {
        return;
    }
    fwrite(prefix, 1, std::char_traits<char>::length(prefix), file);
    fwrite(message.data(), 1, message.size(), file);
    fwrite("\r\n", 1, 2, file);
    fclose(file); // wjy: 每条关键诊断立即关闭文件，目标异常重启或整机重启时仍保留最后完成的阶段。
}
// ===end====

} // namespace

// =====wjy====
void reset_stream_capture_diagnostic_log()
{
    std::lock_guard lock(g_capture_log_mutex);
    g_capture_log_last_write_ms.clear(); // wjy: 新Host代际重新允许首条重复错误立即落盘，不继承上一轮限频状态。
    const std::wstring path = capture_log_path();
    if (path.empty()) {
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") == 0 && file) {
        fclose(file); // wjy: 仅Host启动时截断旧文件，不在每个Viewer连接时覆盖同一轮复现上下文。
    }
}

void append_stream_capture_diagnostic_log(
    std::string_view component,
    std::string_view message)
{
    std::lock_guard lock(g_capture_log_mutex);
    append_stream_capture_diagnostic_log_locked(component, message); // wjy: 非高频阶段无条件写入，保证开始/成功/失败边界完整可见。
}

void append_stream_capture_diagnostic_log_rate_limited(
    std::string_view component,
    std::string_view message,
    uint32_t interval_ms)
{
    const std::string key = std::string(component) + '\n' + std::string(message); // wjy: 相同组件中的不同HRESULT和阶段分别限频，关键错误不会互相遮挡。
    const uint64_t now_ms = ::GetTickCount64();
    std::lock_guard lock(g_capture_log_mutex);
    const auto previous = g_capture_log_last_write_ms.find(key);
    if (previous != g_capture_log_last_write_ms.end()
        && now_ms - previous->second < interval_ms) {
        return;
    }
    g_capture_log_last_write_ms[key] = now_ms;
    append_stream_capture_diagnostic_log_locked(component, message); // wjy: 到达时间窗后写一条最新状态，持续故障仍能观察但不会逐帧刷盘。
}
// ===end====

} // namespace lsp
