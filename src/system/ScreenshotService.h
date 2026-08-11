#pragma once

#include <QDateTime>
#include <QString>

namespace platform {

// =====wjy====
struct ScreenshotCaptureResult {
    bool success = false;
    QString filePath;
    QString errorMessage;
}; // wjy: 本机和目标端共用同一结果结构，命令层只传最终共享路径，不传输截图像素。

class ScreenshotService final {
public:
    static QString sharedDirectory();
    static QString sanitizeFileNamePart(const QString& value, const QString& fallback);
    static QString defaultFileBaseName(
        const QString& groupName,
        const QString& deviceName,
        const QDateTime& capturedAt = QDateTime::currentDateTime());
    static ScreenshotCaptureResult capturePrimaryScreen(
        const QString& groupName,
        const QString& deviceName);
    static bool isManagedScreenshotPath(const QString& filePath);
}; // wjy: 截图服务只负责目标桌面原始像素采集和固定共享目录文件管理，不依赖Viewer窗口或远控帧。
// ===end====

} // namespace platform
