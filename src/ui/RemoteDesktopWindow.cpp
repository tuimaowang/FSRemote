#include "ui/RemoteDesktopWindow.h"

#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "stream/StreamRuntime.h"
#include "ui/D3D11FramePresenter.h"

#include <QByteArray>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMetaObject>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QRegion>
#include <QResizeEvent>
#include <QScreen>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
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
QSet<int> g_hookPressedKeys;
#endif

enum ResizeEdge {
    ResizeNone = 0,
    ResizeLeft = 0x1,
    ResizeTop = 0x2,
    ResizeRight = 0x4,
    ResizeBottom = 0x8,
};

constexpr int kWindowResizeMargin = 6; // wjy: 无边框远控窗口四周统一保留 6px 缩放带，标题栏按钮不得覆盖这一区域。

QString zh(const char* utf8)
{
    return QString::fromUtf8(utf8);
}

QPixmap icon(const QString& name)
{
    return QPixmap(QStringLiteral(":/UUGuest/resource/images/titlebar/") + name);
}

QRect normalizedSavedWindowGeometry(const QRect& geometry, const QSize& minimumSize)
{
    if (!geometry.isValid()) {
        return {};
    }

    QRect result = geometry;
    result.setWidth(qMax(result.width(), minimumSize.width()));
    result.setHeight(qMax(result.height(), minimumSize.height()));

    QRect availableRect;
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (QScreen* screen : screens) {
        if (!screen) {
            continue;
        }
        const QRect screenRect = screen->availableGeometry();
        if (screenRect.intersects(result)) {
            availableRect = screenRect;
            break;
        }
    }

    if (availableRect.isNull()) {
        QScreen* primaryScreen = QGuiApplication::primaryScreen();
        availableRect = primaryScreen ? primaryScreen->availableGeometry() : QRect(0, 0, 1280, 720);
        result.moveTopLeft(availableRect.topLeft() + QPoint(32, 32));
    }

    if (result.width() > availableRect.width()) {
        result.setWidth(availableRect.width());
    }
    if (result.height() > availableRect.height()) {
        result.setHeight(availableRect.height());
    }
    if (result.right() > availableRect.right()) {
        result.moveRight(availableRect.right());
    }
    if (result.bottom() > availableRect.bottom()) {
        result.moveBottom(availableRect.bottom());
    }
    if (result.left() < availableRect.left()) {
        result.moveLeft(availableRect.left());
    }
    if (result.top() < availableRect.top()) {
        result.moveTop(availableRect.top());
    }

    return result;
}

// =====wjy====
void appendViewerDebugLog(const QString& line)
{
    Q_UNUSED(line)
    return; // wjy: disable Qt-side viewer diagnostics completely while tuning remote desktop performance.

    const QString dataDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data")); // wjy: keep Qt-side frame logs beside the native stream DLL logs.
    QDir().mkpath(dataDir); // wjy: make the data directory if this is the first stream log in a fresh build folder.
    QFile file(QDir(dataDir).filePath(QStringLiteral("stream_viewer_debug.log"))); // wjy: use the same file as the native WebRTC diagnostics.
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return; // wjy: logging must never block or crash the remote desktop UI.
    }
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
           << QStringLiteral(" qt ")
           << line
           << Qt::endl; // wjy: Qt::endl flushes each line so a crash leaves the latest checkpoint.
}
// ===end====

void appendInputDebugLog(const QString& line)
{
    QFile file(QDir::temp().filePath(QStringLiteral("fsremote_input_debug.log")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
           << QStringLiteral(" qt ")
           << line
           << Qt::endl;
}

bool shouldLogInputMessage(const QByteArray& message)
{
    if (!message.startsWith("m ")) {
        return true;
    }

    static qint64 lastMouseMoveLogMs = 0;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - lastMouseMoveLogMs < 500) {
        return false;
    }
    lastMouseMoveLogMs = nowMs;
    return true;
}

// =====wjy====
bool isShortcutModifierKey(int key)
{
    return key == Qt::Key_Control
        || key == Qt::Key_Shift
        || key == Qt::Key_Alt
        || key == Qt::Key_Meta; // wjy: 单独修饰键不触发本地快捷键，等待后续主键组成 Ctrl+D 这类序列。
}

Qt::KeyboardModifiers shortcutModifiers(Qt::KeyboardModifiers modifiers)
{
    return modifiers & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier); // wjy: 去掉小键盘等附加标志，确保和设置页保存的快捷键一致。
}

QKeySequence shortcutSequenceFromKeyEvent(const QKeyEvent* event)
{
    if (!event || isShortcutModifierKey(event->key())) {
        return {};
    }
    return QKeySequence(shortcutModifiers(event->modifiers()).toInt() | event->key()); // wjy: Qt 按键事件转成和设置页相同的可比较快捷键序列。
}

bool matchesShortcut(const QKeySequence& current, const QKeySequence& shortcut)
{
    return !current.isEmpty() && !shortcut.isEmpty() && current == shortcut; // wjy: 精确匹配用户自定义快捷键，避免 Ctrl+F4 误触发 F4。
}

#if defined(Q_OS_WIN)
int normalizedHookVirtualKey(int virtualKey)
{
    switch (virtualKey) {
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_CONTROL:
        return VK_CONTROL;
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_SHIFT:
        return VK_SHIFT;
    case VK_LMENU:
    case VK_RMENU:
    case VK_MENU:
        return VK_MENU;
    case VK_LWIN:
    case VK_RWIN:
        return VK_LWIN;
    default:
        return virtualKey;
    }
}

bool isTrackedHookKeyDown(int virtualKey)
{
    return g_hookPressedKeys.contains(normalizedHookVirtualKey(virtualKey));
}

void updateTrackedHookKey(int virtualKey, bool down)
{
    const int normalizedKey = normalizedHookVirtualKey(virtualKey);
    if (down) {
        g_hookPressedKeys.insert(normalizedKey);
    } else {
        g_hookPressedKeys.remove(normalizedKey);
    }
}

Qt::KeyboardModifiers trackedHookShortcutModifiers()
{
    Qt::KeyboardModifiers modifiers;
    if (isTrackedHookKeyDown(VK_CONTROL)) {
        modifiers |= Qt::ControlModifier;
    }
    if (isTrackedHookKeyDown(VK_SHIFT)) {
        modifiers |= Qt::ShiftModifier;
    }
    if (isTrackedHookKeyDown(VK_MENU)) {
        modifiers |= Qt::AltModifier;
    }
    if (isTrackedHookKeyDown(VK_LWIN) || isTrackedHookKeyDown(VK_RWIN)) {
        modifiers |= Qt::MetaModifier;
    }
    return modifiers;
}

int qtKeyFromNativeVirtualKey(int virtualKey)
{
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        return Qt::Key_A + (virtualKey - 'A');
    }
    if (virtualKey >= '0' && virtualKey <= '9') {
        return Qt::Key_0 + (virtualKey - '0');
    }
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return Qt::Key_F1 + (virtualKey - VK_F1);
    }

    switch (virtualKey) {
    case VK_ESCAPE: return Qt::Key_Escape;
    case VK_SPACE: return Qt::Key_Space;
    case VK_TAB: return Qt::Key_Tab;
    case VK_BACK: return Qt::Key_Backspace;
    case VK_RETURN: return Qt::Key_Return;
    case VK_INSERT: return Qt::Key_Insert;
    case VK_DELETE: return Qt::Key_Delete;
    case VK_HOME: return Qt::Key_Home;
    case VK_END: return Qt::Key_End;
    case VK_PRIOR: return Qt::Key_PageUp;
    case VK_NEXT: return Qt::Key_PageDown;
    case VK_LEFT: return Qt::Key_Left;
    case VK_RIGHT: return Qt::Key_Right;
    case VK_UP: return Qt::Key_Up;
    case VK_DOWN: return Qt::Key_Down;
    default: return 0;
    }
}

bool virtualKeyFromQtKey(int key, int* virtualKey)
{
    if (!virtualKey) {
        return false;
    }
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        *virtualKey = 'A' + (key - Qt::Key_A);
        return true;
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        *virtualKey = '0' + (key - Qt::Key_0);
        return true;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        *virtualKey = VK_F1 + (key - Qt::Key_F1);
        return true;
    }

    switch (key) {
    case Qt::Key_Escape: *virtualKey = VK_ESCAPE; return true;
    case Qt::Key_Space: *virtualKey = VK_SPACE; return true;
    case Qt::Key_Tab: *virtualKey = VK_TAB; return true;
    case Qt::Key_Backtab: *virtualKey = VK_TAB; return true;
    case Qt::Key_Backspace: *virtualKey = VK_BACK; return true;
    case Qt::Key_Return: *virtualKey = VK_RETURN; return true;
    case Qt::Key_Enter: *virtualKey = VK_RETURN; return true;
    case Qt::Key_Insert: *virtualKey = VK_INSERT; return true;
    case Qt::Key_Delete: *virtualKey = VK_DELETE; return true;
    case Qt::Key_Home: *virtualKey = VK_HOME; return true;
    case Qt::Key_End: *virtualKey = VK_END; return true;
    case Qt::Key_PageUp: *virtualKey = VK_PRIOR; return true;
    case Qt::Key_PageDown: *virtualKey = VK_NEXT; return true;
    case Qt::Key_Left: *virtualKey = VK_LEFT; return true;
    case Qt::Key_Right: *virtualKey = VK_RIGHT; return true;
    case Qt::Key_Up: *virtualKey = VK_UP; return true;
    case Qt::Key_Down: *virtualKey = VK_DOWN; return true;
    default: return false;
    }
}

QVector<int> shortcutVirtualKeys(const QKeySequence& shortcut)
{
    QVector<int> virtualKeys;
    if (shortcut.isEmpty()) {
        return virtualKeys;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int combined = shortcut[0].toCombined();
#else
    const int combined = shortcut[0];
#endif
    if (combined <= 0) {
        return virtualKeys;
    }

    const int modifierMask = static_cast<int>(Qt::KeyboardModifierMask);
    const Qt::KeyboardModifiers modifiers = Qt::KeyboardModifiers(combined & modifierMask);
    if (modifiers.testFlag(Qt::ControlModifier)) {
        virtualKeys.append(VK_CONTROL);
    }
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        virtualKeys.append(VK_SHIFT);
    }
    if (modifiers.testFlag(Qt::AltModifier)) {
        virtualKeys.append(VK_MENU);
    }
    if (modifiers.testFlag(Qt::MetaModifier)) {
        virtualKeys.append(VK_LWIN);
    }

    int mainVirtualKey = 0;
    if (virtualKeyFromQtKey(combined & ~modifierMask, &mainVirtualKey) && mainVirtualKey > 0) {
        virtualKeys.append(mainVirtualKey);
    }
    return virtualKeys;
}

QKeySequence shortcutSequenceFromNativeVirtualKey(int virtualKey, Qt::KeyboardModifiers modifiers)
{
    const int key = qtKeyFromNativeVirtualKey(virtualKey);
    if (key <= 0 || isShortcutModifierKey(key)) {
        return {};
    }
    return QKeySequence(shortcutModifiers(modifiers).toInt() | key); // wjy: Windows 钩子路径也转为 QKeySequence，和设置页自定义值统一比较。
}
#endif
// ===end====

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

int computerNameWidth(const QString& name, const QFont& font)
{
    return qBound(18, QFontMetrics(font).horizontalAdvance(name), 156);
}

// =====wjy====
// wjy: 标题栏虚拟屏标签已删除，不再需要计算标签起始位置。
// ===end====

// =====wjy====
// wjy: “虚拟屏”文字、显示器图标和“+”点击区已从标题栏移除，不再保留对应布局与绘制辅助函数。
// ===end====

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
            const int virtualKey = static_cast<int>(info->vkCode);
            updateTrackedHookKey(virtualKey, down);
            RemoteDesktopWindow* target = g_keyboardForwardTarget;
            const bool wasWaitingShortcutRelease = target->isWaitingShortcutRelease();
            if (wasWaitingShortcutRelease) {
                target->updateShortcutReleaseGuard();
                return 1; // wjy: 本地快捷键触发后，直到组合键真正松开之前都不再把任何按键继续转发到远端。
            }
            const Qt::KeyboardModifiers modifiers = trackedHookShortcutModifiers();
            if (down && target->handleLocalShortcutKey(virtualKey, modifiers)) {
                return 1;
            }
            target->forwardNativeKey(virtualKey, down);
            return 1;
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

void installKeyboardHook(RemoteDesktopWindow* window)
{
    if (g_keyboardForwardTarget != window) {
        g_hookPressedKeys.clear();
    }
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
        g_hookPressedKeys.clear();
    }
}
#endif

} // namespace

namespace {

void FSREMOTE_STREAM_CALL onRemoteFrame(void* user, int width, int height, const uint8_t* bgra, uint32_t bgraSize)
{
    // =====wjy====
    // appendViewerDebugLog(QStringLiteral("onRemoteFrame enter user=%1 size=%2x%3 bytes=%4")
    //     .arg(reinterpret_cast<quintptr>(user))
    //     .arg(width)
    //     .arg(height)
    //     .arg(bgraSize)); // wjy: per-frame log disabled to avoid synchronous file writes on the video path.
    // ===end====
    auto* window = static_cast<RemoteDesktopWindow*>(user);
    if (!window || width <= 0 || height <= 0 || !bgra) {
        // appendViewerDebugLog(QStringLiteral("onRemoteFrame reject invalid args")); // wjy: per-frame guard log disabled for smoother rendering.
        return;
    }

    const qsizetype expected = qsizetype(width) * qsizetype(height) * 4;
    if (expected <= 0 || bgraSize < quint32(expected)) {
        // appendViewerDebugLog(QStringLiteral("onRemoteFrame reject byte size expected=%1").arg(expected)); // wjy: per-frame size log disabled for smoother rendering.
        return;
    }

    // appendViewerDebugLog(QStringLiteral("onRemoteFrame before QImage copy")); // wjy: per-frame copy log disabled because QImage copy already costs enough.
    QImage image(bgra, width, height, width * 4, QImage::Format_RGB32); // wjy: Ignore decoder alpha; remote desktop pixels should be opaque, otherwise Qt blending can look like a pale veil.
    QImage copy = image.copy();
    // appendViewerDebugLog(QStringLiteral("onRemoteFrame after QImage copy")); // wjy: per-frame copy log disabled to reduce IO pressure.
    window->enqueueRemoteFrame(std::move(copy)); // wjy: Keep only the newest frame instead of queuing every decoded frame into the Qt event loop.
}

int FSREMOTE_STREAM_CALL onRemoteTextureFrame(void* user, int width, int height, void* sharedHandle, uint64_t frameId, double encodedMbps)
{
    auto* window = static_cast<RemoteDesktopWindow*>(user);
    if (!window || width <= 0 || height <= 0 || !sharedHandle) {
        return 0;
    }
    return window->enqueueRemoteTextureFrame(width, height, sharedHandle, frameId, encodedMbps) ? 1 : 0;
}

void FSREMOTE_STREAM_CALL onViewerStatus(void* user, int code, const char* message)
{
    auto* window = static_cast<RemoteDesktopWindow*>(user);
    if (!window) {
        return;
    }

    const QString text = message ? QString::fromUtf8(message) : QString();
    // =====wjy====
    if (code != 60) {
        appendViewerDebugLog(QStringLiteral("viewer status code=%1 message=%2").arg(code).arg(text)); // wjy: correlate UI status text with native stream checkpoints while avoiding bitrate log spam.
    }
    // ===end====
    QMetaObject::invokeMethod(window, [window, code, text] {
        if (window->isClosingConnection()) {
            return;
        }
        if (code == 60 && text.startsWith(QStringLiteral("ENC "))) {
            bool ok = false;
            const double mbps = text.mid(4).toDouble(&ok);
            if (ok) {
                window->setEncodedBitrateMbps(mbps);
            }
            return;
        }
        if (code == 61 && text.startsWith(QStringLiteral("MOUSE "))) {
            window->setRemoteMouseCaptureActive(text == QStringLiteral("MOUSE relative"));
            return;
        }
        if (code == 62 && text.startsWith(QStringLiteral("cb "))) {
            window->applyRemoteClipboardPayload(text.mid(3)); // wjy: Host 推送的文本剪贴板。
            return;
        }
        // ===end====
        window->setConnectionStatus(code, text);
    }, Qt::QueuedConnection);
}

} // namespace

RemoteDesktopWindow::RemoteDesktopWindow(const QString& deviceName, const QString& hostIp, QWidget* parent)
    : QWidget(parent)
    , m_deviceName(deviceName)
    , m_hostIp(hostIp)
{
    // =====wjy====
    appendViewerDebugLog(QStringLiteral("RemoteDesktopWindow ctor device=%1 host=%2").arg(deviceName, hostIp)); // wjy: mark each remote desktop window creation.
    // ===end====
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_QuitOnClose, false);
    // =====wjy====
    setMinimumSize(100, 100); // wjy: 远控窗口允许缩放到 100x100，但继续阻止窗口缩小到无法操作标题栏和远控画面的尺寸。
    // ===end====
    // =====wjy====
    // wjy: 删除远控窗口 520x360 固定最小尺寸，让手动缩放和平铺布局可以按屏幕空间继续缩小。
    // ===end====
    const QRect savedGeometry = normalizedSavedWindowGeometry(
        platform::AppSettings::remoteDesktopWindowGeometry(m_hostIp),
        minimumSize());
    if (savedGeometry.isValid()) {
        setGeometry(savedGeometry);
    } else {
        // =====wjy====
        QScreen* initialScreen = QGuiApplication::screenAt(QCursor::pos()); // wjy: 首次远控优先显示在鼠标当前所在屏幕，方便多显示器环境直接操作。
        if (!initialScreen) {
            initialScreen = QGuiApplication::primaryScreen(); // wjy: 无法按鼠标定位屏幕时回退到主屏幕，确保始终有稳定的居中基准。
        }
        const QRect availableGeometry = initialScreen
            ? initialScreen->availableGeometry()
            : QRect(0, 0, 1280, 720); // wjy: 使用扣除任务栏后的可用区域；极端无屏幕信息时采用安全的 720p 区域。
        constexpr int screenMargin = 64;
        const QSize maximumInitialSize(
            qMax(minimumWidth(), availableGeometry.width() - screenMargin),
            qMax(minimumHeight(), availableGeometry.height() - screenMargin)); // wjy: 四周预留空间，避免首次窗口紧贴屏幕边缘或遮住任务栏。
        QSize initialSize(1280, 720); // wjy: 首次远控默认使用更紧凑的 16:9 尺寸；已有记录的设备仍恢复上次大小。
        if (initialSize.width() > maximumInitialSize.width()
            || initialSize.height() > maximumInitialSize.height()) {
            initialSize.scale(maximumInitialSize, Qt::KeepAspectRatio); // wjy: 小屏幕按比例缩小，避免窗口超出可用显示区域。
        }
        QRect initialGeometry(QPoint(0, 0), initialSize);
        initialGeometry.moveCenter(availableGeometry.center()); // wjy: 首次窗口放在目标屏幕正中央。
        setGeometry(initialGeometry);
        // ===end====
    }
    setWindowTitle(m_deviceName);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    updateWindowMask();
    // =====wjy====
    m_clipboardSyncEnabled = platform::AppSettings::remoteClipboardSyncEnabled(); // wjy: 新窗口继承设置页默认剪切板同步开关。
    m_clipboardPollTimer = new QTimer(this);
    m_clipboardPollTimer->setInterval(350);
    connect(m_clipboardPollTimer, &QTimer::timeout, this, [this] {
        pushLocalClipboardIfNeeded(); // wjy: 轮询本机剪贴板，变化时经 data-channel 推到被控端。
    });
    if (m_clipboardSyncEnabled) {
        m_clipboardPollTimer->start();
    }
    // ===end====
    m_connectionStatus = zh("\xE5\x87\x86\xE5\xA4\x87\xE8\xBF\x9E\xE6\x8E\xA5");

    m_sessionClock.start();
    m_frameStatsClock.start(); // wjy: Start a lightweight in-memory FPS meter for the remote desktop overlay.
    m_framePresentTimer = new QTimer(this);
    m_framePresentTimer->setTimerType(Qt::PreciseTimer);
    m_framePresentTimer->setInterval(16);
    connect(m_framePresentTimer, &QTimer::timeout, this, &RemoteDesktopWindow::flushPendingRemoteFrame);
    m_framePresentTimer->start(); // wjy: Fixed latest-frame presentation prevents full-screen repaint pressure from building visible latency.
    m_texturePresenter = new D3D11FramePresenter(this);
    m_texturePresenter->setMouseMoveCallback([this](const QPoint& parentPosition, Qt::MouseButtons buttons) {
        m_hoveredPos = parentPosition;
        if (!isFullScreen()) {
            update(QRect(0, 0, width(), titleBarHeight())); // wjy: 共享纹理模式的鼠标移动只在普通窗口刷新标题栏。
        }
        sendRemoteMouseMove(parentPosition, buttons);
    });
    m_sessionTimer = new QTimer(this);
    m_sessionTimer->setInterval(1000);
    connect(m_sessionTimer, &QTimer::timeout, this, [this] {
        if (!isFullScreen()) {
            update(QRect(0, 0, width(), titleBarHeight())); // wjy: 会话计时只显示在普通标题栏。
        }
    });
    m_sessionTimer->start();

    // =====wjy====
    m_remoteUpdateTimer = new QTimer(this);
    m_remoteUpdateTimer->setInterval(250); // wjy: 250ms 只推进遮罩动画，网络状态查询限制为约每秒一次。
    connect(m_remoteUpdateTimer, &QTimer::timeout, this, [this] {
        if (!remoteUpdateActive() || m_closeInProgress) {
            return;
        }

        m_remoteUpdateSpinnerStep = (m_remoteUpdateSpinnerStep + 1) % 12;
        const qint64 elapsedMs = m_remoteUpdateClock.isValid() ? m_remoteUpdateClock.elapsed() : 0;
        if (m_remoteUpdateState != RemoteUpdateState::Failed && elapsedMs >= 5 * 60 * 1000) {
            m_remoteUpdateState = RemoteUpdateState::Failed;
            m_remoteUpdateFailure = QString::fromUtf8("等待目标设备更新完成超时，请稍后重新远控。");
            m_remoteUpdateReconnectRequested = false;
            m_remoteUpdateTimer->stop();
            stopViewerConnectionAsync(false); // wjy: 超时后停止仍在尝试的连接，避免后台继续占用目标会话资源。
        } else if (m_remoteUpdateState != RemoteUpdateState::Failed
            && m_remoteUpdateState != RemoteUpdateState::Reconnecting
            && !m_remoteUpdateProbeInProgress
            && elapsedMs >= m_nextRemoteUpdateProbeAtMs) {
            pollRemoteUpdateStatus(); // wjy: 准备和安装阶段轮询目标端，重新连接阶段由流回调确认画面恢复。
        }
        update(isFullScreen() ? rect() : QRect(0, titleBarHeight(), width(), height() - titleBarHeight()));
    });

    // ===end====

    QTimer::singleShot(0, this, &RemoteDesktopWindow::startViewerConnection);
}

RemoteDesktopWindow::~RemoteDesktopWindow()
{
    // =====wjy====
    appendViewerDebugLog(QStringLiteral("RemoteDesktopWindow dtor begin")); // wjy: identify crashes during window teardown.
    // ===end====
    if (m_remoteUpdateTimer) {
        m_remoteUpdateTimer->stop(); // wjy: 窗口析构后停止更新状态轮询和遮罩动画。
    }
    setRemoteMouseCaptureActive(false);
    releasePressedKeys();
    setKeyboardForwardingActive(false);
    if (m_viewerHandle) {
        stream::StreamRuntime::instance().stop(m_viewerHandle);
        m_viewerHandle = nullptr;
    }
    if (m_texturePresenter) {
        m_texturePresenter->reset();
    }
    appendViewerDebugLog(QStringLiteral("RemoteDesktopWindow dtor end")); // wjy: teardown completed.
}

// =====wjy====
bool RemoteDesktopWindow::event(QEvent* event)
{
    const bool windowStateChanged = event && event->type() == QEvent::WindowStateChange; // wjy: 监听 Ctrl+D 触发的窗口状态切换，让标题栏和画面区域在全屏/普通窗口之间同步更新。
    if (event && event->type() == QEvent::WindowActivate) {
        emit activated(this);
    }

    const bool handled = QWidget::event(event);
    if (windowStateChanged) {
        updateWindowMask(); // wjy: 进入全屏时清除圆角遮罩，退出全屏时恢复普通窗口圆角。
        updateTexturePresenterGeometry(); // wjy: 纹理直呈模式也要立即扩展到新的远控画面区域。
        update(); // wjy: 状态改变后重绘，确保旧标题栏不会残留在全屏画面顶部。
    }
    return handled;
}
// ===end====

bool RemoteDesktopWindow::isWaitingShortcutRelease() const
{
    return m_waitingShortcutRelease;
}

// =====wjy====
QString RemoteDesktopWindow::hostIp() const
{
    return m_hostIp.trimmed(); // wjy: 更新操作按窗口固定目标 IP 匹配，不跟随主界面选择变化。
}

void RemoteDesktopWindow::setRemoteUpdateAvailable(bool available)
{
    const bool normalizedAvailable = available && !remoteUpdateActive(); // wjy: 已进入更新遮罩后不再保留标题栏入口，避免重复点击同一任务。
    if (m_remoteUpdateAvailable == normalizedAvailable) {
        return;
    }
    m_remoteUpdateAvailable = normalizedAvailable;
    if (!isFullScreen()) {
        update(QRect(0, 0, width(), titleBarHeight())); // wjy: 版本状态变化只重绘标题栏，不刷新正在播放的远控画面。
    }
}

bool RemoteDesktopWindow::isRemoteUpdateActive() const
{
    return remoteUpdateActive(); // wjy: 仅向设备列表公开只读更新状态，更新阶段的具体状态机仍由远控窗口内部维护。
}

bool RemoteDesktopWindow::remoteUpdateActive() const
{
    return m_remoteUpdateState != RemoteUpdateState::None;
}

bool RemoteDesktopWindow::remoteUpdateAcceptsFrames() const
{
    return m_remoteUpdateState == RemoteUpdateState::None
        || m_remoteUpdateState == RemoteUpdateState::Reconnecting; // wjy: 更新准备和安装阶段丢弃旧帧，重连阶段才接受首帧。
}

QString RemoteDesktopWindow::remoteUpdateTitle() const
{
    if (m_remoteUpdateState == RemoteUpdateState::Reconnecting) {
        return QString::fromUtf8("更新完成，正在恢复远控");
    }
    if (m_remoteUpdateState == RemoteUpdateState::Failed) {
        return QString::fromUtf8("更新失败");
    }
    return QString::fromUtf8("正在更新中");
}

QString RemoteDesktopWindow::remoteUpdateDetail() const
{
    switch (m_remoteUpdateState) {
    case RemoteUpdateState::Preparing:
        return QString::fromUtf8("目标设备正在准备和校验更新文件");
    case RemoteUpdateState::Installing:
        return QString::fromUtf8("目标程序正在替换文件并重新启动");
    case RemoteUpdateState::Reconnecting:
        return QString::fromUtf8("目标设备已重新上线，正在等待远程画面");
    case RemoteUpdateState::Failed:
        return m_remoteUpdateFailure.trimmed().isEmpty()
            ? QString::fromUtf8("目标设备未能完成更新，请稍后重试")
            : m_remoteUpdateFailure.trimmed();
    case RemoteUpdateState::None:
    default:
        return {};
    }
}

void RemoteDesktopWindow::beginRemoteUpdateWait()
{
    if (m_closeInProgress || m_hostIp.trimmed().isEmpty()) {
        return;
    }

    setRemoteMouseCaptureActive(false); // wjy: 切换状态前先向旧连接释放鼠标和按键，防止目标端残留输入状态。
    releaseForwardedKeys();
    m_waitingShortcutRelease = false;
    m_shortcutReleaseVirtualKeys.clear(); // wjy: 更新开始后不再等待本地组合键释放，确保键盘 hook 可以立即卸载。
    setKeyboardForwardingActive(false);
    if (m_clipboardPollTimer) {
        m_clipboardPollTimer->stop();
    }

    m_remoteUpdateAvailable = false; // wjy: 请求被目标端受理后立即隐藏更新按钮，由中央遮罩接管反馈。
    m_remoteUpdateState = RemoteUpdateState::Preparing;
    m_remoteUpdateFailure.clear();
    m_remoteUpdateClock.restart();
    m_nextRemoteUpdateProbeAtMs = 0;
    m_remoteUpdateProbeInProgress = false;
    m_remoteUpdateReconnectRequested = false;
    m_remoteUpdateSpinnerStep = 0;
    ++m_remoteUpdateGeneration;
    m_textureFrameActive = false;
    if (m_texturePresenter) {
        m_texturePresenter->hide(); // wjy: D3D 子窗口位于父窗口之上，更新时隐藏才能看见中央遮罩。
    }
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        m_pendingRemoteFrame = QImage(); // wjy: 清掉更新开始前排队的旧帧，避免重连前闪回旧桌面。
    }
    if (m_remoteUpdateTimer) {
        m_remoteUpdateTimer->start();
    }
    pollRemoteUpdateStatus(); // wjy: accepted 后立即查询准备状态，目标端失败时不用等待超时。
    update();
}

void RemoteDesktopWindow::pollRemoteUpdateStatus()
{
    if (!remoteUpdateActive() || m_remoteUpdateState == RemoteUpdateState::Failed
        || m_remoteUpdateProbeInProgress || m_closeInProgress) {
        return;
    }

    m_remoteUpdateProbeInProgress = true;
    m_nextRemoteUpdateProbeAtMs = (m_remoteUpdateClock.isValid() ? m_remoteUpdateClock.elapsed() : 0) + 1000;
    const QString targetIp = m_hostIp.trimmed();
    const int updateGeneration = m_remoteUpdateGeneration;
    QPointer<RemoteDesktopWindow> self(this);
    std::thread([self, targetIp, updateGeneration] {
        QString errorMessage;
        const platform::RemoteUpdateStatus status =
            platform::DeviceCommandService::queryUpdateStatus(targetIp, &errorMessage);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, status, errorMessage, updateGeneration] {
            if (!self) {
                return;
            }
            RemoteDesktopWindow* window = self.data();
            if (window->m_remoteUpdateGeneration != updateGeneration) {
                return; // wjy: 用户重新发起更新后忽略上一轮网络线程的迟到结果。
            }
            window->m_remoteUpdateProbeInProgress = false;
            if (!window->remoteUpdateActive() || window->m_remoteUpdateState == RemoteUpdateState::Failed
                || window->m_closeInProgress) {
                return;
            }

            switch (status) {
            case platform::RemoteUpdateStatus::Preparing:
            case platform::RemoteUpdateStatus::Idle:
            case platform::RemoteUpdateStatus::Unsupported:
                window->m_remoteUpdateState = RemoteUpdateState::Preparing; // wjy: 旧客户端不支持查询时继续等待其断线，兼容第一次升级。
                break;
            case platform::RemoteUpdateStatus::Unreachable:
                window->m_remoteUpdateState = RemoteUpdateState::Installing;
                window->stopViewerConnectionAsync(false); // wjy: 命令端口消失说明主进程已退出，停止旧流但保留窗口。
                break;
            case platform::RemoteUpdateStatus::Complete:
                window->m_remoteUpdateState = RemoteUpdateState::Reconnecting;
                window->m_remoteUpdateReconnectRequested = true;
                if (window->m_viewerHandle || window->m_viewerStopInProgress) {
                    window->stopViewerConnectionAsync(false); // wjy: 旧会话完全停止后才连接重启后的新进程。
                } else {
                    window->startViewerAfterUpdate();
                }
                break;
            case platform::RemoteUpdateStatus::Failed:
                window->m_remoteUpdateState = RemoteUpdateState::Failed;
                window->m_remoteUpdateFailure = errorMessage.trimmed();
                window->m_remoteUpdateReconnectRequested = false;
                if (window->m_remoteUpdateTimer) {
                    window->m_remoteUpdateTimer->stop();
                }
                break;
            }
            window->update();
        }, Qt::QueuedConnection);
    }).detach();
}

void RemoteDesktopWindow::stopViewerConnectionAsync(bool deleteAfterStop)
{
    if (deleteAfterStop) {
        m_deleteAfterViewerStop = true;
    }
    if (m_viewerStopInProgress) {
        return;
    }

    const FsRemoteStreamHandle handle = m_viewerHandle;
    m_viewerHandle = nullptr;
    if (!handle) {
        if (m_deleteAfterViewerStop || m_closeInProgress) {
            deleteLater();
        } else if (m_remoteUpdateReconnectRequested) {
            startViewerAfterUpdate();
        }
        return;
    }

    m_viewerStopInProgress = true;
    QPointer<RemoteDesktopWindow> self(this);
    std::thread([self, handle] {
        stream::StreamRuntime::instance().stop(handle);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self] {
            if (!self) {
                return;
            }
            RemoteDesktopWindow* window = self.data();
            window->m_viewerStopInProgress = false;
            if (window->m_deleteAfterViewerStop || window->m_closeInProgress) {
                window->deleteLater();
                return;
            }
            if (window->m_remoteUpdateReconnectRequested) {
                window->startViewerAfterUpdate(); // wjy: stop 返回后再创建新 viewer，避免旧回调污染新会话。
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void RemoteDesktopWindow::startViewerAfterUpdate()
{
    if (m_closeInProgress || m_remoteUpdateState == RemoteUpdateState::Failed
        || m_remoteUpdateState == RemoteUpdateState::None) {
        return;
    }
    if (m_viewerStopInProgress) {
        m_remoteUpdateReconnectRequested = true;
        return;
    }
    if (m_viewerHandle) {
        m_remoteUpdateReconnectRequested = true;
        stopViewerConnectionAsync(false);
        return;
    }

    m_remoteUpdateReconnectRequested = false;
    m_remoteUpdateState = RemoteUpdateState::Reconnecting;
    m_texturePresentFailed.store(false);
    startViewerConnection();
    if (!m_viewerHandle) {
        m_remoteUpdateState = RemoteUpdateState::Installing;
        m_nextRemoteUpdateProbeAtMs = 0; // wjy: 流句柄创建失败时回到状态查询，不关闭远控窗口。
    }
    update();
}

void RemoteDesktopWindow::finishRemoteUpdateWait()
{
    if (m_remoteUpdateState != RemoteUpdateState::Reconnecting) {
        return;
    }

    m_remoteUpdateState = RemoteUpdateState::None;
    m_remoteUpdateFailure.clear();
    m_remoteUpdateReconnectRequested = false;
    m_remoteUpdateProbeInProgress = false;
    if (m_remoteUpdateTimer) {
        m_remoteUpdateTimer->stop();
    }
    if (m_clipboardSyncEnabled && m_clipboardPollTimer) {
        m_clipboardPollTimer->start();
    }
    if (isActiveWindow()) {
        setKeyboardForwardingActive(true); // wjy: 首帧真正到达后才恢复键盘和剪贴板转发。
    }
    update();
}
// ===end====

// =====wjy====
int RemoteDesktopWindow::titleBarHeight() const
{
    return isFullScreen() ? 0 : 28; // wjy: 与主窗口 kTitleBarHeight=28 对齐，全屏时不占画面。
}

QRect RemoteDesktopWindow::remoteUpdateButtonRect() const
{
    const QRect clipboardRect = clipboardSyncRect();
    return QRect(clipboardRect.left() - 58, 3, 54, qMax(0, titleBarHeight() - 6)); // wjy: 54px 蓝色按钮放在剪切板左侧并保留 4px 间距，对齐用户标出的区域。
}

QRect RemoteDesktopWindow::clipboardSyncRect() const
{
    return QRect(width() - 160, 0, 28, titleBarHeight()); // wjy: 剪切板同步按钮固定在最小化左侧。
}

QRect RemoteDesktopWindow::minimizeRect() const
{
    return QRect(width() - 120, 0, 36, titleBarHeight()); // wjy: 三个窗口按钮改为连续靠右布局，36px 热区比旧 32px 更容易识别和点击。
}

QRect RemoteDesktopWindow::maximizeRect() const
{
    return QRect(width() - 84, 0, 36, titleBarHeight()); // wjy: 最大化紧接最小化，不再保留视觉上不规则的空档。
}

QRect RemoteDesktopWindow::closeRect() const
{
    return QRect(width() - 48, 0, 48 - kWindowResizeMargin, titleBarHeight()); // wjy: 关闭按钮右侧留出 6px 缩放空隙，拖动右边缘时不会落入关闭响应区。
}

bool RemoteDesktopWindow::isTitleBarBlankArea(const QPoint& position) const
{
    return !isFullScreen()
        && position.y() >= 0
        && position.y() < titleBarHeight()
        && !(m_remoteUpdateAvailable && remoteUpdateButtonRect().contains(position)) // wjy: 更新按钮可见时从拖动、双击和右键空白区中排除。
        && !clipboardSyncRect().contains(position)
        && !minimizeRect().contains(position)
        && !maximizeRect().contains(position)
        && !closeRect().contains(position);
}

void RemoteDesktopWindow::toggleMaximizedState()
{
    isMaximized() ? showNormal() : showMaximized();
    saveWindowGeometry();
}

void RemoteDesktopWindow::setClipboardSyncEnabled(bool enabled)
{
    if (m_clipboardSyncEnabled == enabled) {
        return;
    }
    m_clipboardSyncEnabled = enabled;
    if (m_clipboardPollTimer) {
        if (enabled) {
            m_clipboardPollTimer->start();
            pushLocalClipboardIfNeeded();
        } else {
            m_clipboardPollTimer->stop();
        }
    }
    update(QRect(0, 0, width(), titleBarHeight()));
}

bool RemoteDesktopWindow::isClipboardSyncEnabled() const
{
    return m_clipboardSyncEnabled;
}

void RemoteDesktopWindow::toggleClipboardSync()
{
    setClipboardSyncEnabled(!m_clipboardSyncEnabled);
    platform::AppSettings::setRemoteClipboardSyncEnabled(m_clipboardSyncEnabled);
}

void RemoteDesktopWindow::pushLocalClipboardIfNeeded()
{
    if (!m_clipboardSyncEnabled || !m_viewerHandle || m_closeInProgress) {
        return;
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        return;
    }
    const QString text = clipboard->text();
    if (text.isEmpty() || text == m_lastLocalClipboardText || text == m_lastAppliedRemoteClipboardText) {
        return; // wjy: 空内容、未变化或刚从远端写入的内容不回推，避免环路。
    }
    if (text.size() > 512 * 1024) {
        return;
    }
    m_lastLocalClipboardText = text;
    const QByteArray encoded = text.toUtf8().toBase64();
    sendInputMessage(QByteArray("cb ") + encoded); // wjy: Viewer -> Host 文本剪贴板同步。
}

void RemoteDesktopWindow::applyRemoteClipboardPayload(const QString& encodedBase64)
{
    if (!m_clipboardSyncEnabled) {
        return;
    }
    const QByteArray decoded = QByteArray::fromBase64(encodedBase64.toLatin1());
    if (decoded.isEmpty()) {
        return;
    }
    const QString text = QString::fromUtf8(decoded);
    if (text.isEmpty() || text == m_lastAppliedRemoteClipboardText) {
        return;
    }
    m_lastAppliedRemoteClipboardText = text;
    m_lastLocalClipboardText = text;
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text); // wjy: Host -> Viewer 文本剪贴板落地。
    }
}
// ===end====

QRect RemoteDesktopWindow::remoteImageRect() const
{
    const QSize remoteSize = remoteFrameSize();
    if (!remoteSize.isValid()) {
        return {};
    }
    const int barHeight = titleBarHeight();
    const QRect contentRect(0, barHeight, width(), qMax(0, height() - barHeight));
    const QSize scaled = remoteSize.scaled(contentRect.size(), Qt::KeepAspectRatio);
    return QRect(
        contentRect.x() + (contentRect.width() - scaled.width()) / 2,
        contentRect.y() + (contentRect.height() - scaled.height()) / 2,
        scaled.width(),
        scaled.height());
}

QSize RemoteDesktopWindow::remoteFrameSize() const
{
    if (!m_remoteFrame.isNull()) {
        return m_remoteFrame.size();
    }
    if (m_textureFrameActive && m_remoteTextureSize.isValid()) {
        return m_remoteTextureSize;
    }
    return {};
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

void RemoteDesktopWindow::setRememberGeometryEnabled(bool enabled)
{
    m_rememberGeometry = enabled;
}

void RemoteDesktopWindow::saveWindowGeometry()
{
    if (!m_rememberGeometry || isMinimized()) {
        return;
    }
    platform::AppSettings::setRemoteDesktopWindowGeometry(m_hostIp, frameGeometry());
}

void RemoteDesktopWindow::sendInputMessage(const QByteArray& message)
{
    if (remoteUpdateActive()) {
        return; // wjy: 更新遮罩期间统一阻断鼠标、键盘和剪贴板消息，避免发送到正在退出或刚重启的目标进程。
    }
    const bool shouldLog = shouldLogInputMessage(message);
    const bool ok = stream::StreamRuntime::instance().sendInput(m_viewerHandle, message);
    if (shouldLog) {
        appendInputDebugLog(QStringLiteral("viewer send handle=%1 ok=%2 msg=\"%3\"")
            .arg(reinterpret_cast<quintptr>(m_viewerHandle))
            .arg(ok ? 1 : 0)
            .arg(QString::fromLatin1(message)));
    }
}

void RemoteDesktopWindow::setRemoteMouseCaptureActive(bool active)
{
    if (active && remoteUpdateActive()) {
        return; // wjy: 更新过程中忽略旧流迟到的 relative mouse 状态，保持本机鼠标可见可操作。
    }
    if (m_remoteMouseCaptureActive == active) {
        if (active) {
            recenterRemoteMouseCapture();
        }
        return;
    }

    m_remoteMouseCaptureActive = active;
    if (active) {
        setCursor(Qt::BlankCursor);
        if (m_texturePresenter) {
            m_texturePresenter->setCursor(Qt::BlankCursor);
        }
        recenterRemoteMouseCapture();
    } else {
        unsetCursor();
        if (m_texturePresenter) {
            m_texturePresenter->unsetCursor();
        }
        sendInputMessage(QByteArray("c 0"));
    }
}

bool RemoteDesktopWindow::sendRemoteMouseMove(const QPoint& position, Qt::MouseButtons buttons)
{
    if (m_remoteMouseCaptureActive) {
        return sendRemoteMouseRelativeMove(position, buttons);
    }

    int x = 0;
    int y = 0;
    if (!normalizedRemotePoint(position, &x, &y)) {
        return false;
    }

    const int remoteButtons =
        (buttons & Qt::LeftButton ? 1 : 0) |
        (buttons & Qt::RightButton ? 2 : 0) |
        (buttons & Qt::MiddleButton ? 4 : 0);
    sendInputMessage(QByteArray("m ")
        + QByteArray::number(x) + ' '
        + QByteArray::number(y) + ' '
        + QByteArray::number(remoteButtons));
    return true;
}

bool RemoteDesktopWindow::sendRemoteMouseRelativeMove(const QPoint& position, Qt::MouseButtons buttons)
{
    const QRect imageRect = remoteImageRect();
    if (!imageRect.isValid()) {
        return false;
    }

    const QPoint center = imageRect.center();
    const QPoint delta = position - center;
    if (delta.isNull()) {
        return true;
    }

    const QSize remoteSize = remoteFrameSize();
    int dx = delta.x();
    int dy = delta.y();
    if (remoteSize.isValid() && imageRect.width() > 0 && imageRect.height() > 0) {
        dx = qRound(double(delta.x()) * double(remoteSize.width()) / double(imageRect.width()));
        dy = qRound(double(delta.y()) * double(remoteSize.height()) / double(imageRect.height()));
    }

    const int remoteButtons =
        (buttons & Qt::LeftButton ? 1 : 0) |
        (buttons & Qt::RightButton ? 2 : 0) |
        (buttons & Qt::MiddleButton ? 4 : 0);
    sendInputMessage(QByteArray("r ")
        + QByteArray::number(dx) + ' '
        + QByteArray::number(dy) + ' '
        + QByteArray::number(remoteButtons));
    recenterRemoteMouseCapture();
    return true;
}

void RemoteDesktopWindow::recenterRemoteMouseCapture()
{
#if defined(Q_OS_WIN)
    if (!m_remoteMouseCaptureActive || !isActiveWindow()) {
        return;
    }
    const QRect imageRect = remoteImageRect();
    if (!imageRect.isValid()) {
        return;
    }
    QCursor::setPos(mapToGlobal(imageRect.center()));
#endif
}

void RemoteDesktopWindow::setKeyboardForwardingActive(bool active)
{
#if defined(Q_OS_WIN)
    if (m_waitingShortcutRelease && !m_closeInProgress) {
        installKeyboardHook(this); // wjy: 等待组合键松开时保留 hook，但 hook 只记录 keyup，不转发远端。
    } else if (active && !m_closeInProgress && !remoteUpdateActive() && isActiveWindow()) {
        installKeyboardHook(this);
    } else {
        uninstallKeyboardHook(this);
    }
#else
    Q_UNUSED(active)
#endif
}

void RemoteDesktopWindow::beginShortcutReleaseGuard(const QKeySequence& shortcut)
{
    releasePressedKeys();
#if defined(Q_OS_WIN)
    m_shortcutReleaseVirtualKeys = shortcutVirtualKeys(shortcut);
    m_waitingShortcutRelease = true;
    installKeyboardHook(this); // wjy: 不卸载 hook，否则 Ctrl 按下被吞后就无法可靠观察它何时松开。
    for (int virtualKey : m_shortcutReleaseVirtualKeys) {
        updateTrackedHookKey(virtualKey, true); // wjy: Qt 按键路径触发快捷键时，也把这组实际按下的键同步进 hook 状态。
    }
#else
    Q_UNUSED(shortcut)
#endif
}

void RemoteDesktopWindow::updateShortcutReleaseGuard()
{
#if defined(Q_OS_WIN)
    if (!m_waitingShortcutRelease) {
        return;
    }

    bool allReleased = true;
    for (int virtualKey : m_shortcutReleaseVirtualKeys) {
        if (isTrackedHookKeyDown(virtualKey)) {
            allReleased = false;
            break;
        }
    }
    if (!allReleased) {
        return;
    }

    m_waitingShortcutRelease = false;
    m_shortcutReleaseVirtualKeys.clear();
    if (!m_closeInProgress && isActiveWindow()) {
        setKeyboardForwardingActive(true);
    } else {
        setKeyboardForwardingActive(false);
    }
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

void RemoteDesktopWindow::releaseForwardedShortcutKeys(const QVector<int>& virtualKeys)
{
    QSet<int> releasedKeys;
    for (int key : virtualKeys) {
        if (key <= 0 || releasedKeys.contains(key)) {
            continue;
        }
        releasedKeys.insert(key);
        m_pressedKeys.remove(key);
        sendInputMessage(QByteArray("k ")
            + QByteArray::number(key) + " 0");
    }
}

void RemoteDesktopWindow::releaseForwardedKeys()
{
    releasePressedKeys();
#if defined(Q_OS_WIN)
    const int modifierKeys[] = {
        VK_CONTROL,
        VK_LCONTROL,
        VK_RCONTROL,
        VK_SHIFT,
        VK_LSHIFT,
        VK_RSHIFT,
        VK_MENU,
        VK_LMENU,
        VK_RMENU,
        VK_LWIN,
        VK_RWIN,
    };
    for (int key : modifierKeys) {
        sendInputMessage(QByteArray("k ")
            + QByteArray::number(key) + " 0");
    }
#endif
}

void RemoteDesktopWindow::shutdownForApplicationExit()
{
    saveWindowGeometry();
    m_closeInProgress = true;
    m_remoteUpdateState = RemoteUpdateState::None;
    m_remoteUpdateReconnectRequested = false;
    if (m_remoteUpdateTimer) {
        m_remoteUpdateTimer->stop(); // wjy: 控制端自身退出时终止远端更新轮询，不再尝试恢复窗口。
    }
    m_waitingShortcutRelease = false;
    m_shortcutReleaseVirtualKeys.clear();
    hide();
    setRemoteMouseCaptureActive(false);
    releaseForwardedKeys();
    setKeyboardForwardingActive(false);
    if (m_framePresentTimer) {
        m_framePresentTimer->stop();
    }
    if (m_sessionTimer) {
        m_sessionTimer->stop();
    }
    if (m_texturePresenter) {
        m_texturePresenter->reset();
    }

    const FsRemoteStreamHandle handle = m_viewerHandle;
    m_viewerHandle = nullptr;
    if (handle) {
        stream::StreamRuntime::instance().stop(handle);
    }
}

int RemoteDesktopWindow::remoteButton(Qt::MouseButton button) const
{
    if (button == Qt::LeftButton) return 1;
    if (button == Qt::RightButton) return 2;
    if (button == Qt::MiddleButton) return 4;
    return 0;
}

// =====wjy====
// wjy: 标题栏虚拟屏标签和“+”入口已删除，对应的标签命中测试也一并移除，避免残留不可见点击热区。
// ===end====

int RemoteDesktopWindow::resizeEdgesAt(const QPoint& position) const
{
    if (isMaximized() || isFullScreen()) { // wjy: 全屏窗口不允许边缘缩放，顶部和边缘的鼠标事件全部交给远端桌面。
        return ResizeNone;
    }

    int edges = ResizeNone;
    if (position.x() <= kWindowResizeMargin) {
        edges |= ResizeLeft;
    } else if (position.x() >= width() - kWindowResizeMargin) {
        edges |= ResizeRight;
    }

    if (position.y() <= kWindowResizeMargin) {
        edges |= ResizeTop;
    } else if (position.y() >= height() - kWindowResizeMargin) {
        edges |= ResizeBottom;
    }
    return edges;
}

void RemoteDesktopWindow::updateResizeCursor(const QPoint& position)
{
    if (!isFullScreen() && m_remoteUpdateAvailable && remoteUpdateButtonRect().contains(position)) {
        setCursor(Qt::PointingHandCursor);
        return; // wjy: 更新入口使用手型指针；按钮远离 6px 缩放边缘，不影响窗口缩放命中。
    }
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
    if (isFullScreen()) {
        clearMask(); // wjy: 全屏必须使用完整矩形窗口，避免普通窗口的圆角裁掉屏幕边缘像素。
        return;
    }

    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, width(), height()), 6, 6);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
}

void RemoteDesktopWindow::updateTexturePresenterGeometry()
{
    if (!m_texturePresenter) {
        return;
    }
    const QRect target = remoteImageRect();
    if (target.isEmpty() || !m_textureFrameActive) {
        m_texturePresenter->hide();
        return;
    }
    if (m_texturePresenter->geometry() != target) {
        m_texturePresenter->setGeometry(target);
    }
}

void RemoteDesktopWindow::enqueueRemoteFrame(QImage image)
{
    if (image.isNull()) {
        return;
    }

    QMutexLocker locker(&m_pendingFrameMutex);
    m_pendingRemoteFrame = std::move(image); // wjy: Overwrite stale frames; the UI timer will pull the newest one when the event loop is ready.
}

bool RemoteDesktopWindow::enqueueRemoteTextureFrame(int width, int height, void* sharedHandle, quint64 frameId, double encodedMbps)
{
    Q_UNUSED(frameId)
    if (m_closeInProgress || m_texturePresentFailed.load() || width <= 0 || height <= 0 || !sharedHandle) {
        return false;
    }

    QMetaObject::invokeMethod(this, [this, width, height, sharedHandle, encodedMbps] {
        if (isClosingConnection() || m_texturePresentFailed.load() || !m_texturePresenter
            || !remoteUpdateAcceptsFrames()) {
            return;
        }
        m_remoteTextureSize = QSize(width, height);
        m_textureFrameActive = true;
        m_remoteFrame = QImage();
        m_encodedMbps = qMax(0.0, encodedMbps);
        updateTexturePresenterGeometry();
        if (!m_texturePresenter->presentSharedTexture(sharedHandle, width, height)) {
            m_texturePresentFailed.store(true);
            m_textureFrameActive = false;
            m_texturePresenter->hide();
            m_texturePresenter->reset();
            update(); // wjy: 全屏画面从 y=0 开始，纹理呈现失败后重绘整窗以清掉可能残留的旧标题栏区域。
            return;
        }
        m_connectionStatusCode = 50;
        m_connectionStatus = QString::fromUtf8("画面已接收");
        m_texturePresenter->show();
        m_texturePresenter->raise();
        if (m_remoteUpdateState == RemoteUpdateState::Reconnecting) {
            finishRemoteUpdateWait(); // wjy: 共享纹理首帧成功呈现后才移除更新遮罩并恢复输入。
        }
        update(QRect(0, 0, this->width(), titleBarHeight()));
    }, Qt::QueuedConnection);
    return true;
}

void RemoteDesktopWindow::flushPendingRemoteFrame()
{
    QImage image;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        image = std::move(m_pendingRemoteFrame);
        m_pendingRemoteFrame = QImage();
    }

    if (isClosingConnection() || image.isNull() || !remoteUpdateAcceptsFrames()) {
        return;
    }

    setRemoteFrame(image);
}

void RemoteDesktopWindow::setRemoteFrame(const QImage& image)
{
    if (!remoteUpdateAcceptsFrames()) {
        return; // wjy: 目标更新期间忽略旧连接残留帧，避免遮罩后面继续变化或误判更新完成。
    }
    // =====wjy====
    // appendViewerDebugLog(QStringLiteral("setRemoteFrame enter size=%1x%2").arg(image.width()).arg(image.height())); // wjy: per-frame UI log disabled for smoother rendering.
    // ===end====
    updateFrameStats(image); // wjy: Count received UI frames before repainting so the overlay reflects the latest decoded BGRA flow.
    m_textureFrameActive = false;
    if (m_texturePresenter) {
        m_texturePresenter->hide();
    }
    m_remoteFrame = image;
    m_connectionStatusCode = 50;
    m_connectionStatus = QString::fromUtf8("画面已接收");
    if (m_remoteUpdateState == RemoteUpdateState::Reconnecting) {
        finishRemoteUpdateWait(); // wjy: 软件帧路径同样以首帧到达作为恢复正常远控的唯一完成点。
    }
    update(isFullScreen() ? rect() : QRect(0, titleBarHeight(), width(), height() - titleBarHeight())); // wjy: 普通窗口只刷新标题栏下方远控画面。
    // appendViewerDebugLog(QStringLiteral("setRemoteFrame update requested")); // wjy: per-frame repaint log disabled to avoid disk IO on every frame.
}

void RemoteDesktopWindow::updateFrameStats(const QImage& image)
{
    if (image.isNull()) {
        return;
    }
    updateFrameColorStats(image); // wjy: Keep RGB diagnostics visible while testing pure-black remote pages.
    if (!m_frameStatsClock.isValid()) {
        m_frameStatsClock.start(); // wjy: Defensive start in case a frame arrives before the constructor timer state is valid.
    }

    ++m_frameStatsCount; // wjy: Count frames that reached the Qt/UI handoff, which is the number users actually care about for visual smoothness.
    m_frameStatsBytes += static_cast<qint64>(image.sizeInBytes()); // wjy: Track decoded BGRA bytes, not compressed WebRTC network bitrate.
    const qint64 elapsedMs = m_frameStatsClock.elapsed();
    if (elapsedMs < 1000) {
        return;
    }

    m_receiveFps = m_frameStatsCount * 1000.0 / double(elapsedMs); // wjy: Smooth FPS over roughly one second to avoid noisy per-frame values.
    m_rawBgraMbps = m_frameStatsBytes * 8.0 * 1000.0 / double(elapsedMs) / 1000000.0; // wjy: Raw decoded BGRA throughput helps identify UI/copy pressure separately from network bitrate.
    m_frameStatsCount = 0;
    m_frameStatsBytes = 0;
    m_frameStatsClock.restart();
}

void RemoteDesktopWindow::updateFrameColorStats(const QImage& image)
{
    if (image.isNull() || image.depth() < 24) {
        return;
    }

    // =====wjy====
    sampleFrameColorRegion(image, image.rect(), &m_rgbMin, &m_rgbAvg, &m_rgbMax); // wjy: Whole-frame values show the complete decoded desktop range.
    const int centerWidth = qMax(1, image.width() / 3); // wjy: Center region avoids browser chrome/taskbar when testing a full-screen black webpage.
    const int centerHeight = qMax(1, image.height() / 3);
    const QRect centerRegion(
        (image.width() - centerWidth) / 2,
        (image.height() - centerHeight) / 2,
        centerWidth,
        centerHeight);
    sampleFrameColorRegion(image, centerRegion, &m_centerRgbMin, &m_centerRgbAvg, &m_centerRgbMax); // wjy: CTR is the key value for checking whether pure black becomes gray in the video path.
    // ===end====
}

void RemoteDesktopWindow::sampleFrameColorRegion(const QImage& image, const QRect& region, int* minValue, int* avgValue, int* maxValue) const
{
    if (image.isNull() || image.depth() < 24 || !minValue || !avgValue || !maxValue) {
        return;
    }

    const QRect sampleRegion = region.intersected(image.rect());
    if (sampleRegion.isEmpty()) {
        return;
    }

    const int stepX = qMax(1, sampleRegion.width() / 64);
    const int stepY = qMax(1, sampleRegion.height() / 36);
    int minSample = 255;
    int maxSample = 0;
    qint64 sum = 0;
    qint64 count = 0;

    // =====wjy====
    for (int y = sampleRegion.top(); y <= sampleRegion.bottom(); y += stepY) {
        const uchar* row = image.constScanLine(y); // wjy: Remote frames are stored as 4-byte BGRA/RGB32 scanlines.
        for (int x = sampleRegion.left(); x <= sampleRegion.right(); x += stepX) {
            const uchar* pixel = row + x * 4; // wjy: Sample B/G/R from the decoded frame without using Qt color conversion helpers.
            const int b = pixel[0];
            const int g = pixel[1];
            const int r = pixel[2];
            minSample = qMin(minSample, qMin(r, qMin(g, b))); // wjy: Pure black should keep this close to 0.
            maxSample = qMax(maxSample, qMax(r, qMax(g, b))); // wjy: Pure white or bright UI should move this toward 255.
            sum += r + g + b;
            count += 3;
        }
    }
    // ===end====

    if (count <= 0) {
        return;
    }
    *minValue = minSample;
    *avgValue = static_cast<int>((sum + count / 2) / count);
    *maxValue = maxSample;
}

bool RemoteDesktopWindow::isClosingConnection() const
{
    return m_closeInProgress;
}

bool RemoteDesktopWindow::forwardNativeKey(int virtualKey, bool down)
{
    if (m_closeInProgress || remoteUpdateActive() || virtualKey <= 0) {
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

bool RemoteDesktopWindow::handleLocalShortcutKey(int virtualKey, Qt::KeyboardModifiers modifiers)
{
#if defined(Q_OS_WIN)
    if (m_closeInProgress || remoteUpdateActive()) {
        return false;
    }
    const QKeySequence current = shortcutSequenceFromNativeVirtualKey(virtualKey, modifiers); // wjy: 低级键盘钩子路径同样使用用户保存的快捷键设置。
    if (matchesShortcut(current, platform::AppSettings::remoteShortcutFullscreen())) {
        beginShortcutReleaseGuard(platform::AppSettings::remoteShortcutFullscreen());
        emit shortcutFullscreenRequested();
        return true;
    }
    if (matchesShortcut(current, platform::AppSettings::remoteShortcutTile())) {
        beginShortcutReleaseGuard(platform::AppSettings::remoteShortcutTile());
        emit shortcutTileRequested();
        return true;
    }
    if (matchesShortcut(current, platform::AppSettings::remoteShortcutCloseAll())) {
        beginShortcutReleaseGuard(platform::AppSettings::remoteShortcutCloseAll());
        emit shortcutCloseAllRequested();
        return true;
    }
    if (matchesShortcut(current, platform::AppSettings::remoteShortcutCloseTopmost())) {
        beginShortcutReleaseGuard(platform::AppSettings::remoteShortcutCloseTopmost());
        emit shortcutCloseTopmostRequested();
        return true;
    }
    // =====wjy====
    if (matchesShortcut(current, platform::AppSettings::remoteShortcutClipboardSync())) {
        beginShortcutReleaseGuard(platform::AppSettings::remoteShortcutClipboardSync());
        emit shortcutClipboardSyncRequested();
        return true;
    }
    // ===end====
#else
    Q_UNUSED(virtualKey)
    Q_UNUSED(modifiers)
#endif
    return false;
}

void RemoteDesktopWindow::startViewerConnection()
{
    // =====wjy====
    appendViewerDebugLog(QStringLiteral("startViewerConnection begin host=%1").arg(m_hostIp)); // wjy: mark when the UI asks the stream DLL to connect.
    // ===end====
    if (m_viewerHandle || m_viewerStopInProgress || m_closeInProgress) {
        return; // wjy: 更新重连必须等待旧 viewer 完全停止，禁止同一窗口同时持有两个流会话。
    }
    if (!m_hostIp.trimmed().isEmpty()) {
        m_viewerHandle = stream::StreamRuntime::instance().startViewer(
            m_hostIp.trimmed(),
            49100,
            onRemoteFrame,
            onRemoteTextureFrame,
            onViewerStatus,
            this);
        appendViewerDebugLog(QStringLiteral("startViewerConnection handle=%1").arg(reinterpret_cast<quintptr>(m_viewerHandle))); // wjy: record whether the DLL returned a handle.
        if (!m_viewerHandle) {
            setConnectionStatus(90, stream::StreamRuntime::instance().lastError());
        }
    } else {
        setConnectionStatus(90, QString::fromUtf8("设备 IP 为空"));
    }
}

void RemoteDesktopWindow::setConnectionStatus(int code, const QString& message)
{
    // =====wjy====
    if (remoteUpdateActive()) {
        if (m_remoteUpdateState == RemoteUpdateState::Reconnecting && code == 50) {
            finishRemoteUpdateWait();
            return;
        }
        if (code == 80 || code == 90) {
            m_remoteUpdateState = RemoteUpdateState::Installing;
            m_remoteUpdateReconnectRequested = false;
            m_nextRemoteUpdateProbeAtMs = 0;
            stopViewerConnectionAsync(false); // wjy: 更新过程中的断线属于目标主进程退出，保持窗口并转入安装等待。
        }
        update(isFullScreen() ? rect() : QRect(0, titleBarHeight(), width(), height() - titleBarHeight()));
        return; // wjy: 更新遮罩接管连接文字，不显示“连接失败/已断开”。
    }
    // ===end====
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
    update(isFullScreen() ? rect() : QRect(0, titleBarHeight(), width(), height() - titleBarHeight())); // wjy: 连接状态在全屏覆盖整窗。
}

void RemoteDesktopWindow::setEncodedBitrateMbps(double mbps)
{
    m_encodedMbps = qMax(0.0, mbps); // wjy: Clamp the overlay value because transient decoder stats should never draw negative bitrate.
    update(remoteImageRect());
}

void RemoteDesktopWindow::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool fullScreen = isFullScreen();

    painter.fillRect(rect(), QColor(QStringLiteral("#000000")));
    if (m_textureFrameActive && m_texturePresenter && m_texturePresenter->isVisible()) {
        updateTexturePresenterGeometry();
    } else if (!m_remoteFrame.isNull()) {
        const QRect target = remoteImageRect();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false); // wjy: Prefer low-latency full-screen remote drawing over expensive smooth scaling.
        painter.drawImage(target, m_remoteFrame);
        // =====wjy====
        const QStringList statLines = {
            QStringLiteral("UI %1 fps").arg(m_receiveFps, 0, 'f', 1),
            QStringLiteral("%1 x %2").arg(m_remoteFrame.width()).arg(m_remoteFrame.height()),
            QStringLiteral("ENC %1 Mbps").arg(m_encodedMbps, 0, 'f', 2),
            QStringLiteral("RAW %1 Mbps").arg(m_rawBgraMbps, 0, 'f', 1),
            QStringLiteral("RGB %1/%2/%3").arg(m_rgbMin).arg(m_rgbAvg).arg(m_rgbMax),
            QStringLiteral("CTR %1/%2/%3").arg(m_centerRgbMin).arg(m_centerRgbAvg).arg(m_centerRgbMax),
        }; // wjy: Show real compressed bitrate first; RAW stays as a local decoded-throughput diagnostic.
        QFont statFont(QStringLiteral("Consolas"));
        statFont.setPixelSize(12);
        painter.setFont(statFont);
        const QFontMetrics statMetrics(statFont);
        int statWidth = 0;
        for (const QString& line : statLines) {
            statWidth = qMax(statWidth, statMetrics.horizontalAdvance(line)); // wjy: Size the panel to the widest metric line so text never clips.
        }
        const int panelPaddingX = 10;
        const int panelPaddingY = 7;
        const int panelWidth = statWidth + panelPaddingX * 2;
        const int panelHeight = statLines.size() * statMetrics.height() + panelPaddingY * 2;
        const QRect statPanel(
            target.right() - panelWidth - 12,
            target.bottom() - panelHeight - 12,
            panelWidth,
            panelHeight); // wjy: Anchor stats to the remote image bottom-right so window chrome and letterboxing stay clear.
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 156));
        painter.drawRoundedRect(QRectF(statPanel), 5, 5);
        painter.setPen(QColor(QStringLiteral("#EAF2FF")));
        int textY = statPanel.top() + panelPaddingY + statMetrics.ascent();
        for (const QString& line : statLines) {
            painter.drawText(statPanel.left() + panelPaddingX, textY, line); // wjy: Draw one metric per line for quick reading during remote-control testing.
            textY += statMetrics.height();
        }
        // ===end====
    } else {
        const int barHeight = fullScreen ? 0 : titleBarHeight();
        const QRect contentRect(0, barHeight, width(), qMax(0, height() - barHeight));
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
    // =====wjy====
    if (remoteUpdateActive()) {
        const int contentTop = fullScreen ? 0 : titleBarHeight();
        const QRect contentRect(0, contentTop, width(), qMax(0, height() - contentTop));
        painter.fillRect(contentRect, QColor(3, 10, 22, 196)); // wjy: 保留窗口和最后画面位置，用半透明遮罩明确当前不可操作。

        const int cardWidth = qMin(380, qMax(240, contentRect.width() - 48));
        const int cardHeight = 136;
        const QRect card(
            contentRect.center().x() - cardWidth / 2,
            contentRect.center().y() - cardHeight / 2,
            cardWidth,
            cardHeight);
        painter.setPen(QPen(QColor(255, 255, 255, 28), 1));
        painter.setBrush(QColor(17, 27, 45, 238));
        painter.drawRoundedRect(QRectF(card), 12, 12);

        const QRect spinnerRect(card.center().x() - 15, card.top() + 20, 30, 30);
        if (m_remoteUpdateState == RemoteUpdateState::Failed) {
            painter.setPen(QPen(QColor(QStringLiteral("#FF7A7A")), 3));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(spinnerRect);
            painter.drawLine(spinnerRect.center().x(), spinnerRect.top() + 7,
                spinnerRect.center().x(), spinnerRect.bottom() - 9);
            painter.drawPoint(spinnerRect.center().x(), spinnerRect.bottom() - 5);
        } else {
            painter.setPen(QPen(QColor(255, 255, 255, 48), 4, Qt::SolidLine, Qt::RoundCap));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(spinnerRect);
            painter.setPen(QPen(QColor(QStringLiteral("#3A9BFF")), 4, Qt::SolidLine, Qt::RoundCap));
            const int startAngle = (90 - m_remoteUpdateSpinnerStep * 30) * 16;
            painter.drawArc(spinnerRect, startAngle, -210 * 16); // wjy: 轻量圆环随 250ms 定时器旋转，不阻塞更新状态查询。
        }

        QFont updateTitleFont(QStringLiteral("Microsoft YaHei UI"));
        updateTitleFont.setPixelSize(17);
        updateTitleFont.setWeight(QFont::DemiBold);
        painter.setFont(updateTitleFont);
        painter.setPen(QColor(QStringLiteral("#F4F8FF")));
        painter.drawText(QRect(card.left() + 20, card.top() + 58, card.width() - 40, 26),
            Qt::AlignCenter, remoteUpdateTitle());

        QFont updateDetailFont(QStringLiteral("Microsoft YaHei UI"));
        updateDetailFont.setPixelSize(12);
        painter.setFont(updateDetailFont);
        painter.setPen(m_remoteUpdateState == RemoteUpdateState::Failed
                ? QColor(QStringLiteral("#FFB4B4"))
                : QColor(QStringLiteral("#AEBBCD")));
        painter.drawText(QRect(card.left() + 26, card.top() + 91, card.width() - 52, 34),
            Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, remoteUpdateDetail());
    }
    // ===end====
    // =====wjy====
    if (fullScreen) {
        return; // wjy: Ctrl+D 进入全屏后只保留远控画面/连接提示，不绘制标题栏、边框和窗口控制按钮。
    }
    const int barH = titleBarHeight();
    // ===end====

    painter.fillRect(QRectF(0, 0, width(), barH), QColor(QStringLiteral("#E9EEF2")));

    painter.setPen(QPen(QColor(QStringLiteral("#AEB7C2")), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 6, 6);

    // =====wjy====
    // wjy: logo/文字/按钮都按 28px 标题栏垂直居中，贴近主窗口标题栏视觉。
    const int logoY = (barH - 16) / 2;
    painter.drawPixmap(QRect(12, logoY, 16, 16), icon(QStringLiteral("fs_session_logo.svg")));
    QFont textFont(QStringLiteral("Microsoft YaHei UI"));
    textFont.setPixelSize(12);
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#111820")));
    const int nameWidth = computerNameWidth(m_deviceName, textFont);
    painter.drawText(QRectF(34, 0, nameWidth, barH), Qt::AlignVCenter | Qt::AlignLeft, m_deviceName);

    const int nameRight = 34 + nameWidth;
    const int ipX = nameRight + 10;
    constexpr int elapsedTextWidth = 70;
    constexpr int elapsedGap = 14;
    const int titleTextRight = m_remoteUpdateAvailable
        ? remoteUpdateButtonRect().left()
        : clipboardSyncRect().left(); // wjy: 更新按钮出现时名称/IP/计时整体提前截止，防止文字覆盖按钮。
    const int maxIpWidth = qMax(0, titleTextRight - ipX - elapsedGap - elapsedTextWidth - 8);
    const QFontMetrics titleMetrics(textFont);
    const int ipWidth = qMin(titleMetrics.horizontalAdvance(m_hostIp), maxIpWidth);
    int elapsedX = ipX;
    if (ipWidth > 0) {
        painter.setPen(QColor(QStringLiteral("#667085")));
        painter.drawText(
            QRectF(ipX, 0, ipWidth, barH),
            Qt::AlignVCenter | Qt::AlignLeft,
            titleMetrics.elidedText(m_hostIp, Qt::ElideRight, ipWidth));
        elapsedX = ipX + ipWidth + elapsedGap;
    }

    const qint64 elapsedSeconds = m_sessionClock.elapsed() / 1000;
    const qint64 hours = elapsedSeconds / 3600;
    const qint64 minutes = (elapsedSeconds / 60) % 60;
    const qint64 seconds = elapsedSeconds % 60;
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#4B4B4C")));
    painter.drawText(
        QRectF(elapsedX, 0, elapsedTextWidth, barH),
        Qt::AlignVCenter | Qt::AlignLeft,
        QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0')));

    if (m_remoteUpdateAvailable) {
        const QRect updateRect = remoteUpdateButtonRect();
        const bool updateHovered = updateRect.contains(m_hoveredPos);
        painter.setPen(Qt::NoPen);
        painter.setBrush(updateHovered
                ? QColor(QStringLiteral("#2F6FE4"))
                : QColor(QStringLiteral("#3A7BFC"))); // wjy: 更新入口沿用主窗口蓝色，悬停时加深以明确点击反馈。
        painter.drawRoundedRect(QRectF(updateRect), 4, 4);
        QFont updateFont(QStringLiteral("Microsoft YaHei UI"));
        updateFont.setPixelSize(12);
        updateFont.setWeight(QFont::DemiBold);
        painter.setFont(updateFont);
        painter.setPen(QColor(QStringLiteral("#FFFFFF")));
        painter.drawText(updateRect, Qt::AlignCenter, QString::fromUtf8("更新")); // wjy: 只在目标端明确返回需要更新时显示文字按钮。
    }

    // wjy: 剪切板同步按钮在最小化左侧；开启用主蓝，关闭用灰色，中间画剪贴板简图。
    const QRect clipRect = clipboardSyncRect();
    const QColor clipAccent = m_clipboardSyncEnabled ? QColor(QStringLiteral("#3A7BFC")) : QColor(QStringLiteral("#9CA3AF"));
    painter.setPen(QPen(clipAccent, 1.2));
    painter.setBrush(m_clipboardSyncEnabled ? QColor(QStringLiteral("#EAF2FF")) : QColor(QStringLiteral("#F3F4F6")));
    painter.drawRoundedRect(QRectF(clipRect).adjusted(4, 5, -4, -5), 4, 4);
    painter.setPen(QPen(clipAccent, 1.2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(clipRect.center().x() - 4, clipRect.center().y() - 5, 8, 10), 1.5, 1.5);
    painter.drawLine(QPointF(clipRect.center().x() - 2, clipRect.center().y() - 2), QPointF(clipRect.center().x() + 2, clipRect.center().y() - 2));

    const auto drawTitleButton = [&](const QRect& hitRect, const QString& iconName, bool closeButton) {
        const bool hovered = hitRect.contains(m_hoveredPos); // wjy: 鼠标进入按钮热区后绘制背景，明确提示当前将操作哪个窗口按钮。
        if (hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(closeButton
                    ? QColor(QStringLiteral("#FCE8E6"))
                    : QColor(QStringLiteral("#DCE4EC"))); // wjy: 关闭使用浅红警示，其余按钮使用浅灰蓝，兼顾辨识度和当前标题栏配色。
            painter.drawRect(hitRect);
        }
        const QPixmap raw = icon(iconName);
        if (raw.isNull()) return;
        const int maxH = qMax(16, barH - 8); // wjy: 28px 标题栏内固定保留约 20px 图标画布，紧凑 SVG 的主体可显示到 12px 左右。
        const int maxW = qMax(16, hitRect.width() - 12);
        QSize target = raw.size();
        target.scale(maxW, maxH, Qt::KeepAspectRatio);
        const QRect drawRect(
            hitRect.center().x() - target.width() / 2,
            hitRect.center().y() - target.height() / 2,
            target.width(),
            target.height());
        painter.drawPixmap(drawRect, raw);
    };
    drawTitleButton(minimizeRect(), QStringLiteral("rd_minimize.svg"), false);
    drawTitleButton(maximizeRect(), QStringLiteral("rd_maximize.svg"), false);
    drawTitleButton(closeRect(), QStringLiteral("rd_close.svg"), true); // wjy: 三个按钮共用尺寸与居中规则，仅关闭按钮使用警示悬停色。
    // ===end====
}

void RemoteDesktopWindow::closeEvent(QCloseEvent* event)
{
    saveWindowGeometry();
    // =====wjy====
    appendViewerDebugLog(QStringLiteral("closeEvent handle=%1 closing=%2")
        .arg(reinterpret_cast<quintptr>(m_viewerHandle))
        .arg(m_closeInProgress ? 1 : 0)); // wjy: show whether a crash is triggered by closing/stop.
    // ===end====
    if (m_closeInProgress) {
        event->ignore();
        return;
    }
    if (!m_viewerHandle && !m_viewerStopInProgress) {
        QWidget::closeEvent(event);
        return;
    }

    event->ignore();
    m_closeInProgress = true;
    if (m_remoteUpdateTimer) {
        m_remoteUpdateTimer->stop();
    }
    setRemoteMouseCaptureActive(false);
    releasePressedKeys();
    setKeyboardForwardingActive(false);
    hide();
    stopViewerConnectionAsync(true); // wjy: 更新等待或普通远控都复用安全停流，stop 返回前窗口对象保持存活。
}

void RemoteDesktopWindow::mousePressEvent(QMouseEvent* event)
{
    emit activated(this);
    // =====wjy====
    if (event->button() == Qt::RightButton && isTitleBarBlankArea(event->pos())) {
        emit titleBarContextMenuRequested(m_hostIp, event->globalPosition().toPoint()); // wjy: 使用窗口构造时固定保存的 IP，菜单目标不跟随主界面当前选中设备变化。
        event->accept();
        return; // wjy: 仅截获普通窗口标题栏空白区域；远控画面内的右键继续走下面的输入转发逻辑。
    }
    // ===end====
    if (event->button() == Qt::LeftButton) {
        m_resizeEdges = resizeEdgesAt(event->pos());
        if (m_resizeEdges != ResizeNone) {
            m_resizingWindow = true;
            m_resizeStartGlobal = event->globalPosition().toPoint();
            m_resizeStartGeometry = frameGeometry();
            event->accept();
            return;
        }

        if (isTitleBarBlankArea(event->pos())) { // wjy: 标题栏空白区域同时支持拖动和双击最大化，命中规则集中到同一个函数里维护。
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

// =====wjy====
void RemoteDesktopWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    emit activated(this);
    if (event->button() == Qt::LeftButton && isTitleBarBlankArea(event->pos())) {
        m_draggingWindow = false; // wjy: 双击时取消前一次按下建立的拖动状态，避免最大化后继续按拖动逻辑移动窗口。
        toggleMaximizedState(); // wjy: 双击标题栏空白处复用右侧最大化图标的 showMaximized/showNormal 切换逻辑。
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
// ===end====

void RemoteDesktopWindow::mouseMoveEvent(QMouseEvent* event)
{
    m_hoveredPos = event->pos();
    if (!isFullScreen()) {
        update(QRect(0, 0, width(), titleBarHeight())); // wjy: 普通窗口才需要刷新标题栏。
    }

    // =====wjy====
    const QString titleButtonTip = !isFullScreen() && m_remoteUpdateAvailable
            && remoteUpdateButtonRect().contains(event->pos())
        ? zh("更新目标设备")
        : (!isFullScreen() && clipboardSyncRect().contains(event->pos())
                ? zh("开关剪切板")
                : QString()); // wjy: 更新和剪切板按钮共用一套标题栏气泡，远控画面及全屏状态不显示本地提示。
    if (toolTip() != titleButtonTip) {
        setToolTip(titleButtonTip); // wjy: 保存当前提示内容，避免鼠标在同一按钮内移动时反复重置气泡。
        if (!titleButtonTip.isEmpty()) {
            QToolTip::showText(event->globalPosition().toPoint(), titleButtonTip, this); // wjy: 气泡跟随鼠标显示，明确“更新”操作的是当前窗口绑定设备。
        } else {
            QToolTip::hideText(); // wjy: 离开两个标题栏按钮后立即收起气泡，防止提示残留在远控画面上。
        }
    } else if (!titleButtonTip.isEmpty() && !QToolTip::isVisible()) {
        QToolTip::showText(event->globalPosition().toPoint(), titleButtonTip, this); // wjy: 系统自动隐藏气泡后，只要鼠标仍停在按钮上就允许再次显示。
    }
    // ===end====

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

    if (sendRemoteMouseMove(event->pos(), event->buttons())) {
        event->accept();
        return;
    }

    updateResizeCursor(event->pos());
    QWidget::mouseMoveEvent(event);
}

void RemoteDesktopWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const bool wasDraggingWindow = m_draggingWindow;
        const bool wasResizingWindow = m_resizingWindow;
        m_draggingWindow = false;
        m_resizingWindow = false;
        m_resizeEdges = ResizeNone;
        if (wasDraggingWindow || wasResizingWindow) {
            saveWindowGeometry(); // wjy: 拖动或缩放完成后只保存新几何，不再把这次鼠标释放解释为标题栏按钮点击。
            updateResizeCursor(event->pos());
            event->accept();
            return; // wjy: 防止从右上角缩放窗口后，释放位置仍在关闭矩形内而误关闭远控窗口。
        }

        // =====wjy====
        if (!isFullScreen()) { // wjy: 全屏时顶部右侧也属于远端画面，释放鼠标不能误触发本地最小化、最大化或关闭。
            if (m_remoteUpdateAvailable && remoteUpdateButtonRect().contains(event->pos())) {
                emit titleBarUpdateRequested(m_hostIp); // wjy: 只发出固定 IP 请求，实际检查、提示和更新窗口保持全部复用设备菜单逻辑。
                event->accept();
                return;
            }
            if (clipboardSyncRect().contains(event->pos())) {
                toggleClipboardSync(); // wjy: 标题栏按钮切换剪切板同步，与 Ctrl+B 共用同一状态。
                event->accept();
                return;
            }
            if (minimizeRect().contains(event->pos())) {
                showMinimized();
                event->accept();
                return;
            }
            if (maximizeRect().contains(event->pos())) {
                toggleMaximizedState(); // wjy: 最大化图标点击也走统一函数，保证标题栏双击和按钮点击行为完全一致。
                event->accept();
                return;
            }
            if (closeRect().contains(event->pos())) {
                close();
                event->accept();
                return;
            }
        }
        // ===end====
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
#if defined(Q_OS_WIN)
    updateShortcutReleaseGuard();
    if (m_waitingShortcutRelease) {
        event->accept();
        return;
    }
#endif
// =====wjy====
    const QKeySequence current = shortcutSequenceFromKeyEvent(event); // wjy: 普通 Qt 按键路径也读取设置页自定义快捷键。
    if (matchesShortcut(current, platform::AppSettings::remoteShortcutFullscreen())) {
        beginShortcutReleaseGuard(platform::AppSettings::remoteShortcutFullscreen());
        emit shortcutFullscreenRequested();
        event->accept();
        return;
    }
    if (matchesShortcut(current, platform::AppSettings::remoteShortcutTile())) {
        beginShortcutReleaseGuard(platform::AppSettings::remoteShortcutTile());
        emit shortcutTileRequested();
        event->accept();
        return;
    }
    if (matchesShortcut(current, platform::AppSettings::remoteShortcutCloseAll())) {
        beginShortcutReleaseGuard(platform::AppSettings::remoteShortcutCloseAll());
        emit shortcutCloseAllRequested();
        event->accept();
        return;
    }
    if (matchesShortcut(current, platform::AppSettings::remoteShortcutCloseTopmost())) {
        beginShortcutReleaseGuard(platform::AppSettings::remoteShortcutCloseTopmost());
        emit shortcutCloseTopmostRequested();
        event->accept();
        return;
    }
    if (matchesShortcut(current, platform::AppSettings::remoteShortcutClipboardSync())) {
        beginShortcutReleaseGuard(platform::AppSettings::remoteShortcutClipboardSync());
        emit shortcutClipboardSyncRequested();
        event->accept();
        return;
    }
// ===end====

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
#if defined(Q_OS_WIN)
    const bool wasWaitingShortcutRelease = m_waitingShortcutRelease;
    updateShortcutReleaseGuard();
    if (wasWaitingShortcutRelease || m_waitingShortcutRelease) {
        event->accept();
        return;
    }
#endif
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
    emit activated(this);
    setKeyboardForwardingActive(!remoteUpdateActive()); // wjy: 更新遮罩获得焦点时也不安装全局键盘转发钩子。
    QWidget::focusInEvent(event);
}

void RemoteDesktopWindow::focusOutEvent(QFocusEvent* event)
{
    setRemoteMouseCaptureActive(false);
    releasePressedKeys();
    setKeyboardForwardingActive(false);
    QWidget::focusOutEvent(event);
}

void RemoteDesktopWindow::resizeEvent(QResizeEvent* event)
{
    updateWindowMask();
    updateTexturePresenterGeometry();
    QWidget::resizeEvent(event);
}

void RemoteDesktopWindow::leaveEvent(QEvent* event)
{
    m_hoveredPos = QPoint(-1, -1);
    // =====wjy====
    if (!toolTip().isEmpty()) {
        setToolTip(QString()); // wjy: 鼠标离开整个远控窗口时同步清空剪切板按钮的提示状态。
        QToolTip::hideText(); // wjy: 主动关闭已显示的系统气泡，避免切到其他窗口后仍短暂残留。
    }
    // ===end====
    if (!isFullScreen()) {
        update(QRect(0, 0, width(), titleBarHeight())); // wjy: 普通窗口离开时清理标题栏悬停重绘。
    }
    QWidget::leaveEvent(event);
}

} // namespace ui
