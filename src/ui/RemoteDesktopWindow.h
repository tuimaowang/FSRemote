#pragma once

#include <QByteArray>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QVector>
#include <QWidget>
#include <QElapsedTimer>
#include <QImage>
#include <QHash>
#include <QMutex>
#include <QSize>

#include <atomic>
#include <memory>

#include "FsRemoteStreamApi.h"
#include "stream/RemoteQualityPolicy.h"
#include "ui/LatestTextureFrameSlot.h" // wjy: 远控纹理跨线程交接改为可单测的单槽最新帧模型，禁止每帧都堆积Qt任务。
#include "ui/RemoteQualityCoordinator.h"
#include "ui/RemoteInputBroadcastCoordinator.h"
#include "ui/RemoteInputScript.h" // wjy: 远控窗口直接保存和回放统一语义键鼠事件，文件格式与既有远程PowerShell脚本面板完全隔离。
#include "ui/RemoteWindowCompositor.h" // wjy: 统一记录输出几何、缩放状态和回退状态，旧可见路径仍由开关控制。

class QEvent;
class QCloseEvent;
class QFocusEvent;
class QKeyEvent;
class QKeySequence;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QRubberBand;
class QTimer;
class QWheelEvent;

namespace ui {

struct RemoteTitleBarLayoutSnapshot;
struct RemoteTitleBarVisualState;

class D3D11FramePresenter;
class NativeRemoteTitleBarSurface;
class RemoteViewerLifecycleManager; // wjy: DeviceGrid持有的共享管理器统一限制Viewer初始化并发并等待全部stop任务。
struct RemoteDesktopViewerCallbackContext; // wjy: 每个原生viewer拥有独立回调上下文和代际，旧会话停止完成前上下文仍保持有效。

class RemoteDesktopWindow final : public QWidget, public RemoteInputEndpoint {
    Q_OBJECT

public:
    explicit RemoteDesktopWindow(
        const QString& deviceName,
        const QString& hostIp,
        RemoteViewerLifecycleManager* lifecycleManager,
        RemoteInputBroadcastCoordinator* inputBroadcastCoordinator,
        QWidget* parent = nullptr); // wjy: 每个窗口共享同一生命周期管理器，不再自行创建无法等待的后台线程。
    ~RemoteDesktopWindow() override;
    void enqueueRemoteFrame(QImage image, quint64 viewerGeneration); // wjy: BGRA回调携带viewer代际，重连后迟到旧帧会在写入pending前被拒绝。
    int enqueueRemoteTextureFrame(
        int width,
        int height,
        void* sharedHandle,
        quint64 frameId,
        qint64 rtpTimestamp,
        qint64 renderTimeMs,
        quint64 decodedAtUs,
        double encodedMbps,
        quint64 viewerGeneration); // wjy: 旧Presenter迁移路径保留三态结果，同时接收真实解码时间轴供后续诊断和RenderWorker使用。
    void setRemoteFrame(const QImage& image);
    void setConnectionStatus(int code, const QString& message);
    void setEncodedBitrateMbps(double mbps);
    void setRemoteMouseCaptureActive(bool active); // wjy: Host 的 absolute/relative 状态只更新自动请求，最终捕获由 Host 请求与 F2 手动锁定共同决定。
    // =====wjy====
    void setRemoteCursorShape(const QString& statusMessage); // wjy: 查看器状态回调在 Qt 主线程缓存远端 Windows 标准光标，并按当前命中区域安全刷新。
    void setRemoteMouseBackendStatus(const QString& statusMessage); // wjy: 只接受 Host 已确认的 system/faker 状态，不在按钮点击时乐观修改真实后端。
    // ===end====
    void setRememberGeometryEnabled(bool enabled);
    bool isClosingConnection() const;
    bool acceptsViewerGeneration(quint64 viewerGeneration) const; // wjy: 原生工作线程通过原子代际判断当前回调是否仍属于本窗口现用viewer。
    bool forwardNativeKey(int virtualKey, bool down);
    bool handleLocalShortcutKey(int virtualKey, Qt::KeyboardModifiers modifiers);
    void releaseForwardedShortcutKeys(const QVector<int>& virtualKeys);
    void shutdownForApplicationExit();
    bool isWaitingShortcutRelease() const;
    void updateShortcutReleaseGuard();
    // =====wjy====
    QSize remoteFrameSize() const; // wjy: 整组吸附重排时读取每个窗口自己的远端分辨率，保证批量缩放后仍无黑边。
    int titleBarHeight() const; // wjy: 整组布局计算需要扣除每个远控窗口的实际标题栏高度。
    // ===end====
    // =====wjy====
    QString hostIp() const; // wjy: 设备菜单更新成功后按固定 IP 找到所有对应远控窗口。
    QString deviceName() const; // wjy: 平铺排序读取窗口绑定的设备名，避免依赖窗口标题显示状态。
    void beginRemoteUpdateWait(); // wjy: 远控窗口进入更新遮罩、暂停输入并自动等待目标设备重启。
    bool isRemoteUpdateActive() const; // wjy: 供设备状态刷新识别“预期更新离线”，避免把正在等待重启的远控窗口当作普通断线关闭。
    void setRemoteUpdateAvailable(bool available); // wjy: 主界面统一探测目标版本后控制标题栏更新按钮，不让远控流线程参与版本检测。
    void setGlobalQualityConfiguration(const stream::RemoteQualityConfiguration& configuration); // wjy: 主窗口全局设置变化时立即更新跟随全局窗口的会话内策略快照。
    stream::RemoteQualityMode qualityOverrideMode() const; // wjy: 兼容读取历史设备模式；智能协调器不再把它作为活动 UI 的手动覆盖入口。
    RemoteQualityWindowMetrics remoteQualityMetrics(); // wjy: 协调器每秒读取只读原生统计并对累计值求差，不阻塞或修改 WebRTC 会话。
    void applyRemoteQualityDecision(const RemoteQualityDecision& decision); // wjy: 在线下发最新质量档位；相同请求去重，连接建立后自动补发，不停止或重建流。
    void refreshAppliedRemoteQualityStatus(); // wjy: 收到状态码63后从类型化ABI读取Host实际应用值，标题栏不会把请求值冒充结果。
    QString remoteResourceDiagnosticSummary(); // wjy: 低频汇总代际、单槽队列、FPS、码率、质量和D3D11原因，禁止逐帧调用。
    // ===end====
    // =====wjy====
    void setClipboardSyncEnabled(bool enabled);
    bool isClipboardSyncEnabled() const;
    void pushLocalClipboardIfNeeded();
    void applyRemoteClipboardPayload(const QString& encodedBase64);
    // ===end====

signals:
    void activated(RemoteDesktopWindow* window);
    void shortcutFullscreenRequested();
    void shortcutTileRequested();
    void shortcutCloseTopmostRequested();
    void shortcutCloseAllRequested();
    // =====wjy====
    void shortcutClipboardSyncRequested();
    void titleBarContextMenuRequested(const QString& hostIp, const QPoint& globalPosition); // wjy: 标题栏右键只上报本窗口绑定的目标 IP 和屏幕坐标，由设备主界面复用统一菜单逻辑。
    void titleBarUpdateRequested(const QString& hostIp); // wjy: 标题栏更新按钮只上报固定目标 IP，由 DeviceGrid 复用设备右键菜单的更新入口。
    void remoteQualityInputsChanged(); // wjy: 最小化、恢复、显示状态或单窗口模式变化时通知全局协调器立即重算，不等待下一秒轮询。
    // ===end====

protected:
    bool event(QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override; // wjy: Windows 相对捕获优先接收 WM_INPUT 原始鼠标位移；未注册时继续由既有 Qt 事件路径处理。
    void paintEvent(QPaintEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override; // wjy: 最大化按钮删除后，标题栏非按钮区域双击成为最大化/还原的唯一标题栏入口。
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    friend class RemoteViewerLifecycleManager; // wjy: 管理器获得初始化名额后只调用受控入口，普通业务代码不能绕过并发限制。

    // =====wjy====
    enum class RemoteUpdateState {
        None,
        Preparing,
        Installing,
        Reconnecting,
        Failed,
    }; // wjy: 把更新过程与普通连接状态分开，避免目标重启被误显示为普通断线。

    enum class RemoteMouseBackend {
        System,
        Faker,
    }; // wjy: 仅表示注入来源；与远端游戏自动进入的相对鼠标捕获状态完全独立。
    // ===end====
    void startViewerConnection();
    void startViewerConnectionWithAdmission(); // wjy: 仅在共享管理器授予名额后真正调用原生startViewer。
    void releaseViewerStartupAdmission(); // wjy: 连接成功、失败或stop完成时幂等释放初始化名额。
    // =====wjy====
    void beginNetworkRecoveryGracePeriod(); // wjy: ICE短时断开先保留旧会话三秒，等待WebRTC自行恢复。
    void beginNetworkReconnect(); // wjy: 确认会话失效后停止旧Viewer，并在stop完成后进入无限退避重试。
    void scheduleNetworkReconnect(); // wjy: 以1/2/4/8/10秒退避安排下一次连接，达到10秒后不限制重试次数。
    void attemptNetworkReconnect(); // wjy: 单次重试仍通过共享初始化管理器，不能绕过多窗口并发上限。
    void finishNetworkReconnect(); // wjy: 只有成功呈现新帧才恢复输入、清除警告并重置退避。
    void cancelNetworkReconnect(); // wjy: 关闭窗口、程序退出或远程更新接管时终止所有后续重试。
    void startFirstFrameWatchdog(); // wjy: 每个Viewer代际最多等待15秒真实呈现首帧，超时后给出采集/编码提示并自动重连。
    void stopFirstFrameWatchdog(); // wjy: 首帧呈现、会话停止或终态到达时取消当前代际的超时任务。
    void handleFirstFrameTimeout(); // wjy: Qt主线程统一提交明确错误并进入安全stop与退避重建流程。
    // ===end====
    // =====wjy====
    void pollRemoteUpdateStatus();
    void stopViewerConnectionAsync(bool deleteAfterStop);
    void finishViewerStop(const QString& errorMessage); // wjy: 原生stop无论成功或异常都回到Qt线程统一释放初始化名额并决定删除/重连。
    void startViewerAfterUpdate();
    void finishRemoteUpdateWait();
    bool remoteUpdateActive() const;
    bool remoteUpdateAcceptsFrames() const;
    QString remoteUpdateTitle() const;
    QString remoteUpdateDetail() const;
    // ===end====
    // =====wjy====
    // wjy: 远控标题栏已删除“虚拟屏”标签和“+”入口，因此不再保留标签命中类型。
    // ===end====

    // =====wjy====
    QRect remoteUpdateButtonRect() const; // wjy: 更新按钮位于剪切板按钮左侧，对应用户标出的标题栏空白区域。
    QRect mouseInputModeRect() const; // wjy: 兼容保留既有函数名；该标题栏开关现在统一切换系统/驱动键鼠后端。
    QRect clipboardSyncRect() const;
    QRect audioToggleRect() const; // wjy: 标题栏独立音频开关，默认静音且不参与画质焦点判定。
    QRect inputSyncRect() const; // wjy: 键鼠同步按钮固定在剪切板按钮左侧，并作为标题栏本地点击区排除远端输入。
    // ===end====
    QRect minimizeRect() const;
    QRect closeRect() const;
    RemoteTitleBarLayoutSnapshot titleBarLayoutSnapshot() const; // wjy: 每次按当前窗口宽度和设备信息覆盖范围生成统一标题栏控件快照。
    QRect titleBarHoverRectAt(const QPoint& position) const; // wjy: 只返回真正具有悬停视觉的标题栏按钮，空白区移动不再触发整条标题栏重画。
    void updateTitleBarHover(const QPoint& position); // wjy: 仅当鼠标跨入或跨出按钮热区时刷新相关矩形，远控画面内移动不会持续重画标题栏。
    void requestTitleBarUpdate(const QRect& region = QRect()); // wjy: 移动或缩放期间延迟标题栏刷新，交互结束后统一提交一次最终状态。
    // =====wjy====
    bool isTitleBarBlankArea(const QPoint& position) const; // wjy: 统一判断标题栏可拖动/可双击区域，排除右侧窗口控制按钮。
    void toggleMaximizedState(); // wjy: 删除最大化图标后仅由标题栏双击调用，统一切换最大化与普通状态。
    void toggleClipboardSync();
    void toggleViewerAudio(); // wjy: 只切换当前远控窗口本地音频播放器，不影响其它窗口。
    void toggleInputSynchronization(); // wjy: 标题栏一次点击根据关闭、主控、跟随三态执行开启、关闭或主控切换。
    void toggleRemoteMouseBackend(); // wjy: 兼容沿用现有协议入口，根据 Host 确认状态同时切换键盘和鼠标注入后端。
    void requestRemoteMouseBackend(RemoteMouseBackend backend);
    void queryRemoteMouseBackend(); // wjy: 每次新 Viewer 收到画面后查询 Host 全局状态，多控制窗口由真实值对齐。
    void toggleManualMouseLock(); // wjy: F2 只切换本窗口的手动相对鼠标意图，播放脚本期间也允许立即锁定或解除手动覆盖。
    void updateRemoteMouseCaptureState(); // wjy: 根据 Host 请求、F2 手动意图、连接资格和窗口焦点统一计算实际捕获状态。
    void applyRemoteMouseCaptureState(bool active); // wjy: 只负责实际 Raw Input、光标隐藏、回中心和远端释放，不改变两类请求来源。
    void suspendRemoteMouseCapture(); // wjy: 焦点离开、断线、更新或关闭时强制释放实际捕获，同时保留 F2 手动意图供重新获得资格后恢复。
    stream::RemoteQualityMode effectiveQualityMode() const; // wjy: FollowGlobal解析为最新全局模式，局部覆盖则直接返回覆盖值。
    void sendCurrentRemoteQualityDecision(); // wjy: 把内存中的最新决策转换为稳定C ABI结构；连接中只保留最新请求，重连后按新代际补发。
    void sendCurrentViewerAudioDecision(); // wjy: 按Viewer代际和布尔状态去重在线音频切换，连接前只保留协调器最新决策。
    QString remoteQualityStatusSummary() const; // wjy: 统一生成诊断中的请求/实际/降级说明，避免状态文案分叉。
    bool remoteQualityIsDegraded() const; // wjy: 判断当前是否处于最小化、自动降级、Host限制或高质量硬边界保护状态。
    // ===end====
    QRect remoteContentRect() const; // wjy: D3D11 Presenter覆盖标题栏下的完整内容区，真实远端矩形仍由remoteImageRect单独计算。
    QRect remoteImageRect() const;
    bool normalizedRemotePoint(const QPoint& position, int* x, int* y) const;
    bool sendInputMessage(const QByteArray& message);
    bool dispatchRemoteInputEvent(const RemoteInputEvent& event); // wjy: 来源窗口只发送一次，并由共享协调器决定是否向当前跟随窗口扇出。
    // =====wjy====
    void toggleInputScriptRecording(); // wjy: 按设置页当前录制快捷键在当前远控窗口切换录制状态，停止时再进入命名和原子保存流程。
    void startInputScriptRecording();
    void finishInputScriptRecording(bool limitReached = false);
    void cancelInputScriptRecording(const QString& reason); // wjy: 关闭、更新或失去控制权时直接丢弃未完成录制，生命周期路径不得弹出模态命名框。
    void recordRemoteInputEvent(const RemoteInputEvent& event);
    void toggleInputScriptPlayback(); // wjy: 按设置页当前播放快捷键在空闲时选择本地脚本，播放中再次按下则立即停止并释放远端持有输入。
    void chooseAndStartInputScriptPlayback();
    void scheduleNextInputScriptPlaybackEvent();
    void processInputScriptPlaybackEvents();
    void completeInputScriptPlaybackLoop(); // wjy: 每轮结束统一释放持有输入，再按循环次数决定自然结束或等待 F10 配置的轮间隔后重启。
    void stopInputScriptPlayback(const QString& reason, bool releaseRemoteInputs = true);
    void releaseInputScriptPlaybackInputs();
    void trackInputScriptPlaybackState(const RemoteInputEvent& event);
    void setInputScriptDialogActive(bool active); // wjy: 命名和文件选择期间暂停全局键盘转发，防止对话框文字误送到远端。
    // ===end====
    bool synchronizedInputEligible() const override;
    QSize synchronizedInputFrameSize() const override;
    bool sendSynchronizedInputEvent(const RemoteInputEvent& event) override;
    void synchronizedInputRoleChanged(RemoteInputSyncRole role) override;
    bool sendRemoteMouseMove(const QPoint& position, Qt::MouseButtons buttons);
    bool sendRemoteMouseRelativeMove(const QPoint& position, Qt::MouseButtons buttons);
    // =====wjy====
    bool sendRemoteRawMouseRelativeMove(int dx, int dy, Qt::MouseButtons buttons); // wjy: 将控制端实体鼠标的 Raw Input 计数送入现有相对移动协议，不删除旧的中心差值实现。
    bool setRawInputMouseCaptureEnabled(bool enabled); // wjy: 每个进程只把 Raw Input 鼠标注册给当前活动远控窗口，失败时返回 false 触发 Qt 回退。
    // ===end====
    void recenterRemoteMouseCapture();
    void setKeyboardForwardingActive(bool active);
    void beginShortcutReleaseGuard(const QKeySequence& shortcut);
    void releasePressedKeys();
    void releaseForwardedKeys();
    int remoteButton(Qt::MouseButton button) const;
    // =====wjy====
    // wjy: 标题栏不再存在虚拟屏标签点击区，因此删除对应的命中测试声明。
    // ===end====
    int resizeEdgesAt(const QPoint& position) const;
    void updateResizeCursor(const QPoint& position);
    // =====wjy====
    void setWindowAndPresenterCursor(Qt::CursorShape shape); // wjy: 软件绘制父窗口与 D3D 纹理子窗口必须使用同一光标，否则共享纹理区域会覆盖父窗口设置。
    void unsetWindowAndPresenterCursor();
    // ===end====
    void updateWindowMask();
    void updateTexturePresenterGeometry();
    RemoteWindowLayoutSnapshot compositorLayoutSnapshot() const; // wjy: 从当前顶层窗口一次性生成物理输出、内容和输入映射快照。
    void commitCompositorLayout(); // wjy: 统一把父窗口几何提交给新合成器，避免各表面各自推导尺寸。
    void presentCompositorOverlay(const QRect& dirtyLogicalRect = QRect()); // wjy: 空脏区完整合成；正常视频中的标题栏变化只更新缓存图层顶部区域。
    void beginResizeDebugTrace(); // wjy: 开始一次尺寸手势的内存追踪，不在高频事件期间写磁盘。
    void appendResizeDebugTrace(const QString& stage); // wjy: 记录父窗口、D3D子窗口和Present状态的同一时序节点。
    void flushResizeDebugTrace(); // wjy: 松手后一次性把本次追踪写入data/remote_resize_trace.log，避免日志改变拖拽时序。
    void sampleVisibleCompositorRegion(); // wjy: 可选地从屏幕实际可见像素采样黑色比例，区分DComp空帧与普通WM_SIZE事件。
    // =====wjy====
    RemoteTitleBarVisualState titleBarVisualState() const; // wjy: 把窗口可变成员压缩为一次渲染使用的不可变标题栏快照。
    void updateNativeTitleBarSurface(bool forceRender = false); // wjy: 仅在状态版本、宽度或DPI变化时生成完整新图并原子提交到持久DIB。
    void updateNativeTitleBarSurfaceGeometry(); // wjy: 同步标题栏子HWND的可见矩形和全屏显隐，不触碰缓存像素所有权。
    // ===end====
    void showSnapPreviews(const QHash<RemoteDesktopWindow*, QRect>& geometries); // wjy: 拖拽越界重排时同时显示整组窗口的最终虚影。
    void clearSnapPreviews(); // wjy: 拖离、释放、双击或关闭时统一清理整组吸附预览。
    bool startSystemWindowMove(); // wjy: 标题栏拖动交给Windows/DWM移动现有窗口表面，避免Qt逐像素move导致父窗和D3D子窗分帧合成。
    bool startSystemWindowResize(); // wjy: 边缘缩放优先交给Windows原生尺寸循环，使顶层窗口和原生子表面的合成节奏由DWM统一管理。
    void updateSnapPreviewForGeometry(const QRect& proposedGeometry, const QPoint& cursorGlobal); // wjy: 系统移动和手动回退共用同一吸附候选计算。
    void finishInteractiveWindowOperation(); // wjy: 系统与手动移动/缩放统一恢复圆角、吸附提交和几何保存。
    // =====wjy====
    bool restoreSavedGeometryForDrag(const QPoint& cursorGlobal, const QPoint& pressedPosition); // wjy: 平铺或最大化窗口开始拖动时恢复 JSON 普通尺寸，并保持鼠标原抓取位置。
    // ===end====
    void flushPendingRemoteFrame();
    void drainPendingRemoteTextureFrame(); // wjy: Qt线程一次消费一个已接受纹理；单槽忙时新帧由解码器受控丢弃并立即归还。
    void discardPendingTextureFrame(); // wjy: 取消单槽帧时完成keyed mutex消费者交接，避免解码纹理槽永久占用。
    void invalidateViewerCallbacks(); // wjy: 关闭或重连前统一递增代际并清空待呈现帧，让全部旧异步结果立即失效。
    void saveWindowGeometry();
    void updateFrameStats(const QImage& image); // wjy: Update the in-memory stream stats used by the title bar.
    void updatePresentedFrameStats(qint64 bgraBytes); // wjy: BGRA和共享纹理成功呈现统一计入真实接收FPS，零拷贝纹理不增加RAW带宽。
    void updateFrameColorStats(const QImage& image); // wjy: Update right-bottom RGB diagnostics for pure-black webpage tests.
    // =====wjy====
    // ===end====
    void sampleFrameColorRegion(const QImage& image, const QRect& region, int* minValue, int* avgValue, int* maxValue) const; // wjy: Sample RGB values cheaply without scanning every pixel.
    QString m_deviceName;
    QString m_hostIp;
    QString m_connectionStatus;
    int m_connectionStatusCode = 0;
    bool m_hasReceivedVideoInCurrentViewer = false; // wjy: 区分建立远控后的网络中断与首次连接失败，避免标题栏误报。
    bool m_networkWarningVisible = false; // wjy: 只保存静态“网络不佳”可见状态，不跟随每帧或计时器闪动。
    // =====wjy====
    QTimer* m_networkReconnectTimer = nullptr;
    QTimer* m_firstFrameWatchdogTimer = nullptr; // wjy: 与网络退避定时器分离，避免首次连接无画面时永远停在等待状态。
    bool m_networkReconnectActive = false; // wjy: 一旦正常远控因网络中断进入恢复流程，窗口保持到用户主动关闭并无限重试。
    bool m_networkRecoveryGraceActive = false; // wjy: true表示当前3秒计时器用于等待旧ICE会话自恢复，false表示用于退避重建。
    int m_networkReconnectAttempt = 0; // wjy: 保存0到5的退避档位，第五档后固定10秒且不停止累计恢复流程。
    bool m_hasPresentedVideoInCurrentViewer = false; // wjy: 只在D3D或BGRA真正进入可见路径后置位，不能被解码回调提前上报的状态50替代。
    // ===end====
    QImage m_remoteFrame;
    QSize m_remoteTextureSize;
    QMutex m_pendingFrameMutex; // wjy: Protects latest-frame handoff from the decoder thread to the Qt UI thread.
    QImage m_pendingRemoteFrame; // wjy: Holds only the newest decoded frame so slow full-screen painting cannot build a stale-frame queue.
    LatestTextureFrameSlot m_pendingTextureFrames; // wjy: 每个远控窗口最多保留一个纹理描述符和一个Qt drain任务，20路高帧率时队列仍有硬上限。
    RemoteViewerLifecycleManager* m_lifecycleManager = nullptr; // wjy: 由DeviceGrid持有且晚于全部远控窗口停止，窗口只借用它提交可等待任务。
    RemoteInputBroadcastCoordinator* m_inputBroadcastCoordinator = nullptr; // wjy: DeviceGrid 统一持有协调器，生命周期覆盖普通和平铺远控窗口。
    RemoteInputSyncRole m_inputSyncRole = RemoteInputSyncRole::Off;
    stream::RemoteQualityConfiguration m_globalQualityConfiguration; // wjy: 当前窗口持有最新全局策略，后续本地覆盖和协调器从这里解析有效值。
    stream::RemoteQualityMode m_qualityOverrideMode = stream::RemoteQualityMode::Automatic; // wjy: 首次远控设备默认“自动”，构造时若存在该设备历史记录则恢复上次选择。
    RemoteQualityDecision m_remoteQualityDecision; // wjy: 保存协调器针对当前窗口的最新目标，Viewer尚未创建时也不会丢失用户选择。
    bool m_hasRemoteQualityDecision = false;
    FsRemoteViewerQualityConfig m_lastSentQualityConfig = {}; // wjy: 记录最后一次成功交给DLL的配置，用于1秒协调循环去重，避免控制通道重复刷屏。
    bool m_hasLastSentQualityConfig = false;
    quint64 m_lastQualityViewerGeneration = 0; // wjy: 相同配置在新Viewer代际仍需补发，不能被旧连接的去重状态吞掉。
    bool m_lastSentViewerAudioEnabled = false; // wjy: 记录当前代际最后一次交给DLL的音频所有权状态，避免每秒重复start/stop。
    bool m_hasLastSentViewerAudioState = false;
    quint64 m_lastAudioViewerGeneration = 0; // wjy: 新Viewer即使仍是焦点也必须重新补发音频意图，旧句柄状态不能复用。
    quint64 m_nextQualityRequestId = 0; // wjy: 每个窗口单调递增请求号，迟到确认无法覆盖后续模式选择。
    FsRemoteViewerQualityStatus m_appliedQualityStatus = {}; // wjy: Host确认的实际宽高、FPS和码率快照，仅在匹配当前请求时展示。
    bool m_hasAppliedQualityStatus = false;
    bool m_qualityRequestPending = false;
    bool m_qualityProtocolUnavailable = false; // wjy: 旧DLL缺导出或旧Host三秒未确认时只显示不支持，现有流继续。
    RemotePerformanceSignalSampler m_performanceSignalSampler; // wjy: 每个窗口独立维护 WebRTC 累计统计基线，重连计数器回退不会污染其它会话。
    quint64 m_lastPresenterSampleFrames = 0;
    quint64 m_lastPresenterSampleDrops = 0;
    qint64 m_lastPresenterSampleMs = 0;
    FsRemoteStreamHandle m_viewerHandle = nullptr;
    std::shared_ptr<RemoteDesktopViewerCallbackContext> m_viewerCallbackContext; // wjy: stop返回前持续持有原生回调user指针，避免异步销毁期间访问已经释放的上下文。
    std::atomic<quint64> m_viewerGeneration = 0; // wjy: viewer每次创建、停止或关闭都推进代际，跨线程回调只读取这一原子值。
    std::atomic<quint64> m_staleTextureFrameDrops = 0; // wjy: 独立统计旧Viewer代际迟到纹理，诊断时不再与Presenter背压、网络丢包混合。
    bool m_closeInProgress = false;
    // =====wjy====
    RemoteUpdateState m_remoteUpdateState = RemoteUpdateState::None;
    QString m_remoteUpdateFailure;
    QTimer* m_remoteUpdateTimer = nullptr;
    QElapsedTimer m_remoteUpdateClock;
    qint64 m_nextRemoteUpdateProbeAtMs = 0;
    bool m_remoteUpdateProbeInProgress = false;
    bool m_remoteUpdateReconnectRequested = false;
    bool m_viewerStartQueued = false; // wjy: 标记窗口正在等待4路初始化名额，关闭时可从FIFO队列精确取消。
    bool m_viewerStartAdmissionActive = false; // wjy: 标记当前窗口占用初始化预算，成功、失败或停止后只释放一次。
    bool m_viewerStopInProgress = false;
    bool m_deleteAfterViewerStop = false;
    bool m_applicationExitInProgress = false; // wjy: 应用批量退出由DeviceGrid统一删除窗口，stop完成回调不得抢先deleteLater。
    int m_remoteUpdateSpinnerStep = 0; // wjy: 更新遮罩状态及异步停流标志都由远控窗口自身维护，窗口不会因目标重启而销毁。
    int m_remoteUpdateGeneration = 0; // wjy: 每次重新发起更新递增，丢弃上一轮迟到的后台查询结果。
    bool m_remoteUpdateAvailable = false; // wjy: 仅保存 DeviceGrid 最近一次状态刷新结果，不在远控连接回调中主动探测更新。
    // ===end====
    bool m_rememberGeometry = true;
    bool m_textureFrameActive = false;
    std::atomic_bool m_texturePresentFailed = false;
    QTimer* m_textureRecoveryTimer = nullptr; // wjy: D3D11失败后按退避间隔重新允许一帧纹理尝试，成功即可退出软件回退。
    int m_textureFailureCount = 0; // wjy: 连续失败次数只用于限制重试频率，不会停止或重建远控会话。
    bool m_softwareFallbackActive = false; // wjy: 重试窗口开放期间仍保持540p/24档，只有纹理真正成功呈现才解除回退上限。
    // =====wjy====
    // wjy: 删除仅服务于标题栏虚拟屏标签的编号、列表和当前标签状态。
    // ===end====
    QPoint m_hoveredPos = QPoint(-1, -1);
    bool m_draggingWindow = false;
    // =====wjy====
    bool m_dragRestorePending = false; // wjy: 平铺或最大化标题栏按下后先等待真实移动，单击和双击不会提前恢复窗口尺寸。
    bool m_systemWindowOperationActive = false; // wjy: Windows正在接管移动/缩放时禁止Qt鼠标路径再次setGeometry。
    bool m_systemWindowOperationAttempted = false; // wjy: 系统接口失败后本次手势固定走手动回退，不在每个鼠标事件重复调用。
    bool m_windowPaintingSuspendedForMove = false; // wjy: 标题栏拖动期间冻结父QWidget backing store，原生D3D子窗口仍独立Present。
    QPoint m_dragPressGlobal;
    QPoint m_dragPressPosition; // wjy: 保存按下时的屏幕坐标和窗口内抓取点，超过系统拖拽阈值后用于恢复 JSON 尺寸。
    // ===end====
    bool m_resizingWindow = false;
    int m_resizeEdges = 0;
    QPoint m_dragOffset;
    // =====wjy====
    QVector<QRubberBand*> m_snapPreviews; // wjy: 一个吸附方案可能同时调整多个窗口，因此为整组目标矩形分别显示虚影。
    QHash<RemoteDesktopWindow*, QRect> m_pendingSnapGeometries; // wjy: 保存整组待提交几何，只有松开左键时才批量应用。
    // ===end====
    QPoint m_resizeStartGlobal;
    QRect m_resizeStartGeometry;
    QElapsedTimer m_resizeDebugClock; // wjy: 记录缩放手势内的单调毫秒时间，不依赖系统时间回拨。
    quint64 m_resizeDebugTraceId = 0; // wjy: 每次边缘缩放递增，便于把多次手势从同一个诊断文件中分开。
    QVector<QString> m_resizeDebugTrace; // wjy: 只保留当前手势前256条事件，避免诊断本身制造无界内存或磁盘压力。
    bool m_resizePixelProbeEnabled = false; // wjy: 仅通过FSREMOTE_RESIZE_PIXEL_PROBE=1开启实际屏幕采样，正常运行不做截图。
    qint64 m_lastResizePixelProbeMs = -1000;
    quint64 m_resizeVisibleSampleCount = 0;
    // =====wjy====
    bool m_compositorPixelProbeEnabled = false; // wjy: 通过FSREMOTE_COMPOSITOR_PIXEL_PROBE=1开启普通运行期可见像素采样，默认关闭以保持零截图开销。
    qint64 m_lastCompositorPixelProbeMs = -1000; // wjy: 普通运行期采样限制为每窗口最多10次/秒。
    quint64 m_compositorVisibleSampleCount = 0; // wjy: 给普通运行期样本编号，便于按设备检索连续黑帧。
    qint64 m_lastCompositorFrameTimelineMs = -1000; // wjy: 视频Present成功时每窗口最多每秒记录一次，失败不受限频影响。
    qint64 m_lastCompositorPartialTimelineMs = -1000; // wjy: 标题栏脏区提交每窗口最多每500毫秒记录一次，整窗提交全部记录。
    // ===end====
    QElapsedTimer m_sessionClock;
    QElapsedTimer m_frameStatsClock; // wjy: Measures one-second windows for the remote desktop title-bar statistics.
    int m_frameStatsCount = 0; // wjy: 统计当前窗口真正成功进入显示路径的BGRA或共享纹理帧数。
    quint64 m_totalPresentedFrames = 0; // wjy: 单调累计成功Present数，与单槽旧pending替换计数求差得到本地呈现压力而非网络结果。
    qint64 m_frameStatsBytes = 0; // wjy: Accumulates decoded BGRA bytes for raw-throughput diagnostics.
    double m_receiveFps = 0.0; // wjy: Last calculated UI-side received FPS shown in the title bar.
    double m_rawBgraMbps = 0.0; // wjy: Last calculated decoded BGRA throughput, separate from compressed network bitrate.
    double m_encodedMbps = 0.0; // wjy: Last calculated compressed video bitrate reported by the decoder from encoded frame bytes.
    // =====wjy====
    // ===end====
    int m_rgbMin = 0; // wjy: Whole-frame minimum sampled RGB value.
    int m_rgbAvg = 0; // wjy: Whole-frame average sampled RGB value.
    int m_rgbMax = 0; // wjy: Whole-frame maximum sampled RGB value.
    int m_centerRgbMin = 0; // wjy: Center-region minimum sampled RGB value for black-page testing.
    int m_centerRgbAvg = 0; // wjy: Center-region average sampled RGB value for black-page testing.
    int m_centerRgbMax = 0; // wjy: Center-region maximum sampled RGB value for black-page testing.
    // =====wjy====
    bool m_remoteMouseCaptureRequested = false; // wjy: 保存 Host 游戏检测产生的 relative 请求；焦点离开只暂停实际捕获，不丢失该请求。
    bool m_manualMouseLockActive = false; // wjy: 保存 F2 手动锁定意图，断线或窗口失焦期间仍保留，恢复连接和焦点后自动重新生效。
    bool m_remoteMouseCaptureActive = false; // wjy: 仅表示当前窗口正在实际隐藏光标并接收相对鼠标输入，不再等同于 Host 请求。
    bool m_rawInputMouseCaptureActive = false; // wjy: 仅表示控制端 WM_INPUT 已成功注册；目标端仍独立选择系统或 FakerInput 注入后端。
    Qt::CursorShape m_remoteCursorShape = Qt::ArrowCursor; // wjy: 缓存最近一次远端桌面光标，退出相对鼠标模式后无需等待下一条状态即可恢复。
    RemoteMouseBackend m_remoteMouseBackend = RemoteMouseBackend::System;
    RemoteMouseBackend m_pendingRemoteMouseBackend = RemoteMouseBackend::System;
    bool m_remoteMouseBackendKnown = false; // wjy: 新连接未收到 Host 回应前显示安全默认值，按钮状态会标记等待确认。
    bool m_remoteMouseBackendPending = false;
    bool m_remoteMouseBackendFallback = false;
    bool m_mouseBackendButtonPressed = false;
    quint64 m_remoteMouseBackendRequestGeneration = 0; // wjy: 两秒超时按请求代际判定，迟到定时器不能覆盖后续成功确认。
    QString m_remoteMouseBackendMessage;
    // ===end====
    QTimer* m_framePresentTimer = nullptr; // wjy: Presents the newest pending frame at a fixed UI pace instead of posting one UI task per decoded frame.
    QTimer* m_sessionTimer = nullptr;
    bool m_waitingShortcutRelease = false;
    QVector<int> m_shortcutReleaseVirtualKeys;
    QSet<int> m_localShortcutReleaseKeys; // wjy: Qt 兜底路径记录已消费的自定义 F2/F9/F10 主键，确保组合键松开顺序变化时也不漏给远端。
    QSet<int> m_pressedKeys;
    // =====wjy====
    bool m_inputScriptRecording = false;
    bool m_inputScriptRecordingStopQueued = false; // wjy: 事件或时长达到安全上限时只投递一次停止任务，避免高频鼠标事件重复打开命名框。
    QElapsedTimer m_inputScriptRecordingClock;
    QSize m_inputScriptRecordingFrameSize;
    QVector<RemoteInputScriptEvent> m_recordedInputScriptEvents;
    bool m_inputScriptPlaying = false;
    bool m_dispatchingInputScriptPlayback = false; // wjy: 区分定时器产生的脚本事件和用户实时输入，播放期间只允许前者进入远端发送路径。
    bool m_inputScriptDialogActive = false;
    QTimer* m_inputScriptPlaybackTimer = nullptr;
    QElapsedTimer m_inputScriptPlaybackClock;
    QString m_inputScriptPlaybackFilePath; // wjy: 同一远控窗口再次按F10时默认带回上次选择，仍可在设置弹窗内重新选择。
    int m_inputScriptPlaybackLoopCount = 1;
    int m_inputScriptPlaybackCompletedLoopCount = 0;
    int m_inputScriptPlaybackLoopIntervalMs = 0; // wjy: 保存 F10 设置的两轮完整脚本之间等待时间，0 表示连续执行。
    bool m_inputScriptPlaybackWaitingForLoopInterval = false; // wjy: 复用可取消的播放定时器等待下一轮，F10 停止时无需处理遗留 singleShot 回调。
    double m_inputScriptPlaybackSpeedMultiplier = 1.0;
    bool m_inputScriptPasteRandomSuffixEnabled = false; // wjy: F10 播放时仅对脚本内 Ctrl+V 追加随机后缀。
    QString m_inputScriptPasteRandomSeparator = QStringLiteral("......");
    int m_inputScriptPasteRandomLength = 3;
    int m_inputScriptPasteRandomMode = 0;
    bool m_inputScriptPlaybackCtrlDown = false;
    QVector<RemoteInputScriptEvent> m_inputScriptPlaybackEvents;
    qsizetype m_inputScriptPlaybackIndex = 0;
    QSet<int> m_inputScriptPlaybackHeldKeys;
    QSet<int> m_inputScriptPlaybackHeldButtons;
    int m_inputScriptPlaybackX = 32768;
    int m_inputScriptPlaybackY = 32768;
    // ===end====
    bool m_inputSyncButtonPressed = false; // wjy: 仅按下并在同一按钮内释放才切换主控，拖出热区不会误操作同步状态。
    // =====wjy====
    bool m_clipboardSyncEnabled = true; // wjy: 标题栏按钮与 Ctrl+B 共同切换，默认开启。
    bool m_viewerAudioEnabled = false; // wjy: 每个远控窗口默认静音，状态不与焦点或画质绑定。
    QString m_lastLocalClipboardText;
    QString m_lastAppliedRemoteClipboardText;
    QTimer* m_clipboardPollTimer = nullptr;
    // ===end====
    D3D11FramePresenter* m_texturePresenter = nullptr;
    std::unique_ptr<RemoteWindowCompositor> m_unifiedCompositor; // wjy: 新路径默认开启，现场可通过环境开关回退；对象状态在窗口构造时固定。
    std::unique_ptr<NativeRemoteTitleBarSurface> m_nativeTitleBarSurface; // wjy: 独立Win32子窗口持有标题栏DIB，父Qt backing store不再负责可见标题栏像素。
    QImage m_compositorOverlayCache; // wjy: 保留完整透明Overlay像素；秒级标题栏刷新只改顶部脏区，不再分配和上传整窗图像。
    quint64 m_titleBarVisualRevision = 1; // wjy: 只有标题栏可见状态变化才递增，远控视频帧不参与该版本。
    quint64 m_committedTitleBarVisualRevision = 0;
    QSize m_committedTitleBarLogicalSize; // wjy: 现在保存身份布局签名而不是窗口尺寸，普通缩放不会命中重绘条件。
    int m_committedTitleBarBarHeight = 0;
    qreal m_committedTitleBarDevicePixelRatio = 0.0;
    int nativeTitleBarBandLogicalWidth() const; // wjy: 身份层的固定渲染宽度，按虚拟屏逻辑宽度预留。
    // =====wjy====
    quint64 m_committedButtonVisualRevision = 0;
    int m_committedButtonGroupWidth = 0;
    quint32 m_committedButtonVisibleSignature = 0;
    qreal m_committedButtonDevicePixelRatio = 0.0;
    int m_committedButtonBarHeight = 0; // wjy: 按钮段只在视觉版本、可见集合、组宽度、DPI或栏高变化时重绘，窗口宽度变化本身不触发。
    void updateNativeTitleBarButtonBand(bool forceRender); // wjy: 渲染并提交按钮段位图，内部按上述维度去重。
    void updateNativeTitleBarButtonOrigin(); // wjy: 在原生合成缓冲内同步按钮段位置；交互缩放期间不切换标题栏表面所有者。
    // ===end====
};

} // namespace ui
