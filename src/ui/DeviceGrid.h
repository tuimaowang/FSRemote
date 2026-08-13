#pragma once

#include "system/DeviceInfoService.h"
#include "system/LocalNetworkBandwidthMonitor.h" // wjy: 主窗口直接持有只读网卡采样器，标题栏不启动独立监控进程。
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
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QEvent;
class QByteArray;
class QComboBox;
class QKeyEvent;
class QJsonObject;
class QLineEdit;
class QMenu;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QTextEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QVariantAnimation;
class QWheelEvent;

namespace ui {

class DeviceSearchPanel;
class RemoteDesktopWindow;
class RemoteMonitorWindow; // wjy: 专用监控槽位复用远控流内核，只保留设备名、IP和连续计时标题栏。
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
    void mouseDoubleClickEvent(QMouseEvent* event) override; // wjy: 双击设备启动当前单选或多选远控；双击分组仍进入原地重命名。
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override; // wjy: The hand-painted device list handles scrolling here.
    void keyPressEvent(QKeyEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override; // wjy: 从托盘或最小化恢复时确保任一屏幕边缘自动隐藏窗口完整滑出。
    void leaveEvent(QEvent* event) override;

private:
    enum class SettingsTab {
        General,
        Keyboard,
        RemoteControl,
        RemoteMonitor,
    };

    enum class DeviceDetailTab {
        Local, // wjy: 本机页位于脚本左侧，展示控制端系统信息和实时 CPU 占用。
        ScriptLog, // wjy: 枚举顺序与界面从左到右一致，后续新增页签时更容易核对绘制和命中区域。
        Config, // wjy: 配置页固定排在脚本右侧，保持状态定义、绘制顺序和点击顺序一致。
        Search, // wjy: 查找与脚本、配置共用设备详情页签区域，不再使用独立弹窗。
    };

    // =====wjy====
    struct ScriptLaunchPreflightResult {
        bool entryAvailable = false; // wjy: 后台已经确认脚本目录存在受支持入口，主线程不再对 UNC 路径调用 exists。
        QString entryScriptPath; // wjy: 保存后台解析出的实际入口文件，后续运行命令和编辑器继续使用同一文件。
        QString entryScriptHash; // wjy: 保存入口脚本内容 SHA-256，工作区 Hash 变化时创建新副本而不覆盖旧配置。
        platform::DeviceStatusInfo remoteStatus; // wjy: 一次状态查询同时携带终端用户和权威脚本运行态，避免重复连接 49101。
        bool authorizationSucceeded = false; // wjy: 公钥已成功登记到目标端后才允许进入原脚本启动状态机。
        QString errorMessage; // wjy: 后台脚本入口、状态或授权失败的详细原因只在主线程显示。
    };
    // ===end====

    // =====wjy====
    struct RemoteMonitorSlot {
        QPointer<RemoteMonitorWindow> window; // wjy: 宫格槽位长期持有同一个顶层窗口，轮询只替换其内部Viewer视频源。
        QString deviceKey; // wjy: 记录当前页分配到该槽位的稳定设备键，重复刷新同一页不触发无意义切源。
    };
    // ===end====

    // =====wjy====
    enum class ScreenEdgeDock {
        None, // wjy: 主窗口当前没有停靠，不启用自动隐藏监视和位置动画。
        Top, // wjy: 顶部停靠时沿 y 轴向屏幕外收起，保持现有顶部行为不变。
        Left, // wjy: 左侧停靠时沿 x 轴向左收起，仅保留窄触发条。
        Right, // wjy: 右侧停靠时沿 x 轴向右收起，仅保留窄触发条。
    };
    // ===end====

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
    void ensureScreenEdgeAutoHideControllers(); // wjy: 首次停靠顶部、左侧或右侧时统一创建位置动画、全局鼠标监视和收起延迟定时器。
    void updateScreenEdgeAutoHideAfterWindowDrag(const QPoint& releaseGlobalPosition); // wjy: 拖窗松开后按目标屏幕工作区判断顶部、左侧或右侧停靠方向。
    QPoint screenEdgeAutoHideTargetPosition(bool hidden) const; // wjy: 根据当前停靠方向计算展开位置或仅露出触发条的隐藏位置。
    void setScreenEdgeAutoHidden(bool hidden); // wjy: 使用同一段位置动画沿当前停靠方向平滑收起或展开主窗口。
    void monitorScreenEdgeAutoHide(); // wjy: 窗口缩入任一边缘后继续检查全局鼠标，进入对应触发条即可滑出。
    void cancelScreenEdgeAutoHide(); // wjy: 用户重新拖窗、缩放或退出时停止所有边缘自动位置修改。
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
    bool scheduleOpenTerminals(const QVector<int>& deviceIndexes, bool showMessages); // wjy: 终端用户名查询、密钥授权和进程启动统一在一个后台批次完成。
    void executeCurrentDeviceScriptFolder(const QString& scriptEntryPath); // wjy: Copy the selected entry file's parent folder and run that exact entry on the current device.
    bool executeDeviceScriptFolder(int deviceIndex, const QString& scriptEntryPath, bool showMessages); // wjy: Run the selected entry file's parent folder on one specified device.
    void executeDeviceGroupScriptFolder(int groupIndex, const QString& scriptEntryPath); // wjy: Run one selected entry file for every device in a group.
    void openRemoteDesktopWindow();
    void openRemoteDesktopWindowForDevice(int deviceIndex); // wjy: 首次窗口的位置由 RemoteDesktopWindow 统一按当前屏幕居中，不再由设备列表附加偏移。
    void launchSelectedRemoteDesktopWindows();
    void openDeviceGroupTiledWindows(int groupIndex); // wjy: Open all devices in one group and tile remote windows.
    void authorizeRemoteControlDevices(
        const QVector<QString>& deviceIds,
        bool showMessages,
        std::function<void(const QVector<QString>&)> completion); // wjy: 普通、多选和平铺远控共用一个后台公钥授权批次。
    void openAuthorizedTiledWindows(const QVector<QString>& deviceIds); // wjy: 后台授权完成后仅在主线程创建、注册并排列平铺窗口。
    QVector<QPointer<RemoteDesktopWindow>> openedRemoteWindows() const;
    void setRemoteUpdateAvailability(const QString& hostIp, bool available); // wjy: 同步缓存并刷新同 IP 的普通/平铺远控窗口更新按钮。
    bool realtimeUpdateAvailable(const platform::DeviceRealtimeUpdateState& updateState) const;
    void refreshRealtimeUpdateAvailability();
    void rememberRemoteWindowActivation(RemoteDesktopWindow* window);
    RemoteDesktopWindow* topmostRemoteWindow() const;
    void toggleTopmostRemoteWindowFullscreen();
    void toggleRemoteWindowTiling();
    void toggleRemoteMonitorMode(); // wjy: 主窗口标题栏按钮开启独立设备轮询或关闭全部监控窗口，普通远控窗口不参与切换。
    void setRemoteMonitorModeEnabled(bool enabled); // wjy: 统一管理监控设备扫描、分页定时器和独立只读Viewer生命周期。
    void refreshRemoteMonitorMode(bool advancePage); // wjy: 从黑名单优先、白名单强制纳入后的设备集合中选出当前页，并只切换固定槽位Viewer视频源。
    void applyRemoteMonitorQualityPreset(); // wjy: 将设置页监控画质只同步到独立监控窗口，普通远控保留自身自动或手选档。
    int titlebarRemoteSessionCount() const; // wjy: 标题栏显示普通远控会话数加当前页监控Viewer数，宫格容量直接限制监控增量。
    QVector<int> remoteMonitorDeviceIndexes() const; // wjy: 返回黑名单优先排除、白名单绕过普通筛选后的有效远端设备，并按稳定自然名称轮询。
    QString remoteMonitorDeviceKey(int deviceIndex) const; // wjy: 优先使用设备稳定ID，兼容旧记录时回退规范化IP作为固定槽位的当前来源键。
    void ensureRemoteMonitorWindowSlots(int slotCount); // wjy: 仅在开启监控或宫格容量变化时增减专用窗口，普通轮询不会创建新窗口。
    QVector<QPointer<RemoteDesktopWindow>> openedRemoteMonitorWindows() const; // wjy: 清理空指针后返回当前独立监控窗口快照。
    QVector<QPointer<RemoteDesktopWindow>> qualityManagedRemoteWindows() const; // wjy: 质量协调器同时覆盖普通与监控窗口，其它快捷键和关闭命令仍只操作普通远控。
    void closeRemoteMonitorWindows(); // wjy: 关闭监控模式时停止并释放全部只读Viewer，不恢复进入前的任何窗口布局。
    void closeTopmostRemoteWindow();
    void closeAllRemoteWindows();
    void refreshLocalDeviceInfo();
    void shutdownCurrentDevice();
    void restartCurrentDevice();
    bool shutdownDeviceForIndex(int deviceIndex, bool showMessages);
    bool restartDeviceForIndex(int deviceIndex, bool showMessages);
    // =====wjy====
    bool updateDeviceForIndex(int deviceIndex, bool showMessages); // wjy: 向指定在线设备发送远程更新请求，并区分已受理、已是最新版和失败。
    bool scheduleDevicePowerActions(const QVector<int>& deviceIndexes, bool restart, bool showMessages); // wjy: 单个和批量关机/重启统一进入一个后台批次，弱网等待不再停住 UI。
    bool scheduleDeviceUpdateRequests(const QVector<int>& deviceIndexes, bool showMessages); // wjy: 单个和批量更新请求统一后台发送，并在主线程归并窗口遮罩和提示。
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
    void applyRemoteInputScriptRuntimeState(const QString& deviceIp, const platform::RemoteInputScriptRuntimeInfo& runtime); // wjy: 同一IP的普通、平铺和后来重开的窗口共用一份目标端F10权威状态。
    void publishRemoteControllerTarget(RemoteDesktopWindow* window, const QString& deviceName, const QString& deviceIp); // wjy: 远控窗口创建时发布本机目标租约，仅用于双向诊断。
    void removeRemoteControllerTarget(RemoteDesktopWindow* window); // wjy: 窗口销毁立即移除对应租约并广播最新完整快照。
    // ===end====
    platform::DevicePresenceState devicePresenceForIndex(int index) const;
    int totalRemoteControlSessionCount() const; // wjy: 汇总设备列表中的真实远控会话路数，标题栏数字与设备行徽标共用同一状态口径。
    bool devicePoweringOnForIndex(int index) const;
    int devicePoweringOnRemainingSecondsForIndex(int index) const;
    void setupSettingsControls();
    void updateSettingsControls();
    // =====wjy====
    void refreshRollbackVersions(bool forceRefresh = false); // wjy: 后台枚举共享历史版本；普通进入设置可复用短时缓存，发布或失败恢复时允许强制刷新。
    void startRollbackVersionsRefresh(); // wjy: SMB 连接测试成功后才真正创建历史版本枚举任务。
    void handleSharedStorageProbeFinished(bool available); // wjy: 一轮异步探测同时承接等待中的壁纸和回撤请求。
    // ===end====
    void applySettingsControlGeometry(QWidget* control, const QRect& geometry, bool visible, bool enabled, bool raiseWhenVisible); // wjy: 统一设置页真实控件的几何、显隐、可用状态和层级，避免每个字段重复维护同一套生命周期逻辑。
    void applyDesktopWallpaperRotationSetting(bool rotateImmediately); // wjy: 根据持久化开关启停分钟定时器，用户开启时可立即触发首轮。
    void startDesktopWallpaperRotation(bool userInitiated); // wjy: 先异步探测 SMB，连接成功才进入真实壁纸任务。
    void performDesktopWallpaperRotation(bool userInitiated); // wjy: 已通过连接门禁后在后台选择并应用下一张共享图片。
    void saveRemoteQualitySettingsFromControls(); // wjy: 收集远控画质页字段、统一归一化持久化并立即通知跟随全局的窗口。
    void refreshRemoteMonitorFilterList(bool preserveDraft); // wjy: 合并设备目录、持久化名单和未应用草稿，刷新黑白名单勾选表格。
    void saveRemoteMonitorFilterSettings(); // wjy: 点击应用后收集勾选项、持久化黑白名单并刷新当前监控页。
    void registerRemoteQualityWindow(RemoteDesktopWindow* window, bool monitorWindow = false); // wjy: 普通窗口恢复设备手选档；监控窗口只使用统一监控档位并纳入同一质量协调器。
    void requestRemoteQualityEvaluation(); // wjy: 合并同一事件循环内多次窗口变化，最多排队一个全局质量计算任务。
    void evaluateRemoteQuality(); // wjy: 每秒汇总窗口并在状态事件时即时重算，手选/监控/自动按优先级恢复，完全遮挡临时360/1。
    void saveShortcutKeySetting(int shortcutIndex, const QString& shortcutText); // wjy: Save one keyboard shortcut when its editor loses focus or receives Enter.
    void registerGlobalShortcuts();
    void unregisterGlobalShortcuts();
    void triggerShortcutAction(int shortcutIndex);
    void releaseRemoteShortcutKeyState(int shortcutIndex);
    // =====wjy====
    RemoteDesktopWindow* focusedRemoteWindow() const; // wjy: 截图只认当前真实活动远控窗口，最近激活但已经失焦的窗口不能截远端。
    void triggerScreenshotCapture(); // wjy: F12统一入口按焦点选择远端目标或本机回退。
    void requestRemoteScreenshot(RemoteDesktopWindow* remoteWindow); // wjy: 后台等待目标端截图命令，主线程只显示结果窗口。
    void captureLocalScreenshot(); // wjy: 没有远控焦点时直接调用本机原始主屏截图服务。
    void showScreenshotReview(const QString& filePath, QWidget* preferredParent); // wjy: 控制端统一预览、重命名并决定保留或删除共享PNG。
    // ===end====
    // =====wjy====
    void applyPeriodicDeviceDiscoverySetting(bool scanImmediately);
    void startBatchAddDevices(bool userInitiated = true); // wjy: 手动按钮和周期定时器复用同一网段扫描；周期触发时不跳转当前页面或选择新设备。
    void syncHideLocalDeviceSettingFromCatalog(); // wjy: 从共享设备实体读取本机全局隐藏状态，确保设置开关与所有客户端共同使用的记录一致。
    void migrateLegacyHideLocalDeviceSetting(); // wjy: 将旧版仅本机生效的开关一次性迁移到共享设备记录，成功后不再读取旧键。
    void applyHideLocalDeviceSetting(bool persistSharedState); // wjy: 用户点击时更新本机设备实体并提交共享同步；普通刷新只重新应用全局过滤结果。
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
    // =====wjy====
    struct BackgroundThreadEntry {
        std::thread thread; // wjy: 保留可正常 join 的真实工作线程，禁止 detach 或强制终止。
        std::shared_ptr<std::atomic_bool> finished; // wjy: 任务入口退出前置位，后续新任务可安全回收已完成线程句柄。
    };
    void reapCompletedBackgroundThreads(); // wjy: 每次登记新任务前移除并 join 已结束线程，长期运行不持续积累句柄。
    void runBackgroundTask(std::function<void()> task); // wjy: 后台任务保持可汇合，并在统一线程入口拦截异常，避免异常越界导致进程终止。
    void cancelBlockingBackgroundIo(); // wjy: 退出阶段请求中断 DeviceGrid 后台线程正在等待的 Windows 同步文件 I/O，防止不可达网盘拖住析构。
    // ===end====
    void setupScriptFileEditor();
    // =====wjy====
    void setupScriptFolderTree();
    void requestScriptFolderTreeRefresh(); // wjy: 先异步检测共享服务器，主窗口构造阶段不直接触碰 UNC。
    void startScriptFolderTreeLoad(); // wjy: 连接成功后才登记后台目录枚举任务。
    void applyScriptFolderTreeSnapshot(const QJsonObject& snapshot); // wjy: 后台结果回到 UI 线程后一次性创建树节点。
    void setScriptFolderTreePlaceholder(const QString& text); // wjy: 检测、加载和离线状态都使用无网络访问的本地占位节点。
    void populateCachedScriptFolderMenu(QMenu* menu) const; // wjy: 右键菜单复用已加载树快照，不再临时同步遍历网盘。
    // ===end====
    void selectScriptFolderTreeItem(QTreeWidgetItem* item);
    void syncScriptFolderTreeSelection();
    void updateScriptFileEditorControls();
    void loadScriptFileEditor(const QString& deviceIp, const QString& loginUser, const QString& scriptWorkName);
    void saveScriptFileEditor();
    void stopCurrentDeviceScript(); // wjy: Stop the running script on the target device, not only the local SSH process.
    bool stopDeviceScriptForDeviceIndex(int deviceIndex, bool showMessages); // wjy: Stop a running script for one specified device, used by current-device and group-stop actions.
    void stopDeviceScriptsForIndexes(const QVector<int>& deviceIndexes); // wjy: 单选和多选菜单共用批量停止入口，并只汇总提示一次。
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
    void batchExecuteDeviceScriptFolder(const QVector<int>& deviceIndexes, const QString& scriptEntryPath, const QString& noExecutableMessage); // wjy: 多选设备和分组脚本共用统一批量入口，先校验用户点选的入口再逐台启动。
    void batchUpdateDevices(const QVector<int>& deviceIndexes); // wjy: 多选设备或分组菜单统一复用单设备更新请求逻辑。
    // ===end====
    void batchOpenDeviceTerminals(const QVector<int>& deviceIndexes);
    // =====wjy====
    platform::DeviceRealtimeStateService* m_realtimeStateService = nullptr; // wjy: 由 main 持有且晚于 MainWindow 销毁，DeviceGrid 只连接和调用，不负责释放。
    RemoteInputBroadcastCoordinator m_remoteInputBroadcastCoordinator; // wjy: 单一协调器覆盖普通和平铺窗口，DeviceGrid 析构前先由窗口注销并完成同步输入释放。
    std::unique_ptr<RemoteViewerLifecycleManager> m_remoteViewerLifecycleManager; // wjy: 生命周期晚于窗口批量stop，析构时再次兜底join固定工作线程。
    RemoteQualityCoordinator m_remoteQualityCoordinator; // wjy: 一个控制端统一计算手选、监控、全屏、默认和遮挡精确档位，不再按焦点分配画质。
    QTimer* m_remoteQualityTimer = nullptr; // wjy: 1秒采样接收FPS/码率，最小化和用户切换模式另走即时重算信号。
    // =====wjy====
    platform::LocalNetworkBandwidthMonitor m_titlebarBandwidthMonitor; // wjy: 主窗口内部只读采样物理网卡，不依赖或启动外部监控程序。
    platform::LocalNetworkBandwidthSample m_titlebarBandwidthSample; // wjy: UI 线程保存最新完整样本，paintEvent 只格式化文字而不查询系统 API。
    QTimer* m_titlebarBandwidthTimer = nullptr; // wjy: 每秒更新一次版本号右侧的当前接收 Mbps 与被控会话总数。
    QTimer* m_remoteMonitorTimer = nullptr; // wjy: 仅监控模式开启时运行，默认每30秒切换下一批远控窗口。
    QVector<RemoteMonitorSlot> m_remoteMonitorSlots; // wjy: 固定宫格槽位按位置持有专用监控窗口，设备页切换只调用switchSource。
    QSet<QString> m_pendingRemoteMonitorDeviceIds; // wjy: 当前页公钥授权期间按稳定设备ID去重，异步返回时再次校验设备仍在当前页。
    QString m_remoteMonitorScreenName; // wjy: 开启监控时锁定目标显示器名称，主窗口之后跨屏移动不会带动固定槽位迁移。
    bool m_remoteMonitorModeEnabled = false; // wjy: 程序启动默认关闭，只由标题栏按钮控制当前会话。
    int m_remoteMonitorPageIndex = 0; // wjy: 当前监控页按在线设备目标集合计算，设备上下线或宫格变化时自动夹紧。
    // ===end====
    bool m_remoteQualityEvaluationQueued = false; // wjy: 多窗口同时最小化或创建时合并为一个Qt任务，避免事件队列放大。
    qint64 m_lastRemoteResourceDiagnosticAtMs = 0; // wjy: 资源快照限制为30秒一次，避免稳定性诊断本身成为性能热点。
    QHash<RemoteDesktopWindow*, QString> m_realtimeControllerTargetSessionIds; // wjy: 每个普通/平铺窗口映射一个唯一目标租约，关闭时精确删除。
    QHash<QString, platform::RealtimeScriptState> m_deviceRealtimeScriptStates; // wjy: Unknown/Idle/Running 独立于可恢复脚本 UI 元数据，离线只隐藏 Logo 不破坏恢复信息。
    QHash<QString, platform::RemoteInputScriptRuntimeInfo> m_deviceRealtimeInputScriptStates; // wjy: 缓存被控端本地键鼠脚本快照，新窗口无需依赖原发起窗口即可恢复标题栏状态。
    // ===end====
    QString m_currentDeviceName;
    ScriptUiStateStore m_scriptUiStateStore; // wjy: 每设备脚本状态由独立仓储唯一持有，DeviceGrid 只投影当前设备页面。
    // =====wjy====
    QSet<QString> m_pendingScriptLaunchKeys; // wjy: 按设备稳定 ID 和具体入口路径组合去重，预检查期间连续点击不会重复访问网盘或目标端口。
    QSet<QString> m_pendingScriptBatchValidationPaths; // wjy: 分组或多选脚本先后台验证一次共享入口，同一路径验证期间不重复创建整批任务。
    QHash<QString, ScriptLaunchPreflightResult> m_scriptLaunchPreflightResults; // wjy: 后台结果暂存到主线程后由原执行函数一次性消费，保持后续脚本状态逻辑不变。
    // ===end====
    QLineEdit* m_periodicDeviceDiscoveryIntervalEdit = nullptr; // wjy: 周期检查新增设备的秒数输入框，默认 60 秒。
    QLineEdit* m_batchSubnetEdit = nullptr; // wjy: 批量新增网段输入框，支持以空格分隔多个 IPv4 通配网段。
    QPushButton* m_batchAddButton = nullptr; // wjy: 批量新增按钮，扫描期间禁用避免重复启动。
    QLineEdit* m_wallpaperRotationIntervalEdit = nullptr; // wjy: 自动壁纸轮换分钟输入框，仅在开关开启且卡片可见时显示。
    // =====wjy====
    QComboBox* m_rollbackVersionCombo = nullptr; // wjy: 常规页只展示低于当前安装版本且关键载荷完整的共享历史版本。
    // ===end====
    QVector<QLineEdit*> m_shortcutKeyEdits; // wjy: 键盘设置页统一承载主窗口、远控窗口和删除设备快捷键输入框。
    // =====wjy====
    stream::RemoteQualityConfiguration m_remoteQualityConfiguration; // wjy: 主窗口缓存当前持久化全局画质，所有跟随全局窗口读取同一份值。
    QComboBox* m_remoteQualityModeCombo = nullptr; // wjy: 远控画质设置只保留默认模式，下方固定预设由手绘说明展示。
    stream::RemoteMonitorConfiguration m_remoteMonitorConfiguration; // wjy: 缓存监控宫格、统一画质和轮询周期，设置变更立即作用于运行中监控。
    QComboBox* m_remoteMonitorGridCombo = nullptr; // wjy: 设置页提供4/9/12/16/20/25六种单页容量。
    QComboBox* m_remoteMonitorQualityCombo = nullptr; // wjy: 设置页选择独立监控窗口的统一画质，不读取普通远控按设备保存的手选档。
    QLineEdit* m_remoteMonitorIntervalEdit = nullptr; // wjy: 轮询秒数默认30，失焦或回车后持久化并重启计时。
    QTreeWidget* m_remoteMonitorFilterTree = nullptr; // wjy: 设置页按设备逐行展示黑白名单互斥复选框，避免手工输入错误名称。
    QPushButton* m_remoteMonitorApplyButton = nullptr; // wjy: 用户点击应用后才让勾选草稿写入设置并影响运行中的监控。
    QStringList m_remoteMonitorBlacklist; // wjy: 当前运行中的监控黑名单快照，黑名单优先于白名单。
    QStringList m_remoteMonitorWhitelist; // wjy: 当前运行中的监控白名单快照，白名单优先绕过Busy和英文名条件。
    bool m_remoteMonitorFilterDraftDirty = false; // wjy: 区分尚未应用的界面勾选和已经生效的运行时名单。
    // ===end====
    QSet<int> m_registeredGlobalShortcutIds;
    quintptr m_globalShortcutWindowHandle = 0;
    QSet<QString> m_pendingScreenshotTargets; // wjy: 记录当前远端截图请求；集合非空期间统一暂停新截图，保证确认窗口串行显示。
    bool m_screenshotReviewActive = false; // wjy: 截图确认窗口存在时暂停新截图，避免多个异步结果形成嵌套模态窗口。
    QLineEdit* m_deviceIpEdit = nullptr;
    QLineEdit* m_deviceNameEdit = nullptr;
    QLineEdit* m_deviceMacEdit = nullptr;
    QLineEdit* m_deviceRemarkEdit = nullptr;
    QLineEdit* m_deviceGroupNameEdit = nullptr; // wjy: Group rename editor.
    QLineEdit* m_deviceListNameEdit = nullptr; // wjy: Device rename editor shown directly on the left list row.
    DeviceSearchPanel* m_deviceSearchPanel = nullptr; // wjy: 主界面右侧查找页的唯一控件实例，生命周期由 DeviceGrid 父子关系管理。
    platform::LocalSystemInfo m_localSystemInfo; // wjy: 本机详情页只保存 CPU、GPU、内存和磁盘硬件快照，不再包含身份或网络字段。
    platform::CpuUsageSampler m_localCpuUsageSampler; // wjy: 保存相邻 GetSystemTimes 样本，只在本机页可见期间推进基线。
    platform::GpuUsageSampler m_localGpuUsageSampler; // wjy: 持有 Windows GPU Engine 性能计数器查询，只在本机页可见期间采样。
    platform::MemoryUsageSampler m_localMemoryUsageSampler; // wjy: 每秒只读当前物理内存负载，不保存跨页面基线。
    platform::LocalMemoryUsage m_localMemoryUsage; // wjy: 保存最新同一时刻的已用、总量和可用容量，页面重绘不再次调用系统 API。
    QTimer* m_localSystemInfoTimer = nullptr; // wjy: 本机页 CPU、GPU、内存占用共用一秒刷新定时器，离开页面后停止。
    int m_localDiskRefreshTick = 0; // wjy: 复用一秒定时器累计十次后刷新磁盘容量，避免新增常驻定时器或每秒枚举盘符。
    QVariantAnimation* m_localCpuUsageAnimation = nullptr; // wjy: CPU 圆环在相邻样本间平滑扫动。
    QVariantAnimation* m_localGpuUsageAnimation = nullptr; // wjy: GPU 圆环独立缓动，不与 CPU 新样本互相抢占动画状态。
    QVariantAnimation* m_localMemoryUsageAnimation = nullptr; // wjy: 内存圆环独立缓动，并与 CPU/GPU 使用相同速度曲线。
    qreal m_localCpuUsagePercent = -1.0; // wjy: CPU 动画中的小数显示值；负数表示正在建立采样基线。
    qreal m_localGpuUsagePercent = -1.0; // wjy: GPU 动画显示值；性能计数器不可用时保持负数并显示占位。
    qreal m_localMemoryUsagePercent = -1.0; // wjy: 内存动画显示值；有效结果始终限制在 0-100。
    QTextEdit* m_scriptFileEdit = nullptr;
    QTextEdit* m_scriptOutputEdit = nullptr; // wjy: 脚本日志正文使用只读文本控件，支持鼠标选择、Ctrl+C和右键复制。
    QPushButton* m_scriptFileSaveButton = nullptr;
    // =====wjy====
    QTreeWidget* m_scriptFolderTree = nullptr; // wjy: 只显示后台生成的脚本目录快照，UI 线程不直接访问共享路径。
    bool m_scriptFolderShareProbePending = false; // wjy: 等待 TCP 445 探测时合并重复刷新请求。
    bool m_scriptFolderLoadInProgress = false; // wjy: 后台递归枚举未完成时禁止启动第二个共享目录线程。
    bool m_scriptFolderTreeLoaded = false; // wjy: 成功快照可供脚本页和右键菜单共同复用。
    quint64 m_scriptFolderLoadRequestId = 0; // wjy: 过滤退出或后续刷新产生的过期目录结果。
    // ===end====
    QPushButton* m_saveDeviceButton = nullptr;
    QPushButton* m_cancelDeviceButton = nullptr;
    QVector<QPushButton*> m_localInfoCopyButtons;
    // =====wjy====
    QVariantAnimation* m_screenEdgeAutoHideAnimation = nullptr; // wjy: 对完整窗口坐标做插值，顶部改变 y、左右边缘改变 x，窗口尺寸始终不变。
    QTimer* m_screenEdgeAutoHideMonitorTimer = nullptr; // wjy: 低频读取全局鼠标位置，使仅剩 4px 的任一边缘触发条仍可唤回窗口。
    QTimer* m_screenEdgeAutoHideDelayTimer = nullptr; // wjy: 鼠标离开后统一延迟收起，菜单、拖放和窗口操作期间保持展开。
    QRect m_screenEdgeDockScreenGeometry; // wjy: 保存停靠屏幕工作区，兼容副屏负坐标以及任务栏位于顶部或左右侧。
    ScreenEdgeDock m_screenEdgeDock = ScreenEdgeDock::None; // wjy: 单一枚举同时表达是否停靠和当前方向，避免多个布尔状态组合冲突。
    bool m_screenEdgeAutoHidden = false; // wjy: 记录位置动画目标状态，滑动中途鼠标反向进入时可从当前位置平滑回转。
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
    bool m_hideLocalDeviceEnabled = false; // wjy: 表示本机设备实体的全局隐藏状态；开启后所有同步客户端都从设备列表过滤本机。
    bool m_statusRefreshInProgress = false;
    bool m_wakeProbeInProgress = false;
    bool m_batchAddInProgress = false; // wjy: 标记批量新增扫描是否正在后台执行。
    bool m_wallpaperRotationEnabled = false; // wjy: 新安装默认关闭，避免未经用户选择就修改当前桌面。
    bool m_wallpaperRotationInProgress = false; // wjy: 网络目录访问和缓存写入期间阻止定时器启动重叠任务。
    bool m_wallpaperShareProbePending = false; // wjy: 壁纸正在等待共享连接测试时不创建工作线程，也不重复发起探测。
    bool m_wallpaperShareProbeUserInitiated = false; // wjy: 合并等待期间的手动开启请求，失败时只对用户主动操作提示一次。
    int m_periodicDeviceDiscoveryIntervalSeconds = 60; // wjy: 周期新增设备默认每 60 秒扫描一次。
    int m_wallpaperRotationIntervalMinutes = 1; // wjy: 自动壁纸默认每 1 分钟切换一次，设置页允许修改。
    QString m_lastWallpaperSourcePath; // wjy: 只记录最后成功应用的共享源文件，下一轮据此按稳定顺序继续。
    QString m_wallpaperRotationStatusText; // wjy: 在设置卡片展示正在切换、成功文件名或最近失败原因，定时失败不使用重复弹窗。
    // =====wjy====
    bool m_updateAvailable = false; // wjy: 后台检测到更高版本时显示标题栏更新按钮，用户点击前绝不开始安装。
    bool m_updatePreparing = false; // wjy: 防止用户连续点击重复创建多个更新任务和更新器进程。
    QString m_availableUpdateVersion; // wjy: 保存检测到的远端版本，供标题栏更新状态使用。
    bool m_rollbackPreparing = false; // wjy: 回撤载荷暂存期间禁用下拉框和按钮，防止同一目标重复启动多个事务任务。
    bool m_publishPreparing = false; // wjy: 发布包在后台复制和校验期间禁用发布入口，防止重复发布同一版本或并发清理共享目录。
    QSet<QString> m_pendingPowerActionIps; // wjy: 保存正在发送关机或重启命令的 IP，同一设备不能在弱网等待期间重复提交电源动作。
    QSet<QString> m_pendingUpdateRequestIps; // wjy: 保存正在请求远程更新的 IP，批量和单设备入口共享去重状态。
    QSet<QString> m_pendingTerminalOpenIps; // wjy: 终端预检查期间按 IP 去重，连续点击和批量菜单不会重复授权或打开多个黑窗。
    QSet<QString> m_authorizedRemoteControlIps; // wjy: 本进程已经成功登记公钥的目标无需每次开窗重复访问 49102，远控连接失败仍由会话层独立判断。
    QSet<QString> m_pendingRemoteControlAuthorizationIps; // wjy: 一个后台授权批次进行时阻止其它入口重复登记同一设备公钥。
    bool m_rollbackShareProbePending = false; // wjy: 打开设置时先等待异步 SMB 探测，失败直接显示不可用而不创建枚举线程。
    QString m_rollbackVersionBeforeProbe; // wjy: 连接测试期间暂存用户原选择，枚举成功后优先恢复同一历史版本。
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

    QSet<int> m_doubleClickRemoteDeviceIndexes; // wjy: 保存第一次按下前的可见选择快照，避免双击首个释放事件把多选收敛后只远控一台。

    // wjy: Anchor index for Shift range selection.
    int m_selectionAnchorDeviceIndex = -1;

    int m_previousDeviceIndex = 0;
    QString m_previousDeviceName;
    // =====wjy====
    std::mutex m_backgroundThreadsMutex; // wjy: 保护后台线程列表，启动任务、退出取消和析构搬移必须互斥。
    std::vector<BackgroundThreadEntry> m_backgroundThreads; // wjy: 已完成项在运行期渐进回收，剩余活动项退出时取消阻塞 I/O 后正常 join。
    // ===end====
    bool m_shuttingDown = false; // wjy: No new background tasks after destruction begins.
    QHash<QString, platform::DevicePresenceState> m_deviceStatuses;
    QHash<QString, bool> m_deviceUpdateAvailability; // wjy: 设备状态刷新线程统一维护目标是否需要更新，远控窗口只消费结果。
    QHash<QString, platform::DeviceRealtimeUpdateState> m_deviceRealtimeUpdateStates;
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
