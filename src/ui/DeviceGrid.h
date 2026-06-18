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

class QEvent;
class QComboBox;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QTimer;

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

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
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

    QString m_currentDeviceName;
    QComboBox* m_statusRefreshIntervalCombo = nullptr;
    QLineEdit* m_deviceIpEdit = nullptr;
    QLineEdit* m_deviceNameEdit = nullptr;
    QLineEdit* m_deviceMacEdit = nullptr;
    QLineEdit* m_deviceRemarkEdit = nullptr;
    QPushButton* m_saveDeviceButton = nullptr;
    QPushButton* m_cancelDeviceButton = nullptr;
    QVector<QPushButton*> m_localInfoCopyButtons;
    bool m_draggingWindow = false;
    QPoint m_dragOffset;
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
    int m_statusAutoRefreshIntervalSeconds = 10;
    int m_selectedDeviceIndex = 0;
    int m_previousDeviceIndex = 0;
    QString m_previousDeviceName;
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
