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
