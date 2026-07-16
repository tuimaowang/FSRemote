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
    // =====wjy====
    static bool periodicDeviceDiscoveryEnabled();
    static void setPeriodicDeviceDiscoveryEnabled(bool enabled);
    static int periodicDeviceDiscoveryIntervalSeconds();
    static void setPeriodicDeviceDiscoveryIntervalSeconds(int seconds);
    // ===end====
    // =====wjy====
    static int remoteHostMaxSessions();
    static void setRemoteHostMaxSessions(int sessions);
    static int remoteHostAggregateVideoKbps();
    static void setRemoteHostAggregateVideoKbps(int kbps);
    static int remoteHostHandshakeTimeoutMs();
    static void setRemoteHostHandshakeTimeoutMs(int timeoutMs);
    static QString remoteHostOwnershipPolicy();
    static void setRemoteHostOwnershipPolicy(const QString& policy);
    // ===end====
    static QKeySequence remoteShortcutFullscreen();
    static void setRemoteShortcutFullscreen(const QKeySequence& shortcut);
    static QKeySequence remoteShortcutTile();
    static void setRemoteShortcutTile(const QKeySequence& shortcut);
    static QKeySequence remoteShortcutCloseTopmost();
    static void setRemoteShortcutCloseTopmost(const QKeySequence& shortcut);
    static QKeySequence remoteShortcutCloseAll();
    static void setRemoteShortcutCloseAll(const QKeySequence& shortcut);
    // =====wjy====
    static QKeySequence remoteShortcutClipboardSync();
    static void setRemoteShortcutClipboardSync(const QKeySequence& shortcut);
    static QKeySequence deviceShortcutDelete();
    static void setDeviceShortcutDelete(const QKeySequence& shortcut);
    static bool remoteClipboardSyncEnabled();
    static void setRemoteClipboardSyncEnabled(bool enabled);
    static bool autoUpdateCheckEnabled();
    static void setAutoUpdateCheckEnabled(bool enabled);
    static bool startMinimizedToTray();
    static void setStartMinimizedToTray(bool enabled);
    // ===end====
    static bool hasRemoteDesktopWindowGeometry(const QString& deviceKey);
    static QRect remoteDesktopWindowGeometry(const QString& deviceKey);
    static void setRemoteDesktopWindowGeometry(const QString& deviceKey, const QRect& geometry);
};

} // namespace platform
