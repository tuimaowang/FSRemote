#pragma once

#include <QPoint>
#include <QRect>
#include <QSet>
#include <QVector>
#include <QWidget>
#include <QElapsedTimer>
#include <QImage>
#include <QPair>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

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
    void startInputWorker();
    void stopInputWorker();
    void queueRemoteMouseMove(int x, int y, int buttons);
    void flushRemoteMouseMove();
    void flushPendingRemoteFrame();
    void setKeyboardForwardingActive(bool active);
    void releasePressedKeys();
    int remoteButton(Qt::MouseButton button) const;
    TabHit hitTestTabs(const QPoint& position) const;
    int resizeEdgesAt(const QPoint& position) const;
    void updateResizeCursor(const QPoint& position);
    void updateWindowMask();
    QString m_deviceName;
    QString m_hostIp;
    QString m_connectionStatus;
    int m_connectionStatusCode = 0;
    QImage m_remoteFrame;
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
    QElapsedTimer m_mouseMoveInputClock;
    QTimer* m_sessionTimer = nullptr;
    QTimer* m_mouseMoveInputTimer = nullptr;
    QSet<int> m_pressedKeys;
    std::mutex m_pendingFrameMutex; // wjy: 保护远程帧合并缓存，原生回调线程和 UI 线程都会访问。
    QImage m_pendingRemoteFrame;
    bool m_remoteFrameUpdateQueued = false; // wjy: UI 线程已有待处理帧任务时，新帧只覆盖缓存，不继续堆积事件。
    QPoint m_pendingRemoteMousePos = QPoint(-1, -1);
    int m_pendingRemoteMouseButtons = 0;
    std::mutex m_inputQueueMutex; // wjy: 保护远程输入发送队列，让 UI 线程只入队，不直接阻塞在 DLL 发送上。
    std::condition_variable m_inputQueueWake;
    std::deque<QPair<FsRemoteStreamHandle, QByteArray>> m_inputQueue;
    std::thread m_inputThread;
    bool m_inputThreadStopping = false;
};

} // namespace ui
