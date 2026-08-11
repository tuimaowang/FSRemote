#include "system/ScreenshotService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageWriter>
#include <QPixmap>
#include <QSaveFile>
#include <QScreen>

namespace platform {
namespace {

// =====wjy====
constexpr qsizetype kMaximumScreenshotNamePartLength = 80; // wjy: 分组和设备名各自限制长度，避免UNC完整路径超过常见Windows工具兼容范围。

QString normalizedScreenshotPath(const QString& filePath)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(filePath.trimmed())); // wjy: UNC和本机路径统一转成Qt分隔符后清理点段，后续目录边界比较不依赖当前工作目录。
}

QString uniqueScreenshotPath(const QString& baseName)
{
    const QDir directory(ScreenshotService::sharedDirectory());
    QString candidate = directory.filePath(baseName + QStringLiteral(".png"));
    for (int suffix = 2; QFileInfo::exists(candidate); ++suffix) {
        candidate = directory.filePath(QStringLiteral("%1_%2.png").arg(baseName).arg(suffix)); // wjy: 同设备同秒多次截图时保留标准主体，并从_2开始避让已有文件。
    }
    return candidate;
}
// ===end====

} // namespace

// =====wjy====
QString ScreenshotService::sharedDirectory()
{
    return QString::fromUtf8(R"(\\192.168.1.100\ggc\喊话截图)"); // wjy: 本机和所有目标端只允许把F12截图写入这一固定共享目录。
}

QString ScreenshotService::sanitizeFileNamePart(const QString& value, const QString& fallback)
{
    QString safe = value.trimmed();
    static const QString invalidCharacters = QStringLiteral("<>:\"/\\|?*");
    for (QChar& character : safe) {
        if (character.unicode() < 0x20 || invalidCharacters.contains(character)) {
            character = QLatin1Char('_'); // wjy: Windows非法字符和控制字符统一替换，分组名不能构造子目录或破坏UNC路径。
        }
    }
    while (safe.endsWith(QLatin1Char('.')) || safe.endsWith(QLatin1Char(' '))) {
        safe.chop(1); // wjy: Windows文件名不能以点或空格结束，重命名确认时使用同一规则。
    }
    if (safe.isEmpty()) safe = fallback.trimmed();
    if (safe.isEmpty()) safe = QString::fromUtf8("未命名");
    return safe.left(kMaximumScreenshotNamePartLength); // wjy: 截断只作用单个名称段，时间后缀和PNG扩展名始终完整保留。
}

QString ScreenshotService::defaultFileBaseName(
    const QString& groupName,
    const QString& deviceName,
    const QDateTime& capturedAt)
{
    return QStringLiteral("%1_%2_%3")
        .arg(
            sanitizeFileNamePart(groupName, QString::fromUtf8("我的设备")),
            sanitizeFileNamePart(deviceName, QString::fromUtf8("未知设备")),
            capturedAt.toString(QStringLiteral("yyyyMMdd_HHmmss"))); // wjy: 文件主体严格采用“分组_设备_年月日_时分秒”，与用户给出的示例一致。
}

ScreenshotCaptureResult ScreenshotService::capturePrimaryScreen(
    const QString& groupName,
    const QString& deviceName)
{
    ScreenshotCaptureResult result;
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) {
        result.errorMessage = QString::fromUtf8("当前系统没有可用的主屏幕。");
        return result;
    }

    const QPixmap screenshot = screen->grabWindow(0); // wjy: 直接在执行截图的设备上抓取主屏窗口0，像素不经过Viewer编码、缩放或网络传输。
    if (screenshot.isNull()) {
        result.errorMessage = QString::fromUtf8("无法抓取当前主屏幕画面。");
        return result;
    }

    QDir directory(sharedDirectory());
    if (!directory.mkpath(QStringLiteral("."))) {
        result.errorMessage = QString::fromUtf8("无法访问截图共享目录：%1").arg(QDir::toNativeSeparators(sharedDirectory()));
        return result;
    }
    const QString filePath = uniqueScreenshotPath(defaultFileBaseName(groupName, deviceName));
    QString writeError;
    bool removeFailedFile = false;
    {
        QSaveFile output(filePath);
        output.setDirectWriteFallback(true); // wjy: SMB共享不支持原子临时文件重命名时退回直接写入，仍由PNG编码结果决定最终成功状态。
        if (!output.open(QIODevice::WriteOnly)) {
            writeError = QString::fromUtf8("无法创建截图文件：%1").arg(output.errorString());
        } else {
            removeFailedFile = true; // wjy: 只有本次确实打开过输出后才允许失败清理，避免并发设备刚创建同名文件时被误删。
            QImageWriter writer(&output, "png");
            writer.setCompression(3); // wjy: PNG保持无损原图，使用较低压缩级别缩短目标端写入共享目录的等待时间。
            if (!writer.write(screenshot.toImage())) {
                writeError = QString::fromUtf8("无法写入PNG截图：%1").arg(writer.errorString());
                output.cancelWriting(); // wjy: 先标记取消，离开作用域关闭QSaveFile后再删除SMB直接写入留下的半成品。
            } else if (!output.commit()) {
                writeError = QString::fromUtf8("无法提交截图文件：%1").arg(output.errorString());
            }
        }
    } // wjy: QSaveFile在这里先析构关闭句柄，下面删除失败文件时不会被Windows占用规则阻挡。
    if (!writeError.isEmpty()) {
        if (removeFailedFile) QFile::remove(filePath); // wjy: 文件句柄已经关闭后再清理本次失败路径，Windows共享目录不会因占用而残留损坏PNG。
        result.errorMessage = writeError;
        return result;
    }

    result.success = true;
    result.filePath = QDir::toNativeSeparators(filePath); // wjy: 命令返回Windows原生UNC路径，主控可直接加载、重命名或删除同一文件。
    return result;
}

bool ScreenshotService::isManagedScreenshotPath(const QString& filePath)
{
    const QString root = normalizedScreenshotPath(sharedDirectory());
    const QString candidate = normalizedScreenshotPath(filePath);
    if (candidate.isEmpty() || !QDir::isAbsolutePath(candidate)) return false;
    const QFileInfo candidateInfo(candidate);
    const QString parentDirectory = normalizedScreenshotPath(candidateInfo.absolutePath());
    return parentDirectory.compare(root, Qt::CaseInsensitive) == 0
        && candidateInfo.suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0; // wjy: 只允许固定共享目录第一层PNG，子目录、相对路径和命令返回的其它文件全部拒绝。
}
// ===end====

} // namespace platform
