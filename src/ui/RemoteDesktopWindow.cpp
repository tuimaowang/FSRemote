#include "ui/RemoteDesktopWindow.h"

#include "stream/StreamRuntime.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRegion>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>

#include <thread>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ui {

namespace {

#if defined(Q_OS_WIN)
RemoteDesktopWindow* g_keyboardForwardTarget = nullptr;
HHOOK g_keyboardHook = nullptr;
#endif

enum ResizeEdge {
    ResizeNone = 0,
    ResizeLeft = 0x1,
    ResizeTop = 0x2,
    ResizeRight = 0x4,
    ResizeBottom = 0x8,
};

QString zh(const char* utf8)
{
    return QString::fromUtf8(utf8);
}

QPixmap icon(const QString& name)
{
    return QPixmap(QStringLiteral(":/UUGuest/resource/images/titlebar/") + name);
}

void drawWindowButtonIcon(QPainter& painter, const QRectF& rect, const QString& type)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#111820")), 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    if (type == QLatin1String("min")) {
        painter.drawLine(QPointF(rect.left() + 3, rect.center().y()), QPointF(rect.right() - 3, rect.center().y()));
    } else if (type == QLatin1String("max")) {
        painter.drawRect(QRectF(rect.left() + 2.5, rect.top() + 2.5, rect.width() - 5, rect.height() - 5));
    } else if (type == QLatin1String("close")) {
        painter.drawLine(QPointF(rect.left() + 2.5, rect.top() + 2.5), QPointF(rect.right() - 2.5, rect.bottom() - 2.5));
        painter.drawLine(QPointF(rect.right() - 2.5, rect.top() + 2.5), QPointF(rect.left() + 2.5, rect.bottom() - 2.5));
    }

    painter.restore();
}

void drawPlus(QPainter& painter, const QPointF& center)
{
    painter.save();
    painter.setPen(QPen(QColor(QStringLiteral("#333333")), 1.3, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(center.x() - 5, center.y()), QPointF(center.x() + 5, center.y()));
    painter.drawLine(QPointF(center.x(), center.y() - 5), QPointF(center.x(), center.y() + 5));
    painter.restore();
}

int computerNameWidth(const QString& name, const QFont& font)
{
    return qBound(18, QFontMetrics(font).horizontalAdvance(name), 156);
}

int tabXForComputerName(const QString& name, const QFont& font)
{
    return 40 + computerNameWidth(name, font) + 12;
}

QString virtualScreenTitle(int number)
{
    return zh("\xE8\x99\x9A\xE6\x8B\x9F\xE5\xB1\x8F %1").arg(number);
}

int tabWidth(const QString& title, const QFont& font)
{
    return qMax(224, QFontMetrics(font).horizontalAdvance(title) + 80);
}

QRect tabCloseRect(int tabX, int tabW)
{
    return QRect(tabX + tabW - 27, 12, 16, 16);
}

QRect plusRect(int x)
{
    return QRect(x, 6, 28, 28);
}

void drawVirtualScreenIcon(QPainter& painter, int tabX)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#111820")), 1.1, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(tabX + 11.5, 10.5, 14, 10), 1.5, 1.5);
    painter.setPen(QPen(QColor(QStringLiteral("#111820")), 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(QPointF(tabX + 15.5, 23), QPointF(tabX + 21.5, 23));
    painter.drawLine(QPointF(tabX + 18.5, 20), QPointF(tabX + 18.5, 23));
    painter.restore();
}

void drawSignalBars(QPainter& painter, int x, int y)
{
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#00B86B")));
    painter.drawRoundedRect(QRectF(x, y + 8, 2, 4), 1, 1);
    painter.drawRoundedRect(QRectF(x + 4, y + 5, 2, 7), 1, 1);
    painter.drawRoundedRect(QRectF(x + 8, y + 1, 2, 11), 1, 1);
    painter.restore();
}

void drawMutedMic(QPainter& painter, int x, int y)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#111820")), 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(x + 5, y + 1, 5, 9), 2.5, 2.5);
    painter.drawArc(QRectF(x + 3, y + 6, 9, 7), 200 * 16, 140 * 16);
    painter.drawLine(QPointF(x + 7.5, y + 14), QPointF(x + 7.5, y + 17));
    painter.drawLine(QPointF(x + 4.5, y + 17), QPointF(x + 10.5, y + 17));
    painter.drawLine(QPointF(x + 2, y + 2), QPointF(x + 14, y + 18));
    painter.restore();
}

#if defined(Q_OS_WIN)
LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_keyboardForwardTarget) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        if ((down || up) && info) {
            if (info->flags & LLKHF_INJECTED) {
                return 1;
            }
            g_keyboardForwardTarget->forwardNativeKey(static_cast<int>(info->vkCode), down);
            return 1;
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

void installKeyboardHook(RemoteDesktopWindow* window)
{
    g_keyboardForwardTarget = window;
    if (!g_keyboardHook) {
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, GetModuleHandleW(nullptr), 0);
    }
}

void uninstallKeyboardHook(RemoteDesktopWindow* window)
{
    if (g_keyboardForwardTarget == window) {
        g_keyboardForwardTarget = nullptr;
    }
    if (!g_keyboardForwardTarget && g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
}
#endif

} // namespace

namespace {

void FSREMOTE_STREAM_CALL onRemoteFrame(void* user, int width, int height, const uint8_t* bgra, uint32_t bgraSize)
{
    auto* window = static_cast<RemoteDesktopWindow*>(user);
    if (!window || width <= 0 || height <= 0 || !bgra) {
        return;
    }

    const qsizetype expected = qsizetype(width) * qsizetype(height) * 4;
    if (expected <= 0 || bgraSize < quint32(expected)) {
        return;
    }

    QImage image(bgra, width, height, width * 4, QImage::Format_ARGB32);
    QImage copy = image.copy();
    QMetaObject::invokeMethod(window, [window, copy = std::move(copy)] {
        if (window->isClosingConnection()) {
            return;
        }
        window->setRemoteFrame(copy);
    }, Qt::QueuedConnection);
}

void FSREMOTE_STREAM_CALL onViewerStatus(void* user, int code, const char* message)
{
    auto* window = static_cast<RemoteDesktopWindow*>(user);
    if (!window) {
        return;
    }

    const QString text = message ? QString::fromUtf8(message) : QString();
    QMetaObject::invokeMethod(window, [window, code, text] {
        if (window->isClosingConnection()) {
            return;
        }
        window->setConnectionStatus(code, text);
    }, Qt::QueuedConnection);
}

} // namespace

RemoteDesktopWindow::RemoteDesktopWindow(const QString& deviceName, const QString& hostIp, QWidget* parent)
    : QWidget(parent)
    , m_deviceName(deviceName)
    , m_hostIp(hostIp)
{
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1920, 1120);
    setMinimumSize(520, 360);
    setWindowTitle(m_deviceName);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    updateWindowMask();
    m_virtualScreens.append(1);
    m_nextVirtualScreenNumber = 2;
    m_connectionStatus = zh("\xE5\x87\x86\xE5\xA4\x87\xE8\xBF\x9E\xE6\x8E\xA5");

    m_sessionClock.start();
    m_sessionTimer = new QTimer(this);
    m_sessionTimer->setInterval(1000);
    connect(m_sessionTimer, &QTimer::timeout, this, [this] {
        update(QRect(0, 0, width(), 40));
    });
    m_sessionTimer->start();

    QTimer::singleShot(0, this, &RemoteDesktopWindow::startViewerConnection);
}

RemoteDesktopWindow::~RemoteDesktopWindow()
{
    releasePressedKeys();
    setKeyboardForwardingActive(false);
    if (m_viewerHandle) {
        stream::StreamRuntime::instance().stop(m_viewerHandle);
        m_viewerHandle = nullptr;
    }
}

QRect RemoteDesktopWindow::minimizeRect() const
{
    return QRect(width() - 138, 0, 48, 40);
}

QRect RemoteDesktopWindow::maximizeRect() const
{
    return QRect(width() - 90, 0, 43, 40);
}

QRect RemoteDesktopWindow::closeRect() const
{
    return QRect(width() - 47, 0, 47, 40);
}

QRect RemoteDesktopWindow::controlCenterRect() const
{
    return QRect(width() - 223, 0, 90, 40);
}

QRect RemoteDesktopWindow::remoteImageRect() const
{
    if (m_remoteFrame.isNull()) {
        return {};
    }
    const QRect contentRect(0, 40, width(), height() - 40);
    const QSize scaled = m_remoteFrame.size().scaled(contentRect.size(), Qt::KeepAspectRatio);
    return QRect(
        contentRect.x() + (contentRect.width() - scaled.width()) / 2,
        contentRect.y() + (contentRect.height() - scaled.height()) / 2,
        scaled.width(),
        scaled.height());
}

bool RemoteDesktopWindow::normalizedRemotePoint(const QPoint& position, int* x, int* y) const
{
    const QRect imageRect = remoteImageRect();
    if (!imageRect.contains(position) || imageRect.width() <= 1 || imageRect.height() <= 1) {
        return false;
    }
    if (x) {
        *x = qBound(0, (position.x() - imageRect.left()) * 65535 / (imageRect.width() - 1), 65535);
    }
    if (y) {
        *y = qBound(0, (position.y() - imageRect.top()) * 65535 / (imageRect.height() - 1), 65535);
    }
    return true;
}

void RemoteDesktopWindow::sendInputMessage(const QByteArray& message)
{
    stream::StreamRuntime::instance().sendInput(m_viewerHandle, message);
}

void RemoteDesktopWindow::setKeyboardForwardingActive(bool active)
{
#if defined(Q_OS_WIN)
    if (active && !m_closeInProgress && isActiveWindow()) {
        installKeyboardHook(this);
    } else {
        uninstallKeyboardHook(this);
    }
#else
    Q_UNUSED(active)
#endif
}

void RemoteDesktopWindow::releasePressedKeys()
{
    const auto keys = m_pressedKeys;
    m_pressedKeys.clear();
    for (int key : keys) {
        sendInputMessage(QByteArray("k ")
            + QByteArray::number(key) + " 0");
    }
}

int RemoteDesktopWindow::remoteButton(Qt::MouseButton button) const
{
    if (button == Qt::LeftButton) return 1;
    if (button == Qt::RightButton) return 2;
    if (button == Qt::MiddleButton) return 4;
    return 0;
}

RemoteDesktopWindow::TabHit RemoteDesktopWindow::hitTestTabs(const QPoint& position) const
{
    QFont textFont(QStringLiteral("Microsoft YaHei UI"));
    textFont.setPixelSize(12);

    int tabX = tabXForComputerName(m_deviceName, textFont);
    for (int i = 0; i < m_virtualScreens.size(); ++i) {
        const int tabW = tabWidth(virtualScreenTitle(m_virtualScreens.at(i)), textFont);
        if (tabCloseRect(tabX, tabW).contains(position)) {
            return {TabHitType::CloseTab, i};
        }
            if (QRect(tabX, 5, tabW, 35).contains(position)) {
                return {TabHitType::Tab, i};
            }
        tabX += tabW;
    }

    if (plusRect(tabX + 10).contains(position)) {
        return {TabHitType::Add, -1};
    }

    return {};
}

int RemoteDesktopWindow::resizeEdgesAt(const QPoint& position) const
{
    if (isMaximized()) {
        return ResizeNone;
    }

    constexpr int margin = 6;
    int edges = ResizeNone;
    if (position.x() <= margin) {
        edges |= ResizeLeft;
    } else if (position.x() >= width() - margin) {
        edges |= ResizeRight;
    }

    if (position.y() <= margin) {
        edges |= ResizeTop;
    } else if (position.y() >= height() - margin) {
        edges |= ResizeBottom;
    }
    return edges;
}

void RemoteDesktopWindow::updateResizeCursor(const QPoint& position)
{
    const int edges = resizeEdgesAt(position);
    if ((edges & ResizeLeft && edges & ResizeTop) || (edges & ResizeRight && edges & ResizeBottom)) {
        setCursor(Qt::SizeFDiagCursor);
    } else if ((edges & ResizeRight && edges & ResizeTop) || (edges & ResizeLeft && edges & ResizeBottom)) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges & (ResizeLeft | ResizeRight)) {
        setCursor(Qt::SizeHorCursor);
    } else if (edges & (ResizeTop | ResizeBottom)) {
        setCursor(Qt::SizeVerCursor);
    } else {
        unsetCursor();
    }
}

void RemoteDesktopWindow::updateWindowMask()
{
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, width(), height()), 6, 6);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
}

void RemoteDesktopWindow::setRemoteFrame(const QImage& image)
{
    m_remoteFrame = image;
    m_connectionStatusCode = 50;
    m_connectionStatus = QString::fromUtf8("画面已接收");
    update(QRect(0, 40, width(), height() - 40));
}

bool RemoteDesktopWindow::isClosingConnection() const
{
    return m_closeInProgress;
}

bool RemoteDesktopWindow::forwardNativeKey(int virtualKey, bool down)
{
    if (m_closeInProgress || virtualKey <= 0) {
        return false;
    }
    if (down) {
        m_pressedKeys.insert(virtualKey);
    } else {
        m_pressedKeys.remove(virtualKey);
    }
    sendInputMessage(QByteArray("k ")
        + QByteArray::number(virtualKey) + ' '
        + (down ? QByteArray("1") : QByteArray("0")));
    return true;
}

void RemoteDesktopWindow::startViewerConnection()
{
    if (!m_hostIp.trimmed().isEmpty()) {
        m_viewerHandle = stream::StreamRuntime::instance().startViewer(
            m_hostIp.trimmed(),
            49100,
            onRemoteFrame,
            onViewerStatus,
            this);
        if (!m_viewerHandle) {
            setConnectionStatus(90, stream::StreamRuntime::instance().lastError());
        }
    } else {
        setConnectionStatus(90, QString::fromUtf8("设备 IP 为空"));
    }
}

void RemoteDesktopWindow::setConnectionStatus(int code, const QString& message)
{
    m_connectionStatusCode = code;
    switch (code) {
    case 10:
        m_connectionStatus = zh("\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xBF\x9E\xE6\x8E\xA5 TCP");
        break;
    case 20:
        m_connectionStatus = zh("TCP \xE5\xB7\xB2\xE8\xBF\x9E\xE6\x8E\xA5");
        break;
    case 30:
        m_connectionStatus = zh("\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x9D\xE5\xA7\x8B\xE5\x8C\x96 WebRTC");
        break;
    case 40:
        m_connectionStatus = zh("\xE7\xAD\x89\xE5\xBE\x85\xE8\xBF\x9C\xE7\xA8\x8B\xE7\x94\xBB\xE9\x9D\xA2");
        break;
    case 50:
        m_connectionStatus = zh("\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x8E\xA5\xE6\x94\xB6\xE7\x94\xBB\xE9\x9D\xA2");
        break;
    case 80:
        m_connectionStatus = zh("\xE8\xBF\x9E\xE6\x8E\xA5\xE5\xB7\xB2\xE6\x96\xAD\xE5\xBC\x80");
        break;
    case 90:
        m_connectionStatus = message.isEmpty()
            ? zh("\xE8\xBF\x9E\xE6\x8E\xA5\xE5\xA4\xB1\xE8\xB4\xA5")
            : zh("\xE8\xBF\x9E\xE6\x8E\xA5\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x9A%1").arg(message);
        break;
    default:
        m_connectionStatus = message.isEmpty() ? zh("\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xBF\x9E\xE6\x8E\xA5") : message;
        break;
    }
    update(QRect(0, 40, width(), height() - 40));
}

void RemoteDesktopWindow::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    painter.fillRect(rect(), QColor(QStringLiteral("#000000")));
    if (!m_remoteFrame.isNull()) {
        const QRect target = remoteImageRect();
        painter.drawImage(target, m_remoteFrame);
    } else {
        const QRect contentRect(0, 40, width(), height() - 40);
        QFont titleFont(QStringLiteral("Microsoft YaHei UI"));
        titleFont.setPixelSize(18);
        titleFont.setWeight(QFont::DemiBold);
        QFont statusFont(QStringLiteral("Microsoft YaHei UI"));
        statusFont.setPixelSize(13);

        painter.setFont(titleFont);
        painter.setPen(QColor(QStringLiteral("#FFFFFF")));
        painter.drawText(
            QRectF(contentRect.left(), contentRect.center().y() - 34, contentRect.width(), 26),
            Qt::AlignCenter,
            zh("\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xBF\x9E\xE6\x8E\xA5 %1").arg(m_deviceName));

        painter.setFont(statusFont);
        painter.setPen(m_connectionStatusCode == 90 ? QColor(QStringLiteral("#FFB4B4")) : QColor(QStringLiteral("#B8C0CC")));
        painter.drawText(
            QRectF(contentRect.left() + 60, contentRect.center().y(), contentRect.width() - 120, 48),
            Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
            m_connectionStatus);
    }
    painter.fillRect(QRectF(0, 0, width(), 40), QColor(QStringLiteral("#E9EEF2")));

    painter.setPen(QPen(QColor(QStringLiteral("#AEB7C2")), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 6, 6);

    painter.drawPixmap(QRect(16, 10, 20, 20), icon(QStringLiteral("fs_session_logo.svg")));
    QFont textFont(QStringLiteral("Microsoft YaHei UI"));
    textFont.setPixelSize(12);
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#111820")));
    const int nameWidth = computerNameWidth(m_deviceName, textFont);
    painter.drawText(QRectF(40, 0, nameWidth, 40), Qt::AlignVCenter | Qt::AlignLeft, m_deviceName);

    int tabX = tabXForComputerName(m_deviceName, textFont);
    int tabsEndX = tabX;
    for (int i = 0; i < m_virtualScreens.size(); ++i) {
        const QString title = virtualScreenTitle(m_virtualScreens.at(i));
        const int tabW = tabWidth(title, textFont);

        QPainterPath tabPath;
        tabPath.moveTo(tabX, 40);
        tabPath.lineTo(tabX, 13);
        tabPath.quadTo(tabX, 5, tabX + 8, 5);
        tabPath.lineTo(tabX + tabW - 8, 5);
        tabPath.quadTo(tabX + tabW, 5, tabX + tabW, 13);
        tabPath.lineTo(tabX + tabW, 40);
        tabPath.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(i == m_activeTabIndex ? QColor(QStringLiteral("#F6F7F8")) : QColor(QStringLiteral("#ECEFF2")));
        painter.drawPath(tabPath);

        drawVirtualScreenIcon(painter, tabX + 1);
        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#111820")));
        painter.drawText(QRectF(tabX + 40, 10, tabW - 76, 20), Qt::AlignVCenter | Qt::AlignLeft, title);

        const QRect closeTab = tabCloseRect(tabX, tabW);
        if (closeTab.contains(m_hoveredPos)) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#E5E7EB")));
            painter.drawRoundedRect(QRectF(closeTab), 4, 4);
        }
        painter.setPen(QPen(QColor(QStringLiteral("#6B7280")), 1.2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(closeTab.center().x() - 4, closeTab.center().y() - 4), QPointF(closeTab.center().x() + 4, closeTab.center().y() + 4));
        painter.drawLine(QPointF(closeTab.center().x() + 4, closeTab.center().y() - 4), QPointF(closeTab.center().x() - 4, closeTab.center().y() + 4));

        tabX += tabW;
        tabsEndX = tabX;
    }

    QFont plusFont(QStringLiteral("Segoe UI"));
    plusFont.setPixelSize(18);
    plusFont.setWeight(QFont::Normal);
    painter.setFont(plusFont);
    painter.setPen(QColor(QStringLiteral("#111820")));
    painter.drawText(QRectF(plusRect(tabsEndX + 10)), Qt::AlignCenter, QStringLiteral("+"));

    const int signalX = tabsEndX + 48;
    drawSignalBars(painter, signalX, 12);
    const qint64 elapsedSeconds = m_sessionClock.elapsed() / 1000;
    const qint64 hours = elapsedSeconds / 3600;
    const qint64 minutes = (elapsedSeconds / 60) % 60;
    const qint64 seconds = elapsedSeconds % 60;
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#4B4B4C")));
    painter.drawText(
        QRectF(signalX + 20, 9, 70, 22),
        Qt::AlignVCenter | Qt::AlignLeft,
        QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0')));

    painter.drawPixmap(QRect(width() - 223, 7, 26, 26), icon(QStringLiteral("rd_control_center.svg")));
    painter.setPen(QColor(QStringLiteral("#111820")));
    painter.drawText(QRectF(width() - 197, 9, 64, 22), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE6\x8E\xA7\xE5\x88\xB6\xE4\xB8\xAD\xE5\xBF\x83"));

    painter.drawPixmap(QRect(width() - 137, 0, 46, 40), icon(QStringLiteral("rd_minimize.svg")));
    painter.drawPixmap(QRect(width() - 91, 0, 46, 40), icon(QStringLiteral("rd_maximize.svg")));
    painter.drawPixmap(QRect(width() - 48, -4, 48, 48), icon(QStringLiteral("rd_close.svg")));
}

void RemoteDesktopWindow::closeEvent(QCloseEvent* event)
{
    if (!m_viewerHandle || m_closeInProgress) {
        QWidget::closeEvent(event);
        return;
    }

    event->ignore();
    m_closeInProgress = true;
    releasePressedKeys();
    setKeyboardForwardingActive(false);
    const FsRemoteStreamHandle handle = m_viewerHandle;
    m_viewerHandle = nullptr;
    hide();

    std::thread([this, handle] {
        stream::StreamRuntime::instance().stop(handle);
        QMetaObject::invokeMethod(this, [this] {
            deleteLater();
        }, Qt::QueuedConnection);
    }).detach();
}

void RemoteDesktopWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_resizeEdges = resizeEdgesAt(event->pos());
        if (m_resizeEdges != ResizeNone) {
            m_resizingWindow = true;
            m_resizeStartGlobal = event->globalPosition().toPoint();
            m_resizeStartGeometry = frameGeometry();
            event->accept();
            return;
        }

        if (event->pos().y() >= 0
            && event->pos().y() < 40
            && hitTestTabs(event->pos()).type == TabHitType::None
            && !controlCenterRect().contains(event->pos())
            && !minimizeRect().contains(event->pos())
            && !maximizeRect().contains(event->pos())
            && !closeRect().contains(event->pos())) {
            m_draggingWindow = true;
            m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
            event->accept();
            return;
        }
    }

    int x = 0;
    int y = 0;
    const int button = remoteButton(event->button());
    if (button && normalizedRemotePoint(event->pos(), &x, &y)) {
        setFocus(Qt::MouseFocusReason);
        sendInputMessage(QByteArray("d ")
            + QByteArray::number(button) + ' '
            + QByteArray::number(x) + ' '
            + QByteArray::number(y));
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void RemoteDesktopWindow::mouseMoveEvent(QMouseEvent* event)
{
    m_hoveredPos = event->pos();
    update(QRect(0, 0, width(), 40));

    if (m_resizingWindow && (event->buttons() & Qt::LeftButton)) {
        const QPoint delta = event->globalPosition().toPoint() - m_resizeStartGlobal;
        QRect next = m_resizeStartGeometry;
        const QSize minSize = minimumSize();

        if (m_resizeEdges & ResizeLeft) {
            next.setLeft(next.left() + delta.x());
            if (next.width() < minSize.width()) {
                next.setLeft(next.right() - minSize.width() + 1);
            }
        }
        if (m_resizeEdges & ResizeRight) {
            next.setRight(next.right() + delta.x());
            if (next.width() < minSize.width()) {
                next.setRight(next.left() + minSize.width() - 1);
            }
        }
        if (m_resizeEdges & ResizeTop) {
            next.setTop(next.top() + delta.y());
            if (next.height() < minSize.height()) {
                next.setTop(next.bottom() - minSize.height() + 1);
            }
        }
        if (m_resizeEdges & ResizeBottom) {
            next.setBottom(next.bottom() + delta.y());
            if (next.height() < minSize.height()) {
                next.setBottom(next.top() + minSize.height() - 1);
            }
        }

        setGeometry(next);
        event->accept();
        return;
    }

    if (m_draggingWindow && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }

    int x = 0;
    int y = 0;
    if (normalizedRemotePoint(event->pos(), &x, &y)) {
        const int buttons =
            (event->buttons() & Qt::LeftButton ? 1 : 0) |
            (event->buttons() & Qt::RightButton ? 2 : 0) |
            (event->buttons() & Qt::MiddleButton ? 4 : 0);
        sendInputMessage(QByteArray("m ")
            + QByteArray::number(x) + ' '
            + QByteArray::number(y) + ' '
            + QByteArray::number(buttons));
        event->accept();
        return;
    }

    updateResizeCursor(event->pos());
    QWidget::mouseMoveEvent(event);
}

void RemoteDesktopWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_draggingWindow = false;
        m_resizingWindow = false;
        m_resizeEdges = ResizeNone;

        const TabHit tabHit = hitTestTabs(event->pos());
        if (tabHit.type == TabHitType::Add) {
            m_virtualScreens.append(m_nextVirtualScreenNumber++);
            m_activeTabIndex = m_virtualScreens.size() - 1;
            update(QRect(0, 0, width(), 40));
            event->accept();
            return;
        }
        if (tabHit.type == TabHitType::CloseTab) {
            if (m_virtualScreens.size() <= 1) {
                close();
                event->accept();
                return;
            }
            m_virtualScreens.removeAt(tabHit.index);
            if (m_activeTabIndex >= m_virtualScreens.size()) {
                m_activeTabIndex = m_virtualScreens.size() - 1;
            } else if (tabHit.index < m_activeTabIndex) {
                --m_activeTabIndex;
            }
            update(QRect(0, 0, width(), 40));
            event->accept();
            return;
        }
        if (tabHit.type == TabHitType::Tab) {
            m_activeTabIndex = tabHit.index;
            update(QRect(0, 0, width(), 40));
            event->accept();
            return;
        }

        if (minimizeRect().contains(event->pos())) {
            showMinimized();
            event->accept();
            return;
        }
        if (maximizeRect().contains(event->pos())) {
            isMaximized() ? showNormal() : showMaximized();
            event->accept();
            return;
        }
        if (closeRect().contains(event->pos())) {
            close();
            event->accept();
            return;
        }
    }

    int x = 0;
    int y = 0;
    const int button = remoteButton(event->button());
    if (button && normalizedRemotePoint(event->pos(), &x, &y)) {
        sendInputMessage(QByteArray("u ")
            + QByteArray::number(button) + ' '
            + QByteArray::number(x) + ' '
            + QByteArray::number(y));
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void RemoteDesktopWindow::wheelEvent(QWheelEvent* event)
{
    int x = 0;
    int y = 0;
    if (normalizedRemotePoint(event->position().toPoint(), &x, &y)) {
        sendInputMessage(QByteArray("w ")
            + QByteArray::number(event->angleDelta().y()) + ' '
            + QByteArray::number(x) + ' '
            + QByteArray::number(y));
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void RemoteDesktopWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->nativeVirtualKey() > 0) {
        m_pressedKeys.insert(event->nativeVirtualKey());
        sendInputMessage(QByteArray("k ")
            + QByteArray::number(event->nativeVirtualKey()) + " 1");
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RemoteDesktopWindow::keyReleaseEvent(QKeyEvent* event)
{
    if (event->nativeVirtualKey() > 0) {
        m_pressedKeys.remove(event->nativeVirtualKey());
        sendInputMessage(QByteArray("k ")
            + QByteArray::number(event->nativeVirtualKey()) + " 0");
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void RemoteDesktopWindow::focusInEvent(QFocusEvent* event)
{
    setKeyboardForwardingActive(true);
    QWidget::focusInEvent(event);
}

void RemoteDesktopWindow::focusOutEvent(QFocusEvent* event)
{
    releasePressedKeys();
    setKeyboardForwardingActive(false);
    QWidget::focusOutEvent(event);
}

void RemoteDesktopWindow::resizeEvent(QResizeEvent* event)
{
    updateWindowMask();
    QWidget::resizeEvent(event);
}

void RemoteDesktopWindow::leaveEvent(QEvent* event)
{
    m_hoveredPos = QPoint(-1, -1);
    update(QRect(0, 0, width(), 40));
    QWidget::leaveEvent(event);
}

} // namespace ui
