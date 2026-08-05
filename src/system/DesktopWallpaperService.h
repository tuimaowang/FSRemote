#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

namespace platform {

// =====wjy====
struct DesktopWallpaperApplyResult {
    bool success = false; // wjy: 只有本地 BMP 已写入且 Windows 接受壁纸切换请求时才为 true。
    QString sourcePath; // wjy: 记录最终采用的共享图片，供设置页向用户反馈具体文件名。
    QString cachedPath; // wjy: 记录稳定的本地 BMP 路径，便于后续自动轮播复用和问题定位。
    QString errorMessage; // wjy: 任一前置步骤失败时返回可直接展示的中文原因，避免 UI 猜测失败阶段。
};

class DesktopWallpaperService final {
public:
    static QString sharedDirectoryPath(); // wjy: 测试阶段固定返回用户指定的共享图片目录。
    static QStringList supportedImageCandidates(const QString& directoryPath); // wjy: 只筛选顶层受支持图片并按文件名稳定排序，供生产逻辑和测试共同使用。
    static QString firstDecodableImage(const QString& directoryPath); // wjy: 跳过扩展名正确但内容损坏的文件，返回第一张真正可解码的图片。
    static QString nextDecodableImage(const QString& directoryPath, const QString& previousSourcePath); // wjy: 从上次成功图片之后继续查找，跳过坏图并在目录末尾回绕。
    static QImage composeDeviceNameOverlay(const QImage& sourceImage, const QString& deviceName); // wjy: 把目标机器名绘制到图片副本右上角，返回可直接写入壁纸缓存的完整像素图。
    static DesktopWallpaperApplyResult applyFirstSharedImage(const QString& deviceName); // wjy: 使用目标机器名完成首张共享图片选择、本地 BMP 缓存和当前 Windows 桌面切换。
    static DesktopWallpaperApplyResult applyNextSharedImage(const QString& previousSourcePath, const QString& deviceName); // wjy: 自动轮换携带目标机器名，同时复用稳定 BMP 缓存与 Windows API。
};
// ===end====

} // namespace platform
