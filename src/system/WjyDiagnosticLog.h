#pragma once

#include <QString>

namespace platform {

// =====wjy====
void writeWjyDiagnosticLog(const QString& message); // wjy: 写入有4MB硬上限的低频轮转诊断日志，禁止在逐帧热路径调用。
// ===end====

} // namespace platform
