#include "system/DesktopWallpaperService.h"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace platform {
namespace {

// =====wjy====
QSet<QString> supportedImageSuffixes()
{
    QSet<QString> suffixes;
    const QList<QByteArray> formats = QImageReader::supportedImageFormats(); // wjy: 直接跟随当前 Qt 图片插件能力，避免界面声称支持但运行时无法解码。
    for (const QByteArray& format : formats) {
        suffixes.insert(QString::fromLatin1(format).toLower()); // wjy: 扩展名统一小写后比较，兼容 JPG、Png 等大小写组合。
    }
    return suffixes;
}

bool loadNextDecodableImage(const QString& directoryPath, const QString& previousSourcePath, QString* sourcePath, QImage* image)
{
    const QStringList candidates = DesktopWallpaperService::supportedImageCandidates(directoryPath); // wjy: 候选顺序由单一入口确定，测试和实际轮换结果保持一致。
    if (candidates.isEmpty()) {
        return false;
    }

    int startIndex = 0;
    const QString normalizedPrevious = QDir::cleanPath(previousSourcePath.trimmed());
    if (!normalizedPrevious.isEmpty()) {
        for (int index = 0; index < candidates.size(); ++index) {
            if (QString::compare(QDir::cleanPath(candidates.at(index)), normalizedPrevious, Qt::CaseInsensitive) == 0) {
                startIndex = (index + 1) % candidates.size(); // wjy: 从上次成功图片的下一项开始；最后一项会自然回到首项。
                break;
            }
        }
    }

    for (int offset = 0; offset < candidates.size(); ++offset) {
        const QString& candidate = candidates.at((startIndex + offset) % candidates.size());
        QImageReader reader(candidate);
        reader.setAutoTransform(true); // wjy: 应用 JPEG EXIF 旋转信息，缓存到桌面时方向与普通图片查看器一致。
        QImage decoded = reader.read();
        if (decoded.isNull()) {
            continue; // wjy: 文件扩展名受支持但内容损坏时继续尝试下一张，不让单个坏文件阻塞整个目录。
        }

        if (sourcePath) {
            *sourcePath = candidate; // wjy: 返回真实采用的共享源路径，供成功提示显示文件名。
        }
        if (image) {
            *image = std::move(decoded); // wjy: 把已经解码的像素直接交给缓存步骤，避免再次读取网络文件。
        }
        return true;
    }
    return false;
}
// ===end====

} // namespace

// =====wjy====
QString DesktopWallpaperService::sharedDirectoryPath()
{
    return QString::fromUtf8(R"(\\192.168.1.100\广告部工具\远程软件_桌面)"); // wjy: 使用 UTF-8 原始字符串保存 UNC 路径，中文目录和反斜杠不会被错误转义。
}

QStringList DesktopWallpaperService::supportedImageCandidates(const QString& directoryPath)
{
    const QDir directory(directoryPath);
    if (!directory.exists()) {
        return {}; // wjy: 网络共享不存在或当前账号不可访问时返回空列表，调用方负责给出明确提示。
    }

    const QSet<QString> suffixes = supportedImageSuffixes();
    QStringList candidates;
    const QFileInfoList entries = directory.entryInfoList(QDir::Files | QDir::Readable | QDir::NoSymLinks, QDir::NoSort); // wjy: 只读取目录顶层普通可读文件，不递归也不跟随可能指向其它位置的链接。
    for (const QFileInfo& entry : entries) {
        if (suffixes.contains(entry.suffix().toLower())) {
            candidates.append(entry.absoluteFilePath()); // wjy: 候选保留绝对路径，后续 QImageReader 和成功提示不依赖当前工作目录。
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const QString& left, const QString& right) {
        const QString leftName = QFileInfo(left).fileName();
        const QString rightName = QFileInfo(right).fileName();
        const int insensitive = QString::compare(leftName, rightName, Qt::CaseInsensitive); // wjy: 主排序忽略大小写，让 Windows 共享目录的顺序符合用户看到的文件名顺序。
        return insensitive == 0
            ? QString::compare(leftName, rightName, Qt::CaseSensitive) < 0 // wjy: 仅大小写不同的同名文件再用区分大小写排序，保证每次结果完全稳定。
            : insensitive < 0;
    });
    return candidates;
}

QString DesktopWallpaperService::firstDecodableImage(const QString& directoryPath)
{
    return nextDecodableImage(directoryPath, QString()); // wjy: 首次选择等价于没有历史源路径的下一张，继续保留原测试入口语义。
}

QString DesktopWallpaperService::nextDecodableImage(const QString& directoryPath, const QString& previousSourcePath)
{
    QString sourcePath;
    return loadNextDecodableImage(directoryPath, previousSourcePath, &sourcePath, nullptr) ? sourcePath : QString(); // wjy: 无副作用选择接口只做目录枚举和解码，不写缓存也不修改真实桌面。
}

QImage DesktopWallpaperService::composeDeviceNameOverlay(const QImage& sourceImage, const QString& deviceName)
{
    if (sourceImage.isNull()) {
        return sourceImage; // wjy: 防御性保留空图语义，调用方可以继续按原有解码失败流程处理。
    }

    const QString normalizedDeviceName = deviceName.trimmed(); // wjy: 去掉设备名首尾空白，避免右上角出现无意义留白或把纯空格当作有效名称。
    if (normalizedDeviceName.isEmpty()) {
        return sourceImage; // wjy: 名称确实不可用时保持原图，不绘制旧的固定“99”或空白占位图形。
    }

    QImage composedImage = sourceImage.convertToFormat(QImage::Format_ARGB32_Premultiplied); // wjy: 只修改解码后的内存副本，绝不覆盖共享目录中的源文件。
    const int shorterSide = qMin(composedImage.width(), composedImage.height());
    const int margin = qMax(12, qRound(shorterSide * 0.025)); // wjy: 右上角边距按图片比例计算，并保留至少 12 像素避免贴边。
    int fontPixelSize = qBound(24, qRound(shorterSide * 0.13), 180); // wjy: 初始字号沿用数字测试的醒目比例，普通长度机器名仍清晰可见。
    const int minimumFontPixelSize = qBound(16, qRound(shorterSide * 0.045), 40); // wjy: 长名称允许缩小但保留可读下限，避免为了完整显示而变成细小文字。
    const qreal availableWidth = qMax<qreal>(1.0, composedImage.width() - margin * 2.0); // wjy: 无论图片多窄都保留左右安全边距，文字路径不能超出画布。
    const qreal maximumLabelWidth = qMin<qreal>(availableWidth, composedImage.width() * 0.70); // wjy: 名称最多占七成宽度，使它仍然保持右上角标识而不是横跨整张壁纸。

    QFont deviceNameFont(QStringLiteral("Segoe UI"));
    deviceNameFont.setWeight(QFont::Bold); // wjy: 使用 Windows 常见粗体字体绘制设备名，Qt 会为中文等缺失字形自动选择回退字体。

    QString renderedDeviceName = normalizedDeviceName;
    QPainterPath deviceNamePath;
    QRectF deviceNameBounds;
    auto rebuildDeviceNamePath = [&] {
        deviceNameFont.setPixelSize(fontPixelSize); // wjy: 每次调整字号后重新生成字形路径，宽度判断与最终绘制使用完全相同的字体状态。
        deviceNamePath = QPainterPath();
        deviceNamePath.addText(0, 0, deviceNameFont, renderedDeviceName); // wjy: 设备名先转成矢量路径，后续白色填充和深色描边会一起烘焙进壁纸像素。
        deviceNameBounds = deviceNamePath.boundingRect();
    };
    rebuildDeviceNamePath();
    while (deviceNameBounds.width() > maximumLabelWidth && fontPixelSize > minimumFontPixelSize) {
        --fontPixelSize; // wjy: 优先逐级缩小并保留完整设备名，只有到达可读下限仍放不下时才省略中间内容。
        rebuildDeviceNamePath();
    }
    if (deviceNameBounds.width() > maximumLabelWidth) {
        const QFontMetrics metrics(deviceNameFont);
        renderedDeviceName = metrics.elidedText(renderedDeviceName, Qt::ElideMiddle, qFloor(maximumLabelWidth)); // wjy: 超长名称保留首尾辨识信息，中间用省略号收紧到右上角安全宽度。
        rebuildDeviceNamePath();
    }
    if (deviceNameBounds.isEmpty()) {
        return composedImage;
    }

    const qreal targetLeft = composedImage.width() - margin - deviceNameBounds.width();
    const qreal targetTop = margin;
    deviceNamePath.translate(targetLeft - deviceNameBounds.left(), targetTop - deviceNameBounds.top()); // wjy: 用真实字形边界对齐右上角，避免字体基线导致设备名上下偏移。

    QPainter painter(&composedImage);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    const qreal outlineWidth = qMax<qreal>(2.0, fontPixelSize * 0.075);
    painter.setPen(QPen(QColor(0, 0, 0, 220), outlineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); // wjy: 深色圆角描边保证白色设备名在浅色照片区域仍可辨认。
    painter.setBrush(QColor(255, 255, 255));
    painter.drawPath(deviceNamePath); // wjy: 一次绘制同时生成白色填充和深色外轮廓，结果成为目标设备自己的本地壁纸像素。
    painter.end();
    return composedImage;
}

DesktopWallpaperApplyResult DesktopWallpaperService::applyFirstSharedImage(const QString& deviceName)
{
    return applyNextSharedImage(QString(), deviceName); // wjy: 首次调用采用排序后的第一张可解码图片，并使用调用端确认的目标机器名。
}

DesktopWallpaperApplyResult DesktopWallpaperService::applyNextSharedImage(const QString& previousSourcePath, const QString& deviceName)
{
    DesktopWallpaperApplyResult result;
    const QString sourceDirectory = sharedDirectoryPath();
    const QDir directory(sourceDirectory);
    if (!directory.exists()) {
        result.errorMessage = QString::fromUtf8("无法访问桌面图片目录：%1").arg(sourceDirectory); // wjy: 在网络目录不可达时立即停止，绝不调用 Windows 壁纸接口。
        return result;
    }

    QImage image;
    if (!loadNextDecodableImage(sourceDirectory, previousSourcePath, &result.sourcePath, &image)) {
        result.errorMessage = QString::fromUtf8("目录中没有可用图片：%1").arg(sourceDirectory); // wjy: 空目录、扩展名不支持和全部图片损坏统一给出可操作提示。
        return result;
    }
    image = composeDeviceNameOverlay(image, deviceName); // wjy: 在创建 current.bmp 前把目标设备名合成为右上角像素，共享源图、图片数量和轮换顺序均保持不变。

    const QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dataRoot.isEmpty()) {
        result.errorMessage = QString::fromUtf8("无法确定本地壁纸缓存目录。");
        return result;
    }

    const QString cacheDirectory = QDir(dataRoot).filePath(QStringLiteral("wallpaper"));
    if (!QDir().mkpath(cacheDirectory)) {
        result.errorMessage = QString::fromUtf8("无法创建本地壁纸缓存目录：%1").arg(cacheDirectory); // wjy: 缓存目录创建失败时保留原桌面，不尝试把 UNC 路径直接交给系统。
        return result;
    }

    result.cachedPath = QDir(cacheDirectory).filePath(QStringLiteral("current.bmp"));
    QSaveFile cachedFile(result.cachedPath);
    if (!cachedFile.open(QIODevice::WriteOnly)
        || !image.save(&cachedFile, "BMP")
        || !cachedFile.commit()) {
        result.errorMessage = QString::fromUtf8("无法写入本地壁纸缓存：%1").arg(result.cachedPath); // wjy: 原子写入失败不会留下半张 BMP，也不会请求系统切换壁纸。
        return result;
    }

#if defined(_WIN32)
    const std::wstring nativePath = QDir::toNativeSeparators(result.cachedPath).toStdWString();
    if (!SystemParametersInfoW(
            SPI_SETDESKWALLPAPER,
            0,
            const_cast<wchar_t*>(nativePath.c_str()),
            SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) { // wjy: 写入当前用户设置并广播桌面刷新，让资源管理器立即显示新壁纸。
        result.errorMessage = QString::fromUtf8("Windows 设置桌面壁纸失败（错误码 %1）。").arg(GetLastError());
        return result;
    }

    result.success = true; // wjy: 只有 Windows API 明确返回成功才允许 UI 展示设置完成。
#else
    result.errorMessage = QString::fromUtf8("当前系统不支持 Windows 桌面壁纸测试。"); // wjy: 非 Windows 构建保留可编译路径，但不伪装设置成功。
#endif
    return result;
}
// ===end====

} // namespace platform
