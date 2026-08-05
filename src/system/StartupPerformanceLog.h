#pragma once

#include <QString>

namespace platform {

// =====wjy====
class StartupPerformanceLog final {
public:
    static void checkpoint(const QString& stepName); // wjy: 写入当前步骤相对上一检查点耗时和启动累计耗时。
    static void finish(const QString& finalStepName); // wjy: 写入最后一步后关闭本次启动计时，运行期不再产生额外日志开销。
    static QString logFilePath(); // wjy: 返回可执行文件目录中的启动计时日志路径，便于界面外直接定位。
    static QString settingsFilePath(); // wjy: 返回同目录 INI 开关路径，后续无需重新编译即可关闭日志。
    static bool isEnabled();
};
// ===end====

} // namespace platform
