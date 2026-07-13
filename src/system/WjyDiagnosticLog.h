#pragma once

#include <QString>

namespace platform {

// =====wjy====
void writeWjyDiagnosticLog(const QString& message); // wjy: 统一追加到 LOCALAPPDATA 诊断日志，使用 Win32 立即落盘以覆盖卡死和强杀场景。
// ===end====

} // namespace platform
