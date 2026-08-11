#pragma once

#include <QString>

namespace platform {

// =====wjy====
struct RuntimeLogResetResult {
    int removedFileCount = 0; // wjy: 统计本次主实例启动实际删除的当前和旧版日志文件数量，供启动诊断确认清理已执行。
    int failedFileCount = 0; // wjy: 文件仍被其它进程占用或目录无写权限时累计失败数量，但日志问题不能阻断主程序启动。
    bool dataDirectoryReady = false; // wjy: 明确记录 FSRemote.exe/data 是否可用，避免各模块在目录创建失败后回退到临时目录。
};

class RuntimeLogManager final {
public:
    static QString dataDirectory(); // wjy: 所有运行日志统一以 FSRemote.exe/data 为根目录，允许各模块在其下建立独立子目录。
    static RuntimeLogResetResult resetForPrimaryProcessStart(); // wjy: 仅由成功取得单实例服务的主进程调用，删除上一轮日志并清理已知旧路径。
};
// ===end====

} // namespace platform
