#include "system/UpdateService.h"
#include "system/SharedStorageAvailabilityService.h"
#include "system/StartupPerformanceLog.h"
#include "system/WjyDiagnosticLog.h"


#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <thread>
#include <tuple>

#if defined(Q_OS_WIN)
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
constexpr const char* kUpdateShareRoot = "\\\\192.168.1.100\\广告部工具\\远程软件_FS";
constexpr const char* kVersionFileName = "FSRemote.version";
constexpr const char* kUpdateResourcesDirName = "更新资源";
constexpr const char* kInstallerFileName = "FSRemote安装器.exe";
constexpr int kRemoteVersionReadTimeoutMs = 2500; // wjy: 端口可连接但 SMB 文件读取超过 2.5 秒时立即把本轮视为失败，界面不再等待网络恢复。
constexpr int kRemoteVersionCancelRetryMs = 250; // wjy: 首次取消可能早于线程真正进入系统 I/O，短间隔重试覆盖这一竞态窗口。
constexpr qint64 kFailedUpdateCheckCooldownMs = 30 * 1000; // wjy: 弱网失败后至少冷却 30 秒，避免其它功能触发探测时反复读取同一网盘文件。
constexpr int kPeriodicUpdateCheckIntervalMs = 20 * 1000; // wjy: 最新版本只以共享 FSRemote.version 为权威，每 20 秒异步检查一次，不再依赖设备间版本提示。

bool sharedStorageAccessAllowed(QString* error)
{
    if (SharedStorageAvailabilityService::instance().isAvailable()) {
        return true; // wjy: 最近一次异步 SMB 端口检测成功后，更新流程才允许继续访问具体 UNC 文件。
    }
    if (error) {
        *error = QString::fromUtf8("网盘连接测试未通过，已跳过共享目录访问。");
    }
    return false; // wjy: 离线设备直接快速失败，不创建文件线程，也不调用可能长时间等待的 QDir/QFile。
}

QStringList rootRuntimeFileNames()
{
    return {
        QStringLiteral("FSRemote.exe"),
        QStringLiteral("FSRemoteUpdater.exe"), // wjy: 发布稳定更新器，运行时会复制到临时目录而不直接从安装目录执行。
        QStringLiteral("fsremote_stream.dll"),
        // =====wjy====
        QStringLiteral("FakerInputBridge.exe"), // wjy: 驱动鼠标依赖的独立本机 Bridge 必须随每个不可变 release 发布、更新和回撤。
        QStringLiteral("FakerInput_Setup_0.1.1_x64.msi"), // wjy: 目标端缺少驱动时由 FSRemote/安装器静默调用此固定 MSI，缺失即拒绝形成发布版本。
        QStringLiteral("FakerInput_Ryodigi_Solutions_LLC.cer"), // wjy: 全新网络安装先预置信任固定发布者，避免 Windows 设备软件确认窗口阻断无人值守安装。
        // ===end====
        QStringLiteral("Qt6Core.dll"), QStringLiteral("Qt6Gui.dll"), QStringLiteral("Qt6Network.dll"),
        QStringLiteral("Qt6Widgets.dll"), QStringLiteral("Qt6Svg.dll"),
        QStringLiteral("avcodec-62.dll"), QStringLiteral("avutil-60.dll"), QStringLiteral("swresample-6.dll"),
        QStringLiteral("swscale-9.dll"), QStringLiteral("msvcp140.dll"), QStringLiteral("vcruntime140.dll"),
        QStringLiteral("vcruntime140_1.dll"), QStringLiteral("concrt140.dll"), QStringLiteral("d3dcompiler_47.dll"),
        QStringLiteral("opengl32sw.dll"), QStringLiteral("dxcompiler.dll"), QStringLiteral("dxil.dll"),
    };
}

QStringList runtimePluginRoots()
{
    return {QStringLiteral("platforms"), QStringLiteral("imageformats"), QStringLiteral("iconengines"),
        QStringLiteral("styles"), QStringLiteral("tls"), QStringLiteral("networkinformation"), QStringLiteral("generic")};
}

QStringList opensshRuntimeFileNames()
{
    return {QStringLiteral("ssh.exe"), QStringLiteral("sshd.exe"), QStringLiteral("ssh-keygen.exe"),
        QStringLiteral("sftp-server.exe"), QStringLiteral("libcrypto.dll"), QStringLiteral("scp.exe"),
        QStringLiteral("sftp.exe"), QStringLiteral("ssh-add.exe"), QStringLiteral("ssh-agent.exe"),
        QStringLiteral("ssh-keyscan.exe"), QStringLiteral("ssh-pkcs11-helper.exe"), QStringLiteral("ssh-shellhost.exe"),
        QStringLiteral("ssh-sk-helper.exe"), QStringLiteral("sshd-auth.exe"), QStringLiteral("sshd-session.exe")};
}

QString updateResourcesRoot()
{
    return QDir(UpdateService::updateShareRoot()).filePath(QString::fromUtf8(kUpdateResourcesDirName)); // wjy: 新结构把全部版本标记和运行载荷收进一个目录，共享根目录只展示安装器。
}

bool ensureParentDir(const QString& filePath, QString* error)
{
    if (QDir().mkpath(QFileInfo(filePath).absolutePath())) return true;
    if (error) *error = QString::fromUtf8("无法创建目录：%1").arg(QFileInfo(filePath).absolutePath());
    return false;
}

bool copyFileClean(const QString& source, const QString& destination, bool required, QString* error)
{
    if (!QFileInfo::exists(source)) {
        if (!required) return true;
        if (error) *error = QString::fromUtf8("缺少必需更新文件：%1").arg(source);
        return false;
    }
    if (!ensureParentDir(destination, error)) return false;
    const qint64 expectedSize = QFileInfo(source).size(); // wjy: 每次复制后用源文件大小判断是否已经完整落盘，兼容安全软件在完成瞬间短暂占用文件。
    QString lastError;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (QFileInfo::exists(destination) && QFileInfo(destination).size() == expectedSize) {
            return true; // wjy: 上一次复制虽报告失败但完整文件已存在时直接接受，避免无意义地删除重拷。
        }
        if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
            lastError = QString::fromUtf8("无法清理目标文件");
        } else {
            QFile sourceFile(source);
            if (sourceFile.copy(destination)) {
                return QFileInfo(destination).size() == expectedSize; // wjy: QFile 报告成功后仍核对大小，半文件不能进入更新任务。
            }
            lastError = sourceFile.errorString(); // wjy: 保存 Qt 底层错误，最终提示能区分占用、权限和网络失败。
        }
        if (attempt < 3) {
            QThread::msleep(300); // wjy: 给 Defender 或杀毒软件的短时扫描留出释放窗口，再进行下一次复制。
        }
    }
    if (error) *error = QString::fromUtf8("复制失败：%1（%2）").arg(destination, lastError);
    return false;
}

bool removeLegacyPath(const QString& path, QString* error)
{
    const QFileInfo info(path);
    if (!info.exists()) return true;
    const bool removed = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
    if (!removed && error) *error = QString::fromUtf8("无法清理共享目录旧版文件：%1").arg(path);
    return removed;
}

bool cleanupLegacyShareLayout(const QString& shareRoot, QString* error)
{
    // =====wjy====
    // wjy: 用户明确不兼容旧客户端；发布成功时只删除旧版 FSRemote 自己创建的已知文件，绝不枚举删除共享根目录中的其它业务文件。
    for (const QString& name : rootRuntimeFileNames()) {
        if (!removeLegacyPath(QDir(shareRoot).filePath(name), error)) return false;
    }
    if (!removeLegacyPath(QDir(shareRoot).filePath(QString::fromLatin1(kVersionFileName)), error)) return false;
    for (const QString& root : runtimePluginRoots()) {
        if (!removeLegacyPath(QDir(shareRoot).filePath(root), error)) return false;
    }
    const QStringList legacyDirectories{QStringLiteral("openssh"), QStringLiteral("parsec_vdd"), QStringLiteral("releases")};
    for (const QString& directory : legacyDirectories) {
        if (!removeLegacyPath(QDir(shareRoot).filePath(directory), error)) return false;
    }
    return true;
    // ===end====
}

bool copyDirectoryFiltered(const QString& sourceDir, const QString& destinationDir,
    const QSet<QString>& allowedNames, bool required, QString* error)
{
    QDir source(sourceDir);
    if (!source.exists()) {
        if (!required) return true;
        if (error) *error = QString::fromUtf8("缺少必需目录：%1").arg(sourceDir);
        return false;
    }
    QDirIterator it(sourceDir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (!allowedNames.isEmpty() && !allowedNames.contains(info.fileName())) continue;
        if (info.suffix().compare(QStringLiteral("pdb"), Qt::CaseInsensitive) == 0
            || info.fileName().endsWith(QStringLiteral(".old"), Qt::CaseInsensitive)) continue;
        if (!copyFileClean(info.absoluteFilePath(), QDir(destinationDir).filePath(source.relativeFilePath(info.absoluteFilePath())), true, error)) return false;
    }
    return true;
}

// =====wjy====
bool verifyFileMatches(const QString& source, const QString& destination, QString* error)
{
    const QFileInfo sourceInfo(source);
    const QFileInfo destinationInfo(destination);
    if (!sourceInfo.isFile() || !destinationInfo.isFile() || sourceInfo.size() != destinationInfo.size()) {
        if (error) *error = QString::fromUtf8("共享运行包文件缺失或大小不一致：%1").arg(destination); // wjy: 发布验收失败时指出共享目录的具体问题文件。
        return false;
    }
    return true;
}

bool verifyDirectoryMatches(const QString& sourceDir, const QString& destinationDir,
    const QSet<QString>& allowedNames, bool required, QString* error)
{
    const QDir source(sourceDir);
    if (!source.exists()) {
        if (!required) return true;
        if (error) *error = QString::fromUtf8("发布端缺少必需运行目录：%1").arg(sourceDir);
        return false;
    }
    QDirIterator it(sourceDir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    int verifiedCount = 0;
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (!allowedNames.isEmpty() && !allowedNames.contains(info.fileName())) continue;
        if (info.suffix().compare(QStringLiteral("pdb"), Qt::CaseInsensitive) == 0
            || info.fileName().endsWith(QStringLiteral(".old"), Qt::CaseInsensitive)) continue;
        const QString destination = QDir(destinationDir).filePath(source.relativeFilePath(info.absoluteFilePath()));
        if (!verifyFileMatches(info.absoluteFilePath(), destination, error)) return false;
        ++verifiedCount; // wjy: 统计实际纳入运行包的文件，必需目录不能只是空壳目录。
    }
    if (required && verifiedCount == 0) {
        if (error) *error = QString::fromUtf8("必需运行目录没有可发布文件：%1").arg(sourceDir);
        return false;
    }
    return true;
}

bool verifyRuntimePayload(const QString& sourceRoot, const QString& destinationRoot, QString* error)
{
    for (const QString& name : rootRuntimeFileNames()) {
        if (!verifyFileMatches(QDir(sourceRoot).filePath(name), QDir(destinationRoot).filePath(name), error)) return false; // wjy: 根目录白名单全部视为干净设备运行包必需文件。
    }
    for (const QString& root : runtimePluginRoots()) {
        if (!verifyDirectoryMatches(QDir(sourceRoot).filePath(root), QDir(destinationRoot).filePath(root), {},
                root == QStringLiteral("platforms"), error)) return false;
    }
    if (!verifyFileMatches(QDir(sourceRoot).filePath(QStringLiteral("platforms/qwindows.dll")),
            QDir(destinationRoot).filePath(QStringLiteral("platforms/qwindows.dll")), error)) return false; // wjy: Qt 程序在全新 Windows 设备启动必须具备平台插件。

    const QStringList opensshNames = opensshRuntimeFileNames();
    const QSet<QString> opensshAllowed(opensshNames.cbegin(), opensshNames.cend());
    if (!verifyDirectoryMatches(QDir(sourceRoot).filePath(QStringLiteral("openssh/OpenSSH-Win64")),
            QDir(destinationRoot).filePath(QStringLiteral("openssh/OpenSSH-Win64")), opensshAllowed, true, error)) return false;
    const QStringList requiredOpenSsh{QStringLiteral("ssh.exe"), QStringLiteral("sshd.exe"), QStringLiteral("ssh-keygen.exe"),
        QStringLiteral("sftp-server.exe"), QStringLiteral("libcrypto.dll")};
    for (const QString& name : requiredOpenSsh) {
        if (!verifyFileMatches(QDir(sourceRoot).filePath(QStringLiteral("openssh/OpenSSH-Win64/%1").arg(name)),
                QDir(destinationRoot).filePath(QStringLiteral("openssh/OpenSSH-Win64/%1").arg(name)), error)) return false;
    }
    return verifyFileMatches(QDir(sourceRoot).filePath(QStringLiteral("parsec_vdd/parsec-vdd-0.45.0.0.exe")),
        QDir(destinationRoot).filePath(QStringLiteral("parsec_vdd/parsec-vdd-0.45.0.0.exe")), error);
}
// ===end====

bool syncRuntimePayload(const QString& sourceRoot, const QString& destinationRoot, QString* error)
{
    for (const QString& name : rootRuntimeFileNames()) {
        if (!copyFileClean(QDir(sourceRoot).filePath(name), QDir(destinationRoot).filePath(name), true, error)) return false; // wjy: 发布白名单全部补齐，缺少任何一项都拒绝形成不完整共享运行包。
    }
    for (const QString& root : runtimePluginRoots()) {
        if (!copyDirectoryFiltered(QDir(sourceRoot).filePath(root), QDir(destinationRoot).filePath(root), {},
                root == QStringLiteral("platforms"), error)) return false;
    }
    const QStringList opensshNames = opensshRuntimeFileNames(); // wjy: 保存列表生命周期，避免从两个临时容器取得失效迭代器。
    QSet<QString> opensshAllowed(opensshNames.cbegin(), opensshNames.cend());
    if (!copyDirectoryFiltered(QDir(sourceRoot).filePath(QStringLiteral("openssh/OpenSSH-Win64")),
            QDir(destinationRoot).filePath(QStringLiteral("openssh/OpenSSH-Win64")), opensshAllowed, true, error)) return false;
    return copyFileClean(QDir(sourceRoot).filePath(QStringLiteral("parsec_vdd/parsec-vdd-0.45.0.0.exe")),
        QDir(destinationRoot).filePath(QStringLiteral("parsec_vdd/parsec-vdd-0.45.0.0.exe")), true, error);
}

QString readVersionFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

bool writeVersionFile(const QString& path, const QString& version, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(version.toUtf8()) < 0 || !file.commit()) {
        if (error) *error = QString::fromUtf8("无法写入版本文件：%1").arg(path);
        return false;
    }
    return true;
}

bool parseSemanticVersion(const QString& text, int* major, int* minor, int* patch)
{
    const QStringList parts = text.trimmed().split(QLatin1Char('.'));
    if (parts.size() != 3) return false;
    bool okMajor = false, okMinor = false, okPatch = false;
    const int m = parts[0].toInt(&okMajor), n = parts[1].toInt(&okMinor), p = parts[2].toInt(&okPatch);
    if (!okMajor || !okMinor || !okPatch || m < 0 || n < 0 || p < 0) return false;
    if (major) *major = m;
    if (minor) *minor = n;
    if (patch) *patch = p;
    return true;
}

QString normalizeSemanticVersion(const QString& text, const QString& fallback = QStringLiteral("1.1.1"))
{
    int major = 0, minor = 0, patch = 0;
    return parseSemanticVersion(text, &major, &minor, &patch)
        ? QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch) : fallback;
}

// =====wjy====
QString canonicalSemanticVersion(const QString& text)
{
    int major = 0, minor = 0, patch = 0;
    if (!parseSemanticVersion(text, &major, &minor, &patch)) {
        return {}; // wjy: 历史目录名和用户选择必须是真实三段语义版本，不能用默认版本掩盖无效输入。
    }
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch); // wjy: 统一去除空白和非规范数字格式，后续目录匹配与版本比较使用同一文本。
}
// ===end====

QString updateWorkRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("Updates"));
}

QString immutableReleaseRoot(const QString& version)
{
    return QDir(updateResourcesRoot()).filePath(QStringLiteral("releases/%1").arg(version)); // wjy: 新客户端只读取更新资源目录中的不可变版本，不再回退到旧共享根目录。
}

QString remotePayloadRoot(const QString& version)
{
    return immutableReleaseRoot(version); // wjy: 用户明确放弃旧版兼容；目标版本目录缺失时更新直接失败，避免误读共享根目录中的残留文件。
}

// =====wjy====
bool releaseHasRequiredEntries(const QString& version, QString* error)
{
    const QString releaseRoot = immutableReleaseRoot(version); // wjy: 回撤只能读取发布完成后的不可变目录，绝不读取 .publishing 临时目录或共享根目录旧载荷。
    const QDir releaseDir(releaseRoot);
    if (!releaseDir.exists()) {
        if (error) *error = QString::fromUtf8("目标版本目录不存在：%1").arg(releaseRoot);
        return false;
    }

    const QString identity = canonicalSemanticVersion(
        readVersionFile(releaseDir.filePath(QString::fromLatin1(kVersionFileName)))); // wjy: 目录内版本标记必须与目录名一致，避免手工复制错包后回撤到混合版本。
    if (identity != version) {
        if (error) *error = QString::fromUtf8("目标版本标记无效或与目录不一致：%1").arg(releaseRoot);
        return false;
    }

    QStringList requiredRelativePaths = rootRuntimeFileNames(); // wjy: 根目录运行白名单在现有暂存逻辑中全部为必需文件，发现阶段用元数据先排除明显残缺版本。
    requiredRelativePaths.append(QStringLiteral("platforms/qwindows.dll"));
    requiredRelativePaths.append(QStringLiteral("openssh/OpenSSH-Win64/ssh.exe"));
    requiredRelativePaths.append(QStringLiteral("openssh/OpenSSH-Win64/sshd.exe"));
    requiredRelativePaths.append(QStringLiteral("openssh/OpenSSH-Win64/ssh-keygen.exe"));
    requiredRelativePaths.append(QStringLiteral("openssh/OpenSSH-Win64/sftp-server.exe"));
    requiredRelativePaths.append(QStringLiteral("openssh/OpenSSH-Win64/libcrypto.dll"));
    requiredRelativePaths.append(QStringLiteral("parsec_vdd/parsec-vdd-0.45.0.0.exe"));
    for (const QString& relativePath : requiredRelativePaths) {
        const QFileInfo fileInfo(releaseDir.filePath(relativePath));
        if (!fileInfo.isFile() || fileInfo.size() <= 0) {
            if (error) *error = QString::fromUtf8("目标版本缺少必需文件：%1").arg(fileInfo.absoluteFilePath());
            return false; // wjy: 下拉框只做轻量文件元数据检查；点击回撤后仍会逐项复制并校验完整暂存载荷。
        }
    }
    return true;
}
// ===end====

bool writeUpdateTask(const QString& taskPath, const QString& sourceDir, const QString& targetDir,
    const QString& backupDir, const QString& fromVersion, const QString& toVersion, QString* error)
{
    QJsonArray files;
    QDirIterator it(sourceDir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        files.append(QJsonObject{{QStringLiteral("path"), QDir(sourceDir).relativeFilePath(info.absoluteFilePath()).replace('\\', '/')},
            {QStringLiteral("size"), static_cast<double>(info.size())}}); // wjy: 任务只列相对路径和期望大小，更新器不理解任何 FSRemote 业务文件名。
    }
    const QJsonObject task{{QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("processId"), static_cast<double>(QCoreApplication::applicationPid())},
        {QStringLiteral("sourceDir"), QDir::toNativeSeparators(sourceDir)}, {QStringLiteral("targetDir"), QDir::toNativeSeparators(targetDir)},
        {QStringLiteral("backupDir"), QDir::toNativeSeparators(backupDir)}, {QStringLiteral("restartExecutable"), QStringLiteral("FSRemote.exe")},
        {QStringLiteral("fromVersion"), fromVersion}, {QStringLiteral("toVersion"), toVersion}, {QStringLiteral("files"), files}};
    QSaveFile output(taskPath);
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(task).toJson(QJsonDocument::Compact)) < 0 || !output.commit()) {
        if (error) *error = QString::fromUtf8("无法写入更新任务：%1").arg(taskPath);
        return false;
    }
    return !files.isEmpty();
}
// ===end====

} // namespace

// =====wjy====
UpdateService& UpdateService::instance()
{
    static UpdateService service;
    return service;
}

UpdateService::UpdateService(QObject* parent)
    : QObject(parent)
{
    connect(&SharedStorageAvailabilityService::instance(), &SharedStorageAvailabilityService::probeFinished,
        this, &UpdateService::handleSharedStorageProbeFinished); // wjy: 更新检查复用全局连接门禁，不自己创建探测线程或阻塞主界面。

    // =====wjy====
    m_remoteVersionReadTimeoutTimer = new QTimer(this);
    m_remoteVersionReadTimeoutTimer->setSingleShot(true); // wjy: 首次 2.5 秒超时和后续 250ms 取消重试共用一个单次定时器，永远不会并发触发取消逻辑。
    connect(m_remoteVersionReadTimeoutTimer, &QTimer::timeout,
        this, &UpdateService::handleRemoteVersionReadTimeout); // wjy: 定时器只在主线程改变任务状态并调用系统取消 API，不执行文件操作。
    // ===end====
}

UpdateService::~UpdateService()
{
    stopPeriodicCheck(); // wjy: main 正常退出已提前调用一次；析构再次兜底，保证 std::thread 成员离开前必定不可连接。
}

QString UpdateService::updateShareRoot() { return QString::fromUtf8(kUpdateShareRoot); }
QString UpdateService::localVersionPath() { return QDir(QCoreApplication::applicationDirPath()).filePath(QString::fromLatin1(kVersionFileName)); }
QString UpdateService::remoteVersionPath() { return QDir(updateResourcesRoot()).filePath(QString::fromLatin1(kVersionFileName)); }
QString UpdateService::localVersionText() { return readVersionFile(localVersionPath()); }
QString UpdateService::remoteVersionText() { return readVersionFile(remoteVersionPath()); }
QString UpdateService::displayVersion() { return normalizeSemanticVersion(localVersionText()); }

// =====wjy====
bool UpdateService::canPublishCurrentBuild()
{
    const QDir appDir(QCoreApplication::applicationDirPath()); // wjy: 发布身份只由当前 EXE 所在目录判断，不依赖容易变化的 IP 或电脑名。
    return QFileInfo::exists(appDir.filePath(QStringLiteral("CMakeCache.txt")))
        && QDir(appDir.filePath(QStringLiteral("CMakeFiles"))).exists(); // wjy: 共享运行包白名单不包含这两个构建产物，因此复制到其它设备后自然失去发布权限。
}
// ===end====

QString UpdateService::bumpPatchVersion(const QString& version)
{
    int major = 1, minor = 1, patch = 1;
    parseSemanticVersion(normalizeSemanticVersion(version), &major, &minor, &patch);
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(++patch);
}

int UpdateService::compareSemanticVersions(const QString& left, const QString& right)
{
    int lm = 0, ln = 0, lp = 0, rm = 0, rn = 0, rp = 0;
    parseSemanticVersion(left, &lm, &ln, &lp);
    parseSemanticVersion(right, &rm, &rn, &rp);
    const auto l = std::tuple(lm, ln, lp), r = std::tuple(rm, rn, rp);
    return l < r ? -1 : (l > r ? 1 : 0); // wjy: 数值比较三段版本，远端较旧时不会触发更新。
}

bool UpdateService::runtimeDependenciesNeedRepair(const QString& runtimeRoot)
{
    const QDir runtimeDir(runtimeRoot); // wjy: 只检查当前 FSRemote 可执行文件所在目录，不扫描用户磁盘或接受外部任意目标。
    const QStringList repairFileNames{
        QStringLiteral("FakerInputBridge.exe"), // wjy: 已安装驱动仍必须有 Bridge 才能把远控鼠标命令送入 FakerInput HID。
        QStringLiteral("FakerInput_Setup_0.1.1_x64.msi"), // wjy: 未安装驱动的新设备需要固定 MSI；旧客户端漏下发时允许用同版本 release 补齐。
    };
    for (const QString& fileName : repairFileNames) {
        const QFileInfo fileInfo(runtimeDir.filePath(fileName)); // wjy: 空文件与不存在都视为残缺，防止中断复制留下占位文件后错误显示已修复。
        if (!fileInfo.isFile() || fileInfo.size() <= 0) {
            return true;
        }
    }
    return false; // wjy: 两个依赖均为非空普通文件时不开放同版本更新，避免形成无限更新循环。
}

bool UpdateService::remoteUpdateOrRepairAvailable(
    const QString& remoteVersion,
    const QString& localVersion,
    const QString& runtimeRoot)
{
    const QString remote = canonicalSemanticVersion(remoteVersion); // wjy: 共享版本必须是严格三段语义版本，无效标记不能借修复分支启动更新器。
    if (remote.isEmpty()) return false;
    const QString local = normalizeSemanticVersion(localVersion, QStringLiteral("0.0.0")); // wjy: 本地首次安装缺少版本标记时仍按旧逻辑接受有效远端版本。
    const int comparison = compareSemanticVersions(remote, local); // wjy: 只计算一次方向，保证检查状态与点击更新使用完全相同的判定。
    return comparison > 0
        || (comparison == 0 && runtimeDependenciesNeedRepair(runtimeRoot)); // wjy: 远端较旧永不覆盖本机；同版本只修复旧白名单漏掉的 FakerInput 依赖。
}

// =====wjy====
QStringList UpdateService::availableRollbackVersions(QString* error)
{
    if (error) error->clear();
    if (!sharedStorageAccessAllowed(error)) {
        return {}; // wjy: 设置页离线时不枚举 releases，避免每次打开设置都留下阻塞线程。
    }
    const QString localVersion = canonicalSemanticVersion(localVersionText());
    if (localVersion.isEmpty()) {
        if (error) *error = QString::fromUtf8("无法确认当前已安装版本。");
        return {}; // wjy: 当前版本未知时不猜测回撤方向，避免把同版本或更高版本误当成历史版本。
    }

    const QDir releasesDir(QDir(updateResourcesRoot()).filePath(QStringLiteral("releases")));
    if (!releasesDir.exists()) {
        if (error) *error = QString::fromUtf8("无法访问共享历史版本目录：%1").arg(releasesDir.absolutePath());
        return {};
    }

    QStringList versions;
    const QStringList directoryNames = releasesDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& directoryName : directoryNames) {
        const QString version = canonicalSemanticVersion(directoryName);
        if (version.isEmpty() || version != directoryName.trimmed()
            || compareSemanticVersions(version, localVersion) >= 0) {
            continue; // wjy: 排除发布临时目录、非规范目录、当前版本和更高版本，升级仍只走原有更新入口。
        }
        if (releaseHasRequiredEntries(version, nullptr)) {
            versions.append(version); // wjy: 发现阶段不读取整包内容，只把关键文件齐全的不可变历史版本交给设置页。
        }
    }
    std::sort(versions.begin(), versions.end(), [](const QString& left, const QString& right) {
        return UpdateService::compareSemanticVersions(left, right) > 0; // wjy: 下拉框优先展示最接近当前版本的历史版本，降低误选跨度。
    });
    return versions;
}
// ===end====

bool UpdateService::isUpdateAvailable() const
{
    return m_updateAvailable; // wjy: 设备命令查询只读取内存状态，网盘离线时不会把命令处理线程拖入 UNC 等待。
}

QString UpdateService::confirmedRemoteVersion() const
{
    return m_remoteVersion;
}

bool UpdateService::publishCurrentBuild(QString* error)
{
    // =====wjy====
    std::unique_lock<std::mutex> transactionLock(m_versionTransactionMutex, std::try_to_lock); // wjy: 后台发布与本机/远端升级共用服务层事务锁，不能并发改写 releases 和版本标记。
    if (!transactionLock.owns_lock()) {
        if (error) *error = QString::fromUtf8("已有版本更新、回撤或发布任务正在进行。");
        return false; // wjy: 界面状态只能阻止本窗口重复点击，服务锁继续拦截其它线程和远端命令入口。
    }
    if (!canPublishCurrentBuild()) {
        if (error) *error = QString::fromUtf8("当前程序不是从构建目录运行，不能发布更新。"); // wjy: 服务层再次拦截，避免未来其它入口绕过界面隐藏直接调用发布。
        return false;
    }
    if (!sharedStorageAccessAllowed(error)) {
        return false; // wjy: 发布按钮只能在最近一次连接测试成功后触碰共享目录。
    }
    // ===end====
    const QString share = updateShareRoot(), resourcesRoot = updateResourcesRoot();
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString local = normalizeSemanticVersion(localVersionText());
    const QString remote = normalizeSemanticVersion(remoteVersionText());
    const QString installerOutputRoot = QDir(appDir).filePath(QStringLiteral("installer/output"));
    const QString installerSource = QDir(installerOutputRoot).filePath(QString::fromUtf8(kInstallerFileName));
    if (!QFileInfo::exists(installerSource)) {
        if (error) *error = QString::fromUtf8("缺少网络安装器，请先构建一次 FSRemoteInstaller：%1").arg(installerOutputRoot);
        return false;
    }
    const QString base = compareSemanticVersions(local, remote) >= 0 ? local : remote;
    const QString next = bumpPatchVersion(base); // wjy: 网络安装器与业务版本解耦，后续发布只递增程序版本并替换共享 release，无需重新生成安装器。
    const QString releasesRoot = QDir(resourcesRoot).filePath(QStringLiteral("releases"));
    const QString publishingName = QStringLiteral(".publishing-%1-%2").arg(
        next,
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString publishingRoot = QDir(releasesRoot).filePath(publishingName);
    const QString finalReleaseRoot = QDir(releasesRoot).filePath(next);

    if (!QDir().mkpath(releasesRoot) || !QDir().mkpath(publishingRoot)) {
        if (error) *error = QString::fromUtf8("无法创建共享版本发布目录：%1").arg(publishingRoot);
        return false;
    }
    if (!syncRuntimePayload(appDir, publishingRoot, error)
        || !writeVersionFile(QDir(publishingRoot).filePath(QString::fromLatin1(kVersionFileName)), next, error)
        || !verifyRuntimePayload(appDir, publishingRoot, error)) {
        return false; // wjy: 临时版本目录不完整时保留旧版本标记，所有客户端继续使用上一份不可变发布。
    }
    if (QDir(finalReleaseRoot).exists() && !QDir(finalReleaseRoot).removeRecursively()) {
        if (error) *error = QString::fromUtf8("无法清理重复版本目录：%1").arg(finalReleaseRoot);
        return false;
    }
    if (!QDir(releasesRoot).rename(publishingName, next)) {
        if (error) *error = QString::fromUtf8("无法提交共享版本目录：%1").arg(finalReleaseRoot);
        return false;
    }

    const QString installerDestination = QDir(share).filePath(QString::fromUtf8(kInstallerFileName));
    if (QFileInfo::exists(installerDestination) && !QFile::remove(installerDestination)) {
        if (error) *error = QString::fromUtf8("无法替换共享目录安装器：%1").arg(installerDestination);
        return false;
    }
    if (!copyFileClean(installerSource, installerDestination, true, error)
        || !verifyFileMatches(installerSource, installerDestination, error)) {
        return false; // wjy: 根目录安装器完整落盘后才能删除旧结构，避免共享目录短暂失去新设备安装入口。
    }
    if (!cleanupLegacyShareLayout(share, error)) return false;
    if (!writeVersionFile(remoteVersionPath(), next, error)) return false; // wjy: 新目录载荷、根目录安装器和旧文件清理全部成功后，最后提交远端版本标记。
    writeVersionFile(localVersionPath(), next, nullptr);
    QMetaObject::invokeMethod(this, [this, next] {
        m_remoteVersion = next;
        const QString localVersion = normalizeSemanticVersion(localVersionText(), QStringLiteral("0.0.0"));
        m_updateAvailable = remoteUpdateOrRepairAvailable(next, localVersion, QCoreApplication::applicationDirPath());
        emit updateAvailabilityChanged(m_updateAvailable, next);
    }, Qt::QueuedConnection);
    return true;
}

bool UpdateService::applyRemoteUpdate(QString* error)
{
    // =====wjy====
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] apply begin appDir=%1")
        .arg(QCoreApplication::applicationDirPath())); // wjy: 用户点击或远端命令刚进入更新事务时立即落盘，覆盖复制校验前原本没有日志的盲区。
    std::unique_lock<std::mutex> transactionLock(m_versionTransactionMutex, std::try_to_lock); // wjy: 升级载荷暂存期间独占版本事务，避免另一条发布或回撤线程清理同一目录。
    if (!transactionLock.owns_lock()) {
        if (error) *error = QString::fromUtf8("已有版本更新、回撤或发布任务正在进行。");
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] apply rejected transaction busy")); // wjy: 明确区分互斥拒绝与网盘、版本或复制失败。
        return false;
    }
    if (!sharedStorageAccessAllowed(error)) {
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] apply failed shared storage unavailable error=%1")
            .arg(error ? *error : QString())); // wjy: 门禁失败时记录真实错误，不再表现为点击后完全没有输出。
        return false; // wjy: 连接状态已经失败时不读取远端版本，也不启动暂存或更新器流程。
    }
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] version read begin")); // wjy: UNC 版本文件读取可能阻塞，开始和结束日志可以直接测出耗时。
    QElapsedTimer versionReadTimer;
    versionReadTimer.start();
    const QString remoteVersion = canonicalSemanticVersion(remoteVersionText());
    const QString localVersion = normalizeSemanticVersion(localVersionText(), QStringLiteral("0.0.0"));
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] version read end elapsedMs=%1 local=%2 remote=%3")
        .arg(versionReadTimer.elapsed())
        .arg(localVersion, remoteVersion)); // wjy: 同时记录本地和共享版本，排除错误目录、旧进程及无效版本标记。
    if (remoteVersion.isEmpty()) {
        if (error) *error = QString::fromUtf8("共享目录没有有效的最新版本标记。");
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] apply failed invalid remote version"));
        return false;
    }
    if (!remoteUpdateOrRepairAvailable(remoteVersion, localVersion, QCoreApplication::applicationDirPath())) {
        if (error) *error = QString::fromUtf8("共享版本不是更高版本，且当前版本没有需要补齐的运行依赖。");
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] apply rejected no update or repair required"));
        return false; // wjy: 禁止无条件重复安装同版本；只有 Bridge/MSI 确实缺失时才允许复用当前不可变 release 修复。
    }
    const bool prepared = prepareRemoteVersionInstall(remoteVersion, error); // wjy: 正常升级与同版本依赖修复都复用完整暂存、校验、退出替换和重启事务。
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] apply end prepared=%1 error=%2")
        .arg(prepared ? 1 : 0)
        .arg(error ? *error : QString())); // wjy: 成功代表更新器和任务已经就绪；失败则保留最后一个服务层错误。
    return prepared;
    // ===end====
}

// =====wjy====
bool UpdateService::applyVersionRollback(const QString& targetVersion, QString* error)
{
    std::unique_lock<std::mutex> transactionLock(m_versionTransactionMutex, std::try_to_lock); // wjy: 回撤与升级、发布在服务层互斥，后台化后仍保持单一文件事务。
    if (!transactionLock.owns_lock()) {
        if (error) *error = QString::fromUtf8("已有版本更新、回撤或发布任务正在进行。");
        return false;
    }
    const QString target = canonicalSemanticVersion(targetVersion);
    const QString local = canonicalSemanticVersion(localVersionText());
    if (target.isEmpty() || target != targetVersion.trimmed()) {
        if (error) *error = QString::fromUtf8("请选择有效的回撤版本。");
        return false;
    }
    if (local.isEmpty()) {
        if (error) *error = QString::fromUtf8("无法确认当前已安装版本，不能安全回撤。");
        return false;
    }
    if (compareSemanticVersions(target, local) >= 0) {
        if (error) *error = QString::fromUtf8("回撤目标必须低于当前版本 %1。").arg(local);
        return false; // wjy: 当前版本和更高版本不能借回撤入口安装，防止绕过现有升级判断。
    }
    if (!sharedStorageAccessAllowed(error)) {
        return false; // wjy: 确认目标合法后仍必须通过连接门禁，离线时不会进入共享载荷验证。
    }
    return prepareRemoteVersionInstall(target, error); // wjy: 方向确认后复用完整暂存和独立更新器，主程序不直接修改任何运行文件。
}

bool UpdateService::prepareRemoteVersionInstall(const QString& targetVersion, QString* error)
{
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] install preparation begin target=%1")
        .arg(targetVersion)); // wjy: 升级与回撤共用此入口，目标版本用于关联本地 Updates 任务目录。
    if (!sharedStorageAccessAllowed(error)) {
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] install preparation failed shared storage unavailable error=%1")
            .arg(error ? *error : QString()));
        return false; // wjy: 服务内部再次防御未来新增调用入口绕过 UI 或版本方向检查。
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    QString releaseError;
    QElapsedTimer phaseTimer;
    phaseTimer.start();
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] release validation begin target=%1")
        .arg(targetVersion)); // wjy: 发布目录枚举与必需文件检查单独计时，定位共享盘元数据访问变慢。
    if (!releaseHasRequiredEntries(targetVersion, &releaseError)) {
        if (error) *error = releaseError;
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] release validation failed elapsedMs=%1 error=%2")
            .arg(phaseTimer.elapsed())
            .arg(releaseError));
        return false; // wjy: 用户确认后重新验证目标目录，处理下拉框打开后版本被删除或发布包损坏的竞态。
    }
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] release validation end elapsedMs=%1")
        .arg(phaseTimer.elapsed()));
    const QString sourceRoot = remotePayloadRoot(targetVersion); // wjy: 显式目标版本只读取对应不可变目录，最新版本标记变化不会把本次回撤切换到其它版本。
    const QString attemptName = QStringLiteral("%1-%2").arg(
        targetVersion,
        QUuid::createUuid().toString(QUuid::WithoutBraces)); // wjy: 每次更新使用唯一任务目录，旧更新器仍运行或残留时不会阻塞新的 runner 副本。
    const QString versionRoot = QDir(updateWorkRoot()).filePath(attemptName);
    const QString staging = QDir(versionRoot).filePath(QStringLiteral("payload"));
    const QString backup = QDir(versionRoot).filePath(QStringLiteral("backup"));
    const QString runner = QDir(versionRoot).filePath(QStringLiteral("runner"));
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] staging paths source=%1 staging=%2")
        .arg(sourceRoot, staging)); // wjy: 日志直接给出本次不可变 release 与唯一暂存目录，方便核对目标设备实际读取的包。
    if (!QDir().mkpath(staging)) {
        if (error) *error = QString::fromUtf8("无法创建更新暂存目录：%1").arg(staging);
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] staging directory create failed path=%1")
            .arg(staging));
        return false;
    }

    phaseTimer.restart();
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] payload copy begin"));
    if (!syncRuntimePayload(sourceRoot, staging, error)) {
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] payload copy failed elapsedMs=%1 error=%2")
            .arg(phaseTimer.elapsed())
            .arg(error ? *error : QString()));
        return false;
    }
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] payload copy end elapsedMs=%1")
        .arg(phaseTimer.elapsed()));

    phaseTimer.restart();
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] payload verify begin"));
    if (!verifyRuntimePayload(sourceRoot, staging, error)) {
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] payload verify failed elapsedMs=%1 error=%2")
            .arg(phaseTimer.elapsed())
            .arg(error ? *error : QString()));
        return false;
    }
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] payload verify end elapsedMs=%1")
        .arg(phaseTimer.elapsed())); // wjy: 主程序退出前完整复制并逐项核对目标载荷，网络中断或残缺包不会触碰当前安装。

    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] staged version write begin"));
    if (!writeVersionFile(QDir(staging).filePath(QString::fromLatin1(kVersionFileName)), targetVersion, error)) {
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] staged version write failed error=%1")
            .arg(error ? *error : QString()));
        return false;
    }
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] staged version write end")); // wjy: 版本文件作为最后一个安装文件，成功后客户端按实际目标版本继续检测更新。

    const QString runnerExe = QDir(runner).filePath(QStringLiteral("FSRemoteUpdater.exe"));
    const QString stagedUpdaterExe = QDir(staging).filePath(QStringLiteral("FSRemoteUpdater.exe")); // wjy: 使用本次已完整下载并校验的更新器，使更新器自身修复无需等安装完成后到下一版才生效。
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] updater runner copy begin source=%1 target=%2")
        .arg(stagedUpdaterExe, runnerExe));
    if (!QDir().mkpath(runner) || !QFile::copy(stagedUpdaterExe, runnerExe)) { // wjy: 临时 runner 仍与安装目录隔离，执行期间不会占用待替换的 FSRemoteUpdater.exe。
        if (error) *error = QString::fromUtf8("无法准备独立更新器。");
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] updater runner copy failed error=%1")
            .arg(error ? *error : QString()));
        return false;
    }
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] updater runner copy end"));
    const QString taskPath = QDir(versionRoot).filePath(QStringLiteral("update-task.json"));
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] task write begin path=%1").arg(taskPath));
    if (!writeUpdateTask(taskPath, staging, appDir, backup, displayVersion(), targetVersion, error)) {
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] task write failed error=%1")
            .arg(error ? *error : QString()));
        return false;
    }
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] task write end"));

    qint64 updaterPid = 0;
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] updater start begin"));
    if (!QProcess::startDetached(runnerExe, {QStringLiteral("--task"), taskPath}, runner, &updaterPid)) {
        if (error) *error = QString::fromUtf8("无法启动独立更新器。");
        writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] updater start failed error=%1")
            .arg(error ? *error : QString()));
        return false;
    }
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] updater start end pid=%1")
        .arg(updaterPid)); // wjy: 独立更新器 PID 会写入安装目录 data/updater.log，并由重启参数保证新主程序等待其关闭日志句柄。
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] updateReadyToQuit emit begin"));
    emit updateReadyToQuit(); // wjy: 任务与更新器都已在本地就绪，此后网络断开也不影响安装。
    writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] updateReadyToQuit emit returned")); // wjy: 若只有 begin 没有 returned，说明存在异常直接连接或信号处理未返回；正常跨线程队列会立即返回。
    return true;
}
// ===end====

// =====wjy====
void UpdateService::startPeriodicCheck()
{
    m_periodicCheckRunning = true; // wjy: 先开放异步结果门禁，再启动定时器和首次检查，极快返回的探测也不会被当成晚到结果。
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setInterval(kPeriodicUpdateCheckIntervalMs); // wjy: 20 秒周期提供确定的更新发现延迟，实际 UNC 读取仍经过异步 445 门禁和单任务保护。
        connect(m_timer, &QTimer::timeout, this, &UpdateService::checkNow); // wjy: 周期到期只发起异步 TCP 门禁，连接成功后再由专用后台线程读取版本文件。
    }
    if (!m_timer->isActive()) m_timer->start();
    checkNow(); // wjy: 启动后的首次检查只安排异步探测和后台读取，窗口事件循环始终可以继续响应。
}

void UpdateService::stopPeriodicCheck()
{
    // =====wjy====
    m_periodicCheckRunning = false; // wjy: 先关闭结果门禁，停止过程中排入事件队列的旧结果不能再修改缓存或发信号。
    if (m_timer) {
        m_timer->stop();
    }
    if (m_remoteVersionReadTimeoutTimer) {
        m_remoteVersionReadTimeoutTimer->stop();
    }
    m_checkPending = false; // wjy: 退出后忽略共享探测的晚到结果，不再启动新的 UNC 读取。
    ++m_remoteVersionReadGeneration; // wjy: 立即使当前工作线程已经排队的完成结果失效。
    m_remoteVersionReadTimedOut = true;
    cancelRemoteVersionReadIo(QStringLiteral("stop-periodic-check")); // wjy: 正在等待 SMB 的线程先收到系统取消，再进入确定性 join。

    const std::shared_ptr<std::thread> worker = m_remoteVersionReadThread;
    if (worker && worker->joinable()) {
        worker->join(); // wjy: 取消后汇合专用线程，UpdateService 生命周期结束时不会遗留访问 Qt 或 UNC 的后台代码。
    }
    m_remoteVersionReadThread.reset();
    m_remoteVersionReadInProgress = false;
    m_remoteVersionReadTimedOut = false;
    // ===end====
}

void UpdateService::checkNow()
{
    // =====wjy====
    if (!m_periodicCheckRunning) {
        return; // wjy: 服务停止或应用退出期间不再接受手动、定时器或晚到回调触发的新检查。
    }
    if (QDateTime::currentMSecsSinceEpoch() < m_updateCheckCooldownUntilMs) {
        return; // wjy: 最近一次弱网失败仍在冷却期时直接使用内存缓存，禁止连续触碰 UNC。
    }
    if (m_checkPending || m_remoteVersionReadInProgress) {
        return; // wjy: TCP 探测或版本读取任一阶段仍在进行时合并本轮请求，同一进程最多保留一个网络检查。
    }
    m_checkPending = true;
    SharedStorageAvailabilityService::instance().requestProbe(); // wjy: QTcpSocket 异步探测 445，离线环境不会启动文件访问线程。
    // ===end====
}

void UpdateService::handleSharedStorageProbeFinished(bool available)
{
    if (!m_periodicCheckRunning) {
        return; // wjy: 应用退出后忽略共享服务发出的晚到结果，不能重新启动后台线程。
    }
    if (!m_checkPending) {
        if (!available) {
            // =====wjy====
            if (m_remoteVersionReadInProgress && !m_remoteVersionReadTimedOut) {
                ++m_remoteVersionReadGeneration; // wjy: 其它功能更新出的较新“服务器不可用”结论使当前文件读取结果立即过期。
                m_remoteVersionReadTimedOut = true;
                cancelRemoteVersionReadIo(QStringLiteral("newer-probe-failed")); // wjy: 不让旧读取在网络恢复瞬间重新覆盖较新的离线状态。
                if (m_remoteVersionReadTimeoutTimer) {
                    m_remoteVersionReadTimeoutTimer->start(kRemoteVersionCancelRetryMs);
                }
            }
            const bool hadCachedUpdateState = m_updateAvailable || !m_remoteVersion.isEmpty(); // wjy: 即使旧缓存本来为空，也必须取消正在进行的读取；只有可见状态变化时才重复发信号。
            m_updateAvailable = false;
            m_remoteVersion.clear();
            m_updateCheckCooldownUntilMs = QDateTime::currentMSecsSinceEpoch() + kFailedUpdateCheckCooldownMs; // wjy: 共享服务器失败后延迟下一轮自动检查，降低弱网压力。
            if (hadCachedUpdateState) {
                emit updateAvailabilityChanged(false, QString()); // wjy: 壁纸或回撤探测发现服务器离线时同步撤销旧更新状态，按钮不会继续保持可点击。
            }
            // ===end====
        }
        return; // wjy: 本轮探测由其它功能单独请求时，不额外执行更新版本读取。
    }

    m_checkPending = false;
    if (!available) {
        m_updateAvailable = false;
        m_remoteVersion.clear();
        m_updateCheckCooldownUntilMs = QDateTime::currentMSecsSinceEpoch() + kFailedUpdateCheckCooldownMs; // wjy: TCP 门禁失败后短时停止自动重试，避免路由器抖动时制造连续连接。
        emit updateAvailabilityChanged(false, QString()); // wjy: 连接失败立即隐藏旧更新入口，且完全不触碰 UNC。
        return;
    }

    startRemoteVersionRead(); // wjy: 445 可连接只代表允许尝试；真正可能阻塞的 QFile::open/readAll 全部移到专用线程。
}

void UpdateService::startRemoteVersionRead()
{
    // =====wjy====
    if (!m_periodicCheckRunning || m_remoteVersionReadInProgress) {
        return; // wjy: 停止状态和已有任务都不能创建第二条 UNC 读取线程。
    }

    const quint64 generation = ++m_remoteVersionReadGeneration; // wjy: 本轮结果携带唯一代次，超时、停止或新失败探测可以使它失效。
    const QString versionPath = remoteVersionPath(); // wjy: 主线程只计算纯字符串路径，不调用 QFileInfo、QDir 枚举或文件打开。
    const std::shared_ptr<std::thread> worker = std::make_shared<std::thread>(); // wjy: 完成回调持有本轮精确线程对象，旧回调不会误 join 新任务。
    m_remoteVersionReadThread = worker;
    m_remoteVersionReadInProgress = true;
    m_remoteVersionReadTimedOut = false;

    try {
        *worker = std::thread([this, generation, versionPath, worker] {
            StartupPerformanceLog::checkpoint(QStringLiteral("[startup-update] background remote version read begin")); // wjy: 日志线程 ID 可直接证明 UNC 文件读取已离开主界面线程。
            const QString remoteVersion = normalizeSemanticVersion(readVersionFile(versionPath)); // wjy: 唯一可能被 SMB 拖慢的同步文件访问只在本专用线程执行。
            StartupPerformanceLog::checkpoint(QStringLiteral("[startup-update] background remote version read end version=%1")
                .arg(remoteVersion));
            QMetaObject::invokeMethod(this, [this, generation, remoteVersion, worker] {
                finishRemoteVersionRead(generation, remoteVersion, worker); // wjy: 纯字符串结果排队回对象线程，所有缓存和信号仍由主线程统一维护。
            }, Qt::QueuedConnection);
        });
    } catch (...) {
        m_remoteVersionReadThread.reset();
        m_remoteVersionReadInProgress = false;
        m_remoteVersionReadTimedOut = false;
        m_remoteVersion.clear();
        m_updateAvailable = false;
        m_updateCheckCooldownUntilMs = QDateTime::currentMSecsSinceEpoch() + kFailedUpdateCheckCooldownMs; // wjy: 系统无法创建线程时按一次检查失败处理，不允许异常越出 UI 事件回调。
        emit updateAvailabilityChanged(false, QString());
        return;
    }

    m_remoteVersionReadTimeoutTimer->start(kRemoteVersionReadTimeoutMs); // wjy: 主线程只等待定时器事件；即使 SMB 无响应，窗口绘制和鼠标输入仍会继续运行。
    // ===end====
}

void UpdateService::finishRemoteVersionRead(
    quint64 generation,
    const QString& remoteVersion,
    const std::shared_ptr<std::thread>& worker)
{
    // =====wjy====
    if (worker && worker->joinable()) {
        worker->join(); // wjy: 文件读取已经结束且完成回调已排队，此处只回收线程句柄，不再等待任何网络操作。
    }

    const bool currentWorker = m_remoteVersionReadThread == worker;
    if (!currentWorker) {
        return; // wjy: 停止后重新启动服务时，旧任务回调只能回收自己的线程，绝不能触碰新任务状态。
    }

    if (m_remoteVersionReadTimeoutTimer) {
        m_remoteVersionReadTimeoutTimer->stop();
    }
    m_remoteVersionReadThread.reset();
    m_remoteVersionReadInProgress = false;
    const bool acceptResult = m_periodicCheckRunning
        && !m_remoteVersionReadTimedOut
        && generation == m_remoteVersionReadGeneration; // wjy: 只有仍在运行、未超时且代次完全一致的结果才具有更新缓存的资格。
    m_remoteVersionReadTimedOut = false;
    if (!acceptResult) {
        return; // wjy: CancelSynchronousIo 后晚到的空值或旧值必须静默丢弃，避免更新按钮反复闪烁。
    }

    m_remoteVersion = remoteVersion;
    const QString localVersion = normalizeSemanticVersion(localVersionText(), QStringLiteral("0.0.0")); // wjy: 本地版本文件位于可执行目录，读取快速且不依赖任何网络路径。
    m_updateAvailable = remoteUpdateOrRepairAvailable(
        m_remoteVersion,
        localVersion,
        QCoreApplication::applicationDirPath()); // wjy: 新版启动后即使版本已追平，只要旧更新白名单漏了 MSI/Bridge，标题栏与远端更新命令仍会提供修复入口。
    m_updateCheckCooldownUntilMs = m_remoteVersion.isEmpty()
        ? QDateTime::currentMSecsSinceEpoch() + kFailedUpdateCheckCooldownMs
        : 0; // wjy: 空结果按一次网盘读取失败冷却；合法版本成功后立即解除旧失败状态。
    emit updateAvailabilityChanged(m_updateAvailable, m_remoteVersion);
    // ===end====
}

void UpdateService::handleRemoteVersionReadTimeout()
{
    // =====wjy====
    if (!m_remoteVersionReadInProgress || !m_remoteVersionReadThread) {
        return; // wjy: 工作线程已经回收时忽略晚到定时器事件。
    }

    if (!m_remoteVersionReadTimedOut) {
        m_remoteVersionReadTimedOut = true;
        ++m_remoteVersionReadGeneration; // wjy: 在请求系统取消之前先使本轮结果过期，极限竞态下先返回的旧结果也不会被接受。
        m_remoteVersion.clear();
        m_updateAvailable = false;
        m_updateCheckCooldownUntilMs = QDateTime::currentMSecsSinceEpoch() + kFailedUpdateCheckCooldownMs; // wjy: SMB 超时后进入冷却，周期检查不会立刻创建下一条线程。
        emit updateAvailabilityChanged(false, QString()); // wjy: 超时即撤销可能过期的更新状态，界面保持可操作且不会误导用户点击网盘更新。
    }

    cancelRemoteVersionReadIo(QStringLiteral("remote-version-timeout")); // wjy: 每次定时器到达都尝试取消当前线程尚未返回的同步文件 I/O。
    if (m_remoteVersionReadInProgress && m_remoteVersionReadThread) {
        m_remoteVersionReadTimeoutTimer->start(kRemoteVersionCancelRetryMs); // wjy: 直到工作线程回调完成前持续覆盖“首次取消早于 QFile 进入内核等待”的竞态。
    }
    // ===end====
}

void UpdateService::cancelRemoteVersionReadIo(const QString& phase)
{
    // =====wjy====
#if defined(Q_OS_WIN)
    const std::shared_ptr<std::thread> worker = m_remoteVersionReadThread;
    if (!worker || !worker->joinable()) {
        return; // wjy: 没有活动线程或线程已经完成时不能向 Windows 传入无效句柄。
    }

    if (!CancelSynchronousIo(worker->native_handle())) {
        const DWORD errorCode = GetLastError();
        if (errorCode != ERROR_NOT_FOUND) {
            StartupPerformanceLog::checkpoint(QStringLiteral("[startup-update] CancelSynchronousIo failed phase=%1 error=%2")
                .arg(phase)
                .arg(errorCode)); // wjy: ERROR_NOT_FOUND 只是当前瞬间没有可取消 I/O，其它错误写入已有诊断日志供定位。
        }
    }
#else
    Q_UNUSED(phase)
#endif
    // ===end====
}
// ===end====
// ===end====

} // namespace platform
