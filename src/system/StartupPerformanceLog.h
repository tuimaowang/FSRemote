#pragma once

#include <QString>

namespace platform {

// =====wjy====
class StartupPerformanceLog final {
public:
    static void checkpoint(const QString& stepName); // wjy: 写入当前步骤相对上一检查点耗时和启动累计耗时。
    static void finish(const QString& finalStepName); // wjy: 写入最后一步后关闭本次启动计时，运行期不再产生额外日志开销。
    static QString logFilePath(); // wjy: 返回 FSRemote.exe/data 中的启动计时日志路径，所有运行日志统一从 data 定位。
    static QString settingsFilePath(); // wjy: 返回 data 中的 INI 开关路径，重启清日志时保留用户设置。
    static bool isEnabled();
};
// ===end====

} // namespace platform
