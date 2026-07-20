#pragma once

#include <QKeySequence>
#include <QRect>
#include <QString>

#include "stream/RemoteQualityPolicy.h"

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
    static bool desktopWallpaperRotationEnabled(); // wjy: 自动桌面壁纸轮换默认关闭，仅在用户主动开启后跨启动恢复。
    static void setDesktopWallpaperRotationEnabled(bool enabled);
    static int desktopWallpaperRotationIntervalMinutes(); // wjy: 壁纸周期统一使用整分钟，默认 1 分钟。
    static void setDesktopWallpaperRotationIntervalMinutes(int minutes);
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
    // =====wjy====
    static stream::RemoteQualityConfiguration remoteQualityConfiguration(); // wjy: 一次读取并归一化控制端全局远控画质默认值。
    static void setRemoteQualityConfiguration(const stream::RemoteQualityConfiguration& configuration); // wjy: 主窗口设置变更统一持久化完整配置，避免字段间约束被分散写坏。
    static bool remoteDeviceQualityMode(const QString& deviceKey, stream::RemoteQualityMode* mode); // wjy: 按目标设备读取上次远控窗口画质；首次设备返回false并由窗口使用“自动”。
    static void setRemoteDeviceQualityMode(const QString& deviceKey, stream::RemoteQualityMode mode); // wjy: 用户切换标题栏画质后立即按设备保存，关闭程序后仍能恢复。
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
