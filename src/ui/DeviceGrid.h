#pragma once

#include "system/DeviceInfoService.h"
#include "system/DeviceStatusService.h"

#include <atomic>
#include <functional>
#include <QElapsedTimer>
#include <QFrame>
#include <QHash>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QEvent;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QTextEdit;
class QTimer;
class QWheelEvent;

namespace ui {

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

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override; // wjy: Double click a group row to rename in place.
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override; // wjy: The hand-painted device list handles scrolling here.
    void leaveEvent(QEvent* event) override;

private:
    void startDeviceSwitchAnimation(int newIndex, const QString& newName);
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
    void openCurrentDeviceTerminal();
    void executeCurrentDeviceScriptFolder(const QString& scriptFolderPath); // wjy: Copy one shared script folder to remote work directory and run its entry script.
    bool executeDeviceScriptFolder(int deviceIndex, const QString& scriptFolderPath, bool showMessages); // wjy: Run one shared script folder on a specified device without forcing the current selection.
    void executeDeviceGroupScriptFolder(int groupIndex, const QString& scriptFolderPath); // wjy: Run one selected script folder for every device in a group.
    void openRemoteDesktopWindow();
    void openDeviceGroupTiledWindows(int groupIndex); // wjy: Open all devices in one group and tile remote windows.
    void refreshLocalDeviceInfo();
    void shutdownCurrentDevice();
    void restartCurrentDevice();
    void renameCurrentDevice();
    void deleteCurrentDevice();
    void startDeviceWakeVisual(const QString& ip);
    void wakeCurrentDevice();
    void startCurrentDeviceWakeVisual();
    void toggleRemoteWakeup();
    void refreshDeviceStatuses();
    void probePoweringOnDevices();
    platform::DevicePresenceState devicePresenceForIndex(int index) const;
    bool devicePoweringOnForIndex(int index) const;
    int devicePoweringOnRemainingSecondsForIndex(int index) const;
    void setupSettingsControls();
    void updateSettingsControls();
    void applyStatusAutoRefreshSetting(bool refreshImmediately);
    void startBatchAddDevices(); // wjy: 从设置页网段输入框启动批量扫描并追加在线设备。
    void beginDeviceGroupRename(int groupIndex); // wjy: Show the group rename editor in place.
    void finishDeviceGroupRename(bool saveText); // wjy: Finish group rename on Enter or focus loss.
    void pruneHiddenDeviceSelections(); // wjy: Remove collapsed hidden devices from multi-selection state.
    void runBackgroundTask(std::function<void()> task); // wjy: Keep background tasks joinable until DeviceGrid is destroyed.
    void setupScriptFileEditor();
    void updateScriptFileEditorControls();
    void loadScriptFileEditor(const QString& deviceIp, const QString& loginUser, const QString& scriptWorkName);
    void saveScriptFileEditor();
    void stopCurrentDeviceScript(); // wjy: Stop the running script on the target device, not only the local SSH process.
    bool stopDeviceScriptForDeviceIndex(int deviceIndex, bool showMessages); // wjy: Stop a running script for one specified device, used by current-device and group-stop actions.
    void stopDeviceGroupScripts(int groupIndex); // wjy: Stop all running scripts in one group.
    QString currentScriptUiDeviceIp() const; // wjy: Return the IP whose script UI should be shown by the current detail page.
    void saveCurrentScriptUiState(); // wjy: Persist the visible script/editor UI into the per-device state cache before switching devices.
    void loadScriptUiStateForDevice(const QString& deviceIp); // wjy: Restore script/editor UI that belongs to the newly selected device.

    struct ScriptUiState {
        bool outputVisible = false;
        bool outputRunning = false;
        bool outputFailed = false;
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
    QLineEdit* m_batchSubnetEdit = nullptr; // wjy: 批量新增网段输入框，支持 192.168.3.* 格式。
    QPushButton* m_batchAddButton = nullptr; // wjy: 批量新增按钮，扫描期间禁用避免重复启动。
    QLineEdit* m_deviceIpEdit = nullptr;
    QLineEdit* m_deviceNameEdit = nullptr;
    QLineEdit* m_deviceMacEdit = nullptr;
    QLineEdit* m_deviceRemarkEdit = nullptr;
    QLineEdit* m_deviceGroupNameEdit = nullptr; // wjy: Group rename editor.
    QTextEdit* m_scriptFileEdit = nullptr;
    QPushButton* m_scriptFileSaveButton = nullptr;
    QPushButton* m_saveDeviceButton = nullptr;
    QPushButton* m_cancelDeviceButton = nullptr;
    QVector<QPushButton*> m_localInfoCopyButtons;
    bool m_draggingWindow = false;
    QPoint m_dragOffset;
    bool m_deviceDragCandidateActive = false; // wjy: Mouse-down device row is a drag candidate before crossing threshold.
    bool m_draggingDevice = false; // wjy: True while dragging devices.
    int m_draggingDeviceIndex = -1; // wjy: Current dragged device index, -1 means none.

    // wjy: Device indexes that should move together in the current drag.
    QSet<int> m_draggingDeviceIndexes;
    QPoint m_deviceDragStartPos; // wjy: Device drag start position.
    QPoint m_deviceDragCurrentPos; // wjy: Current mouse position while dragging devices.
    bool m_deviceGroupExpanded = true;
    bool m_remoteAssistExpanded = true;
    bool m_remoteAssistSelected = false;
    bool m_localInfoSelected = false;
    bool m_settingsSelected = false;
    bool m_autoRunEnabled = true;
    bool m_remoteWakeupEnabled = false;
    bool m_wolDetectionInProgress = false;
    bool m_preventSleepEnabled = true;
    bool m_statusAutoRefreshEnabled = false;
    bool m_statusRefreshInProgress = false;
    bool m_wakeProbeInProgress = false;
    bool m_batchAddInProgress = false; // wjy: 标记批量新增扫描是否正在后台执行。
    int m_statusAutoRefreshIntervalSeconds = 60; // wjy: Default auto refresh interval is 60 seconds.
    int m_deviceListScrollOffset = 0; // wjy: Scroll offset for the hand-painted device list.
    int m_renamingDeviceGroupIndex = -1; // wjy: Current renaming group index, -1 means none.
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
    QTimer* m_detailAnimationTimer = nullptr;
    QTimer* m_desktopHoverTimer = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QTimer* m_statusAutoRefreshTimer = nullptr;
    QTimer* m_wakeVisualTimer = nullptr;
    QTimer* m_scriptOutputFlushTimer = nullptr;
    QElapsedTimer m_detailAnimationClock;
    QElapsedTimer m_desktopHoverClock;
    QElapsedTimer m_refreshClock;
    QElapsedTimer m_wakeVisualClock;
    qreal m_detailAnimationProgress = 1.0;
    qreal m_desktopHoverProgress = 0.0;
    qreal m_desktopHoverStartProgress = 0.0;
    qreal m_refreshRotation = 0.0;
    qreal m_wakeVisualRotation = 0.0;
    qint64 m_lastWakeProbeAtMs = 0;
    bool m_desktopHovered = false;
    BottomAction m_hoveredBottomAction = BottomAction::None;
    QSet<QString> m_poweringOnDeviceIps;
    QHash<QString, qint64> m_poweringOnStartedAtMs;
    platform::DeviceInfo m_localDeviceInfo;
};

} // namespace ui
