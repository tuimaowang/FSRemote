#include "system/AppSettings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

namespace platform {
namespace {

QSettings settings()
{
    return QSettings(QStringLiteral("FSRemote"), QStringLiteral("FSRemote"));
}

QString remoteDeviceQualityModeKey(const QString& deviceKey)
{
    const QByteArray encodedKey = QUrl::toPercentEncoding(deviceKey.trimmed().toLower()); // wjy: 对IP或设备键做百分号编码，避免IPv6冒号等字符破坏QSettings层级。
    return QStringLiteral("remoteQuality/deviceModes/%1").arg(QString::fromLatin1(encodedKey));
}

bool isRemoteWindowQualityMode(stream::RemoteQualityMode mode)
{
    return mode == stream::RemoteQualityMode::FollowGlobal
        || stream::isPersistentGlobalQualityMode(mode); // wjy: 允许保存标题栏提供的全部模式，同时拒绝未知枚举污染后续会话。
}

QString configDirectoryPath()
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (path.trimmed().isEmpty()) {
        path = QDir::homePath() + QStringLiteral("/.fsremote");
    }
    QDir().mkpath(path);
    return path;
}

QString remoteDesktopWindowGeometryPath()
{
    return QDir(configDirectoryPath()).filePath(QStringLiteral("remote_desktop_window.json"));
}

// =====wjy====
QKeySequence shortcutFromSettings(const QString& key, const QKeySequence& fallback)
{
    const QString value = settings().value(key, fallback.toString(QKeySequence::PortableText)).toString().trimmed(); // wjy: 快捷键统一用 QSettings 保存字符串，便于跨启动恢复。
    const QKeySequence shortcut(value, QKeySequence::PortableText);
    return shortcut.isEmpty() ? fallback : shortcut; // wjy: 旧配置为空或解析失败时回退到原来的默认快捷键，避免用户无法操作远程窗口。
}

void setShortcutToSettings(const QString& key, const QKeySequence& shortcut, const QKeySequence& fallback)
{
    const QKeySequence normalized = shortcut.isEmpty() ? fallback : shortcut; // wjy: 不保存空快捷键，始终保留一个可触发的按键组合。
    QSettings appSettings = settings();
    appSettings.setValue(key, normalized.toString(QKeySequence::PortableText)); // wjy: PortableText 不受系统语言影响，后续解析更稳定。
}
// ===end====

QRect geometryFromJsonObject(const QJsonObject& object)
{
    const int width = object.value(QStringLiteral("width")).toInt();
    const int height = object.value(QStringLiteral("height")).toInt();
    if (width <= 0 || height <= 0) {
        return {};
    }
    return QRect(
        object.value(QStringLiteral("x")).toInt(),
        object.value(QStringLiteral("y")).toInt(),
        width,
        height);
}

QJsonObject geometryToJsonObject(const QRect& geometry)
{
    QJsonObject object;
    object.insert(QStringLiteral("x"), geometry.x());
    object.insert(QStringLiteral("y"), geometry.y());
    object.insert(QStringLiteral("width"), geometry.width());
    object.insert(QStringLiteral("height"), geometry.height());
    return object;
}

QJsonObject readRemoteDesktopWindowGeometryRoot()
{
    QFile file(remoteDesktopWindowGeometryPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return QJsonObject();
    }
    return document.object();
}

QString normalizedRemoteDesktopGeometryKey(const QString& deviceKey)
{
    return deviceKey.trimmed();
}

QRect readRemoteDesktopWindowGeometry(const QString& deviceKey)
{
    const QString key = normalizedRemoteDesktopGeometryKey(deviceKey);
    if (key.isEmpty()) {
        return {};
    }

    const QJsonObject root = readRemoteDesktopWindowGeometryRoot();
    const QJsonObject devices = root.value(QStringLiteral("devices")).toObject();
    const QRect deviceGeometry = geometryFromJsonObject(devices.value(key).toObject());
    if (deviceGeometry.isValid()) {
        return deviceGeometry;
    }

    return {};
}

} // namespace

bool AppSettings::preventSleepEnabled()
{
    return settings().value(QStringLiteral("preventSleepEnabled"), true).toBool();
}

void AppSettings::setPreventSleepEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("preventSleepEnabled"), enabled);
}

bool AppSettings::remoteWakeupEnabled()
{
    return settings().value(QStringLiteral("remoteWakeupEnabled"), true).toBool(); //允许远程控制
}

void AppSettings::setRemoteWakeupEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("remoteWakeupEnabled"), enabled);
}

// =====wjy====
bool AppSettings::periodicDeviceDiscoveryEnabled()
{
    return settings().value(QStringLiteral("periodicDeviceDiscoveryEnabled"), false).toBool(); // wjy: 新安装默认关闭，用户主动开启后才周期扫描三个默认网段。
}

void AppSettings::setPeriodicDeviceDiscoveryEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("periodicDeviceDiscoveryEnabled"), enabled);
}

int AppSettings::periodicDeviceDiscoveryIntervalSeconds()
{
    const int seconds = settings().value(QStringLiteral("periodicDeviceDiscoveryIntervalSeconds"), 60).toInt();
    return seconds > 0 ? seconds : 60; // wjy: 空值或旧注册表异常值统一回退到用户指定的 60 秒默认周期。
}

void AppSettings::setPeriodicDeviceDiscoveryIntervalSeconds(int seconds)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("periodicDeviceDiscoveryIntervalSeconds"), seconds > 0 ? seconds : 60);
}

bool AppSettings::hideLocalDeviceEnabled()
{
    return settings().value(QStringLiteral("hideLocalDeviceEnabled"), false).toBool(); // wjy: 新安装继续显示本机，只有用户主动开启后才过滤本机设备。
}

void AppSettings::setHideLocalDeviceEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("hideLocalDeviceEnabled"), enabled); // wjy: 开关只写当前用户 QSettings，不修改共享设备快照。
}
// ===end====

// =====wjy====
bool AppSettings::desktopWallpaperRotationEnabled(bool defaultEnabled)
{
    return settings().value(QStringLiteral("desktopWallpaperRotationEnabled"), defaultEnabled).toBool(); // wjy: 用户保存过的开关覆盖设备名默认值，未配置的新设备才采用调用方计算的默认状态。
}

void AppSettings::setDesktopWallpaperRotationEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("desktopWallpaperRotationEnabled"), enabled);
}

int AppSettings::desktopWallpaperRotationIntervalMinutes()
{
    return qBound(1, settings().value(QStringLiteral("desktopWallpaperRotationIntervalMinutes"), 1).toInt(), 1440); // wjy: 异常注册表值夹紧到 1 分钟至 24 小时，保证 QTimer 毫秒值安全有效。
}

void AppSettings::setDesktopWallpaperRotationIntervalMinutes(int minutes)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("desktopWallpaperRotationIntervalMinutes"), qBound(1, minutes, 1440));
}
// ===end====

// =====wjy====
int AppSettings::remoteHostMaxSessions()
{
    return qBound(1, settings().value(QStringLiteral("remoteHostMaxSessions"), 3).toInt(), 3); // wjy: 注册表没有该项时直接返回 3，新设备复制程序后无需额外配置即可使用三路会话。
}

void AppSettings::setRemoteHostMaxSessions(int sessions)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("remoteHostMaxSessions"), qBound(1, sessions, 3)); // wjy: 配置落盘前夹紧范围，避免异常值传入原生监听器。
}

int AppSettings::remoteHostAggregateVideoKbps()
{
    return qBound(9000, settings().value(QStringLiteral("remoteHostAggregateVideoKbps"), 80000).toInt(), 240000); // wjy: 总预算默认沿用当前高质量单流上限，允许后续按实测结果调整。
}

void AppSettings::setRemoteHostAggregateVideoKbps(int kbps)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("remoteHostAggregateVideoKbps"), qBound(9000, kbps, 240000)); // wjy: 至少保留一个可用视频流的最低预算并限制误配置峰值。
}

int AppSettings::remoteHostHandshakeTimeoutMs()
{
    return qBound(1000, settings().value(QStringLiteral("remoteHostHandshakeTimeoutMs"), 5000).toInt(), 30000); // wjy: 未认证连接最多等待 5 秒，避免长期占住准入槽位。
}

void AppSettings::setRemoteHostHandshakeTimeoutMs(int timeoutMs)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("remoteHostHandshakeTimeoutMs"), qBound(1000, timeoutMs, 30000)); // wjy: 超时范围兼顾局域网响应和较慢设备签名工具启动。
}

QString AppSettings::remoteHostOwnershipPolicy()
{
    const QString policy = settings().value(QStringLiteral("remoteHostOwnershipPolicy"), QStringLiteral("exclusive")).toString().trimmed().toLower();
    return policy == QStringLiteral("exclusive") ? policy : QStringLiteral("exclusive"); // wjy: 首版只允许独占控制权，未知旧配置统一回退到安全策略。
}

void AppSettings::setRemoteHostOwnershipPolicy(const QString& policy)
{
    Q_UNUSED(policy)
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("remoteHostOwnershipPolicy"), QStringLiteral("exclusive")); // wjy: 为未来策略扩展保留设置接口，但当前禁止写入协同输入模式。
}
// ===end====

// =====wjy====
stream::RemoteQualityConfiguration AppSettings::remoteQualityConfiguration()
{
    QSettings appSettings = settings();
    stream::RemoteQualityConfiguration configuration;
    configuration.defaultMode = static_cast<stream::RemoteQualityMode>(
        appSettings.value(QStringLiteral("remoteQuality/defaultMode"), static_cast<int>(stream::RemoteQualityMode::Automatic)).toInt()); // wjy: 全局默认模式读取整数枚举，未知值在统一归一化阶段回退自动。
    return stream::normalizedRemoteQualityConfiguration(configuration); // wjy: 旧版复杂FPS/预算参数和历史阈值设置不再读取，高清面积阈值由代码默认值决定。
}

void AppSettings::setRemoteQualityConfiguration(const stream::RemoteQualityConfiguration& configuration)
{
    const stream::RemoteQualityConfiguration normalized =
        stream::normalizedRemoteQualityConfiguration(configuration); // wjy: 保存前校验默认模式，固定预设参数不再开放持久化修改。
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("remoteQuality/defaultMode"), static_cast<int>(normalized.defaultMode));
    // wjy: 旧阈值键保留在注册表中以便历史诊断，但新版本既不读取也不覆盖，避免它们改变代码内置阈值。
}
// ===end====

// =====wjy====
bool AppSettings::remoteDeviceQualityMode(const QString& deviceKey, stream::RemoteQualityMode* mode)
{
    if (!mode || deviceKey.trimmed().isEmpty()) {
        return false; // wjy: 空设备键或空输出指针不读取共享配置，调用方继续使用“自动”默认值。
    }
    const QVariant stored = settings().value(remoteDeviceQualityModeKey(deviceKey));
    if (!stored.isValid()) {
        return false; // wjy: 首次远控该设备没有记录，明确让窗口保持“自动”。
    }
    bool parsed = false;
    const int storedValue = stored.toInt(&parsed); // wjy: 显式检查整数转换，损坏字符串不能静默变成FollowGlobal枚举0。
    const stream::RemoteQualityMode storedMode = static_cast<stream::RemoteQualityMode>(storedValue);
    if (!parsed || !isRemoteWindowQualityMode(storedMode)) {
        return false; // wjy: 旧版本或损坏枚举不参与恢复，防止下发未定义画质模式。
    }
    *mode = storedMode;
    return true;
}

void AppSettings::setRemoteDeviceQualityMode(const QString& deviceKey, stream::RemoteQualityMode mode)
{
    if (deviceKey.trimmed().isEmpty() || !isRemoteWindowQualityMode(mode)) {
        return; // wjy: 只保存有稳定设备键且属于标题栏菜单的合法模式。
    }
    QSettings appSettings = settings();
    appSettings.setValue(remoteDeviceQualityModeKey(deviceKey), static_cast<int>(mode)); // wjy: 每台设备独立覆盖同一键，模式切换立即落盘且不会影响其它设备。
}
// ===end====

// =====wjy====
QKeySequence AppSettings::remoteShortcutFullscreen()
{
    return shortcutFromSettings(QStringLiteral("remoteShortcutFullscreen"), QKeySequence(QStringLiteral("Ctrl+D"))); // wjy: 默认保持现有全屏切换快捷键。
}

void AppSettings::setRemoteShortcutFullscreen(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("remoteShortcutFullscreen"), shortcut, QKeySequence(QStringLiteral("Ctrl+D"))); // wjy: 保存设置页录入的全屏快捷键。
}

QKeySequence AppSettings::remoteShortcutTile()
{
    return shortcutFromSettings(QStringLiteral("remoteShortcutTile"), QKeySequence(QStringLiteral("Ctrl+P"))); // wjy: 默认保持现有平铺切换快捷键。
}

void AppSettings::setRemoteShortcutTile(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("remoteShortcutTile"), shortcut, QKeySequence(QStringLiteral("Ctrl+P"))); // wjy: 保存设置页录入的平铺快捷键。
}

QKeySequence AppSettings::remoteShortcutCloseTopmost()
{
    return shortcutFromSettings(QStringLiteral("remoteShortcutCloseTopmost"), QKeySequence(QStringLiteral("Ctrl+W"))); // wjy: 未保存自定义值时使用Ctrl+W关闭最上方远控窗口，符合常见标签页关闭习惯。
}

void AppSettings::setRemoteShortcutCloseTopmost(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("remoteShortcutCloseTopmost"), shortcut, QKeySequence(QStringLiteral("Ctrl+W"))); // wjy: 保存设置页录入值；空值回退到新的Ctrl+W默认组合。
}

QKeySequence AppSettings::remoteShortcutCloseAll()
{
    return shortcutFromSettings(QStringLiteral("remoteShortcutCloseAll"), QKeySequence(QStringLiteral("Ctrl+F4"))); // wjy: 默认保持现有关闭全部窗口快捷键。
}

void AppSettings::setRemoteShortcutCloseAll(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("remoteShortcutCloseAll"), shortcut, QKeySequence(QStringLiteral("Ctrl+F4"))); // wjy: 保存设置页录入的关闭全部窗口快捷键。
}

// =====wjy====
QKeySequence AppSettings::remoteShortcutClipboardSync()
{
    return shortcutFromSettings(QStringLiteral("remoteShortcutClipboardSync"), QKeySequence(QStringLiteral("Ctrl+B"))); // wjy: 默认 Ctrl+B 切换远控剪切板同步。
}

void AppSettings::setRemoteShortcutClipboardSync(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("remoteShortcutClipboardSync"), shortcut, QKeySequence(QStringLiteral("Ctrl+B")));
}

QKeySequence AppSettings::remoteShortcutMouseLock()
{
    return shortcutFromSettings(QStringLiteral("remoteShortcutMouseLock"), QKeySequence(QStringLiteral("F2"))); // wjy: 兼容原先固定 F2 行为，旧配置首次读取自动采用相同默认值。
}

void AppSettings::setRemoteShortcutMouseLock(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("remoteShortcutMouseLock"), shortcut, QKeySequence(QStringLiteral("F2"))); // wjy: 保存设置页录入的手动鼠标锁定快捷键。
}

QKeySequence AppSettings::remoteShortcutInputScriptRecording()
{
    return shortcutFromSettings(QStringLiteral("remoteShortcutInputScriptRecording"), QKeySequence(QStringLiteral("F9"))); // wjy: 兼容原先固定 F9 录制行为。
}

void AppSettings::setRemoteShortcutInputScriptRecording(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("remoteShortcutInputScriptRecording"), shortcut, QKeySequence(QStringLiteral("F9"))); // wjy: 保存设置页录入的键鼠录制快捷键。
}

QKeySequence AppSettings::remoteShortcutInputScriptPlayback()
{
    return shortcutFromSettings(QStringLiteral("remoteShortcutInputScriptPlayback"), QKeySequence(QStringLiteral("F10"))); // wjy: 兼容原先固定 F10 播放行为。
}

void AppSettings::setRemoteShortcutInputScriptPlayback(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("remoteShortcutInputScriptPlayback"), shortcut, QKeySequence(QStringLiteral("F10"))); // wjy: 保存设置页录入的脚本播放快捷键。
}

QKeySequence AppSettings::deviceShortcutDelete()
{
    return shortcutFromSettings(QStringLiteral("deviceShortcutDelete"), QKeySequence(QStringLiteral("Delete"))); // wjy: 删除设备默认保持 Delete，同时允许从键盘设置页持久化为其它组合键。
}

void AppSettings::setDeviceShortcutDelete(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("deviceShortcutDelete"), shortcut, QKeySequence(QStringLiteral("Delete"))); // wjy: 复用统一快捷键存储格式，重启软件后继续使用用户设置。
}

bool AppSettings::remoteClipboardSyncEnabled()
{
    return settings().value(QStringLiteral("remoteClipboardSyncEnabled"), true).toBool(); // wjy: 默认开启剪切板同步。
}

void AppSettings::setRemoteClipboardSyncEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("remoteClipboardSyncEnabled"), enabled);
}

bool AppSettings::autoUpdateCheckEnabled()
{
    return settings().value(QStringLiteral("autoUpdateCheckEnabled"), true).toBool(); // wjy: 默认允许目标设备检查共享目录更新。
}

void AppSettings::setAutoUpdateCheckEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("autoUpdateCheckEnabled"), enabled);
}

bool AppSettings::startMinimizedToTray()
{
    return settings().value(QStringLiteral("startMinimizedToTray"), true).toBool(); // wjy: 开机自启默认最小化到托盘。
}

void AppSettings::setStartMinimizedToTray(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("startMinimizedToTray"), enabled);
}
// ===end====

bool AppSettings::hasRemoteDesktopWindowGeometry(const QString& deviceKey)
{
    return readRemoteDesktopWindowGeometry(deviceKey).isValid();
}

QRect AppSettings::remoteDesktopWindowGeometry(const QString& deviceKey)
{
    return readRemoteDesktopWindowGeometry(deviceKey);
}

void AppSettings::setRemoteDesktopWindowGeometry(const QString& deviceKey, const QRect& geometry)
{
    const QString key = normalizedRemoteDesktopGeometryKey(deviceKey);
    if (key.isEmpty() || !geometry.isValid()) {
        return;
    }

    QJsonObject root = readRemoteDesktopWindowGeometryRoot();
    QJsonObject devices = root.value(QStringLiteral("devices")).toObject();
    devices.insert(key, geometryToJsonObject(geometry));
    root.insert(QStringLiteral("devices"), devices);

    QFile file(remoteDesktopWindowGeometryPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

} // namespace platform
