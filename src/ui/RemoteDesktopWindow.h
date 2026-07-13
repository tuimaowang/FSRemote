#pragma once

#include <QPoint>
#include <QRect>
#include <QSet>
#include <QVector>
#include <QWidget>
#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QSize>

#include <atomic>

#include "FsRemoteStreamApi.h"

class QEvent;
class QCloseEvent;
class QFocusEvent;
class QKeyEvent;
class QKeySequence;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QTimer;
class QWheelEvent;

namespace ui {

class D3D11FramePresenter;

class RemoteDesktopWindow final : public QWidget {
    Q_OBJECT

public:
    explicit RemoteDesktopWindow(const QString& deviceName, const QString& hostIp, QWidget* parent = nullptr);
    ~RemoteDesktopWindow() override;
    void enqueueRemoteFrame(QImage image);
    bool enqueueRemoteTextureFrame(int width, int height, void* sharedHandle, quint64 frameId, double encodedMbps);
    void setRemoteFrame(const QImage& image);
    void setConnectionStatus(int code, const QString& message);
    void setEncodedBitrateMbps(double mbps);
    void setRemoteMouseCaptureActive(bool active);
    void setRememberGeometryEnabled(bool enabled);
    bool isClosingConnection() const;
    bool forwardNativeKey(int virtualKey, bool down);
    bool handleLocalShortcutKey(int virtualKey, Qt::KeyboardModifiers modifiers);
    void releaseForwardedShortcutKeys(const QVector<int>& virtualKeys);
    void shutdownForApplicationExit();
    bool isWaitingShortcutRelease() const;
    void updateShortcutReleaseGuard();
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
    // ===end====

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override; // wjy: 捕获标题栏非按钮区域双击，并复用右侧最大化按钮的窗口切换逻辑。
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
    void startViewerConnection();
    // =====wjy====
    // wjy: 远控标题栏已删除“虚拟屏”标签和“+”入口，因此不再保留标签命中类型。
    // ===end====

    // =====wjy====
    int titleBarHeight() const; // wjy: 远控标题栏高度与主窗口 28px 对齐。
    QRect clipboardSyncRect() const;
    // ===end====
    QRect minimizeRect() const;
    QRect maximizeRect() const;
    QRect closeRect() const;
    // =====wjy====
    bool isTitleBarBlankArea(const QPoint& position) const; // wjy: 统一判断标题栏可拖动/可双击区域，排除右侧窗口控制按钮。
    void toggleMaximizedState(); // wjy: 最大化图标点击和标题栏双击共用这一段窗口状态切换逻辑。
    void toggleClipboardSync();
    // ===end====
    QRect remoteImageRect() const;
    QSize remoteFrameSize() const;
    bool normalizedRemotePoint(const QPoint& position, int* x, int* y) const;
    void sendInputMessage(const QByteArray& message);
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
    void flushPendingRemoteFrame();
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
    FsRemoteStreamHandle m_viewerHandle = nullptr;
    bool m_closeInProgress = false;
    bool m_rememberGeometry = true;
    bool m_textureFrameActive = false;
    std::atomic_bool m_texturePresentFailed = false;
    // =====wjy====
    // wjy: 删除仅服务于标题栏虚拟屏标签的编号、列表和当前标签状态。
    // ===end====
    QPoint m_hoveredPos = QPoint(-1, -1);
    bool m_draggingWindow = false;
    bool m_resizingWindow = false;
    int m_resizeEdges = 0;
    QPoint m_dragOffset;
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
    // =====wjy====
    bool m_clipboardSyncEnabled = true; // wjy: 标题栏按钮与 Ctrl+B 共同切换，默认开启。
    QString m_lastLocalClipboardText;
    QString m_lastAppliedRemoteClipboardText;
    QTimer* m_clipboardPollTimer = nullptr;
    // ===end====
    D3D11FramePresenter* m_texturePresenter = nullptr;
};

} // namespace ui
