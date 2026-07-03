#include "system/WjyDiagnosticLog.h"

namespace platform {

void writeWjyDiagnosticLog(const QString& message)
{
    (void)message; // wjy: 堆损坏定位期间禁用诊断日志写文件，避免 QFile/QTextStream 参与崩溃路径。
}

} // namespace platform
