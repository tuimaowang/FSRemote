#include "system/AppSettings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QSettings>
#include <QStandardPaths>

namespace platform {
namespace {

QSettings settings()
{
    return QSettings(QStringLiteral("FSRemote"), QStringLiteral("FSRemote"));
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

bool AppSettings::statusAutoRefreshEnabled()
{
    return settings().value(QStringLiteral("statusAutoRefreshEnabled"), true).toBool();
}

void AppSettings::setStatusAutoRefreshEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("statusAutoRefreshEnabled"), enabled);
}

int AppSettings::statusAutoRefreshIntervalSeconds()
{
    const int seconds = settings().value(QStringLiteral("statusAutoRefreshIntervalSeconds"), 60).toInt();
    return seconds > 0 ? seconds : 60;
}

void AppSettings::setStatusAutoRefreshIntervalSeconds(int seconds)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("statusAutoRefreshIntervalSeconds"), seconds > 0 ? seconds : 60);
}

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
    return qBound(9000, settings().value(QStringLiteral("remoteHostAggregateVideoKbps"), 120000).toInt(), 240000); // wjy: 总预算默认沿用当前单流上限，允许后续按实测结果调整。
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
    return shortcutFromSettings(QStringLiteral("remoteShortcutCloseTopmost"), QKeySequence(QStringLiteral("F4"))); // wjy: 默认保持现有关闭最上方窗口快捷键。
}

void AppSettings::setRemoteShortcutCloseTopmost(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("remoteShortcutCloseTopmost"), shortcut, QKeySequence(QStringLiteral("F4"))); // wjy: 保存设置页录入的关闭单个窗口快捷键。
}

QKeySequence AppSettings::remoteShortcutCloseAll()
{
    return shortcutFromSettings(QStringLiteral("remoteShortcutCloseAll"), QKeySequence(QStringLiteral("Ctrl+F4"))); // wjy: 默认保持现有关闭全部窗口快捷键。
}

void AppSettings::setRemoteShortcutCloseAll(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("remoteShortcutCloseAll"), shortcut, QKeySequence(QStringLiteral("Ctrl+F4"))); // wjy: 保存设置页录入的关闭全部窗口快捷键。
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
