#include "system/DesktopWallpaperService.h"

#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>

#include <cassert>

namespace {

// =====wjy====
void writeTestImage(const QString& path, QRgb color)
{
    QImage image(4, 4, QImage::Format_ARGB32);
    image.fill(color); // wjy: 生成真实可解码像素文件，让测试覆盖生产 QImageReader 而不是伪造扩展名列表。
    assert(image.save(path));
}

void testEmptyDirectory()
{
    QTemporaryDir directory;
    assert(directory.isValid());
    assert(platform::DesktopWallpaperService::supportedImageCandidates(directory.path()).isEmpty()); // wjy: 空目录不能产生候选，也不能误触发任何桌面修改。
    assert(platform::DesktopWallpaperService::firstDecodableImage(directory.path()).isEmpty());
}

void testSupportedFilesUseStableFilenameOrder()
{
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString firstPath = directory.filePath(QStringLiteral("A-first.bmp"));
    const QString lastPath = directory.filePath(QStringLiteral("z-last.png"));
    writeTestImage(lastPath, qRgb(0, 0, 255));
    writeTestImage(firstPath, qRgb(255, 0, 0));

    QFile ignored(directory.filePath(QStringLiteral("middle.txt")));
    assert(ignored.open(QIODevice::WriteOnly));
    ignored.write("not an image candidate"); // wjy: 不支持扩展名即使存在内容也必须在解码前被过滤。
    ignored.close();

    const QStringList candidates = platform::DesktopWallpaperService::supportedImageCandidates(directory.path());
    assert(candidates.size() == 2);
    assert(candidates.at(0) == firstPath); // wjy: 创建顺序与期望顺序相反，确认结果确实按文件名而不是目录枚举顺序排列。
    assert(candidates.at(1) == lastPath);
    assert(platform::DesktopWallpaperService::firstDecodableImage(directory.path()) == firstPath);
}

void testDamagedImageIsSkipped()
{
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString brokenPath = directory.filePath(QStringLiteral("A-broken.png"));
    QFile broken(brokenPath);
    assert(broken.open(QIODevice::WriteOnly));
    broken.write("broken png"); // wjy: 扩展名受支持但内容损坏，生产逻辑应继续尝试下一张而不是直接失败。
    broken.close();

    const QString validPath = directory.filePath(QStringLiteral("B-valid.bmp"));
    writeTestImage(validPath, qRgb(0, 255, 0));
    assert(platform::DesktopWallpaperService::firstDecodableImage(directory.path()) == validPath);
}

void testNextImageAdvancesAndWraps()
{
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString firstPath = directory.filePath(QStringLiteral("A-first.bmp"));
    const QString secondPath = directory.filePath(QStringLiteral("B-second.bmp"));
    const QString lastPath = directory.filePath(QStringLiteral("C-last.bmp"));
    writeTestImage(firstPath, qRgb(255, 0, 0));
    writeTestImage(secondPath, qRgb(0, 255, 0));
    writeTestImage(lastPath, qRgb(0, 0, 255));

    assert(platform::DesktopWallpaperService::nextDecodableImage(directory.path(), QString()) == firstPath); // wjy: 没有成功历史时从稳定排序首项开始。
    assert(platform::DesktopWallpaperService::nextDecodableImage(directory.path(), firstPath) == secondPath); // wjy: 正常轮换必须选择紧随上次成功源之后的图片。
    assert(platform::DesktopWallpaperService::nextDecodableImage(directory.path(), secondPath) == lastPath);
    assert(platform::DesktopWallpaperService::nextDecodableImage(directory.path(), lastPath) == firstPath); // wjy: 到达末尾后回绕首项，保证定时轮换可以持续运行。
}

void testNextImageSkipsDamagedCandidate()
{
    QTemporaryDir directory;
    assert(directory.isValid());
    const QString firstPath = directory.filePath(QStringLiteral("A-first.bmp"));
    const QString brokenPath = directory.filePath(QStringLiteral("B-broken.png"));
    const QString lastPath = directory.filePath(QStringLiteral("C-last.bmp"));
    writeTestImage(firstPath, qRgb(255, 0, 0));
    QFile broken(brokenPath);
    assert(broken.open(QIODevice::WriteOnly));
    broken.write("broken png");
    broken.close();
    writeTestImage(lastPath, qRgb(0, 0, 255));

    assert(platform::DesktopWallpaperService::nextDecodableImage(directory.path(), firstPath) == lastPath); // wjy: 下一项损坏时继续向后解码，不让坏图中断整个自动周期。
    assert(platform::DesktopWallpaperService::nextDecodableImage(directory.path(), lastPath) == firstPath);
}

void testDeviceNameOverlayIsFlattenedIntoUpperRightPixels()
{
    QImage sourceImage(640, 360, QImage::Format_ARGB32);
    sourceImage.fill(qRgb(36, 94, 150));
    const QImage originalImage = sourceImage.copy();

    const QImage composedImage = platform::DesktopWallpaperService::composeDeviceNameOverlay(sourceImage, QStringLiteral("TARGET-PC-99"));
    assert(!composedImage.isNull());
    assert(composedImage.size() == sourceImage.size()); // wjy: 设备名合成不能改变壁纸尺寸，否则 Windows 原有填充/适应策略会出现额外缩放。
    assert(sourceImage == originalImage); // wjy: 合成函数只返回新像素图，输入图像及其对应共享源文件保持不变。

    bool upperRightChanged = false;
    for (int y = 0; y < composedImage.height() / 2 && !upperRightChanged; ++y) {
        for (int x = composedImage.width() / 2; x < composedImage.width(); ++x) {
            if (composedImage.pixel(x, y) != originalImage.pixel(x, y)) {
                upperRightChanged = true;
                break;
            }
        }
    }
    assert(upperRightChanged); // wjy: 设备名的填充或描边必须真实改变右上角像素，证明它不是独立悬浮控件。

    for (int y = composedImage.height() / 2; y < composedImage.height(); ++y) {
        for (int x = 0; x < composedImage.width() / 2; ++x) {
            assert(composedImage.pixel(x, y) == originalImage.pixel(x, y)); // wjy: 左下区域应完全保留原图，防止绘制坐标或画布转换污染整张图片。
        }
    }
}

void testOverlayPixelsFollowDeviceNameAndTrimWhitespace()
{
    QImage sourceImage(800, 450, QImage::Format_ARGB32);
    sourceImage.fill(qRgb(24, 60, 96));

    const QImage firstName = platform::DesktopWallpaperService::composeDeviceNameOverlay(sourceImage, QStringLiteral("DEVICE-A"));
    const QImage paddedFirstName = platform::DesktopWallpaperService::composeDeviceNameOverlay(sourceImage, QStringLiteral("  DEVICE-A  "));
    const QImage secondName = platform::DesktopWallpaperService::composeDeviceNameOverlay(sourceImage, QStringLiteral("DEVICE-B"));
    const QImage longName = platform::DesktopWallpaperService::composeDeviceNameOverlay(
        sourceImage,
        QStringLiteral("VERY-LONG-TARGET-DEVICE-NAME-THAT-MUST-STAY-INSIDE-THE-WALLPAPER"));

    assert(firstName == paddedFirstName); // wjy: 设备名首尾空白不应改变最终壁纸，避免同一目标产生视觉上不同的缓存图。
    assert(firstName != secondName); // wjy: 不同目标设备名必须生成不同像素，确认实现已不再固定绘制“99”。
    assert(longName.size() == sourceImage.size());
    assert(longName != sourceImage); // wjy: 超长名称经过缩小或居中省略后仍要成功烘焙，且不能扩展或裁切原图画布。
    assert(platform::DesktopWallpaperService::composeDeviceNameOverlay(sourceImage, QStringLiteral("   ")) == sourceImage); // wjy: 纯空白名称不绘制任何占位文字，更不会回退成旧数字。
}

void testDigitPrefixedDeviceNameEnablesRotationByDefault()
{
    assert(platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(QStringLiteral("99-DESKTOP"))); // wjy: 常见数字编号设备在没有历史设置时默认启动自动壁纸。
    assert(platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(QStringLiteral("  7-PC  "))); // wjy: 防御性忽略名称首尾空白后仍按真实首字符判断。
    assert(!platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(QStringLiteral("PC-99"))); // wjy: 数字不在首位时不能误开启默认轮换。
    assert(!platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(QString::fromUtf8("设备99"))); // wjy: 中文或其它非数字首字符继续沿用默认关闭。
    assert(!platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(QString())); // wjy: 无法读取设备名时保持安全的关闭默认值。
}
// ===end====

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv); // wjy: 设备名路径需要 Qt 字体引擎，使用无窗口 GUI 应用初始化字体和图片插件但不创建真实桌面窗口。
    testEmptyDirectory();
    testSupportedFilesUseStableFilenameOrder();
    testDamagedImageIsSkipped();
    testNextImageAdvancesAndWraps();
    testNextImageSkipsDamagedCandidate();
    testDeviceNameOverlayIsFlattenedIntoUpperRightPixels();
    testOverlayPixelsFollowDeviceNameAndTrimWhitespace();
    testDigitPrefixedDeviceNameEnablesRotationByDefault(); // wjy: 回归验证设备名默认开启策略，不读取注册表也不修改真实桌面。
    return 0;
}
