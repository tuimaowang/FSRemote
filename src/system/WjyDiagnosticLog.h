#pragma once

#include <QString>

namespace platform {

// =====wjy====
void writeWjyDiagnosticLog(const QString& message); // wjy: 统一写入 WJY 诊断日志，内部负责加锁、首次清空和追加写入。
// ===end====

} // namespace platform
