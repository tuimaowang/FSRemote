#pragma once

#include "system/DeviceInfoService.h"
#include "system/DeviceStatusService.h"

#include <atomic>
#include <functional>
#include <QElapsedTimer>
#include <QFrame>
#include <QHash>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QEvent;
class QByteArray;
class QKeyEvent;
class QJsonObject;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QTextEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QWheelEvent;

namespace ui {

class RemoteDesktopWindow;

enum class BottomAction {
    None,
    FileTransfer,
    More,
};

class DeviceGrid final : public QFrame {
    Q_OBJECT

public:
    explicit DeviceGrid(QWidget* parent = nullptr);
    ~DeviceGrid() override; // wjy: Used to record DeviceGrid destroy timing and wait background threads safely.
    void prepareForApplicationExit();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override; // wjy: Double click device or group rows to rename in place.
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override; // wjy: The hand-painted device list handles scrolling here.
    void keyPressEvent(QKeyEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    enum class SettingsTab {
        General,
        Keyboard,
    };

    enum class DeviceDetailTab {
        Config,
        ScriptLog,
    };

    void startDeviceSwitchAnimation(int newIndex, const QString& newName);
    void applySyncedDeviceSnapshot(const QJsonObject& snapshot); // wjy: 在 UI 线程按稳定 ID 应用共享快照，并保留本机分组展开状态和当前选择。
    void syncResponsiveLayoutState() const;
    void beginWindowResize(const QPoint& position, const QPoint& globalPosition);
    void updateWindowResize(const QPoint& globalPosition);
    void finishWindowResize();
    void setDesktopHoverActive(bool active);
    void updateDesktopHover(const QPoint& position);
    void updateBottomActionHover(const QPoint& position);
    void clearBottomActionHover();
    void setupAddDeviceControls();
    void updateAddDeviceControls();
    void setupLocalInfoControls();
    void updateLocalInfoControls();
    void saveNewDevice();
    void cancelNewDevice();
    void showDeviceMenu();
    // =====wjy====
    void showDeviceContextMenuForIndexes(int deviceIndex, const QVector<int>& targetDeviceIndexes, const QPoint& globalPosition); // wjy: 设备列表和远控标题栏共用同一菜单构建及动作分发逻辑。
    void showRemoteWindowDeviceMenu(const QString& hostIp, const QPoint& globalPosition); // wjy: 按远控窗口固定 IP 解析真实设备下标，避免错误控制主界面当前设备。
    // ===end====
    void openCurrentDeviceTerminal();
    bool openTerminalForDeviceIndex(int deviceIndex, bool showMessages);
    void executeCurrentDeviceScriptFolder(const QString& scriptFolderPath); // wjy: Copy one shared script folder to remote work directory and run its entry script.
    bool executeDeviceScriptFolder(int deviceIndex, const QString& scriptFolderPath, bool showMessages); // wjy: Run one shared script folder on a specified device without forcing the current selection.
    void executeDeviceGroupScriptFolder(int groupIndex, const QString& scriptFolderPath); // wjy: Run one selected script folder for every device in a group.
    void openRemoteDesktopWindow();
    void openRemoteDesktopWindowForDevice(int deviceIndex, const QPoint& fallbackOffset);
    void launchSelectedRemoteDesktopWindows();
    void openDeviceGroupTiledWindows(int groupIndex); // wjy: Open all devices in one group and tile remote windows.
    QVector<QPointer<RemoteDesktopWindow>> openedRemoteWindows() const;
    void rememberRemoteWindowActivation(RemoteDesktopWindow* window);
    RemoteDesktopWindow* topmostRemoteWindow() const;
    void toggleTopmostRemoteWindowFullscreen();
    void toggleRemoteWindowTiling();
    void closeTopmostRemoteWindow();
    void closeAllRemoteWindows();
    void refreshLocalDeviceInfo();
    void shutdownCurrentDevice();
    void restartCurrentDevice();
    bool shutdownDeviceForIndex(int deviceIndex, bool showMessages);
    bool restartDeviceForIndex(int deviceIndex, bool showMessages);
    // =====wjy====
    bool updateDeviceForIndex(int deviceIndex, bool showMessages); // wjy: 向指定在线设备发送远程更新请求，并区分已受理、已是最新版和失败。
    // ===end====
    void renameCurrentDevice();
    void deleteCurrentDevice();
    void deleteDeviceForIndex(int deviceIndex); // wjy: 设备列表和远控标题栏右键按明确下标删除，避免误用当前详情选择删除另一台设备。
    void startDeviceWakeVisual(const QString& ip);
    void wakeCurrentDevice();
    bool wakeDeviceForIndex(int deviceIndex, bool showMessages);
    void startCurrentDeviceWakeVisual();
    void toggleRemoteWakeup();
    void refreshDeviceStatuses();
    void probePoweringOnDevices();
    platform::DevicePresenceState devicePresenceForIndex(int index) const;
    bool devicePoweringOnForIndex(int index) const;
    int devicePoweringOnRemainingSecondsForIndex(int index) const;
    void setupSettingsControls();
    void updateSettingsControls();
    void saveShortcutKeySetting(int shortcutIndex, const QString& shortcutText); // wjy: Save one keyboard shortcut when its editor loses focus or receives Enter.
    void registerGlobalShortcuts();
    void unregisterGlobalShortcuts();
    void triggerShortcutAction(int shortcutIndex);
    void releaseRemoteShortcutKeyState(int shortcutIndex);
    void applyStatusAutoRefreshSetting(bool refreshImmediately);
    // =====wjy====
    void applyPeriodicDeviceDiscoverySetting(bool scanImmediately);
    void startBatchAddDevices(bool userInitiated = true); // wjy: 手动按钮和周期定时器复用同一网段扫描；周期触发时不跳转当前页面或选择新设备。
    // ===end====
    void beginDeviceGroupRename(int groupIndex); // wjy: Show the group rename editor in place.
    void finishDeviceGroupRename(bool saveText); // wjy: Finish group rename on Enter or focus loss.
    bool beginDeviceRename(int deviceIndex);
    void finishDeviceRename(bool saveText);
    void applyDeviceRename(int deviceIndex, const QString& newName);
    void pruneHiddenDeviceSelections(); // wjy: Remove collapsed hidden devices from multi-selection state.
    void runBackgroundTask(std::function<void()> task); // wjy: Keep background tasks joinable until DeviceGrid is destroyed.
    void setupScriptFileEditor();
    void setupScriptFolderTree();
    void populateScriptFolderTree();
    void addScriptFolderTreeChildren(QTreeWidgetItem* parentItem, const QString& folderPath);
    void selectScriptFolderTreeItem(QTreeWidgetItem* item);
    void syncScriptFolderTreeSelection();
    void updateScriptFileEditorControls();
    void loadScriptFileEditor(const QString& deviceIp, const QString& loginUser, const QString& scriptWorkName);
    void saveScriptFileEditor();
    void stopCurrentDeviceScript(); // wjy: Stop the running script on the target device, not only the local SSH process.
    bool stopDeviceScriptForDeviceIndex(int deviceIndex, bool showMessages); // wjy: Stop a running script for one specified device, used by current-device and group-stop actions.
    void stopDeviceGroupScripts(int groupIndex); // wjy: Stop all running scripts in one group.
    QString currentScriptUiDeviceIp() const; // wjy: Return the IP whose script UI should be shown by the current detail page.
    void saveCurrentScriptUiState(); // wjy: Persist the visible script/editor UI into the per-device state cache before switching devices.
    void loadScriptUiStateForDevice(const QString& deviceIp); // wjy: Restore script/editor UI that belongs to the newly selected device.
    void applyRemoteScriptRuntimeState(const QString& deviceIp, const QString& loginUser, const platform::RemoteScriptRuntimeInfo& runtime); // wjy: Reconcile target-authoritative script state into the per-device cache after startup or refresh.
    QVector<int> deviceIndexesForGroup(int groupIndex) const;
    QVector<int> contextDeviceIndexesForRightClick(int clickedDeviceIndex) const;
    void batchWakeDevices(const QVector<int>& deviceIndexes);
    void batchShutdownDevices(const QVector<int>& deviceIndexes);
    void batchRestartDevices(const QVector<int>& deviceIndexes);
    // =====wjy====
    void batchUpdateDevices(const QVector<int>& deviceIndexes); // wjy: 多选设备或分组菜单统一复用单设备更新请求逻辑。
    // ===end====
    void batchOpenDeviceTerminals(const QVector<int>& deviceIndexes);
    bool ensureRemoteControlAuthorization(int deviceIndex, bool showMessages); // wjy: 远控窗口创建前确保当前控制端公钥已登记到目标设备。

    struct ScriptUiState {
        bool outputVisible = false;
        bool outputRunning = false;
        bool outputFailed = false;
        // =====wjy====
        bool localLaunchInProgress = false; // wjy: 本控制端仍持有启动 SSH 任务时为 true，避免活动清单刚写入前被一次空闲刷新提前清掉图标。
        bool remoteStatusConfirmed = false; // wjy: 标识 outputRunning 是否已经被目标状态服务确认，供后续远端空闲响应清理陈旧图标。
        QString remoteRunId; // wjy: 保存远端唯一运行 ID，停止或正常结束时只清理同一次任务的活动清单。
        qint64 remoteControllerPid = 0; // wjy: 缓存目标 PowerShell PID，重启恢复时用于诊断和精确显示状态来源。
        qint64 remoteStartedAtEpochMs = 0; // wjy: 缓存目标开始时间，和状态服务返回的同一次运行保持关联。
        // ===end====
        int outputScrollOffset = 0;
        bool outputAutoScroll = true;
        bool outputDirty = false;
        QString outputTitle;
        QString outputText;
        QString outputFilePath;
        QString lastScriptFolderPath;
        std::shared_ptr<std::atomic_bool> cancelRequested;
        bool editorVisible = false;
        bool editorLoading = false;
        bool editorSaving = false;
        QString editorTitle;
        QString editorRemotePath;
        QString editorDeviceIp;
        QString editorLoginUser;
        QString editorWorkName;
        QString editorText;
        bool editorModified = false;
    };

    QString m_currentDeviceName;
    bool m_scriptOutputVisible = false; // wjy: Show the in-page script terminal after a device script is selected.
    bool m_scriptOutputRunning = false; // wjy: True while the remote script command is still executing.
    bool m_scriptOutputFailed = false; // wjy: Paint failure status in the script terminal without blocking the UI with a modal dialog.
    int m_scriptOutputScrollOffset = 0; // wjy: Terminal scroll offset measured in lines from the newest output; zero means pinned to bottom.
    bool m_scriptOutputAutoScroll = true; // wjy: Keep following new output until the user scrolls upward.
    bool m_scriptOutputDirty = false; // wjy: True when the local temp output file has new content waiting to be loaded into the panel.
    QString m_scriptOutputTitle;
    QString m_scriptOutputText;
    QString m_scriptOutputFilePath;
    QString m_lastScriptFolderPath;
    std::shared_ptr<std::atomic_bool> m_scriptCancelRequested;
    bool m_scriptEditorVisible = false;
    bool m_scriptEditorLoading = false;
    bool m_scriptEditorSaving = false;
    QString m_scriptEditorTitle;
    QString m_scriptEditorRemotePath;
    QString m_scriptEditorDeviceIp;
    QString m_scriptEditorLoginUser;
    QString m_scriptEditorWorkName;
    QHash<QString, ScriptUiState> m_scriptUiStates; // wjy: Keep script output/editor state isolated per device IP so switching devices does not mix panels.
    QLineEdit* m_statusRefreshIntervalEdit = nullptr; // wjy: Auto refresh interval edit.
    QLineEdit* m_periodicDeviceDiscoveryIntervalEdit = nullptr; // wjy: 周期检查新增设备的秒数输入框，默认 60 秒。
    QLineEdit* m_batchSubnetEdit = nullptr; // wjy: 批量新增网段输入框，支持以空格分隔多个 IPv4 通配网段。
    QPushButton* m_batchAddButton = nullptr; // wjy: 批量新增按钮，扫描期间禁用避免重复启动。
    QVector<QLineEdit*> m_shortcutKeyEdits; // wjy: Keyboard settings shortcut editors, one per remote-window action.
    QSet<int> m_registeredGlobalShortcutIds;
    quintptr m_globalShortcutWindowHandle = 0;
    QLineEdit* m_deviceIpEdit = nullptr;
    QLineEdit* m_deviceNameEdit = nullptr;
    QLineEdit* m_deviceMacEdit = nullptr;
    QLineEdit* m_deviceRemarkEdit = nullptr;
    QLineEdit* m_deviceGroupNameEdit = nullptr; // wjy: Group rename editor.
    QLineEdit* m_deviceListNameEdit = nullptr; // wjy: Device rename editor shown directly on the left list row.
    QTextEdit* m_scriptFileEdit = nullptr;
    QPushButton* m_scriptFileSaveButton = nullptr;
    QTreeWidget* m_scriptFolderTree = nullptr; // wjy: Script Log tab tree used to choose a shared script folder before execution.
    QPushButton* m_saveDeviceButton = nullptr;
    QPushButton* m_cancelDeviceButton = nullptr;
    QVector<QPushButton*> m_localInfoCopyButtons;
    bool m_draggingWindow = false;
    bool m_resizingWindow = false;
    int m_resizeEdges = 0;
    QPoint m_dragOffset;
    QPoint m_resizeStartGlobal;
    QRect m_resizeStartGeometry;
    bool m_deviceDragCandidateActive = false; // wjy: Mouse-down device row is a drag candidate before crossing threshold.
    bool m_draggingDevice = false; // wjy: True while dragging devices.
    int m_draggingDeviceIndex = -1; // wjy: Current dragged device index, -1 means none.
    bool m_groupDragCandidateActive = false; // wjy: Mouse-down group row before crossing the drag threshold.
    bool m_draggingGroup = false; // wjy: True while reordering groups.
    int m_draggingGroupIndex = -1; // wjy: Current dragged group index, -1 means none.

    // wjy: Device indexes that should move together in the current drag.
    QSet<int> m_draggingDeviceIndexes;
    QPoint m_deviceDragStartPos; // wjy: Device drag start position.
    QPoint m_deviceDragCurrentPos; // wjy: Current mouse position while dragging devices.
    QPoint m_groupDragStartPos; // wjy: Group drag start position.
    QPoint m_groupDragCurrentPos; // wjy: Current mouse position while dragging a group.
    bool m_deviceGroupExpanded = true;
    bool m_remoteAssistSelected = false;
    bool m_localInfoSelected = false;
    bool m_settingsSelected = false;
    SettingsTab m_settingsTab = SettingsTab::General;
    // =====wjy====
    DeviceDetailTab m_deviceDetailTab = DeviceDetailTab::ScriptLog; // wjy: 设备详情页默认显示脚本界面。
    // ===end====
    bool m_leftSidebarCollapsed = false;
    bool m_settingsLocalInfoExpanded = false;
    bool m_settingsAddDeviceExpanded = false;
    bool m_autoRunEnabled = true;
    bool m_remoteWakeupEnabled = false;
    bool m_wolDetectionInProgress = false;
    bool m_preventSleepEnabled = true;
    bool m_statusAutoRefreshEnabled = false;
    bool m_periodicDeviceDiscoveryEnabled = false; // wjy: 默认关闭，开启后按批量新增输入框中的网段周期扫描。
    bool m_statusRefreshInProgress = false;
    bool m_wakeProbeInProgress = false;
    bool m_batchAddInProgress = false; // wjy: 标记批量新增扫描是否正在后台执行。
    int m_statusAutoRefreshIntervalSeconds = 60; // wjy: Default auto refresh interval is 60 seconds.
    int m_periodicDeviceDiscoveryIntervalSeconds = 60; // wjy: 周期新增设备默认每 60 秒扫描一次。
    // =====wjy====
    bool m_updateAvailable = false; // wjy: 后台检测到更高版本时显示标题栏更新按钮，用户点击前绝不开始安装。
    bool m_updatePreparing = false; // wjy: 防止用户连续点击重复创建多个更新任务和更新器进程。
    QString m_availableUpdateVersion; // wjy: 保存检测到的远端版本，供标题栏更新状态使用。
    // ===end====
    int m_deviceListScrollOffset = 0; // wjy: Scroll offset for the hand-painted device list.
    int m_settingsScrollOffset = 0; // wjy: Scroll offset for the Settings > General content area.
    int m_renamingDeviceGroupIndex = -1; // wjy: Current renaming group index, -1 means none.
    int m_renamingDeviceIndex = -1; // wjy: Current inline-renaming device index, -1 means none.
    // wjy: Primary device shown on the right detail page.
    int m_selectedDeviceIndex = 0;

    // wjy: All selected device indexes in the left list.
    QSet<int> m_selectedDeviceIndexes;

    // wjy: Anchor index for Shift range selection.
    int m_selectionAnchorDeviceIndex = -1;

    int m_previousDeviceIndex = 0;
    QString m_previousDeviceName;
    std::mutex m_backgroundThreadsMutex; // wjy: Protects background thread list.
    std::vector<std::thread> m_backgroundThreads; // wjy: Joined in destructor to avoid late UI callbacks.
    bool m_shuttingDown = false; // wjy: No new background tasks after destruction begins.
    QHash<QString, platform::DevicePresenceState> m_deviceStatuses;
    // =====wjy====
    QHash<QString, int> m_deviceRemoteSessionCounts; // wjy: 目标端远控会话数 0-10，设备行数字徽标的权威缓存。
    QHash<QString, QString> m_deviceRemoteControllerNames; // wjy: 控制端设备名列表，悬停远控徽标气泡展示。
    // ===end====
    QTimer* m_detailAnimationTimer = nullptr;
    QTimer* m_desktopHoverTimer = nullptr;
    QTimer* m_refreshTimer = nullptr;
    // =====wjy====
    QTimer* m_settingsGearTimer = nullptr; // wjy: 设置齿轮点击后旋转半圈的动画定时器。
    // ===end====
    QTimer* m_statusAutoRefreshTimer = nullptr;
    QTimer* m_periodicDeviceDiscoveryTimer = nullptr; // wjy: 到期后复用批量新增扫描，已有扫描进行中时自动跳过本轮。
    QTimer* m_wakeVisualTimer = nullptr;
    QTimer* m_scriptOutputFlushTimer = nullptr;
    QElapsedTimer m_detailAnimationClock;
    QElapsedTimer m_desktopHoverClock;
    QElapsedTimer m_refreshClock;
    // =====wjy====
    QElapsedTimer m_settingsGearClock;
    // ===end====
    QElapsedTimer m_wakeVisualClock;
    qreal m_detailAnimationProgress = 1.0;
    qreal m_desktopHoverProgress = 0.0;
    qreal m_desktopHoverStartProgress = 0.0;
    qreal m_refreshRotation = 0.0;
    // =====wjy====
    qreal m_settingsGearRotation = 0.0; // wjy: 设置图标当前旋转角，点击后在 0→180 间插值。
    // ===end====
    qreal m_wakeVisualRotation = 0.0;
    qint64 m_lastWakeProbeAtMs = 0;
    bool m_desktopHovered = false;
    BottomAction m_hoveredBottomAction = BottomAction::None;
    QSet<QString> m_poweringOnDeviceIps;
    QHash<QString, qint64> m_poweringOnStartedAtMs;
    QHash<QString, QPointer<RemoteDesktopWindow>> m_remoteDesktopWindows; // wjy: 标题栏启动的远程桌面窗口按设备 IP 去重，再次启动只激活原窗口。
    QHash<QString, QString> m_pendingRemoteRenameNames; // wjy: 记录远端改名等待重启生效的设备，避免自动刷新立刻用旧电脑名覆盖手动新名字。
    platform::DeviceInfo m_localDeviceInfo;
    QVector<QPointer<RemoteDesktopWindow>> m_tiledRemoteWindows; // wjy: 记录设备平铺创建的窗口，下次平铺前先关闭旧窗口再重新排列。
    QVector<QPointer<RemoteDesktopWindow>> m_remoteWindowActivationOrder;
    QHash<RemoteDesktopWindow*, QRect> m_remoteTileRestoreGeometries;
    bool m_remoteWindowsTiled = false;
    int m_lastShortcutActionIndex = -1;
    qint64 m_lastShortcutActionAtMs = 0;
};

} // namespace ui
