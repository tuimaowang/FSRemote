#include "ui/RemoteDesktopWindow.h"

#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "stream/StreamRuntime.h"
#include "ui/D3D11FramePresenter.h"

#include <QByteArray>
#include <QApplication>
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
#include <QRubberBand>
#include <QScreen>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include <thread>

#include <algorithm>
#include <limits>

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
constexpr int kWindowEdgeSnapDistance = 16; // wjy: 标题栏拖动到屏幕可用区域 16px 内时自动贴边，既容易触发又不会在远处突然跳动。
constexpr int kWindowGroupJoinTolerance = 2; // wjy: 已贴齐窗口允许 2px 的系统边框/缩放误差，超过后不再视为同一连续窗口组。

enum class NearbyWindowSnapSide {
    None,
    Above,
    Below,
    Left,
    Right,
};

struct NearbyWindowSnapCandidate {
    RemoteDesktopWindow* target = nullptr;
    NearbyWindowSnapSide side = NearbyWindowSnapSide::None;
    int distance = std::numeric_limits<int>::max();
    int pointerAxisDistance = std::numeric_limits<int>::max(); // wjy: 鼠标在目标窗口对应轴范围内时为 0，优先选择鼠标当前指向的那一格。
    int pointerCenterDistance = std::numeric_limits<int>::max(); // wjy: 鼠标不在任何目标范围内或位于公共边界时，用到目标中心的距离稳定判定。
};

struct SnapGeometryResult {
    bool snapped = false;
    QRect movingGeometry;
    QHash<RemoteDesktopWindow*, QRect> geometries; // wjy: 普通吸附只有拖动窗口，越界重排时包含整个关联窗口组。
};

struct GroupSnapNode {
    RemoteDesktopWindow* window = nullptr;
    QRect originalGeometry;
};

struct GroupSnapContact {
    int first = -1;
    int second = -1;
    NearbyWindowSnapSide secondSide = NearbyWindowSnapSide::None; // wjy: 记录第二个窗口位于第一个窗口的哪一侧，用于缩放后恢复紧贴关系。
};

struct BuiltGroupLayout {
    QHash<RemoteDesktopWindow*, QRect> geometries;
    QRect bounds;
};

QString zh(const char* utf8)
{
    return QString::fromUtf8(utf8);
}

QPixmap icon(const QString& name)
{
    return QPixmap(QStringLiteral(":/UUGuest/resource/images/titlebar/") + name);
}

QSize aspectWindowSizeForWidth(
    int windowWidth,
    const QSize& remoteFrameSize,
    int titleBarHeight,
    const QSize& minimumSize)
{
    if (!remoteFrameSize.isValid() || remoteFrameSize.width() <= 0 || remoteFrameSize.height() <= 0) {
        return QSize(qMax(windowWidth, minimumSize.width()), minimumSize.height());
    }

    int width = qMax(windowWidth, minimumSize.width());
    int contentHeight = qMax(1, int((qint64(width) * remoteFrameSize.height()
        + remoteFrameSize.width() / 2) / remoteFrameSize.width()));
    int height = contentHeight + titleBarHeight;
    if (height < minimumSize.height()) {
        height = minimumSize.height();
        contentHeight = qMax(1, height - titleBarHeight);
        width = qMax(minimumSize.width(), int((qint64(contentHeight) * remoteFrameSize.width()
            + remoteFrameSize.height() / 2) / remoteFrameSize.height()));
    }
    return QSize(width, height); // wjy: 固定吸附宽度后按被控设备分辨率计算内容高度，窗口内容区不再产生上下或左右黑边。
}

QSize aspectWindowSizeForHeight(
    int windowHeight,
    const QSize& remoteFrameSize,
    int titleBarHeight,
    const QSize& minimumSize)
{
    if (!remoteFrameSize.isValid() || remoteFrameSize.width() <= 0 || remoteFrameSize.height() <= 0) {
        return QSize(minimumSize.width(), qMax(windowHeight, minimumSize.height()));
    }

    int height = qMax(windowHeight, minimumSize.height());
    int contentHeight = qMax(1, height - titleBarHeight);
    int width = qMax(1, int((qint64(contentHeight) * remoteFrameSize.width()
        + remoteFrameSize.height() / 2) / remoteFrameSize.height()));
    if (width < minimumSize.width()) {
        width = minimumSize.width();
        contentHeight = qMax(1, int((qint64(width) * remoteFrameSize.height()
            + remoteFrameSize.width() / 2) / remoteFrameSize.width()));
        height = qMax(minimumSize.height(), contentHeight + titleBarHeight);
    }
    return QSize(width, height); // wjy: 固定吸附高度后按被控设备分辨率计算窗口宽度，标题栏不参与远控画面宽高比。
}

QSize aspectWindowSizeByShrinking(
    const QSize& currentWindowSize,
    const QSize& remoteFrameSize,
    int titleBarHeight,
    const QSize& minimumSize)
{
    if (!remoteFrameSize.isValid() || remoteFrameSize.width() <= 0 || remoteFrameSize.height() <= 0) {
        return currentWindowSize;
    }
    const int contentHeight = qMax(1, currentWindowSize.height() - titleBarHeight);
    const qint64 currentAspectLeft = qint64(currentWindowSize.width()) * remoteFrameSize.height();
    const qint64 currentAspectRight = qint64(contentHeight) * remoteFrameSize.width();
    return currentAspectLeft > currentAspectRight
        ? aspectWindowSizeForHeight(currentWindowSize.height(), remoteFrameSize, titleBarHeight, minimumSize)
        : aspectWindowSizeForWidth(currentWindowSize.width(), remoteFrameSize, titleBarHeight, minimumSize); // wjy: 屏幕边缘吸附只缩掉产生黑边的多余方向，不主动放大用户当前窗口。
}

QRect snappedToScreenEdges(
    const QRect& proposed,
    const QPoint& cursorGlobal,
    const QSize& remoteFrameSize,
    int titleBarHeight,
    const QSize& minimumSize,
    bool* snapped)
{
    if (snapped) *snapped = false;
    QScreen* targetScreen = QGuiApplication::screenAt(cursorGlobal); // wjy: 多显示器拖动时以鼠标当前所在屏幕为目标，跨屏后立即使用新屏幕边界。
    if (!targetScreen) {
        targetScreen = QGuiApplication::screenAt(proposed.center());
    }
    if (!targetScreen) return proposed;

    const QRect available = targetScreen->availableGeometry();
    const int leftDistance = qAbs(proposed.left() - available.left());
    const int rightDistance = qAbs(proposed.right() - available.right());
    const int topDistance = qAbs(proposed.top() - available.top());
    const int bottomDistance = qAbs(proposed.bottom() - available.bottom());
    const bool snapHorizontal = qMin(leftDistance, rightDistance) <= kWindowEdgeSnapDistance;
    const bool snapVertical = qMin(topDistance, bottomDistance) <= kWindowEdgeSnapDistance;
    if (!snapHorizontal && !snapVertical) return proposed;

    QRect result(proposed.topLeft(), aspectWindowSizeByShrinking(
        proposed.size(), remoteFrameSize, titleBarHeight, minimumSize)); // wjy: 第一个窗口触发屏幕吸附时立即按远端屏幕比例缩掉黑边。
    if (snapHorizontal) {
        leftDistance <= rightDistance ? result.moveLeft(available.left()) : result.moveRight(available.right());
    }
    if (snapVertical) {
        topDistance <= bottomDistance ? result.moveTop(available.top()) : result.moveBottom(available.bottom());
    }
    if (snapped) *snapped = true;
    return result;
}

bool rangesOverlap(int firstStart, int firstEnd, int secondStart, int secondEnd)
{
    return qMin(firstEnd, secondEnd) >= qMax(firstStart, secondStart); // wjy: 只有窗口在另一方向存在实际交叠时才允许边缘互吸，避免斜对角远距离跳动。
}

int distanceToRange(int value, int rangeStart, int rangeEnd)
{
    if (value < rangeStart) return rangeStart - value;
    if (value > rangeEnd) return value - rangeEnd;
    return 0; // wjy: 鼠标落在目标窗口投影范围内时距离为 0，直接把该窗口视为鼠标指向的吸附目标。
}

QList<RemoteDesktopWindow*> visibleSnapWindows(RemoteDesktopWindow* movingWindow)
{
    QList<RemoteDesktopWindow*> windows;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* remoteWindow = qobject_cast<RemoteDesktopWindow*>(widget);
        if (!remoteWindow || remoteWindow == movingWindow || !remoteWindow->isVisible()
            || remoteWindow->isMinimized() || remoteWindow->isMaximized()
            || remoteWindow->isFullScreen() || remoteWindow->isClosingConnection()) {
            continue; // wjy: 隐藏、最小化、全屏、最大化和正在关闭的远控窗口都不能作为吸附目标。
        }
        windows.append(remoteWindow);
    }
    return windows;
}

NearbyWindowSnapCandidate nearestWindowSnapCandidate(
    const QRect& proposed,
    const QList<RemoteDesktopWindow*>& windows,
    const QPoint& cursorGlobal)
{
    NearbyWindowSnapCandidate nearest;
    const auto consider = [&nearest](
                              RemoteDesktopWindow* target,
                              NearbyWindowSnapSide side,
                              int distance,
                              int pointerAxisDistance,
                              int pointerCenterDistance) {
        if (distance > kWindowEdgeSnapDistance) return;
        const bool betterCandidate = !nearest.target
            || distance < nearest.distance
            || (distance == nearest.distance
                && pointerAxisDistance < nearest.pointerAxisDistance)
            || (distance == nearest.distance
                && pointerAxisDistance == nearest.pointerAxisDistance
                && pointerCenterDistance < nearest.pointerCenterDistance); // wjy: 边缘距离相同时完全依据鼠标所在目标范围和鼠标到目标中心的距离选择，不再比较窗口交叉长度。
        if (betterCandidate) {
            nearest.target = target;
            nearest.side = side;
            nearest.distance = distance;
            nearest.pointerAxisDistance = pointerAxisDistance;
            nearest.pointerCenterDistance = pointerCenterDistance; // wjy: 保存鼠标方向排序依据，使虚影随鼠标跨过相邻窗口边界时切换到对应位置。
        }
    };

    for (RemoteDesktopWindow* target : windows) {
        const QRect targetGeometry = target->frameGeometry();
        if (rangesOverlap(proposed.left(), proposed.right(), targetGeometry.left(), targetGeometry.right())) {
            const int pointerHorizontalDistance = distanceToRange(
                cursorGlobal.x(), targetGeometry.left(), targetGeometry.right()); // wjy: 上下吸附只看鼠标横坐标当前指向左侧还是右侧目标窗口。
            const int pointerHorizontalCenterDistance = qAbs(
                cursorGlobal.x() - targetGeometry.center().x());
            consider(target, NearbyWindowSnapSide::Above,
                qAbs(proposed.bottom() + 1 - targetGeometry.top()),
                pointerHorizontalDistance,
                pointerHorizontalCenterDistance);
            consider(target, NearbyWindowSnapSide::Below,
                qAbs(proposed.top() - targetGeometry.bottom() - 1),
                pointerHorizontalDistance,
                pointerHorizontalCenterDistance);
        }
        if (rangesOverlap(proposed.top(), proposed.bottom(), targetGeometry.top(), targetGeometry.bottom())) {
            const int pointerVerticalDistance = distanceToRange(
                cursorGlobal.y(), targetGeometry.top(), targetGeometry.bottom()); // wjy: 左右吸附只看鼠标纵坐标当前指向上方还是下方目标窗口。
            const int pointerVerticalCenterDistance = qAbs(
                cursorGlobal.y() - targetGeometry.center().y());
            consider(target, NearbyWindowSnapSide::Left,
                qAbs(proposed.right() + 1 - targetGeometry.left()),
                pointerVerticalDistance,
                pointerVerticalCenterDistance);
            consider(target, NearbyWindowSnapSide::Right,
                qAbs(proposed.left() - targetGeometry.right() - 1),
                pointerVerticalDistance,
                pointerVerticalCenterDistance);
        }
    }
    return nearest;
}

bool windowsAreJoined(const QRect& first, const QRect& second, bool horizontalGroup)
{
    if (horizontalGroup) {
        const bool sameRow = qAbs(first.top() - second.top()) <= kWindowGroupJoinTolerance
            && qAbs(first.bottom() - second.bottom()) <= kWindowGroupJoinTolerance;
        const bool touching = qAbs(first.right() + 1 - second.left()) <= kWindowGroupJoinTolerance
            || qAbs(second.right() + 1 - first.left()) <= kWindowGroupJoinTolerance;
        return sameRow && touching; // wjy: 上下吸附只把同高且左右相连的窗口识别为横向组。
    }

    const bool sameColumn = qAbs(first.left() - second.left()) <= kWindowGroupJoinTolerance
        && qAbs(first.right() - second.right()) <= kWindowGroupJoinTolerance;
    const bool touching = qAbs(first.bottom() + 1 - second.top()) <= kWindowGroupJoinTolerance
        || qAbs(second.bottom() + 1 - first.top()) <= kWindowGroupJoinTolerance;
    return sameColumn && touching; // wjy: 左右吸附只把同宽且上下相连的窗口识别为纵向组。
}

QRect selectedWindowGroupGeometry(
    RemoteDesktopWindow* target,
    const QList<RemoteDesktopWindow*>& windows,
    bool horizontalGroup,
    const QRect& proposed)
{
    QSet<RemoteDesktopWindow*> connected;
    connected.insert(target);
    bool addedWindow = true;
    while (addedWindow) {
        addedWindow = false;
        for (RemoteDesktopWindow* candidate : windows) {
            if (connected.contains(candidate)) continue;
            const QList<RemoteDesktopWindow*> currentMembers = connected.values(); // wjy: 遍历快照，避免循环中扩展 QSet 导致迭代器失效。
            for (RemoteDesktopWindow* member : currentMembers) {
                if (windowsAreJoined(member->frameGeometry(), candidate->frameGeometry(), horizontalGroup)) {
                    connected.insert(candidate); // wjy: 递归收集与目标直接或间接相连的整行/整列窗口。
                    addedWindow = true;
                    break;
                }
            }
        }
    }

    QList<RemoteDesktopWindow*> ordered = connected.values();
    std::sort(ordered.begin(), ordered.end(), [horizontalGroup](RemoteDesktopWindow* first, RemoteDesktopWindow* second) {
        return horizontalGroup
            ? first->frameGeometry().left() < second->frameGeometry().left()
            : first->frameGeometry().top() < second->frameGeometry().top();
    });

    const QRect targetGeometry = target->frameGeometry();
    const int unitSpan = horizontalGroup ? targetGeometry.width() : targetGeometry.height();
    const int draggedSpan = horizontalGroup ? proposed.width() : proposed.height();
    int desiredUnits = 1;
    for (int units = 2; units <= ordered.size(); ++units) {
        if (draggedSpan * 2 > (units * 2 - 1) * unitSpan) {
            desiredUnits = units; // wjy: 1.5/2.5/3.5 倍分别匹配 2/3/4 个连续窗口单元。
        }
    }

    const int targetIndex = ordered.indexOf(target);
    const int firstStart = qMax(0, targetIndex - desiredUnits + 1);
    const int lastStart = qMin(targetIndex, ordered.size() - desiredUnits);
    QRect bestGroup;
    int bestCenterDistance = std::numeric_limits<int>::max();
    for (int start = firstStart; start <= lastStart; ++start) {
        QRect groupGeometry = ordered.at(start)->frameGeometry();
        for (int offset = 1; offset < desiredUnits; ++offset) {
            groupGeometry = groupGeometry.united(ordered.at(start + offset)->frameGeometry());
        }
        const int centerDistance = horizontalGroup
            ? qAbs(groupGeometry.center().x() - proposed.center().x())
            : qAbs(groupGeometry.center().y() - proposed.center().y());
        if (centerDistance < bestCenterDistance) {
            bestCenterDistance = centerDistance;
            bestGroup = groupGeometry; // wjy: 大组中选择包含目标且最靠近拖动窗口中心的连续子组进行对齐。
        }
    }
    return bestGroup.isValid() ? bestGroup : targetGeometry;
}

NearbyWindowSnapSide oppositeSnapSide(NearbyWindowSnapSide side)
{
    switch (side) {
    case NearbyWindowSnapSide::Above: return NearbyWindowSnapSide::Below;
    case NearbyWindowSnapSide::Below: return NearbyWindowSnapSide::Above;
    case NearbyWindowSnapSide::Left: return NearbyWindowSnapSide::Right;
    case NearbyWindowSnapSide::Right: return NearbyWindowSnapSide::Left;
    default: return NearbyWindowSnapSide::None;
    }
}

NearbyWindowSnapSide touchingSide(const QRect& first, const QRect& second)
{
    NearbyWindowSnapSide bestSide = NearbyWindowSnapSide::None;
    int bestDistance = std::numeric_limits<int>::max();
    const auto consider = [&bestSide, &bestDistance](NearbyWindowSnapSide side, int distance) {
        if (distance <= kWindowGroupJoinTolerance && distance < bestDistance) {
            bestSide = side;
            bestDistance = distance;
        }
    };

    if (rangesOverlap(first.left(), first.right(), second.left(), second.right())) {
        consider(NearbyWindowSnapSide::Above, qAbs(second.bottom() + 1 - first.top()));
        consider(NearbyWindowSnapSide::Below, qAbs(second.top() - first.bottom() - 1));
    }
    if (rangesOverlap(first.top(), first.bottom(), second.top(), second.bottom())) {
        consider(NearbyWindowSnapSide::Left, qAbs(second.right() + 1 - first.left()));
        consider(NearbyWindowSnapSide::Right, qAbs(second.left() - first.right() - 1));
    }
    return bestSide; // wjy: 返回第二个窗口相对第一个窗口的接触方向，横竖复杂布局也能形成统一连接图。
}

QList<RemoteDesktopWindow*> connectedSnapComponent(
    RemoteDesktopWindow* target,
    const QList<RemoteDesktopWindow*>& windows)
{
    QList<RemoteDesktopWindow*> connected;
    if (!target) return connected;
    connected.append(target);
    for (int index = 0; index < connected.size(); ++index) {
        RemoteDesktopWindow* member = connected.at(index);
        for (RemoteDesktopWindow* candidate : windows) {
            if (!candidate || connected.contains(candidate)) continue;
            if (touchingSide(member->frameGeometry(), candidate->frameGeometry())
                != NearbyWindowSnapSide::None) {
                connected.append(candidate); // wjy: 递归收集所有直接或间接贴在目标上的窗口，只调整真正属于同一吸附组的设备。
            }
        }
    }
    return connected;
}

QSize scaledAspectWindowSize(RemoteDesktopWindow* window, const QRect& original, double scale)
{
    if (!window) return original.size();
    const QSize minimumSize = window->minimumSize();
    const int scaledWidth = qMax(minimumSize.width(), qRound(original.width() * scale));
    const QSize remoteSize = window->remoteFrameSize();
    if (remoteSize.isValid()) {
        return aspectWindowSizeForWidth(
            scaledWidth, remoteSize, window->titleBarHeight(), minimumSize); // wjy: 每个窗口都用自己的设备分辨率重算高度，整组压缩后仍保持画面比例无黑边。
    }
    return QSize(
        scaledWidth,
        qMax(minimumSize.height(), qRound(original.height() * scale))); // wjy: 尚未收到远端分辨率时按原外框比例缩放，避免无法形成完整布局。
}

QRect placeRelativeGeometry(
    const QRect& anchorGeometry,
    const QRect& anchorOriginal,
    const QRect& childOriginal,
    const QSize& childSize,
    NearbyWindowSnapSide childSide,
    double scale)
{
    QRect child(QPoint(0, 0), childSize);
    if (childSide == NearbyWindowSnapSide::Above || childSide == NearbyWindowSnapSide::Below) {
        child.moveLeft(anchorGeometry.left()
            + qRound((childOriginal.left() - anchorOriginal.left()) * scale)); // wjy: 上下相连时按原横向相对位置缩放，保留跨多个窗口单元的布局关系。
        childSide == NearbyWindowSnapSide::Above
            ? child.moveBottom(anchorGeometry.top() - 1)
            : child.moveTop(anchorGeometry.bottom() + 1);
    } else {
        child.moveTop(anchorGeometry.top()
            + qRound((childOriginal.top() - anchorOriginal.top()) * scale)); // wjy: 左右相连时按原纵向相对位置缩放，保留上下错位或多层窗口关系。
        childSide == NearbyWindowSnapSide::Left
            ? child.moveRight(anchorGeometry.left() - 1)
            : child.moveLeft(anchorGeometry.right() + 1);
    }
    return child;
}

BuiltGroupLayout buildGroupLayoutAtScale(
    const QList<GroupSnapNode>& nodes,
    int rootIndex,
    double scale)
{
    BuiltGroupLayout layout;
    if (nodes.isEmpty() || rootIndex < 0 || rootIndex >= nodes.size()) return layout;

    QVector<QSize> sizes;
    sizes.reserve(nodes.size());
    for (const GroupSnapNode& node : nodes) {
        sizes.append(scaledAspectWindowSize(node.window, node.originalGeometry, scale));
    }

    QList<GroupSnapContact> contacts;
    for (int first = 0; first < nodes.size(); ++first) {
        for (int second = first + 1; second < nodes.size(); ++second) {
            const NearbyWindowSnapSide side = touchingSide(
                nodes.at(first).originalGeometry, nodes.at(second).originalGeometry);
            if (side != NearbyWindowSnapSide::None) {
                contacts.append({first, second, side});
            }
        }
    }

    QVector<QRect> placedGeometries(nodes.size());
    QVector<bool> placed(nodes.size(), false);
    placedGeometries[rootIndex] = QRect(QPoint(0, 0), sizes.at(rootIndex));
    placed[rootIndex] = true;
    int placedCount = 1;
    bool madeProgress = true;
    while (madeProgress && placedCount < nodes.size()) {
        madeProgress = false;
        for (const GroupSnapContact& contact : contacts) {
            int anchor = -1;
            int child = -1;
            NearbyWindowSnapSide childSide = NearbyWindowSnapSide::None;
            if (placed.at(contact.first) && !placed.at(contact.second)) {
                anchor = contact.first;
                child = contact.second;
                childSide = contact.secondSide;
            } else if (placed.at(contact.second) && !placed.at(contact.first)) {
                anchor = contact.second;
                child = contact.first;
                childSide = oppositeSnapSide(contact.secondSide);
            }
            if (anchor < 0 || child < 0) continue;

            placedGeometries[child] = placeRelativeGeometry(
                placedGeometries.at(anchor),
                nodes.at(anchor).originalGeometry,
                nodes.at(child).originalGeometry,
                sizes.at(child),
                childSide,
                scale);
            placed[child] = true;
            ++placedCount;
            madeProgress = true; // wjy: 沿吸附连接图逐个放置窗口，保证至少一条原有接触边在重排后仍然完全贴合。
        }
    }

    const QRect rootOriginal = nodes.at(rootIndex).originalGeometry;
    for (int index = 0; index < nodes.size(); ++index) {
        if (!placed.at(index)) {
            placedGeometries[index] = QRect(
                QPoint(
                    qRound((nodes.at(index).originalGeometry.left() - rootOriginal.left()) * scale),
                    qRound((nodes.at(index).originalGeometry.top() - rootOriginal.top()) * scale)),
                sizes.at(index)); // wjy: 极端异常连接图下仍按原相对位置放置，避免漏掉任何需要调整的窗口。
        }
        layout.geometries.insert(nodes.at(index).window, placedGeometries.at(index));
        layout.bounds = layout.bounds.isValid()
            ? layout.bounds.united(placedGeometries.at(index))
            : placedGeometries.at(index);
    }
    return layout;
}

bool fitConnectedSnapLayout(
    RemoteDesktopWindow* movingWindow,
    RemoteDesktopWindow* target,
    const QList<RemoteDesktopWindow*>& windows,
    const QRect& movingGeometry,
    const QRect& available,
    QHash<RemoteDesktopWindow*, QRect>* fittedGeometries)
{
    if (!movingWindow || !target || !available.isValid() || !fittedGeometries) return false;
    QList<RemoteDesktopWindow*> component = connectedSnapComponent(target, windows);
    if (!component.contains(target)) component.append(target);

    QList<GroupSnapNode> nodes;
    nodes.reserve(component.size() + 1);
    int rootIndex = -1;
    for (RemoteDesktopWindow* window : component) {
        if (window == target) rootIndex = nodes.size();
        nodes.append({window, window->frameGeometry()});
    }
    nodes.append({movingWindow, movingGeometry}); // wjy: 先把新窗口按原吸附结果接入连接图，再整体判断需要缩小多少。
    if (rootIndex < 0) return false;

    BuiltGroupLayout smallest = buildGroupLayoutAtScale(nodes, rootIndex, 0.0);
    if (!smallest.bounds.isValid()
        || smallest.bounds.width() > available.width()
        || smallest.bounds.height() > available.height()) {
        return false; // wjy: 所有窗口都达到 100x100 最小限制后仍放不下时，物理上无法完成整组吸附。
    }

    BuiltGroupLayout best = smallest;
    double low = 0.0;
    double high = 1.0;
    for (int iteration = 0; iteration < 28; ++iteration) {
        const double middle = (low + high) * 0.5;
        BuiltGroupLayout candidate = buildGroupLayoutAtScale(nodes, rootIndex, middle);
        const bool fits = candidate.bounds.isValid()
            && candidate.bounds.width() <= available.width()
            && candidate.bounds.height() <= available.height();
        if (fits) {
            low = middle;
            best = candidate; // wjy: 二分寻找屏幕内可容纳的最大整组尺寸，尽量减少已有窗口被缩小的幅度。
        } else {
            high = middle;
        }
    }

    const int maximumLeft = available.right() - best.bounds.width() + 1;
    const int maximumTop = available.bottom() - best.bounds.height() + 1;
    const int desiredLeft = qBound(available.left(), movingGeometry.united(target->frameGeometry()).left(), maximumLeft);
    const int desiredTop = qBound(available.top(), movingGeometry.united(target->frameGeometry()).top(), maximumTop);
    const QPoint translation = QPoint(desiredLeft, desiredTop) - best.bounds.topLeft();
    fittedGeometries->clear();
    for (auto it = best.geometries.cbegin(); it != best.geometries.cend(); ++it) {
        fittedGeometries->insert(it.key(), it.value().translated(translation)); // wjy: 将完整布局整体平移回目标附近，并确保所有窗口都位于屏幕可用区域内。
    }
    return fittedGeometries->contains(movingWindow);
}

SnapGeometryResult snappedToNearbyWindows(
    RemoteDesktopWindow* movingWindow,
    const QRect& proposed,
    const QPoint& cursorGlobal,
    const QSize& remoteFrameSize,
    int titleBarHeight,
    const QSize& minimumSize)
{
    SnapGeometryResult snapResult;
    snapResult.movingGeometry = proposed;
    const QList<RemoteDesktopWindow*> windows = visibleSnapWindows(movingWindow);
    const NearbyWindowSnapCandidate candidate = nearestWindowSnapCandidate(
        proposed, windows, cursorGlobal); // wjy: 将拖拽鼠标的全局坐标传入候选选择，虚影跟随鼠标指向的目标格切换。
    if (!candidate.target || candidate.side == NearbyWindowSnapSide::None) {
        return snapResult;
    }

    const bool verticalSnap = candidate.side == NearbyWindowSnapSide::Above
        || candidate.side == NearbyWindowSnapSide::Below;
    const QRect targetGeometry = candidate.target->frameGeometry();
    const QRect groupGeometry = selectedWindowGroupGeometry(
        candidate.target, windows, verticalSnap, proposed);
    QRect result = proposed;
    if (verticalSnap) {
        result.setSize(remoteFrameSize.isValid()
            ? aspectWindowSizeForWidth(groupGeometry.width(), remoteFrameSize, titleBarHeight, minimumSize)
            : QSize(groupGeometry.width(), targetGeometry.height())); // wjy: 上下吸附先沿用 1 倍/2 倍组宽，再按当前被控设备比例计算无黑边高度。
        result.moveLeft(groupGeometry.left());
        if (candidate.side == NearbyWindowSnapSide::Above) {
            result.moveBottom(groupGeometry.top() - 1);
        } else {
            result.moveTop(groupGeometry.bottom() + 1);
        }
    } else {
        result.setSize(remoteFrameSize.isValid()
            ? aspectWindowSizeForHeight(groupGeometry.height(), remoteFrameSize, titleBarHeight, minimumSize)
            : QSize(targetGeometry.width(), groupGeometry.height())); // wjy: 左右吸附先沿用 1 倍/2 倍组高，再按当前被控设备比例计算无黑边宽度。
        result.moveTop(groupGeometry.top());
        if (candidate.side == NearbyWindowSnapSide::Left) {
            result.moveRight(groupGeometry.left() - 1);
        } else {
            result.moveLeft(groupGeometry.right() + 1);
        }
    }

    QScreen* targetScreen = QGuiApplication::screenAt(groupGeometry.center());
    if (targetScreen && !targetScreen->availableGeometry().contains(result)) {
        QHash<RemoteDesktopWindow*, QRect> fittedGeometries;
        if (!fitConnectedSnapLayout(
                movingWindow,
                candidate.target,
                windows,
                result,
                targetScreen->availableGeometry(),
                &fittedGeometries)) {
            return snapResult; // wjy: 达到全部窗口最小尺寸后仍无法容纳时，才真正取消这次窗口间吸附。
        }
        snapResult.snapped = true;
        snapResult.geometries = fittedGeometries;
        snapResult.movingGeometry = fittedGeometries.value(movingWindow, result);
        return snapResult; // wjy: 新窗口越界时返回整组重排方案，拖拽阶段显示所有目标虚影，松开后批量提交。
    }
    snapResult.snapped = true;
    snapResult.movingGeometry = result;
    snapResult.geometries.insert(movingWindow, result);
    return snapResult;
}

SnapGeometryResult snappedDraggedWindowGeometry(
    RemoteDesktopWindow* movingWindow,
    const QRect& proposed,
    const QPoint& cursorGlobal,
    const QSize& remoteFrameSize,
    int titleBarHeight,
    const QSize& minimumSize)
{
    SnapGeometryResult nearbyResult = snappedToNearbyWindows(
        movingWindow, proposed, cursorGlobal, remoteFrameSize, titleBarHeight, minimumSize);
    if (nearbyResult.snapped) {
        return nearbyResult; // wjy: 窗口之间吸附优先于屏幕边缘，整组越界重排方案也在这里直接返回。
    }

    bool snappedToScreen = false;
    const QRect screenResult = snappedToScreenEdges(
        proposed, cursorGlobal, remoteFrameSize, titleBarHeight, minimumSize, &snappedToScreen);
    SnapGeometryResult result;
    result.snapped = snappedToScreen;
    result.movingGeometry = screenResult;
    if (snappedToScreen) {
        result.geometries.insert(movingWindow, screenResult); // wjy: 普通屏幕边缘吸附仍只预览和提交当前拖动窗口。
    }
    return result;
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
        if (m_resizingWindow && (buttons & Qt::LeftButton)) {
            QMouseEvent forwardedEvent(
                QEvent::MouseMove,
                QPointF(parentPosition),
                QPointF(parentPosition),
                QPointF(mapToGlobal(parentPosition)),
                Qt::NoButton,
                buttons,
                Qt::NoModifier);
            mouseMoveEvent(&forwardedEvent); // wjy: 缩放开始后鼠标仍由纹理子控件接收，必须把连续移动交回父窗口才能更新窗口几何。
            return;
        }
        m_hoveredPos = parentPosition;
        if (!isFullScreen()) {
            update(QRect(0, 0, width(), titleBarHeight())); // wjy: 共享纹理模式的鼠标移动只在普通窗口刷新标题栏。
        }
        updateResizeCursor(parentPosition); // wjy: D3D 子控件会截获远控画面内的移动事件，必须在转发前同步本地边缘光标状态。
        sendRemoteMouseMove(parentPosition, buttons);
    });
    m_texturePresenter->setMouseButtonCallbacks(
        [this](const QPoint& parentPosition, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers) {
            QMouseEvent forwardedEvent(
                QEvent::MouseButtonPress,
                QPointF(parentPosition),
                QPointF(parentPosition),
                QPointF(mapToGlobal(parentPosition)),
                button,
                buttons,
                modifiers);
            mousePressEvent(&forwardedEvent); // wjy: 纹理子控件的按下事件复用父窗口逻辑，边缘位置即可进入窗口缩放状态。
        },
        [this](const QPoint& parentPosition, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers) {
            QMouseEvent forwardedEvent(
                QEvent::MouseButtonRelease,
                QPointF(parentPosition),
                QPointF(parentPosition),
                QPointF(mapToGlobal(parentPosition)),
                button,
                buttons,
                modifiers);
            mouseReleaseEvent(&forwardedEvent); // wjy: 纹理子控件的释放事件复用父窗口逻辑，确保缩放结束和远端按钮释放一致。
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
    // =====wjy====
    clearSnapPreviews();
    for (QRubberBand* preview : m_snapPreviews) {
        delete preview; // wjy: 整组虚影都是无父对象顶层窗口，析构时逐个释放，避免残留在桌面上。
    }
    m_snapPreviews.clear();
    // ===end====
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

void RemoteDesktopWindow::showSnapPreviews(
    const QHash<RemoteDesktopWindow*, QRect>& geometries)
{
    m_pendingSnapGeometries = geometries;
    while (m_snapPreviews.size() < geometries.size()) {
        auto* preview = new QRubberBand(QRubberBand::Rectangle); // wjy: 按整组调整数量按需创建顶层虚影，普通单窗口吸附仍只使用一个。
        preview->setAttribute(Qt::WA_TransparentForMouseEvents);
        preview->setAttribute(Qt::WA_ShowWithoutActivating);
        preview->setWindowFlag(Qt::WindowTransparentForInput, true); // wjy: 每个虚影都完全鼠标穿透，不会打断当前窗口持续拖拽。
        preview->setStyleSheet(QStringLiteral(
            "QRubberBand { border: 2px solid #2f8cff; background-color: rgba(47, 140, 255, 48); }"));
        m_snapPreviews.append(preview);
    }

    int previewIndex = 0;
    for (auto it = geometries.cbegin(); it != geometries.cend(); ++it, ++previewIndex) {
        QRubberBand* preview = m_snapPreviews.at(previewIndex);
        preview->setGeometry(it.value());
        preview->show();
        preview->raise(); // wjy: 同时显示新窗口和所有将被调整窗口的最终位置，松开前真实窗口保持不变。
    }
    for (; previewIndex < m_snapPreviews.size(); ++previewIndex) {
        m_snapPreviews.at(previewIndex)->hide();
    }
}

void RemoteDesktopWindow::clearSnapPreviews()
{
    for (QRubberBand* preview : m_snapPreviews) {
        if (preview) preview->hide();
    }
    m_pendingSnapGeometries.clear(); // wjy: 拖离吸附范围时同时撤销整组待提交数据，避免释放后应用过期布局。
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
    isMaximized() ? showNormal() : showMaximized(); // wjy: 最大化和恢复只切换显示状态，不把系统最大化矩形写入设备窗口 JSON。
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
    if (!m_rememberGeometry || isMinimized() || isMaximized() || isFullScreen()) {
        return;
    } // wjy: 最大化、最小化和全屏都属于临时显示状态，JSON 始终保留最后一次普通窗口的位置与大小。
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
    if (m_remoteMouseCaptureActive) {
        return; // wjy: 相对鼠标捕获模式由 BlankCursor 统一控制，不能被普通边缘缩放光标覆盖。
    }
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

    QFont identityFont(QStringLiteral("Microsoft YaHei UI"));
    identityFont.setPixelSize(12);
    bool showIdentityLogo = true;
    int identityTextX = 34;
    const auto identityTextWidth = [this](const QFont& font) {
        const QFontMetrics metrics(font);
        return metrics.horizontalAdvance(m_deviceName)
            + (m_hostIp.isEmpty() ? 0 : 10 + metrics.horizontalAdvance(m_hostIp));
    };
    if (identityTextX + identityTextWidth(identityFont) + 6 > width()) {
        showIdentityLogo = false;
        identityTextX = 6; // wjy: 窗口变窄时先让出非必要 Logo 空间，设备名和 IP 的显示优先级最高。
    }
    while (identityFont.pixelSize() > 7
        && identityTextWidth(identityFont) > qMax(0, width() - identityTextX - 6)) {
        identityFont.setPixelSize(identityFont.pixelSize() - 1); // wjy: 继续缩小时逐级压缩设备信息字号，尽量在最小窗口中完整保留名称和 IP。
    }

    const QFontMetrics identityMetrics(identityFont);
    const int identityNameWidth = identityMetrics.horizontalAdvance(m_deviceName);
    const int identityIpX = identityTextX + identityNameWidth + (m_hostIp.isEmpty() ? 0 : 10);
    const int identityIpWidth = identityMetrics.horizontalAdvance(m_hostIp);
    const int identityRight = qMin(
        width() - 1,
        identityIpX + identityIpWidth + 6);
    if (identityRight > 1) {
        painter.fillRect(
            QRect(1, 1, identityRight - 1, qMax(0, barH - 2)),
            QColor(QStringLiteral("#E9EEF2"))); // wjy: 最后覆盖计时、更新入口和窗口按钮的视觉内容，为设备名/IP 保留最高绘制层级。
    }
    if (showIdentityLogo) {
        painter.drawPixmap(QRect(12, logoY, 16, 16), icon(QStringLiteral("fs_session_logo.svg")));
    }
    painter.setFont(identityFont);
    painter.setPen(QColor(QStringLiteral("#111820")));
    painter.drawText(
        QRectF(identityTextX, 0, identityNameWidth, barH),
        Qt::AlignVCenter | Qt::AlignLeft,
        m_deviceName); // wjy: 设备名在所有标题栏元素之后绘制，窗口再窄也不会被按钮覆盖。
    if (!m_hostIp.isEmpty()) {
        painter.setPen(QColor(QStringLiteral("#667085")));
        painter.drawText(
            QRectF(identityIpX, 0, identityIpWidth, barH),
            Qt::AlignVCenter | Qt::AlignLeft,
            m_hostIp); // wjy: IP 使用完整原文且不省略，允许覆盖右侧低优先级按钮和状态内容。
    }
    // ===end====
}

void RemoteDesktopWindow::closeEvent(QCloseEvent* event)
{
    // =====wjy====
    clearSnapPreviews(); // wjy: 拖拽过程中关闭窗口时立即清除整组吸附虚影和待提交布局。
    // ===end====
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
            // =====wjy====
            clearSnapPreviews(); // wjy: 开始手动缩放时取消上一次整组拖拽候选，缩放操作不参与吸附提交。
            // ===end====
            m_resizingWindow = true;
            m_resizeStartGlobal = event->globalPosition().toPoint();
            m_resizeStartGeometry = frameGeometry();
            event->accept();
            return;
        }

        if (isTitleBarBlankArea(event->pos())) { // wjy: 标题栏空白区域同时支持拖动和双击最大化，命中规则集中到同一个函数里维护。
            // =====wjy====
            clearSnapPreviews(); // wjy: 每次开始拖拽都从无候选状态重新判断，防止提交旧的整组吸附位置。
            // ===end====
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
        // =====wjy====
        clearSnapPreviews(); // wjy: 双击最大化不会沿用第一次按下时产生的整组吸附虚影。
        // ===end====
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
        const QPoint cursorGlobal = event->globalPosition().toPoint();
        const QPoint proposedTopLeft = cursorGlobal - m_dragOffset;
        const QRect proposedGeometry(proposedTopLeft, frameGeometry().size());
        const SnapGeometryResult snapResult = snappedDraggedWindowGeometry(
            this,
            proposedGeometry,
            cursorGlobal,
            remoteFrameSize(),
            titleBarHeight(),
            minimumSize()); // wjy: 拖拽过程中计算单窗口或整组候选目标，不直接改变任何窗口大小。
        move(proposedTopLeft); // wjy: 无论是否经过吸附范围，真实窗口都保持原大小并连续跟随鼠标移动。
        if (snapResult.snapped) {
            showSnapPreviews(snapResult.geometries); // wjy: 越界重排时显示所有相关窗口的最终虚影，普通吸附仍显示当前窗口一个虚影。
        } else {
            clearSnapPreviews(); // wjy: 未松开并拖离吸附范围后立即撤销整组候选，所有真实窗口保持不变。
        }
        event->accept();
        return;
    }

    updateResizeCursor(event->pos()); // wjy: 先刷新本地光标，再决定是否把当前位置转发到远端，避免远端画面路径提前返回留下旧的缩放光标。
    if (sendRemoteMouseMove(event->pos(), event->buttons())) {
        event->accept();
        return;
    }

    QWidget::mouseMoveEvent(event);
}

void RemoteDesktopWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const bool wasDraggingWindow = m_draggingWindow;
        const bool wasResizingWindow = m_resizingWindow;
        const QHash<RemoteDesktopWindow*, QRect> snapGeometries = m_pendingSnapGeometries; // wjy: 先复制释放瞬间的完整布局，再统一隐藏虚影和清空候选。
        clearSnapPreviews();
        m_draggingWindow = false;
        m_resizingWindow = false;
        m_resizeEdges = ResizeNone;
        if (wasDraggingWindow || wasResizingWindow) {
            if (wasDraggingWindow && !snapGeometries.isEmpty()) {
                const QList<QWidget*> topLevelWindows = QApplication::topLevelWidgets();
                for (auto it = snapGeometries.cbegin(); it != snapGeometries.cend(); ++it) {
                    if (it.key() && topLevelWindows.contains(it.key()) && it.value().isValid()) {
                        it.key()->setGeometry(it.value()); // wjy: 在鼠标释放这一刻批量应用整组最终位置和大小，拖拽期间真实窗口完全不动。
                    }
                }
                for (auto it = snapGeometries.cbegin(); it != snapGeometries.cend(); ++it) {
                    if (it.key() && topLevelWindows.contains(it.key())) {
                        it.key()->saveWindowGeometry(); // wjy: 为所有被整组重排的设备分别保存新几何，重开远控时恢复一致布局。
                    }
                }
            }
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
