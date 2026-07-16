#pragma once

#include <QString>

namespace platform {

// =====wjy====
void writeWjyDiagnosticLog(const QString& message); // wjy: 保留兼容调用入口，当前为空实现，不再生成统一诊断日志。
// ===end====

} // namespace platform
