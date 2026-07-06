#pragma once

#include <QPoint>
#include <QRect>
#include <QSet>
#include <QVector>
#include <QWidget>
#include <QElapsedTimer>
#include <QImage>
#include <QMutex>

#include "FsRemoteStreamApi.h"

class QEvent;
class QCloseEvent;
class QFocusEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QTimer;
class QWheelEvent;

namespace ui {

class RemoteDesktopWindow final : public QWidget {
    Q_OBJECT

public:
    explicit RemoteDesktopWindow(const QString& deviceName, const QString& hostIp, QWidget* parent = nullptr);
    ~RemoteDesktopWindow() override;
    void enqueueRemoteFrame(QImage image);
    void setRemoteFrame(const QImage& image);
    void setConnectionStatus(int code, const QString& message);
    void setEncodedBitrateMbps(double mbps);
    bool isClosingConnection() const;
    bool forwardNativeKey(int virtualKey, bool down);

protected:
    void paintEvent(QPaintEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
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
    enum class TabHitType {
        None,
        Tab,
        CloseTab,
        Add,
    };

    struct TabHit {
        TabHitType type = TabHitType::None;
        int index = -1;
    };

    QRect minimizeRect() const;
    QRect maximizeRect() const;
    QRect closeRect() const;
    QRect controlCenterRect() const;
    QRect remoteImageRect() const;
    bool normalizedRemotePoint(const QPoint& position, int* x, int* y) const;
    void sendInputMessage(const QByteArray& message);
    void setKeyboardForwardingActive(bool active);
    void releasePressedKeys();
    int remoteButton(Qt::MouseButton button) const;
    TabHit hitTestTabs(const QPoint& position) const;
    int resizeEdgesAt(const QPoint& position) const;
    void updateResizeCursor(const QPoint& position);
    void updateWindowMask();
    void flushPendingRemoteFrame();
    void updateFrameStats(const QImage& image); // wjy: Update the bottom-right stream stats overlay from each received remote frame.
    void updateFrameColorStats(const QImage& image); // wjy: Update right-bottom RGB diagnostics for pure-black webpage tests.
    void sampleFrameColorRegion(const QImage& image, const QRect& region, int* minValue, int* avgValue, int* maxValue) const; // wjy: Sample RGB values cheaply without scanning every pixel.
    QString m_deviceName;
    QString m_hostIp;
    QString m_connectionStatus;
    int m_connectionStatusCode = 0;
    QImage m_remoteFrame;
    QMutex m_pendingFrameMutex; // wjy: Protects latest-frame handoff from the decoder thread to the Qt UI thread.
    QImage m_pendingRemoteFrame; // wjy: Holds only the newest decoded frame so slow full-screen painting cannot build a stale-frame queue.
    FsRemoteStreamHandle m_viewerHandle = nullptr;
    bool m_closeInProgress = false;
    QVector<int> m_virtualScreens;
    int m_nextVirtualScreenNumber = 1;
    int m_activeTabIndex = 0;
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
    QTimer* m_framePresentTimer = nullptr; // wjy: Presents the newest pending frame at a fixed UI pace instead of posting one UI task per decoded frame.
    QTimer* m_sessionTimer = nullptr;
    QSet<int> m_pressedKeys;
};

} // namespace ui
