#pragma once

#include <QKeySequence>
#include <QRect>
#include <QString>

namespace platform {

class AppSettings final {
public:
    static bool preventSleepEnabled();
    static void setPreventSleepEnabled(bool enabled);
    static bool remoteWakeupEnabled();
    static void setRemoteWakeupEnabled(bool enabled);
    static bool statusAutoRefreshEnabled();
    static void setStatusAutoRefreshEnabled(bool enabled);
    static int statusAutoRefreshIntervalSeconds();
    static void setStatusAutoRefreshIntervalSeconds(int seconds);
    static QKeySequence remoteShortcutFullscreen();
    static void setRemoteShortcutFullscreen(const QKeySequence& shortcut);
    static QKeySequence remoteShortcutTile();
    static void setRemoteShortcutTile(const QKeySequence& shortcut);
    static QKeySequence remoteShortcutCloseTopmost();
    static void setRemoteShortcutCloseTopmost(const QKeySequence& shortcut);
    static QKeySequence remoteShortcutCloseAll();
    static void setRemoteShortcutCloseAll(const QKeySequence& shortcut);
    static bool hasRemoteDesktopWindowGeometry(const QString& deviceKey);
    static QRect remoteDesktopWindowGeometry(const QString& deviceKey);
    static void setRemoteDesktopWindowGeometry(const QString& deviceKey, const QRect& geometry);
};

} // namespace platform
