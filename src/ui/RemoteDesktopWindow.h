#pragma once

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

class D3D11FramePresenter;
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
    bool enqueueRemoteTextureFrame(int width, int height, void* sharedHandle, quint64 frameId, double encodedMbps, quint64 viewerGeneration); // wjy: 纹理帧同时校验代际和单槽调度，旧连接不能覆盖新连接画面。
    void setRemoteFrame(const QImage& image);
    void setConnectionStatus(int code, const QString& message);
    void setEncodedBitrateMbps(double mbps);
    void setRemoteMouseCaptureActive(bool active);
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
    void beginRemoteUpdateWait(); // wjy: 远控窗口进入更新遮罩、暂停输入并自动等待目标设备重启。
    bool isRemoteUpdateActive() const; // wjy: 供设备状态刷新识别“预期更新离线”，避免把正在等待重启的远控窗口当作普通断线关闭。
    void setRemoteUpdateAvailable(bool available); // wjy: 主界面统一探测目标版本后控制标题栏更新按钮，不让远控流线程参与版本检测。
    void setGlobalQualityConfiguration(const stream::RemoteQualityConfiguration& configuration); // wjy: 主窗口全局设置变化时立即更新跟随全局窗口的会话内策略快照。
    stream::RemoteQualityMode qualityOverrideMode() const; // wjy: 返回当前窗口模式；每次选择都会按设备持久化并在下次打开时恢复。
    RemoteQualityWindowMetrics remoteQualityMetrics() const; // wjy: 协调器每秒读取窗口可见性、源尺寸、显示尺寸、接收FPS和编码码率，不触碰原生Viewer线程。
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
    // ===end====
    void startViewerConnection();
    void startViewerConnectionWithAdmission(); // wjy: 仅在共享管理器授予名额后真正调用原生startViewer。
    void releaseViewerStartupAdmission(); // wjy: 连接成功、失败或stop完成时幂等释放初始化名额。
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
    QRect qualityButtonRect() const; // wjy: 单窗口画质按钮位于剪切板左侧，菜单设置只影响当前远控窗口。
    QRect clipboardSyncRect() const;
    QRect inputSyncRect() const; // wjy: 键鼠同步按钮固定在剪切板按钮左侧，并作为标题栏本地点击区排除远端输入。
    // ===end====
    QRect minimizeRect() const;
    QRect closeRect() const;
    // =====wjy====
    bool isTitleBarBlankArea(const QPoint& position) const; // wjy: 统一判断标题栏可拖动/可双击区域，排除右侧窗口控制按钮。
    void toggleMaximizedState(); // wjy: 删除最大化图标后仅由标题栏双击调用，统一切换最大化与普通状态。
    void toggleClipboardSync();
    void toggleInputSynchronization(); // wjy: 标题栏一次点击根据关闭、主控、跟随三态执行开启、关闭或主控切换。
    QString inputSynchronizationToolTip() const;
    void showQualityMenu(const QPoint& globalPosition); // wjy: 构建自定义/自动/高质量/均衡/流畅菜单，并保存当前窗口临时选择。
    void setQualityOverrideMode(stream::RemoteQualityMode mode); // wjy: 修改当前窗口会话内模式，不写AppSettings也不影响其它窗口。
    stream::RemoteQualityMode effectiveQualityMode() const; // wjy: FollowGlobal解析为最新全局模式，局部覆盖则直接返回覆盖值。
    void sendCurrentRemoteQualityDecision(); // wjy: 把内存中的最新决策转换为稳定C ABI结构；连接中只保留最新请求，重连后按新代际补发。
    QString remoteQualityStatusSummary() const; // wjy: 统一生成标题栏气泡和菜单中的请求/实际/降级说明，避免两处反馈不一致。
    bool remoteQualityIsDegraded() const; // wjy: 判断当前是否处于最小化、自动降级、Host限制或高质量硬边界保护状态。
    // ===end====
    QRect remoteImageRect() const;
    bool normalizedRemotePoint(const QPoint& position, int* x, int* y) const;
    bool sendInputMessage(const QByteArray& message);
    bool dispatchRemoteInputEvent(const RemoteInputEvent& event); // wjy: 来源窗口只发送一次，并由共享协调器决定是否向当前跟随窗口扇出。
    bool synchronizedInputEligible() const override;
    QSize synchronizedInputFrameSize() const override;
    bool sendSynchronizedInputEvent(const RemoteInputEvent& event) override;
    void synchronizedInputRoleChanged(RemoteInputSyncRole role) override;
    bool sendRemoteMouseMove(const QPoint& position, Qt::MouseButtons buttons);
    bool sendRemoteMouseRelativeMove(const QPoint& position, Qt::MouseButtons buttons);
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
    void updateWindowMask();
    void updateTexturePresenterGeometry();
    void showSnapPreviews(const QHash<RemoteDesktopWindow*, QRect>& geometries); // wjy: 拖拽越界重排时同时显示整组窗口的最终虚影。
    void clearSnapPreviews(); // wjy: 拖离、释放、双击或关闭时统一清理整组吸附预览。
    void flushPendingRemoteFrame();
    void drainPendingRemoteTextureFrame(); // wjy: Qt线程一次只取最新共享纹理，执行期间到达的旧帧会被后续新帧覆盖。
    void invalidateViewerCallbacks(); // wjy: 关闭或重连前统一递增代际并清空待呈现帧，让全部旧异步结果立即失效。
    void saveWindowGeometry();
    void updateFrameStats(const QImage& image); // wjy: Update the bottom-right stream stats overlay from each received remote frame.
    void updateFrameColorStats(const QImage& image); // wjy: Update right-bottom RGB diagnostics for pure-black webpage tests.
    void sampleFrameColorRegion(const QImage& image, const QRect& region, int* minValue, int* avgValue, int* maxValue) const; // wjy: Sample RGB values cheaply without scanning every pixel.
    QString m_deviceName;
    QString m_hostIp;
    QString m_connectionStatus;
    int m_connectionStatusCode = 0;
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
    quint64 m_nextQualityRequestId = 0; // wjy: 每个窗口单调递增请求号，迟到确认无法覆盖后续模式选择。
    FsRemoteViewerQualityStatus m_appliedQualityStatus = {}; // wjy: Host确认的实际宽高、FPS和码率快照，仅在匹配当前请求时展示。
    bool m_hasAppliedQualityStatus = false;
    bool m_qualityRequestPending = false;
    bool m_qualityProtocolUnavailable = false; // wjy: 旧DLL缺导出或旧Host三秒未确认时只显示不支持，现有流继续。
    FsRemoteStreamHandle m_viewerHandle = nullptr;
    std::shared_ptr<RemoteDesktopViewerCallbackContext> m_viewerCallbackContext; // wjy: stop返回前持续持有原生回调user指针，避免异步销毁期间访问已经释放的上下文。
    std::atomic<quint64> m_viewerGeneration = 0; // wjy: viewer每次创建、停止或关闭都推进代际，跨线程回调只读取这一原子值。
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
    bool m_resizingWindow = false;
    int m_resizeEdges = 0;
    QPoint m_dragOffset;
    // =====wjy====
    QVector<QRubberBand*> m_snapPreviews; // wjy: 一个吸附方案可能同时调整多个窗口，因此为整组目标矩形分别显示虚影。
    QHash<RemoteDesktopWindow*, QRect> m_pendingSnapGeometries; // wjy: 保存整组待提交几何，只有松开左键时才批量应用。
    // ===end====
    QPoint m_resizeStartGlobal;
    QRect m_resizeStartGeometry;
    QElapsedTimer m_sessionClock;
    QElapsedTimer m_frameStatsClock; // wjy: Measures one-second windows for the remote desktop stats overlay.
    int m_frameStatsCount = 0; // wjy: Counts frames received by the Qt UI handoff during the current stats window.
    qint64 m_frameStatsBytes = 0; // wjy: Accumulates decoded BGRA bytes for raw-throughput diagnostics.
    double m_receiveFps = 0.0; // wjy: Last calculated UI-side received FPS shown in the overlay.
    double m_rawBgraMbps = 0.0; // wjy: Last calculated decoded BGRA throughput, separate from compressed network bitrate.
    double m_encodedMbps = 0.0; // wjy: Last calculated compressed video bitrate reported by the decoder from encoded frame bytes.
    int m_rgbMin = 0; // wjy: Whole-frame minimum sampled RGB value.
    int m_rgbAvg = 0; // wjy: Whole-frame average sampled RGB value.
    int m_rgbMax = 0; // wjy: Whole-frame maximum sampled RGB value.
    int m_centerRgbMin = 0; // wjy: Center-region minimum sampled RGB value for black-page testing.
    int m_centerRgbAvg = 0; // wjy: Center-region average sampled RGB value for black-page testing.
    int m_centerRgbMax = 0; // wjy: Center-region maximum sampled RGB value for black-page testing.
    bool m_remoteMouseCaptureActive = false;
    QTimer* m_framePresentTimer = nullptr; // wjy: Presents the newest pending frame at a fixed UI pace instead of posting one UI task per decoded frame.
    QTimer* m_sessionTimer = nullptr;
    bool m_waitingShortcutRelease = false;
    QVector<int> m_shortcutReleaseVirtualKeys;
    QSet<int> m_pressedKeys;
    bool m_inputSyncButtonPressed = false; // wjy: 仅按下并在同一按钮内释放才切换主控，拖出热区不会误操作同步状态。
    // =====wjy====
    bool m_clipboardSyncEnabled = true; // wjy: 标题栏按钮与 Ctrl+B 共同切换，默认开启。
    QString m_lastLocalClipboardText;
    QString m_lastAppliedRemoteClipboardText;
    QTimer* m_clipboardPollTimer = nullptr;
    // ===end====
    D3D11FramePresenter* m_texturePresenter = nullptr;
};

} // namespace ui
