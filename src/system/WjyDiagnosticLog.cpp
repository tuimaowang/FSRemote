#include "system/WjyDiagnosticLog.h"

namespace platform {

void writeWjyDiagnosticLog(const QString& message)
{
    (void)message; // wjy: 统一诊断日志已停用，不再创建文件、加锁、写盘或强制刷新。
}

} // namespace platform
