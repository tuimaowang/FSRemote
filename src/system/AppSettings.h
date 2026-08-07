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
    static bool hideLocalDeviceEnabled(); // wjy: 隐藏本机属于当前电脑自己的设备列表偏好，默认关闭并跨启动恢复。
    static void setHideLocalDeviceEnabled(bool enabled);
    // ===end====
    // =====wjy====
    static bool desktopWallpaperRotationEnabled(bool defaultEnabled = false); // wjy: 已保存开关始终优先；没有历史配置时允许调用方按当前设备名提供默认值。
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
    static void setRemoteQualityConfiguration(const stream::RemoteQualityConfiguration& configuration); // wjy: 主窗口设置变更只持久化默认模式，高清面积阈值由代码内置配置控制。
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
    static QKeySequence remoteShortcutMouseLock(); // wjy: 远控窗口 F2 手动鼠标锁定快捷键，默认仍为 F2 但允许键盘设置页自定义。
    static void setRemoteShortcutMouseLock(const QKeySequence& shortcut);
    static QKeySequence remoteShortcutInputScriptRecording(); // wjy: 远控窗口 F9 键鼠录制快捷键，设置修改后由远控窗口实时读取。
    static void setRemoteShortcutInputScriptRecording(const QKeySequence& shortcut);
    static QKeySequence remoteShortcutInputScriptPlayback(); // wjy: 远控窗口 F10 脚本播放快捷键，默认仍为 F10 并持久化用户修改。
    static void setRemoteShortcutInputScriptPlayback(const QKeySequence& shortcut);
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
