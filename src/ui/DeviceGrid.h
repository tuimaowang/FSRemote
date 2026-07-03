#pragma once

#include "system/DeviceInfoService.h"
#include "system/DeviceStatusService.h"

#include <functional>
#include <QElapsedTimer>
#include <QFrame>
#include <QHash>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class QEvent;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
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
    ~DeviceGrid() override; // wjy: 仅用于记录 DeviceGrid 销毁时机，帮助判断后台线程是否晚于界面销毁返回。

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override; // wjy: 双击分组行时进入原地重命名。
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override; // wjy: 手绘设备列表使用滚轮事件实现滚动，不额外拆子控件。
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
    void openRemoteDesktopWindow();
    void openDeviceGroupTiledWindows(int groupIndex); // wjy: 打开某个分组内所有设备的远程桌面，并按屏幕网格平铺。
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
    void beginDeviceGroupRename(int groupIndex); // wjy: 在分组文字原位置显示输入框，开始原地重命名。
    void finishDeviceGroupRename(bool saveText); // wjy: 回车或点击外部时结束重命名，并按规则保存或恢复。
    void pruneHiddenDeviceSelections(); // wjy: 分组折叠后清理不可见设备的多选和拖拽状态，避免隐藏设备参与批量移动。
    void runBackgroundTask(std::function<void()> task); // wjy: 后台任务统一由 DeviceGrid 持有，关闭窗口时等待它们结束，避免 detach 线程晚于界面销毁。

    QString m_currentDeviceName;
    QLineEdit* m_statusRefreshIntervalEdit = nullptr; // wjy: 列表自动刷新间隔输入框，只允许输入秒数数字，替代下拉框。
    QLineEdit* m_deviceIpEdit = nullptr;
    QLineEdit* m_deviceNameEdit = nullptr;
    QLineEdit* m_deviceMacEdit = nullptr;
    QLineEdit* m_deviceRemarkEdit = nullptr;
    QLineEdit* m_deviceGroupNameEdit = nullptr; // wjy: 分组原地重命名输入框，平时隐藏，双击分组时显示。
    QPushButton* m_saveDeviceButton = nullptr;
    QPushButton* m_cancelDeviceButton = nullptr;
    QVector<QPushButton*> m_localInfoCopyButtons;
    bool m_draggingWindow = false;
    QPoint m_dragOffset;
    bool m_deviceDragCandidateActive = false; // wjy: 鼠标按下设备行后先作为拖拽候选，移动超过阈值才进入真正拖拽。
    bool m_draggingDevice = false; // wjy: 当前是否正在拖拽设备；第一步只用于输出识别日志，不改变数据。
    int m_draggingDeviceIndex = -1; // wjy: 当前拖拽候选/正在拖拽的设备下标，-1 表示没有设备被拖拽。

    // 本次真正需要批量移动的所有设备下标
    QSet<int> m_draggingDeviceIndexes;
    QPoint m_deviceDragStartPos; // wjy: 记录设备拖拽起点，用来判断鼠标移动距离是否达到拖拽阈值。
    QPoint m_deviceDragCurrentPos; // wjy: 记录拖拽过程中的当前鼠标位置，用来绘制跟随鼠标移动的半透明设备虚影。
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
    int m_statusAutoRefreshIntervalSeconds = 60; // wjy: 自动刷新间隔默认 60 秒，和设置页数字输入框默认值保持一致。
    int m_deviceListScrollOffset = 0; // wjy: “我的设备”列表的手绘滚动偏移，设备和分组很多时用于上下滚动。
    int m_renamingDeviceGroupIndex = -1; // wjy: 当前正在重命名的分组下标，-1 表示没有分组处于编辑状态。
    // 当前右侧详情页对应的主设备
    int m_selectedDeviceIndex = 0;

    // 左侧所有被选中的设备
    QSet<int> m_selectedDeviceIndexes;

    // Shift 范围选择的起点
    int m_selectionAnchorDeviceIndex = -1;

    int m_previousDeviceIndex = 0;
    QString m_previousDeviceName;
    std::mutex m_backgroundThreadsMutex; // wjy: 保护后台线程列表，析构等待和新任务登记不能同时改 vector。
    std::vector<std::thread> m_backgroundThreads; // wjy: 保存状态刷新/唤醒检测等后台线程，析构时 join 防止关闭阶段堆损坏。
    bool m_shuttingDown = false; // wjy: DeviceGrid 析构开始后不再接受新的后台任务。
    QHash<QString, platform::DevicePresenceState> m_deviceStatuses;
    QTimer* m_detailAnimationTimer = nullptr;
    QTimer* m_desktopHoverTimer = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QTimer* m_statusAutoRefreshTimer = nullptr;
    QTimer* m_wakeVisualTimer = nullptr;
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
