#include "ui/RemoteControllerOverlay.h"

#include <QAbstractAnimation>
#include <QGuiApplication>
#include <QCursor>
#include <QGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>

#include <cmath>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace ui {
namespace {

constexpr int dragThreshold = 8;
constexpr int hoverIntentDelayMs = 150; // wjy: 短暂过滤快速划过，同时保持用户主动悬停时的即时感。
constexpr int vortexSize = 36; // wjy: 拖拽漩涡尺寸缩小为原来的二分之一，减少跟随鼠标时对下层内容的遮挡。
constexpr qreal pi = 3.14159265358979323846;

#ifdef Q_OS_WIN
HWND windowBelowOverlayAtPoint(const POINT& point, HWND overlayWindow)
{
    // wjy: WindowFromPoint 在部分透明窗口组合场景下仍可能返回悬浮层，因此沿 Z 序继续查找其下方窗口。
    for (HWND candidate = GetWindow(overlayWindow, GW_HWNDNEXT);
         candidate;
         candidate = GetWindow(candidate, GW_HWNDNEXT)) {
        if (!IsWindowVisible(candidate)) continue;
        RECT bounds{};
        if (GetWindowRect(candidate, &bounds) && PtInRect(&bounds, point)) {
            HWND deepestWindow = candidate;
            while (true) {
                POINT childPoint = point;
                ScreenToClient(deepestWindow, &childPoint); // wjy: 将屏幕坐标转换为当前窗口客户坐标，继续寻找桌面图标列表子窗口。
                HWND child = ChildWindowFromPointEx(deepestWindow, childPoint, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
                if (!child || child == deepestWindow) break;
                deepestWindow = child;
            }
            return deepestWindow; // wjy: 返回鼠标点下方最深层窗口，确保能够识别 SysListView32 等桌面图标控件。
        }
    }
    return nullptr;
}

HWND underlyingWindowAtPoint(const QPoint& cursor, HWND overlayWindow)
{
    POINT point{cursor.x(), cursor.y()};
    HWND underlyingWindow = WindowFromPoint(point); // wjy: 查找当前鼠标下方真正接收点击的窗口，悬浮层本身保持穿透。
    if (underlyingWindow && underlyingWindow != overlayWindow
        && GetAncestor(underlyingWindow, GA_ROOT) != overlayWindow) {
        return underlyingWindow;
    }
    underlyingWindow = windowBelowOverlayAtPoint(point, overlayWindow);
    return underlyingWindow;
}

bool cursorIndicatesSystemMoveOrResize()
{
    CURSORINFO cursorInfo{};
    cursorInfo.cbSize = sizeof(cursorInfo);
    if (!GetCursorInfo(&cursorInfo) || !(cursorInfo.flags & CURSOR_SHOWING)) return false;

    const HCURSOR cursor = cursorInfo.hCursor;
    return cursor == LoadCursorW(nullptr, IDC_SIZEWE)
        || cursor == LoadCursorW(nullptr, IDC_SIZENS)
        || cursor == LoadCursorW(nullptr, IDC_SIZENWSE)
        || cursor == LoadCursorW(nullptr, IDC_SIZENESW)
        || cursor == LoadCursorW(nullptr, IDC_SIZEALL); // wjy: 直接识别用户看到的窗口缩放/移动光标，补足自绘窗口未返回标准命中值的情况。
}

bool pointIsOutsideClientArea(HWND rootWindow, const POINT& point)
{
    RECT windowBounds{};
    RECT clientBounds{};
    if (!GetWindowRect(rootWindow, &windowBounds) || !GetClientRect(rootWindow, &clientBounds)) return false;

    POINT clientPoints[2] = {
        {clientBounds.left, clientBounds.top},
        {clientBounds.right, clientBounds.bottom}
    };
    SetLastError(ERROR_SUCCESS); // wjy: MapWindowPoints 返回 0 也可能代表坐标无需偏移，先清除旧错误码再判断是否真正失败。
    if (MapWindowPoints(rootWindow, nullptr, clientPoints, 2) == 0 && GetLastError() != ERROR_SUCCESS) return false;

    const RECT clientScreenBounds{
        clientPoints[0].x,
        clientPoints[0].y,
        clientPoints[1].x,
        clientPoints[1].y
    };
    return PtInRect(&windowBounds, point) && !PtInRect(&clientScreenBounds, point); // wjy: 标题栏和系统边框位于窗口矩形内、客户区外，应完全交给下层窗口。
}

bool pointIsInLikelyTitleBar(HWND rootWindow, const POINT& point)
{
    RECT windowBounds{};
    if (!GetWindowRect(rootWindow, &windowBounds) || !PtInRect(&windowBounds, point)) return false;

    const UINT dpi = GetDpiForWindow(rootWindow);
    const int customTitleBarHeight = MulDiv(28, dpi > 0 ? static_cast<int>(dpi) : 96, 96); // wjy: 覆盖项目自身 28px 自绘标题栏，并随窗口 DPI 缩放。
    const int systemTitleBarHeight = GetSystemMetricsForDpi(SM_CYCAPTION, dpi > 0 ? dpi : 96)
        + GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi > 0 ? dpi : 96)
        + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi > 0 ? dpi : 96); // wjy: 标准窗口按系统标题栏及顶部边框高度判断。
    const int titleBandHeight = qMax(customTitleBarHeight, systemTitleBarHeight);
    return point.y >= windowBounds.top && point.y < windowBounds.top + titleBandHeight; // wjy: 自绘标题栏即使返回 HTCLIENT，也优先穿透给下面窗口移动。
}

bool startsSystemWindowDrag(const QPoint& cursor, HWND overlayWindow)
{
    HWND underlyingWindow = underlyingWindowAtPoint(cursor, overlayWindow);
    if (!underlyingWindow) return false;

    HWND rootWindow = GetAncestor(underlyingWindow, GA_ROOT);
    if (!rootWindow || rootWindow == overlayWindow) return false;

    POINT point{cursor.x(), cursor.y()};
    if (cursorIndicatesSystemMoveOrResize()
        || pointIsOutsideClientArea(rootWindow, point)
        || pointIsInLikelyTitleBar(rootWindow, point)) {
        return true; // wjy: 优先按真实光标形态和客户区边界判定，保证标题栏及 <--> 缩放状态不会触发悬浮拖拽。
    }

    DWORD_PTR hitTestResult = HTCLIENT;
    const LRESULT queryResult = SendMessageTimeoutW(
        rootWindow,
        WM_NCHITTEST,
        0,
        MAKELPARAM(point.x, point.y),
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        50,
        &hitTestResult); // wjy: 使用带超时的非客户区命中测试，避免下层窗口卡顿阻塞悬浮窗状态轮询。
    if (queryResult == 0) return false;

    switch (static_cast<LRESULT>(hitTestResult)) {
    case HTCAPTION: // wjy: 标题栏移动交给下面的窗口处理。
    case HTLEFT: // wjy: 左边缘水平缩放交给下面的窗口处理。
    case HTRIGHT: // wjy: 右边缘水平缩放交给下面的窗口处理。
    case HTTOP: // wjy: 上边缘垂直缩放交给下面的窗口处理。
    case HTTOPLEFT: // wjy: 左上角缩放交给下面的窗口处理。
    case HTTOPRIGHT: // wjy: 右上角缩放交给下面的窗口处理。
    case HTBOTTOM: // wjy: 下边缘垂直缩放交给下面的窗口处理。
    case HTBOTTOMLEFT: // wjy: 左下角缩放交给下面的窗口处理。
    case HTBOTTOMRIGHT: // wjy: 右下角缩放交给下面的窗口处理。
        return true;
    default:
        return false;
    }
}

#endif

QString controllerName(const QString& entry)
{
    const int separator = entry.indexOf(QLatin1Char('\t'));
    return (separator < 0 ? entry : entry.left(separator)).trimmed();
}

QString controllerIp(const QString& entry)
{
    const int separator = entry.indexOf(QLatin1Char('\t'));
    return (separator < 0 ? QString() : entry.mid(separator + 1)).trimmed();
}

} // namespace

RemoteControllerOverlay::RemoteControllerOverlay(QWidget* parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::Tool
        | Qt::FramelessWindowHint
        | Qt::WindowStaysOnTopHint); // wjy: 是否穿透由 Windows 命中测试动态决定，不能再使用永久输入穿透标志。
    setAttribute(Qt::WA_TranslucentBackground); // wjy: 窗口透明背景只保留圆角卡片，不在四角留下不透明矩形。
    setAttribute(Qt::WA_ShowWithoutActivating); // wjy: 被控端提示出现时不抢走用户或远控输入焦点。
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);

    m_geometryAnimation = new QPropertyAnimation(this, "geometry", this);
    m_geometryAnimation->setEasingCurve(QEasingCurve::OutCubic); // wjy: 完整卡片与右侧收起标签之间使用短促平滑的滑动动画。

    m_autoCollapseTimer = new QTimer(this);
    m_autoCollapseTimer->setSingleShot(true);
    m_autoCollapseTimer->setInterval(10000);
    connect(m_autoCollapseTimer, &QTimer::timeout, this, [this] {
        if (m_controllers.isEmpty()) return;
        if (m_dragging) return; // wjy: 拖拽漩涡期间禁止自动收起逻辑抢占窗口几何位置。
        if (geometry().contains(QCursor::pos())) {
            m_hoverExpanded = true; // wjy: 10 秒到达时鼠标正位于卡片上，保持展开直到鼠标移出。
            return;
        }
        showCollapsed();
    });

    // =====wjy====
    m_hoverExpandTimer = new QTimer(this);
    m_hoverExpandTimer->setSingleShot(true); // wjy: 每次进入收起徽标只验证一次悬停意图，离开后由轮询取消并等待下一次进入。
    m_hoverExpandTimer->setInterval(hoverIntentDelayMs); // wjy: 150ms 足以过滤快速划过，不影响正常停留查看详情。
    connect(m_hoverExpandTimer, &QTimer::timeout, this, [this] {
        if (m_controllers.isEmpty() || !isVisible() || m_expanded || m_dragging
            || m_dragCandidate || m_forwardingClick) {
            return; // wjy: 状态已经变化时丢弃过期计时，不让旧悬停任务重新打开气泡。
        }
        if (m_geometryAnimation->state() == QAbstractAnimation::Running) {
            return; // wjy: 展开或收起动画期间不从中间几何重新触发，动画结束后再由轮询重新判断。
        }
        if (!collapsedGeometry().contains(QCursor::pos())) {
            return; // wjy: 计时结束时必须仍位于稳定收起徽标，快速划过或停在未来展开区域都不会触发。
        }
        showExpanded(false); // wjy: 确认真实悬停意图后沿用现有展开和完整卡片离开检测。
    });
    // ===end====

    m_hoverPollTimer = new QTimer(this);
    m_hoverPollTimer->setInterval(16); // wjy: 约 60 FPS 轮询全局鼠标状态，同时保证拖拽跟手和漩涡旋转流畅。
    connect(m_hoverPollTimer, &QTimer::timeout, this, [this] { pollMouseState(); });
    m_hoverPollTimer->start();
    hide();
}

void RemoteControllerOverlay::setControllers(const QStringList& controllers)
{
    QStringList normalized;
    for (const QString& controller : controllers) {
        const QString trimmed = controller.trimmed();
        if (!trimmed.isEmpty() && !normalized.contains(trimmed)) {
            normalized.append(trimmed); // wjy: 多次轮询同一会话详情时去重，避免提示卡片重复显示同一控制端。
        }
    }

    if (m_controllers == normalized) {
        if (!normalized.isEmpty()) updatePosition();
        return;
    }

    m_controllers = normalized;
    if (m_controllers.isEmpty()) {
        m_autoCollapseTimer->stop();
        m_hoverExpandTimer->stop(); // wjy: 所有会话结束时取消尚未触发的悬停展开，防止隐藏后旧计时回调重新显示。
        m_geometryAnimation->stop();
        if (m_dragging || m_dragCandidate) releaseMouse(); // wjy: 会话结束时释放 Qt 鼠标捕获，避免隐藏窗口仍保留拖拽状态。
        m_expanded = false;
        m_hoverExpanded = false;
        m_dragCandidate = false;
        m_dragging = false;
        m_waitForPostDragLeave = false;
        m_forwardingClick = false;
        hide(); // wjy: 所有控制端断开后立即撤销目标机屏幕上的远控提示。
        return;
    }

    showExpanded(true); // wjy: 首次连接或控制端列表变化时立即完整弹出，并从这一刻重新计时 10 秒。
}

void RemoteControllerOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // =====wjy====
    painter.setCompositionMode(QPainter::CompositionMode_Source); // wjy: 直接覆盖上一帧像素，清除收起动画遗留在圆角区域的旧背景色。
    painter.fillRect(rect(), Qt::transparent); // wjy: 每次绘制先恢复完整透明底，避免收起态圆角外侧出现直角色块。
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver); // wjy: 恢复正常透明混合，再绘制卡片、边框和文字。
    // ===end====

    if (m_dragging) {
        paintVortex(painter); // wjy: 只有达到拖拽判定后才替换成动态漩涡，普通点击仍保持完全穿透。
        return;
    }

    const QRectF cardRect(rect()); // wjy: 使用浮点矩形覆盖完整窗口像素范围，避免 QRect 的包含式右下坐标少填充一个像素。

    constexpr qreal leftRadius = 9.0;
    // =====wjy====
    // wjy: 填充与边框共用左侧圆角路径，避免背景仍按直角矩形绘制而露出直角色块。
    QPainterPath cardFillPath;
    cardFillPath.moveTo(cardRect.right(), cardRect.top()); // wjy: 右上角保持直角，满足右侧不显示圆角的设计。
    cardFillPath.lineTo(cardRect.left() + leftRadius, cardRect.top()); // wjy: 沿上边回到左上圆角起点。
    cardFillPath.quadTo(cardRect.left(), cardRect.top(), cardRect.left(), cardRect.top() + leftRadius); // wjy: 构造左上圆角。
    cardFillPath.lineTo(cardRect.left(), cardRect.bottom() - leftRadius); // wjy: 绘制左侧直边。
    cardFillPath.quadTo(cardRect.left(), cardRect.bottom(), cardRect.left() + leftRadius, cardRect.bottom()); // wjy: 构造左下圆角。
    cardFillPath.lineTo(cardRect.right(), cardRect.bottom()); // wjy: 沿下边回到右下角。
    cardFillPath.closeSubpath(); // wjy: 闭合右侧直角边，确保填充区域完整覆盖卡片。

    painter.setPen(Qt::NoPen); // wjy: 填充阶段不绘制边框，边框由下面的开放路径单独绘制。
    painter.setBrush(QColor(20, 30, 48, 238)); // wjy: 使用悬浮窗深色背景。
    painter.drawPath(cardFillPath); // wjy: 通过非对称路径填充，左侧圆角与底色保持一致。
    // ===end====

    // =====wjy====
    constexpr qreal borderInset = 0.5;
    const QRectF borderRect = cardRect.adjusted(borderInset, borderInset, -borderInset, -borderInset); // wjy: 将 1px 描边中心线移入窗口半个像素，防止外半边被窗口裁掉。
    const qreal borderRadius = leftRadius - borderInset; // wjy: 同步缩小中心线半径，使边框外沿仍与 9px 填充圆角对齐。
    QPainterPath leftBorderPath;
    leftBorderPath.moveTo(borderRect.right(), borderRect.top()); // wjy: 从右上方开始绘制，但不闭合右侧竖边。
    leftBorderPath.lineTo(borderRect.left() + borderRadius, borderRect.top()); // wjy: 绘制完整的上边并连接左上圆角。
    leftBorderPath.quadTo(borderRect.left(), borderRect.top(), borderRect.left(), borderRect.top() + borderRadius); // wjy: 半像素内缩后绘制左上圆角，保留完整抗锯齿像素。
    leftBorderPath.lineTo(borderRect.left(), borderRect.bottom() - borderRadius); // wjy: 绘制左侧边框。
    leftBorderPath.quadTo(borderRect.left(), borderRect.bottom(), borderRect.left() + borderRadius, borderRect.bottom()); // wjy: 半像素内缩后绘制左下圆角，避免边缘缺点。
    leftBorderPath.lineTo(borderRect.right(), borderRect.bottom()); // wjy: 绘制完整下边，仍不增加右侧边框。
    painter.setPen(QPen(QColor(119, 173, 255, 170), 1.0)); // wjy: 保持清晰的 1px 视觉粗细，缺失问题通过坐标对齐解决而非盲目加粗。
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(leftBorderPath); // wjy: 使用开放路径形成左圆右直的非对称卡片轮廓。
    // ===end====

    if (!m_expanded) {
        QFont collapsedFont(QStringLiteral("Microsoft YaHei UI"));
        collapsedFont.setPixelSize(13); // wjy: 数量徽标使用略大的字号，收起后仍能一眼识别当前被控路数。
        collapsedFont.setWeight(QFont::DemiBold);
        painter.setFont(collapsedFont);
        painter.setPen(QColor(QStringLiteral("#FFFFFF")));
        painter.drawText(rect(), Qt::AlignCenter, collapsedTitle()); // wjy: 收起态只显示数量并居中，避免设备名或 IP 造成视觉噪声。
        return;
    }

    QFont titleFont(QStringLiteral("Microsoft YaHei UI"));
    titleFont.setPixelSize(14);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(QColor(QStringLiteral("#EAF3FF")));
    const QString title = m_controllers.size() == 1
        ? QString::fromUtf8("正在被远程控制")
        : QString::fromUtf8("正在被 %1 台设备远程控制").arg(m_controllers.size());
    painter.drawText(QRect(16, 11, width() - 32, 22), Qt::AlignLeft | Qt::AlignVCenter, title);

    QFont nameFont(QStringLiteral("Microsoft YaHei UI"));
    nameFont.setPixelSize(13);
    painter.setFont(nameFont);
    QFont ipFont(QStringLiteral("Microsoft YaHei UI"));
    ipFont.setPixelSize(11);

    int top = 38;
    for (const QString& entry : m_controllers) {
        const QString name = controllerName(entry).isEmpty()
            ? QString::fromUtf8("未知设备")
            : controllerName(entry);
        const QString ip = controllerIp(entry).isEmpty()
            ? QString::fromUtf8("IP：未知")
            : QString::fromUtf8("IP：") + controllerIp(entry);
        painter.setFont(nameFont);
        painter.setPen(QColor(QStringLiteral("#FFFFFF")));
        painter.drawText(QRect(16, top, width() - 32, 18), Qt::AlignLeft | Qt::AlignVCenter, name);
        painter.setFont(ipFont);
        painter.setPen(QColor(QStringLiteral("#B5C8E7")));
        painter.drawText(QRect(16, top + 19, width() - 32, 16), Qt::AlignLeft | Qt::AlignVCenter, ip);
        top += 42;
    }
}

void RemoteControllerOverlay::pollMouseState()
{
    const QPoint cursor = QCursor::pos();
    if (m_controllers.isEmpty() || !isVisible()) return;
    if (m_dragging) {
        m_hoverExpandTimer->stop(); // wjy: 拖拽状态完全独立于悬停展开，进入拖拽后立即撤销等待中的意图计时。
        m_vortexAngle += 7.5; // wjy: 拖拽期间由 16ms 定时器持续推进角度，鼠标静止时漩涡仍保持旋转。
        if (m_vortexAngle >= 360.0) m_vortexAngle -= 360.0;
        update();
        return;
    }
    if (m_dragCandidate || m_forwardingClick) {
        m_hoverExpandTimer->stop(); // wjy: 点击判定或穿透转发期间不允许悬停计时完成，避免点击动作意外展开气泡。
        return;
    }

    if (m_waitForPostDragLeave) {
        m_hoverExpandTimer->stop(); // wjy: 拖拽吸附后先等待鼠标离开徽标，再允许建立新的悬停意图。
        if (geometry().contains(cursor)) return; // wjy: 松手后光标仍在徽标上时保持收起，防止刚吸附就立即展开。
        m_waitForPostDragLeave = false;
    }

    if (!m_expanded) {
        if (m_geometryAnimation->state() == QAbstractAnimation::Running) {
            m_hoverExpandTimer->stop(); // wjy: 收起动画的移动矩形不能充当悬停触发区，避免动画扫过鼠标后再次展开。
            return;
        }
        if (collapsedGeometry().contains(cursor)) {
            if (!m_hoverExpandTimer->isActive()) {
                m_hoverExpandTimer->start(); // wjy: 进入稳定徽标后开始 150ms 意图判断，不再因单次 16ms 采样立即展开。
            }
        } else {
            m_hoverExpandTimer->stop(); // wjy: 鼠标在阈值到达前离开立即取消，快速划过不会留下展开气泡。
        }
        return;
    }
    m_hoverExpandTimer->stop(); // wjy: 已经展开后只使用完整卡片离开逻辑，不保留收起态计时任务。
    if (m_hoverExpanded && !geometry().contains(cursor)) {
        showCollapsed(); // wjy: 鼠标移出完整提示卡片后立即滑回屏幕右侧，仅保留数量。
    }
}

bool RemoteControllerOverlay::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef Q_OS_WIN
    MSG* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage && nativeMessage->message == WM_NCHITTEST && result) {
        // =====wjy====
        const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (m_forwardingClick || m_controllers.isEmpty() || !leftButtonDown) {
            *result = HTTRANSPARENT; // wjy: 普通悬停及转发点击时让系统直接命中悬浮窗下面的窗口。
            return true;
        }

        const int x = static_cast<short>(LOWORD(nativeMessage->lParam));
        const int y = static_cast<short>(HIWORD(nativeMessage->lParam));
        const QPoint cursor(x, y);
        HWND overlayWindow = reinterpret_cast<HWND>(winId());
        if (startsSystemWindowDrag(cursor, overlayWindow)) {
            *result = HTTRANSPARENT; // wjy: 下层标题栏或缩放边缘从按下开始就直接穿透，不暂存鼠标事件。
            return true;
        }

        *result = HTCLIENT; // wjy: 普通内容或桌面图标区域先由悬浮窗接收按下，用移动阈值决定点击转发还是悬浮拖拽。
        return true;
        // ===end====
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

void RemoteControllerOverlay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragCandidate = true; // wjy: 先暂存左键，不立即传给桌面图标，避免图标提前进入拖动状态。
        m_dragPressPosition = event->globalPosition().toPoint(); // wjy: 保存全局按下位置供 8px 拖拽阈值判断。
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void RemoteControllerOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint cursor = event->globalPosition().toPoint();
    if (!m_dragging && m_dragCandidate
        && (cursor - m_dragPressPosition).manhattanLength() >= dragThreshold) {
        beginDrag(cursor); // wjy: 只有真实移动超过阈值才进入漩涡，下面文件或程序图标从未收到按下事件。
    } else if (m_dragging) {
        updateDrag(cursor);
    }
    event->accept();
}

void RemoteControllerOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const QPoint cursor = event->globalPosition().toPoint();
        if (m_dragging) {
            finishDrag(cursor);
        } else if (m_dragCandidate) {
            m_dragCandidate = false;
            releaseMouse(); // wjy: 转发普通点击前先释放 Qt 自动鼠标捕获，确保注入点击能够命中下层窗口。
            forwardClickThrough(); // wjy: 未形成拖拽时补发一次普通左键点击，实现用户看到的点击穿透。
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void RemoteControllerOverlay::beginDrag(const QPoint& cursor)
{
    // =====wjy====
    m_autoCollapseTimer->stop(); // wjy: 拖拽开始后停止自动收起计时，避免状态竞争。
    m_hoverExpandTimer->stop(); // wjy: 拖拽开始前撤销悬停展开，两个几何状态机不能同时修改窗口位置。
    m_geometryAnimation->stop(); // wjy: 停止展开/收起动画，将窗口位置控制权交给拖拽状态。
    m_dragCandidate = false;
    m_dragging = true;
    m_waitForPostDragLeave = false;
    m_expanded = false;
    m_hoverExpanded = false;
    m_vortexAngle = 0.0;
    grabMouse(); // wjy: 使用 Qt 自己的鼠标捕获维持拖拽，不再与原生 SetCapture/ReleaseCapture 混用。
    setGeometry(dragGeometry(cursor)); // wjy: 将卡片立即变成以鼠标为中心的方形漩涡画布。
    raise();
    update();
    // ===end====
}

void RemoteControllerOverlay::updateDrag(const QPoint& cursor)
{
    setGeometry(dragGeometry(cursor)); // wjy: 漩涡仅跟随当前 Qt 拖拽，不再让下层图标同时收到移动事件。
    update();
}

void RemoteControllerOverlay::finishDrag(const QPoint& cursor)
{
    // =====wjy====
    releaseMouse(); // wjy: 松开左键后释放 Qt 鼠标捕获，后续命中测试重新恢复动态穿透。
    m_dragging = false;
    m_dragCandidate = false;
    m_expanded = false;
    m_hoverExpanded = false;
    m_waitForPostDragLeave = true; // wjy: 等待鼠标离开吸附后的徽标，再恢复悬浮展开响应。
    m_dockedCenterY = cursor.y(); // wjy: 保存松手高度，收起徽标水平吸附到右边界时维持用户选择的垂直位置。
    animateTo(collapsedGeometry(), 220); // wjy: 松手后平滑向右吸附，并恢复只显示设备数量的收起状态。
    update();
    // ===end====
}

void RemoteControllerOverlay::forwardClickThrough()
{
#ifdef Q_OS_WIN
    // =====wjy====
    m_forwardingClick = true; // wjy: 注入点击期间命中测试强制返回 HTTRANSPARENT，防止点击再次落回悬浮窗形成递归。
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT)); // wjy: 普通单击在松手后原样补发给当前光标下的窗口，保持点击穿透体验。
    QTimer::singleShot(60, this, [this] {
        m_forwardingClick = false; // wjy: 等注入消息完成命中后再恢复悬浮窗的拖拽接收能力。
    });
    // ===end====
#endif
}

void RemoteControllerOverlay::paintVortex(QPainter& painter)
{
    const QPointF center = QRectF(rect()).center();
    const qreal radius = qMin(width(), height()) * 0.5 - 2.0;

    // =====wjy====
    QRadialGradient background(center, radius); // wjy: 漩涡底色沿用悬浮卡片的深蓝配色，并在中心提高亮度。
    background.setColorAt(0.0, QColor(48, 76, 122, 155)); // wjy: 中心深蓝保留辨识度，同时允许下层画面透出。
    background.setColorAt(0.58, QColor(20, 30, 48, 138)); // wjy: 中段降低不透明度，形成明确的半透明底色。
    background.setColorAt(1.0, QColor(10, 18, 32, 105)); // wjy: 外圈进一步透明，缩小后的漩涡边缘更轻盈。
    painter.setPen(QPen(QColor(119, 173, 255, 165), 1.0));
    painter.setBrush(background);
    painter.drawEllipse(QRectF(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0));

    painter.save();
    painter.translate(center);
    painter.rotate(m_vortexAngle); // wjy: 整体旋转三条螺旋臂，保持鼠标静止时漩涡仍持续运动。
    for (int arm = 0; arm < 3; ++arm) {
        QPainterPath spiral;
        for (int step = 0; step <= 34; ++step) {
            const qreal progress = step / 34.0;
            const qreal angle = arm * (2.0 * pi / 3.0) + progress * 1.65 * 2.0 * pi;
            const qreal spiralRadius = 3.0 + progress * (radius - 7.0);
            const QPointF point(std::cos(angle) * spiralRadius, std::sin(angle) * spiralRadius);
            if (step == 0) spiral.moveTo(point);
            else spiral.lineTo(point);
        }
        painter.setPen(QPen(QColor(83, 143, 235, 65), 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); // wjy: 光晕宽度同步缩小一半，避免 36px 画布内旋臂粘连。
        painter.drawPath(spiral);
        painter.setPen(QPen(QColor(145, 196, 255, 220), 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); // wjy: 主旋臂按比例变细，仍保持当前浅蓝配色和清晰动态感。
        painter.drawPath(spiral);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(225, 241, 255, 235));
    painter.drawEllipse(QPointF(0.0, 0.0), 1.6, 1.6); // wjy: 中心亮点同步缩小一半，与整体 36px 尺寸保持比例。
    painter.restore();
    // ===end====
}

void RemoteControllerOverlay::updatePosition()
{
    if (m_controllers.isEmpty() || m_dragging) return; // wjy: 拖拽时禁止状态轮询把漩涡拉回右下角。
    if (m_geometryAnimation->state() == QAbstractAnimation::Running) return; // wjy: 保留展开、收起和拖拽吸附动画的中间帧，避免状态轮询提前跳到终点。
    const QRect target = m_expanded ? expandedGeometry() : collapsedGeometry();
    if (geometry() != target) setGeometry(target); // wjy: 分辨率、缩放或任务栏位置变化后，轮询会把提示重新贴到主屏右下角。
}

void RemoteControllerOverlay::showExpanded(bool startAutoCollapse)
{
    if (m_dragging) return;
    m_hoverExpandTimer->stop(); // wjy: 无论首次连接还是确认悬停触发展开，都清理已经完成或等待中的意图计时。
    m_expanded = true;
    m_hoverExpanded = !startAutoCollapse;
    const QRect target = expandedGeometry();
    if (!isVisible()) {
        setGeometry(target);
        show(); // wjy: 刚建立远控时完整卡片立即出现，不等待滑动动画。
    } else {
        animateTo(target, 160);
    }
    raise();
    update();
    if (startAutoCollapse) {
        m_autoCollapseTimer->start();
    } else {
        m_autoCollapseTimer->stop();
    }
}

void RemoteControllerOverlay::showCollapsed()
{
    if (m_controllers.isEmpty() || m_dragging) return;
    m_autoCollapseTimer->stop();
    m_hoverExpandTimer->stop(); // wjy: 收起时清理旧计时，必须重新停留满阈值才允许再次展开。
    m_expanded = false;
    m_hoverExpanded = false;
    animateTo(collapsedGeometry(), 140); // wjy: 自动计时结束或鼠标离开时快速缩到屏幕右侧。
    update();
}

void RemoteControllerOverlay::animateTo(const QRect& geometry, int durationMs)
{
    m_geometryAnimation->stop();
    m_geometryAnimation->setDuration(durationMs);
    m_geometryAnimation->setStartValue(this->geometry());
    m_geometryAnimation->setEndValue(geometry);
    m_geometryAnimation->start();
}

QRect RemoteControllerOverlay::dragGeometry(const QPoint& cursor) const
{
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) return {};
    const QRect available = screen->availableGeometry();
    const int maximumX = available.right() - vortexSize + 1;
    const int maximumY = available.bottom() - vortexSize + 1;
    const int left = qBound(available.left(), cursor.x() - vortexSize / 2, maximumX);
    const int top = qBound(available.top(), cursor.y() - vortexSize / 2, maximumY);
    return QRect(left, top, vortexSize, vortexSize); // wjy: 漩涡中心跟随鼠标，同时限制在主屏可用区域内避免被裁切。
}

QRect RemoteControllerOverlay::expandedGeometry() const
{
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) return {};
    const QRect available = screen->availableGeometry();
    constexpr int width = 330;
    const int height = 58 + m_controllers.size() * 42;
    constexpr int rightMargin = 0; // wjy: 展开卡片贴近屏幕右边，避免右侧空隙造成视觉上偏离任务栏。
    constexpr int bottomMargin = 18;
    int top = available.bottom() - height - bottomMargin + 1;
    if (m_dockedCenterY >= 0) {
        top = qBound(available.top(), m_dockedCenterY - height / 2, available.bottom() - height + 1); // wjy: 拖拽吸附后，展开态围绕用户选择的高度展开。
    }
    return QRect(
        available.right() - width - rightMargin + 1,
        top,
        width,
        height); // wjy: 展开态位于任务栏上方并贴近屏幕右边，保持与收起态一致的横向基准。
}

QRect RemoteControllerOverlay::collapsedGeometry() const
{
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) return {};
    const QRect available = screen->availableGeometry();
    constexpr int width = 52; // wjy: 收起态固定为紧凑数量徽标，不再根据设备名长度扩展。
    constexpr int height = 34;
    constexpr int rightMargin = 0; // wjy: 收起态与展开态共用贴右定位，鼠标移出后不会横向跳位。
    constexpr int bottomMargin = 18;
    int top = available.bottom() - height - bottomMargin + 1;
    if (m_dockedCenterY >= 0) {
        top = qBound(available.top(), m_dockedCenterY - height / 2, available.bottom() - height + 1); // wjy: 松手后保留当前垂直位置，只向右侧窗口边界吸附。
    }
    return QRect(
        available.right() - width - rightMargin + 1,
        top,
        width,
        height); // wjy: 收起徽标紧贴屏幕右边，固定尺寸只显示被控设备数量。
}

QString RemoteControllerOverlay::collapsedTitle() const
{
    if (m_controllers.isEmpty()) return {};
    return QString::number(m_controllers.size()); // wjy: 收起态严格只返回控制端数量，避免设备名长度影响徽标宽度。
}

} // namespace ui
