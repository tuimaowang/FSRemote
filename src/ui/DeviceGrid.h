#pragma once

#include "system/DeviceInfoService.h"
#include "system/LocalSystemInfoService.h"
#include "system/DeviceRealtimeStateService.h"
#include "system/DeviceStatusService.h"
#include "system/DeviceStatusRefreshResult.h"
#include "stream/RemoteQualityPolicy.h"
#include "ui/RemoteQualityCoordinator.h"
#include "ui/RemoteInputBroadcastCoordinator.h"
#include "ui/RemoteWindowCoordinator.h"
#include "ui/ScriptPanelController.h"
#include "ui/ScriptUiStateStore.h"

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
class QCheckBox;
class QComboBox;
class QKeyEvent;
class QJsonObject;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QSpinBox;
class QTextEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QVariantAnimation;
class QWheelEvent;

namespace ui {

class DeviceSearchPanel;
class RemoteDesktopWindow;
class RemoteViewerLifecycleManager; // wjy: DeviceGrid统一拥有远控初始化准入和可等待生命周期工作池。

enum class BottomAction {
    None,
    FileTransfer,
    More,
};

class DeviceGrid final : public QFrame, private ScriptPanelController {
    Q_OBJECT

public:
    explicit DeviceGrid(platform::DeviceRealtimeStateService* realtimeStateService, QWidget* parent = nullptr);
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
    void showEvent(QShowEvent* event) override; // wjy: 从托盘或最小化恢复时确保顶部自动隐藏窗口完整滑出。
    void leaveEvent(QEvent* event) override;

private:
    enum class SettingsTab {
        General,
        Keyboard,
        RemoteControl,
    };

    enum class DeviceDetailTab {
        Local, // wjy: 本机页位于脚本左侧，展示控制端系统信息和实时 CPU 占用。
        ScriptLog, // wjy: 枚举顺序与界面从左到右一致，后续新增页签时更容易核对绘制和命中区域。
        Config, // wjy: 配置页固定排在脚本右侧，保持状态定义、绘制顺序和点击顺序一致。
        Search, // wjy: 查找与脚本、配置共用设备详情页签区域，不再使用独立弹窗。
    };

    // =====wjy====
    void setupDeviceSearchPanel(); // wjy: 创建一次嵌入式查找面板，后续页签切换只更新快照、几何和显隐。
    void updateDeviceSearchPanel(); // wjy: 将面板限制在配置页同一内容区域，并在离开查找页时立即隐藏。
    void showDeviceSearchPanel(); // wjy: 点击查找页签或 Ctrl+F 时刷新只读快照、切换页面并聚焦输入框。
    void selectDeviceFromSearch(const QString& deviceId); // wjy: 双击结果后按稳定设备 ID 重新解析、展开分组并定位详情。
    void showLocalSystemInfoTab(); // wjy: 点击本机页签时读取静态信息、建立 CPU/GPU 基线并启动三类资源周期刷新。
    void refreshLocalSystemInfoTab(); // wjy: 重新读取本机静态信息并重置 CPU/GPU/内存显示状态，避免跨页面复用旧样本。
    void animateLocalUsageTo(QVariantAnimation* animation, qreal& displayPercent, int targetPercent); // wjy: 三类资源共用同一缓动入口，让圆环和数字从当前显示值平滑过渡。
    void updateLocalSystemMonitorState(); // wjy: 只在本机页选中时保持 CPU、GPU、内存定时采样，其它页面立即停止。
    // ===end====
    void startDeviceSwitchAnimation(int newIndex, const QString& newName);
    void applySyncedDeviceSnapshot(const QJsonObject& snapshot); // wjy: 在 UI 线程按稳定 ID 应用共享快照，并保留本机分组展开状态和当前选择。
    void syncResponsiveLayoutState() const;
    // =====wjy====
    void setDetailPanelCollapsed(bool collapsed); // wjy: 启动详情栏收起或展开的窗口宽度缓动，不再直接闪变尺寸。
    void finishDetailPanelCollapseTransition(); // wjy: 动画结束后统一固定最终宽度约束、控件显隐和本机采样状态。
    // ===end====
    // =====wjy====
    void ensureTopEdgeAutoHideControllers(); // wjy: 首次顶部停靠时延迟创建位置动画、全局鼠标监视和收起延迟定时器。
    void updateTopEdgeAutoHideAfterWindowDrag(const QPoint& releaseGlobalPosition); // wjy: 拖窗松开后按鼠标所在屏幕判断是否吸附顶部。
    void setTopEdgeAutoHidden(bool hidden); // wjy: 在当前屏幕顶部与仅露出触发条的位置之间平滑移动主窗口。
    void monitorTopEdgeAutoHide(); // wjy: 窗口缩入屏幕后仍检查全局鼠标，进入顶部触发条即可滑下。
    void cancelTopEdgeAutoHide(); // wjy: 用户重新拖窗、缩放或退出时停止所有自动位置修改。
    // ===end====
    void beginWindowResize(const QPoint& position, const QPoint& globalPosition);
    void updateWindowResize(const QPoint& globalPosition);
    void finishWindowResize();
    void setDesktopHoverActive(bool active);
    void updateDesktopHover(const QPoint& position);
    void updateBottomActionHover(const QPoint& position);
    void clearBottomActionHover();
    // =====wjy====
    void animateDeviceListScrollTo(int targetOffset); // wjy: 设备轨道空白点击后用短促缓动移动滑块，直接拖动仍保持实时跟手。
    void animateSettingsScrollTo(int targetOffset); // wjy: 常规设置轨道点击使用同样的快速缓动，并同步移动真实输入控件。
    // ===end====
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
    void updateRemoteWindowDevice(const QString& hostIp); // wjy: 远控标题栏按钮按固定 IP 复用设备右键菜单的单设备更新逻辑。
    // ===end====
    void openCurrentDeviceTerminal();
    bool openTerminalForDeviceIndex(int deviceIndex, bool showMessages);
    void executeCurrentDeviceScriptFolder(const QString& scriptFolderPath); // wjy: Copy one shared script folder to remote work directory and run its entry script.
    bool executeDeviceScriptFolder(int deviceIndex, const QString& scriptFolderPath, bool showMessages); // wjy: Run one shared script folder on a specified device without forcing the current selection.
    void executeDeviceGroupScriptFolder(int groupIndex, const QString& scriptFolderPath); // wjy: Run one selected script folder for every device in a group.
    void openRemoteDesktopWindow();
    void openRemoteDesktopWindowForDevice(int deviceIndex); // wjy: 首次窗口的位置由 RemoteDesktopWindow 统一按当前屏幕居中，不再由设备列表附加偏移。
    void launchSelectedRemoteDesktopWindows();
    void openDeviceGroupTiledWindows(int groupIndex); // wjy: Open all devices in one group and tile remote windows.
    QVector<QPointer<RemoteDesktopWindow>> openedRemoteWindows() const;
    void setRemoteUpdateAvailability(const QString& hostIp, bool available); // wjy: 同步缓存并刷新同 IP 的普通/平铺远控窗口更新按钮。
    void refreshOpenedRemoteUpdateAvailability(); // wjy: 仅轮询当前已打开远控窗口的目标版本，不依赖设备列表自动刷新开关。
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
    void applyDeviceStatusRefreshResult(platform::DeviceStatusRefreshResult refreshResult); // wjy: 在 UI 线程统一归并状态刷新结果，隔离后台采集与控件更新职责。
    void probePoweringOnDevices();
    // =====wjy====
    void updateRealtimeConfiguredDevices(); // wjy: 把当前 devices.json 中的 IP 白名单同步给 UDP 接收器，广播不能创建未配置设备。
    void applyRealtimeDeviceState(const QString& deviceIp, const platform::DeviceRealtimeReducedState& state); // wjy: 只消费单线程归并后的最终状态，统一刷新在线、人数、提示和脚本三态。
    void publishRemoteControllerTarget(RemoteDesktopWindow* window, const QString& deviceName, const QString& deviceIp); // wjy: 远控窗口创建时发布本机目标租约，仅用于双向诊断。
    void removeRemoteControllerTarget(RemoteDesktopWindow* window); // wjy: 窗口销毁立即移除对应租约并广播最新完整快照。
    // ===end====
    platform::DevicePresenceState devicePresenceForIndex(int index) const;
    bool devicePoweringOnForIndex(int index) const;
    int devicePoweringOnRemainingSecondsForIndex(int index) const;
    void setupSettingsControls();
    void updateSettingsControls();
    // =====wjy====
    void refreshRollbackVersions(bool forceRefresh = false); // wjy: 后台枚举共享历史版本；普通进入设置可复用短时缓存，发布或失败恢复时允许强制刷新。
    // ===end====
    void applySettingsControlGeometry(QWidget* control, const QRect& geometry, bool visible, bool enabled, bool raiseWhenVisible); // wjy: 统一设置页真实控件的几何、显隐、可用状态和层级，避免每个字段重复维护同一套生命周期逻辑。
    void applyDesktopWallpaperRotationSetting(bool rotateImmediately); // wjy: 根据持久化开关启停分钟定时器，用户开启时可立即触发首轮。
    void startDesktopWallpaperRotation(bool userInitiated); // wjy: 后台选择并应用下一张共享图片，防止网络访问阻塞设置界面或产生重叠任务。
    void saveRemoteQualitySettingsFromControls(); // wjy: 收集远控画质页字段、统一归一化持久化并立即通知跟随全局的窗口。
    void registerRemoteQualityWindow(RemoteDesktopWindow* window); // wjy: 普通和平铺窗口共用同一套质量注册、销毁清理和即时重算逻辑。
    void requestRemoteQualityEvaluation(); // wjy: 合并同一事件循环内多次窗口变化，最多排队一个全局质量计算任务。
    void evaluateRemoteQuality(); // wjy: 每秒汇总全部不断流窗口并下发分辨率优先、FPS次级的在线质量决策。
    void saveShortcutKeySetting(int shortcutIndex, const QString& shortcutText); // wjy: Save one keyboard shortcut when its editor loses focus or receives Enter.
    void registerGlobalShortcuts();
    void unregisterGlobalShortcuts();
    void triggerShortcutAction(int shortcutIndex);
    void releaseRemoteShortcutKeyState(int shortcutIndex);
    // =====wjy====
    void applyPeriodicDeviceDiscoverySetting(bool scanImmediately);
    void startBatchAddDevices(bool userInitiated = true); // wjy: 手动按钮和周期定时器复用同一网段扫描；周期触发时不跳转当前页面或选择新设备。
    // ===end====
    // =====wjy====
    QString nextDefaultDeviceGroupName() const; // wjy: 为所有新建分组入口生成不重复的“默认分组N”名称。
    int createDefaultDeviceGroup(); // wjy: 只创建带稳定 ID 且默认展开的分组，保存和设备归属由调用流程统一提交。
    bool assignDevicesToGroup(const QVector<int>& deviceIndexes, int groupIndex); // wjy: 去重并过滤菜单目标，把有效设备一次性写入指定分组。
    void revealDeviceGroup(int groupIndex, bool beginRename); // wjy: 展开并滚动到目标分组，按需进入默认名称全选编辑状态。
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
    QVector<int> normalizedBatchDeviceIndexes(const QVector<int>& deviceIndexes) const; // wjy: 批量动作统一过滤过期/重复下标，再按稳定 ID 恢复当前展示位置。
    void batchWakeDevices(const QVector<int>& deviceIndexes);
    void batchShutdownDevices(const QVector<int>& deviceIndexes);
    void batchRestartDevices(const QVector<int>& deviceIndexes);
    // =====wjy====
    void batchExecuteDeviceScriptFolder(const QVector<int>& deviceIndexes, const QString& scriptFolderPath, const QString& noExecutableMessage); // wjy: 多选设备和分组脚本共用统一批量入口，一次校验脚本后再逐台启动。
    void batchUpdateDevices(const QVector<int>& deviceIndexes); // wjy: 多选设备或分组菜单统一复用单设备更新请求逻辑。
    // ===end====
    void batchOpenDeviceTerminals(const QVector<int>& deviceIndexes);
    bool ensureRemoteControlAuthorization(int deviceIndex, bool showMessages); // wjy: 远控窗口创建前确保当前控制端公钥已登记到目标设备。

    // =====wjy====
    platform::DeviceRealtimeStateService* m_realtimeStateService = nullptr; // wjy: 由 main 持有且晚于 MainWindow 销毁，DeviceGrid 只连接和调用，不负责释放。
    RemoteInputBroadcastCoordinator m_remoteInputBroadcastCoordinator; // wjy: 单一协调器覆盖普通和平铺窗口，DeviceGrid 析构前先由窗口注销并完成同步输入释放。
    std::unique_ptr<RemoteViewerLifecycleManager> m_remoteViewerLifecycleManager; // wjy: 生命周期晚于窗口批量stop，析构时再次兜底join固定工作线程。
    RemoteQualityCoordinator m_remoteQualityCoordinator; // wjy: 一个控制端统一协调全部远控窗口，高质量锁定按优先级最后降级但不能突破稳定性硬边界。
    QTimer* m_remoteQualityTimer = nullptr; // wjy: 1秒采样接收FPS/码率，最小化和用户切换模式另走即时重算信号。
    bool m_remoteQualityEvaluationQueued = false; // wjy: 多窗口同时最小化或创建时合并为一个Qt任务，避免事件队列放大。
    qint64 m_lastRemoteResourceDiagnosticAtMs = 0; // wjy: 资源快照限制为30秒一次，避免稳定性诊断本身成为性能热点。
    QHash<RemoteDesktopWindow*, QString> m_realtimeControllerTargetSessionIds; // wjy: 每个普通/平铺窗口映射一个唯一目标租约，关闭时精确删除。
    QHash<QString, platform::RealtimeScriptState> m_deviceRealtimeScriptStates; // wjy: Unknown/Idle/Running 独立于可恢复脚本 UI 元数据，离线只隐藏 Logo 不破坏恢复信息。
    // ===end====
    QString m_currentDeviceName;
    ScriptUiStateStore m_scriptUiStateStore; // wjy: 每设备脚本状态由独立仓储唯一持有，DeviceGrid 只投影当前设备页面。
    QLineEdit* m_periodicDeviceDiscoveryIntervalEdit = nullptr; // wjy: 周期检查新增设备的秒数输入框，默认 60 秒。
    QLineEdit* m_batchSubnetEdit = nullptr; // wjy: 批量新增网段输入框，支持以空格分隔多个 IPv4 通配网段。
    QPushButton* m_batchAddButton = nullptr; // wjy: 批量新增按钮，扫描期间禁用避免重复启动。
    QLineEdit* m_wallpaperRotationIntervalEdit = nullptr; // wjy: 自动壁纸轮换分钟输入框，仅在开关开启且卡片可见时显示。
    // =====wjy====
    QComboBox* m_rollbackVersionCombo = nullptr; // wjy: 常规页只展示低于当前安装版本且关键载荷完整的共享历史版本。
    // ===end====
    QVector<QLineEdit*> m_shortcutKeyEdits; // wjy: Keyboard settings shortcut editors, one per remote-window action.
    // =====wjy====
    stream::RemoteQualityConfiguration m_remoteQualityConfiguration; // wjy: 主窗口缓存当前持久化全局画质，所有跟随全局窗口读取同一份值。
    QComboBox* m_remoteQualityModeCombo = nullptr;
    QSpinBox* m_remoteTargetFpsSpin = nullptr;
    QSpinBox* m_remoteMinimumVisibleFpsSpin = nullptr;
    QSpinBox* m_remoteSevereMinimumFpsSpin = nullptr;
    QComboBox* m_remoteMinimumResolutionCombo = nullptr;
    QSpinBox* m_remoteMinimizedFpsSpin = nullptr;
    QComboBox* m_remoteMinimizedResolutionCombo = nullptr;
    QSpinBox* m_remoteDegradationDelaySpin = nullptr;
    QSpinBox* m_remoteRecoveryDelaySpin = nullptr;
    QSpinBox* m_remoteReceiveBudgetSpin = nullptr;
    QCheckBox* m_remoteAutomaticRecoveryCheck = nullptr; // wjy: 数值和下拉框使用真实Qt控件，保证键盘输入和可访问性可靠。
    // ===end====
    QSet<int> m_registeredGlobalShortcutIds;
    quintptr m_globalShortcutWindowHandle = 0;
    QLineEdit* m_deviceIpEdit = nullptr;
    QLineEdit* m_deviceNameEdit = nullptr;
    QLineEdit* m_deviceMacEdit = nullptr;
    QLineEdit* m_deviceRemarkEdit = nullptr;
    QLineEdit* m_deviceGroupNameEdit = nullptr; // wjy: Group rename editor.
    QLineEdit* m_deviceListNameEdit = nullptr; // wjy: Device rename editor shown directly on the left list row.
    DeviceSearchPanel* m_deviceSearchPanel = nullptr; // wjy: 主界面右侧查找页的唯一控件实例，生命周期由 DeviceGrid 父子关系管理。
    platform::LocalSystemInfo m_localSystemInfo; // wjy: 本机详情页展示的静态系统和网络快照，不写回设备目录。
    platform::CpuUsageSampler m_localCpuUsageSampler; // wjy: 保存相邻 GetSystemTimes 样本，只在本机页可见期间推进基线。
    platform::GpuUsageSampler m_localGpuUsageSampler; // wjy: 持有 Windows GPU Engine 性能计数器查询，只在本机页可见期间采样。
    platform::MemoryUsageSampler m_localMemoryUsageSampler; // wjy: 每秒只读当前物理内存负载，不保存跨页面基线。
    QTimer* m_localSystemInfoTimer = nullptr; // wjy: 本机页 CPU、GPU、内存占用共用一秒刷新定时器，离开页面后停止。
    QVariantAnimation* m_localCpuUsageAnimation = nullptr; // wjy: CPU 圆环在相邻样本间平滑扫动。
    QVariantAnimation* m_localGpuUsageAnimation = nullptr; // wjy: GPU 圆环独立缓动，不与 CPU 新样本互相抢占动画状态。
    QVariantAnimation* m_localMemoryUsageAnimation = nullptr; // wjy: 内存圆环独立缓动，并与 CPU/GPU 使用相同速度曲线。
    qreal m_localCpuUsagePercent = -1.0; // wjy: CPU 动画中的小数显示值；负数表示正在建立采样基线。
    qreal m_localGpuUsagePercent = -1.0; // wjy: GPU 动画显示值；性能计数器不可用时保持负数并显示占位。
    qreal m_localMemoryUsagePercent = -1.0; // wjy: 内存动画显示值；有效结果始终限制在 0-100。
    QTextEdit* m_scriptFileEdit = nullptr;
    QTextEdit* m_scriptOutputEdit = nullptr; // wjy: 脚本日志正文使用只读文本控件，支持鼠标选择、Ctrl+C和右键复制。
    QPushButton* m_scriptFileSaveButton = nullptr;
    QTreeWidget* m_scriptFolderTree = nullptr; // wjy: Script Log tab tree used to choose a shared script folder before execution.
    QPushButton* m_saveDeviceButton = nullptr;
    QPushButton* m_cancelDeviceButton = nullptr;
    QVector<QPushButton*> m_localInfoCopyButtons;
    // =====wjy====
    QVariantAnimation* m_topEdgeAutoHideAnimation = nullptr; // wjy: 只改变顶层窗口 y 坐标，保持横向位置和窗口尺寸不变。
    QTimer* m_topEdgeAutoHideMonitorTimer = nullptr; // wjy: 低频读取全局鼠标位置，使仅剩 4px 的隐藏窗口仍能响应顶部悬停。
    QTimer* m_topEdgeAutoHideDelayTimer = nullptr; // wjy: 鼠标离开后延迟收起，避免经过标题栏边缘时窗口立即消失。
    QRect m_topEdgeDockScreenGeometry; // wjy: 保存停靠屏幕的工作区，支持副屏负坐标和不同顶部位置。
    bool m_topEdgeDocked = false; // wjy: true 表示本次拖窗已明确停靠到某块屏幕顶部。
    bool m_topEdgeAutoHidden = false; // wjy: 记录位置动画的目标状态，用于滑动中途正确处理鼠标重新进入。
    // ===end====
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
    DeviceDetailTab m_deviceDetailTab = DeviceDetailTab::Local; // wjy: 主控窗口首次显示设备详情时默认进入本机页，脚本页仍可通过右侧页签手动切换。
    // ===end====
    // =====wjy====
    bool m_detailPanelCollapsed = false; // wjy: true 只隐藏右侧详情栏，左侧设备栏在两种状态下都保持完整可用。
    int m_expandedWindowWidth = 920; // wjy: 收起前记录用户当前窗口宽度，重新展开时保持当前位置并恢复该宽度。
    QVariantAnimation* m_detailPanelWidthAnimation = nullptr; // wjy: 逐帧修改主窗口宽度，让详情栏从右侧平滑收起或展开。
    bool m_detailPanelAnimationTargetCollapsed = false; // wjy: 动画期间保存最终折叠状态，收起时允许详情内容保持到滑动结束再隐藏。
    // ===end====
    bool m_settingsLocalInfoExpanded = false;
    bool m_settingsAddDeviceExpanded = false;
    bool m_autoRunEnabled = true;
    bool m_remoteWakeupEnabled = false;
    bool m_wolDetectionInProgress = false;
    bool m_preventSleepEnabled = true;
    bool m_periodicDeviceDiscoveryEnabled = false; // wjy: 默认关闭，开启后按批量新增输入框中的网段周期扫描。
    bool m_statusRefreshInProgress = false;
    bool m_remoteUpdateAvailabilityRefreshInProgress = false; // wjy: 防止上一轮目标版本查询未结束时重复创建后台任务。
    bool m_wakeProbeInProgress = false;
    bool m_batchAddInProgress = false; // wjy: 标记批量新增扫描是否正在后台执行。
    bool m_wallpaperRotationEnabled = false; // wjy: 新安装默认关闭，避免未经用户选择就修改当前桌面。
    bool m_wallpaperRotationInProgress = false; // wjy: 网络目录访问和缓存写入期间阻止定时器启动重叠任务。
    int m_periodicDeviceDiscoveryIntervalSeconds = 60; // wjy: 周期新增设备默认每 60 秒扫描一次。
    int m_wallpaperRotationIntervalMinutes = 1; // wjy: 自动壁纸默认每 1 分钟切换一次，设置页允许修改。
    QString m_lastWallpaperSourcePath; // wjy: 只记录最后成功应用的共享源文件，下一轮据此按稳定顺序继续。
    QString m_wallpaperRotationStatusText; // wjy: 在设置卡片展示正在切换、成功文件名或最近失败原因，定时失败不使用重复弹窗。
    // =====wjy====
    bool m_updateAvailable = false; // wjy: 后台检测到更高版本时显示标题栏更新按钮，用户点击前绝不开始安装。
    bool m_updatePreparing = false; // wjy: 防止用户连续点击重复创建多个更新任务和更新器进程。
    QString m_availableUpdateVersion; // wjy: 保存检测到的远端版本，供标题栏更新状态使用。
    bool m_rollbackPreparing = false; // wjy: 回撤载荷暂存期间禁用下拉框和按钮，防止同一目标重复启动多个事务任务。
    bool m_rollbackVersionsRefreshInProgress = false; // wjy: 共享目录扫描在后台运行时阻止设置点击重复创建遍历任务。
    bool m_rollbackVersionsRefreshPending = false; // wjy: 扫描期间收到强制刷新请求时，在当前结果返回后补做一次最新扫描。
    QElapsedTimer m_rollbackVersionsRefreshClock; // wjy: 普通重复进入设置在 30 秒内复用下拉框结果，减少网络目录元数据请求。
    // ===end====
    int m_deviceListScrollOffset = 0; // wjy: Scroll offset for the hand-painted device list.
    // =====wjy====
    bool m_draggingDeviceListScrollbar = false; // wjy: 按住左侧设备列表滑块时独占鼠标移动，避免同时触发设备或分组拖拽。
    int m_deviceListScrollbarGrabOffsetY = 0; // wjy: 保存鼠标相对滑块顶部的位置，开始拖动时滑块不会突然跳到鼠标中心。
    QVariantAnimation* m_deviceListScrollbarAnimation = nullptr; // wjy: 轨道空白点击时负责设备滑块的短距离快速缓动，不参与直接拖拽。
    // ===end====
    int m_settingsScrollOffset = 0; // wjy: Scroll offset for the Settings > General content area.
    // =====wjy====
    bool m_draggingSettingsScrollbar = false; // wjy: 常规设置滚动条按下后独占鼠标移动，避免同时触发卡片、按钮或真实输入框。
    int m_settingsScrollbarGrabOffsetY = 0; // wjy: 记录鼠标相对设置滑块顶部的距离，拖拽滑块时保持抓取点不跳动。
    QVariantAnimation* m_settingsScrollbarAnimation = nullptr; // wjy: 设置轨道点击动画逐帧刷新手绘卡片和真实子控件位置。
    // ===end====
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
    QHash<QString, bool> m_deviceUpdateAvailability; // wjy: 设备状态刷新线程统一维护目标是否需要更新，远控窗口只消费结果。
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
    QTimer* m_remoteUpdateAvailabilityTimer = nullptr; // wjy: 打开远控窗口期间每 10 秒检查一次目标是否发现共享新版。
    QTimer* m_periodicDeviceDiscoveryTimer = nullptr; // wjy: 到期后复用批量新增扫描，已有扫描进行中时自动跳过本轮。
    QTimer* m_wallpaperRotationTimer = nullptr; // wjy: 开关开启后按用户分钟数触发下一张壁纸，关闭或退出时立即停止。
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
    QHash<QString, QString> m_pendingRemoteRenameNames; // wjy: 记录远端改名等待重启生效的设备，避免自动刷新立刻用旧电脑名覆盖手动新名字。
    platform::DeviceInfo m_localDeviceInfo;
    std::unique_ptr<RemoteWindowCoordinator> m_remoteWindowCoordinator; // wjy: 远控窗口集合、激活顺序、平铺恢复几何和 close-all 的唯一所有者。
    int m_lastShortcutActionIndex = -1;
    qint64 m_lastShortcutActionAtMs = 0;
};

} // namespace ui
