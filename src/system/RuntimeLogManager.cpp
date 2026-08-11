#include "system/RuntimeLogManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

namespace platform {
namespace {

// =====wjy====
bool isRuntimeLogFileName(const QString& fileName)
{
    const QString lowerName = fileName.toLower(); // wjy: Windows 文件名大小写不敏感，统一转小写后识别当前日志和轮转备份。
    return lowerName.endsWith(QStringLiteral(".log"))
        || lowerName.contains(QStringLiteral(".log."))
        || lowerName.endsWith(QStringLiteral(".jsonl")); // wjy: 视频管线逐行 JSON 也是运行诊断，随每次主程序重启一起清空。
}

void removeFileIfPresent(const QString& filePath, RuntimeLogResetResult& result)
{
    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return;
    }
    if (QFile::remove(fileInfo.absoluteFilePath())) {
        ++result.removedFileCount; // wjy: 只统计真实存在且已成功删除的文件，不把不存在的旧路径算作已清理。
    } else {
        ++result.failedFileCount; // wjy: 保留失败计数供新日志提示，不能因诊断文件被占用而中止远控服务。
    }
}

void removeRuntimeLogsRecursively(const QString& rootPath, RuntimeLogResetResult& result)
{
    if (rootPath.trimmed().isEmpty() || !QFileInfo::exists(rootPath)) {
        return;
    }
    QDirIterator iterator(
        rootPath,
        QDir::Files | QDir::NoSymLinks,
        QDirIterator::Subdirectories); // wjy: 只遍历普通文件，绝不跟随 data 内的目录链接越界删除共享目录内容。
    while (iterator.hasNext()) {
        const QFileInfo fileInfo(iterator.next());
        if (isRuntimeLogFileName(fileInfo.fileName())) {
            removeFileIfPresent(fileInfo.absoluteFilePath(), result); // wjy: data 下仅按日志扩展名删除，devices.json、密钥、脚本状态和用户文件全部保留。
        }
    }
}

void removeNamedFilesRecursively(
    const QString& rootPath,
    const QStringList& fileNames,
    RuntimeLogResetResult& result)
{
    if (rootPath.trimmed().isEmpty() || !QFileInfo::exists(rootPath)) {
        return;
    }
    QDirIterator iterator(
        rootPath,
        QDir::Files | QDir::NoSymLinks,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QFileInfo fileInfo(iterator.next());
        if (fileNames.contains(fileInfo.fileName(), Qt::CaseInsensitive)) {
            removeFileIfPresent(fileInfo.absoluteFilePath(), result); // wjy: work 目录只清理旧版生成的两个固定日志，不触碰脚本、配置和执行状态文件。
        }
    }
}

void migrateStartupTimingSettings(const QString& dataPath, RuntimeLogResetResult& result)
{
    const QString legacyPath = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("FSRemote_startup_timing.ini"));
    const QString targetPath = QDir(dataPath)
        .filePath(QStringLiteral("FSRemote_startup_timing.ini"));
    if (!QFileInfo::exists(legacyPath)) {
        return;
    }
    if (!QFileInfo::exists(targetPath)) {
        if (QFile::rename(legacyPath, targetPath)) {
            return; // wjy: 同卷安装目录优先原子迁移，保留用户已经设置的 StartupTiming/Enabled 值。
        }
        if (QFile::copy(legacyPath, targetPath)) {
            removeFileIfPresent(legacyPath, result); // wjy: 重命名失败时复制后删除旧文件，兼容权限或文件系统限制。
            return;
        }
        ++result.failedFileCount;
        return;
    }
    removeFileIfPresent(legacyPath, result); // wjy: data 中已有新配置时以新位置为准，清除根目录重复副本避免用户改错文件。
}

void removeLegacyAppDataLogs(RuntimeLogResetResult& result)
{
    const QString appLocalDataPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QStringList legacyRoots;
    if (!appLocalDataPath.isEmpty()) {
        legacyRoots.append(QDir::cleanPath(appLocalDataPath));
        QDir parentDirectory(appLocalDataPath);
        if (parentDirectory.cdUp()
            && QFileInfo(parentDirectory.absolutePath()).fileName().compare(
                   QStringLiteral("FSRemote"), Qt::CaseInsensitive) == 0) {
            legacyRoots.append(QDir::cleanPath(parentDirectory.absolutePath())); // wjy: 兼容旧版本只设置应用名时产生的 %LOCALAPPDATA%/FSRemote 日志根目录。
        }
    }
    legacyRoots.removeDuplicates();
    for (const QString& rootPath : legacyRoots) {
        removeFileIfPresent(
            QDir(rootPath).filePath(QStringLiteral("fsremote_diagnostic.log")),
            result); // wjy: 旧诊断文件使用固定名称，不能递归删除 AppData 中的其它配置或缓存。
        removeFileIfPresent(
            QDir(rootPath).filePath(QStringLiteral("fsremote_diagnostic.log.1")),
            result);
        removeRuntimeLogsRecursively(
            QDir(rootPath).filePath(QStringLiteral("Updates")),
            result); // wjy: 旧更新任务目录只删除日志扩展名文件，暂存包和回滚数据仍由更新服务自己的生命周期管理。
    }
}

void removeLegacyTemporaryLogs(RuntimeLogResetResult& result)
{
    QDir temporaryDirectory(QDir::tempPath());
    const QFileInfoList entries = temporaryDirectory.entryInfoList(
        {QStringLiteral("fsremote_input_debug.log"),
         QStringLiteral("fsremote_script_output_*.log")},
        QDir::Files | QDir::NoSymLinks); // wjy: 只匹配 FSRemote 旧版明确创建的临时日志，绝不扫描删除其它程序的临时文件。
    for (const QFileInfo& fileInfo : entries) {
        removeFileIfPresent(fileInfo.absoluteFilePath(), result);
    }
}
// ===end====

} // namespace

// =====wjy====
QString RuntimeLogManager::dataDirectory()
{
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
    QDir().mkpath(path); // wjy: 各日志模块可独立调用该入口；目录创建失败时仍返回唯一目标路径，不允许回退到 Temp 或 AppData。
    return QDir::cleanPath(path);
}

RuntimeLogResetResult RuntimeLogManager::resetForPrimaryProcessStart()
{
    RuntimeLogResetResult result;
    const QString dataPath = dataDirectory();
    result.dataDirectoryReady = QFileInfo(dataPath).isDir(); // wjy: 单实例确认后首先固定本轮日志根目录，再开始任何文件日志写入。
    if (result.dataDirectoryReady) {
        removeRuntimeLogsRecursively(dataPath, result); // wjy: 每次真正主程序启动删除 data 及其子目录中的上一轮日志，非日志数据保持不变。
        migrateStartupTimingSettings(dataPath, result);
    } else {
        ++result.failedFileCount;
    }

    const QDir executableDirectory(QCoreApplication::applicationDirPath());
    const QStringList legacyRootLogNames{
        QStringLiteral("FSRemote_startup_timing.log"),
        QStringLiteral("stream_host.log"),
        QStringLiteral("stream_viewer_debug.log"),
        QStringLiteral("stream_lifecycle.log"),
        QStringLiteral("fsremote_input_debug.log"),
    };
    for (const QString& fileName : legacyRootLogNames) {
        removeFileIfPresent(executableDirectory.filePath(fileName), result); // wjy: 清理曾经写在 EXE 根目录的固定日志，之后所有新写入都只进入 data。
    }

    removeLegacyTemporaryLogs(result);
    removeLegacyAppDataLogs(result);
    removeNamedFilesRecursively(
        executableDirectory.filePath(QStringLiteral("work")),
        {QStringLiteral("fsremote_robocopy.log"),
         QStringLiteral("fsremote_script_run.log")},
        result);
    return result;
}
// ===end====

} // namespace platform
