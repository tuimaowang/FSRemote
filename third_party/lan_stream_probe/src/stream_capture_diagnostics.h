#pragma once

#include <cstdint>
#include <string_view>

namespace lsp {

// =====wjy====
void reset_stream_capture_diagnostic_log(); // wjy: 每次被控端 Host 启动时清空旧采集日志，使复现文件只保留本轮桌面媒体链路。
void append_stream_capture_diagnostic_log(
    std::string_view component,
    std::string_view message); // wjy: 统一写入带时间、进程和线程信息的被控端采集诊断行。
void append_stream_capture_diagnostic_log_rate_limited(
    std::string_view component,
    std::string_view message,
    uint32_t interval_ms); // wjy: 高频重复错误按“组件+消息”限频，防止60 FPS失败循环反向制造磁盘卡顿。
// ===end====

} // namespace lsp
