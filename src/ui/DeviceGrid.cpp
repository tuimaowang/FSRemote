#include "ui/DeviceGrid.h"

#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "system/DesktopWallpaperService.h"
#include "system/SharedStorageAvailabilityService.h"
#include "system/StartupPerformanceLog.h"
#include "system/DeviceInfoService.h"
#include "system/DeviceCatalog.h"
#include "system/DeviceCatalogRepository.h"
#include "system/DeviceActionTargetResolver.h"
#include "system/DeviceActionPolicy.h"
#include "system/DeviceStatusRefreshResult.h"
#include "system/DeviceListSyncModel.h"
#include "system/DeviceListSyncService.h"
#include "system/DeviceStatusService.h"
#include "system/PowerManager.h"
#include "system/PortableOpenSshManager.h"
#include "system/StartupManager.h"
#include "system/UpdateService.h"
#include "system/WolDetector.h"
#include "system/WjyDiagnosticLog.h"
#include "ui/DeviceSearchPanel.h"
#include "ui/DeviceListSortPolicy.h" // wjy: 设备栏统一复用在线优先、数字自然排序和英文字母排序策略。
#include "ui/RemoteDesktopWindow.h"
#include "ui/ScriptPanelVisibility.h"
#include "ui/SettingsLayoutSnapshot.h"
#include "ui/RemoteViewerLifecycleManager.h"

#include <QAction>
#include <QAbstractItemView>
#include <QAbstractSocket>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QEasingCurve>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QHostAddress>
#include <QIcon>
#include <QIntValidator>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QPixmap>
#include <QPushButton>
#include <QToolTip>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QTextEdit>
#include <QTextDocument>
#include <QTextStream>
#include <QVariantAnimation>
#include <QTextCursor>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVector>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <memory>
#include <mutex>
#include <tuple>
#include <thread>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <psapi.h>
#include <tlhelp32.h>
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
#endif

namespace ui {

namespace {

#if defined(Q_OS_WIN)
QString virtualDesktopKey(const RemoteDesktopWindow* window)
{
    if (!window) {
        return QStringLiteral("unknown");
    }
    IVirtualDesktopManager* manager = nullptr;
    const HRESULT createResult = CoCreateInstance(
        CLSID_VirtualDesktopManager,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&manager));
    if (FAILED(createResult) || !manager) {
        return QStringLiteral("unknown"); // wjy: 虚拟桌面接口不可用时回退到同一分组，保持原有平铺行为。
    }
    GUID desktopId{};
    const HRESULT queryResult = manager->GetWindowDesktopId(reinterpret_cast<HWND>(window->winId()), &desktopId);
    manager->Release();
    if (FAILED(queryResult)) {
        return QStringLiteral("unknown"); // wjy: 单个窗口查询失败时不阻断整个平铺动作。
    }
    return QStringLiteral("%1-%2-%3-%4")
        .arg(QString::number(desktopId.Data1, 16))
        .arg(QString::number(desktopId.Data2, 16))
        .arg(QString::number(desktopId.Data3, 16))
        .arg(QString::fromLatin1(reinterpret_cast<const char*>(desktopId.Data4), 8).toHex()); // wjy: GUID 转为稳定字符串作为分组键。
}
#endif

//标题栏高度常量
constexpr int kTitleBarHeight = 28;
// =====wjy====
constexpr int kDetailScriptTabTop = kTitleBarHeight + 2; // wjy: 将配置文件/脚本日志标签贴近标题栏下沿，让截图中的整块区域尽量上移。
constexpr int kDetailScriptPanelTop = kDetailScriptTabTop + 42; // wjy: 面板紧跟标签栏下方，只保留少量间距，避免顶部出现大块空白。
constexpr int kDeviceDetailBottomBoundaryHeight = 44; // wjy: 设备详情页底部保留不可绘制安全高度，给折叠按钮留出不会和详情内容重叠的空间。
constexpr int kDetailScriptPanelBottomGap = 8; // wjy: 终端/配置面板和安全下沿之间保留小间距，避免视觉上贴到底部。
// ===end====
constexpr int kSidebarContentTop = kTitleBarHeight + 10;
constexpr int kDefaultShellWidth = 920;
constexpr int kDefaultShellHeight = 680;
constexpr int kSidebarWidth = 240;
// =====wjy====
constexpr int kMinimumExpandedShellWidth = 720; // wjy: 详情栏展开时继续使用原主窗口最低宽度，保证右侧页面仍有可用空间。
constexpr int kMinimumShellHeight = 520; // wjy: 收起详情只改变宽度，窗口最低高度继续沿用原值。
constexpr int kDetailToggleStripWidth = 42; // wjy: 设备栏右侧独立窄条只承载 << / >>，不侵入设备行和滚动条。
constexpr int kCollapsedShellWidth = kSidebarWidth + kDetailToggleStripWidth; // wjy: 紧凑窗口由完整 240px 设备栏和 42px 按钮窄条组成。
constexpr int kScreenEdgeDockSnapDistance = 14; // wjy: 拖窗松开时鼠标或窗口边缘进入工作区边界附近即可吸附，不要求精确对齐单个像素。
constexpr int kScreenEdgeHiddenStripThickness = 4; // wjy: 顶部、左侧和右侧收起后都保留 4px 触发条，三种方向保持一致视觉厚度。
constexpr int kScreenEdgeRevealTriggerDepth = 6; // wjy: 全局鼠标命中比可见条略深 2px，降低高 DPI 下边缘难以触发的问题。
// ===end====
constexpr int kExpandedContentLeft = 270;
constexpr int kShellResizeGrip = 7;
constexpr int kResizeNone = 0;
constexpr int kResizeLeft = 0x1;
constexpr int kResizeRight = 0x2;
constexpr int kResizeTop = 0x4;
constexpr int kResizeBottom = 0x8;

QSize g_shellSize(kDefaultShellWidth, kDefaultShellHeight);

void setResponsiveLayoutState(const QSize& size, bool detailPanelCollapsed)
{
    // =====wjy====
    const int minimumWidth = detailPanelCollapsed
        ? kCollapsedShellWidth
        : kMinimumExpandedShellWidth; // wjy: 紧凑态按设备栏宽度布局，展开态不再错误地把 720-919px 窗口当成 920px 绘制。
    g_shellSize = QSize(qMax(minimumWidth, size.width()), qMax(kMinimumShellHeight, size.height()));
    // ===end====
}

int shellWidth()
{
    return g_shellSize.width();
}

int shellHeight()
{
    return g_shellSize.height();
}

int contentLeft()
{
    return kExpandedContentLeft; // wjy: 设备栏始终保留，右侧详情展开时固定从设备栏右侧开始。
}

int contentRightMargin()
{
    return 28;
}

int contentWidth()
{
    return qMax(360, shellWidth() - contentLeft() - contentRightMargin());
}

QRect contentClipRect()
{
    return QRect(kSidebarWidth, kTitleBarHeight, shellWidth() - kSidebarWidth, shellHeight() - kTitleBarHeight); // wjy: 详情内容永远不能覆盖左侧设备栏。
}

// =====wjy====
int deviceDetailBottomBoundaryTop()
{
    return qMax(kTitleBarHeight, shellHeight() - kDeviceDetailBottomBoundaryHeight); // wjy: 这个 y 值是设备详情允许绘制的最下沿，不再额外绘制白色条。
}

QRect deviceDetailContentClipRect()
{
    QRect clip = contentClipRect(); // wjy: 先复用右侧内容裁剪范围，保证折叠侧栏状态下左右边界仍然正确。
    clip.setBottom(qMax(clip.top(), deviceDetailBottomBoundaryTop() - 1)); // wjy: 设备详情内容不能画进底部安全区，避免压住折叠按钮所在区域。
    return clip;
}
// ===end====

int windowResizeEdgesAt(const QPoint& position, const QSize& size, bool allowHorizontalResize)
{
    int edges = kResizeNone;
    if (allowHorizontalResize) {
        if (position.x() <= kShellResizeGrip) {
            edges |= kResizeLeft;
        } else if (position.x() >= size.width() - kShellResizeGrip) {
            edges |= kResizeRight;
        }
    }
    if (position.y() <= kShellResizeGrip) {
        edges |= kResizeTop;
    } else if (position.y() >= size.height() - kShellResizeGrip) {
        edges |= kResizeBottom;
    }
    return edges;
}

// =====wjy====
constexpr int kTitleBarVisualHeight = 18; // wjy: 标题栏内文字、图标和分隔竖线共用的视觉高度，避免各自写死不同的上下位置。

QRect titleBarCenteredRect(int x, int width, int visualHeight = kTitleBarVisualHeight)
{
    return QRect(x, (kTitleBarHeight - visualHeight) / 2, width, visualHeight); // wjy: 根据标题栏总高度统一计算垂直居中位置，让刷新、最小化、关闭和左上标题名高度对齐。
}

QRect titlebarSettingsRect()
{
    return QRect(shellWidth() - 180, 0, 48, kTitleBarHeight); // wjy: 设置入口贴着刷新按钮左侧，窗口变宽时跟随右边缘移动。
}

QRect titlebarBandwidthUpdateRect()
{
    return QRect(132, 0, qMax(0, titlebarSettingsRect().x() - 132), kTitleBarHeight); // wjy: 一秒采样只重绘版本号到设置按钮之间的标题栏，不触碰设备列表和详情内容。
}

// =====wjy====
QRect titlebarUpdateRect()
{
    return QRect(shellWidth() - 260, 5, 76, kTitleBarHeight - 10); // wjy: 更新按钮位于设置按钮左侧，仅在检测到新版本时参与绘制和点击。
}
// ===end====
// ===end====


QString zh(const char* utf8)
{
    return QString::fromUtf8(utf8);
}

QPixmap uupix(const QString& name)
{
    return QPixmap(QStringLiteral(":/UUGuest/resource/images/") + name);
}

QIcon menuIcon(const QString& name)
{
    return QIcon(QStringLiteral(":/UUGuest/resource/images/menu/") + name);
}

// =====wjy====
void writeDeviceGridStartupLog(const QString& message)
{
    platform::writeWjyDiagnosticLog(message); // wjy: DeviceGrid 和后台线程日志统一走加锁写入，避免多线程日志交叉成半行。
    platform::StartupPerformanceLog::checkpoint(message); // wjy: 启动前三秒复用现有细粒度打点生成步骤耗时，观察窗口结束后不再写性能日志。
}

void cancelThreadSynchronousIo(std::thread& thread, const QString& phase)
{
#if defined(Q_OS_WIN)
    if (!thread.joinable()) {
        return; // wjy: 已结束或没有系统线程句柄的任务无需请求取消，也不能把无效句柄传给 Windows API。
    }

    const HANDLE threadHandle = thread.native_handle(); // wjy: std::thread 在 Windows 下保留真实线程句柄，CancelSynchronousIo 必须针对发起阻塞 I/O 的原线程调用。
    if (!CancelSynchronousIo(threadHandle)) {
        const DWORD errorCode = GetLastError(); // wjy: ERROR_NOT_FOUND 只表示该线程当前没有可取消的同步 I/O，属于正常竞态而不是退出失败。
        if (errorCode != ERROR_NOT_FOUND) {
            writeDeviceGridStartupLog(QStringLiteral("[wjy-exit] CancelSynchronousIo failed phase=%1 error=%2")
                .arg(phase)
                .arg(errorCode)); // wjy: 记录其它系统错误，后续可区分句柄权限问题和具体共享目录阻塞问题。
        }
    }
#else
    Q_UNUSED(thread)
    Q_UNUSED(phase)
#endif
}

QString currentProcessResourceSummary()
{
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS_EX memory = {};
    memory.cb = sizeof(memory);
    K32GetProcessMemoryInfo(
        GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
        sizeof(memory));
    DWORD handleCount = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handleCount);
    int threadCount = 0;
    const DWORD processId = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry = {};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == processId) ++threadCount;
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return QStringLiteral("working_mb=%1 private_mb=%2 threads=%3 handles=%4")
        .arg(memory.WorkingSetSize / (1024.0 * 1024.0), 0, 'f', 1)
        .arg(memory.PrivateUsage / (1024.0 * 1024.0), 0, 'f', 1)
        .arg(threadCount)
        .arg(handleCount); // wjy: 30秒级读取进程资源，20窗口soak可直接判断内存、线程或句柄是否持续无恢复增长。
#else
    return QStringLiteral("process_metrics=unsupported");
#endif
}
// ===end====

void drawDeviceTileIcon(QPainter& painter, int x, int y, int size, const QColor& backgroundColor)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(backgroundColor);
    painter.drawRoundedRect(QRectF(x, y, size, size), 4, 4);

    const qreal cell = size >= 20 ? 5.0 : 4.0;
    const qreal gap = size >= 20 ? 2.0 : 1.6;
    const qreal left = x + (size - cell * 2 - gap) / 2.0;
    const qreal top = y + (size - cell * 2 - gap) / 2.0;
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(QRectF(left, top, cell, cell), 1, 1);
    painter.drawRoundedRect(QRectF(left + cell + gap, top, cell, cell), 1, 1);
    painter.drawRoundedRect(QRectF(left, top + cell + gap, cell, cell), 1, 1);
    painter.drawRoundedRect(QRectF(left + cell + gap, top + cell + gap, cell, cell), 1, 1);
}

void drawDeviceTileIcon(QPainter& painter, int x, int y, int size = 20)
{
    drawDeviceTileIcon(painter, x, y, size, QColor(QStringLiteral("#3A7BFC")));
}

// =====wjy====
void drawScriptRunningIcon(QPainter& painter, const QRectF& bounds)
{
    painter.save(); // wjy: 独立保存绘制状态，避免运行图标的画笔和抗锯齿设置影响设备行其它元素。
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF badgeRect = bounds.adjusted(0.55, 0.55, -0.55, -0.55); // wjy: 半像素内缩让 1.1 像素边框在 18 像素徽标内保持清晰。
    painter.setPen(QPen(QColor(QStringLiteral("#3A7BFC")), 1.1));
    painter.setBrush(QColor(QStringLiteral("#F8FBFF")));
    painter.drawRoundedRect(badgeRect, 4, 4); // wjy: 浅色圆角终端外框和现有蓝色设备图标保持同一视觉体系。

    const qreal x = bounds.x();
    const qreal y = bounds.y();
    painter.setPen(QPen(QColor(QStringLiteral("#3A7BFC")), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(QPointF(x + 5.0, y + 5.2), QPointF(x + 8.2, y + 9.0));
    painter.drawLine(QPointF(x + 8.2, y + 9.0), QPointF(x + 5.0, y + 12.8));

    painter.setPen(QPen(QColor(QStringLiteral("#F5C542")), 2.2, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(x + 10.2, y + 12.6), QPointF(x + 13.3, y + 12.6)); // wjy: 黄色短光标用于强调“运行中”，并和绿色在线状态点区分语义。
    painter.restore();
}

QColor remoteControlCountAccent(int count)
{
    // wjy: 1 至 10 路控制端各用一档色相，从产品主蓝逐步升温到深红，一眼区分占用强度。
    switch (qBound(1, count, 10)) {
    case 1: return QColor(QStringLiteral("#3A7BFC"));
    case 2: return QColor(QStringLiteral("#2563EB"));
    case 3: return QColor(QStringLiteral("#0B66DD"));
    case 4: return QColor(QStringLiteral("#0891B2"));
    case 5: return QColor(QStringLiteral("#0D9488"));
    case 6: return QColor(QStringLiteral("#16A34A"));
    case 7: return QColor(QStringLiteral("#CA8A04"));
    case 8: return QColor(QStringLiteral("#EA580C"));
    case 9: return QColor(QStringLiteral("#DC2626"));
    case 10:
    default: return QColor(QStringLiteral("#991B1B"));
    }
}

QColor remoteControlCountFill(int count)
{
    // wjy: 低并发保持近白底；高并发改用浅色底，避免只有描边变化、人数难辨。
    switch (qBound(1, count, 10)) {
    case 1: return QColor(QStringLiteral("#F8FBFF"));
    case 2: return QColor(QStringLiteral("#EEF4FF"));
    case 3: return QColor(QStringLiteral("#E8F1FF"));
    case 4: return QColor(QStringLiteral("#E6F8FB"));
    case 5: return QColor(QStringLiteral("#E6F7F4"));
    case 6: return QColor(QStringLiteral("#EAF8EE"));
    case 7: return QColor(QStringLiteral("#FFF8E6"));
    case 8: return QColor(QStringLiteral("#FFF1E8"));
    case 9: return QColor(QStringLiteral("#FEECEC"));
    case 10:
    default: return QColor(QStringLiteral("#FCE8E8"));
    }
}

void drawRemoteControlCountIcon(QPainter& painter, const QRectF& bounds, int controllerCount)
{
    const int count = qBound(1, controllerCount, 10);
    const QColor accent = remoteControlCountAccent(count);
    const QColor fill = remoteControlCountFill(count);

    painter.save(); // wjy: 与脚本运行徽标一样隔离画笔状态，保证设备行其它元素不受影响。
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QRectF badgeRect = bounds.adjusted(0.55, 0.55, -0.55, -0.55);
    // wjy: 1-10 统一圆角徽标外形；人数只用数字区分，色相随人数升温辅助扫读。
    painter.setPen(QPen(accent, count >= 8 ? 1.35 : 1.1));
    painter.setBrush(fill);
    painter.drawRoundedRect(badgeRect, 4, 4);

    QFont countFont(QStringLiteral("Microsoft YaHei UI"));
    // wjy: 单位数 1-9 用 11px 居中；10 用 9px 保证两位数仍完整落在 18px 徽标内。
    countFont.setPixelSize(count >= 10 ? 9 : 11);
    countFont.setBold(true);
    painter.setFont(countFont);
    painter.setPen(accent);
    painter.drawText(badgeRect, Qt::AlignCenter, QString::number(count));

    painter.restore();
}
// ===end====

void drawRemoteBadge(QPainter& painter, int x, int y)
{
    painter.drawPixmap(
        QRect(x, y, 26, 16),
        uupix(QStringLiteral("titlebar/remote_badge.svg")));
}

void drawRoundedDesktopImage(QPainter& painter, const QRectF& target, const QPixmap& pixmap, qreal radius)
{
    QPainterPath path;
    path.addRoundedRect(target, radius, radius);

    QRect source = pixmap.rect();
    const qreal targetRatio = target.width() / target.height();
    const int coverHeight = qRound(source.width() / targetRatio);
    if (coverHeight < source.height()) {
        source.setY(20);
        source.setHeight(coverHeight);
    }

    painter.save();
    painter.setClipPath(path);
    painter.drawPixmap(target.toRect(), pixmap, source);
    painter.restore();
}

void drawUiIcon(QPainter& painter, const QRect& target, const QString& name)
{
    painter.drawPixmap(target, uupix(QStringLiteral("titlebar/") + name));
}

// =====wjy====
void drawRotatedUiIcon(
    QPainter& painter,
    const QRect& target,
    const QString& name,
    qreal rotationDegrees,
    const QPointF& normalizedPivot = QPointF(0.5, 0.5))
{
    const QPointF rotationCenter(
        target.x() + target.width() * normalizedPivot.x(),
        target.y() + target.height() * normalizedPivot.y()); // wjy: 使用浮点坐标映射 SVG 真实圆心，避免 QRect::center 对偶数尺寸向左上取整 1px。
    painter.save();
    painter.translate(rotationCenter);
    painter.rotate(rotationDegrees);
    painter.translate(-rotationCenter); // wjy: 平移到图标圆心后旋转再还原坐标，图标只原地自转而不会绕错误支点画小圆。
    drawUiIcon(painter, target, name);
    painter.restore();
}
// ===end====

void drawResourceIcon(QPainter& painter, const QRect& target, const QString& name)
{
    painter.drawPixmap(target, uupix(name));
}

// =====wjy====
constexpr int kRemoteShortcutCount = 5; // wjy: 增加剪切板同步快捷键编辑项。
constexpr int kRemoteWindowShortcutMouseLockIndex = 5;
constexpr int kRemoteWindowShortcutRecordingIndex = 6;
constexpr int kRemoteWindowShortcutPlaybackIndex = 7;
constexpr int kShortcutEditorCount = 9; // wjy: 五个远控全局快捷键、三个远控窗口快捷键和一个删除设备快捷键共用同一编辑控件。
constexpr int kDeleteDeviceShortcutIndex = 8;
constexpr int kGlobalShortcutIdBase = 0x5100;

bool isShortcutModifierKey(int key)
{
    return key == Qt::Key_Control
        || key == Qt::Key_Shift
        || key == Qt::Key_Alt
        || key == Qt::Key_Meta; // wjy: 单独按修饰键只表示正在组合，不把它保存成完整快捷键。
}

Qt::KeyboardModifiers shortcutModifiers(Qt::KeyboardModifiers modifiers)
{
    return modifiers & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier); // wjy: 只保留用户能看到的修饰键，去掉小键盘等内部标志。
}

QKeySequence shortcutSequenceFromKeyEvent(const QKeyEvent* event)
{
    if (!event || isShortcutModifierKey(event->key())) {
        return {}; // wjy: Ctrl/Shift/Alt 单独按下时继续等待下一个真实按键。
    }
    return QKeySequence(shortcutModifiers(event->modifiers()).toInt() | event->key()); // wjy: 把当前修饰键和主键组合成 Ctrl+D 这种可保存格式。
}

QKeySequence shortcutSequenceFromText(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    QKeySequence shortcut(trimmed, QKeySequence::NativeText);
    if (shortcut.isEmpty()) {
        shortcut = QKeySequence(trimmed, QKeySequence::PortableText); // wjy: 兼容 QSettings 里保存的可移植格式。
    }
    return shortcut;
}

QString shortcutDisplayText(const QKeySequence& shortcut)
{
    return shortcut.toString(QKeySequence::NativeText); // wjy: 输入框显示系统习惯格式，例如 Ctrl+D。
}

QKeySequence remoteShortcutForIndex(int index)
{
    switch (index) {
    case 0: return platform::AppSettings::remoteShortcutFullscreen();
    case 1: return platform::AppSettings::remoteShortcutTile();
    case 2: return platform::AppSettings::remoteShortcutCloseTopmost();
    case 3: return platform::AppSettings::remoteShortcutCloseAll();
    case 4: return platform::AppSettings::remoteShortcutClipboardSync();
    case kRemoteWindowShortcutMouseLockIndex: return platform::AppSettings::remoteShortcutMouseLock();
    case kRemoteWindowShortcutRecordingIndex: return platform::AppSettings::remoteShortcutInputScriptRecording();
    case kRemoteWindowShortcutPlaybackIndex: return platform::AppSettings::remoteShortcutInputScriptPlayback();
    case kDeleteDeviceShortcutIndex: return platform::AppSettings::deviceShortcutDelete();
    default: return {};
    }
}

void setRemoteShortcutForIndex(int index, const QKeySequence& shortcut)
{
    switch (index) {
    case 0: platform::AppSettings::setRemoteShortcutFullscreen(shortcut); break;
    case 1: platform::AppSettings::setRemoteShortcutTile(shortcut); break;
    case 2: platform::AppSettings::setRemoteShortcutCloseTopmost(shortcut); break;
    case 3: platform::AppSettings::setRemoteShortcutCloseAll(shortcut); break;
    case 4: platform::AppSettings::setRemoteShortcutClipboardSync(shortcut); break;
    case kRemoteWindowShortcutMouseLockIndex: platform::AppSettings::setRemoteShortcutMouseLock(shortcut); break;
    case kRemoteWindowShortcutRecordingIndex: platform::AppSettings::setRemoteShortcutInputScriptRecording(shortcut); break;
    case kRemoteWindowShortcutPlaybackIndex: platform::AppSettings::setRemoteShortcutInputScriptPlayback(shortcut); break;
    case kDeleteDeviceShortcutIndex: platform::AppSettings::setDeviceShortcutDelete(shortcut); break;
    default: break;
    }
}

QString remoteShortcutDisplayText(int index)
{
    return shortcutDisplayText(remoteShortcutForIndex(index)); // wjy: 绘制层和真实输入框都从同一个设置源读取当前快捷键。
}

bool matchesShortcut(const QKeyEvent* event, const QKeySequence& shortcut)
{
    const QKeySequence current = shortcutSequenceFromKeyEvent(event);
    return !current.isEmpty() && !shortcut.isEmpty() && current == shortcut; // wjy: 主界面快捷键判断改为匹配用户自定义设置。
}

#if defined(Q_OS_WIN)
int combinedShortcutValue(const QKeySequence& shortcut)
{
    if (shortcut.isEmpty()) {
        return 0;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return shortcut[0].toCombined();
#else
    return shortcut[0];
#endif
}

int globalShortcutIdForIndex(int index)
{
    return kGlobalShortcutIdBase + index;
}

int shortcutIndexForGlobalShortcutId(int id)
{
    const int index = id - kGlobalShortcutIdBase;
    return index >= 0 && index < kRemoteShortcutCount ? index : -1;
}

bool virtualKeyFromQtKey(int key, UINT* virtualKey)
{
    if (!virtualKey) {
        return false;
    }
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        *virtualKey = static_cast<UINT>('A' + (key - Qt::Key_A));
        return true;
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        *virtualKey = static_cast<UINT>('0' + (key - Qt::Key_0));
        return true;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        *virtualKey = static_cast<UINT>(VK_F1 + (key - Qt::Key_F1));
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
    case Qt::Key_Print: *virtualKey = VK_SNAPSHOT; return true;
    case Qt::Key_Pause: *virtualKey = VK_PAUSE; return true;
    case Qt::Key_CapsLock: *virtualKey = VK_CAPITAL; return true;
    case Qt::Key_NumLock: *virtualKey = VK_NUMLOCK; return true;
    case Qt::Key_ScrollLock: *virtualKey = VK_SCROLL; return true;
    case Qt::Key_Semicolon: *virtualKey = VK_OEM_1; return true;
    case Qt::Key_Equal: *virtualKey = VK_OEM_PLUS; return true;
    case Qt::Key_Plus: *virtualKey = VK_OEM_PLUS; return true;
    case Qt::Key_Comma: *virtualKey = VK_OEM_COMMA; return true;
    case Qt::Key_Minus: *virtualKey = VK_OEM_MINUS; return true;
    case Qt::Key_Period: *virtualKey = VK_OEM_PERIOD; return true;
    case Qt::Key_Slash: *virtualKey = VK_OEM_2; return true;
    case Qt::Key_QuoteLeft: *virtualKey = VK_OEM_3; return true;
    case Qt::Key_BracketLeft: *virtualKey = VK_OEM_4; return true;
    case Qt::Key_Backslash: *virtualKey = VK_OEM_5; return true;
    case Qt::Key_BracketRight: *virtualKey = VK_OEM_6; return true;
    case Qt::Key_Apostrophe: *virtualKey = VK_OEM_7; return true;
    default: return false;
    }
}

bool windowsHotkeyFromShortcut(const QKeySequence& shortcut, UINT* modifiers, UINT* virtualKey)
{
    if (!modifiers || !virtualKey) {
        return false;
    }
    const int combined = combinedShortcutValue(shortcut);
    if (combined <= 0) {
        return false;
    }

    const int modifierMask = static_cast<int>(Qt::KeyboardModifierMask);
    const int key = combined & ~modifierMask;
    if (key <= 0 || isShortcutModifierKey(key) || !virtualKeyFromQtKey(key, virtualKey)) {
        return false;
    }

    const Qt::KeyboardModifiers qtModifiers = Qt::KeyboardModifiers(combined & modifierMask);
    UINT nativeModifiers = MOD_NOREPEAT;
    if (qtModifiers.testFlag(Qt::ControlModifier)) {
        nativeModifiers |= MOD_CONTROL;
    }
    if (qtModifiers.testFlag(Qt::ShiftModifier)) {
        nativeModifiers |= MOD_SHIFT;
    }
    if (qtModifiers.testFlag(Qt::AltModifier)) {
        nativeModifiers |= MOD_ALT;
    }
    if (qtModifiers.testFlag(Qt::MetaModifier)) {
        nativeModifiers |= MOD_WIN;
    }
    *modifiers = nativeModifiers;
    return true;
}

QVector<int> virtualKeysForShortcut(const QKeySequence& shortcut)
{
    QVector<int> virtualKeys;
    UINT modifiers = 0;
    UINT virtualKey = 0;
    if (!windowsHotkeyFromShortcut(shortcut, &modifiers, &virtualKey)) {
        return virtualKeys;
    }

    // =====wjy====
    const auto appendUniqueKey = [&virtualKeys](int key) {
        if (key > 0 && !virtualKeys.contains(key)) {
            virtualKeys.append(key); // wjy: 同一键码只释放一次；主键恰好是修饰键变体时也不会生成重复控制消息。
        }
    };
    if ((modifiers & MOD_CONTROL) != 0) {
        appendUniqueKey(VK_CONTROL);
        appendUniqueKey(VK_LCONTROL);
        appendUniqueKey(VK_RCONTROL); // wjy: 被控端按精确虚拟键码持有按键，通用Ctrl不能替代实际转发的左/右Ctrl抬键。
    }
    if ((modifiers & MOD_SHIFT) != 0) {
        appendUniqueKey(VK_SHIFT);
        appendUniqueKey(VK_LSHIFT);
        appendUniqueKey(VK_RSHIFT); // wjy: Shift同样覆盖通用和左右变体，未由本会话按下的额外KeyUp会被被控端状态机忽略。
    }
    if ((modifiers & MOD_ALT) != 0) {
        appendUniqueKey(VK_MENU);
        appendUniqueKey(VK_LMENU);
        appendUniqueKey(VK_RMENU); // wjy: Alt/Menu保持与Ctrl一致的精确释放语义，防止自定义快捷键留下远端修饰状态。
    }
    if ((modifiers & MOD_WIN) != 0) {
        appendUniqueKey(VK_LWIN);
        appendUniqueKey(VK_RWIN);
    }
    appendUniqueKey(static_cast<int>(virtualKey));
    // ===end====
    return virtualKeys;
}
#endif

class ShortcutKeyEdit final : public QLineEdit {
public:
    explicit ShortcutKeyEdit(int shortcutIndex, QWidget* parent = nullptr)
        : QLineEdit(parent)
        , m_shortcutIndex(shortcutIndex)
    {
        setReadOnly(true);
        setAlignment(Qt::AlignCenter);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }

    void setCommittedText(const QString& text)
    {
        m_committedText = text.trimmed(); // wjy: 记录已经保存的文本，Esc 或非法输入时可以恢复。
        setText(m_committedText);
        deselect(); // wjy: 初始化和页面刷新只同步文字，不让所有未聚焦的快捷键输入框保持蓝色全选状态。
        setCursorPosition(m_committedText.size()); // wjy: 未进入修改状态时把光标停在末尾；真正获得焦点后仍由 focusInEvent 全选。
    }

    std::function<QString(int, const QString&)> commitCallback;

protected:
    void focusInEvent(QFocusEvent* event) override
    {
        QLineEdit::focusInEvent(event);
        selectAll(); // wjy: 点击输入框后全选当前快捷键，下一次按键直接替换。
    }

    void focusOutEvent(QFocusEvent* event) override
    {
        commitCurrentText(); // wjy: 点击其它地方时立即保存并应用当前输入框里的快捷键。
        QLineEdit::focusOutEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (!event) {
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            commitCurrentText(); // wjy: 回车保存当前快捷键，并把焦点交回手绘界面。
            clearFocus();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            setText(m_committedText); // wjy: Esc 放弃本次录入，恢复上一次已保存快捷键。
            clearFocus();
            event->accept();
            return;
        }
        if (isShortcutModifierKey(event->key())) {
            event->accept();
            return; // wjy: 例如先按 Ctrl 时不保存，等待用户再按 D 形成 Ctrl+D。
        }

        const QKeySequence shortcut = shortcutSequenceFromKeyEvent(event);
        if (!shortcut.isEmpty()) {
            setText(shortcutDisplayText(shortcut)); // wjy: 录入结果直接显示在输入框里，回车或失焦时保存。
            selectAll();
            event->accept();
            return;
        }

        QLineEdit::keyPressEvent(event);
    }

private:
    void commitCurrentText()
    {
        const QString appliedText = commitCallback
            ? commitCallback(m_shortcutIndex, text().trimmed())
            : text().trimmed(); // wjy: 由 DeviceGrid 负责校验并写入 AppSettings，然后返回最终采用的显示文本。
        setCommittedText(appliedText);
    }

    int m_shortcutIndex = -1;
    QString m_committedText;
};
// ===end====

QRect minimizeRect()
{
    return QRect(shellWidth() - 84, 0, 36, kTitleBarHeight); // wjy: 最小化热区到关闭按钮左边结束，紧凑标题栏中两个窗口按钮不再重叠。
}

QRect refreshRect()
{
    return QRect(shellWidth() - 132, 0, 48, kTitleBarHeight); //刷新图标
}

QRect closeRect()
{
    return QRect(shellWidth() - 48, 0, 48, kTitleBarHeight); //关闭按钮图标
}

QRect detailPanelToggleButtonRect(bool collapsed)
{
    Q_UNUSED(collapsed)
    const int y = qMax(kTitleBarHeight + 64, shellHeight() - 40); // wjy: 底部保留 12px 间距，按钮不会抢到无边框窗口的底边缩放热区。
    return QRect(kSidebarWidth + 5, y, 32, 28); // wjy: 两种状态都使用设备栏右侧专用窄条，按钮不再覆盖设备或分组。
}

int deviceDetailHeaderX(bool detailPanelCollapsed)
{
    Q_UNUSED(detailPanelCollapsed)
    return kExpandedContentLeft; // wjy: 详情栏只在展开状态绘制，不再存在隐藏设备栏后的左移布局。
}

int deviceDetailHeaderRight(bool detailPanelCollapsed)
{
    Q_UNUSED(detailPanelCollapsed)
    return contentLeft() + contentWidth();
}

void drawDetailPanelToggleButton(QPainter& painter, bool collapsed)
{
    const QRect buttonRect = detailPanelToggleButtonRect(collapsed);
    painter.save();
    painter.setPen(QPen(QColor(QStringLiteral("#D8DEE5")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(QRectF(buttonRect), 6, 6);

    QFont buttonFont(QStringLiteral("Microsoft YaHei UI"));
    buttonFont.setPixelSize(13);
    buttonFont.setBold(true);
    painter.setFont(buttonFont);
    painter.setPen(QColor(QStringLiteral("#334155")));
    painter.drawText(QRectF(buttonRect), Qt::AlignCenter, collapsed ? QStringLiteral(">>") : QStringLiteral("<<"));
    painter.restore();
}

QRect rootDeviceDropZoneRect()
{
    return QRect(0, kTitleBarHeight, 236, 10);
}

QRect deviceDragGhostRect(const QPoint& currentPos, const QSize& bounds)
{
    constexpr int ghostWidth = 172;
    constexpr int ghostHeight = 36;
    const int ghostX = qBound(8, currentPos.x() - ghostWidth / 2, bounds.width() - ghostWidth - 8);
    const int ghostY = qBound(kTitleBarHeight+4, currentPos.y() - ghostHeight / 2, bounds.height() - ghostHeight - 8); //这是设备拖动上方限制位置
    return QRect(ghostX, ghostY, ghostWidth, ghostHeight);
}

QRect groupDragGhostRect(const QPoint& currentPos, const QSize& bounds)
{
    constexpr int ghostWidth = 188;
    constexpr int ghostHeight = 36;
    const int ghostX = qBound(8, currentPos.x() - ghostWidth / 2, bounds.width() - ghostWidth - 8);
    const int ghostY = qBound(kTitleBarHeight + 4, currentPos.y() - ghostHeight / 2, bounds.height() - ghostHeight - 8);
    return QRect(ghostX, ghostY, ghostWidth, ghostHeight);
}

QRect deviceGroupHeaderRect()//左侧设备列表开始位置
{
    return QRect(0, kSidebarContentTop, 236, 34);
}

using DeviceEntry = platform::DeviceRecord; // wjy: 设备实体类型统一由 DeviceCatalog 提供，DeviceGrid 暂时保留旧别名以分阶段迁移调用方。

// =====wjy====
struct DeviceListRow {
    enum class Type {
        Device,
        Group
    };

    Type type = Type::Device;
    int deviceIndex = -1; // wjy: 真实设备下标，只有设备行有效。
    int groupIndex = -1;  // wjy: 真实分组下标，分组行有效；分组内设备也记录所属分组。
};
// ===end====

constexpr int kDeviceGroupReservedBlankMinHeight = 30; // wjy: 列表内容很多时，底部仍至少保留 30 像素空白给右键菜单和拖回根部使用。
constexpr int kGroupNameTextX = 40; // wjy: 分组名从远程唤醒图标右侧开始显示。
constexpr int kGroupNameDisplayCharacters = 10; // wjy: 分组名显示和编辑都限制为十个字符宽度。
constexpr int kDeviceNameEditCharacters = 5; // wjy: 设备原地重命名输入框缩小到五个字符宽。
constexpr const char* kRemoteScriptFolderPath = "\\\\192.168.1.100\\广告部工具\\远程脚本文件"; // wjy: 执行脚本入口当前只浏览这个固定共享目录，真正执行逻辑后续再接。
constexpr const char* kRemoteScriptWorkPath = "%FSREMOTE_DIR%work"; // wjy: 目标设备 FSRemote.exe 所在目录下的 work 文件夹，每次执行前重建。

class RenameDeviceDialog final : public QDialog {
public:
    explicit RenameDeviceDialog(const QString& currentName, QWidget* parent = nullptr)
        : QDialog(parent)
    {
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedSize(443, 219);

        auto* title = new QLabel(zh("\xE4\xBF\xAE\xE6\x94\xB9\xE8\xAE\xBE\xE5\xA4\x87\xE5\x90\x8D\xE7\xA7\xB0"), this);
        title->setGeometry(23, 24, 180, 24);
        title->setStyleSheet(QStringLiteral(
            "QLabel{font-family:'Microsoft YaHei UI';font-size:16px;color:#000000;background:transparent;}"));

        m_nameEdit = new QLineEdit(this);
        m_nameEdit->setGeometry(23, 59, 400, 32);
        m_nameEdit->setMaxLength(40);
        m_nameEdit->setText(currentName);
        m_nameEdit->selectAll();
        m_nameEdit->setStyleSheet(QStringLiteral(
            "QLineEdit{background:#FFFFFF;border:1px solid #DADDE2;border-radius:3px;"
            "padding:0 30px 0 10px;font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
            "QLineEdit:focus{border:1px solid #006BFF;}"));
        auto* clearButton = new QToolButton(this);
        clearButton->setGeometry(395, 64, 22, 22);
        clearButton->setIcon(QIcon(QStringLiteral(":/UUGuest/resource/images/titlebar/close.svg")));
        clearButton->setIconSize(QSize(10, 10));
        clearButton->setCursor(Qt::PointingHandCursor);
        clearButton->setStyleSheet(QStringLiteral("QToolButton{border:0;background:transparent;padding:0;}"));
        clearButton->raise();
        connect(clearButton, &QToolButton::clicked, m_nameEdit, &QLineEdit::clear);

        auto* hint = new QLabel(zh("\xE6\x9C\x80\xE5\xA4\x9A\xE5\x8F\xAF\xE8\xBE\x93\xE5\x85\xA5" "40" "\xE4\xB8\xAA\xE5\xAD\x97\xE7\xAC\xA6"), this);
        hint->setGeometry(23, 99, 180, 18);
        hint->setStyleSheet(QStringLiteral(
            "QLabel{font-family:'Microsoft YaHei UI';font-size:12px;color:#666666;background:transparent;}"));

        m_countLabel = new QLabel(this);
        m_countLabel->setGeometry(367, 99, 56, 18);
        m_countLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_countLabel->setStyleSheet(QStringLiteral(
            "QLabel{font-family:'Microsoft YaHei UI';font-size:12px;color:#666666;background:transparent;}"));

        m_saveButton = new QPushButton(zh("\xE4\xBF\x9D\xE5\xAD\x98\xE6\x9B\xB4\xE6\x94\xB9"), this);
        m_saveButton->setGeometry(23, 164, 195, 31);
        m_saveButton->setCursor(Qt::PointingHandCursor);
        m_saveButton->setStyleSheet(QStringLiteral(
            "QPushButton{border:0;border-radius:3px;background:#3A7BFC;"
            "font-family:'Microsoft YaHei UI';font-size:14px;color:#FFFFFF;}"
            "QPushButton:hover{background:#2F6FEF;}"
            "QPushButton:disabled{background:#C9D0DA;color:#FFFFFF;}"));

        auto* cancelButton = new QPushButton(zh("\xE5\x8F\x96\xE6\xB6\x88"), this);
        cancelButton->setGeometry(227, 164, 196, 31);
        cancelButton->setCursor(Qt::PointingHandCursor);
        cancelButton->setStyleSheet(QStringLiteral(
            "QPushButton{border:1px solid #DADDE2;border-radius:3px;background:#FFFFFF;"
            "font-family:'Microsoft YaHei UI';font-size:14px;color:#000000;}"
            "QPushButton:hover{background:#F3F7FF;}"));

        connect(m_nameEdit, &QLineEdit::textChanged, this, &RenameDeviceDialog::updateState);
        connect(m_saveButton, &QPushButton::clicked, this, &QDialog::accept);
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
        updateState();
    }

    QString name() const
    {
        return m_nameEdit->text().trimmed();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QPainterPath path;
        path.addRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 4, 4);
        painter.setClipPath(path);
        painter.fillPath(path, Qt::white);
        painter.fillRect(QRectF(0, 139, width(), 80), QColor(QStringLiteral("#F6F8FA")));
        painter.setClipping(false);
        painter.setPen(QPen(QColor(QStringLiteral("#EEF1F5")), 1));
        painter.drawLine(QPointF(0, 138.5), QPointF(width(), 138.5));
        painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
        painter.drawPath(path);
    }

private:
    void updateState()
    {
        const int length = m_nameEdit->text().size();
        m_countLabel->setText(QStringLiteral("%1/40").arg(length));
        m_saveButton->setEnabled(!m_nameEdit->text().trimmed().isEmpty());
    }

    QLineEdit* m_nameEdit = nullptr;
    QLabel* m_countLabel = nullptr;
    QPushButton* m_saveButton = nullptr;
};

// =====wjy====
constexpr int kScriptTreeNodeTypeRole = Qt::UserRole + 1;
constexpr int kScriptTreeFolderNode = 0;
constexpr int kScriptTreeFileNode = 1;

QStringList supportedScriptSuffixes()
{
    return {
        QStringLiteral("bat"),
        QStringLiteral("cmd"),
        QStringLiteral("ps1"),
        QStringLiteral("py"),
        QStringLiteral("exe"),
    }; // wjy: 右键和脚本树只展示真正能够被远端包装器执行的入口后缀，whl 等依赖包仍作为目录内容复制。
}

bool isSupportedScriptSuffix(const QString& suffix)
{
    return supportedScriptSuffixes().contains(suffix.trimmed().toLower()); // wjy: 所有入口校验统一走同一份后缀白名单，避免菜单和执行器出现两套标准。
}

QFileInfoList scriptEntryFiles(const QString& folderPath)
{
    QDir dir(folderPath);
    QStringList filters;
    for (const QString& suffix : supportedScriptSuffixes()) {
        filters.append(QStringLiteral("*.%1").arg(suffix)); // wjy: 目录快照只收集可执行入口文件，配置和依赖文件不占用菜单动作。
    }
    return dir.entryInfoList(
        filters,
        QDir::Files,
        QDir::Name | QDir::IgnoreCase); // wjy: 同一目录中的多个入口按名称稳定排序，菜单显示顺序和后台预检保持一致。
}

QString scriptWorkspaceName(const QString& sourceFolderPath, const QString& entryScriptHash)
{
    const QDir scriptRoot(QString::fromUtf8(kRemoteScriptFolderPath));
    const QString relativeFolder = QDir::fromNativeSeparators(
        scriptRoot.relativeFilePath(QDir::cleanPath(sourceFolderPath)))
        .trimmed()
        .toLower();
    const QByteArray folderDigest = QCryptographicHash::hash(
        relativeFolder.toUtf8(),
        QCryptographicHash::Sha256)
        .toHex()
        .left(10); // wjy: 用共享根目录下的相对文件夹路径生成稳定短哈希，避免不同目录中的同名脚本共用 work 缓存。
    QString baseName = QFileInfo(QDir::cleanPath(sourceFolderPath)).fileName().trimmed();
    if (baseName.isEmpty()) {
        baseName = QStringLiteral("script"); // wjy: 根目录或异常 UNC 路径没有文件夹名时仍提供合法、可定位的工作区前缀。
    }
    baseName.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    const QString normalizedEntryHash = entryScriptHash.trimmed().toLower();
    return baseName
        + QStringLiteral("__")
        + QString::fromLatin1(folderDigest)
        + QStringLiteral("__")
        + normalizedEntryHash; // wjy: 目录路径 Hash 和入口内容 Hash 同时编码进 work 名称，任一 Hash 变化都会自然落到新工作区。
}

QFileInfoList scriptChildDirectories(const QString& folderPath)
{
    QDir dir(folderPath);
    return dir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
        QDir::Name | QDir::IgnoreCase); // wjy: 后台只读取真实目录并按名称排序，忽略符号链接避免共享目录循环递归。
}

QJsonArray scriptFolderTreeChildrenSnapshot(const QString& folderPath, int depth)
{
    constexpr int maximumScriptFolderDepth = 24;
    if (depth >= maximumScriptFolderDepth) {
        return {}; // wjy: 异常深目录在后台安全截断，避免错误共享结构无限占用线程和内存。
    }

    QJsonArray children;
    const QFileInfoList childDirectories = scriptChildDirectories(folderPath); // wjy: 此函数只在 DeviceGrid 后台任务调用，UNC 枚举不占用 UI 线程。
    for (const QFileInfo& childInfo : childDirectories) {
        QJsonObject child;
        child.insert(QStringLiteral("name"), childInfo.fileName());
        child.insert(QStringLiteral("path"), childInfo.absoluteFilePath());
        child.insert(QStringLiteral("type"), QStringLiteral("folder")); // wjy: 明确区分目录节点和入口文件节点，菜单不再把目录名误当成脚本动作。
        QJsonArray scripts;
        for (const QFileInfo& scriptInfo : scriptEntryFiles(childInfo.absoluteFilePath())) {
            QJsonObject script;
            script.insert(QStringLiteral("name"), scriptInfo.fileName());
            script.insert(QStringLiteral("path"), scriptInfo.absoluteFilePath());
            script.insert(QStringLiteral("type"), QStringLiteral("file")); // wjy: 入口动作绑定具体文件路径，用户选择哪个文件就执行哪个文件。
            scripts.append(script);
        }
        child.insert(QStringLiteral("scripts"), scripts); // wjy: 一个目录可同时展示多个 bat/cmd/ps1/py/exe 入口。
        child.insert(QStringLiteral("children"),
            scriptFolderTreeChildrenSnapshot(childInfo.absoluteFilePath(), depth + 1)); // wjy: 后台递归生成纯数据快照，禁止在线程中创建 Qt 控件。
        children.append(child);
    }
    return children;
}

QJsonObject loadScriptFolderTreeSnapshot(const QString& rootPath)
{
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("path"), rootPath);
    snapshot.insert(QStringLiteral("name"), QString::fromUtf8("远程脚本文件"));

    const QDir rootDirectory(rootPath);
    if (!rootDirectory.exists()) {
        snapshot.insert(QStringLiteral("available"), false);
        snapshot.insert(QStringLiteral("error"), QString::fromUtf8("无法访问脚本目录"));
        return snapshot; // wjy: 即使 Windows SMB 在这里等待，阻塞的也是可取消后台线程，主窗口已经显示并可正常操作。
    }

    snapshot.insert(QStringLiteral("available"), true);
    QJsonArray scripts;
    for (const QFileInfo& scriptInfo : scriptEntryFiles(rootPath)) {
        QJsonObject script;
        script.insert(QStringLiteral("name"), scriptInfo.fileName());
        script.insert(QStringLiteral("path"), scriptInfo.absoluteFilePath());
        script.insert(QStringLiteral("type"), QStringLiteral("file")); // wjy: 根共享目录下的入口文件也纳入统一菜单/树节点模型。
        scripts.append(script);
    }
    snapshot.insert(QStringLiteral("scripts"), scripts);
    snapshot.insert(QStringLiteral("children"), scriptFolderTreeChildrenSnapshot(rootPath, 0));
    return snapshot;
}

QString escapedCmdPath(const QString& path)
{
    QString escaped = QDir::toNativeSeparators(path);
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return escaped; // wjy: 放入 cmd 引号中的路径只需处理双引号，空格和中文由外层引号保护。
}

QString escapedPowerShellSingleQuoted(const QString& value)
{
    QString escaped = QDir::toNativeSeparators(value);
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return escaped; // wjy: PowerShell 单引号字符串通过两个单引号转义，中文路径由 EncodedCommand 以 Unicode 传到目标设备。
}

QString powerShellEncodedCommand(const QString& script)
{
    const QByteArray utf16LittleEndian(
        reinterpret_cast<const char*>(script.utf16()),
        script.size() * int(sizeof(ushort)));
    return QString::fromLatin1(utf16LittleEndian.toBase64()); // wjy: powershell -EncodedCommand 要求 UTF-16LE 后再 Base64，避免中文 UNC 路径经过 cmd/ssh 时乱码。
}

QString scriptOutputTempFilePath()
{
    const QString fileName = QStringLiteral("fsremote_script_output_%1_%2.log")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch());
    return QDir(QDir::tempPath()).filePath(fileName); // wjy: Each script run owns one local temp output file, decoupling SSH chunks from the painted terminal.
}

bool writeScriptOutputFile(const QString& filePath, const QString& text, QIODevice::OpenMode mode)
{
    if (filePath.trimmed().isEmpty()) {
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | mode)) {
        return false;
    }
    file.write(text.toUtf8());
    return true;
}

QString readScriptOutputFileTail(const QString& filePath, qint64 maxBytes = 256 * 1024)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    if (file.size() > maxBytes) {
        file.seek(file.size() - maxBytes);
        file.readLine(); // wjy: Drop a possibly partial first line when reading a large output tail.
    }
    return QString::fromUtf8(file.readAll());
}

QFileInfo scriptEntryFileForSelection(const QString& entryPath)
{
    const QFileInfo entryScript(QDir::cleanPath(entryPath));
    if (!entryScript.exists()
        || !entryScript.isFile()
        || !isSupportedScriptSuffix(entryScript.suffix())) {
        return {}; // wjy: 菜单已经明确选择入口文件，预检只验证该文件仍存在且后缀受支持，不再按优先级猜测其它文件。
    }
    return entryScript; // wjy: 返回用户点选的精确入口，避免同一目录多个脚本时执行错文件。
}

QString scriptEntryContentHash(const QFileInfo& scriptFile, QString* errorMessage = nullptr)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    QFile file(scriptFile.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QString::fromUtf8("无法读取入口脚本：%1").arg(file.errorString());
        }
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty()) {
            if (!file.atEnd()) {
                if (errorMessage) {
                    *errorMessage = QString::fromUtf8("读取入口脚本失败：%1").arg(file.errorString());
                }
                return {};
            }
            break;
        }
        hash.addData(chunk); // wjy: 分块计算 SHA-256，避免大脚本或可执行入口一次性占满后台线程内存。
    }
    if (file.error() != QFile::NoError) {
        if (errorMessage) {
            *errorMessage = QString::fromUtf8("读取入口脚本失败：%1").arg(file.errorString());
        }
        return {};
    }
    return QString::fromLatin1(hash.result().toHex()); // wjy: 返回完整内容 Hash，脚本版本变化时生成新的 work 目录。
}

QString scriptRunCommandForFile(const QFileInfo& scriptFile)
{
    const QString scriptName = scriptFile.fileName();
    const QString suffix = scriptFile.suffix().toLower();
    if (suffix == QStringLiteral("py")) {
        return QStringLiteral("python \"%1\"").arg(escapedCmdPath(scriptName)); // wjy: Python 脚本用目标设备命令行里的 python 解释器运行。
    }
    if (suffix == QStringLiteral("ps1")) {
        return QStringLiteral("powershell -NoProfile -ExecutionPolicy Bypass -File \"%1\"").arg(escapedCmdPath(scriptName)); // wjy: ps1 用 PowerShell 绕过本次进程策略执行。
    }
    if (suffix == QStringLiteral("bat") || suffix == QStringLiteral("cmd")) {
        return QStringLiteral("call \"%1\"").arg(escapedCmdPath(scriptName)); // wjy: 批处理用 call 执行，避免脚本返回后中断外层命令流。
    }
    if (suffix == QStringLiteral("exe")) {
        return QStringLiteral("\"%1\"").arg(escapedCmdPath(scriptName)); // wjy: exe 入口直接运行。
    }
    return {};
}
// ===end====

QString deviceDisplayName(const DeviceEntry& device)
{
    const QString name = device.name.trimmed();
    return name.isEmpty() ? device.ip.trimmed() : name;
}

bool proxyWakeCapableState(platform::DevicePresenceState state)
{
    return state == platform::DevicePresenceState::Online
        || state == platform::DevicePresenceState::Busy;
}

bool parseIpv4Address(const QString& text, quint32* outValue)
{
    if (!outValue) {
        return false;
    }

    QHostAddress address;
    if (!address.setAddress(text.trimmed()) || address.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }

    *outValue = address.toIPv4Address();
    return true;
}

bool isWildcardSubnetPattern(const QString& text)
{
    const QStringList parts = text.trimmed().split('.');
    if (parts.size() != 4 || parts.at(3).trimmed() != QStringLiteral("*")) {
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        const int value = parts.at(i).trimmed().toInt(&ok);
        if (!ok || value < 0 || value > 255) {
            return false;
        }
    }
    return true;
}

QStringList wildcardSubnetScanIps(const QString& text)
{
    QStringList result;
    const QStringList parts = text.trimmed().split('.');
    if (parts.size() != 4 || parts.at(3).trimmed() != QStringLiteral("*")) {
        return result; // wjy: 非标准 IPv4 通配网段格式时不生成扫描列表。
    }

    const QString prefix = QStringLiteral("%1.%2.%3.")
        .arg(parts.at(0).trimmed())
        .arg(parts.at(1).trimmed())
        .arg(parts.at(2).trimmed()); // wjy: 只遍历最后一段，前三段保持用户输入的网段。
    result.reserve(254);
    for (int host = 1; host <= 254; ++host) {
        result.append(prefix + QString::number(host)); // wjy: 跳过 .0 和 .255，避免扫描网络号和广播地址。
    }
    return result;
}

QStringList batchSubnetPatterns(const QString& text)
{
    QString normalized = text;
    normalized.replace(QRegularExpression(QStringLiteral("[,;，；\\r\\n\\t]+")), QStringLiteral(" "));
    return normalized.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

struct BatchAddResult {
    QString name;
    QString ip;
    QString mac;
    QString broadcastIp;
    platform::DeviceStatusInfo status; // wjy: 保留 TCP 49101 返回的完整权威状态，UI 线程通过实时归并器应用而不是直接写 Online。
};

bool isSameSubnet(const QString& targetIp, const QString& candidateIp, const QString& subnetMask)
{
    quint32 target = 0;
    quint32 candidate = 0;
    quint32 mask = 0;
    if (!parseIpv4Address(targetIp, &target)
        || !parseIpv4Address(candidateIp, &candidate)
        || !parseIpv4Address(subnetMask, &mask)) {
        return false;
    }

    return (target & mask) == (candidate & mask);
}

// =====wjy====
platform::DeviceCatalog g_deviceCatalog; // wjy: 设备、分组和快照元数据的唯一内存所有者，先通过兼容引用迁移旧 UI 调用，避免并行状态副本。
platform::DeviceCatalogRepository g_deviceCatalogRepository(g_deviceCatalog, {}); // wjy: 先创建纯内存仓储，真正路径在首次读写前注入，避免静态初始化阶段依赖 QApplication 环境。
const auto& g_devices = g_deviceCatalog.devices(); // wjy: 绘制和命中测试只能读取目录顺序，下标仅作为本次展示位置，不能直接修改权威设备数据。
const auto& g_deviceGroupNames = g_deviceCatalog.groupNames(); // wjy: 分组名称通过目录只读视图提供，写操作必须携带稳定 groupId 调用目录方法。
const auto& g_deviceGroupExpandedStates = g_deviceCatalog.groupExpandedStates(); // wjy: 展开状态只读消费，切换操作统一交给目录校验分组身份。
const auto& g_deviceGroupIds = g_deviceCatalog.groupIds(); // wjy: 稳定 groupId 与展示顺序仍保持同位，但匿名命名空间不再拥有可写容器。
bool g_hideLocalDeviceFromList = false; // wjy: 本机隐藏是当前窗口进程的展示策略，不写入共享 DeviceCatalog 快照。
platform::DeviceInfo g_localDeviceIdentityForList; // wjy: 保存设备名/IP/MAC 三种本机身份，供列表、搜索和批量新增使用同一判定。
// ===end====

// =====wjy====
QString normalizedDeviceMac(QString value)
{
    value = value.trimmed().toUpper();
    value.remove(QLatin1Char(':'));
    value.remove(QLatin1Char('-'));
    value.remove(QLatin1Char('.'));
    value.remove(QLatin1Char(' '));
    return value; // wjy: 不同来源可能使用冒号、横线或无分隔格式，去除分隔符后再比较物理地址。
}

bool deviceIdentityMatchesLocal(
    const QString& candidateName,
    const QString& candidateIp,
    const QString& candidateMac,
    const platform::DeviceInfo& localInfo)
{
    const QString normalizedCandidateIp = candidateIp.trimmed();
    const QString normalizedLocalIp = localInfo.ip.trimmed();
    if (!normalizedCandidateIp.isEmpty()
        && !normalizedLocalIp.isEmpty()
        && normalizedCandidateIp.compare(normalizedLocalIp, Qt::CaseInsensitive) == 0) {
        return true; // wjy: 当前本机 IPv4 完全一致时可以直接确认是本机设备。
    }

    const QString normalizedCandidateMac = normalizedDeviceMac(candidateMac);
    const QString normalizedLocalMac = normalizedDeviceMac(localInfo.mac);
    if (!normalizedCandidateMac.isEmpty()
        && !normalizedLocalMac.isEmpty()
        && normalizedCandidateMac == normalizedLocalMac) {
        return true; // wjy: IP 变化后仍通过稳定 MAC 识别原来的本机记录。
    }

    const QString normalizedCandidateName = candidateName.trimmed();
    const QString normalizedLocalName = localInfo.name.trimmed();
    return !normalizedCandidateName.isEmpty()
        && !normalizedLocalName.isEmpty()
        && normalizedCandidateName.compare(normalizedLocalName, Qt::CaseInsensitive) == 0; // wjy: 启动早期尚未读取网卡时使用 Windows 设备名兜底，保证列表不会短暂显示本机。
}

bool deviceRecordMatchesLocal(const DeviceEntry& device)
{
    return deviceIdentityMatchesLocal(device.name, device.ip, device.mac, g_localDeviceIdentityForList); // wjy: 目录记录和批量扫描结果共享同一身份判定规则。
}

bool batchAddResultMatchesLocal(const BatchAddResult& result, const platform::DeviceInfo& localInfo)
{
    return deviceIdentityMatchesLocal(result.name, result.ip, result.mac, localInfo); // wjy: 批量新增回调在写目录前过滤本机，不依赖列表当前是否已有对应记录。
}

bool deviceHiddenByLocalPreference(const DeviceEntry& device)
{
    return g_hideLocalDeviceFromList && deviceRecordMatchesLocal(device); // wjy: 开关关闭时所有记录照常显示，开启时只隐藏确认属于本机的设备。
}

int localDeviceCatalogIndex()
{
    for (int index = 0; index < g_devices.size(); ++index) {
        if (deviceRecordMatchesLocal(g_devices.at(index))) {
            return index; // wjy: 关闭隐藏开关时优先复用目录中已有本机实体，避免重复添加相同设备。
        }
    }
    return -1;
}
// ===end====

// =====wjy====
bool deviceIndexesAreAllUngrouped(const QSet<int>& deviceIndexes)
{
    if (deviceIndexes.isEmpty()) {
        return false; // wjy: 没有有效拖拽设备时不能把顶部危险区域误显示为删除入口。
    }
    for (const int deviceIndex : deviceIndexes) {
        if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
            return false; // wjy: 同步或删除造成下标失效时采用安全行为，不允许通过旧拖拽快照删除其它设备。
        }
        const auto& device = g_devices.at(deviceIndex);
        if (!device.groupId.trimmed().isEmpty() || !device.group.trimmed().isEmpty()) {
            return false; // wjy: 只要本次拖动中仍有分组设备，顶部区域就继续执行“移出分组”，避免批量误删。
        }
    }
    return true; // wjy: 本次拖动的全部设备都位于根部时，顶部区域才允许切换为删除设备。
}
// ===end====

int deviceIndexForIp(const QString& ip)
{
    const QString trimmedIp = ip.trimmed();
    if (trimmedIp.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < g_devices.size(); ++i) {
        if (g_devices.at(i).ip.trimmed() == trimmedIp) {
            return i; // wjy: 按 IP 定位设备记录，批量扫描补齐 MAC 时复用同一份去重依据。
        }
    }
    return -1;
}

bool deviceIpExists(const QString& ip)
{
    return deviceIndexForIp(ip) >= 0; // wjy: 按 IP 去重，避免批量扫描把已有设备再次写入 devices.json。
}

QString deviceStorePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data/devices.json"));
}

// =====wjy====
void saveDevices()
{
    // =====wjy====
    g_deviceCatalogRepository.setStorePath(deviceStorePath());
    g_deviceCatalogRepository.saveLocal(); // wjy: 原子保存和“仅本地修改才提交同步”的判断统一由仓储处理，UI 不再维护同步生命周期标志。
    // ===end====
}

void loadDevices()
{
    // =====wjy====
    g_deviceCatalogRepository.setStorePath(deviceStorePath());
    g_deviceCatalogRepository.loadLocal(); // wjy: 文件读取、旧数组格式迁移和规范化重写由仓储一次完成，UI 只消费目录结果。
    // ===end====
}

QStringList deviceNames();
int visibleDeviceListRowCount(); // wjy: 统计“我的设备”下拉框里的可见行数，包含设备行和新建分组行。
// =====wjy====self1
bool deviceGroupExpandedForIndex(int groupIndex);
QVector<DeviceListRow> visibleDeviceRows(); // wjy: 仅供行数/几何统计使用；真实绘制和命中必须传入当前状态缓存。
QVector<DeviceListRow> visibleDeviceRows(const QHash<QString, platform::DevicePresenceState>& deviceStatuses); // wjy: 绘制、点击和拖拽共享同一份在线优先排序快照。
int rootDeviceRowCount();
int visualRowIndexForDeviceIndex(int deviceIndex, const QHash<QString, platform::DevicePresenceState>& deviceStatuses); // wjy: 状态变化会重排设备，定位行号时必须使用当前缓存。
int visualRowIndexForGroupIndex(int groupIndex, const QHash<QString, platform::DevicePresenceState>& deviceStatuses); // wjy: 分组定位和设备列表使用同一排序上下文。
QRect visibleDeviceRowRect(int rowIndex);
int visibleDeviceListContentHeight();
int visibleDeviceListViewportHeight(bool deviceGroupExpanded);
int maxDeviceListScrollOffset();
QRect deviceListViewportRect(bool deviceGroupExpanded);
QRect deviceListScrollbarTrackRect(); // wjy: 绘制与鼠标拖拽共用同一条设备列表滚动轨道。
QRect deviceListScrollbarThumbRect(int scrollOffset); // wjy: 按当前列表偏移计算滑块位置，避免视觉和命中区域错位。
int deviceListScrollOffsetForThumbTop(int thumbTop); // wjy: 把拖拽后的滑块顶部位置映射回完整列表滚动范围。
QRect scrolledVisibleDeviceRowRect(int rowIndex, int scrollOffset);
QRect scrolledDeviceGroupReservedBlankRect(int scrollOffset);
QRect groupNameTextRect(int rowY, const QFont& font);
QRect groupNameEditRect(int rowY);
QRect deviceNameEditRect(int rowY, bool insideGroup);
// =====end====self1
// QStringList deviceNames()
// {
//     QStringList names;
//     names.reserve(g_devices.size());
//     for (const DeviceEntry& device : g_devices) {
//         names.append(deviceDisplayName(device));
//     }
//     return names;
// }

// int visibleDeviceListRowCount()
// {
// // =====wjy====
//     return deviceNames().size() + g_deviceGroupNames.size(); // wjy: 当前可见行 = 真实设备行数量 + 新建分组行数量。
// // ===end====
// }

// bool deviceGroupExpandedForIndex(int groupIndex)
// {
// // =====wjy====
//     return groupIndex >= 0
//         && (groupIndex >= g_deviceGroupExpandedStates.size() || g_deviceGroupExpandedStates.at(groupIndex)); // wjy: 分组状态缺失时默认展开，避免状态数组不同步导致看不到展开箭头。
// // ===end====
// }

// int visualRowIndexForGroupIndex(int groupIndex)
// {
// // =====wjy====
//     if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
//         return -1; // wjy: 分组下标无效时返回 -1，调用方据此放弃显示输入框。
//     }
//     return deviceNames().size() + groupIndex; // wjy: 当前阶段分组行固定排在所有设备行后面。
// // ===end====
// }
// =====wjy====self
QStringList deviceNames()
{
    QStringList names;
    names.reserve(g_devices.size());
    for (const DeviceEntry& device : g_devices) {
        if (deviceHiddenByLocalPreference(device)) {
            continue; // wjy: 所有基于设备名称数量的旧布局入口也必须排除隐藏本机。
        }
        names.append(deviceDisplayName(device));
    }
    return names;
}

bool deviceGroupExpandedForIndex(int groupIndex)
{
    // =====wjy====
    return groupIndex >= 0
           && (groupIndex >= g_deviceGroupExpandedStates.size() || g_deviceGroupExpandedStates.at(groupIndex));
    // ===end====
}

// =====wjy====
int deviceGroupIndexByName(const QString& groupName)
{
    const QString normalizedGroupName = groupName.trimmed();
    if (normalizedGroupName.isEmpty()) {
        return -1;
    }

    for (int i = 0; i < g_deviceGroupNames.size(); ++i) {
        if (g_deviceGroupNames.at(i).trimmed() == normalizedGroupName) {
            return i;
        }
    }

    return -1;
}

platform::DevicePresenceState devicePresenceForSorting(
    int deviceIndex,
    const QHash<QString, platform::DevicePresenceState>& deviceStatuses)
{
    if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return platform::DevicePresenceState::Unknown; // wjy: 失效下标按非在线处理，排序阶段不访问越界设备。
    }
    const QString ip = g_devices.at(deviceIndex).ip.trimmed(); // wjy: 状态缓存和设备列表继续以规范化 IP 关联，不引入第二套状态来源。
    if (ip.isEmpty()) {
        return platform::DevicePresenceState::Offline;
    }
    return deviceStatuses.value(ip, platform::DevicePresenceState::Unknown); // wjy: 尚未收到心跳或探测结果的设备归入非在线分区。
}

QVector<int> sortedDeviceIndexesForGroup(
    int groupIndex,
    const QHash<QString, platform::DevicePresenceState>& deviceStatuses)
{
    QVector<int> deviceIndexes;
    deviceIndexes.reserve(g_devices.size());
    const QString groupName = groupIndex >= 0 && groupIndex < g_deviceGroupNames.size()
        ? g_deviceGroupNames.at(groupIndex).trimmed()
        : QString();

    for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        if (deviceHiddenByLocalPreference(g_devices.at(deviceIndex))) {
            continue; // wjy: 本机在进入分组、在线优先和自然排序之前就被过滤，不产生任何可见设备行。
        }
        if (groupIndex < 0) {
            if (deviceGroupIndexByName(g_devices.at(deviceIndex).group) < 0) {
                deviceIndexes.append(deviceIndex);
            }
            continue;
        }

        if (g_devices.at(deviceIndex).group.trimmed() == groupName) {
            deviceIndexes.append(deviceIndex);
        }
    }

    const ui::DeviceListNaturalLess naturalLess; // wjy: 同一分组的一轮排序复用一个 QCollator，数字按数值、英文按字母比较。
    std::stable_sort(deviceIndexes.begin(), deviceIndexes.end(), [&](int leftIndex, int rightIndex) {
        const ui::DeviceListSortItem left{
            deviceDisplayName(g_devices.at(leftIndex)),
            devicePresenceForSorting(leftIndex, deviceStatuses),
            leftIndex,
        }; // wjy: 每台设备把展示名、实时状态和稳定源下标组合成排序键。
        const ui::DeviceListSortItem right{
            deviceDisplayName(g_devices.at(rightIndex)),
            devicePresenceForSorting(rightIndex, deviceStatuses),
            rightIndex,
        };
        return naturalLess(left, right); // wjy: 第一关键字是在线状态，同一状态分区内再执行自然名称排序。
    });
    return deviceIndexes;
}

QVector<DeviceListRow> visibleDeviceRows(
    const QHash<QString, platform::DevicePresenceState>& deviceStatuses)
{
    QVector<DeviceListRow> rows;
    rows.reserve(g_devices.size() + g_deviceGroupNames.size());

    // 1. 先显示没有分组的设备。
    // 如果设备 group 指向一个已经不存在的分组，也先显示在根部，避免设备消失。
    for (int deviceIndex : sortedDeviceIndexesForGroup(-1, deviceStatuses)) {
        rows.append({DeviceListRow::Type::Device, deviceIndex, -1});
    }

    // 2. 再显示分组行。
    // 如果分组展开，就在分组行下面显示属于它的设备。
    for (int groupIndex = 0; groupIndex < g_deviceGroupNames.size(); ++groupIndex) {
        rows.append({DeviceListRow::Type::Group, -1, groupIndex});

        if (!deviceGroupExpandedForIndex(groupIndex)) {
            continue;
        }

        for (int deviceIndex : sortedDeviceIndexesForGroup(groupIndex, deviceStatuses)) {
            rows.append({DeviceListRow::Type::Device, deviceIndex, groupIndex});
        }
    }

    return rows;
}

QVector<DeviceListRow> visibleDeviceRows()
{
    static const QHash<QString, platform::DevicePresenceState> emptyDeviceStatuses; // wjy: 行数和几何只关心元素数量，不依赖在线排序结果。
    return visibleDeviceRows(emptyDeviceStatuses);
}

int firstUnhiddenDeviceIndex()
{
    for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        if (!deviceHiddenByLocalPreference(g_devices.at(deviceIndex))) {
            return deviceIndex; // wjy: 初始选择、同步回退和关闭隐藏开关只跳过本机过滤，不改变折叠分组原有的详情选择行为。
        }
    }
    return -1;
}
// ===end====

int visibleDeviceListRowCount()
{
    // =====wjy====
    return visibleDeviceRows().size();
    // ===end====
}

int rootDeviceRowCount()
{
// =====wjy====
    int count = 0; // wjy: 统计当前显示在“我的设备”根部的设备数量，用来决定空白区应该插在哪。
    for (const DeviceListRow& row : visibleDeviceRows()) {
        if (row.type == DeviceListRow::Type::Device && row.groupIndex < 0) {
            ++count; // wjy: 只有无分组设备才算根部行，分组行和分组内设备不算。
        }
    }
    return count;
// ===end====
}

int visualRowIndexForDeviceIndex(
    int deviceIndex,
    const QHash<QString, platform::DevicePresenceState>& deviceStatuses)
{
    const QVector<DeviceListRow> rows = visibleDeviceRows(deviceStatuses); // wjy: 设备定位必须和当前在线优先显示顺序完全一致。
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const DeviceListRow& row = rows.at(rowIndex);
        if (row.type == DeviceListRow::Type::Device && row.deviceIndex == deviceIndex) {
            return rowIndex;
        }
    }

    return -1;
}

int visualRowIndexForGroupIndex(
    int groupIndex,
    const QHash<QString, platform::DevicePresenceState>& deviceStatuses)
{
    // =====wjy====
    const QVector<DeviceListRow> rows = visibleDeviceRows(deviceStatuses); // wjy: 分组行号也从同一实时排序快照中解析，避免设备重排后编辑框错位。
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const DeviceListRow& row = rows.at(rowIndex);
        if (row.type == DeviceListRow::Type::Group && row.groupIndex == groupIndex) {
            return rowIndex;
        }
    }

    return -1;
    // ===end====
}
// ===end====self

QSet<int> deviceBadgeIndexes()
{
    return {};
}

QRect settingsLocalInfoCardRect(bool expanded);

QString infoValueText(const QString& value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("--") : trimmed;
}

QRect localInfoFieldRect(int index)
{
    if (index < 0 || index >= 6) {
        return {};
    }

    const int column = index % 2;
    const int row = index / 2;
    const QRect card = settingsLocalInfoCardRect(true);
    const int columnGap = 24;
    const int columnWidth = (card.width() - 68 - columnGap) / 2;
    const int labelX = card.x() + 34 + column * (columnWidth + columnGap);
    return QRect(labelX + 70, card.y() + 56 + row * 32, qMax(120, columnWidth - 116), 26);
}

QRect localInfoLabelRect(int index)
{
    if (index < 0 || index >= 6) {
        return {};
    }

    const int column = index % 2;
    const int row = index / 2;
    const QRect card = settingsLocalInfoCardRect(true);
    const int columnGap = 24;
    const int columnWidth = (card.width() - 68 - columnGap) / 2;
    const int labelX = card.x() + 34 + column * (columnWidth + columnGap);
    return QRect(labelX, card.y() + 56 + row * 32, 64, 26);
}

QRect localInfoCopyButtonRect(int index)
{
    const QRect field = localInfoFieldRect(index);
    return QRect(field.right() + 8, field.y(), 38, 26);
}

QRect settingsLocalInfoHeaderRect()
{
    // =====wjy====
    const int top = (platform::UpdateService::canPublishCurrentBuild() ? 892 : 816)
        + (kDetailScriptPanelTop - 120); // wjy: 新增隐藏本机卡片后，壁纸、回撤、发布和本机信息整体下移一行并继续保持连续布局。
    return QRect(contentLeft(), top, contentWidth(), 44);
    // ===end====
}

QRect settingsLocalInfoCardRect(bool expanded)
{
    const QRect header = settingsLocalInfoHeaderRect(); // wjy: 卡片和标题共用动态顶部，发布区域隐藏后下面所有设置项会连续上移。
    return QRect(header.x(), header.y(), header.width(), expanded ? 152 : 44);
}

QRect settingsScrollViewportRect()
{
    return QRect(contentLeft(), kDetailScriptPanelTop, contentWidth(), qMax(220, deviceDetailBottomBoundaryTop() - kDetailScriptPanelTop - kDetailScriptPanelBottomGap)); // wjy: 设置页内容视口和设备详情页主体区域使用同一顶部与底部安全边界。
}

QRect settingsVerticalScrollbarTrackRect()
{
    const QRect viewport = settingsScrollViewportRect();
    return QRect(viewport.right() + 9, viewport.y() + 8, 5, qMax(1, viewport.height() - 16));
}

QRect settingsVerticalScrollbarThumbRect(int scrollOffset, int maxScrollOffset)
{
    const QRect track = settingsVerticalScrollbarTrackRect();
    if (maxScrollOffset <= 0) {
        return {};
    }

    const int contentHeight = settingsScrollViewportRect().height() + maxScrollOffset;
    const int thumbHeight = qMax(48, track.height() * settingsScrollViewportRect().height() / qMax(1, contentHeight));
    const int travel = qMax(0, track.height() - thumbHeight);
    const int thumbY = track.y() + (travel * qBound(0, scrollOffset, maxScrollOffset)) / maxScrollOffset;
    return QRect(track.x(), thumbY, track.width(), thumbHeight);
}

// =====wjy====
int settingsScrollOffsetForThumbTop(int thumbTop, int maxScrollOffset)
{
    if (maxScrollOffset <= 0) {
        return 0; // wjy: 内容未溢出时不允许滚动，避免轨道点击导致无效除法。
    }

    const QRect track = settingsVerticalScrollbarTrackRect();
    const QRect thumb = settingsVerticalScrollbarThumbRect(0, maxScrollOffset);
    const int thumbTravel = qMax(0, track.height() - thumb.height());
    if (thumbTravel <= 0) {
        return 0; // wjy: 滑块已经占满轨道时没有可映射的移动距离。
    }

    const int relativeTop = qBound(0, thumbTop - track.y(), thumbTravel); // wjy: 轨道上下空白点击和拖拽越界都统一夹到首尾位置。
    return static_cast<int>((static_cast<qint64>(relativeTop) * maxScrollOffset + thumbTravel / 2) / thumbTravel); // wjy: 使用四舍五入让最后一个像素准确对应最大设置页滚动偏移。
}
// ===end====

QRect settingsAddDeviceHeaderRect(bool localInfoExpanded)
{
    const QRect localCard = settingsLocalInfoCardRect(localInfoExpanded);
    return QRect(contentLeft(), localCard.bottom() + 13, contentWidth(), 44);
}

QRect settingsAddDeviceCardRect(bool localInfoExpanded, bool addDeviceExpanded)
{
    const QRect header = settingsAddDeviceHeaderRect(localInfoExpanded);
    return QRect(header.x(), header.y(), header.width(), addDeviceExpanded ? 352 : 44);
}

QRect settingsAddDeviceIpEditRect(bool localInfoExpanded)
{
    const int top = settingsAddDeviceHeaderRect(localInfoExpanded).y();
    const int fieldWidth = qMax(180, (contentWidth() - 96) / 2);
    return QRect(contentLeft() + 34, top + 160, fieldWidth, 34);
}

QRect settingsAddDeviceNameEditRect(bool localInfoExpanded)
{
    const int top = settingsAddDeviceHeaderRect(localInfoExpanded).y();
    const int fieldWidth = qMax(180, (contentWidth() - 96) / 2);
    return QRect(contentLeft() + 62 + fieldWidth, top + 160, fieldWidth, 34);
}

QRect settingsAddDeviceMacEditRect(bool localInfoExpanded)
{
    const int top = settingsAddDeviceHeaderRect(localInfoExpanded).y();
    const int fieldWidth = qMax(180, (contentWidth() - 96) / 2);
    return QRect(contentLeft() + 34, top + 242, fieldWidth, 34);
}

QRect settingsAddDeviceRemarkEditRect(bool localInfoExpanded)
{
    const int top = settingsAddDeviceHeaderRect(localInfoExpanded).y();
    const int fieldWidth = qMax(180, (contentWidth() - 96) / 2);
    return QRect(contentLeft() + 62 + fieldWidth, top + 242, fieldWidth, 34);
}

QRect settingsAddDeviceSaveButtonRect(bool localInfoExpanded);

QRect settingsAddDeviceCancelButtonRect(bool localInfoExpanded)
{
    const int top = settingsAddDeviceHeaderRect(localInfoExpanded).y();
    return QRect(settingsAddDeviceSaveButtonRect(localInfoExpanded).x() - 140, top + 300, 124, 34);
}

QRect settingsAddDeviceSaveButtonRect(bool localInfoExpanded)
{
    const int top = settingsAddDeviceHeaderRect(localInfoExpanded).y();
    return QRect(contentLeft() + contentWidth() - 158, top + 300, 124, 34);
}

int settingsContentBottom(bool localInfoExpanded, bool addDeviceExpanded)
{
    return settingsAddDeviceCardRect(localInfoExpanded, addDeviceExpanded).bottom() + 16;
}

int maxSettingsScrollOffset(bool localInfoExpanded, bool addDeviceExpanded)
{
    return qMax(0, settingsContentBottom(localInfoExpanded, addDeviceExpanded) - settingsScrollViewportRect().bottom());
}

// =====wjy====
int keyboardShortcutMaxScrollOffset(); // wjy: 键盘页布局快照在快捷键几何函数定义前使用前置声明，避免回退到常规页滚动上限。

ui::SettingsLayoutSnapshot settingsLayoutSnapshot(
    bool localInfoExpanded,
    bool addDeviceExpanded,
    int requestedScrollOffset,
    bool keyboardSelected = false)
{
    const int maxScrollOffset = keyboardSelected
        ? keyboardShortcutMaxScrollOffset()
        : maxSettingsScrollOffset(localInfoExpanded, addDeviceExpanded);
    return ui::SettingsLayoutSnapshot(
        settingsScrollViewportRect(),
        maxScrollOffset,
        requestedScrollOffset); // wjy: DeviceGrid 只提供内容相关输入，滚动换算和命中规则交给独立辅助类。
}
// ===end====

QString localInfoLabelText(int index)
{
    switch (index) {
    case 0: return zh("\xE8\xAE\xBE\xE5\xA4\x87\xE5\x90\x8D\xE7\xA7\xB0");
    case 1: return QStringLiteral("IPv4");
    case 2: return QStringLiteral("MAC");
    case 3: return zh("\xE5\xB9\xBF\xE6\x92\xAD\xE5\x9C\xB0\xE5\x9D\x80");
    case 4: return zh("\xE5\xAD\x90\xE7\xBD\x91\xE6\x8E\xA9\xE7\xA0\x81");
    case 5: return zh("\xE9\xBB\x98\xE8\xAE\xA4\xE7\xBD\x91\xE5\x85\xB3");
    default: return {};
    }
}

QString localInfoValueText(const platform::DeviceInfo& info, int index)
{
    switch (index) {
    case 0: return infoValueText(info.name);
    case 1: return infoValueText(info.ip);
    case 2: return infoValueText(info.mac);
    case 3: return infoValueText(info.broadcastIp);
    case 4: return infoValueText(info.subnetMask);
    case 5: return infoValueText(info.gateway);
    default: return QStringLiteral("--");
    }
}

QRect deviceRowRect(int index) //设备行
{
    return QRect(4, kSidebarContentTop  + index * 40, 223, 36);
}

QRect visibleDeviceRowRect(int rowIndex)
{
// =====wjy====
    return deviceRowRect(rowIndex); // wjy: 空白区改到列表最下面，设备和分组行按自然顺序连续排列。
// ===end====
}

int deviceGroupReservedBlankHeight()
{
// =====wjy====
    const int blankTop = kSidebarContentTop + visibleDeviceListRowCount() * 40; // wjy: 空白区从最后一个设备/分组行下面开始。
    return qMax(kDeviceGroupReservedBlankMinHeight, shellHeight() - blankTop); // wjy: 行少时填满到左侧栏底部，行多时保留最小空白并允许滚动。
// ===end====
}

int visibleDeviceListContentHeight()
{
// =====wjy====
    return visibleDeviceListRowCount() * 40 + deviceGroupReservedBlankHeight(); // wjy: 列表真实内容高度 = 所有可见行高度 + 动态根部空白高度。
// ===end====
}

int visibleDeviceListViewportHeight(bool deviceGroupExpanded)
{
// =====wjy====
    if (!deviceGroupExpanded) {
        return 0; // wjy: “我的设备”收起时，内部列表视口高度为 0，下面栏目贴着标题显示。
    }

    return qMin(visibleDeviceListContentHeight(), qMax(0, shellHeight() - kSidebarContentTop)); // wjy: 展开时最多占到左侧栏底部，超出的设备和分组通过滚轮查看。
// ===end====
}

int maxDeviceListScrollOffset()
{
// =====wjy====
    return qMax(0, visibleDeviceListContentHeight() - visibleDeviceListViewportHeight(true)); // wjy: 最大滚动距离 = 真实内容高度 - 可见视口高度，小于 0 时不需要滚动。
// ===end====
}

QRect deviceListViewportRect(bool deviceGroupExpanded)
{
// =====wjy====
    return QRect(0, deviceGroupHeaderRect().top(), 236, visibleDeviceListViewportHeight(deviceGroupExpanded)); // wjy: 去掉“我的设备”标题后，设备和分组直接从原标题位置开始绘制和命中。
// ===end====
}

// =====wjy====
QRect deviceListScrollbarTrackRect()
{
    const QRect viewport = deviceListViewportRect(true); // wjy: 滚动条始终基于设备列表展开时的真实可见高度计算。
    return QRect(231, viewport.y() + 5, 5, qMax(1, viewport.height() - 10)); // wjy: 保持原来的 5 像素轨道位置，让新拖拽命中与现有绘制完全一致。
}

QRect deviceListScrollbarThumbRect(int scrollOffset)
{
    const int maxScrollOffset = maxDeviceListScrollOffset();
    const QRect viewport = deviceListViewportRect(true);
    if (maxScrollOffset <= 0 || viewport.height() <= 0) {
        return {}; // wjy: 内容未溢出时没有可拖动滑块，鼠标逻辑也不会误命中。
    }

    const QRect track = deviceListScrollbarTrackRect();
    const int thumbHeight = qMax(28, track.height() * viewport.height() / qMax(1, visibleDeviceListContentHeight())); // wjy: 继续沿用按可见比例计算且最小 28 像素的滑块高度。
    const int thumbTravel = qMax(0, track.height() - thumbHeight);
    const int boundedOffset = qBound(0, scrollOffset, maxScrollOffset); // wjy: 布局变化后旧偏移也先夹紧，避免滑块绘制跑出轨道。
    const int thumbY = track.y() + static_cast<int>(static_cast<qint64>(thumbTravel) * boundedOffset / maxScrollOffset); // wjy: 使用 64 位中间值，长列表映射不会整数乘法溢出。
    return QRect(track.x(), thumbY, track.width(), thumbHeight);
}

int deviceListScrollOffsetForThumbTop(int thumbTop)
{
    const int maxScrollOffset = maxDeviceListScrollOffset();
    if (maxScrollOffset <= 0) {
        return 0; // wjy: 内容不再溢出时直接回到顶部，避免除以零。
    }

    const QRect track = deviceListScrollbarTrackRect();
    const QRect thumb = deviceListScrollbarThumbRect(0); // wjy: 滑块高度与偏移无关，用顶部位置即可取得当前可移动距离。
    const int thumbTravel = qMax(0, track.height() - thumb.height());
    if (thumbTravel <= 0) {
        return 0;
    }

    const int relativeTop = qBound(0, thumbTop - track.y(), thumbTravel); // wjy: 鼠标拖到轨道上下之外时固定到首尾位置。
    return static_cast<int>((static_cast<qint64>(relativeTop) * maxScrollOffset + thumbTravel / 2) / thumbTravel); // wjy: 四舍五入的比例映射确保首端为 0、末端精确等于最大偏移。
}
// ===end====

QRect scrolledVisibleDeviceRowRect(int rowIndex, int scrollOffset)
{
// =====wjy====
    QRect rowRect = visibleDeviceRowRect(rowIndex); // wjy: 先拿未滚动时的真实行位置。
    rowRect.translate(0, -scrollOffset); // wjy: 再减去滚动偏移，得到当前屏幕上的显示位置。
    return rowRect;
// ===end====
}

//新增下方矩形计算公式
QRect deviceGroupReservedBlankRect() //底部空白区
{
// =====wjy====
    const int blankTop = kSidebarContentTop  + visibleDeviceListRowCount() * 40; // wjy: 空白区固定放到所有设备和分组行后面，不再插在无分组设备与第一个分组之间。
    return QRect(4, blankTop, 232, deviceGroupReservedBlankHeight()); // wjy: 这个空白区行少时直接铺到侧栏底部，行多时保留最小落点高度。
// ===end====
}

QRect scrolledDeviceGroupReservedBlankRect(int scrollOffset)
{
// =====wjy====
    QRect blankRect = deviceGroupReservedBlankRect(); // wjy: 先取得未滚动时的根部空白区。
    blankRect.translate(0, -scrollOffset); // wjy: 列表滚动后，空白区也要跟着内容一起上移或下移。
    return blankRect;
// ===end====
}

int groupNameVisualWidth(const QFont& font)
{
    const QFontMetrics metrics(font);
    return metrics.horizontalAdvance(QString(kGroupNameDisplayCharacters, QChar(0x4E00)));
}

int deviceNameEditVisualWidth(const QFont& font)
{
    const QFontMetrics metrics(font);
    return metrics.horizontalAdvance(QString(kDeviceNameEditCharacters, QChar(0x4E00)));
}

QFont sidebarListTextFont()
{
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(14);
    return font;
}

QRect groupNameTextRect(int rowY, const QFont& font)
{
    return QRect(kGroupNameTextX, rowY + 7, groupNameVisualWidth(font), 22);
}

QRect groupNameEditRect(int rowY)
{
    const QFont font = sidebarListTextFont();
    return QRect(kGroupNameTextX, rowY + 5, groupNameVisualWidth(font), 26);
}

QRect deviceNameEditRect(int rowY, bool insideGroup)
{
    const int textX = insideGroup ? 76 : 56;
    const QFont font = sidebarListTextFont();
    return QRect(textX, rowY + 5, deviceNameEditVisualWidth(font), 26);
}

QColor deviceStatusDotColor(platform::DevicePresenceState state)
{
    switch (state) {
    case platform::DevicePresenceState::Online:
        return QColor(QStringLiteral("#28D13B"));
    case platform::DevicePresenceState::Busy:
        return QColor(QStringLiteral("#EF4444"));
    case platform::DevicePresenceState::Offline:
        return QColor(QStringLiteral("#B0B3B8"));
    case platform::DevicePresenceState::Unknown:
    default:
        return QColor(QStringLiteral("#C7CDD6"));
    }
}

QColor deviceListStatusAccentColor(platform::DevicePresenceState state)
{
    if (state == platform::DevicePresenceState::Busy) {
        return deviceStatusDotColor(platform::DevicePresenceState::Online);
    }

    return deviceStatusDotColor(state);
}

QSize deviceHeaderStatusBadgeSize(platform::DevicePresenceState state, bool poweringOn)
{
    Q_UNUSED(state)
    Q_UNUSED(poweringOn)
    return QSize(72, 28);
}

QString deviceStatusText(platform::DevicePresenceState state)
{
    switch (state) {
    case platform::DevicePresenceState::Busy:
        return zh("\xE8\xA2\xAB\xE5\x8D\xA0\xE7\x94\xA8");
    case platform::DevicePresenceState::Offline:
        return zh("\xE7\xA6\xBB\xE7\xBA\xBF");
    case platform::DevicePresenceState::Unknown:
        return zh("\xE6\x9C\xAA\xE6\xA3\x80\xE6\xB5\x8B");
    case platform::DevicePresenceState::Online:
    default:
        return zh("\xE5\x9C\xA8\xE7\xBA\xBF");
    }
}

QString deviceHeaderStatusText(platform::DevicePresenceState state, bool poweringOn)
{
    if (state == platform::DevicePresenceState::Offline && poweringOn) {
        return zh("\xE5\xBC\x80\xE6\x9C\xBA\xE4\xB8\xAD");
    }
    return deviceStatusText(state);
}

QColor deviceHeaderStatusBackground(platform::DevicePresenceState state, bool poweringOn)
{
    if (poweringOn) {
        return QColor(QStringLiteral("#3A7BFC"));
    }
    if (state == platform::DevicePresenceState::Busy) {
        return QColor(QStringLiteral("#EF4444"));
    }
    if (state == platform::DevicePresenceState::Offline || state == platform::DevicePresenceState::Unknown) {
        return QColor(QStringLiteral("#606266"));
    }
    return QColor(QStringLiteral("#111111"));
}

QColor deviceHeaderStatusDotColor(platform::DevicePresenceState state, bool poweringOn)
{
    if (poweringOn || state == platform::DevicePresenceState::Busy) {
        return QColor(QStringLiteral("#FFFFFF"));
    }
    if (state == platform::DevicePresenceState::Offline || state == platform::DevicePresenceState::Unknown) {
        return QColor(QStringLiteral("#B0B3B8"));
    }
    return QColor(QStringLiteral("#19E58B"));
}

// =====wjy====
constexpr qreal kDeviceDetailCardLeft = 280.0; // wjy: 设备详情卡片左侧固定位置，蓝色桌面图和底部按钮都以它为基准对齐。
constexpr qreal kDeviceDetailCardWidth = 600.0; // wjy: 设备详情卡片宽度保持不变，只压缩上方蓝色桌面预览高度。
constexpr qreal kDeviceDesktopImageHeight = 120.0; // wjy: 原来蓝色桌面预览高度是 240，这里缩到 120，让上方蓝色区域明显变小。
constexpr qreal kDeviceBottomActionHeight = 50.0; // wjy: 底部“文件传输/更多”操作区高度保持 50，避免按钮文字和图标被压缩。
constexpr qreal kDeviceDetailCardHeight = kDeviceDesktopImageHeight + kDeviceBottomActionHeight + 1.0; // wjy: 卡片总高度跟随蓝色图高度计算，防止白色底部区域位置错位。

qreal deviceDetailCardTop(bool isRemoteControlled)
{
    return isRemoteControlled ? 189.0 : 124.0; // wjy: 被控提示出现时，卡片整体下移，原有布局逻辑保持不变。
}

qreal deviceBottomActionTop(bool isRemoteControlled)
{
    return deviceDetailCardTop(isRemoteControlled) + kDeviceDesktopImageHeight; // wjy: 底部操作区紧贴缩小后的蓝色桌面图下边缘。
}

QRectF desktopImageRect(bool isRemoteControlled)
{
    return QRectF(kDeviceDetailCardLeft, deviceDetailCardTop(isRemoteControlled), kDeviceDetailCardWidth, kDeviceDesktopImageHeight); // wjy: 蓝色桌面图的点击热区和绘制高度同步缩小。
}

QRectF fileTransferActionRect(bool isRemoteControlled)
{
    return QRectF(281, deviceBottomActionTop(isRemoteControlled), 298, kDeviceBottomActionHeight); // wjy: 文件传输按钮跟随新的底部操作区位置。
}

QRectF moreActionRect(bool isRemoteControlled)
{
    return QRectF(580, deviceBottomActionTop(isRemoteControlled), 299, kDeviceBottomActionHeight); // wjy: 更多按钮跟随新的底部操作区位置。
}
// ===end====

QRectF wakeButtonRect(bool isRemoteControlled)
{
    const QRectF imageRect = desktopImageRect(isRemoteControlled);
    return QRectF(
        imageRect.center().x() - 36.0,
        imageRect.center().y() - 36.0,
        72.0,
        72.0);
}

QRectF scriptTerminalPanelRect()
{
    return QRectF(contentLeft(), kDetailScriptPanelTop, contentWidth(), qMax(220, deviceDetailBottomBoundaryTop() - kDetailScriptPanelTop - kDetailScriptPanelBottomGap)); // wjy: 脚本日志面板只画到底部安全下沿上方，避免和折叠按钮区域重合。
}

QRect topDragDropZoneRect()
{
    return QRect(0, kTitleBarHeight, 236, 24); // wjy: 拖动设备/分组时，只在左侧设备栏内铺一层较薄的渐变提示，不再画出独立方框。
}

QRect scriptFileEditorRect()
{
    return QRect(contentLeft(), kDetailScriptPanelTop, contentWidth(), qMax(220, deviceDetailBottomBoundaryTop() - kDetailScriptPanelTop - kDetailScriptPanelBottomGap)); // wjy: 配置文件编辑面板同样停在底部安全下沿上方，切换标签页不会覆盖折叠按钮。
}

QRect deviceSearchPanelRect()
{
    return scriptFileEditorRect(); // wjy: 查找页与配置页复用完全相同的详情内容边界，窗口缩放和侧栏折叠时同步变化。
}

QRect detailScriptLogTabRect()
{
    // =====wjy====
    // wjy: 脚本标签排在本机右侧，与配置页保持同宽同间距。
    return QRect(contentLeft() + 72, kDetailScriptTabTop, 56, 36);
    // ===end====
}

QRect detailLocalTabRect()
{
    return QRect(contentLeft(), kDetailScriptTabTop, 56, 36); // wjy: 本机标签位于设备详情页最左侧，先于脚本和配置显示。
}

QRect detailConfigTabRect()
{
    // =====wjy====
    // wjy: 配置标签排在脚本右侧。
    return QRect(contentLeft() + 144, kDetailScriptTabTop, 56, 36);
    // ===end====
}

QRect detailSearchTabRect()
{
    return QRect(contentLeft() + 216, kDetailScriptTabTop, 56, 36); // wjy: 查找页签暂时隐藏，但保留在配置右侧的最终位置供后续恢复。
}

void drawDeviceDetailTabs(
    QPainter& painter,
    bool localSelected,
    bool scriptSelected,
    bool configSelected,
    bool searchSelected,
    const QFont& textFont)
{
    const QRect tabBar(contentLeft(), kDetailScriptTabTop, contentWidth(), 38); // wjy: 标签栏分割线与标签文字使用同一顶部位置，整体靠上显示。
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.drawLine(QPointF(tabBar.left(), tabBar.bottom()), QPointF(tabBar.right(), tabBar.bottom()));

    QFont tabFont(textFont);
    tabFont.setPixelSize(14);
    tabFont.setBold(true);
    // =====wjy====
    // wjy: 顺序为“本机 / 脚本 / 配置 / 查找”，四个页签共用同一蓝线选中规则。
    const auto paintTab = [&](const QRect& rect, const QString& text, bool selected) {
        painter.setFont(tabFont);
        painter.setPen(selected ? QColor(QStringLiteral("#040B18")) : QColor(QStringLiteral("#687384")));
        painter.drawText(QRectF(rect), Qt::AlignVCenter | Qt::AlignLeft, text);
        if (selected) {
            painter.setPen(QPen(QColor(QStringLiteral("#3A7BFC")), 3, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(rect.x() + 4, rect.bottom() + 2), QPointF(rect.x() + 32, rect.bottom() + 2));
        }
    };
    paintTab(detailLocalTabRect(), QString::fromUtf8("本机"), localSelected);
    paintTab(detailScriptLogTabRect(), QString::fromUtf8("脚本"), scriptSelected);
    paintTab(detailConfigTabRect(), QString::fromUtf8("配置"), configSelected);
#if 0
    // wjy: 查找页签暂不显示；保留绘制语句供后续直接恢复。
    paintTab(detailSearchTabRect(), QString::fromUtf8("查找"), searchSelected);
#else
    Q_UNUSED(searchSelected)
#endif
    // ===end====
    painter.restore();
}

// =====wjy====
QString localSystemInfoDisplayValue(const QString& value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("--") : trimmed; // wjy: 系统 API 返回空值时统一显示占位，不让某个字段影响其它信息绘制。
}

QString localSystemMemoryText(quint64 bytes)
{
    if (bytes == 0) {
        return QStringLiteral("--"); // wjy: 内存查询失败时保留明确占位，避免把未知值误显示成 0 GB。
    }
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
}

int localSystemDiskUsagePercent(const platform::LocalDiskInfo& disk)
{
    if (disk.totalBytes == 0) {
        return -1; // wjy: 单个盘符容量查询失败时保留未知状态，不把空容量误画成 0%。
    }
    const quint64 usedBytes = qMin(disk.usedBytes, disk.totalBytes); // wjy: 显示层再次限制已用容量，防止刷新期间容量变化产生超过总量的文本。
    return qBound(0, qRound(usedBytes * 100.0 / disk.totalBytes), 100); // wjy: 每个盘符独立计算 0-100 百分比，容量条和告警颜色共用同一结果。
}

QString localSystemDiskUsageText(const platform::LocalDiskInfo& disk)
{
    if (disk.totalBytes == 0) {
        return QStringLiteral("--");
    }
    const quint64 usedBytes = qMin(disk.usedBytes, disk.totalBytes);
    return QString::fromUtf8("已用 %1 / %2")
        .arg(localSystemMemoryText(usedBytes))
        .arg(localSystemMemoryText(disk.totalBytes)); // wjy: 顶部文字只保留已用和总量，百分比由右侧容量条直观表达。
}

QString localSystemDiskAvailableText(const platform::LocalDiskInfo& disk)
{
    if (disk.totalBytes == 0) {
        return QStringLiteral("--");
    }
    const quint64 usedBytes = qMin(disk.usedBytes, disk.totalBytes);
    return QString::fromUtf8("可用 %1")
        .arg(localSystemMemoryText(disk.totalBytes - usedBytes)); // wjy: 可用容量直接由同一盘符总量减已用量得到，和容量条保持一致。
}

QColor localUsageColor(qreal usagePercent)
{
    if (usagePercent < 0) {
        return QColor(QStringLiteral("#AAB3C0")); // wjy: 尚未取得有效资源样本时使用中性灰色，区分采样状态和真实负载。
    }
    if (usagePercent >= 85) {
        return QColor(QStringLiteral("#E35D6A")); // wjy: 高于 85% 时使用红色提醒控制端资源压力。
    }
    if (usagePercent >= 60) {
        return QColor(QStringLiteral("#E5A93D")); // wjy: 60%-84% 代表持续偏高，使用橙色提示但不制造错误告警。
    }
    return QColor(QStringLiteral("#3A7BFC")); // wjy: 正常负载使用主界面蓝色，和页签选中线保持一致。
}

void drawLocalSystemInfoPage(
    QPainter& painter,
    const platform::LocalSystemInfo& info,
    const platform::LocalMemoryUsage& memoryUsage,
    qreal cpuUsagePercent,
    qreal gpuUsagePercent,
    qreal memoryUsagePercent,
    const QFont& textFont)
{
    const QRect panel = scriptFileEditorRect(); // wjy: 本机页复用脚本/配置内容区域，缩放和底部安全边界保持完全一致。
    const QRect resourceCard = panel; // wjy: CPU、GPU、内存和磁盘共享完整内容区，只绘制一个外层卡片。
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(
        QRectF(resourceCard).adjusted(0.5, 0.5, -0.5, -0.5),
        5,
        5); // wjy: 统一白底和细边框取代原来的资源卡加系统信息卡，不再产生上下两个视觉块。

    const QRect content = resourceCard.adjusted(18, 12, -18, -12);
    constexpr int cpuRowHeight = 78;
    constexpr int gpuRowHeight = 78;
    constexpr int memoryRowHeight = 112; // wjy: 内存行额外容纳已用容量、内存条组合、插槽和类型速率四层信息。
    constexpr int resourceGap = 6;
    const QRect cpuRow(content.x(), content.y(), content.width(), cpuRowHeight);
    const QRect gpuRow(content.x(), cpuRow.bottom() + 1 + resourceGap, content.width(), gpuRowHeight);
    const QRect memoryRow(content.x(), gpuRow.bottom() + 1 + resourceGap, content.width(), memoryRowHeight);

    const auto drawUsageRow = [&](const QRect& rowRect,
                                  const QString& label,
                                  const QString& primaryText,
                                  const QStringList& secondaryTexts,
                                  qreal usagePercent) {
        constexpr int labelWidth = 54;
        const int ringSize = qBound(48, rowRect.height() - 18, 62);
        const QRect ringRect(
            rowRect.x() + labelWidth,
            rowRect.y() + (rowRect.height() - ringSize) / 2,
            ringSize,
            ringSize); // wjy: 标签在最左、圆环在中间、硬件摘要在右侧，严格对应用户确认的纵向草图结构。
        const QRect textRect(
            ringRect.right() + 18,
            rowRect.y() + 7,
            qMax(30, rowRect.right() - ringRect.right() - 18),
            rowRect.height() - 14);

        QFont labelFont(textFont);
        labelFont.setPixelSize(13);
        labelFont.setBold(true);
        painter.setFont(labelFont);
        painter.setPen(QColor(QStringLiteral("#3A7BFC")));
        painter.drawText(
            QRect(rowRect.x(), rowRect.y(), labelWidth - 8, rowRect.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            label); // wjy: 三类资源标签使用统一主色，左侧不再堆叠额外本机身份信息。

        QFont primaryFont(textFont);
        primaryFont.setPixelSize(13);
        primaryFont.setBold(true);
        painter.setFont(primaryFont);
        painter.setPen(QColor(QStringLiteral("#111827")));
        painter.drawText(
            QRect(textRect.x(), textRect.y(), textRect.width(), 20),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(primaryFont).elidedText(
                localSystemInfoDisplayValue(primaryText),
                Qt::ElideRight,
                textRect.width())); // wjy: 型号或已用容量作为每行第一视觉层级，过长时只在自身区域省略。

        QFont secondaryFont(textFont);
        secondaryFont.setPixelSize(11);
        painter.setFont(secondaryFont);
        painter.setPen(QColor(QStringLiteral("#687384")));
        constexpr int secondaryLineHeight = 19;
        for (int index = 0; index < secondaryTexts.size(); ++index) {
            const QRect lineRect(
                textRect.x(),
                textRect.y() + 22 + index * secondaryLineHeight,
                textRect.width(),
                secondaryLineHeight);
            if (lineRect.bottom() > textRect.bottom()) {
                break;
            }
            painter.drawText(
                lineRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(secondaryFont).elidedText(
                    localSystemInfoDisplayValue(secondaryTexts.at(index)),
                    Qt::ElideRight,
                    lineRect.width())); // wjy: CPU/GPU/内存细节按独立行裁切，关键数字不会跨入圆环或卡片边界。
        }

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF ringStrokeRect = QRectF(ringRect).adjusted(3.0, 3.0, -3.0, -3.0);
        painter.setPen(QPen(QColor(QStringLiteral("#EEF2F7")), 5.0, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(ringStrokeRect); // wjy: 三枚圆环先绘制相同浅色底轨，未占用部分保持统一。
        if (usagePercent >= 0) {
            painter.setPen(QPen(localUsageColor(usagePercent), 5.0, Qt::SolidLine, Qt::RoundCap));
            const int spanAngle = -qRound(
                360.0 * 16.0 * qBound<qreal>(0.0, usagePercent, 100.0) / 100.0); // wjy: 每个资源动画值独立换算为从 12 点顺时针扫过的圆弧角度。
            if (spanAngle != 0) {
                painter.drawArc(ringStrokeRect, 90 * 16, spanAngle);
            }
        }
        painter.restore();

        QFont percentFont(textFont);
        percentFont.setPixelSize(usagePercent < 0 ? 9 : 13);
        percentFont.setBold(true);
        painter.setFont(percentFont);
        painter.setPen(localUsageColor(usagePercent));
        const QString percentText = usagePercent < 0
            ? QString::fromUtf8("采样")
            : QStringLiteral("%1%").arg(qRound(usagePercent)); // wjy: 中心数字和圆弧使用同一小数显示值，三类资源都具有同步增减效果。
        painter.drawText(ringRect, Qt::AlignCenter, percentText);
    };

    const QString cpuSecondary = info.logicalProcessorCount > 0
        ? QString::fromUtf8("%1 逻辑处理器 · %2")
              .arg(info.logicalProcessorCount)
              .arg(localSystemInfoDisplayValue(info.cpuArchitecture))
        : localSystemInfoDisplayValue(info.cpuArchitecture);
    drawUsageRow(
        cpuRow,
        QStringLiteral("CPU"),
        info.cpuModel,
        {cpuSecondary},
        cpuUsagePercent);

    const QString gpuMemoryText = info.gpu.dedicatedVideoMemoryBytes > 0
        ? QString::fromUtf8("专用显存 %1").arg(localSystemMemoryText(info.gpu.dedicatedVideoMemoryBytes))
        : QString::fromUtf8("专用显存 --");
    drawUsageRow(
        gpuRow,
        QStringLiteral("GPU"),
        info.gpu.name,
        {gpuMemoryText},
        gpuUsagePercent);

    const quint64 sampledTotalMemory = memoryUsage.totalBytes > 0
        ? memoryUsage.totalBytes
        : info.totalPhysicalMemoryBytes; // wjy: 首次动态采样失败时回退到静态总容量，内存行仍有可用信息。
    const QString memoryPrimary = memoryUsage.totalBytes > 0
        ? QString::fromUtf8("已用 %1 / %2")
              .arg(localSystemMemoryText(memoryUsage.usedBytes))
              .arg(localSystemMemoryText(memoryUsage.totalBytes))
        : QString::fromUtf8("总容量 %1").arg(localSystemMemoryText(sampledTotalMemory));
    const int installedModuleCount = info.memoryModules.size();
    const QString slotText = installedModuleCount > 0
        ? (info.totalMemorySlotCount > 0
                ? QString::fromUtf8("已使用 %1 / %2 个插槽")
                      .arg(installedModuleCount)
                      .arg(info.totalMemorySlotCount)
                : QString::fromUtf8("已安装 %1 条内存").arg(installedModuleCount))
        : QString::fromUtf8("内存条信息 --");
    const QString specificationText = QStringLiteral("%1 · %2")
        .arg(platform::LocalSystemInfoService::memoryTypeSummary(info.memoryModules))
        .arg(platform::LocalSystemInfoService::memorySpeedSummary(info.memoryModules));
    QStringList memoryDetails{
        platform::LocalSystemInfoService::memoryCapacityComposition(info.memoryModules),
        slotText,
        specificationText,
    };
    if (memoryUsage.totalBytes > 0) {
        const QString availableMemoryText = memoryUsage.availableBytes > 0
            ? localSystemMemoryText(memoryUsage.availableBytes)
            : QStringLiteral("0.0 GB"); // wjy: 已成功采样且可用容量为零时显示真实 0，而不是把它误判成未知占位。
        memoryDetails.prepend(
            QString::fromUtf8("可用 %1").arg(availableMemoryText)); // wjy: 可用容量来自同一次 GlobalMemoryStatusEx 快照，不通过整数百分比反推。
    }
    drawUsageRow(
        memoryRow,
        QString::fromUtf8("内存"),
        memoryPrimary,
        memoryDetails,
        memoryUsagePercent);

    painter.save();
    painter.setPen(QPen(QColor(QStringLiteral("#EEF2F7")), 1));
    painter.drawLine(content.left(), cpuRow.bottom() + resourceGap / 2, content.right(), cpuRow.bottom() + resourceGap / 2);
    painter.drawLine(content.left(), gpuRow.bottom() + resourceGap / 2, content.right(), gpuRow.bottom() + resourceGap / 2);
    painter.drawLine(content.left(), memoryRow.bottom() + resourceGap, content.right(), memoryRow.bottom() + resourceGap); // wjy: 浅分隔线只组织同一卡片内部内容，不再形成独立子卡片。
    painter.restore();

    const int diskItemCount = qMax(1, info.disks.size());
    const int diskHeaderY = memoryRow.bottom() + resourceGap + 1;
    const QRect diskHeaderRect(content.x(), diskHeaderY, content.width(), 30);
    QFont diskHeaderFont(textFont);
    diskHeaderFont.setPixelSize(13);
    diskHeaderFont.setBold(true);
    painter.setFont(diskHeaderFont);
    painter.setPen(QColor(QStringLiteral("#111827")));
    painter.drawText(diskHeaderRect, Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8("磁盘"));

    const int diskRowsTop = diskHeaderRect.bottom() + 1;
    const int availableDiskHeight = qMax(42, content.bottom() - diskRowsTop + 1);
    const int diskRowHeight = qBound(
        42,
        availableDiskHeight / diskItemCount,
        58); // wjy: 常见盘符使用接近资源管理器的两行高度，盘符增多时在同一卡片内自动压缩。
    const QRect diskArea(
        content.x(),
        diskRowsTop,
        content.width(),
        diskRowHeight * diskItemCount);

    painter.save();
    painter.setPen(QPen(QColor(QStringLiteral("#EEF2F7")), 1));
    for (int row = 1; row < diskItemCount; ++row) {
        const int separatorY = diskArea.y() + row * diskRowHeight;
        painter.drawLine(diskArea.left(), separatorY, diskArea.right(), separatorY);
    }
    painter.restore();

    QFont diskLabelFont(textFont);
    diskLabelFont.setPixelSize(12);
    diskLabelFont.setBold(true);
    QFont diskValueFont(textFont);
    diskValueFont.setPixelSize(11);
    for (int index = 0; index < diskItemCount; ++index) {
        const QRect cellRect(
            diskArea.x(),
            diskArea.y() + index * diskRowHeight,
            diskArea.width(),
            diskRowHeight); // wjy: 盘符行始终使用磁盘区域完整宽度，容量文本不会与另一块磁盘横向并排或混淆。
        const bool hasDisk = index < info.disks.size();
        const platform::LocalDiskInfo disk = hasDisk ? info.disks.at(index) : platform::LocalDiskInfo{};
        const QString diskLabel = hasDisk
            ? localSystemInfoDisplayValue(disk.rootPath)
            : QString::fromUtf8("磁盘"); // wjy: 每个真实盘符独立作为标签；枚举为空时只显示一行“磁盘 --”占位。
        const QRect labelRect(cellRect.x() + 2, cellRect.y(), 48, cellRect.height());
        const QRect diskContentRect(
            labelRect.right() + 8,
            cellRect.y() + 4,
            qMax(20, cellRect.right() - labelRect.right() - 10),
            cellRect.height() - 8);
        const QRect usageTextRect(
            diskContentRect.x(),
            diskContentRect.y(),
            diskContentRect.width(),
            18);
        const int availableTextWidth = qMin(112, qMax(72, diskContentRect.width() / 3));
        const QRect availableTextRect(
            diskContentRect.right() - availableTextWidth + 1,
            diskContentRect.bottom() - 18,
            availableTextWidth,
            18);
        const QRect barRect(
            diskContentRect.x(),
            diskContentRect.bottom() - 12,
            qMax(18, availableTextRect.x() - diskContentRect.x() - 12),
            8);

        painter.setFont(diskLabelFont);
        painter.setPen(QColor(QStringLiteral("#687384")));
        painter.drawText(
            labelRect,
            Qt::AlignVCenter | Qt::AlignLeft,
            QFontMetrics(diskLabelFont).elidedText(diskLabel, Qt::ElideRight, labelRect.width()));
        painter.setFont(diskValueFont);
        painter.setPen(QColor(QStringLiteral("#111827")));
        painter.drawText(
            usageTextRect,
            Qt::AlignVCenter | Qt::AlignLeft,
            QFontMetrics(diskValueFont).elidedText(
                hasDisk ? localSystemDiskUsageText(disk) : QStringLiteral("--"),
                Qt::ElideRight,
                usageTextRect.width()));
        painter.setPen(QColor(QStringLiteral("#687384")));
        painter.drawText(
            availableTextRect,
            Qt::AlignVCenter | Qt::AlignRight,
            QFontMetrics(diskValueFont).elidedText(
                hasDisk ? localSystemDiskAvailableText(disk) : QStringLiteral("--"),
                Qt::ElideLeft,
                availableTextRect.width())); // wjy: 可用容量固定贴右，容量条在左侧保留足够长度模拟文件资源管理器。

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#EEF2F7")));
        painter.drawRoundedRect(QRectF(barRect), 4, 4);
        const int usagePercent = hasDisk ? localSystemDiskUsagePercent(disk) : -1;
        if (usagePercent >= 0) {
            const int filledWidth = qBound(
                0,
                qRound(barRect.width() * usagePercent / 100.0),
                barRect.width());
            if (filledWidth > 0) {
                painter.setBrush(localUsageColor(usagePercent));
                painter.drawRoundedRect(
                    QRectF(barRect.x(), barRect.y(), filledWidth, barRect.height()),
                    4,
                    4); // wjy: 已用比例填充容量条，60% 和 85% 阈值沿用资源圆环的橙色与红色提醒。
            }
        }
        painter.restore();
    }
}
// ===end====

QRect scriptFileEditorSaveButtonRect()
{
    const QRect rect = scriptFileEditorRect();
    return QRect(rect.right() - 68, rect.y() + 7, 56, 24);
}

QRect scriptFileEditorTextRect()
{
    return scriptFileEditorRect().adjusted(8, 38, -8, -8);
}

QRect scriptFolderTreeRect()
{
    const QRectF panel = scriptTerminalPanelRect();
    const int treeWidth = qBound(180, int(panel.width() * 0.28), 280);
    return QRect(
        qRound(panel.x() + 12),
        qRound(panel.y() + 46),
        treeWidth,
        qMax(80, qRound(panel.height() - 58))); // wjy: 脚本树固定占据脚本日志面板左侧，底部同样停在安全边界上方。
}

QRectF scriptTerminalOutputRect()
{
    const QRectF panel = scriptTerminalPanelRect();
    const QRect tree = scriptFolderTreeRect();
    const qreal left = tree.right() + 12.0;
    return QRectF(
        left,
        panel.y() + 46.0,
        qMax<qreal>(80.0, panel.right() - left - 18.0),
        qMax<qreal>(80.0, panel.height() - 58.0)); // wjy: 终端输出区从脚本树右侧开始，避免文字画到树控件下面。
}

QRectF scriptTerminalExecuteButtonRect()
{
    const QRectF panel = scriptTerminalPanelRect();
    return QRectF(panel.right() - 150, panel.y() + 8, 58, 22); // wjy: 执行和停止拆成两个明确按钮，避免一个按钮在运行态切换含义。
}

QRectF scriptTerminalStopButtonRect()
{
    const QRectF panel = scriptTerminalPanelRect();
    return QRectF(panel.right() - 86, panel.y() + 8, 58, 22);
}

QString stripTerminalControlSequences(const QString& text);

QString base64Utf8(const QString& text)
{
    return QString::fromLatin1(text.toUtf8().toBase64());
}

QString utf8FromBase64(const QString& text)
{
    return QString::fromUtf8(QByteArray::fromBase64(text.trimmed().toLatin1()));
}

QStringList terminalOutputLines(const QString& text)
{
    const QString normalized = text.trimmed().isEmpty()
        ? QString()
        : text.trimmed();
    QStringList compactedLines;
    bool previousLineEmpty = false;
    for (QString line : normalized.split(QLatin1Char('\n'))) {
        while (line.endsWith(QLatin1Char(' ')) || line.endsWith(QLatin1Char('\t'))) {
            line.chop(1);
        }
        const bool lineEmpty = line.trimmed().isEmpty();
        if (lineEmpty && previousLineEmpty) {
            continue;
        }
        compactedLines.append(line);
        previousLineEmpty = lineEmpty;
    }
    return compactedLines; // wjy: Collapse repeated blank lines from terminal repaint/control output while preserving JSON indentation.
}

int terminalVisibleLineCount(const QFontMetrics& metrics, int contentHeight)
{
    return qMax(1, contentHeight / qMax(1, metrics.lineSpacing()));
}

int maxTerminalScrollOffset(const QString& text, const QFontMetrics& metrics, int contentHeight)
{
    const QStringList lines = terminalOutputLines(stripTerminalControlSequences(text));
    return qMax(0, lines.size() - terminalVisibleLineCount(metrics, contentHeight));
}

QString terminalVisibleText(const QString& text, const QFontMetrics& metrics, int contentHeight, int scrollOffset)
{
    QStringList lines = terminalOutputLines(stripTerminalControlSequences(text));
    const int visibleLines = terminalVisibleLineCount(metrics, contentHeight);
    const int maxOffset = qMax(0, lines.size() - visibleLines);
    const int boundedOffset = qBound(0, scrollOffset, maxOffset);
    const int start = qMax(0, lines.size() - visibleLines - boundedOffset);
    if (lines.size() > visibleLines) {
        lines = lines.mid(start, visibleLines);
    }
    return lines.join(QLatin1Char('\n')); // wjy: Draw the selected terminal line window; offset 0 follows the newest output.
}

QString stripTerminalControlSequences(const QString& text)
{
    QString cleaned;
    cleaned.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch.unicode() == 0x1B) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('[')) {
                i += 2;
                while (i < text.size()) {
                    const ushort code = text.at(i).unicode();
                    if (code >= 0x40 && code <= 0x7E) {
                        break;
                    }
                    ++i;
                }
            }
            continue; // wjy: Drop ANSI escape sequences such as ESC[K and ESC[19;1H before painting the fake terminal.
        }
        if (ch == QLatin1Char('\r')) {
            if (i + 1 >= text.size() || text.at(i + 1) != QLatin1Char('\n')) {
                cleaned.append(QLatin1Char('\n'));
            }
            continue;
        }
        if (ch.unicode() < 0x20 && ch != QLatin1Char('\n') && ch != QLatin1Char('\t')) {
            continue;
        }
        cleaned.append(ch);
    }
    return cleaned;
}

void drawTopDragDropZone(QPainter& painter, const QString& label)
{
    const QRect zone = topDragDropZoneRect();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient gradient(zone.topLeft(), zone.bottomLeft());
    gradient.setColorAt(0.0, QColor(248, 113, 113, 122));
    gradient.setColorAt(1.0, QColor(248, 113, 113, 34));
    painter.setPen(Qt::NoPen);
    painter.fillRect(zone, gradient);

    QFont hintFont(QStringLiteral("Microsoft YaHei UI"));
    hintFont.setPixelSize(12);
    hintFont.setBold(true);
    painter.setFont(hintFont);
    painter.setPen(QColor(127, 29, 29, 220));
    painter.drawText(QRect(zone.x(), zone.y() + 2, zone.width(), 18), Qt::AlignHCenter | Qt::AlignTop, label);
    painter.restore();
}

void drawScriptTerminalPanel(
    QPainter& painter,
    const QString& title,
    const QString& text,
    bool running,
    bool failed,
    int scrollOffset,
    bool canExecute)
{
    const QRectF panel = scriptTerminalPanelRect();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#263241")), 1));
    painter.setBrush(QColor(QStringLiteral("#0B1018")));
    painter.drawRoundedRect(panel, 4, 4);

    const QRectF header(panel.x(), panel.y(), panel.width(), 34);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#111827")));
    painter.drawRoundedRect(header, 4, 4);
    painter.fillRect(header.adjusted(0, 18, 0, 0), QColor(QStringLiteral("#111827")));

    painter.setBrush(failed
        ? QColor(QStringLiteral("#FF5C5C"))
        : (running ? QColor(QStringLiteral("#F5C542")) : QColor(QStringLiteral("#22C55E"))));
    painter.drawEllipse(QRectF(panel.x() + 12, panel.y() + 13, 8, 8));

    QFont titleFont(QStringLiteral("Microsoft YaHei UI"));
    titleFont.setPixelSize(12);
    painter.setFont(titleFont);
    painter.setPen(QColor(QStringLiteral("#DDE7F3")));
    painter.drawText(
        QRectF(panel.x() + 28, panel.y() + 7, panel.width() - 194, 20),
        Qt::AlignVCenter | Qt::AlignLeft,
        painter.fontMetrics().elidedText(title, Qt::ElideRight, int(panel.width() - 194)));

    const QRectF executeButton = scriptTerminalExecuteButtonRect();
    painter.setPen(QPen(canExecute ? QColor(QStringLiteral("#34D399")) : QColor(QStringLiteral("#4B5563")), 1));
    painter.setBrush(canExecute ? QColor(QStringLiteral("#10231D")) : QColor(QStringLiteral("#111827")));
    painter.drawRoundedRect(executeButton, 3, 3);
    painter.setFont(titleFont);
    painter.setPen(canExecute ? QColor(QStringLiteral("#A7F3D0")) : QColor(QStringLiteral("#6B7280")));
    painter.drawText(executeButton.adjusted(0, 0, 0, -1), Qt::AlignCenter, QString::fromUtf8("启动"));

    const QRectF stopButton = scriptTerminalStopButtonRect();
    painter.setPen(QPen(running ? QColor(QStringLiteral("#F87171")) : QColor(QStringLiteral("#4B5563")), 1));
    painter.setBrush(running ? QColor(QStringLiteral("#2A151A")) : QColor(QStringLiteral("#111827")));
    painter.drawRoundedRect(stopButton, 3, 3);
    painter.setPen(running ? QColor(QStringLiteral("#FCA5A5")) : QColor(QStringLiteral("#6B7280")));
    painter.drawText(stopButton.adjusted(0, 0, 0, -1), Qt::AlignCenter, QString::fromUtf8("停止"));

    const QRect treeFrame = scriptFolderTreeRect();
    painter.setPen(QPen(QColor(QStringLiteral("#263241")), 1));
    painter.setBrush(QColor(QStringLiteral("#0F172A")));
    painter.drawRoundedRect(QRectF(treeFrame), 3, 3);

    Q_UNUSED(text) // wjy: 日志正文交给只读QTextEdit显示，绘制函数只保留面板、标题和操作按钮。
    Q_UNUSED(scrollOffset)
    painter.restore();
}

void drawScriptFileEditorPanel(
    QPainter& painter,
    const QString& title,
    bool loading,
    bool hasFile)
{
// =====wjy====
    const QRect panel = scriptFileEditorRect(); // wjy: 编辑器面板占用用户标出的左侧空白区域，真实文本编辑控件覆盖在正文区域。
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#263241")), 1));
    painter.setBrush(QColor(QStringLiteral("#05070A")));
    painter.drawRoundedRect(QRectF(panel), 4, 4);

    const QRect header(panel.x(), panel.y(), panel.width(), 34);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#111827")));
    painter.drawRoundedRect(QRectF(header), 4, 4);
    painter.fillRect(header.adjusted(0, 18, 0, 0), QColor(QStringLiteral("#111827")));

    painter.setBrush(loading
        ? QColor(QStringLiteral("#F5C542"))
        : (hasFile ? QColor(QStringLiteral("#22C55E")) : QColor(QStringLiteral("#94A3B8"))));
    painter.drawEllipse(QRectF(panel.x() + 12, panel.y() + 13, 8, 8)); // wjy: 状态点沿用右侧终端的视觉语言，表示读取中/已找到/无文件。

    QFont titleFont(QStringLiteral("Microsoft YaHei UI"));
    titleFont.setPixelSize(12);
    painter.setFont(titleFont);
    painter.setPen(QColor(QStringLiteral("#DDE7F3")));
    const QString displayTitle = title.trimmed().isEmpty()
        ? QString::fromUtf8("本地文件")
        : title.trimmed(); // wjy: 标题显示当前 work 目录里正在编辑的 json/txt 文件名。
    painter.drawText(
        QRectF(panel.x() + 28, panel.y() + 7, panel.width() - 110, 20),
        Qt::AlignVCenter | Qt::AlignLeft,
        painter.fontMetrics().elidedText(displayTitle, Qt::ElideRight, panel.width() - 110));
    painter.restore();
// ===end====
}

qreal easeOutCubic(qreal value)
{
    const qreal inverse = 1.0 - value;
    return 1.0 - inverse * inverse * inverse;
}

void drawDeviceDetail(
    QPainter& painter,
    const QString& deviceName,
    platform::DevicePresenceState deviceState,
    bool poweringOn,
    int wakeRemainingSeconds,
    bool isRemoteControlled,
    qreal desktopHoverProgress,
    qreal wakeVisualRotation,
    BottomAction hoveredBottomAction,
    qreal yOffset,
    qreal opacity,
    bool leftSidebarCollapsed,
    const QFont& textFont)
{
    Q_UNUSED(wakeRemainingSeconds)
    Q_UNUSED(desktopHoverProgress)
    Q_UNUSED(wakeVisualRotation)
    Q_UNUSED(hoveredBottomAction)
    painter.save();
    painter.setOpacity(opacity);
    painter.translate(0, yOffset);

    Q_UNUSED(deviceName)
    Q_UNUSED(deviceState)
    Q_UNUSED(poweringOn)
    Q_UNUSED(leftSidebarCollapsed)

    qreal cardTop = 124;
    if (isRemoteControlled) {
        const QRectF notice(280, 117, 600, 56);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#DDF0FF")));
        painter.drawRoundedRect(notice, 3, 3);

        drawUiIcon(painter, QRect(312, 135, 20, 20), QStringLiteral("controlled_notice.svg"));

        QFont noticeTitle(QStringLiteral("Microsoft YaHei UI"));
        noticeTitle.setPixelSize(14);
        noticeTitle.setBold(true);
        painter.setFont(noticeTitle);
        painter.setPen(QColor(QStringLiteral("#006BFF")));
        painter.drawText(
            QRectF(342, 136, 66, 20),
            Qt::AlignVCenter | Qt::AlignLeft,
            zh("\xE8\xA2\xAB\xE6\x8E\xA7\xE4\xB8\xAD"));

        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#006BFF")));
        painter.drawText(
            QRectF(427, 136, 250, 20),
            Qt::AlignVCenter | Qt::AlignLeft,
            zh("\xE5\xBD\x93\xE5\x89\x8D\xE6\x9C\x89 1 \xE5\x8F\xB0\xE6\x9C\xAC\xE8\xB4\xA6\xE5\x8F\xB7\xE8\xAE\xBE\xE5\xA4\x87\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x8E\xA7\xE5\x88\xB6\xE6\xAD\xA4\xE7\x94\xB5\xE8\x84\x91"));

        painter.setPen(QPen(QColor(QStringLiteral("#BCC8D8")), 1));
        painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
        painter.drawRoundedRect(QRectF(760, 133, 95, 24), 2, 2);
        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#040B18")));
        painter.drawText(
            QRectF(760, 133, 95, 24),
            Qt::AlignCenter,
            zh("\xE6\x96\xAD\xE5\xBC\x80\xE6\x89\x80\xE6\x9C\x89\xE8\xBF\x9C\xE6\x8E\xA7"));

        cardTop = 189;
        painter.restore();
        return;
    }

    const QRectF card(kDeviceDetailCardLeft, cardTop, kDeviceDetailCardWidth, kDeviceDetailCardHeight); // wjy: 卡片高度由缩小后的蓝色桌面图高度自动计算。
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    Q_UNUSED(card)
    painter.restore();
    return;
}

void drawSettingsSwitch(QPainter& painter, int x, int y, bool checked)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(checked ? Qt::NoPen : QPen(QColor(QStringLiteral("#A9ADB3")), 1));
    painter.setBrush(checked ? QColor(QStringLiteral("#3A7BFC")) : QColor(QStringLiteral("#F4F5F7")));
    painter.drawRoundedRect(QRectF(x, y, 40, 20), 10, 10);
    painter.setPen(Qt::NoPen);
    painter.setBrush(checked ? QColor(QStringLiteral("#FFFFFF")) : QColor(QStringLiteral("#666666")));
    painter.drawEllipse(QRectF(checked ? x + 23 : x + 5, y + 4, 12, 12));
    painter.restore();
}

QRect settingsAutoRunSwitchRect()
{
    return QRect(contentLeft() + contentWidth() - 90, 140 + (kDetailScriptPanelTop - 120), 82, 32); // wjy: 常规页控件整体参考设备详情页主体顶部上移。
}

QRect settingsRemoteWakeupSwitchRect()
{
    return QRect(contentLeft() + contentWidth() - 90, 212 + (kDetailScriptPanelTop - 120), 82, 32); // wjy: 远程开机开关跟随设置页统一上移。
}

QRect settingsPreventSleepSwitchRect()
{
    return QRect(contentLeft() + contentWidth() - 90, 290 + (kDetailScriptPanelTop - 120), 82, 32); // wjy: 防休眠开关跟随设置页统一上移。
}

QRect settingsAutoRefreshSwitchRect()
{
    return QRect(contentLeft() + contentWidth() - 90, 368 + (kDetailScriptPanelTop - 120), 82, 32); // wjy: 自动刷新开关跟随设置页统一上移。
}

// =====wjy====
QRect settingsPeriodicDeviceDiscoverySwitchRect()
{
    return QRect(contentLeft() + contentWidth() - 90, 536 + (kDetailScriptPanelTop - 120), 82, 32); // wjy: 周期新增开关位于批量新增卡片下方，与列表自动刷新开关右对齐。
}
// ===end====

// =====wjy====
QRect settingsHideLocalDeviceSwitchRect()
{
    return QRect(contentLeft() + contentWidth() - 90, 607 + (kDetailScriptPanelTop - 120), 82, 32); // wjy: 隐藏本机开关独占周期发现下方的新卡片，并与其它常规开关右对齐。
}
// ===end====

// =====wjy====
QRect settingsPublishUpdateButtonRect()
{
    return QRect(contentLeft() + contentWidth() - 118, 828 + (kDetailScriptPanelTop - 120), 100, 28); // wjy: 隐藏本机卡片插入后发布按钮随发布卡片下移一行。
}
// ===end====

// =====wjy====
QRect settingsRollbackVersionComboRect()
{
    const int buttonX = contentLeft() + contentWidth() - 118;
    const int comboWidth = qBound(132, contentWidth() / 4, 180);
    return QRect(buttonX - comboWidth - 12, 759 + (kDetailScriptPanelTop - 120), comboWidth, 32); // wjy: 新卡片加入后回撤控件整体下移一行，窄窗口宽度策略保持不变。
}

QRect settingsRollbackButtonRect()
{
    return QRect(contentLeft() + contentWidth() - 118, 759 + (kDetailScriptPanelTop - 120), 100, 32); // wjy: 回撤按钮与下移后的回撤卡片保持同一行和右边界。
}
// ===end====

// =====wjy====
QRect settingsWallpaperRotationSwitchRect()
{
    return QRect(contentLeft() + contentWidth() - 90, 683 + (kDetailScriptPanelTop - 120), 82, 32); // wjy: 隐藏本机卡片占用原位置后，壁纸开关下移一行并保持右对齐。
}

QRect settingsWallpaperRotationIntervalInputRect()
{
    return QRect(contentLeft() + contentWidth() - 224, 683 + (kDetailScriptPanelTop - 120), 78, 32); // wjy: 壁纸分钟输入框跟随开关下移，二者仍共用同一行。
}
// ===end====

// =====wjy====
QRect settingsPeriodicDeviceDiscoveryIntervalInputRect()
{
    return QRect(contentLeft() + contentWidth() - 224, 532 + (kDetailScriptPanelTop - 120), 112, 32); // wjy: 秒数输入框与周期新增开关保持同一行，视觉完全参考列表自动刷新。
}
// ===end====

QRect settingsBatchSubnetInputRect()
{
    const int buttonX = contentLeft() + contentWidth() - 120;
    const int inputX = contentLeft() + qMin(300, qMax(180, contentWidth() / 2));
    return QRect(inputX, 448 + (kDetailScriptPanelTop - 120), qMax(120, buttonX - inputX - 14), 32); // wjy: 批量新增输入框跟随设置页统一上移。
}

QRect settingsBatchAddButtonRect()
{
    return QRect(contentLeft() + contentWidth() - 120, 448 + (kDetailScriptPanelTop - 120), 96, 32); // wjy: 批量新增按钮跟随设置页统一上移。
}

QRect settingsGeneralTabRect()
{
    return QRect(contentLeft(), kDetailScriptTabTop, 56, 36); // wjy: 设置页标签栏与设备详情页“配置文件/脚本日志”标签栏对齐。
}

QRect settingsKeyboardTabRect()
{
    return QRect(contentLeft() + 72, kDetailScriptTabTop, 56, 36); // wjy: 键盘标签跟随设置页统一顶部位置。
}

// =====wjy====
QRect settingsRemoteControlTabRect()
{
    return QRect(contentLeft() + 144, kDetailScriptTabTop, 84, 36); // wjy: 第三个“远控画质”标签放在键盘右侧，不挤压现有常规/键盘命中区。
}

QRect settingsRemoteQualityControlRect(int index)
{
    const QRect card = settingsScrollViewportRect();
    const int column = index % 2;
    const int row = index / 2;
    const int columnWidth = qMax(220, (card.width() - 84) / 2);
    const int x = card.x() + 28 + column * (columnWidth + 28) + columnWidth - 150;
    const int y = card.y() + 66 + row * 58;
    return QRect(x, y, 150, 32); // wjy: 保留原设置卡片的首个控件位置，现在只用于全局默认模式下拉框。
}
// ===end====

// =====wjy====
QRect settingsShortcutKeyEditRect(int index)
{
    const QRect keyboardCard = settingsScrollViewportRect(); // wjy: 快捷键页面板和设备详情页主体区域共用同一矩形。
    return QRect(keyboardCard.right() - 170, keyboardCard.y() + 65 + index * 48, 140, 30); // wjy: 每个快捷键输入框贴在键盘设置行右侧，宽度加大以容纳 Ctrl+Shift+X 这类组合。
}

int keyboardShortcutMaxScrollOffset()
{
    const QRect keyboardCard = settingsScrollViewportRect();
    const QRect lastShortcutRect = settingsShortcutKeyEditRect(kShortcutEditorCount - 1);
    return qMax(0, lastShortcutRect.bottom() - keyboardCard.bottom()); // wjy: 键盘页只按九行实际高度计算滚动上限，不把常规页更长内容的偏移带到快捷键页。
}
// ===end====

void drawSettingsTab(QPainter& painter, const QRect& rect, const QString& text, bool selected, const QFont& font)
{
    painter.save();
    painter.setFont(font);
    painter.setPen(selected ? QColor(QStringLiteral("#040B18")) : QColor(QStringLiteral("#687384")));
    painter.drawText(QRectF(rect), Qt::AlignVCenter | Qt::AlignLeft, text);
    if (selected) {
        painter.setPen(QPen(QColor(QStringLiteral("#3A7BFC")), 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(rect.x() + 4, rect.bottom() + 2), QPointF(rect.x() + 32, rect.bottom() + 2)); // wjy: 设置页标签下划线长度和位置对齐设备详情页标签样式。
    }
    painter.restore();
}

void drawSettingsOptionIcon(QPainter& painter, const QRect& rect, int iconKind)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#E9F1FF")));
    painter.drawRoundedRect(QRectF(rect), 6, 6);
    painter.setPen(QPen(QColor(QStringLiteral("#3A7BFC")), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF c = rect.center();
    if (iconKind == 0) {
        painter.drawEllipse(QRectF(c.x() - 6, c.y() - 6, 12, 12));
        painter.drawLine(QPointF(c.x(), c.y() - 10), QPointF(c.x(), c.y() - 2));
    } else if (iconKind == 1) {
        painter.drawArc(QRectF(c.x() - 8, c.y() - 8, 16, 16), 35 * 16, 285 * 16);
        painter.drawLine(QPointF(c.x() + 6, c.y() - 8), QPointF(c.x() + 10, c.y() - 8));
        painter.drawLine(QPointF(c.x() + 10, c.y() - 8), QPointF(c.x() + 10, c.y() - 4));
    } else if (iconKind == 2) {
        painter.drawRoundedRect(QRectF(c.x() - 8, c.y() - 4, 16, 12), 2, 2);
        painter.drawArc(QRectF(c.x() - 5, c.y() - 10, 10, 10), 0, 180 * 16);
    } else if (iconKind == 3) {
        painter.drawEllipse(QRectF(c.x() - 7, c.y() - 7, 14, 14));
        painter.drawLine(c, QPointF(c.x() + 6, c.y() - 3));
    } else if (iconKind == 4) {
        painter.drawRect(QRectF(c.x() - 8, c.y() - 6, 16, 12));
        painter.drawLine(QPointF(c.x() - 4, c.y() - 1), QPointF(c.x() + 4, c.y() - 1));
        painter.drawLine(QPointF(c.x(), c.y() - 5), QPointF(c.x(), c.y() + 5));
    } else if (iconKind == 5) {
        painter.drawRoundedRect(QRectF(c.x() - 8, c.y() - 7, 16, 14), 2, 2);
        painter.drawLine(QPointF(c.x() - 5, c.y() - 1), QPointF(c.x() + 5, c.y() - 1));
        painter.drawLine(QPointF(c.x() - 5, c.y() + 4), QPointF(c.x() + 5, c.y() + 4));
    } else if (iconKind == 7) {
        painter.drawRoundedRect(QRectF(c.x() - 6, c.y() - 1, 12, 9), 2, 2); // wjy: 锁体图标对应 F2 手动鼠标锁定。
        painter.drawArc(QRectF(c.x() - 5, c.y() - 9, 10, 12), 0, 180 * 16);
    } else if (iconKind == 8) {
        painter.drawEllipse(QRectF(c.x() - 7, c.y() - 7, 14, 14)); // wjy: 空心录制圆点对应 F9 键鼠录制。
        painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
        painter.drawEllipse(QRectF(c.x() - 3, c.y() - 3, 6, 6));
    } else if (iconKind == 9) {
        painter.drawPolygon(QPolygonF{QPointF(c.x() - 4, c.y() - 8), QPointF(c.x() + 7, c.y()), QPointF(c.x() - 4, c.y() + 8)}); // wjy: 播放三角对应 F10 脚本播放。
    } else if (iconKind == 10) {
        painter.drawRoundedRect(QRectF(c.x() - 6, c.y() - 5, 12, 12), 1, 1); // wjy: 简单垃圾桶图标对应删除设备。
        painter.drawLine(QPointF(c.x() - 8, c.y() - 7), QPointF(c.x() + 8, c.y() - 7));
        painter.drawLine(QPointF(c.x() - 3, c.y() - 9), QPointF(c.x() + 3, c.y() - 9));
    } else if (iconKind == 11) {
        painter.drawEllipse(QRectF(c.x() - 8, c.y() - 5, 16, 10)); // wjy: 眼睛轮廓表达设备列表可见性设置。
        painter.drawEllipse(QRectF(c.x() - 2, c.y() - 2, 4, 4));
        painter.drawLine(QPointF(c.x() - 9, c.y() + 8), QPointF(c.x() + 9, c.y() - 8)); // wjy: 斜线明确表示开启后隐藏本机设备。
    } else {
        painter.drawRoundedRect(QRectF(c.x() - 9, c.y() - 7, 18, 14), 2, 2); // wjy: 壁纸测试项使用横向图片框图标，与本机信息文档图标区分。
        painter.drawEllipse(QRectF(c.x() + 3, c.y() - 4, 3, 3)); // wjy: 右上圆点表示图片中的太阳。
        painter.drawPolyline(QPolygonF{QPointF(c.x() - 7, c.y() + 5), QPointF(c.x() - 2, c.y()), QPointF(c.x() + 2, c.y() + 4), QPointF(c.x() + 5, c.y() + 1), QPointF(c.x() + 8, c.y() + 5)}); // wjy: 山形折线强化“图片/壁纸”语义。
    }
    painter.restore();
}

void drawShortcutKey(QPainter& painter, const QRect& rect, const QString& text)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#CBD5E1")), 1));
    painter.setBrush(QColor(QStringLiteral("#F8FAFC")));
    painter.drawRoundedRect(QRectF(rect), 4, 4);
    QFont keyFont(QStringLiteral("Microsoft YaHei UI"));
    keyFont.setPixelSize(12);
    keyFont.setBold(true);
    painter.setFont(keyFont);
    painter.setPen(QColor(QStringLiteral("#334155")));
    painter.drawText(QRectF(rect), Qt::AlignCenter, text);
    painter.restore();
}

void drawSettingsPage(
    QPainter& painter,
    const QFont& textFont,
    bool autoRunEnabled,
    bool remoteWakeupEnabled,
    bool preventSleepEnabled,
    bool periodicDeviceDiscoveryEnabled,
    bool hideLocalDeviceEnabled,
    bool wallpaperRotationEnabled,
    const QString& wallpaperRotationStatusText,
    bool rollbackVersionAvailable,
    bool rollbackPreparing,
    bool publishPreparing,
    bool versionTransactionBusy,
    bool localInfoExpanded,
    bool addDeviceExpanded,
    bool keyboardSelected,
    bool remoteControlSelected,
    const platform::DeviceInfo& localInfo,
    int settingsScrollOffset)
{
    const SettingsLayoutSnapshot layout = settingsLayoutSnapshot(
        localInfoExpanded,
        addDeviceExpanded,
        settingsScrollOffset,
        keyboardSelected); // wjy: 键盘页使用九行快捷键自己的滚动上限，绘制、滚动条和真实输入框保持同一快照。
    QFont tabFont(textFont);
    tabFont.setPixelSize(14);
    tabFont.setBold(true);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    const QRect tabBar(contentLeft(), kDetailScriptTabTop, contentWidth(), 38);
    painter.drawLine(QPointF(tabBar.left(), tabBar.bottom()), QPointF(tabBar.right(), tabBar.bottom())); // wjy: 设置页顶部标签分割线和设备详情页保持一致。
    painter.restore();
    drawSettingsTab(painter, settingsGeneralTabRect(), QString::fromUtf8("常规"), !keyboardSelected && !remoteControlSelected, tabFont);
    drawSettingsTab(painter, settingsKeyboardTabRect(), QString::fromUtf8("键盘"), keyboardSelected, tabFont);
    drawSettingsTab(painter, settingsRemoteControlTabRect(), QString::fromUtf8("远控画质"), remoteControlSelected, tabFont);

    // =====wjy====
    if (remoteControlSelected) {
        const QRect qualityCard = layout.viewport();
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
        painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
        painter.drawRoundedRect(QRectF(qualityCard), 4, 4);

        QFont title(textFont);
        title.setPixelSize(14);
        title.setBold(true);
        painter.setFont(title);
        painter.setPen(QColor(QStringLiteral("#040B18")));
        painter.drawText(QRectF(qualityCard.x() + 28, qualityCard.y() + 16, qualityCard.width() - 56, 22),
            Qt::AlignVCenter | Qt::AlignLeft,
            QString::fromUtf8("智能远控画质"));

        QFont detail(textFont);
        detail.setPixelSize(11);
        painter.setFont(detail);
        painter.setPen(QColor(QStringLiteral("#687384")));
        painter.drawText(QRectF(qualityCard.x() + 28, qualityCard.y() + 38, qualityCard.width() - 56, 18),
            Qt::AlignVCenter | Qt::AlignLeft,
            QString::fromUtf8("按当前获得焦点的窗口选择高清，其它窗口使用低质保活档；音频由标题栏按钮独立控制。")); // wjy: 设置页与当前焦点策略保持一致。

        QFont labelFont(textFont);
        labelFont.setPixelSize(12);
        painter.setFont(labelFont);
        painter.setPen(QColor(QStringLiteral("#111827")));
        const QRect modeControlRect = settingsRemoteQualityControlRect(0);
        painter.drawText(
            QRectF(modeControlRect.x() - 142, modeControlRect.y(), 132, modeControlRect.height()),
            Qt::AlignVCenter | Qt::AlignRight,
            QString::fromUtf8("当前策略"));

        QFont presetFont(textFont);
        presetFont.setPixelSize(12);
        painter.setFont(presetFont);
        const QStringList presetLines = {
            QString::fromUtf8("当前焦点窗口    原始分辨率 · 60 FPS"),
            QString::fromUtf8("其它可见窗口    720p · 当前后台 FPS"),
            QString::fromUtf8("无远控窗口焦点  全部使用后台策略"),
            QString::fromUtf8("最小化/隐藏       540p · 当前最小化 FPS"),
            QString::fromUtf8("软件回退            540p · 24 FPS 安全档"),
            QString::fromUtf8("音频                    默认静音，按钮独立控制"),
        };
        for (int index = 0; index < presetLines.size(); ++index) {
            const QRectF lineRect(
                qualityCard.x() + 28,
                qualityCard.y() + 118 + index * 30,
                qualityCard.width() - 56,
                24);
            painter.setPen(index == presetLines.size() - 1
                    ? QColor(QStringLiteral("#687384"))
                    : QColor(QStringLiteral("#111827")));
            painter.drawText(lineRect, Qt::AlignVCenter | Qt::AlignLeft, presetLines.at(index)); // wjy: 直接展示智能优先级和安全档，用户无需逐窗口操作按钮。
        }
        painter.restore();
        return;
    }
    // ===end====

    if (keyboardSelected) {
        const QRect keyboardCard = layout.viewport(); // wjy: 键盘页主面板和设备详情页主体面板使用同一位置与高度。
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
        painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
        painter.drawRoundedRect(QRectF(keyboardCard), 4, 4);

        QFont title(textFont);
        title.setPixelSize(14);
        title.setBold(true);
        painter.setFont(title);
        painter.setPen(QColor(QStringLiteral("#040B18")));
        painter.drawText(QRectF(keyboardCard.x() + 28, keyboardCard.y() + 18, keyboardCard.width() - 56, 22),
            Qt::AlignVCenter | Qt::AlignLeft,
            QString::fromUtf8("键盘快捷键")); // wjy: 键盘页统一展示并编辑主窗口、远控窗口和删除设备快捷键。

        const struct ShortcutRow {
            const char* action;
            const char* detail;
        } rows[] = {
            {"全屏切换", "远程窗口在全屏和原本窗口之间切换"},
            {"平铺切换", "平铺当前远程窗口，再按一次恢复原来位置"},
            {"关闭最上方窗口", "关闭当前最上方的一个远控窗口"},
            {"关闭全部窗口", "关闭所有已经调出的远控窗口"},
            {"剪切板同步", "开启或关闭远控窗口剪切板同步"},
            {"鼠标锁定", "切换当前远控窗口的手动相对鼠标锁定"},
            {"键鼠录制", "开始或结束当前远控窗口的键鼠录制"},
            {"脚本播放", "选择脚本并开始或停止键鼠回放"},
            {"删除设备", "从本机设备列表中移除当前选中的设备"},
        };

        QFont rowTitle(textFont);
        rowTitle.setPixelSize(13);
        QFont rowDetail(textFont);
        rowDetail.setPixelSize(12);
        painter.save();
        painter.setClipRect(keyboardCard.adjusted(0, 56, 0, -1)); // wjy: 追加快捷键行超出最低窗口高度时，只裁剪内容区，标题仍固定在卡片顶部。
        painter.translate(0, -layout.scrollOffset()); // wjy: 键盘页快捷键行与真实 QLineEdit 共用设置页滚动偏移，保证 9 行在窄窗口仍可访问。
        for (int i = 0; i < kShortcutEditorCount; ++i) {
            const int y = keyboardCard.y() + 64 + i * 48;
            const QRect keyRect = settingsShortcutKeyEditRect(i); // wjy: 绘制背景和真实输入框共用同一矩形，避免视觉/点击区域错位。
            // wjy: 快捷键行之间不再画分隔横线，界面更紧凑。
            const int iconKind = i < 5 ? i + 1 : i + 2; // wjy: 保留 iconKind=6 给常规页自动壁纸，新增远控快捷键从 7 开始。
            drawSettingsOptionIcon(painter, QRect(keyboardCard.x() + 28, y + 2, 28, 28), iconKind);
            painter.setFont(rowTitle);
            painter.setPen(QColor(QStringLiteral("#111827")));
            painter.drawText(QRectF(keyboardCard.x() + 72, y, 180, 18), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(rows[i].action));
            painter.setFont(rowDetail);
            painter.setPen(QColor(QStringLiteral("#687384")));
            painter.drawText(QRectF(keyboardCard.x() + 72, y + 20, qMax(120, keyRect.left() - keyboardCard.x() - 88), 18), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(rows[i].detail));
            drawShortcutKey(painter, keyRect, remoteShortcutDisplayText(i));
        }
        painter.restore();
        const int keyboardMaxScrollOffset = keyboardShortcutMaxScrollOffset();
        if (keyboardMaxScrollOffset > 0) {
            const QRect track = settingsVerticalScrollbarTrackRect();
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#E5EAF1")));
            painter.drawRoundedRect(QRectF(track), 2.5, 2.5); // wjy: 键盘页超出最低窗口高度时显示与常规页一致的滚动轨道。
            painter.setBrush(QColor(QStringLiteral("#AAB3C0")));
            painter.drawRoundedRect(QRectF(settingsVerticalScrollbarThumbRect(layout.scrollOffset(), keyboardMaxScrollOffset)), 2.5, 2.5); // wjy: 滑块位置与快捷键行滚动偏移保持一致。
        }
        painter.restore();
        return;
    }

    painter.save();
    painter.setClipRect(layout.viewport());
    painter.translate(0, -layout.scrollOffset()); // wjy: 手绘常规页与布局快照保持同一滚动位置。

    const int settingsYShift = kDetailScriptPanelTop - 120; // wjy: 以旧常规页第一张卡片 y=120 为基准，整体移动到设备详情页主体顶部。
    const QRect startupCard(contentLeft(), 120 + settingsYShift, contentWidth(), 144);
    const QRect sleepCard(contentLeft(), 268 + settingsYShift, contentWidth(), 71);
    const QRect refreshCard(contentLeft(), 344 + settingsYShift, contentWidth(), 71);
    const QRect batchCard(contentLeft(), 420 + settingsYShift, contentWidth(), 88);
    // =====wjy====
    const QRect periodicDiscoveryCard(contentLeft(), 512 + settingsYShift, contentWidth(), 71); // wjy: 周期检查新增设备紧跟在批量新增设备下面。
    const QRect hideLocalDeviceCard(contentLeft(), 588 + settingsYShift, contentWidth(), 71); // wjy: 本机可见性紧跟发现设置，用户可以直观看到它会影响设备列表和批量新增。
    const QRect wallpaperTestCard(contentLeft(), 664 + settingsYShift, contentWidth(), 71); // wjy: 自动壁纸卡片在新增本机可见性卡片后顺延一行。
    const QRect rollbackCard(contentLeft(), 740 + settingsYShift, contentWidth(), 71); // wjy: 所有运行版本继续显示回撤卡片，并随新增设置顺延。
    const QRect updateCard(contentLeft(), 816 + settingsYShift, contentWidth(), 56); // wjy: 开发构建发布卡片保持位于回撤下方，普通运行包仍完全隐藏。
    // ===end====
    const bool canPublishUpdates = platform::UpdateService::canPublishCurrentBuild(); // wjy: 绘制、命中测试和服务层统一使用构建目录身份。
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(QRectF(startupCard).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    painter.drawRoundedRect(QRectF(sleepCard).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    painter.drawRoundedRect(QRectF(refreshCard).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    painter.drawRoundedRect(QRectF(batchCard).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    painter.drawRoundedRect(QRectF(periodicDiscoveryCard).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4); // wjy: 新设置项始终显示，开关关闭时仅隐藏秒数输入框。
    painter.drawRoundedRect(QRectF(hideLocalDeviceCard).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4); // wjy: 隐藏本机开关使用独立卡片，避免与周期发现的秒数输入混淆。
    painter.drawRoundedRect(QRectF(wallpaperTestCard).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4); // wjy: 手绘卡片始终随常规页内容滚动，真实分钟输入框只覆盖右侧编辑区。
    painter.drawRoundedRect(QRectF(rollbackCard).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4); // wjy: 回撤版本沿用常规设置白底细边卡片，不引入新的视觉体系。
    if (canPublishUpdates) {
        painter.drawRoundedRect(QRectF(updateCard).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4); // wjy: 只有构建版本显示完整发布卡片，普通运行包不绘制任何占位背景。
    }
    painter.drawRoundedRect(QRectF(settingsLocalInfoCardRect(localInfoExpanded)).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    painter.drawLine(QPointF(startupCard.left(), startupCard.y() + 72.5), QPointF(startupCard.right(), startupCard.y() + 72.5)); // wjy: 开机自启卡片内部横线随整体上移后的卡片位置计算。
    if (localInfoExpanded) {
        const QRect localCard = settingsLocalInfoCardRect(true);
        painter.drawLine(QPointF(localCard.left(), localCard.y() + 43.5), QPointF(localCard.right(), localCard.y() + 43.5));
    }

    drawSettingsOptionIcon(painter, QRect(contentLeft() + 18, startupCard.y() + 20, 28, 28), 0);
    drawSettingsOptionIcon(painter, QRect(contentLeft() + 18, startupCard.y() + 92, 28, 28), 1);
    drawSettingsOptionIcon(painter, QRect(contentLeft() + 18, sleepCard.y() + 22, 28, 28), 2);
    drawSettingsOptionIcon(painter, QRect(contentLeft() + 18, refreshCard.y() + 22, 28, 28), 3);
    drawSettingsOptionIcon(painter, QRect(contentLeft() + 18, batchCard.y() + 30, 28, 28), 4);
    drawSettingsOptionIcon(painter, QRect(contentLeft() + 18, periodicDiscoveryCard.y() + 22, 28, 28), 3); // wjy: 周期检查复用刷新类图标，与功能语义一致。
    drawSettingsOptionIcon(painter, QRect(contentLeft() + 18, hideLocalDeviceCard.y() + 22, 28, 28), 11); // wjy: 带斜线的眼睛图标表示开启后从设备界面隐藏本机。
    drawSettingsOptionIcon(painter, QRect(contentLeft() + 18, wallpaperTestCard.y() + 22, 28, 28), 6); // wjy: 横向图片图标帮助用户快速识别自动桌面壁纸入口。
    drawSettingsOptionIcon(painter, QRect(contentLeft() + 18, rollbackCard.y() + 22, 28, 28), 3); // wjy: 回撤同样属于版本切换动作，复用刷新类图标提示程序会重新启动。
    drawSettingsOptionIcon(painter, QRect(contentLeft() + 18, settingsLocalInfoHeaderRect().y() + 8, 28, 28), 5);
    drawUiIcon(
        painter,
        QRect(contentLeft() + contentWidth() - 38, settingsLocalInfoHeaderRect().y() + 10, 24, 24),
        localInfoExpanded ? QStringLiteral("chevron_up.svg") : QStringLiteral("chevron_down.svg"));

    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(contentLeft() + 60, startupCard.y() + 26, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("开机自动启动"));
    painter.drawText(QRectF(contentLeft() + 60, startupCard.y() + 88, 230, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("允许通过远程开机启动"));
    painter.drawText(QRectF(contentLeft() + 60, sleepCard.y() + 16, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("防止电脑休眠"));
    painter.drawText(QRectF(contentLeft() + 60, refreshCard.y() + 16, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("实时状态同步")); // wjy: 旧“列表自动刷新”改为只读实时广播说明，不再暗示周期 TCP 轮询。
    painter.drawText(QRectF(contentLeft() + 60, batchCard.y() + 24, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("批量新增设备"));
    painter.drawText(QRectF(contentLeft() + 60, periodicDiscoveryCard.y() + 16, 220, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("周期检查新增设备"));
    painter.drawText(QRectF(contentLeft() + 60, hideLocalDeviceCard.y() + 16, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("隐藏本机设备")); // wjy: 标题明确该开关只控制当前电脑在设备界面的可见性。
    painter.drawText(QRectF(contentLeft() + 60, wallpaperTestCard.y() + 16, 160, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("自动切换桌面壁纸")); // wjy: 标题明确表示开关控制的是持续自动轮换，并为右侧运行状态留出空间。
    painter.drawText(
        QRectF(contentLeft() + 60, rollbackCard.y() + 16, qMax(80, settingsRollbackVersionComboRect().left() - contentLeft() - 76), 20),
        Qt::AlignVCenter | Qt::AlignLeft,
        QString::fromUtf8("回撤版本")); // wjy: 窄窗口下标题宽度止于下拉框左侧，不会被真实控件覆盖或透出残字。
    if (!wallpaperRotationStatusText.isEmpty()) {
        QFont wallpaperStatusFont(textFont);
        wallpaperStatusFont.setPixelSize(11);
        painter.setFont(wallpaperStatusFont);
        painter.setPen(QColor(QStringLiteral("#687384")));
        painter.drawText(
            QRectF(contentLeft() + 230, wallpaperTestCard.y() + 16, qMax(0, settingsWallpaperRotationIntervalInputRect().left() - contentLeft() - 240), 20),
            Qt::AlignVCenter | Qt::AlignLeft,
            wallpaperRotationStatusText); // wjy: 自动轮换结果直接留在卡片中，周期失败不会每分钟弹窗打断用户。
    }
    // =====wjy====
    if (canPublishUpdates) {
        painter.drawText(QRectF(contentLeft() + 18, updateCard.y() + 8, 160, 18), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("软件更新发布")); // wjy: 构建版本说明此区域只负责向共享目录发布新包。
        painter.setPen(QColor(QStringLiteral("#687384")));
        QFont updateSub(textFont);
        updateSub.setPixelSize(11);
        painter.setFont(updateSub);
        painter.drawText(QRectF(contentLeft() + 18, updateCard.y() + 28, 360, 18), Qt::AlignVCenter | Qt::AlignLeft,
            QString::fromUtf8("共享目录: \\\\192.168.1.100\\广告部工具\\远程软件_FS"));
    }
    // ===end====
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(contentLeft() + 60, settingsLocalInfoHeaderRect().y() + 12, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("本机信息"));

    QFont subFont(textFont);
    subFont.setPixelSize(12);
    painter.setFont(subFont);
    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(QRectF(contentLeft() + 60, startupCard.y() + 109, qMax(260, contentWidth() - 220), 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("协助配置本设备进行远程开机"));
    painter.drawText(QRectF(contentLeft() + 60, sleepCard.y() + 37, qMax(260, contentWidth() - 220), 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("休眠将导致电脑无法远程控制"));
    painter.drawText(QRectF(contentLeft() + 60, refreshCard.y() + 37, qMax(260, contentWidth() - 260), 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("状态变化立即同步，心跳仅用于识别异常退出和离线")); // wjy: 明确业务变化是事件驱动，1/5 秒心跳不是全设备轮询。
    painter.drawText(QRectF(contentLeft() + 60, batchCard.y() + 45, 220, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("可输入多个网段，用空格、逗号或换行分隔"));
    painter.drawText(QRectF(contentLeft() + 60, periodicDiscoveryCard.y() + 37, qMax(260, contentWidth() - 340), 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("按批量新增网段自动发现并加入新设备"));
    painter.drawText(QRectF(contentLeft() + 60, hideLocalDeviceCard.y() + 37, qMax(260, contentWidth() - 220), 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("开启后设备列表和批量新增不再显示当前电脑")); // wjy: 直接说明开启、批量发现和当前列表三者的关系。
    painter.drawText(
        QRectF(contentLeft() + 60, wallpaperTestCard.y() + 37, qMax(120, settingsWallpaperRotationIntervalInputRect().left() - contentLeft() - 76), 20),
        Qt::AlignVCenter | Qt::AlignLeft,
        platform::DesktopWallpaperService::sharedDirectoryPath()); // wjy: 在分钟输入框旁直接展示固定 UNC 来源，用户开启前就能核对图片放置位置。
    painter.drawText(
        QRectF(contentLeft() + 60, rollbackCard.y() + 37, qMax(100, settingsRollbackVersionComboRect().left() - contentLeft() - 76), 20),
        Qt::AlignVCenter | Qt::AlignLeft,
        QString::fromUtf8("选择历史版本，回撤完成后自动重启")); // wjy: 在操作前直接说明重启结果，确认弹窗再提示配置兼容风险。

    const auto drawSwitchWithLabel = [&painter, &textFont](const QRect& rect, bool checked) {
        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#040B18")));
        painter.drawText(QRectF(rect.x(), rect.y() + 6, 24, 20), Qt::AlignVCenter | Qt::AlignLeft, checked ? QString::fromUtf8("开") : QString::fromUtf8("关"));
        drawSettingsSwitch(painter, rect.x() + 35, rect.y() + 6, checked);
    };
    drawSwitchWithLabel(settingsAutoRunSwitchRect(), autoRunEnabled);
    drawSwitchWithLabel(settingsRemoteWakeupSwitchRect(), remoteWakeupEnabled);
    drawSwitchWithLabel(settingsPreventSleepSwitchRect(), preventSleepEnabled);
    drawSwitchWithLabel(settingsPeriodicDeviceDiscoverySwitchRect(), periodicDeviceDiscoveryEnabled); // wjy: 开关绘制与列表自动刷新完全一致。
    drawSwitchWithLabel(settingsHideLocalDeviceSwitchRect(), hideLocalDeviceEnabled); // wjy: 本机可见性复用统一开关样式，并和点击命中使用同一矩形。
    drawSwitchWithLabel(settingsWallpaperRotationSwitchRect(), wallpaperRotationEnabled); // wjy: 壁纸轮换复用统一“开/关 + 蓝色滑块”视觉。
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#16A34A")));
    painter.drawText(QRectF(settingsAutoRefreshSwitchRect()), Qt::AlignCenter, QString::fromUtf8("已启用")); // wjy: 实时总线为固定基础能力，设置页只读展示，不提供重新开启旧轮询的入口。
    // =====wjy====
    if (canPublishUpdates) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(versionTransactionBusy
            ? QColor(QStringLiteral("#C9D0DA"))
            : QColor(QStringLiteral("#3A7BFC"))); // wjy: 升级、回撤或发布任一版本事务运行时都禁用发布按钮，视觉与互斥规则保持一致。
        painter.drawRoundedRect(QRectF(settingsPublishUpdateButtonRect()), 4, 4);
        QFont updateBtnFont(textFont);
        updateBtnFont.setPixelSize(12);
        painter.setFont(updateBtnFont);
        painter.setPen(QColor(QStringLiteral("#FFFFFF")));
        painter.drawText(settingsPublishUpdateButtonRect(), Qt::AlignCenter,
            publishPreparing ? QString::fromUtf8("发布中") : QString::fromUtf8("发布更新")); // wjy: 复制和校验期间明确显示进行中，普通设备仍不绘制该按钮。
    }
    // ===end====
    // =====wjy====
    painter.setPen(Qt::NoPen);
    painter.setBrush(rollbackVersionAvailable && !versionTransactionBusy
        ? QColor(QStringLiteral("#3A7BFC"))
        : QColor(QStringLiteral("#C9D0DA"))); // wjy: 没有合法历史版本或任一版本事务运行时使用禁用色，绘制状态与实际点击条件完全一致。
    painter.drawRoundedRect(QRectF(settingsRollbackButtonRect()), 4, 4);
    QFont rollbackButtonFont(textFont);
    rollbackButtonFont.setPixelSize(12);
    painter.setFont(rollbackButtonFont);
    painter.setPen(QColor(QStringLiteral("#FFFFFF")));
    painter.drawText(settingsRollbackButtonRect(), Qt::AlignCenter,
        rollbackPreparing ? QString::fromUtf8("准备中") : QString::fromUtf8("回撤")); // wjy: 暂存大文件期间即时显示准备状态并阻止重复创建更新器任务。
    // ===end====
    if (periodicDeviceDiscoveryEnabled) {
        painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
        painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
        painter.drawRoundedRect(QRectF(settingsPeriodicDeviceDiscoveryIntervalInputRect()), 4, 4); // wjy: 开启后显示 60 秒输入框，关闭时卡片保持简洁。
    }
    if (wallpaperRotationEnabled) {
        painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
        painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
        painter.drawRoundedRect(QRectF(settingsWallpaperRotationIntervalInputRect()), 4, 4); // wjy: 只在轮换开启时显示分钟输入背景，关闭状态保持卡片简洁。
        painter.setFont(subFont);
        painter.setPen(QColor(QStringLiteral("#687384")));
        painter.drawText(QRectF(settingsWallpaperRotationIntervalInputRect().right() + 5, settingsWallpaperRotationIntervalInputRect().y(), 35, 32), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("分钟"));
    }

    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(QRectF(settingsBatchSubnetInputRect()), 4, 4);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
    painter.drawRoundedRect(QRectF(settingsBatchAddButtonRect()), 4, 4);

    if (localInfoExpanded) {
        QFont localLabelFont(textFont);
        localLabelFont.setPixelSize(12);
        painter.setFont(localLabelFont);
        painter.setPen(QColor(QStringLiteral("#687384")));
        for (int i = 0; i < 6; ++i) {
            painter.drawText(QRectF(localInfoLabelRect(i)), Qt::AlignVCenter | Qt::AlignLeft, localInfoLabelText(i));
        }

        painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
        painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
        for (int i = 0; i < 6; ++i) {
            painter.drawRoundedRect(QRectF(localInfoFieldRect(i)), 4, 4);
        }

        painter.setFont(localLabelFont);
        painter.setPen(QColor(QStringLiteral("#040B18")));
        for (int i = 0; i < 6; ++i) {
            painter.drawText(localInfoFieldRect(i).adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, localInfoValueText(localInfo, i));
        }
    }

    const QRect addCard = settingsAddDeviceCardRect(localInfoExpanded, addDeviceExpanded);
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(QRectF(addCard).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    if (addDeviceExpanded) {
        painter.drawLine(QPointF(addCard.left(), addCard.y() + 43.5), QPointF(addCard.right(), addCard.y() + 43.5));
    }
    drawSettingsOptionIcon(painter, QRect(addCard.x() + 18, addCard.y() + 8, 28, 28), 4);
    drawUiIcon(
        painter,
        QRect(addCard.right() - 38, addCard.y() + 10, 24, 24),
        addDeviceExpanded ? QStringLiteral("chevron_up.svg") : QStringLiteral("chevron_down.svg"));
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(addCard.x() + 60, addCard.y() + 12, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("新增设备"));

    if (addDeviceExpanded) {
        QFont sectionFont(textFont);
        sectionFont.setBold(true);
        painter.setFont(sectionFont);
        painter.drawText(QRectF(settingsAddDeviceIpEditRect(localInfoExpanded).x(), addCard.y() + 60, 120, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("设备信息"));
        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#687384")));
        painter.drawText(QRectF(settingsAddDeviceIpEditRect(localInfoExpanded).x(), addCard.y() + 84, 360, 18), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("添加后可以在左侧设备列表中找到它"));
        painter.setPen(QPen(QColor(QStringLiteral("#EEF1F5")), 1));
        painter.drawLine(addCard.left(), addCard.y() + 112, addCard.right(), addCard.y() + 112);
        painter.setPen(QColor(QStringLiteral("#687384")));
        painter.drawText(QRectF(settingsAddDeviceIpEditRect(localInfoExpanded).x(), addCard.y() + 136, 120, 18), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("目标机器IP"));
        painter.drawText(QRectF(settingsAddDeviceNameEditRect(localInfoExpanded).x(), addCard.y() + 136, 120, 18), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("设备名称"));
        painter.drawText(QRectF(settingsAddDeviceMacEditRect(localInfoExpanded).x(), addCard.y() + 218, 120, 18), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("MAC"));
        painter.drawText(QRectF(settingsAddDeviceRemarkEditRect(localInfoExpanded).x(), addCard.y() + 218, 120, 18), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("备注"));

        painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
        painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
        painter.drawRoundedRect(QRectF(settingsAddDeviceIpEditRect(localInfoExpanded)), 4, 4);
        painter.drawRoundedRect(QRectF(settingsAddDeviceNameEditRect(localInfoExpanded)), 4, 4);
        painter.drawRoundedRect(QRectF(settingsAddDeviceMacEditRect(localInfoExpanded)), 4, 4);
        painter.drawRoundedRect(QRectF(settingsAddDeviceRemarkEditRect(localInfoExpanded)), 4, 4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
        painter.drawRoundedRect(QRectF(settingsAddDeviceSaveButtonRect(localInfoExpanded)), 4, 4);
        painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
        painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
        painter.drawRoundedRect(QRectF(settingsAddDeviceCancelButtonRect(localInfoExpanded)), 4, 4);
    }

    painter.restore();

    const int maxScrollOffset = layout.maxScrollOffset();
    if (maxScrollOffset > 0) {
        const QRect track = settingsVerticalScrollbarTrackRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#E5EAF1")));
        painter.drawRoundedRect(QRectF(track), 2.5, 2.5);
        painter.setBrush(QColor(QStringLiteral("#AAB3C0")));
        painter.drawRoundedRect(QRectF(settingsVerticalScrollbarThumbRect(layout.scrollOffset(), maxScrollOffset)), 2.5, 2.5);
    }
}

} // namespace

DeviceGrid::DeviceGrid(platform::DeviceRealtimeStateService* realtimeStateService, QWidget* parent)
    : QFrame(parent)
    , m_realtimeStateService(realtimeStateService)
    , m_remoteViewerLifecycleManager(std::make_unique<RemoteViewerLifecycleManager>(4, 4)) // wjy: 同时最多4路初始化、4个可等待生命周期线程；最终在线窗口数量不设上限。
    , m_remoteWindowCoordinator(std::make_unique<RemoteWindowCoordinator>()) // wjy: 远控窗口协调器先于所有窗口创建，统一承接普通/平铺集合和生命周期索引。
{
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] DeviceGrid ctor begin")); // wjy: 进入 DeviceGrid 构造函数，定位 MainWindow 创建内部崩溃。
    setObjectName(QStringLiteral("DeviceGrid")); // wjy: 恢复正常 QObject 名称，便于样式、调试和对象树识别。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before resize setup after setObjectName")); // wjy: 对象名设置完成后继续记录窗口基础属性初始化。
    setMinimumSize(720, 520);
    resize(kDefaultShellWidth, kDefaultShellHeight);
    syncResponsiveLayoutState();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after resize setup before setMouseTracking")); // wjy: 记录尺寸设置完成，继续判断是否崩在鼠标追踪设置。
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus); // wjy: 允许手绘区域被点击后接管焦点，数字输入框点击外部时才能真正失焦并保存。
    // =====wjy====
#if 0
    // wjy: 查找功能暂缓启用；保留完整 Ctrl+F 注册代码，后续删除本门禁即可恢复快捷键入口。
    auto* deviceSearchShortcut = new QShortcut(QKeySequence::Find, this); // wjy: 使用 Qt 标准查找键序列注册 Ctrl+F，避免手工判断平台按键差异。
    deviceSearchShortcut->setContext(Qt::WidgetWithChildrenShortcut); // wjy: 主控窗口及其输入框有焦点时均可搜索，远控窗口获得焦点时不会误触发。
    connect(deviceSearchShortcut, &QShortcut::activated,
        this, &DeviceGrid::showDeviceSearchPanel); // wjy: Ctrl+F 直接切到主界面查找页，不再创建模态弹窗。
#endif
    // ===end====
    // =====wjy====
    connect(&platform::SharedStorageAvailabilityService::instance(), &platform::SharedStorageAvailabilityService::probeFinished,
        this, &DeviceGrid::handleSharedStorageProbeFinished); // wjy: 更新、回撤和壁纸共享同一轮非阻塞 SMB 端口探测，离线时不会分别启动任务。
    connect(&platform::UpdateService::instance(), &platform::UpdateService::updateAvailabilityChanged,
        this, [this](bool available, const QString& remoteVersion) {
            const QString confirmedRemoteVersion = remoteVersion.trimmed();
            const bool updateStateChanged = m_updateAvailable != available
                || m_availableUpdateVersion != confirmedRemoteVersion; // wjy: 即使本机已是最新版，也保留已确认的共享版本供目标设备按钮比较。
            m_updateAvailable = available; // wjy: 后台检查仅改变标题栏入口可见性，不在信号处理中启动安装或弹出窗口。
            m_availableUpdateVersion = confirmedRemoteVersion;
            // wjy: 不在周期检查信号中重置 m_updatePreparing；后台载荷仍在复制时必须继续阻止重复更新任务。
            update(titlebarUpdateRect().adjusted(-2, -2, 2, 2)); // wjy: 只重绘标题栏更新区域，及时显示或隐藏按钮。
            refreshRealtimeUpdateAvailability();
            if (updateStateChanged && m_settingsSelected && m_settingsTab == SettingsTab::General) {
                refreshRollbackVersions(true); // wjy: 只有远端版本状态变化时才重新探测并刷新回撤列表，固定一分钟检查不会反复创建枚举线程。
            }
        });
    // ===end====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after basic widget setup")); // wjy: 记录基础 QWidget 属性设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before app settings restore")); // wjy: 恢复读取用户设置前打点，若再次异常可以确认是否来自 QSettings/注册表。
    m_autoRunEnabled = platform::StartupManager::isEnabled(); // wjy: 读取当前开机自启状态，让设置页显示真实开关值。
    if (!m_autoRunEnabled) { //永远开机自启
        platform::StartupManager::setEnabled(true);
        m_autoRunEnabled = true;
    }
    m_remoteWakeupEnabled = platform::AppSettings::remoteWakeupEnabled(); // wjy: 读取远程开机设置，恢复用户上次选择。

    m_preventSleepEnabled = platform::AppSettings::preventSleepEnabled(); // wjy: 读取防睡眠设置，后续同步应用到系统执行状态。
    // =====wjy====
    m_periodicDeviceDiscoveryEnabled = platform::AppSettings::periodicDeviceDiscoveryEnabled();
    m_periodicDeviceDiscoveryIntervalSeconds = platform::AppSettings::periodicDeviceDiscoveryIntervalSeconds(); // wjy: 恢复周期新增开关和秒数，新安装默认关闭且为 60 秒。
    m_hideLocalDeviceEnabled = platform::AppSettings::hideLocalDeviceEnabled(); // wjy: 在加载设备目录前恢复本机过滤状态，避免启动首帧先显示再隐藏。
    const QString localWallpaperDeviceName = platform::DeviceInfoService::localDeviceName(); // wjy: 启动阶段只读取计算机名，不提前执行原本延迟的完整网卡枚举。
    g_hideLocalDeviceFromList = m_hideLocalDeviceEnabled; // wjy: 可见行构建发生在构造函数后续阶段，先发布当前窗口的隐藏策略。
    g_localDeviceIdentityForList = platform::DeviceInfo{};
    g_localDeviceIdentityForList.name = localWallpaperDeviceName; // wjy: 启动早期先用轻量计算机名过滤，500ms 后再补齐本机 IP/MAC。
    const bool defaultWallpaperRotationEnabled = platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(localWallpaperDeviceName); // wjy: 数字开头设备在没有历史开关配置时默认启动自动壁纸。
    m_wallpaperRotationEnabled = platform::AppSettings::desktopWallpaperRotationEnabled(defaultWallpaperRotationEnabled); // wjy: 已保存的用户开关优先，用户明确关闭后不会被设备名规则重新打开。
    m_wallpaperRotationIntervalMinutes = platform::AppSettings::desktopWallpaperRotationIntervalMinutes(); // wjy: 恢复自动壁纸分钟数，新安装仍使用 1 分钟默认周期。
    // ===end====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before PowerManager restore")); // wjy: 应用防睡眠前打点，便于区分设置读取和系统 API 调用。
    platform::PowerManager::setPreventSleepEnabled(m_preventSleepEnabled); // wjy: 根据保存的设置恢复防睡眠，保证重启程序后行为一致。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before loadDevices restore")); // wjy: 加载设备文件前打点，验证 devices.json 读写路径是否稳定。
    loadDevices(); // wjy: 恢复读取已保存设备和分组，避免每次启动都丢失设备列表。
    // =====wjy====
    updateRealtimeConfiguredDevices();
    if (m_realtimeStateService) {
        connect(m_realtimeStateService, &platform::DeviceRealtimeStateService::deviceStateChanged,
            this, &DeviceGrid::applyRealtimeDeviceState); // wjy: UDP 接收和 TTL 已在服务内归并，UI 线程只接收按设备 IP 的最终状态。
        connect(m_realtimeStateService, &platform::DeviceRealtimeStateService::deviceExpired,
            this, [this](const QString& deviceIp) {
                writeDeviceGridStartupLog(QStringLiteral("[wjy-realtime] device expired ip=%1").arg(deviceIp)); // wjy: 记录异常退出/断电触发的 TTL 离线，便于区分用户手动刷新结果。
            });
    }
    // ===end====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] defer DeviceInfoService::local")); // wjy: Release 堆损坏诊断：构造函数里先不读取网卡信息，避免窗口创建阶段触发 GetAdaptersAddresses。
    const int initialUnhiddenDeviceIndex = firstUnhiddenDeviceIndex(); // wjy: 初始详情选择第一台未被本机过滤的设备，折叠分组仍沿用原有选择语义。
    if (initialUnhiddenDeviceIndex < 0) {
        m_settingsSelected = true;
        m_remoteAssistSelected = false;
        m_settingsAddDeviceExpanded = true;
        m_settingsScrollOffset = maxSettingsScrollOffset(m_settingsLocalInfoExpanded, m_settingsAddDeviceExpanded);

        m_selectedDeviceIndexes.clear();
        m_selectionAnchorDeviceIndex = -1;
    } else {
        m_selectedDeviceIndex = initialUnhiddenDeviceIndex;
        m_selectedDeviceIndexes.insert(initialUnhiddenDeviceIndex);
        m_selectionAnchorDeviceIndex = initialUnhiddenDeviceIndex;

        m_currentDeviceName =
            deviceDisplayName(g_devices.at(initialUnhiddenDeviceIndex)); // wjy: 目录中排在前面的隐藏本机不会成为启动详情目标。
    }
    m_previousDeviceName = m_currentDeviceName;
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after current device init")); // wjy: 记录当前设备选择状态初始化完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before setupAddDeviceControls")); // wjy: 记录新增设备输入控件创建前的位置。
    setupAddDeviceControls();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after setupAddDeviceControls")); // wjy: 记录新增设备输入控件创建完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before setupLocalInfoControls")); // wjy: 记录本机信息控件创建前的位置。
    setupLocalInfoControls();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after setupLocalInfoControls")); // wjy: 记录本机信息控件创建完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before setupSettingsControls")); // wjy: 记录设置页控件创建前的位置。
    setupSettingsControls();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after setupSettingsControls")); // wjy: 记录设置页控件创建完成。
    setupScriptFileEditor();
    setupScriptFolderTree();
    // setupDeviceSearchPanel(); // wjy: 查找 UI 暂缓启用；保留创建入口，后续取消本行注释即可恢复面板实例。
    updateSettingsControls();
    updateLocalInfoControls();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after update controls")); // wjy: 记录控件显隐和内容刷新完成。

// =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before group rename editor create")); // wjy: 记录分组原地重命名输入框创建前的位置。
    m_deviceGroupNameEdit = new QLineEdit(this); // wjy: 创建分组原地重命名输入框，平时隐藏，双击分组行时才显示。
    m_deviceGroupNameEdit->setVisible(false); // wjy: 初始不显示，避免覆盖左侧设备列表。
    m_deviceGroupNameEdit->setStyleSheet(QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #3A7BFC;border-radius:4px;"
        "padding:0 8px;font-family:'Microsoft YaHei UI';font-size:14px;color:#111827;}")); // wjy: 蓝色边框表示当前正在编辑分组名称。
    connect(m_deviceGroupNameEdit, &QLineEdit::returnPressed, this, [this] { // wjy: 用户按回车时保存当前输入。
        finishDeviceGroupRename(true);
    });
    connect(m_deviceGroupNameEdit, &QLineEdit::editingFinished, this, [this] { // wjy: 输入框失焦时也保存，满足点击外部关闭输入框的需求。
        finishDeviceGroupRename(true);
    });
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after group rename editor create")); // wjy: 记录分组原地重命名输入框创建和信号连接完成。

    m_deviceListNameEdit = new QLineEdit(this); // wjy: 设备通过右键“系统设置/重命名”在左侧列表原地编辑，双击交互专用于启动远控。
    m_deviceListNameEdit->setVisible(false);
    m_deviceListNameEdit->setStyleSheet(QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #3A7BFC;border-radius:4px;"
        "padding:0 8px;font-family:'Microsoft YaHei UI';font-size:14px;color:#111827;}"));
    connect(m_deviceListNameEdit, &QLineEdit::returnPressed, this, [this] {
        finishDeviceRename(true);
    });
    connect(m_deviceListNameEdit, &QLineEdit::editingFinished, this, [this] {
        finishDeviceRename(true);
    });
// ===end====

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before detail animation timer")); // wjy: 记录详情动画定时器创建前的位置。
    m_detailAnimationTimer = new QTimer(this);
    m_detailAnimationTimer->setTimerType(Qt::PreciseTimer);
    m_detailAnimationTimer->setInterval(16);
    connect(m_detailAnimationTimer, &QTimer::timeout, this, [this] {
        constexpr qint64 durationMs = 220;
        m_detailAnimationProgress = qMin<qreal>(1.0, m_detailAnimationClock.elapsed() / qreal(durationMs));
        if (m_detailAnimationProgress >= 1.0) {
            m_detailAnimationTimer->stop();
        }
        update();
    });
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after detail animation timer")); // wjy: 记录详情动画定时器创建完成。

    // =====wjy====
    const auto setupLocalUsageAnimation = [this](QVariantAnimation*& animation, qreal* displayPercent) {
        animation = new QVariantAnimation(this); // wjy: 每类资源独立持有动画，三枚圆环可以同时从各自当前值过渡。
        animation->setDuration(320); // wjy: 320ms 能清楚看见环形扫动方向，同时在下一次一秒采样前完成。
        animation->setEasingCurve(QEasingCurve::OutCubic); // wjy: 前段快速响应、末端柔和减速，数值升降不会闪跳。
        connect(animation, &QVariantAnimation::valueChanged, this, [this, displayPercent](const QVariant& value) {
            *displayPercent = qBound<qreal>(0.0, value.toReal(), 100.0); // wjy: 保留小数精度计算圆弧角度，中心数字再按帧取整。
            update(scriptFileEditorRect()); // wjy: 任意资源动画帧只刷新本机详情区域，三枚圆环互不影响其它页面。
        });
    };
    setupLocalUsageAnimation(m_localCpuUsageAnimation, &m_localCpuUsagePercent);
    setupLocalUsageAnimation(m_localGpuUsageAnimation, &m_localGpuUsagePercent);
    setupLocalUsageAnimation(m_localMemoryUsageAnimation, &m_localMemoryUsagePercent);

    m_localSystemInfoTimer = new QTimer(this);
    m_localSystemInfoTimer->setInterval(1000); // wjy: CPU、GPU、内存统一每秒采样，三枚圆环保持相同刷新节奏。
    m_localSystemInfoTimer->setTimerType(Qt::CoarseTimer);
    connect(m_localSystemInfoTimer, &QTimer::timeout, this, [this] {
        const bool localPageVisible = !m_settingsSelected
            && !m_remoteAssistSelected
            && !m_localInfoSelected
            && m_deviceDetailTab == DeviceDetailTab::Local;
        if (!localPageVisible) {
            m_localSystemInfoTimer->stop(); // wjy: 页面状态变化但尚未来得及刷新控件时，定时回调再次兜底停止隐藏采样。
            m_localDiskRefreshTick = 0; // wjy: 离开本机页后清空累计次数，下次进入由完整快照重新建立磁盘基线。
            m_localCpuUsageAnimation->stop();
            m_localGpuUsageAnimation->stop();
            m_localMemoryUsageAnimation->stop(); // wjy: 页面已经隐藏时同时停止三类显示动画，不再投递无意义重绘。
            m_localCpuUsageSampler.reset(); // wjy: 隐藏页面不保留累计时间基线，下次进入必须重新取得两个相邻样本。
            m_localGpuUsageSampler.reset(); // wjy: GPU 查询句柄跟随页面可见性释放，隐藏期间不持续采集性能计数器。
            m_localCpuUsagePercent = -1.0;
            m_localGpuUsagePercent = -1.0;
            m_localMemoryUsagePercent = -1.0;
            m_localMemoryUsage = {}; // wjy: 清除动态内存容量快照，下次进入不会短暂显示上一次页面的已用和可用值。
            return;
        }
        const int sampledCpuPercent = m_localCpuUsageSampler.samplePercent(); // wjy: 一次定时回调连续读取三类只读资源快照。
        const int sampledGpuPercent = m_localGpuUsageSampler.samplePercent();
        const platform::LocalMemoryUsage sampledMemory = m_localMemoryUsageSampler.sample(); // wjy: 百分比和容量从同一份系统快照取得，内存文字不会与圆环来自不同时间点。
        if (sampledCpuPercent >= 0) {
            animateLocalUsageTo(m_localCpuUsageAnimation, m_localCpuUsagePercent, sampledCpuPercent);
        }
        if (sampledGpuPercent >= 0) {
            animateLocalUsageTo(m_localGpuUsageAnimation, m_localGpuUsagePercent, sampledGpuPercent);
        }
        if (sampledMemory.percent >= 0) {
            m_localMemoryUsage = sampledMemory; // wjy: 先提交最新容量，再让圆环平滑过渡；每次重绘看到的容量始终是最新完整快照。
            animateLocalUsageTo(m_localMemoryUsageAnimation, m_localMemoryUsagePercent, sampledMemory.percent); // wjy: 三类有效样本分别从当前画面平滑过渡，不会互相重置。
        }
        constexpr int kLocalDiskRefreshTicks = 10;
        if (++m_localDiskRefreshTick >= kLocalDiskRefreshTicks) {
            m_localDiskRefreshTick = 0;
            m_localSystemInfo.disks = platform::LocalSystemInfoService::localDisks(); // wjy: 每十秒只更新磁盘总量和已用量，不重复枚举 CPU、GPU、内存条等静态硬件信息。
            update(scriptFileEditorRect()); // wjy: 磁盘容量变化只重绘本机详情内容区域，不影响设备列表和其它页面。
        }
    });
    QTimer::singleShot(0, this, [this] {
        const bool localPageVisible = !m_settingsSelected // wjy: 只有启动后实际落在设备详情区时才加载，空设备进入设置页不会触发系统查询。
            && !m_remoteAssistSelected
            && !m_localInfoSelected
            && m_deviceDetailTab == DeviceDetailTab::Local;
        if (!localPageVisible) {
            return;
        }
        refreshLocalSystemInfoTab(); // wjy: 构造结束并进入事件循环后再读取静态信息及三类资源首样本，避免在窗口构造中同步查询。
        updateLocalSystemMonitorState(); // wjy: 默认本机页可见时启动一秒采样；后续切页仍由统一状态函数立即停止。
        update(scriptFileEditorRect()); // wjy: 延迟数据就绪后只刷新本机详情内容区域，让首次显示立即呈现真实系统信息。
    });
    // ===end====

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before desktop hover timer")); // wjy: 记录桌面悬停动画定时器创建前的位置。
    m_desktopHoverTimer = new QTimer(this);
    m_desktopHoverTimer->setTimerType(Qt::PreciseTimer);
    m_desktopHoverTimer->setInterval(16);
    connect(m_desktopHoverTimer, &QTimer::timeout, this, [this] {
        constexpr qint64 durationMs = 180;
        const qreal target = m_desktopHovered ? 1.0 : 0.0;
        const qreal progress = qMin<qreal>(1.0, m_desktopHoverClock.elapsed() / qreal(durationMs));
        const qreal eased = easeOutCubic(progress);
        m_desktopHoverProgress = m_desktopHoverStartProgress
            + (target - m_desktopHoverStartProgress) * eased;

        if (progress >= 1.0) {
            m_desktopHoverProgress = target;
            m_desktopHoverTimer->stop();
        }
        update();
    });
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after desktop hover timer")); // wjy: 记录桌面悬停动画定时器创建完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before refresh timer")); // wjy: 记录刷新旋转动画定时器创建前的位置。
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setTimerType(Qt::PreciseTimer);
    m_refreshTimer->setInterval(16);
    connect(m_refreshTimer, &QTimer::timeout, this, [this] {
        if (!m_statusRefreshInProgress) {
            m_refreshRotation = 0.0;
            m_refreshTimer->stop();
            update(refreshRect().adjusted(-2, -2, 2, 2));
            return;
        }

        constexpr qreal degreesPerSecond = 405.0;
        const qreal elapsedSeconds = m_refreshClock.elapsed() / 1000.0;
        m_refreshRotation = std::fmod(degreesPerSecond * elapsedSeconds, 360.0);
        update(refreshRect().adjusted(-2, -2, 2, 2));
    });
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after refresh timer")); // wjy: 记录刷新旋转动画定时器创建完成。

    // =====wjy====
    m_settingsGearTimer = new QTimer(this);
    m_settingsGearTimer->setTimerType(Qt::PreciseTimer);
    m_settingsGearTimer->setInterval(16);
    connect(m_settingsGearTimer, &QTimer::timeout, this, [this] {
        constexpr qreal durationMs = 280.0; // wjy: 约 0.28 秒完成半圈，手感接近轻点反馈。
        const qreal progress = qMin<qreal>(1.0, m_settingsGearClock.elapsed() / durationMs);
        m_settingsGearRotation = 180.0 * progress;
        if (progress >= 1.0) {
            m_settingsGearRotation = 180.0;
            m_settingsGearTimer->stop();
        }
        update(titlebarSettingsRect().adjusted(-2, -2, 2, 2));
    });
    // ===end====

    // =====wjy====
    m_remoteQualityTimer = new QTimer(this);
    m_remoteQualityTimer->setTimerType(Qt::CoarseTimer); // wjy: 质量协调是1秒级资源控制，不使用高精度唤醒，降低20窗口场景下主线程定时器开销。
    m_remoteQualityTimer->setInterval(1000);
    connect(m_remoteQualityTimer, &QTimer::timeout, this, &DeviceGrid::evaluateRemoteQuality);
    m_remoteQualityTimer->start(); // wjy: 周期采样只计算和在线改参，永远不关闭、暂停或拒绝已有远控流。
    // ===end====

    // =====wjy====
    m_titlebarBandwidthTimer = new QTimer(this);
    m_titlebarBandwidthTimer->setTimerType(Qt::CoarseTimer); // wjy: 带宽是秒级诊断指标，不使用高精度唤醒增加多窗口场景主线程压力。
    m_titlebarBandwidthTimer->setInterval(1000);
    connect(m_titlebarBandwidthTimer, &QTimer::timeout, this, [this] {
        m_titlebarBandwidthSample = m_titlebarBandwidthMonitor.sample(); // wjy: UI 定时器只读取本机累计计数差，不创建 socket 或额外局域网流量。
        update(titlebarBandwidthUpdateRect()); // wjy: 新样本只刷新版本号右侧的网络文字和可能被覆盖的本机身份区域。
    });
    QTimer::singleShot(0, this, [this] {
        if (m_shuttingDown || !m_titlebarBandwidthTimer) {
            return;
        }
        m_titlebarBandwidthMonitor.reset(); // wjy: 进入事件循环后再建立网卡基线，避免窗口构造阶段同步枚举 Windows 接口。
        m_titlebarBandwidthSample = m_titlebarBandwidthMonitor.sample();
        m_titlebarBandwidthTimer->start(); // wjy: 第二次及后续每秒采样才拥有可计算的真实 Mbps。
        update(titlebarBandwidthUpdateRect());
    });
    // ===end====

    // =====wjy====
    m_periodicDeviceDiscoveryTimer = new QTimer(this);
    connect(m_periodicDeviceDiscoveryTimer, &QTimer::timeout, this, [this] {
        startBatchAddDevices(false); // wjy: 周期扫描复用手动批量新增，但新增后不切换当前页面或设备选择。
    });
    // ===end====

    // =====wjy====
    m_wallpaperRotationTimer = new QTimer(this);
    m_wallpaperRotationTimer->setTimerType(Qt::CoarseTimer); // wjy: 分钟级壁纸任务无需高精度唤醒，降低常驻定时器开销。
    connect(m_wallpaperRotationTimer, &QTimer::timeout, this, [this] {
        startDesktopWallpaperRotation(false); // wjy: 定时触发静默轮换；失败保留开关并等待下一周期重试。
    });
    // ===end====

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before wake visual timer")); // wjy: 记录远程开机视觉定时器创建前的位置。
    m_wakeVisualTimer = new QTimer(this);
    m_wakeVisualTimer->setTimerType(Qt::PreciseTimer);
    m_wakeVisualTimer->setInterval(16);
    connect(m_wakeVisualTimer, &QTimer::timeout, this, [this] {
        if (m_poweringOnDeviceIps.isEmpty()) {
            m_wakeVisualRotation = 0.0;
            m_lastWakeProbeAtMs = 0;
            m_wakeVisualTimer->stop();
            update();
            return;
        }

        constexpr qreal degreesPerSecond = 180.0;
        const qint64 elapsedMs = m_wakeVisualClock.elapsed();
        const qreal elapsedSeconds = elapsedMs / 1000.0;
        m_wakeVisualRotation = std::fmod(degreesPerSecond * elapsedSeconds, 360.0);

        for (auto it = m_poweringOnDeviceIps.begin(); it != m_poweringOnDeviceIps.end();) {
            const qint64 startedAtMs = m_poweringOnStartedAtMs.value(*it, 0);
            if (startedAtMs > 0 && QDateTime::currentMSecsSinceEpoch() - startedAtMs >= 80000) {
                m_poweringOnStartedAtMs.remove(*it);
                it = m_poweringOnDeviceIps.erase(it);
            } else {
                ++it;
            }
        }

        if (!m_poweringOnDeviceIps.isEmpty()
            && !m_wakeProbeInProgress
            && (m_lastWakeProbeAtMs == 0 || elapsedMs - m_lastWakeProbeAtMs >= 2000)) {
            m_lastWakeProbeAtMs = elapsedMs;
            probePoweringOnDevices();
        }

        update();
    });
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after wake visual timer")); // wjy: 记录远程开机视觉定时器创建完成。

    m_scriptOutputFlushTimer = new QTimer(this);
    m_scriptOutputFlushTimer->setInterval(120);
    connect(m_scriptOutputFlushTimer, &QTimer::timeout, this, [this] {
        if (!m_scriptOutputDirty) {
            m_scriptOutputFlushTimer->stop();
            return;
        }
        m_scriptOutputDirty = false;
        m_scriptOutputText = stripTerminalControlSequences(readScriptOutputFileTail(m_scriptOutputFilePath));
        if (m_scriptOutputAutoScroll) {
            m_scriptOutputScrollOffset = 0;
        }
        saveCurrentScriptUiState(); // wjy: 当前设备输出被懒刷新后同步回设备状态缓存，切走再切回仍看到最新滚动位置和文本。
        update(scriptTerminalPanelRect().toAlignedRect().adjusted(-2, -2, 2, 2));
    }); // wjy: Batch script output repaint into a short timer, keeping the UI responsive while still feeling live.

    applyPeriodicDeviceDiscoverySetting(false); // wjy: 启动时只恢复周期，不立即扫描，第一次检查发生在设置的 60 秒之后。
    applyDesktopWallpaperRotationSetting(false); // wjy: 启动时恢复已保存的轮换周期，默认关闭时不会启动定时器或修改桌面。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before delayed local device info setup"));
    QTimer::singleShot(500, this, [this] { // wjy: 窗口创建后再读取本机 IP/MAC，隔离 DeviceInfoService::local 是否导致 Release 启动阶段堆损坏。
        writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] delayed before DeviceInfoService::local")); // wjy: 延迟读取本机信息前打点。
        refreshLocalDeviceInfo();
        if (m_hideLocalDeviceEnabled) {
            applyHideLocalDeviceSetting(false); // wjy: 只有隐藏开关已开启时才按完整 IP/MAC 再过滤，关闭状态不改动折叠分组中的当前选择。
        }
        updateLocalInfoControls();
        update();
        writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] delayed after DeviceInfoService::local")); // wjy: 延迟读取本机信息完成。
    });
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after delayed local device info setup")); // wjy: 启动不再安排全设备 TCP 探测，状态等待 UDP 快照或用户手动刷新。
    registerGlobalShortcuts();
    // =====wjy====
    connect(&platform::DeviceListSyncService::instance(), &platform::DeviceListSyncService::snapshotAvailable,
        this, &DeviceGrid::applySyncedDeviceSnapshot); // wjy: 后台只处理共享文件，所有 UI 和全局设备数组修改统一回到主线程。
    g_deviceCatalogRepository.startSynchronization(
        QFileInfo(deviceStorePath()).absolutePath(),
        [](const QString& dataDirectory, const QJsonObject& localSnapshot) {
            platform::DeviceListSyncService::instance().start(dataDirectory, localSnapshot); // wjy: 具体同步服务通过窄回调注入，仓储仍保持可独立测试。
        },
        [] {
            platform::DeviceListSyncService::instance().stop(); // wjy: 仓储拥有生命周期决定权，回调只负责执行现有服务的停止动作。
        },
        [](const QJsonObject& localSnapshot) {
            platform::DeviceListSyncService::instance().submitLocalSnapshot(localSnapshot); // wjy: 本地原子保存成功后由仓储触发 pending 提交。
        }); // wjy: UI 完成初始化后由仓储启动同步，目录快照和提交抑制状态不再泄漏到 DeviceGrid。
    // ===end====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] DeviceGrid ctor end")); // wjy: DeviceGrid 构造函数正常结束。
}

// =====wjy====
DeviceGrid::~DeviceGrid()
{
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] DeviceGrid dtor begin")); // wjy: 记录 DeviceGrid 开始析构，和后台状态刷新线程日志对照判断关闭时序。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] dtor begin statusRefreshInProgress=%1 wakeProbeInProgress=%2")
        .arg(m_statusRefreshInProgress)
        .arg(m_wakeProbeInProgress)); // wjy: 关闭时输出后台刷新标志到日志文件，判断是否存在“界面销毁但线程仍在跑”的情况。
    // =====wjy====
    prepareForApplicationExit();
    m_shuttingDown = true; // wjy: 析构开始后禁止再登记新的后台任务，避免关闭过程中继续投递 UI 回调。
    std::vector<BackgroundThreadEntry> backgroundThreads; // wjy: 先把仍在列表中的活动或最后完成任务搬到局部变量，避免 join 时长时间持锁。
    {
        std::lock_guard lock(m_backgroundThreadsMutex); // wjy: 和 runBackgroundTask 的登记操作互斥，保证不会边遍历边修改 vector。
        backgroundThreads.swap(m_backgroundThreads); // wjy: 本次析构负责等待当前已经启动的后台线程。
    }
    for (BackgroundThreadEntry& entry : backgroundThreads) {
        cancelThreadSynchronousIo(entry.thread, QStringLiteral("destructor-before-join")); // wjy: 先批量唤醒已阻塞在 UNC/SMB 文件访问中的线程，让多个任务可以同时开始退出。
    }
    for (BackgroundThreadEntry& entry : backgroundThreads) {
        if (entry.thread.joinable()) {
            cancelThreadSynchronousIo(entry.thread, QStringLiteral("destructor-join-current")); // wjy: 每次 join 前再次取消，覆盖线程列表搬移后才进入同步网盘调用的竞态窗口。
            writeDeviceGridStartupLog(QStringLiteral("[wjy-exit] background thread join begin")); // wjy: 最后一条停在这里可确认进程卡在后台任务汇合。
            entry.thread.join(); // wjy: 等状态刷新/唤醒检测等线程结束，防止 Qt 对象销毁后仍访问 UI 或 Qt 资源。
            writeDeviceGridStartupLog(QStringLiteral("[wjy-exit] background thread join end"));
        }
    }
    // ===end====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] DeviceGrid dtor end")); // wjy: 线程已经等待完成，DeviceGrid 可以安全继续析构。
}
// ===end====

// =====wjy====
void DeviceGrid::applySyncedDeviceSnapshot(const QJsonObject& snapshot)
{
    if (m_shuttingDown || snapshot.isEmpty()) {
        return;
    }

    g_deviceCatalog.normalizeState();
    const QJsonObject currentSnapshot = g_deviceCatalogRepository.snapshot(false); // wjy: 比较前统一使用目录规范化快照，避免旧兼容字段造成假变化。
    const bool payloadChanged = !platform::deviceListSnapshotsEquivalent(currentSnapshot, snapshot);
    QString selectedDeviceId;
    if (m_selectedDeviceIndex >= 0 && m_selectedDeviceIndex < g_devices.size()) {
        selectedDeviceId = g_devices.at(m_selectedDeviceIndex).id;
    }
    QSet<QString> selectedDeviceIds;
    for (const int index : std::as_const(m_selectedDeviceIndexes)) {
        if (index >= 0 && index < g_devices.size()) {
            selectedDeviceIds.insert(g_devices.at(index).id);
        }
    }
    saveCurrentScriptUiState(); // wjy: 快照可能删除或移动当前设备，替换数组前先保存它自己的脚本 UI 状态。

    g_deviceCatalogRepository.applySynchronizedSnapshot(snapshot); // wjy: 仓储一次完成目录应用和本机落盘，并在内部阻止远端快照形成递归 pending。
    updateRealtimeConfiguredDevices(); // wjy: 同步快照可能新增、删除或修改 IP，立即重建广播来源白名单。

    m_selectedDeviceIndexes.clear();
    m_selectedDeviceIndex = -1;
    for (int i = 0; i < g_devices.size(); ++i) {
        const QString id = g_devices.at(i).id;
        if (selectedDeviceIds.contains(id)) {
            m_selectedDeviceIndexes.insert(i);
        }
        if (!selectedDeviceId.isEmpty() && id == selectedDeviceId) {
            m_selectedDeviceIndex = i; // wjy: 数组顺序同步变化后按 UUID 恢复选择，不能沿用旧下标控制另一台设备。
        }
    }
    if (m_selectedDeviceIndex < 0 && !m_selectedDeviceIndexes.isEmpty()) {
        m_selectedDeviceIndex = *std::min_element(m_selectedDeviceIndexes.cbegin(), m_selectedDeviceIndexes.cend());
    }
    if (m_selectedDeviceIndex < 0) {
        const int unhiddenFallbackIndex = firstUnhiddenDeviceIndex();
        if (unhiddenFallbackIndex >= 0) {
            m_selectedDeviceIndex = unhiddenFallbackIndex;
            m_selectedDeviceIndexes.insert(unhiddenFallbackIndex); // wjy: 同步快照重新加入本机时，隐藏开关仍只回退到未隐藏设备。
        }
    }

    pruneHiddenDeviceSelections(); // wjy: 共享快照可能重新带回本机记录，本机隐藏偏好必须在每次同步后重新应用。

    if (m_selectedDeviceIndex >= 0 && m_selectedDeviceIndex < g_devices.size()) {
        m_selectionAnchorDeviceIndex = m_selectedDeviceIndex;
        m_previousDeviceIndex = m_selectedDeviceIndex;
        m_currentDeviceName = deviceDisplayName(g_devices.at(m_selectedDeviceIndex));
        m_previousDeviceName = m_currentDeviceName;
        loadScriptUiStateForDevice(currentScriptUiDeviceIp());
    } else {
        m_selectionAnchorDeviceIndex = -1;
        m_currentDeviceName.clear();
        m_previousDeviceName.clear();
        loadScriptUiStateForDevice(QString());
    }

    m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset, maxDeviceListScrollOffset());
    updateSettingsControls();
    updateAddDeviceControls();
    updateLocalInfoControls();
    update();
    Q_UNUSED(payloadChanged); // wjy: 设备集合变化后等待实时广播或用户手动刷新，不再自动发起全设备 TCP 轮询。
}
// ===end====

// =====wjy====
void DeviceGrid::updateRealtimeConfiguredDevices()
{
    if (!m_realtimeStateService) {
        return;
    }
    QSet<QString> configuredIps;
    for (const DeviceEntry& device : std::as_const(g_devices)) {
        if (deviceHiddenByLocalPreference(device)) {
            continue; // wjy: 隐藏本机时不接收它的实时状态，避免不可见记录继续积累会话和更新缓存。
        }
        const QString ip = device.ip.trimmed();
        if (!ip.isEmpty()) {
            configuredIps.insert(ip);
        }
    }
    for (auto it = m_deviceRealtimeUpdateStates.begin(); it != m_deviceRealtimeUpdateStates.end();) {
        if (!configuredIps.contains(it.key())) {
            it = m_deviceRealtimeUpdateStates.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_deviceUpdateAvailability.begin(); it != m_deviceUpdateAvailability.end();) {
        if (!configuredIps.contains(it.key())) {
            it = m_deviceUpdateAvailability.erase(it);
        } else {
            ++it;
        }
    }
    m_realtimeStateService->setConfiguredDeviceIps(configuredIps); // wjy: 来源过滤与设备列表共用一份 IP 集合，未知广播不会写入任何 UI 缓存。
}

void DeviceGrid::applyRealtimeDeviceState(
    const QString& deviceIp,
    const platform::DeviceRealtimeReducedState& state)
{
    const QString ip = deviceIp.trimmed();
    if (m_shuttingDown || ip.isEmpty() || deviceIndexForIp(ip) < 0) {
        return;
    }

    m_deviceStatuses.insert(ip, state.presence);
    m_deviceRemoteSessionCounts.insert(ip, qBound(0, state.remoteSessionCount, 10)); // wjy: 徽标人数只接收目标主机快照里的会话数。
    m_deviceRemoteControllerNames.insert(ip, state.remoteControllerLabels.join(QLatin1Char(',')));
    m_deviceRealtimeScriptStates.insert(ip, state.script.state);
    if (!state.update.installedVersion.trimmed().isEmpty()) {
        m_deviceRealtimeUpdateStates.insert(ip, state.update);
        setRemoteUpdateAvailability(ip, realtimeUpdateAvailable(state.update));
    } else if (state.presence == platform::DevicePresenceState::Offline) {
        m_deviceRealtimeUpdateStates.remove(ip);
        setRemoteUpdateAvailability(ip, false);
    }

    if (state.script.state != platform::RealtimeScriptState::Unknown) {
        platform::RemoteScriptRuntimeInfo runtime;
        runtime.supported = true;
        runtime.statusKnown = true;
        runtime.running = state.script.state == platform::RealtimeScriptState::Running;
        runtime.runId = state.script.runId;
        runtime.workName = state.script.workName;
        runtime.scriptName = state.script.scriptName;
        runtime.controllerPid = state.script.controllerPid;
        runtime.startedAtEpochMs = state.script.startedAtEpochMs;
        applyRemoteScriptRuntimeState(ip, state.loginUser, runtime); // wjy: Running/Idle 使用目标本机验证结果恢复或结束脚本；Unknown 只隐藏图标并保留可恢复元数据。
    }

    if (state.presence == platform::DevicePresenceState::Online
        || state.presence == platform::DevicePresenceState::Busy) {
        m_poweringOnDeviceIps.remove(ip);
        m_poweringOnStartedAtMs.remove(ip); // wjy: 实时心跳到达即确认开机完成，不等待两秒 TCP 唤醒探测。
    }
    update(deviceListViewportRect(m_deviceGroupExpanded));
}

void DeviceGrid::publishRemoteControllerTarget(
    RemoteDesktopWindow* window,
    const QString& deviceName,
    const QString& deviceIp)
{
    if (!window || !m_realtimeStateService) {
        return;
    }
    const QString sessionId = m_realtimeStateService->publishControllerTarget(deviceIp, deviceName);
    if (!sessionId.isEmpty()) {
        m_realtimeControllerTargetSessionIds.insert(window, sessionId); // wjy: 本机窗口与广播租约一一对应，同一目标的多个窗口也不会互相覆盖。
    }
}

void DeviceGrid::removeRemoteControllerTarget(RemoteDesktopWindow* window)
{
    if (!window) {
        return;
    }
    const QString sessionId = m_realtimeControllerTargetSessionIds.take(window);
    if (m_realtimeStateService && !sessionId.isEmpty()) {
        m_realtimeStateService->removeControllerTarget(sessionId);
    }
}
// ===end====

void DeviceGrid::prepareForApplicationExit()
{
    writeDeviceGridStartupLog(QStringLiteral("[wjy-exit] DeviceGrid prepareForApplicationExit begin"));
    m_shuttingDown = true; // wjy: 更新退出准备一开始就阻止新增后台任务，避免退出过程中又启动状态探测或 SSH 操作。
    cancelBlockingBackgroundIo(); // wjy: 在其它退出清理开始前先取消不可达网盘造成的同步 I/O 等待，使后台任务尽快回到可汇合状态。
    // =====wjy====
    cancelScreenEdgeAutoHide(); // wjy: 退出时停止所有边缘的鼠标监视、收起延迟和窗口位置动画，不再修改正在销毁的顶层窗口。
    if (m_localSystemInfoTimer) {
        m_localSystemInfoTimer->stop(); // wjy: 退出准备阶段终止 CPU、GPU、内存统一采样，不再投递资源环重绘。
        m_localCpuUsageSampler.reset();
        m_localGpuUsageSampler.reset(); // wjy: 同步清除 CPU 时间基线并关闭 GPU PDH 查询句柄。
        m_localCpuUsagePercent = -1.0;
        m_localGpuUsagePercent = -1.0;
        m_localMemoryUsagePercent = -1.0;
        m_localMemoryUsage = {}; // wjy: 退出准备同时丢弃已用/可用容量，销毁期间不会再绘制陈旧内存数据。
    }
    if (m_localCpuUsageAnimation) {
        m_localCpuUsageAnimation->stop();
    }
    if (m_localGpuUsageAnimation) {
        m_localGpuUsageAnimation->stop();
    }
    if (m_localMemoryUsageAnimation) {
        m_localMemoryUsageAnimation->stop(); // wjy: 退出时同步终止三枚资源圆环动画，避免采样已停但动画仍请求重绘。
    }
    if (m_remoteQualityTimer) {
        m_remoteQualityTimer->stop(); // wjy: 退出期间停止质量采样，不再向已经提交stop的窗口发送任何新协议请求。
    }
    if (m_titlebarBandwidthTimer) {
        m_titlebarBandwidthTimer->stop(); // wjy: 退出准备阶段停止标题栏网卡枚举，不再投递一秒重绘。
        m_titlebarBandwidthMonitor.reset();
        m_titlebarBandwidthSample = {}; // wjy: 清空系统计数快照，析构期间 paintEvent 不会继续显示陈旧余量。
    }
    m_remoteQualityEvaluationQueued = false;
    if (m_remoteViewerLifecycleManager) {
        m_remoteViewerLifecycleManager->beginApplicationShutdown(); // wjy: 退出第一步关闭Viewer启动准入，排队中的第5台以后设备不会在清理途中又开始连接。
    }
    g_deviceCatalogRepository.stopSynchronization(); // wjy: 同步生命周期归仓储管理，退出时统一停止轮询并等待共享任务汇合。
    platform::PortableOpenSshManager::instance().stopClientProcesses(); // wjy: 用户从托盘或主窗口选择退出时，立即关闭本程序打开的 cmd/ssh 交互终端。
    if (m_periodicDeviceDiscoveryTimer) {
        m_periodicDeviceDiscoveryTimer->stop(); // wjy: 退出准备开始后禁止定时器再启动新的网段扫描任务。
    }
    if (m_wallpaperRotationTimer) {
        m_wallpaperRotationTimer->stop(); // wjy: 退出阶段停止壁纸定时器，避免再向后台线程登记共享目录任务。
    }
    // ===end====
    if (m_scriptCancelRequested) {
        m_scriptCancelRequested->store(true); // wjy: 当前可见的无限期脚本 SSH 会话收到取消信号后会杀掉本地 ssh.exe 并结束后台线程。
    }
    m_scriptUiStateStore.requestCancelAll(); // wjy: 脚本仓储统一取消所有设备任务，DeviceGrid 不再遍历内部容器。
    unregisterGlobalShortcuts();

    // =====wjy====
    const QVector<QPointer<RemoteDesktopWindow>> windows =
        m_remoteWindowCoordinator->shutdownWindows(); // wjy: 协调器统一去重普通、平铺和激活顺序引用，退出时每个窗口只处理一次。
    // ===end====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-exit] remote windows count=%1").arg(windows.size()));
    for (const QPointer<RemoteDesktopWindow>& window : windows) {
        RemoteDesktopWindow* remoteWindow = window.data();
        if (!remoteWindow) {
            continue;
        }
        writeDeviceGridStartupLog(QStringLiteral("[wjy-exit] remote window shutdown begin ptr=%1")
            .arg(reinterpret_cast<quintptr>(remoteWindow))); // wjy: 此阶段只取消回调并提交stop，不再逐窗口同步阻塞Qt线程。
        remoteWindow->setAttribute(Qt::WA_QuitOnClose, false);
        remoteWindow->shutdownForApplicationExit();
        writeDeviceGridStartupLog(QStringLiteral("[wjy-exit] remote window shutdown end ptr=%1")
            .arg(reinterpret_cast<quintptr>(remoteWindow)));
    }

    // =====wjy====
    if (m_remoteViewerLifecycleManager) {
        writeDeviceGridStartupLog(QStringLiteral("[wjy-exit] viewer lifecycle join begin"));
        m_remoteViewerLifecycleManager->shutdownAndWait(); // wjy: 全部窗口已提交stop后再封闭队列，等待原生回调、更新查询和stop任务完全退出。
        writeDeviceGridStartupLog(QStringLiteral("[wjy-exit] viewer lifecycle join end"));
    }
    for (const QPointer<RemoteDesktopWindow>& window : windows) {
        if (RemoteDesktopWindow* remoteWindow = window.data()) {
            delete remoteWindow; // wjy: join完成后才销毁Presenter和Qt对象，后台任务不可能再访问已释放窗口。
            writeDeviceGridStartupLog(QStringLiteral("[wjy-exit] remote window deleted"));
        }
    }
    // ===end====

    m_remoteWindowCoordinator->clear(); // wjy: 所有窗口删除后清空协调器索引，避免退出阶段残留对象地址。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-exit] DeviceGrid prepareForApplicationExit end"));
}

void DeviceGrid::registerGlobalShortcuts()
{
#if defined(Q_OS_WIN)
    unregisterGlobalShortcuts();

    HWND windowHandle = reinterpret_cast<HWND>(winId());
    if (!windowHandle) {
        return;
    }
    m_globalShortcutWindowHandle = reinterpret_cast<quintptr>(windowHandle);

    for (int i = 0; i < kRemoteShortcutCount; ++i) {
        UINT modifiers = 0;
        UINT virtualKey = 0;
        const QKeySequence shortcut = remoteShortcutForIndex(i);
        if (!windowsHotkeyFromShortcut(shortcut, &modifiers, &virtualKey)) {
            qWarning().noquote() << QStringLiteral("[global-hotkey] unsupported shortcut index=%1 value=%2")
                .arg(i)
                .arg(shortcut.toString(QKeySequence::NativeText));
            continue;
        }

        const int id = globalShortcutIdForIndex(i);
        if (RegisterHotKey(windowHandle, id, modifiers, virtualKey)) {
            m_registeredGlobalShortcutIds.insert(id);
        } else {
            qWarning().noquote() << QStringLiteral("[global-hotkey] register failed index=%1 value=%2 error=%3")
                .arg(i)
                .arg(shortcut.toString(QKeySequence::NativeText))
                .arg(static_cast<qulonglong>(GetLastError()));
        }
    }
#endif
}

void DeviceGrid::unregisterGlobalShortcuts()
{
#if defined(Q_OS_WIN)
    if (m_registeredGlobalShortcutIds.isEmpty()) {
        m_globalShortcutWindowHandle = 0;
        return;
    }

    HWND windowHandle = reinterpret_cast<HWND>(m_globalShortcutWindowHandle);
    if (windowHandle) {
        for (int id : m_registeredGlobalShortcutIds) {
            UnregisterHotKey(windowHandle, id);
        }
    }
    m_registeredGlobalShortcutIds.clear();
    m_globalShortcutWindowHandle = 0;
#endif
}

void DeviceGrid::triggerShortcutAction(int shortcutIndex)
{
    releaseRemoteShortcutKeyState(shortcutIndex);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (shortcutIndex == m_lastShortcutActionIndex && nowMs - m_lastShortcutActionAtMs < 120) {
        return;
    }
    m_lastShortcutActionIndex = shortcutIndex;
    m_lastShortcutActionAtMs = nowMs;

    switch (shortcutIndex) {
    case 0:
        toggleTopmostRemoteWindowFullscreen();
        break;
    case 1:
        toggleRemoteWindowTiling();
        break;
    case 2:
        closeTopmostRemoteWindow();
        break;
    case 3:
        closeAllRemoteWindows();
        break;
    case 4: {
        // =====wjy====
        // wjy: Ctrl+B 默认切换剪切板同步；作用于当前最上方远控窗口，并写回全局默认。
        const QVector<QPointer<RemoteDesktopWindow>> windows = openedRemoteWindows();
        RemoteDesktopWindow* target = nullptr;
        for (const QPointer<RemoteDesktopWindow>& window : windows) {
            if (window && !window->isClosingConnection()) {
                target = window.data();
                break;
            }
        }
        const bool enabled = !(target ? target->isClipboardSyncEnabled() : platform::AppSettings::remoteClipboardSyncEnabled());
        platform::AppSettings::setRemoteClipboardSyncEnabled(enabled);
        for (const QPointer<RemoteDesktopWindow>& window : windows) {
            if (window && !window->isClosingConnection()) {
                window->setClipboardSyncEnabled(enabled);
            }
        }
        // ===end====
        break;
    }
    default:
        break;
    }
}

void DeviceGrid::releaseRemoteShortcutKeyState(int shortcutIndex)
{
#if defined(Q_OS_WIN)
    const QVector<int> shortcutKeys = virtualKeysForShortcut(remoteShortcutForIndex(shortcutIndex));
    if (shortcutKeys.isEmpty()) {
        return;
    }
    const QVector<QPointer<RemoteDesktopWindow>> windows = openedRemoteWindows();
    for (const QPointer<RemoteDesktopWindow>& window : windows) {
        if (window && !window->isClosingConnection()) {
            window->releaseForwardedShortcutKeys(shortcutKeys);
        }
    }
#else
    Q_UNUSED(shortcutIndex)
#endif
}

void DeviceGrid::syncResponsiveLayoutState() const
{
    // =====wjy====
    const bool detailWidthAnimating = m_detailPanelWidthAnimation
        && m_detailPanelWidthAnimation->state() == QAbstractAnimation::Running; // wjy: 动画帧使用窗口真实宽度，标题栏按钮会随右边界移动而不是在 720px 处停住。
    setResponsiveLayoutState(size(), m_detailPanelCollapsed || detailWidthAnimating);
    // ===end====
}

// =====wjy====
void DeviceGrid::setDetailPanelCollapsed(bool collapsed)
{
    if (m_detailPanelWidthAnimation
        && m_detailPanelWidthAnimation->state() == QAbstractAnimation::Running) {
        return; // wjy: 220ms 滑动期间忽略重复点击，防止两个相反目标同时修改顶层窗口几何。
    }
    if (m_detailPanelCollapsed == collapsed) {
        return;
    }

    QWidget* shell = window();
    if (collapsed && shell) {
        m_expandedWindowWidth = qMax(kMinimumExpandedShellWidth, shell->width()); // wjy: 只保存展开宽度，紧凑窗口移动后再次展开不会跳回旧位置。
    }
    m_detailPanelAnimationTargetCollapsed = collapsed; // wjy: 收起过程暂不改变可见状态，让详情内容跟随右边界被逐步裁掉。

    if (collapsed) {
        if (m_settingsScrollbarAnimation) {
            m_settingsScrollbarAnimation->stop(); // wjy: 隐藏设置页时终止滚动缓动，避免不可见内容继续改变位置。
        }
        m_draggingSettingsScrollbar = false;
    }

    if (!shell || !shell->isVisible()) {
        finishDetailPanelCollapseTransition(); // wjy: 窗口尚未显示或已隐藏到托盘时无需播放不可见动画，直接应用最终状态。
        return;
    }

    if (!m_detailPanelWidthAnimation) {
        m_detailPanelWidthAnimation = new QVariantAnimation(this);
        m_detailPanelWidthAnimation->setDuration(220); // wjy: 220ms 足够看清折叠方向，同时不会让主窗口操作显得拖沓。
        m_detailPanelWidthAnimation->setEasingCurve(QEasingCurve::OutCubic); // wjy: 前段快速移动、末端柔和减速，边界停止时没有生硬撞击感。
        connect(m_detailPanelWidthAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            QWidget* animatedShell = window();
            if (!animatedShell) {
                return;
            }
            QRect animatedGeometry = animatedShell->geometry();
            animatedGeometry.setWidth(value.toInt()); // wjy: 固定左上角和高度，只推动右侧边界，实现详情栏横向滑入滑出。
            animatedShell->setGeometry(animatedGeometry);
        });
        connect(m_detailPanelWidthAnimation, &QVariantAnimation::finished,
            this, &DeviceGrid::finishDetailPanelCollapseTransition); // wjy: 最后一帧再隐藏详情或恢复展开约束，消除内容先闪没再缩窗的问题。
    }

    setMinimumSize(kCollapsedShellWidth, kMinimumShellHeight); // wjy: 动画期间临时允许中央控件经过 720px 以下宽度，否则 Qt 最小尺寸会阻止收起。
    shell->setMinimumSize(kCollapsedShellWidth, kMinimumShellHeight);
    shell->setMaximumWidth(QWIDGETSIZE_MAX); // wjy: 收起和展开动画都必须暂时解除固定宽度，逐帧 setGeometry 才能生效。

    if (!collapsed) {
        m_detailPanelCollapsed = false; // wjy: 展开第一帧就恢复详情绘制，后续随着窗口变宽从右侧逐步露出内容。
        syncResponsiveLayoutState();
        updateAddDeviceControls();
        updateLocalInfoControls();
        updateSettingsControls();
        updateScriptFileEditorControls();
        update();
    }

    const int startWidth = shell->width();
    const int targetWidth = collapsed
        ? kCollapsedShellWidth
        : qMax(kMinimumExpandedShellWidth, m_expandedWindowWidth); // wjy: 展开恢复收起前宽度，收起固定到设备栏与按钮窄条之和。
    if (startWidth == targetWidth) {
        finishDetailPanelCollapseTransition();
        return;
    }

    m_detailPanelWidthAnimation->setStartValue(startWidth);
    m_detailPanelWidthAnimation->setEndValue(targetWidth);
    m_detailPanelWidthAnimation->start(); // wjy: QVariantAnimation 每帧驱动主窗口宽度，替代原来一次 setGeometry 的闪现。
}

void DeviceGrid::finishDetailPanelCollapseTransition()
{
    const bool collapsed = m_detailPanelAnimationTargetCollapsed;
    QWidget* shell = window();
    m_detailPanelCollapsed = collapsed; // wjy: 收起在最后一帧才隐藏详情；展开状态已在起始帧设置，这里再次确认最终值。

    if (collapsed) {
        setMinimumSize(kCollapsedShellWidth, kMinimumShellHeight);
        if (shell) {
            shell->setMinimumSize(kCollapsedShellWidth, kMinimumShellHeight);
            shell->setMaximumWidth(kCollapsedShellWidth); // wjy: 动画完成后紧凑窗口重新锁定横向宽度，只保留高度调整能力。
            QRect compactGeometry = shell->geometry();
            compactGeometry.setWidth(kCollapsedShellWidth);
            shell->setGeometry(compactGeometry);
        }
    } else {
        if (shell) {
            shell->setMaximumWidth(QWIDGETSIZE_MAX); // wjy: 展开结束后恢复用户横向缩放窗口的能力。
        }
        setMinimumSize(kMinimumExpandedShellWidth, kMinimumShellHeight);
        if (shell) {
            shell->setMinimumSize(kMinimumExpandedShellWidth, kMinimumShellHeight);
            QRect expandedGeometry = shell->geometry();
            expandedGeometry.setWidth(qMax(kMinimumExpandedShellWidth, m_expandedWindowWidth));
            shell->setGeometry(expandedGeometry); // wjy: 用精确目标宽度收尾，避免浮点插值取整留下 1px 边缘误差。
        }
    }

    syncResponsiveLayoutState();
    setDesktopHoverActive(false);
    clearBottomActionHover();
    updateAddDeviceControls();
    updateLocalInfoControls();
    updateSettingsControls();
    updateScriptFileEditorControls(); // wjy: 动画结束后统一隐藏或恢复真实控件，并同步本机资源采样生命周期。
    if (!collapsed
        && !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && m_deviceDetailTab == DeviceDetailTab::Local) {
        refreshLocalSystemInfoTab(); // wjy: 详情完全展开后再重建 CPU/GPU 基线和内存快照，避免动画期间重复刷新。
    }
    update();
}
// ===end====

// =====wjy====
void DeviceGrid::ensureScreenEdgeAutoHideControllers()
{
    if (!m_screenEdgeAutoHideAnimation) {
        m_screenEdgeAutoHideAnimation = new QVariantAnimation(this);
        m_screenEdgeAutoHideAnimation->setDuration(180); // wjy: 三种边缘共用短促动画，能看清收起方向但不会拖慢窗口操作。
        m_screenEdgeAutoHideAnimation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_screenEdgeAutoHideAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            QWidget* shell = window();
            if (m_screenEdgeDock == ScreenEdgeDock::None || !shell) {
                return;
            }
            shell->move(value.toPoint()); // wjy: QPoint 插值让顶部只改变 y、左右只改变 x，并保留另一轴的当前位置。
        });
        connect(m_screenEdgeAutoHideAnimation, &QVariantAnimation::finished, this, [this] {
            QWidget* shell = window();
            if (m_screenEdgeDock == ScreenEdgeDock::None || !shell || !m_screenEdgeDockScreenGeometry.isValid()) {
                return;
            }
            shell->move(screenEdgeAutoHideTargetPosition(m_screenEdgeAutoHidden)); // wjy: 动画结束按当前方向重新计算终点，消除 QPoint 插值取整残留。
        });
    }

    if (!m_screenEdgeAutoHideMonitorTimer) {
        m_screenEdgeAutoHideMonitorTimer = new QTimer(this);
        m_screenEdgeAutoHideMonitorTimer->setTimerType(Qt::CoarseTimer);
        m_screenEdgeAutoHideMonitorTimer->setInterval(50); // wjy: 20Hz 足够及时响应任一边缘悬停，同时避免高频全局鼠标查询。
        connect(m_screenEdgeAutoHideMonitorTimer, &QTimer::timeout,
            this, &DeviceGrid::monitorScreenEdgeAutoHide);
    }

    if (!m_screenEdgeAutoHideDelayTimer) {
        m_screenEdgeAutoHideDelayTimer = new QTimer(this);
        m_screenEdgeAutoHideDelayTimer->setSingleShot(true);
        m_screenEdgeAutoHideDelayTimer->setInterval(650); // wjy: 鼠标离开约三分之二秒后才收起，操作标题栏和窗口边缘时不会误触发。
        connect(m_screenEdgeAutoHideDelayTimer, &QTimer::timeout, this, [this] {
            QWidget* shell = window();
            if (m_screenEdgeDock == ScreenEdgeDock::None || m_screenEdgeAutoHidden || m_draggingWindow || m_resizingWindow
                || !shell || !shell->isVisible() || shell->isMinimized()
                || QApplication::activePopupWidget() || QApplication::activeModalWidget()) {
                return;
            }
            if (shell->frameGeometry().adjusted(-2, -2, 2, 2).contains(QCursor::pos())) {
                return; // wjy: 延迟期间鼠标重新进入窗口则保持展开，下一次真正离开后由监视器重新计时。
            }
            setScreenEdgeAutoHidden(true); // wjy: 延迟确认鼠标仍在窗口外后，沿已记录的顶部、左侧或右侧方向收起。
        });
    }
}

void DeviceGrid::updateScreenEdgeAutoHideAfterWindowDrag(const QPoint& releaseGlobalPosition)
{
    QWidget* shell = window();
    if (!shell || shell->isMaximized()) {
        cancelScreenEdgeAutoHide();
        return;
    }

    QScreen* targetScreen = QGuiApplication::screenAt(releaseGlobalPosition);
    if (!targetScreen) {
        targetScreen = shell->screen();
    }
    if (!targetScreen) {
        targetScreen = QGuiApplication::primaryScreen();
    }
    if (!targetScreen) {
        cancelScreenEdgeAutoHide();
        return;
    }

    const QRect dockGeometry = targetScreen->availableGeometry(); // wjy: 三种方向都使用目标屏幕工作区，任务栏位于顶部或左右侧时不会把触发条藏到任务栏后面。
    const QRect frameGeometry = shell->frameGeometry();
    const int topOffset = frameGeometry.top() - dockGeometry.top();
    const int leftOffset = frameGeometry.left() - dockGeometry.left();
    const int rightOffset = dockGeometry.right() - frameGeometry.right();
    const bool touchesTopEdge = topOffset <= kScreenEdgeDockSnapDistance
        || qAbs(releaseGlobalPosition.y() - dockGeometry.top()) <= kScreenEdgeDockSnapDistance; // wjy: 窗口上沿贴近、触及或越过工作区顶部都吸附，不再限制最多只能越界一个标题栏高度。
    const bool touchesLeftEdge = leftOffset <= kScreenEdgeDockSnapDistance
        || qAbs(releaseGlobalPosition.x() - dockGeometry.left()) <= kScreenEdgeDockSnapDistance; // wjy: 窗口左沿即使已经拖出屏幕较远也会回吸到左侧，鼠标贴边仍保留快捷触发。
    const bool touchesRightEdge = rightOffset <= kScreenEdgeDockSnapDistance
        || qAbs(releaseGlobalPosition.x() - dockGeometry.right()) <= kScreenEdgeDockSnapDistance; // wjy: 窗口右沿超过工作区右侧后同样触发，松开时统一校正回精确右边缘。
    const ScreenEdgeDock dockEdge = touchesTopEdge
        ? ScreenEdgeDock::Top // wjy: 左上或右上角同时命中时优先保持原有顶部行为，避免既有操作习惯变化。
        : touchesLeftEdge
            ? ScreenEdgeDock::Left
            : touchesRightEdge
                ? ScreenEdgeDock::Right
                : ScreenEdgeDock::None;
    if (dockEdge == ScreenEdgeDock::None) {
        cancelScreenEdgeAutoHide();
        return;
    }

    ensureScreenEdgeAutoHideControllers();
    if (m_screenEdgeAutoHideAnimation) {
        m_screenEdgeAutoHideAnimation->stop();
    }
    if (m_screenEdgeAutoHideDelayTimer) {
        m_screenEdgeAutoHideDelayTimer->stop();
    }
    m_screenEdgeDockScreenGeometry = dockGeometry;
    m_screenEdgeDock = dockEdge;
    m_screenEdgeAutoHidden = false;
    QRect snappedGeometry = shell->geometry();
    switch (dockEdge) {
    case ScreenEdgeDock::Top:
        snappedGeometry.moveTop(dockGeometry.top()); // wjy: 顶部停靠保持横向位置，仅校正窗口上沿。
        break;
    case ScreenEdgeDock::Left:
        snappedGeometry.moveLeft(dockGeometry.left()); // wjy: 左侧停靠保持纵向位置，仅校正窗口左沿。
        break;
    case ScreenEdgeDock::Right:
        snappedGeometry.moveRight(dockGeometry.right()); // wjy: 右侧停靠按 QRect 包含式右坐标精确贴合工作区右沿。
        break;
    case ScreenEdgeDock::None:
        break;
    }
    shell->setGeometry(snappedGeometry); // wjy: 松开后先精确吸附所选边缘；鼠标真正离开窗口后再开始延迟收起。
    if (m_screenEdgeAutoHideMonitorTimer && !m_screenEdgeAutoHideMonitorTimer->isActive()) {
        m_screenEdgeAutoHideMonitorTimer->start();
    }
}

QPoint DeviceGrid::screenEdgeAutoHideTargetPosition(bool hidden) const
{
    QWidget* shell = window();
    if (m_screenEdgeDock == ScreenEdgeDock::None || !shell || !m_screenEdgeDockScreenGeometry.isValid()) {
        return shell ? shell->pos() : QPoint();
    }

    QPoint targetPosition = shell->pos(); // wjy: 默认保留与停靠方向垂直的坐标，顶部不改 x、左右不改 y。
    switch (m_screenEdgeDock) {
    case ScreenEdgeDock::Top:
        targetPosition.setY(hidden
                ? m_screenEdgeDockScreenGeometry.top() - shell->height() + kScreenEdgeHiddenStripThickness
                : m_screenEdgeDockScreenGeometry.top()); // wjy: 顶部隐藏向上移动，只在工作区内保留 4px 高触发条。
        break;
    case ScreenEdgeDock::Left:
        targetPosition.setX(hidden
                ? m_screenEdgeDockScreenGeometry.left() - shell->width() + kScreenEdgeHiddenStripThickness
                : m_screenEdgeDockScreenGeometry.left()); // wjy: 左侧隐藏向左移动，只在工作区内保留 4px 宽触发条。
        break;
    case ScreenEdgeDock::Right:
        targetPosition.setX(hidden
                ? m_screenEdgeDockScreenGeometry.right() - kScreenEdgeHiddenStripThickness + 1
                : m_screenEdgeDockScreenGeometry.right() - shell->width() + 1); // wjy: QRect 右边界为包含坐标，补 1 后展开和 4px 可见宽度才精确。
        break;
    case ScreenEdgeDock::None:
        break;
    }
    return targetPosition;
}

void DeviceGrid::setScreenEdgeAutoHidden(bool hidden)
{
    QWidget* shell = window();
    if (m_screenEdgeDock == ScreenEdgeDock::None || !shell || !m_screenEdgeDockScreenGeometry.isValid()) {
        return;
    }
    ensureScreenEdgeAutoHideControllers();

    if (m_screenEdgeAutoHideDelayTimer) {
        m_screenEdgeAutoHideDelayTimer->stop();
    }
    const QPoint targetPosition = screenEdgeAutoHideTargetPosition(hidden); // wjy: 终点由停靠枚举统一计算，动画层不再分别维护三套坐标公式。
    if (m_screenEdgeAutoHideAnimation
        && m_screenEdgeAutoHideAnimation->state() == QAbstractAnimation::Running) {
        if (m_screenEdgeAutoHidden == hidden) {
            return;
        }
        m_screenEdgeAutoHideAnimation->stop(); // wjy: 鼠标在滑动中反向进入或离开时，从当前 QPoint 平滑反向而不是瞬移。
    }

    m_screenEdgeAutoHidden = hidden;
    if (!hidden) {
        shell->show();
        shell->raise();
        shell->activateWindow(); // wjy: 被其它窗口覆盖时，鼠标到达任一边缘触发区仍能把主窗口完整带到前面。
    }
    if (shell->pos() == targetPosition || !shell->isVisible() || shell->isMinimized()) {
        shell->move(targetPosition);
        return;
    }

    m_screenEdgeAutoHideAnimation->setStartValue(shell->pos());
    m_screenEdgeAutoHideAnimation->setEndValue(targetPosition);
    m_screenEdgeAutoHideAnimation->start();
}

void DeviceGrid::monitorScreenEdgeAutoHide()
{
    QWidget* shell = window();
    if (m_screenEdgeDock == ScreenEdgeDock::None || m_shuttingDown || !shell || !m_screenEdgeDockScreenGeometry.isValid()) {
        if (m_screenEdgeAutoHideMonitorTimer) {
            m_screenEdgeAutoHideMonitorTimer->stop();
        }
        return;
    }
    if (!shell->isVisible() || shell->isMinimized()) {
        if (m_screenEdgeAutoHideDelayTimer) {
            m_screenEdgeAutoHideDelayTimer->stop();
        }
        return; // wjy: 隐藏到托盘或最小化期间保留停靠状态，但不继续移动不可见窗口。
    }

    const QPoint cursorPosition = QCursor::pos();
    QRect revealTrigger;
    switch (m_screenEdgeDock) {
    case ScreenEdgeDock::Top:
        revealTrigger = QRect(
            shell->x(),
            m_screenEdgeDockScreenGeometry.top(),
            shell->width(),
            kScreenEdgeRevealTriggerDepth); // wjy: 顶部触发区只覆盖本窗口横向范围，不让整块屏幕顶部都唤出主窗口。
        break;
    case ScreenEdgeDock::Left:
        revealTrigger = QRect(
            m_screenEdgeDockScreenGeometry.left(),
            shell->y(),
            kScreenEdgeRevealTriggerDepth,
            shell->height()); // wjy: 左侧触发区只覆盖窗口原有纵向范围，避免整条屏幕左边缘误触发。
        break;
    case ScreenEdgeDock::Right:
        revealTrigger = QRect(
            m_screenEdgeDockScreenGeometry.right() - kScreenEdgeRevealTriggerDepth + 1,
            shell->y(),
            kScreenEdgeRevealTriggerDepth,
            shell->height()); // wjy: 右侧触发区使用包含式右坐标，保证 6px 命中区域完整落在工作区内。
        break;
    case ScreenEdgeDock::None:
        return;
    }
    if (m_screenEdgeAutoHidden) {
        if (revealTrigger.contains(cursorPosition)) {
            setScreenEdgeAutoHidden(false); // wjy: 全局鼠标进入当前边缘触发区时，即使窗口主体在屏幕外也能沿原方向滑出。
        }
        return;
    }

    const bool interactionActive = m_draggingWindow
        || m_resizingWindow
        || QApplication::activePopupWidget()
        || QApplication::activeModalWidget(); // wjy: 下拉框、菜单、确认框和手动拖放期间始终保持窗口展开。
    const bool cursorInsideWindow = shell->frameGeometry().adjusted(-2, -2, 2, 2).contains(cursorPosition);
    if (interactionActive || cursorInsideWindow) {
        if (m_screenEdgeAutoHideDelayTimer) {
            m_screenEdgeAutoHideDelayTimer->stop();
        }
    } else if (m_screenEdgeAutoHideDelayTimer && !m_screenEdgeAutoHideDelayTimer->isActive()) {
        m_screenEdgeAutoHideDelayTimer->start(); // wjy: 真正离开主窗口后才开始 650ms 倒计时，三种停靠方向共用同一次复核。
    }
}

void DeviceGrid::cancelScreenEdgeAutoHide()
{
    if (m_screenEdgeAutoHideAnimation) {
        m_screenEdgeAutoHideAnimation->stop();
    }
    if (m_screenEdgeAutoHideMonitorTimer) {
        m_screenEdgeAutoHideMonitorTimer->stop();
    }
    if (m_screenEdgeAutoHideDelayTimer) {
        m_screenEdgeAutoHideDelayTimer->stop();
    }
    m_screenEdgeDock = ScreenEdgeDock::None;
    m_screenEdgeAutoHidden = false;
    m_screenEdgeDockScreenGeometry = QRect(); // wjy: 清除旧方向和屏幕工作区，下一次拖到任一显示器边缘都会重新计算。
}
// ===end====

void DeviceGrid::beginWindowResize(const QPoint& position, const QPoint& globalPosition)
{
    syncResponsiveLayoutState();
    m_resizeEdges = windowResizeEdgesAt(position, size(), !m_detailPanelCollapsed); // wjy: 紧凑态屏蔽左右边缘，只保留上下高度调整。
    if (m_resizeEdges == kResizeNone || !window()) {
        return;
    }
    cancelScreenEdgeAutoHide(); // wjy: 用户开始手动改变窗口几何时立即解除任一边缘停靠，后续不会被自动位置动画拉回。
    m_resizingWindow = true;
    m_resizeStartGlobal = globalPosition;
    m_resizeStartGeometry = window()->frameGeometry();
}

void DeviceGrid::updateWindowResize(const QPoint& globalPosition)
{
    if (!m_resizingWindow || !window()) {
        return;
    }

    const QPoint delta = globalPosition - m_resizeStartGlobal;
    QRect next = m_resizeStartGeometry;
    const QSize minSize = window()->minimumSize().expandedTo(minimumSize());

    if (m_resizeEdges & kResizeLeft) {
        next.setLeft(next.left() + delta.x());
        if (next.width() < minSize.width()) {
            next.setLeft(next.right() - minSize.width() + 1);
        }
    }
    if (m_resizeEdges & kResizeRight) {
        next.setRight(next.right() + delta.x());
        if (next.width() < minSize.width()) {
            next.setRight(next.left() + minSize.width() - 1);
        }
    }
    if (m_resizeEdges & kResizeTop) {
        next.setTop(next.top() + delta.y());
        if (next.height() < minSize.height()) {
            next.setTop(next.bottom() - minSize.height() + 1);
        }
    }
    if (m_resizeEdges & kResizeBottom) {
        next.setBottom(next.bottom() + delta.y());
        if (next.height() < minSize.height()) {
            next.setBottom(next.top() + minSize.height() - 1);
        }
    }

    window()->setGeometry(next);
}

void DeviceGrid::finishWindowResize()
{
    m_resizingWindow = false;
    m_resizeEdges = kResizeNone;
    syncResponsiveLayoutState();
    updateAddDeviceControls();
    updateLocalInfoControls();
    updateSettingsControls();
    updateScriptFileEditorControls();
}

// =====wjy====
void DeviceGrid::reapCompletedBackgroundThreads()
{
    std::vector<std::thread> completedThreads; // wjy: 已完成线程先移出共享列表，join 时不占用登记互斥锁。
    {
        std::lock_guard lock(m_backgroundThreadsMutex);
        for (auto entry = m_backgroundThreads.begin(); entry != m_backgroundThreads.end();) {
            const bool finished = entry->finished
                && entry->finished->load(std::memory_order_acquire); // wjy: acquire 与线程结束前的 release 配对，确认任务副作用已提交。
            if (!finished) {
                ++entry;
                continue;
            }
            completedThreads.emplace_back(std::move(entry->thread));
            entry = m_backgroundThreads.erase(entry); // wjy: 每次新任务到来都回收历史完成项，vector 和 Windows 线程句柄保持有界。
        }
    }

    for (std::thread& thread : completedThreads) {
        if (thread.joinable()) {
            thread.join(); // wjy: 完成标志已置位，join 仅负责释放系统句柄，通常立即返回且不影响界面流畅度。
        }
    }
}

void DeviceGrid::runBackgroundTask(std::function<void()> task)
{
    if (!task) {
        return; // wjy: 空任务没有运行意义，直接忽略。
    }

    reapCompletedBackgroundThreads(); // wjy: 新任务启动前清理旧任务资源，长期开机不会按任务次数线性积累线程句柄。
    std::lock_guard lock(m_backgroundThreadsMutex); // wjy: 后台线程登记和析构取走线程列表必须互斥。
    if (m_shuttingDown) {
        return; // wjy: 关闭阶段不再启动新线程，避免任务晚于 DeviceGrid 生命周期。
    }
    const auto finished = std::make_shared<std::atomic_bool>(false);
    m_backgroundThreads.emplace_back(); // wjy: 先完成 vector 扩容，再启动线程，避免登记分配失败时局部 joinable 线程触发 terminate。
    BackgroundThreadEntry& entry = m_backgroundThreads.back();
    entry.finished = finished;
    try {
        entry.thread = std::thread([task = std::move(task), finished]() mutable {
            struct CompletionMarker {
                std::shared_ptr<std::atomic_bool> flag;
                ~CompletionMarker() { flag->store(true, std::memory_order_release); } // wjy: 正常返回和所有异常路径都会发布完成状态。
            } completionMarker{finished};
            try {
                task(); // wjy: 所有 DeviceGrid 后台任务经同一入口执行，退出时可统一取消底层同步 I/O 并等待线程自然返回。
            } catch (const std::exception& exception) {
                try {
                    writeDeviceGridStartupLog(QStringLiteral("[wjy-background] unhandled std exception: %1")
                        .arg(QString::fromLocal8Bit(exception.what()))); // wjy: 后台异常只记录并结束当前任务，禁止越过 std::thread 入口触发 std::terminate。
                } catch (...) {
                }
            } catch (...) {
                try {
                    writeDeviceGridStartupLog(QStringLiteral("[wjy-background] unhandled unknown exception")); // wjy: 非标准异常同样在任务边界收口，避免偶发访问失败直接终止整个程序。
                } catch (...) {
                }
            }
        }); // wjy: 线程仍保持 joinable；完成后由后续任务渐进回收，退出时只等待尚未结束的少量任务。
    } catch (...) {
        m_backgroundThreads.pop_back(); // wjy: 系统拒绝创建线程时撤销空登记项，保持后台列表结构有效。
        throw;
    }
}

void DeviceGrid::cancelBlockingBackgroundIo()
{
    std::lock_guard lock(m_backgroundThreadsMutex); // wjy: 与任务登记和析构搬移互斥，保证取消期间 std::thread 对象及其原生句柄始终有效。
    for (BackgroundThreadEntry& entry : m_backgroundThreads) {
        cancelThreadSynchronousIo(entry.thread, QStringLiteral("prepare-for-exit")); // wjy: Windows 会让目标线程当前阻塞的同步 UNC/SMB 调用尽快失败返回，随后任务按原逻辑结束。
    }
}
// ===end====

QString DeviceGrid::currentScriptUiDeviceIp() const
{
// =====wjy====
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return QString(); // wjy: 没有当前设备时，脚本 UI 没有归属设备。
    }
    return g_devices.at(m_selectedDeviceIndex).ip.trimmed(); // wjy: 以设备 IP 作为脚本 UI 状态 key，避免同名设备切换时串状态。
// ===end====
}

// =====wjy====
void DeviceGrid::saveCurrentScriptUiState()
{
// =====wjy====
    const QString deviceIp = currentScriptUiDeviceIp();
    if (deviceIp.isEmpty()) {
        return;
    }

    ScriptUiState state = m_scriptUiStateStore.state(deviceIp); // wjy: 保留远端 runId/PID/确认状态，当前控件只覆盖 UI 展示字段。
    saveVisibleState(state, m_scriptFileEdit); // wjy: 直接更新目标设备状态，避免先构造临时快照再逐字段复制。
    m_scriptUiStateStore.setState(deviceIp, std::move(state));
// ===end====
}

void DeviceGrid::loadScriptUiStateForDevice(const QString& deviceIp)
{
// =====wjy====
    const QString key = deviceIp.trimmed();
    const ScriptUiState state = m_scriptUiStateStore.state(key);
    applyState(state, key, m_scriptFileEdit); // wjy: 脚本面板字段统一由控制器恢复，避免切换入口各自复制一套赋值逻辑。
    if (m_scriptOutputVisible && !m_scriptOutputFilePath.trimmed().isEmpty()) {
        m_scriptOutputText = stripTerminalControlSequences(readScriptOutputFileTail(m_scriptOutputFilePath)); // wjy: 切回正在运行或已执行设备时，从临时输出文件补读最新内容。
        m_scriptOutputDirty = false;
        if (m_scriptOutputAutoScroll) {
            m_scriptOutputScrollOffset = 0;
        }
    }

    updateScriptFileEditorControls();
    update(scriptFileEditorRect().adjusted(-2, -2, 2, 2).united(scriptTerminalPanelRect().toAlignedRect().adjusted(-2, -2, 2, 2)));
// ===end====
}

void DeviceGrid::applyRemoteScriptRuntimeState(
    const QString& deviceIp,
    const QString& loginUser,
    const platform::RemoteScriptRuntimeInfo& runtime)
{
// =====wjy====
    const QString targetIp = deviceIp.trimmed();
    if (targetIp.isEmpty() || !runtime.supported || !runtime.statusKnown) {
        return; // wjy: 旧目标端或无法确认的响应不覆盖本地状态，避免把“未知”错误显示成“未运行”。
    }

    ScriptUiState state = m_scriptUiStateStore.state(targetIp);
    if (runtime.running) {
        const bool recoveredRun = !state.outputRunning
            || (!runtime.runId.isEmpty() && state.remoteRunId != runtime.runId); // wjy: 新进程首次发现运行或 runId 变化时重建本设备的日志提示。
        state.outputVisible = true;
        state.outputRunning = true;
        state.outputFailed = false;
        state.remoteStatusConfirmed = true;
        state.remoteRunId = runtime.runId;
        state.remoteControllerPid = runtime.controllerPid;
        state.remoteStartedAtEpochMs = runtime.startedAtEpochMs;
        state.editorDeviceIp = targetIp;
        state.editorLoginUser = loginUser.trimmed();
        state.editorWorkName = runtime.workName;
        state.cancelRequested = state.cancelRequested
            ? state.cancelRequested
            : std::make_shared<std::atomic_bool>(false); // wjy: 控制端重启恢复的任务也创建本地取消标志，停止流程可沿用现有 SSH 退出机制。
        if (state.outputFilePath.trimmed().isEmpty()) {
            state.outputFilePath = scriptOutputTempFilePath(); // wjy: 新控制端没有旧临时日志文件时创建独立缓存，避免和其它设备输出混写。
        }
        const QString restoredScriptName = runtime.scriptName.trimmed().isEmpty()
            ? runtime.workName
            : runtime.scriptName;
        if (recoveredRun) {
            state.outputTitle = QString::fromUtf8("已恢复 - %1").arg(restoredScriptName);
            state.outputText = QString::fromUtf8(
                "状态: 已从目标设备恢复正在执行的脚本\n脚本: %1\n目标 PID: %2\n")
                .arg(restoredScriptName)
                .arg(runtime.controllerPid); // wjy: 明确告诉用户该状态来自远端确认，而不是本控制端刚刚发起的任务。
            writeScriptOutputFile(state.outputFilePath, state.outputText, QIODevice::Truncate);
        }
    } else if (state.outputRunning && !state.localLaunchInProgress) {
        state.outputRunning = false;
        state.outputFailed = false;
        state.remoteRunId.clear();
        state.remoteControllerPid = 0;
        state.remoteStartedAtEpochMs = 0;
        if (!state.outputFilePath.trimmed().isEmpty()) {
            writeScriptOutputFile(
                state.outputFilePath,
                QString::fromUtf8("\n状态: 目标设备已确认脚本不再运行。\n"),
                QIODevice::Append); // wjy: 目标端确认结束后同步清掉陈旧图标，并在本地恢复日志中留下原因。
            state.outputText = stripTerminalControlSequences(readScriptOutputFileTail(state.outputFilePath));
        }
    }

    m_scriptUiStateStore.setState(targetIp, std::move(state));
    update(deviceListViewportRect(m_deviceGroupExpanded)); // wjy: 每次远端确认后立即刷新左侧列表，让重启恢复的运行图标无需等待用户切换设备。
    if (currentScriptUiDeviceIp() == targetIp) {
        loadScriptUiStateForDevice(targetIp); // wjy: 当前详情设备同时恢复停止所需的 loginUser/workName 和脚本终端提示。
    }
// ===end====
}

void DeviceGrid::setupScriptFileEditor()
{
// =====wjy====
    m_scriptOutputEdit = new QTextEdit(this);
    m_scriptOutputEdit->setGeometry(scriptTerminalOutputRect().toAlignedRect());
    m_scriptOutputEdit->setReadOnly(true); // wjy: 禁止修改运行日志，但保留标准文本选择、Ctrl+C和右键复制菜单。
    m_scriptOutputEdit->setAcceptRichText(false);
    m_scriptOutputEdit->setLineWrapMode(QTextEdit::NoWrap); // wjy: 日志按远端原始行显示，长命令通过水平滚动查看，复制内容不会被视觉换行改变。
    m_scriptOutputEdit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QFont outputFont(QStringLiteral("Consolas"));
    outputFont.setPixelSize(12);
    m_scriptOutputEdit->setFont(outputFont);
    m_scriptOutputEdit->setStyleSheet(QStringLiteral(
        "QTextEdit{background:#0B1018;border:0;padding:0;color:#A7F3D0;font-family:Consolas;font-size:12px;selection-background-color:#2563EB;selection-color:#FFFFFF;}"
        "QScrollBar:vertical{background:#111827;width:9px;margin:0;}"
        "QScrollBar::handle:vertical{background:#5A6D84;min-height:26px;border-radius:4px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"));
    connect(m_scriptOutputEdit->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (!m_scriptOutputEdit) {
            return;
        }
        m_scriptOutputAutoScroll = value >= m_scriptOutputEdit->verticalScrollBar()->maximum(); // wjy: 用户滚到末尾后继续跟随新日志，向上查看时刷新不会强制抢回底部。
    });

    m_scriptFileEdit = new QTextEdit(this);
    m_scriptFileEdit->setGeometry(scriptFileEditorTextRect());
    m_scriptFileEdit->setAcceptRichText(false);
    m_scriptFileEdit->setLineWrapMode(QTextEdit::NoWrap);
    QFont editorFont(QStringLiteral("Consolas"));
    editorFont.setPixelSize(12);
    m_scriptFileEdit->setFont(editorFont);
    m_scriptFileEdit->setStyleSheet(QStringLiteral(
        "QTextEdit{background:#05070A;border:1px solid #263241;border-radius:4px;"
        "padding:8px;color:#F8FAFC;font-family:Consolas;font-size:12px;selection-background-color:#2563EB;}"
        "QTextEdit:focus{border:1px solid #3A7BFC;}"));

    m_scriptFileSaveButton = new QPushButton(QString::fromUtf8("保存"), this);
    m_scriptFileSaveButton->setGeometry(scriptFileEditorSaveButtonRect());
    m_scriptFileSaveButton->setCursor(Qt::PointingHandCursor);
    m_scriptFileSaveButton->setStyleSheet(QStringLiteral(
        "QPushButton{border:1px solid #3A7BFC;border-radius:4px;background:#FFFFFF;"
        "font-family:'Microsoft YaHei UI';font-size:12px;color:#006BFF;}"
        "QPushButton:hover{background:#F3F7FF;}"
        "QPushButton:disabled{border-color:#CBD5E1;color:#94A3B8;background:#F8FAFC;}"));
    connect(m_scriptFileSaveButton, &QPushButton::clicked, this, &DeviceGrid::saveScriptFileEditor);
    updateScriptFileEditorControls();
// ===end====
}

void DeviceGrid::setupScriptFolderTree()
{
// =====wjy====
    m_scriptFolderTree = new QTreeWidget(this);
    m_scriptFolderTree->setHeaderHidden(true);
    m_scriptFolderTree->setColumnCount(1);
    m_scriptFolderTree->setRootIsDecorated(true);
    m_scriptFolderTree->setAnimated(false);
    m_scriptFolderTree->setIndentation(14);
    m_scriptFolderTree->setUniformRowHeights(true);
    m_scriptFolderTree->setStyleSheet(QStringLiteral(
        "QTreeWidget{background:#0F172A;border:1px solid #263241;border-radius:3px;"
        "color:#DDE7F3;font-family:'Microsoft YaHei UI';font-size:12px;padding:4px;}"
        "QTreeWidget::item{height:24px;border-radius:3px;padding:0 4px;}"
        "QTreeWidget::item:selected{background:#1D4ED8;color:#FFFFFF;}"
        "QTreeWidget::item:hover{background:#1F2937;}"
        "QTreeWidget::branch{background:transparent;}"));
    connect(m_scriptFolderTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        selectScriptFolderTreeItem(item);
    });
    connect(m_scriptFolderTree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        selectScriptFolderTreeItem(item);
    });
    setScriptFolderTreePlaceholder(QString::fromUtf8("正在检测脚本网盘…")); // wjy: 构造阶段只创建本地节点，绝不执行 QDir::exists 或共享目录递归。
    requestScriptFolderTreeRefresh(); // wjy: QTcpSocket 异步探测可立即返回构造流程，窗口不再等待 Windows SMB 超时。
    updateScriptFileEditorControls();
// ===end====
}

void DeviceGrid::setScriptFolderTreePlaceholder(const QString& text)
{
// =====wjy====
    if (!m_scriptFolderTree) {
        return;
    }

    m_scriptFolderTree->clear();
    const QString rootPath = QString::fromUtf8(kRemoteScriptFolderPath);
    QTreeWidgetItem* rootItem = new QTreeWidgetItem(m_scriptFolderTree, QStringList(QString::fromUtf8("远程脚本文件")));
    rootItem->setData(0, Qt::UserRole, rootPath);
    rootItem->setToolTip(0, rootPath);
    rootItem->setExpanded(true);
    QTreeWidgetItem* placeholderItem = new QTreeWidgetItem(rootItem, QStringList(text));
    placeholderItem->setDisabled(true); // wjy: 占位状态完全由内存构造，不解析 UNC 元数据。
// ===end====
}

void DeviceGrid::requestScriptFolderTreeRefresh()
{
// =====wjy====
    if (!m_scriptFolderTree || m_shuttingDown || m_scriptFolderShareProbePending || m_scriptFolderLoadInProgress) {
        return; // wjy: 关闭、探测或枚举期间合并重复请求，不增加套接字和线程。
    }

    m_scriptFolderShareProbePending = true;
    m_scriptFolderTreeLoaded = false;
    setScriptFolderTreePlaceholder(QString::fromUtf8("正在检测脚本网盘…"));
    platform::SharedStorageAvailabilityService::instance().requestProbe(); // wjy: 只发起异步 445 检测；失败时不会创建任何共享文件线程。
// ===end====
}

void DeviceGrid::startScriptFolderTreeLoad()
{
// =====wjy====
    if (!m_scriptFolderTree || m_shuttingDown || m_scriptFolderLoadInProgress) {
        return; // wjy: 探测晚到或已有加载任务时不重复递归共享目录。
    }

    m_scriptFolderLoadInProgress = true;
    m_scriptFolderTreeLoaded = false;
    const quint64 requestId = ++m_scriptFolderLoadRequestId;
    setScriptFolderTreePlaceholder(QString::fromUtf8("正在后台读取脚本目录…"));

    const QString rootPath = QString::fromUtf8(kRemoteScriptFolderPath);
    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, rootPath, requestId] {
        QElapsedTimer loadTimer;
        loadTimer.start();
        platform::StartupPerformanceLog::checkpoint(QStringLiteral("[startup-script] background tree load begin"));
        const QJsonObject snapshot = loadScriptFolderTreeSnapshot(rootPath); // wjy: QDir::exists 和全部递归枚举只在可取消工作线程执行。
        platform::StartupPerformanceLog::checkpoint(QStringLiteral("[startup-script] background tree load end available=%1 duration_ms=%2")
            .arg(snapshot.value(QStringLiteral("available")).toBool())
            .arg(loadTimer.elapsed())); // wjy: 即使期间穿插首帧日志，也能直接从字段读取完整共享目录耗时。
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, requestId, snapshot] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            if (requestId != grid->m_scriptFolderLoadRequestId || grid->m_shuttingDown) {
                return; // wjy: 只应用最新一轮结果，退出或后续刷新不会被旧快照覆盖。
            }
            grid->m_scriptFolderLoadInProgress = false;
            grid->applyScriptFolderTreeSnapshot(snapshot); // wjy: 所有 QTreeWidget 修改统一回到主线程。
        }, Qt::QueuedConnection);
    });
// ===end====
}

void DeviceGrid::applyScriptFolderTreeSnapshot(const QJsonObject& snapshot)
{
// =====wjy====
    if (!m_scriptFolderTree) {
        return;
    }

    m_scriptFolderTree->clear();
    const QString rootPath = snapshot.value(QStringLiteral("path")).toString(QString::fromUtf8(kRemoteScriptFolderPath));
    const QString rootName = snapshot.value(QStringLiteral("name")).toString(QString::fromUtf8("远程脚本文件"));
    QTreeWidgetItem* rootItem = new QTreeWidgetItem(m_scriptFolderTree, QStringList(rootName));
    rootItem->setData(0, Qt::UserRole, rootPath);
    rootItem->setData(0, kScriptTreeNodeTypeRole, kScriptTreeFolderNode); // wjy: 根节点明确标记为目录，不能被当成可执行入口。
    rootItem->setToolTip(0, rootPath);
    rootItem->setExpanded(true);

    if (!snapshot.value(QStringLiteral("available")).toBool()) {
        QTreeWidgetItem* unavailableItem = new QTreeWidgetItem(rootItem,
            QStringList(snapshot.value(QStringLiteral("error")).toString(QString::fromUtf8("无法访问脚本目录"))));
        unavailableItem->setDisabled(true);
        m_scriptFolderTreeLoaded = false;
        return; // wjy: 后台失败只更新占位文字，主线程不会再次验证共享路径。
    }

    std::function<void(QTreeWidgetItem*, const QJsonArray&)> appendScripts;
    appendScripts = [](QTreeWidgetItem* parentItem, const QJsonArray& scripts) {
        for (const QJsonValue& scriptValue : scripts) {
            const QJsonObject script = scriptValue.toObject();
            const QString scriptName = script.value(QStringLiteral("name")).toString();
            const QString scriptPath = script.value(QStringLiteral("path")).toString();
            if (scriptName.isEmpty() || scriptPath.isEmpty()) {
                continue;
            }
            QTreeWidgetItem* scriptItem = new QTreeWidgetItem(parentItem, QStringList(scriptName));
            scriptItem->setData(0, Qt::UserRole, scriptPath);
            scriptItem->setData(0, kScriptTreeNodeTypeRole, kScriptTreeFileNode); // wjy: 树节点绑定具体入口文件，后续点击直接执行该文件所在目录的副本。
            scriptItem->setToolTip(0, scriptPath);
        }
    };

    std::function<void(QTreeWidgetItem*, const QJsonArray&)> appendChildren;
    appendChildren = [&appendChildren, &appendScripts](QTreeWidgetItem* parentItem, const QJsonArray& children) {
        for (const QJsonValue& childValue : children) {
            const QJsonObject child = childValue.toObject();
            const QString childName = child.value(QStringLiteral("name")).toString();
            const QString childPath = child.value(QStringLiteral("path")).toString();
            if (childName.isEmpty() || childPath.isEmpty()) {
                continue;
            }
            QTreeWidgetItem* childItem = new QTreeWidgetItem(parentItem, QStringList(childName));
            childItem->setData(0, Qt::UserRole, childPath);
            childItem->setData(0, kScriptTreeNodeTypeRole,
                child.value(QStringLiteral("type")).toString() == QStringLiteral("file")
                    ? kScriptTreeFileNode
                    : kScriptTreeFolderNode); // wjy: 快照中的目录/文件类型直接传递到 UI，菜单和树选中规则共用这一标记。
            childItem->setToolTip(0, childPath);
            appendScripts(childItem, child.value(QStringLiteral("scripts")).toArray());
            appendChildren(childItem, child.value(QStringLiteral("children")).toArray()); // wjy: UI 只消费纯 JSON 数据，不执行 QFileInfo/QDir 查询。
        }
    };
    appendScripts(rootItem, snapshot.value(QStringLiteral("scripts")).toArray());
    appendChildren(rootItem, snapshot.value(QStringLiteral("children")).toArray());
    if (rootItem->childCount() == 0) {
        QTreeWidgetItem* emptyItem = new QTreeWidgetItem(rootItem, QStringList(QString::fromUtf8("无可执行脚本或子文件夹")));
        emptyItem->setDisabled(true);
    }
    m_scriptFolderTreeLoaded = true;
    syncScriptFolderTreeSelection();
// ===end====
}

void DeviceGrid::populateCachedScriptFolderMenu(QMenu* menu) const
{
// =====wjy====
    if (!menu) {
        return;
    }
    if (m_scriptFolderShareProbePending || m_scriptFolderLoadInProgress) {
        QAction* loadingAction = menu->addAction(QString::fromUtf8("脚本目录正在后台加载"));
        loadingAction->setEnabled(false);
        return;
    }
    if (!m_scriptFolderTreeLoaded || !m_scriptFolderTree || m_scriptFolderTree->topLevelItemCount() == 0) {
        QAction* unavailableAction = menu->addAction(QString::fromUtf8("脚本网盘不可用"));
        unavailableAction->setEnabled(false); // wjy: 右键弹出阶段只读缓存状态，不再同步检测 192.168.1.100。
        return;
    }

    QTreeWidgetItem* rootItem = m_scriptFolderTree->topLevelItem(0);
    std::function<bool(QMenu*, QTreeWidgetItem*)> appendMenuItem;
    appendMenuItem = [&appendMenuItem](QMenu* targetMenu, QTreeWidgetItem* item) {
        if (!targetMenu || !item) {
            return false;
        }
        if (!(item->flags() & Qt::ItemIsEnabled)) {
            QAction* placeholderAction = targetMenu->addAction(item->text(0));
            placeholderAction->setEnabled(false);
            return true;
        }
        if (item->data(0, kScriptTreeNodeTypeRole).toInt() == kScriptTreeFileNode) {
            QAction* scriptFileAction = targetMenu->addAction(item->text(0));
            scriptFileAction->setData(item->data(0, Qt::UserRole)); // wjy: 菜单动作绑定用户点选的具体入口文件，不再把目录名当作隐式入口。
            return true;
        }
        QMenu* childMenu = targetMenu->addMenu(item->text(0));
        for (int childIndex = 0; childIndex < item->childCount(); ++childIndex) {
            appendMenuItem(childMenu, item->child(childIndex));
        }
        if (childMenu->actions().isEmpty()) {
            targetMenu->removeAction(childMenu->menuAction());
            delete childMenu; // wjy: 没有可执行入口的目录不生成空子菜单，避免用户点击后才得到无意义的目录动作。
            return false;
        }
        return true;
    };
    for (int childIndex = 0; childIndex < rootItem->childCount(); ++childIndex) {
        appendMenuItem(menu, rootItem->child(childIndex));
    }
// ===end====
}

void DeviceGrid::selectScriptFolderTreeItem(QTreeWidgetItem* item)
{
// =====wjy====
    if (!item || !(item->flags() & Qt::ItemIsEnabled)) {
        return;
    }

    if (item->data(0, kScriptTreeNodeTypeRole).toInt() != kScriptTreeFileNode) {
        return; // wjy: 文件夹节点只负责展开目录，只有用户明确点击入口文件时才更新执行目标。
    }

    const QString scriptEntryPath = item->data(0, Qt::UserRole).toString().trimmed();
    if (scriptEntryPath.isEmpty()) {
        return;
    }

    m_lastScriptEntryPath = QDir::cleanPath(scriptEntryPath); // wjy: 保存具体入口文件路径，执行时由其父目录作为复制范围。
    const QString entryName = item->text(0).trimmed();
    m_scriptOutputTitle = entryName.trimmed().isEmpty()
        ? QString::fromUtf8("脚本日志")
        : QString::fromUtf8("已选择: %1").arg(entryName); // wjy: 日志标题显示用户实际选择的文件，多个入口同目录时不会再产生歧义。
    saveCurrentScriptUiState();
    update(scriptTerminalPanelRect().toAlignedRect().adjusted(-2, -2, 2, 2));
// ===end====
}

void DeviceGrid::syncScriptFolderTreeSelection()
{
// =====wjy====
    if (!m_scriptFolderTree) {
        return;
    }

    const QString selectedPath = QDir::cleanPath(m_lastScriptEntryPath); // wjy: 仅做字符串规范化，选择同步不查询共享目录元数据。
    if (m_lastScriptEntryPath.trimmed().isEmpty()) {
        QSignalBlocker blocker(m_scriptFolderTree);
        m_scriptFolderTree->clearSelection();
        m_scriptFolderTree->setCurrentItem(nullptr);
        return;
    }

    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> findByPath =
        [&](QTreeWidgetItem* parentItem) -> QTreeWidgetItem* {
            if (!parentItem) {
                return nullptr;
            }
            if (QDir::cleanPath(parentItem->data(0, Qt::UserRole).toString()) == selectedPath) {
                return parentItem;
            }
            for (int i = 0; i < parentItem->childCount(); ++i) {
                if (QTreeWidgetItem* match = findByPath(parentItem->child(i))) {
                    return match;
                }
            }
            return nullptr;
        };

    QTreeWidgetItem* match = nullptr;
    for (int i = 0; i < m_scriptFolderTree->topLevelItemCount(); ++i) {
        match = findByPath(m_scriptFolderTree->topLevelItem(i));
        if (match) {
            break;
        }
    }

    QSignalBlocker blocker(m_scriptFolderTree);
    if (!match) {
        m_scriptFolderTree->clearSelection();
        return;
    }
    m_scriptFolderTree->setCurrentItem(match);
    m_scriptFolderTree->scrollToItem(match, QAbstractItemView::PositionAtCenter);
    for (QTreeWidgetItem* parentItem = match->parent(); parentItem; parentItem = parentItem->parent()) {
        parentItem->setExpanded(true);
    }
// ===end====
}

void DeviceGrid::updateScriptFileEditorControls()
{
// =====wjy====
    syncResponsiveLayoutState();
    const ScriptPanelViewContext panelContext{
        m_settingsSelected,
        m_remoteAssistSelected,
        m_localInfoSelected,
        m_deviceDetailTab == DeviceDetailTab::ScriptLog,
        m_deviceDetailTab == DeviceDetailTab::Config,
        m_scriptOutputVisible,
        m_scriptEditorVisible};
    const ScriptPanelVisibility panelVisibility = computeScriptPanelVisibility(panelContext); // wjy: 脚本树、输出和编辑器共用同一份页面可见性判定，避免三个控件各自维护互相漂移的条件。
    const bool detailControlsVisible = !m_detailPanelCollapsed; // wjy: 紧凑窗口保留页面逻辑状态，但所有右侧真实控件必须隐藏并禁用。
    const bool scriptTreeVisible = detailControlsVisible && panelVisibility.treeVisible;
    if (m_scriptFolderTree) {
        m_scriptFolderTree->setGeometry(scriptFolderTreeRect());
        m_scriptFolderTree->setVisible(scriptTreeVisible);
        m_scriptFolderTree->setEnabled(scriptTreeVisible);
        if (scriptTreeVisible) {
            syncScriptFolderTreeSelection();
            m_scriptFolderTree->raise();
        }
    }

    const bool scriptOutputVisible = detailControlsVisible && panelVisibility.outputVisible;
    if (m_scriptOutputEdit) {
        m_scriptOutputEdit->setGeometry(scriptTerminalOutputRect().toAlignedRect());
        m_scriptOutputEdit->setVisible(scriptOutputVisible);
        m_scriptOutputEdit->setEnabled(scriptOutputVisible);
        if (scriptOutputVisible) {
            const QString normalizedOutput = terminalOutputLines(
                stripTerminalControlSequences(m_scriptOutputText)).join(QLatin1Char('\n'));
            if (m_scriptOutputEdit->toPlainText() != normalizedOutput) {
                const QTextCursor oldCursor = m_scriptOutputEdit->textCursor();
                const bool hadSelection = oldCursor.hasSelection();
                const int oldPosition = oldCursor.position();
                const int oldAnchor = oldCursor.anchor();
                QScrollBar* scrollBar = m_scriptOutputEdit->verticalScrollBar();
                const int oldScrollValue = scrollBar->value(); // wjy: 文档整体替换前保存真实QTextEdit滚动位置，用户向上查看时刷新后仍停在原处。
                const int oldScrollMaximum = scrollBar->maximum();
                const bool followNewest = m_scriptOutputAutoScroll
                    || oldScrollValue >= oldScrollMaximum; // wjy: 首次刚出现滚动条时也按刷新前状态决定是否跟随尾部，不读取重建过程中的临时0范围。
                const QSignalBlocker scrollSignalBlocker(scrollBar); // wjy: setPlainText重建文档时会把滚动条暂时归零，禁止该程序行为反向改写用户的自动滚动意图。
                m_scriptOutputEdit->setPlainText(normalizedOutput); // wjy: 仅日志内容真正变化时刷新控件，避免每次重绘都破坏选择和复制操作。
                const int documentLength = qMax(0, m_scriptOutputEdit->document()->characterCount() - 1);
                if (hadSelection) {
                    QTextCursor restoredCursor(m_scriptOutputEdit->document());
                    restoredCursor.setPosition(qBound(0, oldAnchor, documentLength));
                    restoredCursor.setPosition(qBound(0, oldPosition, documentLength), QTextCursor::KeepAnchor);
                    m_scriptOutputEdit->setTextCursor(restoredCursor); // wjy: 日志追加期间尽量恢复原选择范围，让用户可以稳定完成Ctrl+C。
                } else if (followNewest) {
                    QTextCursor endCursor = m_scriptOutputEdit->textCursor();
                    endCursor.movePosition(QTextCursor::End);
                    m_scriptOutputEdit->setTextCursor(endCursor);
                } else {
                    QTextCursor restoredCursor(m_scriptOutputEdit->document());
                    restoredCursor.setPosition(qBound(0, oldPosition, documentLength));
                    m_scriptOutputEdit->setTextCursor(restoredCursor); // wjy: 未跟随尾部时保留只读文本光标，键盘选择不会在每行日志到达后跳回开头。
                }
                const int restoredScrollValue = followNewest
                    ? scrollBar->maximum()
                    : qBound(scrollBar->minimum(), oldScrollValue, scrollBar->maximum());
                scrollBar->setValue(restoredScrollValue); // wjy: 文档范围稳定后只设置一次最终位置，消除新日志到达时首页与尾页交替跳转。
                m_scriptOutputAutoScroll = followNewest; // wjy: 刷新完成后恢复刷新前的跟随意图，只有真实用户滚动产生的信号才允许改变它。
            }
            m_scriptOutputEdit->raise(); // wjy: 真实文本控件覆盖原自绘正文区域，但不遮挡标题、脚本树和启动/停止按钮。
        }
    }

    const bool visible = detailControlsVisible && panelVisibility.editorVisible;
    if (m_scriptFileEdit) {
        m_scriptFileEdit->setGeometry(scriptFileEditorTextRect());
        m_scriptFileEdit->setVisible(visible);
        m_scriptFileEdit->setEnabled(visible && !m_scriptEditorLoading && !m_scriptEditorSaving && !m_scriptEditorRemotePath.isEmpty());
        if (visible) {
            m_scriptFileEdit->raise();
        }
    }
    if (m_scriptFileSaveButton) {
        m_scriptFileSaveButton->setGeometry(scriptFileEditorSaveButtonRect());
        m_scriptFileSaveButton->setVisible(visible);
        m_scriptFileSaveButton->setEnabled(visible && !m_scriptEditorLoading && !m_scriptEditorSaving && !m_scriptEditorRemotePath.isEmpty());
        m_scriptFileSaveButton->setText(m_scriptEditorSaving ? QString::fromUtf8("保存中") : QString::fromUtf8("保存"));
        if (visible) {
            m_scriptFileSaveButton->raise();
        }
    }
    updateDeviceSearchPanel(); // wjy: 搜索面板与脚本树、日志和配置编辑器共用同一页签可见性刷新时机，切页后不会互相覆盖。
    updateLocalSystemMonitorState(); // wjy: 真实控件显隐刷新完成后同步三类资源定时器，任一页面切换都不会留下后台采样。
// ===end====
}

void DeviceGrid::loadScriptFileEditor(const QString& deviceIp, const QString& loginUser, const QString& scriptWorkName)
{
// =====wjy====
    if (deviceIp.trimmed().isEmpty() || loginUser.trimmed().isEmpty() || scriptWorkName.trimmed().isEmpty()) {
        return;
    }
    const QString targetIp = deviceIp.trimmed();
    const bool targetIsCurrent = currentScriptUiDeviceIp() == targetIp;
    ScriptUiState loadingState = m_scriptUiStateStore.state(targetIp);
    loadingState.editorVisible = true;
    loadingState.editorLoading = true;
    loadingState.editorSaving = false;
    loadingState.editorTitle = QString::fromUtf8("本地文件");
    loadingState.editorRemotePath.clear();
    loadingState.editorDeviceIp = targetIp;
    loadingState.editorLoginUser = loginUser;
    loadingState.editorWorkName = scriptWorkName;
    loadingState.editorRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces); // wjy: 记录本次读取请求，设备切换或再次读取后旧回调自动失效。
    loadingState.editorText = QString::fromUtf8("正在读取目标设备本地 json/txt 文件...");
    loadingState.editorModified = false;
    m_scriptUiStateStore.setState(targetIp, loadingState); // wjy: 开始读取时先更新目标设备状态，切走设备也不会污染当前编辑器。
    if (targetIsCurrent) {
        m_scriptEditorVisible = loadingState.editorVisible;
        m_scriptEditorLoading = loadingState.editorLoading;
        m_scriptEditorSaving = loadingState.editorSaving;
        m_scriptEditorTitle = loadingState.editorTitle;
        m_scriptEditorRemotePath = loadingState.editorRemotePath;
        m_scriptEditorDeviceIp = loadingState.editorDeviceIp;
        m_scriptEditorLoginUser = loadingState.editorLoginUser;
        applyState(loadingState, targetIp, m_scriptFileEdit); // wjy: 当前设备的编辑器显示统一由控制器恢复。
        updateScriptFileEditorControls();
    }

    const QString remotePowerShellScript = QStringLiteral(R"($ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$fsremoteExe = Get-Process FSRemote -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Path
if ([string]::IsNullOrWhiteSpace($fsremoteExe)) {
    Write-Output 'FSREMOTE_EDIT_NOFILE'
    exit 0
}
$fsremoteDir = Split-Path -Parent $fsremoteExe
$work = Join-Path (Join-Path $fsremoteDir 'work') '%1'
if (-not (Test-Path -LiteralPath $work)) {
    Write-Output 'FSREMOTE_EDIT_NOFILE'
    exit 0
}
$file = Get-ChildItem -LiteralPath $work -File | Where-Object { $_.Extension -in '.json', '.txt' } | Sort-Object Name | Select-Object -First 1
if ($null -eq $file) {
    Write-Output 'FSREMOTE_EDIT_NOFILE'
    exit 0
}
$content = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
Write-Output 'FSREMOTE_EDIT_PATH_BEGIN'
Write-Output ([Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($file.FullName)))
Write-Output 'FSREMOTE_EDIT_NAME_BEGIN'
Write-Output ([Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($file.Name)))
Write-Output 'FSREMOTE_EDIT_CONTENT_BEGIN'
Write-Output ([Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($content)))
Write-Output 'FSREMOTE_EDIT_END'
)").arg(escapedPowerShellSingleQuoted(scriptWorkName));
    const QStringList commands = {
        QStringLiteral("powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand %1 & exit %ERRORLEVEL%")
            .arg(powerShellEncodedCommand(remotePowerShellScript)),
    };

    const QString requestId = loadingState.editorRequestId;
    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, deviceIp, loginUser, scriptWorkName, requestId, commands] {
        QString outputText;
        QString errorMessage;
        const bool ok = platform::PortableOpenSshManager::instance().runRemoteCommands(
            deviceIp,
            loginUser,
            commands,
            &outputText,
            &errorMessage,
            60000);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, ok, deviceIp, loginUser, scriptWorkName, requestId, outputText, errorMessage] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            const QString targetIp = deviceIp.trimmed();
            const bool targetIsCurrent = grid->currentScriptUiDeviceIp() == targetIp;
            ScriptUiState state = grid->m_scriptUiStateStore.state(targetIp);
            if (state.editorRequestId != requestId) {
                return; // wjy: 旧读取回调不能覆盖同一设备后续发起的新读取或保存请求。
            }
            const QString activeWorkName = targetIsCurrent ? grid->m_scriptEditorWorkName : state.editorWorkName;
            if (activeWorkName != scriptWorkName) {
                return;
            }
            QString nextRemotePath;
            QString nextTitle;
            QString nextText;
            const QString result = ok ? stripTerminalControlSequences(outputText) : errorMessage.trimmed();
            const QStringList lines = result.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
            const int pathMarker = lines.indexOf(QStringLiteral("FSREMOTE_EDIT_PATH_BEGIN"));
            const int nameMarker = lines.indexOf(QStringLiteral("FSREMOTE_EDIT_NAME_BEGIN"));
            const int contentMarker = lines.indexOf(QStringLiteral("FSREMOTE_EDIT_CONTENT_BEGIN"));
            const int endMarker = lines.indexOf(QStringLiteral("FSREMOTE_EDIT_END"));
            if (ok
                && pathMarker >= 0
                && nameMarker > pathMarker
                && contentMarker > nameMarker
                && endMarker > contentMarker
                && pathMarker + 1 < nameMarker
                && nameMarker + 1 < contentMarker) {
                QStringList pathBase64Parts;
                for (int i = pathMarker + 1; i < nameMarker; ++i) {
                    pathBase64Parts.append(lines.at(i).trimmed()); // wjy: 远端输出可能按终端宽度折行，路径 Base64 也按片段拼回完整字符串。
                }
                QStringList nameBase64Parts;
                for (int i = nameMarker + 1; i < contentMarker; ++i) {
                    nameBase64Parts.append(lines.at(i).trimmed()); // wjy: 文件名单独返回，避免标题从完整路径解析时受特殊字符影响而乱码。
                }
                QStringList contentBase64Parts;
                for (int i = contentMarker + 1; i < endMarker; ++i) {
                    contentBase64Parts.append(lines.at(i).trimmed()); // wjy: JSON/TXT 内容较长时 Base64 会被拆成多行，必须拼接全部片段后再解码。
                }
                nextRemotePath = utf8FromBase64(pathBase64Parts.join(QString()));
                const QString fileName = utf8FromBase64(nameBase64Parts.join(QString()));
                nextText = utf8FromBase64(contentBase64Parts.join(QString()));
                nextTitle = fileName.trimmed().isEmpty()
                    ? QFileInfo(nextRemotePath).fileName()
                    : fileName; // wjy: 标题优先使用远端直接返回的文件名，降低中文路径或控制字符导致的显示乱码概率。
            } else {
                nextText = ok
                    ? QString::fromUtf8("当前本地脚本目录没有 json/txt 文件。")
                    : QString::fromUtf8("读取失败：%1").arg(result);
            }
            state.editorVisible = true;
            state.editorLoading = false;
            state.editorSaving = false;
            state.editorTitle = nextTitle;
            state.editorRemotePath = nextRemotePath;
            state.editorDeviceIp = targetIp;
            state.editorLoginUser = loginUser; // wjy: 读取回调保存目标设备自己的 SSH 登录用户，避免切换设备后借用当前界面的用户。
            state.editorWorkName = scriptWorkName;
            state.editorText = nextText;
            state.editorModified = false;
            grid->m_scriptUiStateStore.setState(targetIp, state); // wjy: 文件读取结果先归档到目标设备自己的状态，切回来时能恢复对应编辑器。

            if (targetIsCurrent) {
                grid->m_scriptEditorVisible = state.editorVisible;
                grid->m_scriptEditorLoading = state.editorLoading;
                grid->m_scriptEditorSaving = state.editorSaving;
                grid->m_scriptEditorTitle = state.editorTitle;
                grid->m_scriptEditorRemotePath = state.editorRemotePath;
                grid->m_scriptEditorDeviceIp = state.editorDeviceIp;
                grid->m_scriptEditorLoginUser = state.editorLoginUser;
                grid->applyState(state, targetIp, grid->m_scriptFileEdit); // wjy: 只刷新当前设备自己的编辑器，避免别的设备后台读取覆盖当前文本。
                grid->updateScriptFileEditorControls();
                grid->update(scriptFileEditorRect().adjusted(-2, -2, 2, 2));
            }
        }, Qt::QueuedConnection);
    });
// ===end====
}

void DeviceGrid::saveScriptFileEditor()
{
// =====wjy====
    if (!m_scriptFileEdit
        || m_scriptEditorRemotePath.trimmed().isEmpty()
        || m_scriptEditorDeviceIp.trimmed().isEmpty()
        || m_scriptEditorLoginUser.trimmed().isEmpty()
        || m_scriptEditorSaving) {
        return;
    }
    m_scriptEditorSaving = true;
    updateScriptFileEditorControls();
    saveCurrentScriptUiState(); // wjy: 保存开始时把按钮状态写回当前设备缓存，切走后再回来仍能看到保存中的状态。
    const QString remotePath = m_scriptEditorRemotePath;
    const QString contentBase64 = base64Utf8(m_scriptFileEdit->toPlainText());
    const QString deviceIp = m_scriptEditorDeviceIp;
    const QString loginUser = m_scriptEditorLoginUser;
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces); // wjy: 保存操作也有独立请求 ID，防止迟到结果改写后续编辑内容。
    ScriptUiState requestState = m_scriptUiStateStore.state(deviceIp);
    requestState.editorRequestId = requestId;
    requestState.editorSaving = true;
    m_scriptUiStateStore.setState(deviceIp, std::move(requestState));
    const QString remotePowerShellScript = QStringLiteral(R"($ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$path = '%1'
$content = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%2'))
[System.IO.File]::WriteAllText($path, $content, [System.Text.UTF8Encoding]::new($false))
Write-Output 'FSREMOTE_EDIT_SAVE_OK'
)").arg(
        escapedPowerShellSingleQuoted(remotePath),
        contentBase64);
    const QStringList commands = {
        QStringLiteral("powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand %1 & exit %ERRORLEVEL%")
            .arg(powerShellEncodedCommand(remotePowerShellScript)),
    };

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, deviceIp, loginUser, remotePath, requestId, commands] {
        QString outputText;
        QString errorMessage;
        const bool ok = platform::PortableOpenSshManager::instance().runRemoteCommands(
            deviceIp,
            loginUser,
            commands,
            &outputText,
            &errorMessage,
            60000);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, ok, deviceIp, remotePath, requestId, outputText, errorMessage] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            const QString targetIp = deviceIp.trimmed();
            const bool targetIsCurrent = grid->currentScriptUiDeviceIp() == targetIp;
            ScriptUiState state = grid->m_scriptUiStateStore.state(targetIp);
            if (state.editorRequestId != requestId) {
                return; // wjy: 旧保存回调不能撤销后续读取/保存对同一设备状态的更新。
            }
            const QString activeRemotePath = targetIsCurrent ? grid->m_scriptEditorRemotePath : state.editorRemotePath;
            if (activeRemotePath != remotePath) {
                return;
            }
            state.editorSaving = false;
            if (ok) {
                state.editorModified = false; // wjy: 保存成功只清掉目标设备自己的编辑器脏标记，不影响其它设备正在编辑的文本。
            }
            grid->m_scriptUiStateStore.setState(targetIp, state);
            if (targetIsCurrent) {
                grid->m_scriptEditorSaving = false;
                if (ok && grid->m_scriptFileEdit) {
                    grid->m_scriptFileEdit->document()->setModified(false); // wjy: 保存成功后标记当前文本已经同步到被控机本地 work 文件。
                }
                if (grid->m_scriptFileSaveButton) {
                    grid->m_scriptFileSaveButton->setText(ok ? QString::fromUtf8("已保存") : QString::fromUtf8("失败"));
                    QTimer::singleShot(1200, grid, [grid] {
                        if (grid) {
                            grid->updateScriptFileEditorControls();
                        }
                    });
                }
            }
            if (!ok) {
                writeScriptOutputFile(
                    state.outputFilePath,
                    QString::fromUtf8("\n编辑器保存失败：%1\n").arg(errorMessage.trimmed().isEmpty() ? outputText.trimmed() : errorMessage.trimmed()),
                    QIODevice::Append);
                state.outputDirty = true;
                grid->m_scriptUiStateStore.setState(targetIp, state);
                if (targetIsCurrent) {
                    grid->m_scriptOutputDirty = true;
                }
                if (targetIsCurrent && !grid->m_scriptOutputFlushTimer->isActive()) {
                    grid->m_scriptOutputFlushTimer->start();
                }
            }
            if (targetIsCurrent) {
                grid->updateScriptFileEditorControls();
            }
        }, Qt::QueuedConnection);
    });
// ===end====
}

void DeviceGrid::stopCurrentDeviceScript()
{
// =====wjy====
    stopDeviceScriptForDeviceIndex(m_selectedDeviceIndex, true); // wjy: 当前面板的停止按钮也走指定设备停止逻辑，和分组批量停止保持一致。
// ===end====
}

bool DeviceGrid::stopDeviceScriptForDeviceIndex(int deviceIndex, bool showMessages)
{
// =====wjy====
    if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return false;
    }

    const QString deviceIp = g_devices.at(deviceIndex).ip.trimmed();
    ScriptUiState state = m_scriptUiStateStore.state(deviceIp);
    if (!state.outputRunning) {
        return false;
    }

    const QString loginUser = state.editorLoginUser.trimmed();
    const QString scriptWorkName = state.editorWorkName.trimmed();
    const QString remoteRunId = state.remoteRunId.trimmed(); // wjy: 恢复任务携带远端 runId，停止清单时只允许删除同一次执行记录。
    const QString outputFilePath = state.outputFilePath;
    const auto cancelRequested = state.cancelRequested;
    if (deviceIp.isEmpty() || loginUser.isEmpty() || scriptWorkName.isEmpty()) {
        if (cancelRequested) {
            cancelRequested->store(true); // wjy: 缺少远端定位信息时退回旧逻辑，至少断开当前 SSH 执行。
        }
        state.outputRunning = false;
        state.outputFailed = false;
        state.localLaunchInProgress = false; // wjy: 无法定位远端 work 时结束本地启动态，避免后续空闲刷新一直被启动竞态保护挡住。
        m_scriptUiStateStore.setState(deviceIp, std::move(state));
        update(deviceListViewportRect(m_deviceGroupExpanded)); // wjy: 停止状态写回后立即重绘左侧设备列表，让运行图标同步消失。
        if (currentScriptUiDeviceIp() == deviceIp) {
            loadScriptUiStateForDevice(deviceIp);
        }
        return true;
    }

    state.outputRunning = false;
    state.outputFailed = false;
    state.localLaunchInProgress = false; // wjy: 用户已明确请求停止，先关闭本地启动竞态保护并隐藏运行图标。
    writeScriptOutputFile(outputFilePath, QString::fromUtf8("\n状态: 正在停止目标进程...\n"), QIODevice::Append);
    state.outputText = stripTerminalControlSequences(readScriptOutputFileTail(outputFilePath));
    m_scriptUiStateStore.setState(deviceIp, state); // wjy: 先把目标设备状态写成停止中，当前 UI 或切回该设备时都能看到停止状态。
    update(deviceListViewportRect(m_deviceGroupExpanded)); // wjy: 发起停止时运行状态已经结束，侧栏徽标无需等待远端 taskkill 返回。
    if (currentScriptUiDeviceIp() == deviceIp) {
        loadScriptUiStateForDevice(deviceIp);
    }

    const QString remotePowerShellScript = QStringLiteral(R"($ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$fsremoteExe = Get-Process FSRemote -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Path
if ([string]::IsNullOrWhiteSpace($fsremoteExe)) {
    Write-Output 'cannot locate FSRemote.exe on target device'
    exit 0
}
$fsremoteDir = Split-Path -Parent $fsremoteExe
$workRoot = Join-Path $fsremoteDir 'work'
$scriptWorkName = '%1'
$expectedRunId = '%2'
$work = Join-Path $workRoot $scriptWorkName
$activeStateFile = Join-Path $workRoot 'fsremote_active_script.json'
$pidFile = Join-Path $work 'fsremote_script_controller_pid.txt'
$stopRequestFile = Join-Path $work 'fsremote_script_stop_requested.txt'
('Stop requested: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) | Set-Content -LiteralPath $stopRequestFile -Encoding UTF8
if (-not (Test-Path -LiteralPath $pidFile)) {
    Write-Output 'remote script pid file not found'
    exit 0
}
$pidText = Get-Content -LiteralPath $pidFile -Raw
$match = [regex]::Match($pidText, '\d+')
if (-not $match.Success) {
    Write-Output 'remote script pid file has no pid'
    exit 0
}
$targetPid = [int]$match.Value
if ($targetPid -le 0) {
    Write-Output 'remote script pid is invalid'
    exit 0
}
Write-Output ('taskkill remote script tree pid=' + $targetPid)
& taskkill.exe /PID $targetPid /T /F 2>&1 | ForEach-Object { Write-Output $_ }
Start-Sleep -Milliseconds 300
if (Get-Process -Id $targetPid -ErrorAction SilentlyContinue) {
    Write-Error ('remote script controller is still running pid=' + $targetPid)
    exit 1
}
# wjy: 只有 Windows 已确认控制进程退出后才删除 PID 和活动清单，taskkill 失败时保留远端权威状态供下次刷新恢复。
Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
if (Test-Path -LiteralPath $activeStateFile) {
    try {
        $activeState = Get-Content -LiteralPath $activeStateFile -Raw | ConvertFrom-Json
        $sameRun = -not [string]::IsNullOrWhiteSpace($expectedRunId) -and [string]$activeState.runId -eq $expectedRunId
        $sameLegacyRun = [string]::IsNullOrWhiteSpace($expectedRunId) -and [string]$activeState.workName -eq $scriptWorkName -and [int64]$activeState.controllerPid -eq $targetPid
        if ($sameRun -or $sameLegacyRun) {
            Remove-Item -LiteralPath $activeStateFile -Force -ErrorAction SilentlyContinue
        }
    } catch {
        Write-Output ('FSRemote active state stop cleanup skipped: ' + $_.Exception.Message)
    }
}
exit 0
)").arg(
        escapedPowerShellSingleQuoted(scriptWorkName),
        escapedPowerShellSingleQuoted(remoteRunId)); // wjy: 停止恢复任务后按 runId 清理全局活动清单；旧任务没有 runId 时用 workName 和 PID 双重匹配。
    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, deviceIp, loginUser, remotePowerShellScript, outputFilePath, cancelRequested] {
        QString outputText;
        QString errorMessage;
        const bool ok = platform::PortableOpenSshManager::instance().runRemotePowerShellScript(
            deviceIp,
            loginUser,
            remotePowerShellScript,
            &outputText,
            &errorMessage,
            15000); // wjy: 停止脚本也通过临时 ps1 执行，避免停止逻辑增长后再次碰到 EncodedCommand 长度和回显问题。
        if (cancelRequested) {
            cancelRequested->store(true); // wjy: 远端 taskkill 发出后再让原 SSH 执行会话退出，避免只杀本地 ssh 留下目标子进程。
        }
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, ok, deviceIp, outputFilePath, outputText, errorMessage] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            const QString targetIp = deviceIp.trimmed();
            ScriptUiState state = grid->m_scriptUiStateStore.state(targetIp);
            if (state.outputFilePath != outputFilePath) {
                return;
            }
            const QString result = ok ? outputText.trimmed() : errorMessage.trimmed();
            if (!result.isEmpty()) {
                writeScriptOutputFile(outputFilePath, QString::fromUtf8("\n%1\n").arg(stripTerminalControlSequences(result)), QIODevice::Append);
            }
            if (ok) {
                state.remoteStatusConfirmed = true;
                state.remoteRunId.clear();
                state.remoteControllerPid = 0;
                state.remoteStartedAtEpochMs = 0; // wjy: 远端停止命令完成后清除恢复元数据，下一次状态刷新会再次核对目标是否确实空闲。
            }
            state.outputDirty = true;
            grid->m_scriptUiStateStore.setState(targetIp, std::move(state));
            if (grid->currentScriptUiDeviceIp() == targetIp) {
                grid->m_scriptOutputDirty = true;
                if (!grid->m_scriptOutputFlushTimer->isActive()) {
                    grid->m_scriptOutputFlushTimer->start();
                }
            }
        }, Qt::QueuedConnection);
    });
    return true;
// ===end====
}

void DeviceGrid::stopDeviceScriptsForIndexes(const QVector<int>& deviceIndexes)
{
    QSet<int> visited;
    int stoppedCount = 0;
    for (const int deviceIndex : deviceIndexes) {
        if (deviceIndex < 0 || deviceIndex >= g_devices.size() || visited.contains(deviceIndex)) {
            continue;
        }
        visited.insert(deviceIndex);
        if (stopDeviceScriptForDeviceIndex(deviceIndex, false)) {
            ++stoppedCount; // wjy: 仅对确实处于运行态的目标发起停止，未运行设备静默跳过。
        }
    }
    if (stoppedCount <= 0) {
        QMessageBox messageBox(
            QMessageBox::Information,
            QString(),
            QString::fromUtf8("目标设备中没有正在执行的脚本。"),
            QMessageBox::NoButton,
            this);
        messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
        messageBox.exec(); // wjy: 无论单选还是多选都只显示一次汇总信息，不按设备重复弹窗。
    } else {
        update();
    }
}

void DeviceGrid::stopDeviceGroupScripts(int groupIndex)
{
// =====wjy====
    if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return;
    }

    int stoppedCount = 0;
    for (const int deviceIndex : deviceIndexesForGroup(groupIndex)) { // wjy: 分组批量停止复用可见目标集合，隐藏本机不会收到停止脚本命令。
        if (stopDeviceScriptForDeviceIndex(deviceIndex, false)) {
            ++stoppedCount; // wjy: 只停止组内确实正在运行脚本的设备，未运行的设备静默跳过。
        }
    }

    if (stoppedCount <= 0) {
        QMessageBox messageBox(
            QMessageBox::Information,
            QString(),
            QString::fromUtf8("分组内没有正在执行的脚本。"),
            QMessageBox::NoButton,
            this);
        messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
        messageBox.exec();
    } else {
        update(); // wjy: 当前详情设备属于该分组时，批量停止后立即刷新停止状态。
    }
// ===end====
}

void DeviceGrid::setupAddDeviceControls()
{
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] setupAddDeviceControls begin")); // wjy: 细分新增设备控件初始化入口，定位 Release 是否崩在控件创建阶段。
    const QString inputStyle = QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;"
        "padding-left:12px;font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QLineEdit:focus{border:1px solid #3A7BFC;}");
    const QString buttonStyle = QStringLiteral(
        "QPushButton{border:1px solid #DDE3EA;border-radius:4px;background:#FFFFFF;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QPushButton:hover{background:#F3F7FF;}");
    const QString primaryButtonStyle = QStringLiteral(
        "QPushButton{border:0;border-radius:4px;background:#3A7BFC;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#FFFFFF;}"
        "QPushButton:hover{background:#2F6FEF;}"
        "QPushButton:disabled{background:#C9D0DA;color:#FFFFFF;}");
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] setupAddDeviceControls styles ready")); // wjy: 样式字符串创建完成，后续开始逐个创建输入框和按钮。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before device ip edit create")); // wjy: 判断是否崩在第一个新增设备 IP 输入框创建。
    m_deviceIpEdit = new QLineEdit(this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device ip edit create")); // wjy: IP 输入框对象创建完成。
    m_deviceIpEdit->setGeometry(304, 250, 252, 34);
    m_deviceIpEdit->setPlaceholderText(QStringLiteral("192.168.1.100"));
    m_deviceIpEdit->setStyleSheet(inputStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device ip edit setup")); // wjy: IP 输入框位置、占位文本和样式设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before device name edit create")); // wjy: 判断是否崩在设备名称输入框创建。
    m_deviceNameEdit = new QLineEdit(this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device name edit create")); // wjy: 设备名称输入框对象创建完成。
    m_deviceNameEdit->setGeometry(584, 250, 252, 34);
    m_deviceNameEdit->setPlaceholderText(zh("\xE4\xBE\x8B\xE5\xA6\x82\xEF\xBC\x9A\xE5\x8A\x9E\xE5\x85\xAC\xE5\xAE\xA4\xE4\xB8\xBB\xE6\x9C\xBA"));
    m_deviceNameEdit->setStyleSheet(inputStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device name edit setup")); // wjy: 设备名称输入框位置、中文占位文本和样式设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before device mac edit create")); // wjy: 判断是否崩在 MAC 输入框创建。
    m_deviceMacEdit = new QLineEdit(this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device mac edit create")); // wjy: MAC 输入框对象创建完成。
    m_deviceMacEdit->setGeometry(304, 332, 252, 34);
    m_deviceMacEdit->setPlaceholderText(zh("\xE5\x8F\xAF\xE9\x80\x89"));
    m_deviceMacEdit->setStyleSheet(inputStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device mac edit setup")); // wjy: MAC 输入框位置、占位文本和样式设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before device remark edit create")); // wjy: 判断是否崩在备注输入框创建。
    m_deviceRemarkEdit = new QLineEdit(this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device remark edit create")); // wjy: 备注输入框对象创建完成。
    m_deviceRemarkEdit->setGeometry(584, 332, 252, 34);
    m_deviceRemarkEdit->setPlaceholderText(zh("\xE5\x8F\xAF\xE9\x80\x89"));
    m_deviceRemarkEdit->setStyleSheet(inputStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device remark edit setup")); // wjy: 备注输入框位置、占位文本和样式设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before cancel button create")); // wjy: 判断是否崩在取消按钮创建。
    m_cancelDeviceButton = new QPushButton(zh("\xE5\x8F\x96\xE6\xB6\x88"), this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after cancel button create")); // wjy: 取消按钮对象创建完成。
    m_cancelDeviceButton->setGeometry(572, 424, 124, 34);
    m_cancelDeviceButton->setCursor(Qt::PointingHandCursor);
    m_cancelDeviceButton->setStyleSheet(buttonStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after cancel button setup")); // wjy: 取消按钮位置、鼠标样式和样式表设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before save button create")); // wjy: 判断是否崩在保存按钮创建。
    m_saveDeviceButton = new QPushButton(zh("\xE4\xBF\x9D\xE5\xAD\x98"), this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after save button create")); // wjy: 保存按钮对象创建完成。
    m_saveDeviceButton->setGeometry(712, 424, 124, 34);
    m_saveDeviceButton->setCursor(Qt::PointingHandCursor);
    m_saveDeviceButton->setStyleSheet(primaryButtonStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after save button setup")); // wjy: 保存按钮位置、鼠标样式和主按钮样式设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before add device signal connects")); // wjy: 判断是否崩在新增设备控件信号连接。
    connect(m_deviceIpEdit, &QLineEdit::textChanged, this, &DeviceGrid::updateAddDeviceControls);
    connect(m_deviceNameEdit, &QLineEdit::textChanged, this, &DeviceGrid::updateAddDeviceControls);
    connect(m_saveDeviceButton, &QPushButton::clicked, this, &DeviceGrid::saveNewDevice);
    connect(m_cancelDeviceButton, &QPushButton::clicked, this, &DeviceGrid::cancelNewDevice);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after add device signal connects")); // wjy: 新增设备控件信号连接完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before updateAddDeviceControls")); // wjy: 判断是否崩在首次刷新新增设备控件显隐状态。
    updateAddDeviceControls();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] setupAddDeviceControls end")); // wjy: 新增设备控件初始化完整结束。
}

void DeviceGrid::setupSettingsControls()
{
    // =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] setupSettingsControls begin")); // wjy: 进入设置控件初始化，细分 Release 偶发崩溃发生在设置页的哪一步。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] status polling controls removed; realtime state is always enabled")); // wjy: 不再创建自动刷新秒数框或开关信号，旧 QSettings 值仅保留为无效历史数据。

    m_periodicDeviceDiscoveryIntervalEdit = new QLineEdit(this);
    m_periodicDeviceDiscoveryIntervalEdit->setGeometry(settingsPeriodicDeviceDiscoveryIntervalInputRect());
    m_periodicDeviceDiscoveryIntervalEdit->setValidator(new QIntValidator(1, 86400, m_periodicDeviceDiscoveryIntervalEdit)); // wjy: 周期新增同样限制为 1 秒至 24 小时的整数。
    m_periodicDeviceDiscoveryIntervalEdit->setText(QString::number(qMax(1, m_periodicDeviceDiscoveryIntervalSeconds)));
    m_periodicDeviceDiscoveryIntervalEdit->setAlignment(Qt::AlignCenter);
    m_periodicDeviceDiscoveryIntervalEdit->setPlaceholderText(QStringLiteral("60"));
    m_periodicDeviceDiscoveryIntervalEdit->setStyleSheet(QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;padding:0 10px;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QLineEdit:focus{border:1px solid #3A7BFC;}"
        "QLineEdit:disabled{background:#F5F7FA;border:1px solid #DDE3EA;color:#687384;}")); // wjy: 视觉样式与列表自动刷新秒数框保持一致。
    const auto savePeriodicDiscoveryInterval = [this] {
        if (!m_periodicDeviceDiscoveryIntervalEdit) {
            return;
        }
        int seconds = m_periodicDeviceDiscoveryIntervalEdit->text().trimmed().toInt();
        if (seconds <= 0) {
            seconds = 60;
            m_periodicDeviceDiscoveryIntervalEdit->setText(QString::number(seconds));
        }
        m_periodicDeviceDiscoveryIntervalSeconds = seconds;
        platform::AppSettings::setPeriodicDeviceDiscoveryIntervalSeconds(seconds);
        applyPeriodicDeviceDiscoverySetting(false); // wjy: 修改秒数后从当前时刻重新计算下一次扫描，不立即打断用户操作。
    };
    connect(m_periodicDeviceDiscoveryIntervalEdit, &QLineEdit::editingFinished, this, savePeriodicDiscoveryInterval);
    connect(m_periodicDeviceDiscoveryIntervalEdit, &QLineEdit::returnPressed, this, savePeriodicDiscoveryInterval);

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before batch add controls create")); // wjy: 开始创建批量新增真实控件，后续若异常可定位到设置页批量新增区域。
    m_batchSubnetEdit = new QLineEdit(this);
    m_batchSubnetEdit->setGeometry(settingsBatchSubnetInputRect());
    m_batchSubnetEdit->setText(QStringLiteral("192.168.1.* 192.168.2.* 192.168.3.*")); // wjy: 默认扫描公司常用的 1、2、3 三个网段，用户仍可按空格输入其它通配网段。
    m_batchSubnetEdit->setPlaceholderText(QStringLiteral("192.168.1.* 192.168.2.* 192.168.3.*")); // wjy: 清空输入框后仍提示完整默认格式。
    m_batchSubnetEdit->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_batchSubnetEdit->setStyleSheet(QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;padding:0 10px;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QLineEdit:focus{border:1px solid #3A7BFC;}"
        "QLineEdit:disabled{background:#F5F7FA;border:1px solid #DDE3EA;color:#687384;}")); // wjy: 复用设置页数字输入框样式，点击按钮时逐项校验 IPv4 通配网段格式。
    m_batchAddButton = new QPushButton(QStringLiteral("批量新增"), this);
    m_batchAddButton->setGeometry(settingsBatchAddButtonRect());
    m_batchAddButton->setCursor(Qt::PointingHandCursor);
    m_batchAddButton->setStyleSheet(QStringLiteral(
        "QPushButton{border:0;border-radius:4px;background:#3A7BFC;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#FFFFFF;}"
        "QPushButton:hover{background:#2F6FEF;}"
        "QPushButton:disabled{background:#C9D0DA;color:#FFFFFF;}")); // wjy: 扫描期间按钮禁用，避免用户重复启动多个批量扫描任务。
    connect(m_batchAddButton, &QPushButton::clicked, this, [this] {
        startBatchAddDevices(true); // wjy: 按钮触发属于用户主动扫描，新增设备后保持原有自动选择体验。
    });

    // =====wjy====
    m_wallpaperRotationIntervalEdit = new QLineEdit(this);
    m_wallpaperRotationIntervalEdit->setGeometry(settingsWallpaperRotationIntervalInputRect());
    m_wallpaperRotationIntervalEdit->setValidator(new QIntValidator(1, 1440, m_wallpaperRotationIntervalEdit)); // wjy: 轮换范围限制为 1 分钟至 24 小时，避免零周期忙循环或毫秒溢出。
    m_wallpaperRotationIntervalEdit->setText(QString::number(m_wallpaperRotationIntervalMinutes));
    m_wallpaperRotationIntervalEdit->setAlignment(Qt::AlignCenter);
    m_wallpaperRotationIntervalEdit->setPlaceholderText(QStringLiteral("1"));
    m_wallpaperRotationIntervalEdit->setStyleSheet(QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;padding:0 8px;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QLineEdit:focus{border:1px solid #3A7BFC;}"
        "QLineEdit:disabled{background:#F5F7FA;border:1px solid #DDE3EA;color:#687384;}"));
    const auto saveWallpaperRotationInterval = [this] {
        if (!m_wallpaperRotationIntervalEdit) {
            return;
        }
        const int minutes = qBound(1, m_wallpaperRotationIntervalEdit->text().trimmed().toInt(), 1440);
        m_wallpaperRotationIntervalMinutes = minutes;
        m_wallpaperRotationIntervalEdit->setText(QString::number(minutes)); // wjy: 空值或输入中间态失焦时回退为 1，界面与最终持久化值保持一致。
        platform::AppSettings::setDesktopWallpaperRotationIntervalMinutes(minutes);
        applyDesktopWallpaperRotationSetting(false); // wjy: 修改分钟数后从当前时刻重新计时，不额外立即切换一次。
    };
    connect(m_wallpaperRotationIntervalEdit, &QLineEdit::editingFinished, this, saveWallpaperRotationInterval);
    connect(m_wallpaperRotationIntervalEdit, &QLineEdit::returnPressed, this, saveWallpaperRotationInterval);
    // ===end====

    // =====wjy====
    m_rollbackVersionCombo = new QComboBox(this);
    m_rollbackVersionCombo->setGeometry(settingsRollbackVersionComboRect());
    m_rollbackVersionCombo->addItem(QString::fromUtf8("打开常规设置后读取"), QString()); // wjy: 构造阶段不访问网络共享，避免主窗口启动被离线目录阻塞。
    m_rollbackVersionCombo->setMaxVisibleItems(10);
    m_rollbackVersionCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_rollbackVersionCombo->setMinimumContentsLength(12);
    m_rollbackVersionCombo->setStyleSheet(QStringLiteral(
        "QComboBox{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;padding:0 9px;"
        "font-family:'Microsoft YaHei UI';font-size:13px;color:#040B18;}"
        "QComboBox:focus{border:1px solid #3A7BFC;}"
        "QComboBox:disabled{background:#F5F7FA;color:#94A3B8;}"
        "QComboBox QAbstractItemView{background:#FFFFFF;border:1px solid #DDE3EA;selection-background-color:#EAF1FF;selection-color:#040B18;}")); // wjy: 下拉框沿用设置页白底蓝色焦点样式，真实控件负责键盘选择和弹出列表。
    m_rollbackVersionCombo->setVisible(false); // wjy: 首次显隐统一交给 updateSettingsControls，构造时不会覆盖其它页面。
    // ===end====

    // =====wjy====
    const QString shortcutEditStyle = QStringLiteral(
        "QLineEdit{background:#F8FAFC;border:1px solid #CBD5E1;border-radius:4px;padding:0 8px;"
        "font-family:'Microsoft YaHei UI';font-size:13px;font-weight:600;color:#334155;}"
        "QLineEdit:focus{background:#FFFFFF;border:1px solid #3A7BFC;color:#040B18;}"
        "QLineEdit:disabled{background:#F5F7FA;border:1px solid #DDE3EA;color:#94A3B8;}"); // wjy: 快捷键输入框复用键盘页按键块视觉，聚焦时用蓝框提示正在录入。
    m_shortcutKeyEdits.clear();
    m_shortcutKeyEdits.reserve(kShortcutEditorCount);
    for (int i = 0; i < kShortcutEditorCount; ++i) {
        auto* shortcutEdit = new ShortcutKeyEdit(i, this);
        shortcutEdit->setGeometry(settingsShortcutKeyEditRect(i));
        shortcutEdit->setPlaceholderText(QStringLiteral("按快捷键"));
        shortcutEdit->setStyleSheet(shortcutEditStyle);
        shortcutEdit->setCommittedText(remoteShortcutDisplayText(i));
        shortcutEdit->commitCallback = [this](int shortcutIndex, const QString& shortcutText) {
            saveShortcutKeySetting(shortcutIndex, shortcutText);
            return remoteShortcutDisplayText(shortcutIndex); // wjy: 保存后把输入框文本同步成最终应用的快捷键显示。
        };
        m_shortcutKeyEdits.append(shortcutEdit); // wjy: 九个输入框按行号映射五项全局操作、三项远控窗口操作和删除设备操作。
    }
    // ===end====

    // =====wjy====
    m_remoteQualityConfiguration = platform::AppSettings::remoteQualityConfiguration();
    m_remoteQualityConfiguration.defaultMode = stream::RemoteQualityMode::Automatic; // wjy: 迁移到智能策略后统一保存自动模式，旧设备手动档不再控制当前 UI。
    platform::AppSettings::setRemoteQualityConfiguration(m_remoteQualityConfiguration);
    const QString qualityControlStyle = QStringLiteral(
        "QComboBox{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;padding:0 8px;"
        "font-family:'Microsoft YaHei UI';font-size:13px;color:#040B18;}"
        "QComboBox:focus{border:1px solid #3A7BFC;}"
        "QComboBox:disabled{background:#F5F7FA;color:#94A3B8;}"); // wjy: 远控画质页只保留一个默认模式下拉框，避免高级参数继续暗示固定预设可被改写。

    m_remoteQualityModeCombo = new QComboBox(this);
    m_remoteQualityModeCombo->addItem(QString::fromUtf8("智能切换"), static_cast<int>(stream::RemoteQualityMode::Automatic));
    m_remoteQualityModeCombo->setCurrentIndex(0);
    m_remoteQualityModeCombo->setEnabled(false); // wjy: 设置页改为只读策略说明，不再允许全局或单窗口手动覆盖智能画质。
    m_remoteQualityModeCombo->setStyleSheet(qualityControlStyle);
    m_remoteQualityModeCombo->setVisible(false); // wjy: 首次显隐统一交给updateSettingsControls，构造阶段不会闪到常规页上。

    // ===end====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after batch add controls create")); // wjy: 批量新增输入框和按钮创建完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before updateSettingsControls in setup")); // wjy: 判断是否崩在首次刷新设置控件显隐状态。
    // ===end====

    updateSettingsControls();
    // =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] setupSettingsControls end")); // wjy: 设置控件初始化完整结束。
    // ===end====
}

// =====wjy====
void DeviceGrid::applyDesktopWallpaperRotationSetting(bool rotateImmediately)
{
    if (!m_wallpaperRotationTimer) {
        return;
    }

    if (!m_wallpaperRotationEnabled) {
        m_wallpaperRotationTimer->stop(); // wjy: 关闭开关立即停止后续周期，不再访问共享目录。
        m_wallpaperShareProbePending = false; // wjy: 用户关闭后丢弃尚在等待的壁纸探测结果，但不影响更新服务共享同一轮探测。
        m_wallpaperShareProbeUserInitiated = false;
        m_wallpaperRotationStatusText.clear();
        if (m_wallpaperRotationIntervalEdit) {
            m_wallpaperRotationIntervalEdit->setToolTip(QString());
        }
        updateSettingsControls();
        update();
        return;
    }

    const int intervalMs = qBound(1, m_wallpaperRotationIntervalMinutes, 1440) * 60 * 1000;
    m_wallpaperRotationTimer->start(intervalMs); // wjy: 开启或修改分钟数后都从当前时刻重新计算下一轮。
    if (rotateImmediately) {
        startDesktopWallpaperRotation(true); // wjy: 用户刚开启时立即验证并切换一张，不必先等待完整周期。
    }
    updateSettingsControls();
    update();
}

void DeviceGrid::startDesktopWallpaperRotation(bool userInitiated)
{
    if (!m_wallpaperRotationEnabled || m_wallpaperRotationInProgress || m_shuttingDown) {
        return; // wjy: 开关关闭、真实壁纸任务仍在运行或退出阶段都不能发起连接测试。
    }
    if (m_wallpaperShareProbePending) {
        m_wallpaperShareProbeUserInitiated = m_wallpaperShareProbeUserInitiated || userInitiated;
        return; // wjy: 定时器和手动开启同时到达时共享当前探测，绝不为一次壁纸切换创建多个连接。
    }

    m_wallpaperShareProbePending = true;
    m_wallpaperShareProbeUserInitiated = userInitiated;
    m_wallpaperRotationStatusText = QString::fromUtf8("正在检测网盘…");
    if (m_wallpaperRotationIntervalEdit) {
        m_wallpaperRotationIntervalEdit->setToolTip(QString::fromUtf8("正在检测 192.168.1.100 的 SMB 服务"));
    }
    updateSettingsControls();
    update();
    platform::SharedStorageAvailabilityService::instance().requestProbe(); // wjy: 探测失败时到此结束，不创建壁纸线程也不访问 UNC。
}

void DeviceGrid::performDesktopWallpaperRotation(bool userInitiated)
{
    if (!m_wallpaperRotationEnabled || m_wallpaperRotationInProgress || m_shuttingDown) {
        return; // wjy: 探测返回期间用户可能关闭功能或退出，成功结果也不能越过最新状态启动线程。
    }

    m_wallpaperRotationInProgress = true;
    m_wallpaperRotationStatusText = QString::fromUtf8("正在切换…");
    update();
    const QString previousSourcePath = m_lastWallpaperSourcePath;
    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, previousSourcePath, userInitiated] {
        const platform::DesktopWallpaperApplyResult result = platform::DesktopWallpaperService::applyNextSharedImage(previousSourcePath); // wjy: 壁纸任务直接使用共享原图，不再读取本机名称或合成字符。
        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, result, userInitiated] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            grid->m_wallpaperRotationInProgress = false;
            if (result.success) {
                grid->m_lastWallpaperSourcePath = result.sourcePath; // wjy: 仅成功应用后推进游标；失败时下一周期继续尝试同一张，避免无声跳过。
                if (grid->m_wallpaperRotationEnabled) {
                    grid->m_wallpaperRotationStatusText = QString::fromUtf8("已切换：%1").arg(QFileInfo(result.sourcePath).fileName());
                    if (grid->m_wallpaperRotationIntervalEdit) {
                        grid->m_wallpaperRotationIntervalEdit->setToolTip(grid->m_wallpaperRotationStatusText);
                    }
                } else {
                    grid->m_wallpaperRotationStatusText.clear(); // wjy: 用户在后台任务结束前关闭开关时不再显示运行状态，且定时器已经停止。
                }
            } else if (grid->m_wallpaperRotationEnabled) {
                grid->m_wallpaperRotationStatusText = QString::fromUtf8("切换失败，稍后重试");
                if (grid->m_wallpaperRotationIntervalEdit) {
                    grid->m_wallpaperRotationIntervalEdit->setToolTip(result.errorMessage); // wjy: 卡片保留简短状态，悬停分钟框可查看完整共享目录或缓存错误。
                }
                if (userInitiated) {
                    QMessageBox::warning(
                        grid,
                        QString(),
                        QString::fromUtf8("桌面壁纸自动切换已开启，但首次切换失败：%1\n程序会在下一个周期继续重试。")
                            .arg(result.errorMessage)); // wjy: 首次开启失败明确告知用户，但定时失败不每分钟弹窗打断工作。
                }
            } else {
                grid->m_wallpaperRotationStatusText.clear();
            }
            grid->updateSettingsControls();
            grid->update();
        }, Qt::QueuedConnection);
    });
}

void DeviceGrid::handleSharedStorageProbeFinished(bool available)
{
    if (m_scriptFolderShareProbePending) {
        m_scriptFolderShareProbePending = false;
        if (!m_shuttingDown) {
            if (available) {
                startScriptFolderTreeLoad(); // wjy: TCP 445 成功后只登记后台目录任务，信号处理本身不访问 UNC。
            } else {
                m_scriptFolderTreeLoaded = false;
                setScriptFolderTreePlaceholder(QString::fromUtf8("脚本网盘不可用")); // wjy: 无法连接时立即结束，主界面不会进入 Windows SMB 超时。
            }
        }
    }

    if (m_wallpaperShareProbePending) {
        const bool userInitiated = m_wallpaperShareProbeUserInitiated;
        m_wallpaperShareProbePending = false;
        m_wallpaperShareProbeUserInitiated = false;
        if (m_wallpaperRotationEnabled && !m_shuttingDown) {
            if (available) {
                performDesktopWallpaperRotation(userInitiated); // wjy: 仅本轮 TCP 445 成功后创建壁纸工作线程并读取共享图片。
            } else {
                m_wallpaperRotationStatusText = QString::fromUtf8("网盘不可用，稍后重试");
                if (m_wallpaperRotationIntervalEdit) {
                    m_wallpaperRotationIntervalEdit->setToolTip(QString::fromUtf8("无法连接 192.168.1.100:445，本轮未访问共享壁纸目录。"));
                }
                if (userInitiated) {
                    QMessageBox::warning(this, QString(),
                        QString::fromUtf8("网盘连接测试未通过，已保留自动壁纸开关，程序会在下一个周期重新检测。")); // wjy: 用户主动开启时明确反馈，定时探测失败保持静默。
                }
                updateSettingsControls();
                update();
            }
        }
    }

    if (!m_rollbackShareProbePending) {
        return; // wjy: 本轮探测没有设置页等待者时，不创建历史版本枚举线程。
    }

    m_rollbackShareProbePending = false;
    if (!m_rollbackVersionCombo || m_rollbackPreparing || m_shuttingDown) {
        m_rollbackVersionBeforeProbe.clear();
        return; // wjy: 探测返回前界面已退出或回撤已经开始时，丢弃旧刷新请求。
    }
    if (available) {
        startRollbackVersionsRefresh(); // wjy: SMB 服务可达后才进入 releases 目录枚举。
        return;
    }

    m_rollbackVersionsRefreshClock.restart();
    m_rollbackVersionBeforeProbe.clear(); // wjy: 连接失败没有枚举结果可恢复，清除本轮临时选择快照。
    {
        const QSignalBlocker blocker(m_rollbackVersionCombo);
        m_rollbackVersionCombo->clear();
        m_rollbackVersionCombo->addItem(QString::fromUtf8("网盘不可用"), QString()); // wjy: 离线状态直接禁用回撤，不把连接失败转换成后台 QFile/QDir 等待。
    }
    m_rollbackVersionCombo->setToolTip(QString::fromUtf8("无法连接 192.168.1.100:445，本次没有读取共享历史版本。"));
    updateSettingsControls();
    update();
}
// ===end====

void DeviceGrid::updateAddDeviceControls()
{
    syncResponsiveLayoutState();
    if (!m_deviceIpEdit || !m_deviceNameEdit || !m_deviceMacEdit || !m_deviceRemarkEdit
        || !m_saveDeviceButton || !m_cancelDeviceButton) { // wjy: 子控件隔离测试期间不创建输入框/按钮，外部点击仍可能触发刷新函数，必须先判空。
        return; // wjy: 没有新增设备控件时直接跳过显隐/可用状态刷新，避免空指针崩溃干扰堆损坏定位。
    }

    const SettingsLayoutSnapshot layout = settingsLayoutSnapshot(
        m_settingsLocalInfoExpanded,
        m_settingsAddDeviceExpanded,
        m_settingsScrollOffset,
        m_settingsTab == SettingsTab::Keyboard); // wjy: 即使键盘页暂时隐藏新增设备控件，也不能把快捷键页偏移重新夹回常规页范围。
    m_settingsScrollOffset = layout.scrollOffset();
    const QRect ipRect = layout.scrolled(settingsAddDeviceIpEditRect(m_settingsLocalInfoExpanded));
    const QRect nameRect = layout.scrolled(settingsAddDeviceNameEditRect(m_settingsLocalInfoExpanded));
    const QRect macRect = layout.scrolled(settingsAddDeviceMacEditRect(m_settingsLocalInfoExpanded));
    const QRect remarkRect = layout.scrolled(settingsAddDeviceRemarkEditRect(m_settingsLocalInfoExpanded));
    const QRect saveRect = layout.scrolled(settingsAddDeviceSaveButtonRect(m_settingsLocalInfoExpanded));
    const QRect cancelRect = layout.scrolled(settingsAddDeviceCancelButtonRect(m_settingsLocalInfoExpanded));
    const bool addPageVisible = !m_detailPanelCollapsed
        && m_settingsSelected
        && m_settingsTab == SettingsTab::General
        && m_settingsAddDeviceExpanded
        && !m_localInfoSelected; // wjy: 收起详情栏时新增设备输入框整体隐藏，展开后沿用原展开状态恢复。
    m_deviceIpEdit->setGeometry(ipRect);
    m_deviceNameEdit->setGeometry(nameRect);
    m_deviceMacEdit->setGeometry(macRect);
    m_deviceRemarkEdit->setGeometry(remarkRect);
    m_saveDeviceButton->setGeometry(saveRect);
    m_cancelDeviceButton->setGeometry(cancelRect);
    m_deviceIpEdit->setVisible(addPageVisible && layout.fullyVisible(settingsAddDeviceIpEditRect(m_settingsLocalInfoExpanded)));
    m_deviceNameEdit->setVisible(addPageVisible && layout.fullyVisible(settingsAddDeviceNameEditRect(m_settingsLocalInfoExpanded)));
    m_deviceMacEdit->setVisible(addPageVisible && layout.fullyVisible(settingsAddDeviceMacEditRect(m_settingsLocalInfoExpanded)));
    m_deviceRemarkEdit->setVisible(addPageVisible && layout.fullyVisible(settingsAddDeviceRemarkEditRect(m_settingsLocalInfoExpanded)));
    m_saveDeviceButton->setVisible(addPageVisible && layout.fullyVisible(settingsAddDeviceSaveButtonRect(m_settingsLocalInfoExpanded)));
    m_cancelDeviceButton->setVisible(addPageVisible && layout.fullyVisible(settingsAddDeviceCancelButtonRect(m_settingsLocalInfoExpanded)));

    m_saveDeviceButton->setEnabled(!m_deviceIpEdit->text().trimmed().isEmpty());
    if (addPageVisible) {
        m_deviceIpEdit->raise();
        m_deviceNameEdit->raise();
        m_deviceMacEdit->raise();
        m_deviceRemarkEdit->raise();
        m_cancelDeviceButton->raise();
        m_saveDeviceButton->raise();
    }
}

void DeviceGrid::setupLocalInfoControls()
{
    const QString buttonStyle = QStringLiteral(
        "QPushButton{border:1px solid #DDE3EA;border-radius:4px;background:#FFFFFF;"
        "font-family:'Microsoft YaHei UI';font-size:12px;color:#040B18;}"
        "QPushButton:hover{background:#F3F7FF;}");

    for (int i = 0; i < 6; ++i) {
        auto* button = new QPushButton(zh("\xE5\xA4\x8D\xE5\x88\xB6"), this);
        button->setGeometry(localInfoCopyButtonRect(i));
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(buttonStyle);
        connect(button, &QPushButton::clicked, this, [this, i] {
            QString value;
            switch (i) {
            case 0: value = m_localDeviceInfo.name.trimmed(); break;
            case 1: value = m_localDeviceInfo.ip.trimmed(); break;
            case 2: value = m_localDeviceInfo.mac.trimmed(); break;
            case 3: value = m_localDeviceInfo.broadcastIp.trimmed(); break;
            case 4: value = m_localDeviceInfo.subnetMask.trimmed(); break;
            case 5: value = m_localDeviceInfo.gateway.trimmed(); break;
            default: break;
            }

            if (!value.isEmpty()) {
                QApplication::clipboard()->setText(value);
            }
        });
        m_localInfoCopyButtons.append(button);
    }
}

void DeviceGrid::updateLocalInfoControls()
{
    syncResponsiveLayoutState();
    const SettingsLayoutSnapshot layout = settingsLayoutSnapshot(
        m_settingsLocalInfoExpanded,
        m_settingsAddDeviceExpanded,
        m_settingsScrollOffset,
        m_settingsTab == SettingsTab::Keyboard); // wjy: 本机信息控件隐藏时仍共享当前页滚动上限，避免刷新把键盘页位置改回常规页。
    m_settingsScrollOffset = layout.scrollOffset();
    const bool visible = !m_detailPanelCollapsed
        && m_settingsSelected
        && m_settingsTab == SettingsTab::General
        && m_settingsLocalInfoExpanded; // wjy: 本机信息复制按钮只在详情栏真实可见时参与布局和点击。
    for (int i = 0; i < m_localInfoCopyButtons.size(); ++i) {
        QPushButton* button = m_localInfoCopyButtons.at(i);
        if (button) {
            const QRect buttonRect = layout.scrolled(localInfoCopyButtonRect(i));
            button->setGeometry(buttonRect);
            button->setVisible(visible && layout.fullyVisible(localInfoCopyButtonRect(i)));
            if (button->isVisible()) {
                button->raise();
            }
        }
    }
}

void DeviceGrid::refreshLocalDeviceInfo()
{
    m_localDeviceInfo = platform::DeviceInfoService::local();
    if (m_localDeviceInfo.name.trimmed().isEmpty()) {
        m_localDeviceInfo.name = platform::DeviceInfoService::localDeviceName(); // wjy: 完整枚举异常时仍保留 Windows 计算机名作为本机身份兜底。
    }
    g_localDeviceIdentityForList = m_localDeviceInfo; // wjy: 列表、搜索和批量新增从此使用完整本机 IP/MAC/名称统一判断。
}

// =====wjy====
void DeviceGrid::refreshRollbackVersions(bool forceRefresh)
{
    if (!m_rollbackVersionCombo || m_rollbackPreparing) {
        return; // wjy: 回撤任务开始后保持用户确认的目标不变，等待主程序有序退出，不再重新访问共享目录。
    }

    if (m_rollbackVersionsRefreshInProgress) {
        m_rollbackVersionsRefreshPending = m_rollbackVersionsRefreshPending || forceRefresh; // wjy: 普通重复点击直接复用正在进行的扫描；强制刷新则记录一次补扫需求。
        return;
    }
    constexpr qint64 rollbackVersionCacheMs = 30 * 1000;
    if (!forceRefresh
        && m_rollbackVersionsRefreshClock.isValid()
        && m_rollbackVersionsRefreshClock.elapsed() < rollbackVersionCacheMs) {
        return; // wjy: 30 秒内再次点击设置只显示现有结果，不重复访问共享目录中的每个版本文件。
    }

    if (m_rollbackShareProbePending) {
        return; // wjy: 设置点击或更新状态变化同时到达时，等待同一轮连接测试，不创建重复探测。
    }

    m_rollbackShareProbePending = true;
    m_rollbackVersionBeforeProbe = m_rollbackVersionCombo->currentData().toString(); // wjy: 清空加载提示前保存用户选择，连接阶段不会让最终下拉框无故跳回第一项。
    {
        const QSignalBlocker blocker(m_rollbackVersionCombo);
        m_rollbackVersionCombo->clear();
        m_rollbackVersionCombo->addItem(QString::fromUtf8("正在检测网盘…"), QString()); // wjy: 先显示轻量连接阶段，只有成功才进入历史版本后台枚举。
    }
    m_rollbackVersionCombo->setToolTip(QString::fromUtf8("正在检测 192.168.1.100 的 SMB 服务"));
    updateSettingsControls();
    update();
    platform::SharedStorageAvailabilityService::instance().requestProbe(); // wjy: 离线时最多等待异步短超时，不启动 QDir 枚举线程。
}

void DeviceGrid::startRollbackVersionsRefresh()
{
    if (!m_rollbackVersionCombo || m_rollbackPreparing || m_rollbackVersionsRefreshInProgress || m_shuttingDown) {
        return; // wjy: 连接测试返回后再次验证界面和任务状态，避免晚到结果重复启动线程。
    }

    const QString previousVersion = m_rollbackVersionBeforeProbe;
    m_rollbackVersionBeforeProbe.clear(); // wjy: 本轮枚举捕获选择快照后立即清理临时状态，下一轮重新从当前下拉框读取。
    m_rollbackVersionsRefreshInProgress = true;
    m_rollbackVersionsRefreshPending = false;
    {
        const QSignalBlocker blocker(m_rollbackVersionCombo);
        m_rollbackVersionCombo->clear();
        m_rollbackVersionCombo->addItem(QString::fromUtf8("正在读取版本…"), QString()); // wjy: 点击设置后立即显示页面和加载状态，耗时共享目录遍历不再阻塞 UI 线程。
    }
    m_rollbackVersionCombo->setToolTip(QString::fromUtf8("正在后台读取共享历史版本"));
    updateSettingsControls();
    update();

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, previousVersion] {
        QString error;
        const QStringList versions = platform::UpdateService::availableRollbackVersions(&error); // wjy: 网络目录枚举和每个版本的关键文件元数据检查全部在工作线程完成。
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, previousVersion, versions, error] {
            if (!self) {
                return;
            }

            DeviceGrid* grid = self.data();
            grid->m_rollbackVersionsRefreshInProgress = false;
            grid->m_rollbackVersionsRefreshClock.restart(); // wjy: 成功、空列表或网络错误都短时缓存，避免用户连续点击时反复等待同一共享目录。
            if (grid->m_rollbackVersionCombo && !grid->m_rollbackPreparing) {
                const QSignalBlocker blocker(grid->m_rollbackVersionCombo);
                grid->m_rollbackVersionCombo->clear();
                for (const QString& version : versions) {
                    grid->m_rollbackVersionCombo->addItem(QStringLiteral("v%1").arg(version), version); // wjy: 后台结果回到 UI 线程后再安全修改下拉框，显示文本带 v、提交数据保持三段版本号。
                }

                if (versions.isEmpty()) {
                    const QString placeholder = error.isEmpty()
                        ? QString::fromUtf8("暂无可回撤版本")
                        : QString::fromUtf8("共享历史版本不可用");
                    grid->m_rollbackVersionCombo->addItem(placeholder, QString()); // wjy: 网络不可用和没有历史版本都使用空数据项，回撤按钮保持禁用。
                    grid->m_rollbackVersionCombo->setToolTip(error);
                } else {
                    const int previousIndex = grid->m_rollbackVersionCombo->findData(previousVersion);
                    grid->m_rollbackVersionCombo->setCurrentIndex(previousIndex >= 0 ? previousIndex : 0); // wjy: 异步刷新完成后优先恢复用户原选择，否则选择最接近当前版本的第一项。
                    grid->m_rollbackVersionCombo->setToolTip(QString::fromUtf8("选择要恢复的历史版本"));
                }
            }

            const bool refreshAgain = grid->m_rollbackVersionsRefreshPending;
            grid->m_rollbackVersionsRefreshPending = false;
            grid->updateSettingsControls();
            grid->update();
            if (refreshAgain && !grid->m_rollbackPreparing) {
                grid->refreshRollbackVersions(true); // wjy: 发布等操作若发生在扫描期间，当前回调结束后再补扫一次，避免旧结果覆盖新版本状态。
            }
        }, Qt::QueuedConnection);
    });
}
// ===end====

// =====wjy====
void DeviceGrid::applySettingsControlGeometry(QWidget* control, const QRect& geometry, bool visible, bool enabled, bool raiseWhenVisible)
{
    if (!control) {
        return; // wjy: 控件尚未创建时跳过布局更新，兼容构造阶段的分批初始化顺序。
    }

    const bool effectiveVisible = visible && !m_detailPanelCollapsed; // wjy: 所有设置页真实控件共用详情栏门禁，避免紧凑窗口外仍有隐藏热区。
    control->setGeometry(geometry); // wjy: 所有真实控件沿用手绘布局计算出的矩形，滚动和缩放只在一个入口生效。
    control->setVisible(effectiveVisible);
    control->setEnabled(effectiveVisible && enabled); // wjy: 详情栏收起时同时禁用键盘焦点和鼠标输入，展开后恢复业务可用状态。
    if (raiseWhenVisible && effectiveVisible) {
        control->raise(); // wjy: 真实控件显示时覆盖手绘背景，保证鼠标命中和键盘输入仍落到控件上。
    }
}
// ===end====

void DeviceGrid::updateSettingsControls()
{
    syncResponsiveLayoutState();
// =====wjy====
    const SettingsLayoutSnapshot layout = settingsLayoutSnapshot(
        m_settingsLocalInfoExpanded,
        m_settingsAddDeviceExpanded,
        m_settingsScrollOffset,
        m_settingsTab == SettingsTab::Keyboard); // wjy: 所有真实设置控件在同一快照下计算滚动后几何和视口可见性。
    m_settingsScrollOffset = layout.scrollOffset();
    if (m_periodicDeviceDiscoveryIntervalEdit) {
        const QRect discoveryIntervalRect = layout.scrolled(settingsPeriodicDeviceDiscoveryIntervalInputRect());
        const bool discoveryIntervalVisible = m_settingsSelected
            && m_settingsTab == SettingsTab::General
            && m_periodicDeviceDiscoveryEnabled
            && layout.fullyVisible(settingsPeriodicDeviceDiscoveryIntervalInputRect());
        applySettingsControlGeometry(m_periodicDeviceDiscoveryIntervalEdit, discoveryIntervalRect,
            discoveryIntervalVisible, m_periodicDeviceDiscoveryEnabled, true);
    }

    if (m_batchSubnetEdit && m_batchAddButton) {
        const QRect batchEditRect = layout.scrolled(settingsBatchSubnetInputRect());
        const QRect batchButtonRect = layout.scrolled(settingsBatchAddButtonRect());
        const bool batchVisible = m_settingsSelected && m_settingsTab == SettingsTab::General; // wjy: 批量新增属于设置常规页功能，切到键盘页时隐藏真实控件。
        const bool batchEditVisible = batchVisible && layout.fullyVisible(settingsBatchSubnetInputRect());
        const bool batchButtonVisible = batchVisible && layout.fullyVisible(settingsBatchAddButtonRect());
        applySettingsControlGeometry(m_batchSubnetEdit, batchEditRect,
            batchEditVisible, batchVisible && !m_batchAddInProgress, true);
        applySettingsControlGeometry(m_batchAddButton, batchButtonRect,
            batchButtonVisible, batchVisible && !m_batchAddInProgress, true);
        m_batchAddButton->setText(m_batchAddInProgress ? QStringLiteral("扫描中") : QStringLiteral("批量新增"));
    }

    // =====wjy====
    if (m_wallpaperRotationIntervalEdit) {
        const QRect wallpaperIntervalRect = layout.scrolled(settingsWallpaperRotationIntervalInputRect()); // wjy: 真实分钟输入框跟随手绘常规页内容使用同一滚动偏移。
        const bool wallpaperIntervalVisible = m_settingsSelected
            && m_settingsTab == SettingsTab::General
            && m_wallpaperRotationEnabled
            && layout.fullyVisible(settingsWallpaperRotationIntervalInputRect()); // wjy: 仅在开关开启且输入框完整位于常规页视口时显示，避免遮住其它页面。
        applySettingsControlGeometry(m_wallpaperRotationIntervalEdit, wallpaperIntervalRect,
            wallpaperIntervalVisible, wallpaperIntervalVisible, true);
    }
    // ===end====

    // =====wjy====
    if (m_rollbackVersionCombo) {
        const QRect rollbackComboRect = layout.scrolled(settingsRollbackVersionComboRect());
        const bool rollbackVisible = m_settingsSelected
            && m_settingsTab == SettingsTab::General
            && layout.fullyVisible(settingsRollbackVersionComboRect());
        const bool rollbackEnabled = rollbackVisible
            && !m_rollbackPreparing
            && !m_updatePreparing
            && !m_publishPreparing
            && !m_rollbackVersionCombo->currentData().toString().isEmpty(); // wjy: 占位项没有目标数据，不能通过键盘或鼠标误发起回撤。
        applySettingsControlGeometry(m_rollbackVersionCombo, rollbackComboRect,
            rollbackVisible, rollbackEnabled, true);
    }
    // ===end====

    for (int i = 0; i < m_shortcutKeyEdits.size(); ++i) {
        QLineEdit* edit = m_shortcutKeyEdits.at(i);
        if (!edit) {
            continue;
        }
        const QRect shortcutContentRect = settingsShortcutKeyEditRect(i);
        const QRect shortcutRect = layout.scrolled(shortcutContentRect);
        const bool shortcutVisible = m_settingsSelected
            && m_settingsTab == SettingsTab::Keyboard
            && layout.fullyVisible(shortcutContentRect); // wjy: 快捷键输入框随键盘页滚动，只有完整进入视口才允许获得焦点和接收输入。
        applySettingsControlGeometry(edit, shortcutRect, shortcutVisible, shortcutVisible, true);
        if (auto* shortcutEdit = static_cast<ShortcutKeyEdit*>(edit); shortcutEdit && !shortcutEdit->hasFocus()) {
            shortcutEdit->setCommittedText(remoteShortcutDisplayText(i)); // wjy: 控件未录入时跟随当前保存设置刷新显示。
        }
    }

    // =====wjy====
    const bool qualityVisible = m_settingsSelected && m_settingsTab == SettingsTab::RemoteControl;
    if (m_remoteQualityModeCombo) {
        applySettingsControlGeometry(
            m_remoteQualityModeCombo,
            settingsRemoteQualityControlRect(0),
            qualityVisible,
            false,
            true); // wjy: 简化页只管理默认模式下拉框，其余预设值作为只读说明绘制，不再创建复杂参数控件。
    }
    // ===end====

// ===end====
}

// =====wjy====
void DeviceGrid::saveRemoteQualitySettingsFromControls()
{
    if (!m_remoteQualityModeCombo) {
        return; // wjy: 构造阶段下拉框尚未创建时不写入默认模式。
    }

    stream::RemoteQualityConfiguration configuration = m_remoteQualityConfiguration;
    configuration.defaultMode = stream::RemoteQualityMode::Automatic; // wjy: 智能策略固定为唯一入口，保留配置对象仅兼容现有在线质量管线。
    m_remoteQualityConfiguration = stream::normalizedRemoteQualityConfiguration(configuration); // wjy: 只接受四种可持久化默认模式，其余FPS/分辨率字段恢复代码内置预设。
    platform::AppSettings::setRemoteQualityConfiguration(m_remoteQualityConfiguration);

    const QVector<QPointer<RemoteDesktopWindow>> windows = openedRemoteWindows();
    for (const QPointer<RemoteDesktopWindow>& window : windows) {
        if (window) {
            window->setGlobalQualityConfiguration(m_remoteQualityConfiguration); // wjy: 已打开窗口无需重连，先更新全局策略快照，后续实时协议直接发送最新请求。
        }
    }
    requestRemoteQualityEvaluation(); // wjy: 全局配置保存后合并所有窗口信号，在当前事件循环结束时只执行一次协调计算。
    update();
}
// ===end====

// =====wjy====
void DeviceGrid::registerRemoteQualityWindow(RemoteDesktopWindow* window)
{
    if (!window) {
        return;
    }
    window->setGlobalQualityConfiguration(m_remoteQualityConfiguration); // wjy: 新窗口读取全局默认模式；设备没有已保存选择时仍从“自动”固定预设开始。
    connect(window, &RemoteDesktopWindow::remoteQualityInputsChanged,
        this, &DeviceGrid::requestRemoteQualityEvaluation); // wjy: 最小化/恢复/模式切换即时生效，不等待下一次1秒采样。
    connect(window, &QObject::destroyed, this, [this, window] {
        m_remoteQualityCoordinator.removeWindow(reinterpret_cast<uintptr_t>(window)); // wjy: 删除该窗口滞回历史，之后同设备重开从全局基线重新开始。
        requestRemoteQualityEvaluation();
    });
    requestRemoteQualityEvaluation();
}

void DeviceGrid::requestRemoteQualityEvaluation()
{
    if (m_shuttingDown || m_remoteQualityEvaluationQueued) {
        return;
    }
    m_remoteQualityEvaluationQueued = true; // wjy: 同一批20个窗口的显示/最小化事件最多排队一个Qt任务，防止协调逻辑反向放大事件队列。
    QTimer::singleShot(0, this, [this] {
        m_remoteQualityEvaluationQueued = false;
        evaluateRemoteQuality();
    });
}

void DeviceGrid::evaluateRemoteQuality()
{
    if (m_shuttingDown) {
        return;
    }
    try {
        const QVector<QPointer<RemoteDesktopWindow>> windows = openedRemoteWindows();
        std::vector<RemoteQualityWindowMetrics> metrics;
        metrics.reserve(static_cast<std::size_t>(windows.size()));
        for (const QPointer<RemoteDesktopWindow>& window : windows) {
            if (window) {
                RemoteQualityWindowMetrics snapshot = window->remoteQualityMetrics();
                snapshot.active = snapshot.visible && !snapshot.minimized && window->isActiveWindow(); // wjy: 只把Qt确认的真实焦点窗口标记为active，协调器据此授予唯一高质量角色。
                metrics.push_back(snapshot); // wjy: 只在Qt线程附加真实焦点身份，不跨线程访问原生WebRTC对象。
            }
        }
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const std::vector<RemoteQualityDecision> decisions = m_remoteQualityCoordinator.evaluate(
            m_remoteQualityConfiguration,
            metrics,
            nowMs);
        for (const RemoteQualityDecision& decision : decisions) {
            auto* window = reinterpret_cast<RemoteDesktopWindow*>(decision.windowId);
            if (window && windows.contains(QPointer<RemoteDesktopWindow>(window))) {
                window->applyRemoteQualityDecision(decision); // wjy: 画质按真实焦点窗口统一下发，音频由每个窗口自己的标题栏按钮控制。
            }
        }
        if (!windows.isEmpty()
            && (m_lastRemoteResourceDiagnosticAtMs == 0
                || nowMs - m_lastRemoteResourceDiagnosticAtMs >= 30000)) {
            m_lastRemoteResourceDiagnosticAtMs = nowMs;
            const RemoteViewerLifecycleManager::Diagnostics lifecycle = m_remoteViewerLifecycleManager
                ? m_remoteViewerLifecycleManager->diagnostics()
                : RemoteViewerLifecycleManager::Diagnostics{};
            writeDeviceGridStartupLog(QStringLiteral("[wjy-remote-resource] viewers=%1 active_starts=%2 waiting_starts=%3 cleanup_tasks=%4 background_tasks=%5 lifecycle_threads=%6 %7")
                .arg(windows.size())
                .arg(lifecycle.activeViewerStarts)
                .arg(lifecycle.waitingViewerStarts)
                .arg(lifecycle.cleanupTasks)
                .arg(lifecycle.backgroundTasks)
                .arg(lifecycle.workerThreads)
                .arg(currentProcessResourceSummary()));
            for (const QPointer<RemoteDesktopWindow>& window : windows) {
                if (window) {
                    writeDeviceGridStartupLog(QStringLiteral("[wjy-remote-window] %1")
                        .arg(window->remoteResourceDiagnosticSummary())); // wjy: 每窗口单行记录队列硬上限、实际画质和D3D失败原因，便于崩溃前定位唯一异常会话。
                }
            }
        }
    } catch (const std::exception& exception) {
        writeDeviceGridStartupLog(QStringLiteral("[wjy-quality] coordinator exception=%1")
            .arg(QString::fromUtf8(exception.what()))); // wjy: 分配或计算异常被限制在本轮协调，不能越过Qt定时器入口导致整个程序退出。
    } catch (...) {
        writeDeviceGridStartupLog(QStringLiteral("[wjy-quality] coordinator unknown exception")); // wjy: 未知异常同样降级为诊断日志，下一秒仍可继续重试。
    }
}
// ===end====

// =====wjy====
void DeviceGrid::saveShortcutKeySetting(int shortcutIndex, const QString& shortcutText)
{
    QKeySequence shortcut = shortcutSequenceFromText(shortcutText);
    if (shortcut.isEmpty()) {
        shortcut = remoteShortcutForIndex(shortcutIndex); // wjy: 空输入不覆盖现有设置，避免用户点一下再点走导致快捷键丢失。
    }
    setRemoteShortcutForIndex(shortcutIndex, shortcut); // wjy: 回车或失焦后立即写入 QSettings，后续主窗口和远程窗口马上按新快捷键判断。
    registerGlobalShortcuts();
    update();
}
// ===end====

// =====wjy====
void DeviceGrid::applyPeriodicDeviceDiscoverySetting(bool scanImmediately)
{
    if (!m_periodicDeviceDiscoveryTimer) {
        return;
    }

    if (m_periodicDeviceDiscoveryEnabled) {
        const int intervalMs = qMax(1, m_periodicDeviceDiscoveryIntervalSeconds) * 1000;
        m_periodicDeviceDiscoveryTimer->start(intervalMs);
        if (scanImmediately) {
            startBatchAddDevices(false); // wjy: 用户刚开启时立即检查一次，之后再按默认 60 秒或用户输入周期运行。
        }
    } else {
        m_periodicDeviceDiscoveryTimer->stop();
    }
    updateSettingsControls();
}
// ===end====

// =====wjy====
void DeviceGrid::applyHideLocalDeviceSetting(bool revealLocalDeviceIfMissing)
{
    g_hideLocalDeviceFromList = m_hideLocalDeviceEnabled; // wjy: 所有可见行入口在下一次计算时立即使用最新开关值。
    if (g_localDeviceIdentityForList.name.trimmed().isEmpty()) {
        g_localDeviceIdentityForList.name = platform::DeviceInfoService::localDeviceName(); // wjy: 500ms 本机信息尚未完成时仍可按计算机名即时隐藏。
    }

    bool localDeviceAdded = false;
    if (!m_hideLocalDeviceEnabled && revealLocalDeviceIfMissing) {
        if (m_localDeviceInfo.ip.trimmed().isEmpty()) {
            refreshLocalDeviceInfo(); // wjy: 用户明确关闭隐藏时同步读取本机 IP/MAC，确保能够立即创建可显示记录。
        }
        if (localDeviceCatalogIndex() < 0 && !m_localDeviceInfo.ip.trimmed().isEmpty()) {
            DeviceEntry localDevice;
            localDevice.name = m_localDeviceInfo.name.trimmed().isEmpty()
                ? m_localDeviceInfo.ip.trimmed()
                : m_localDeviceInfo.name.trimmed(); // wjy: 本机名称不可用时沿用目录统一的 IP 展示兜底。
            localDevice.ip = m_localDeviceInfo.ip.trimmed();
            localDevice.mac = m_localDeviceInfo.mac.trimmed();
            localDevice.broadcastIp = m_localDeviceInfo.broadcastIp.trimmed();
            localDevice.remark = QString::fromUtf8("本机");
            localDeviceAdded = g_deviceCatalog.addDevice(std::move(localDevice)); // wjy: 只有目录中确实没有本机实体时才补回，重复 IP 仍由 DeviceCatalog 拒绝。
            if (localDeviceAdded) {
                saveDevices(); // wjy: 用户关闭隐藏后补回的本机记录需要跨启动保留，下一次启动无需重新发现。
            }
        }
    }

    pruneHiddenDeviceSelections(); // wjy: 开启时移除隐藏本机选择，关闭时把无主选择恢复到第一台未隐藏设备。
    if (firstUnhiddenDeviceIndex() < 0) {
        m_settingsSelected = true;
        m_remoteAssistSelected = false;
        m_localInfoSelected = false;
        m_currentDeviceName.clear();
        m_previousDeviceName.clear(); // wjy: 本机是唯一设备且被隐藏时保持设置页可操作，不让详情页引用不可见设备。
    }
    m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset, maxDeviceListScrollOffset());
    updateRealtimeConfiguredDevices(); // wjy: 隐藏本机后实时状态来源白名单同步排除它，关闭时立即恢复。
    if (localDeviceAdded) {
        m_deviceStatuses.remove(m_localDeviceInfo.ip.trimmed()); // wjy: 新补回本机等待后续实时快照，不继承历史缓存状态。
    }
    updateSettingsControls();
    updateAddDeviceControls();
    updateLocalInfoControls();
    update();
}
// ===end====

void DeviceGrid::startBatchAddDevices(bool userInitiated)
{
// =====wjy====
    if (!m_batchSubnetEdit || m_batchAddInProgress || m_shuttingDown) {
        return; // wjy: 控件未创建、已有扫描或程序正在退出时不再启动新任务。
    }

    QString subnetText = m_batchSubnetEdit->text().trimmed();
    QStringList subnetPatterns = batchSubnetPatterns(subnetText);
    if (subnetPatterns.isEmpty()) {
        m_batchSubnetEdit->setText(QStringLiteral("192.168.1.* 192.168.2.* 192.168.3.*")); // wjy: 输入为空时恢复与初始界面一致的三个默认网段。
        if (userInitiated) {
            m_batchSubnetEdit->selectAll();
            m_batchSubnetEdit->setFocus(Qt::MouseFocusReason);
            return;
        }
        subnetText = m_batchSubnetEdit->text().trimmed();
        subnetPatterns = batchSubnetPatterns(subnetText); // wjy: 周期任务遇到空输入时静默恢复默认网段并继续本轮，不抢占用户焦点。
    }

    QStringList scanIps;
    QSet<QString> seenScanIps;
    for (const QString& pattern : subnetPatterns) {
        if (!isWildcardSubnetPattern(pattern)) {
            qWarning().noquote() << QStringLiteral("[batch-add] invalid subnet=%1 all=%2").arg(pattern, subnetText);
            if (userInitiated) {
                m_batchSubnetEdit->selectAll();
                m_batchSubnetEdit->setFocus(Qt::MouseFocusReason); // wjy: 只有手动扫描才把焦点移到错误输入，后台周期检查不打断当前操作。
            }
            return;
        }
        for (const QString& ip : wildcardSubnetScanIps(pattern)) {
            if (seenScanIps.contains(ip)) {
                continue;
            }
            seenScanIps.insert(ip);
            scanIps.append(ip); // wjy: 已存在设备也继续扫描，用返回的 MAC 补齐旧记录，避免远程开机时仍提示未填写 MAC。
        }
    }
    if (scanIps.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[batch-add] no new ip to scan subnet=%1").arg(subnetText);
        return;
    }

    m_batchAddInProgress = true;
    updateSettingsControls();
    qWarning().noquote() << QStringLiteral("[batch-add] scan begin subnet=%1 count=%2")
        .arg(subnetText)
        .arg(scanIps.size());

    platform::DeviceInfo localDeviceIdentity = m_localDeviceInfo;
    if (localDeviceIdentity.name.trimmed().isEmpty()) {
        localDeviceIdentity.name = platform::DeviceInfoService::localDeviceName(); // wjy: 扫描可能早于延迟网卡枚举，至少携带本机设备名供结果过滤。
    }
    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, scanIps, userInitiated, localDeviceIdentity] {
        QVector<BatchAddResult> results;
        std::mutex resultMutex;
        std::atomic_int nextIndex = 0;
        const int workerCount = std::max(1, std::min<int>(16, scanIps.size()));
        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (int worker = 0; worker < workerCount; ++worker) {
            workers.emplace_back([&, worker] {
                while (true) {
                    const int index = nextIndex.fetch_add(1);
                    if (index >= scanIps.size()) {
                        break;
                    }

                    const QString ip = scanIps.at(index);
                    const platform::DeviceStatusInfo info = platform::DeviceStatusService::query(ip, 49101, 250);
                    if (info.state == platform::DevicePresenceState::Offline
                        || info.state == platform::DevicePresenceState::Unknown) {
                        continue; // wjy: 只有状态服务有响应的远端才视为可批量新增设备。
                    }
                    if (info.localIp.trimmed() != ip || info.mac.trimmed().isEmpty()) {
                        qWarning().noquote() << QStringLiteral("[batch-add] skip weak match ip=%1 remoteIp=%2 mac=%3")
                            .arg(ip)
                            .arg(info.localIp.trimmed())
                            .arg(info.mac.trimmed()); // wjy: 必须拿到 FSRemote 状态服务返回的本机 IP 和 MAC，避免把无效端口响应误加成设备。
                        continue;
                    }

                    BatchAddResult result;
                    result.ip = ip;
                    result.name = info.deviceName.trimmed().isEmpty()
                        ? ip
                        : info.deviceName.trimmed(); // wjy: 设备名优先使用远端电脑名，取不到时用 IP 兜底，不再使用登录账户名。
                    result.mac = info.mac.trimmed();
                    result.broadcastIp = info.broadcastIp.trimmed();
                    result.status = info; // wjy: 扫描线程把在线/占用、会话和脚本状态完整带回主线程，避免只留下布尔在线结论。

                    std::lock_guard lock(resultMutex);
                    results.append(result);
                }
            });
        }

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        if (!self) {
            return; // wjy: 窗口关闭后不再回到 UI 线程追加设备。
        }

        QMetaObject::invokeMethod(self, [self, results = std::move(results), userInitiated, localDeviceIdentity]() mutable {
            if (!self) {
                return; // wjy: queued 回调执行前窗口可能已经销毁。
            }

            DeviceGrid* grid = self.data();
            const bool wasEmpty = firstUnhiddenDeviceIndex() < 0; // wjy: 目录中只有隐藏本机时，新增第一台远端仍按首个未隐藏设备初始化详情。
            int firstAddedIndex = -1;
            int addedCount = 0;
            int updatedCount = 0;
            QSet<QString> addedIps;
            QHash<QString, platform::DeviceStatusInfo> discoveredStatuses; // wjy: 本轮每个命中 IP 只保留一份完整 TCP 校准结果，目录更新完成后统一应用。
            for (const BatchAddResult& result : results) {
                const QString ip = result.ip.trimmed();
                if (ip.isEmpty() || addedIps.contains(ip)) {
                    continue; // wjy: UI 线程最终追加前再次去重，防止扫描期间用户手动新增同一 IP。
                }
                if (grid->m_hideLocalDeviceEnabled
                    && (batchAddResultMatchesLocal(result, localDeviceIdentity)
                        || batchAddResultMatchesLocal(result, grid->m_localDeviceInfo))) {
                    continue; // wjy: 开关开启期间批量或周期发现命中本机时直接跳过，不写入目录也不显示在列表中。
                }

                const int existingIndex = deviceIndexForIp(ip);
                if (existingIndex >= 0) {
                    bool deviceUpdated = false;
                    DeviceEntry existingDevice = g_devices.at(existingIndex); // wjy: 先复制稳定实体，修改完成后再通过目录按 ID 提交，禁止直接写展示数组。
                    if (existingDevice.mac.trimmed().isEmpty() && !result.mac.trimmed().isEmpty()) {
                        existingDevice.mac = result.mac.trimmed(); // wjy: 批量扫描命中旧设备时补齐 MAC，让后续远程开机不再因为旧记录为空而被拦截。
                        deviceUpdated = true;
                    }
                    if (existingDevice.broadcastIp.trimmed().isEmpty() && !result.broadcastIp.trimmed().isEmpty()) {
                        existingDevice.broadcastIp = result.broadcastIp.trimmed(); // wjy: 同步保存广播地址，远程开机代理可直接复用扫描到的网段信息。
                        deviceUpdated = true;
                    }
                    discoveredStatuses.insert(ip, result.status); // wjy: 已存在设备也把本次 TCP 结果交给下面的统一归并步骤，不能绕过 TTL 直接标记在线。
                    addedIps.insert(ip);
                    if (deviceUpdated && g_deviceCatalog.updateDevice(existingDevice.id, std::move(existingDevice))) {
                        ++updatedCount;
                    }
                    continue;
                }

                const QString name = result.name.trimmed().isEmpty() ? ip : result.name.trimmed();
                DeviceEntry newDevice;
                newDevice.name = name;
                newDevice.ip = ip;
                newDevice.mac = result.mac.trimmed();
                newDevice.broadcastIp = result.broadcastIp.trimmed();
                newDevice.remark = QStringLiteral("批量增加");
                if (!g_deviceCatalog.addDevice(std::move(newDevice))) {
                    continue; // wjy: 目录统一拒绝重复 IP 或非法地址，扫描回调不再直接向权威容器追加记录。
                }
                discoveredStatuses.insert(ip, result.status); // wjy: 新设备加入白名单后再应用真实 Online/Busy 状态，后续由 UDP 心跳或校准 TTL 自动修正。
                addedIps.insert(ip);
                if (firstAddedIndex < 0) {
                    firstAddedIndex = g_deviceCatalog.deviceIndexForIp(ip); // wjy: 新实体加入后重新解析展示位置，下标只用于本次选择和动画。
                }
                ++addedCount;
            }

            if (addedCount > 0 || updatedCount > 0) {
                saveDevices();
                grid->updateRealtimeConfiguredDevices(); // wjy: 批量新增完成后立即允许这些 IP 的实时广播进入，不再自动对全部设备做 TCP 状态刷新。
            }

            // =====wjy====
            const bool realtimeAvailable = grid->m_realtimeStateService
                && grid->m_realtimeStateService->isRunning(); // wjy: 批量扫描与标题栏刷新使用同一实时服务可用性判定。
            const platform::DeviceStatusResultSink statusSink =
                platform::deviceStatusResultSink(realtimeAvailable); // wjy: 统一选择实时归并或旧版缓存，禁止扫描路径自行决定状态生命周期。
            for (auto it = discoveredStatuses.cbegin(); it != discoveredStatuses.cend(); ++it) {
                if (deviceIndexForIp(it.key()) < 0) {
                    continue; // wjy: 扫描期间被目录同步删除的设备不再接收迟到状态，避免旧 IP 重新污染界面。
                }
                if (statusSink == platform::DeviceStatusResultSink::RealtimeReducer) {
                    grid->m_realtimeStateService->applyManualCalibration(it.key(), it.value()); // wjy: TCP 49101 结果进入与 UDP 心跳相同的队列，受 15 秒校准 TTL 和新鲜快照优先级约束。
                } else {
                    grid->m_deviceStatuses.insert(it.key(), it.value().state); // wjy: 实时服务不可用时保留真实 Online/Busy，而不是无条件写成 Online。
                }
            }
            // ===end====

            if (addedCount > 0 && userInitiated) {
                grid->m_remoteAssistSelected = false;
                grid->m_localInfoSelected = false;
                grid->m_settingsSelected = false;
                grid->m_deviceGroupExpanded = true;
                grid->clearBottomActionHover();
                grid->setDesktopHoverActive(false);
                grid->m_selectedDeviceIndexes.clear();
                grid->m_selectedDeviceIndexes.insert(firstAddedIndex);
                grid->m_selectionAnchorDeviceIndex = firstAddedIndex;
                if (wasEmpty) {
                    grid->m_selectedDeviceIndex = firstAddedIndex;
                    grid->m_previousDeviceIndex = firstAddedIndex;
                    grid->m_currentDeviceName = deviceDisplayName(g_devices.at(firstAddedIndex));
                    grid->m_previousDeviceName = grid->m_currentDeviceName;
                    grid->loadScriptUiStateForDevice(grid->currentScriptUiDeviceIp()); // wjy: 批量新增第一台设备时初始化它自己的空脚本 UI。
                    if (grid->m_detailAnimationTimer) {
                        grid->m_detailAnimationTimer->stop();
                    }
                } else {
                    grid->startDeviceSwitchAnimation(firstAddedIndex, deviceDisplayName(g_devices.at(firstAddedIndex)));
                }
            }

            grid->m_batchAddInProgress = false;
            grid->updateSettingsControls();
            grid->updateAddDeviceControls();
            grid->updateLocalInfoControls();
            grid->update();
            qWarning().noquote() << QStringLiteral("[batch-add] scan end source=%1 found=%2 added=%3 updated=%4")
                .arg(userInitiated ? QStringLiteral("manual") : QStringLiteral("periodic"))
                .arg(results.size())
                .arg(addedCount)
                .arg(updatedCount);
        }, Qt::QueuedConnection);
    });
// ===end====
}

// =====wjy====
QString DeviceGrid::nextDefaultDeviceGroupName() const
{
    int suffix = 1; // wjy: 默认从“默认分组1”开始寻找，保持原有空白区建组命名习惯。
    QString candidate;
    do {
        candidate = QString::fromUtf8("默认分组%1").arg(suffix++); // wjy: 每次递增后检查完整名称，已有编号不会被重复使用。
    } while (g_deviceGroupNames.contains(candidate));
    return candidate;
}

int DeviceGrid::createDefaultDeviceGroup()
{
    return g_deviceCatalog.addGroup(nextDefaultDeviceGroupName(), {}, true); // wjy: 目录一次创建名称、稳定 UUID 和展开状态，三个字段不再由 UI 分别追加。
}

bool DeviceGrid::assignDevicesToGroup(const QVector<int>& deviceIndexes, int groupIndex)
{
    if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return false; // wjy: 菜单关闭前若目标分组失效，禁止把设备写入错误下标。
    }

    const QString groupName = g_deviceGroupNames.at(groupIndex).trimmed(); // wjy: group 名继续写入兼容字段，旧界面排序和显示逻辑保持不变。
    const QString groupId = g_deviceGroupIds.at(groupIndex); // wjy: groupId 是真正的稳定归属，重命名后仍指向同一分组。
    bool changed = !g_deviceGroupExpandedStates.at(groupIndex); // wjy: 选择已有折叠组也属于状态变化，需要持久化展开状态。
    g_deviceCatalog.setGroupExpanded(groupId, true); // wjy: 通过稳定分组 ID 展开目标组，排序变化后不会写错其它分组。

    QSet<int> assignedIndexes; // wjy: 菜单目标可能重复，确保每台有效设备最多处理一次。
    QVector<QString> deviceIds;
    for (int deviceIndex : deviceIndexes) {
        if (deviceIndex < 0 || deviceIndex >= g_devices.size() || assignedIndexes.contains(deviceIndex)) {
            continue; // wjy: 忽略菜单打开后已失效的下标和重复下标，不影响其它设备。
        }
        assignedIndexes.insert(deviceIndex);

        const DeviceEntry& device = g_devices.at(deviceIndex);
        if (device.group.trimmed() == groupName && device.groupId == groupId) {
            continue; // wjy: 设备已经属于目标稳定分组时保持数据不变，只在后续执行展开和定位。
        }
        deviceIds.append(device.id); // wjy: 从当前展示下标提取稳定设备 ID，真正归组不再依赖数组位置。
    }
    changed = g_deviceCatalog.assignDevicesToGroup(deviceIds, groupId) || changed; // wjy: 目录批量写入 groupId，并同步维护旧界面仍读取的 group 名。

    if (changed) {
        saveDevices(); // wjy: 所有目标处理完后只保存一次，避免多选设备逐台写文件和触发同步。
    }
    return true;
}

void DeviceGrid::revealDeviceGroup(int groupIndex, bool beginRename)
{
    if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return; // wjy: 分组下标失效时不改变侧栏滚动或编辑状态。
    }

    if (m_renamingDeviceGroupIndex >= 0 && m_renamingDeviceGroupIndex != groupIndex) {
        finishDeviceGroupRename(true); // wjy: 定位新目标前提交旧分组编辑，避免同一个输入框残留两个分组下标。
    }
    finishDeviceRename(true); // wjy: 分组定位会改变设备行位置，先结束设备名编辑以免输入框悬浮在错误行。
    m_deviceGroupExpanded = true; // wjy: 若整个设备列表被收起，操作后自动打开以展示目标分组。
    g_deviceCatalog.setGroupExpanded(g_deviceGroupIds.at(groupIndex), true); // wjy: 目标组始终展开，稳定 ID 保证视觉重排后仍命中原分组。

    const int rowIndex = visualRowIndexForGroupIndex(groupIndex, m_deviceStatuses); // wjy: 设备归组或在线状态变化都会重排行，使用当前状态缓存重新解析目标分组位置。
    if (rowIndex < 0) {
        update();
        return;
    }

    const QRect viewport = deviceListViewportRect(true); // wjy: 以展开后的真实侧栏视口计算目标位置。
    const int requestedOffset = visibleDeviceRowRect(rowIndex).top() - viewport.top(); // wjy: 优先把目标分组标题对齐到视口顶部，便于用户确认归组结果。
    m_deviceListScrollOffset = qBound(0, requestedOffset, maxDeviceListScrollOffset()); // wjy: 最后一个分组剩余内容不足一屏时夹紧到最大偏移，仍保证标题可见。
    update(); // wjy: 先重绘展开与滚动结果，再按最终屏幕坐标摆放编辑框。

    if (beginRename) {
        beginDeviceGroupRename(groupIndex); // wjy: 新建组直接进入行内重命名，现有函数会填入默认名并全选获得焦点。
    }
}
// ===end====

void DeviceGrid::beginDeviceGroupRename(int groupIndex)
{
// =====wjy====
    if (!m_deviceGroupNameEdit || groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return; // wjy: 输入框不存在或分组下标无效时，不进入编辑状态。
    }
    finishDeviceRename(true);

    const int rowIndex = visualRowIndexForGroupIndex(groupIndex, m_deviceStatuses); // wjy: 按当前在线优先快照找到分组在左侧列表中的视觉行号。
    if (rowIndex < 0) {
        return;
    }

    const QRect rowRect = scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset); // wjy: 复用分组滚动后的可见行矩形，让输入框贴在当前屏幕位置。
    m_renamingDeviceGroupIndex = groupIndex; // wjy: 记录正在编辑的分组，绘制时隐藏底层文字。
    m_deviceGroupNameEdit->setGeometry(groupNameEditRect(rowRect.y())); // wjy: 输入框和分组显示文字共用十字符宽度，并从图标右侧文字起点开始。
    m_deviceGroupNameEdit->setText(g_deviceGroupNames.at(groupIndex)); // wjy: 把当前分组名放进输入框，方便直接修改。
    m_deviceGroupNameEdit->selectAll(); // wjy: 双击后默认全选，用户可以直接输入新名字覆盖。
    m_deviceGroupNameEdit->show();
    m_deviceGroupNameEdit->raise();
    m_deviceGroupNameEdit->setFocus(Qt::MouseFocusReason); // wjy: 双击后立刻获得焦点，可以直接键入。
    update(rowRect);
// ===end====
}

void DeviceGrid::finishDeviceGroupRename(bool saveText)
{
// =====wjy====
    if (!m_deviceGroupNameEdit || m_renamingDeviceGroupIndex < 0) {
        return; // wjy: 没有正在编辑的分组时直接返回，防止 editingFinished 重复触发。
    }

    const int groupIndex = m_renamingDeviceGroupIndex; // wjy: 先保存下标，后面会清空编辑状态。
    const int rowIndex = visualRowIndexForGroupIndex(groupIndex, m_deviceStatuses); // wjy: 提交分组名称前按最新状态排序定位原编辑行。
    const QRect rowRect = rowIndex >= 0 ? scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset) : QRect();
    if (saveText && groupIndex >= 0 && groupIndex < g_deviceGroupNames.size()) {
        const QString oldName = g_deviceGroupNames.at(groupIndex).trimmed();
        const QString newName = m_deviceGroupNameEdit->text().trimmed();

        bool duplicated = false;
        for (int i = 0; i < g_deviceGroupNames.size(); ++i) {
            if (i != groupIndex
                && g_deviceGroupNames.at(i).trimmed() == newName) {
                duplicated = true;
                break;
            }
        }

        if (!newName.isEmpty() && !duplicated) {
            const QString groupId = g_deviceGroupIds.value(groupIndex);
            if (g_deviceCatalog.renameGroup(groupId, newName)) {
                saveDevices(); // wjy: 目录按稳定 groupId 重命名，并统一更新组内设备的兼容 group 名后只保存一次。
            }
        } else {
            // 空名字或者名字重复时恢复原名称
            m_deviceGroupNameEdit->setText(oldName);
        }
    }

    m_renamingDeviceGroupIndex = -1; // wjy: 清空编辑状态。
    m_deviceGroupNameEdit->hide(); // wjy: 隐藏输入框，恢复普通分组行显示。
    if (rowRect.isValid()) {
        update(rowRect);
    } else {
        update();
    }
// ===end====
}

bool DeviceGrid::beginDeviceRename(int deviceIndex)
{
// =====wjy====
    if (!m_deviceListNameEdit || deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return false;
    }
    finishDeviceGroupRename(true);

    const int rowIndex = visualRowIndexForDeviceIndex(deviceIndex, m_deviceStatuses); // wjy: 在线优先可能改变设备位置，开始编辑时按当前状态解析行号。
    if (rowIndex < 0) {
        return false;
    }

    const QVector<DeviceListRow> rows = visibleDeviceRows(m_deviceStatuses); // wjy: 编辑框位置、缩进和绘制使用同一份在线优先自然排序快照。
    if (rowIndex >= rows.size()) {
        return false;
    }

    const DeviceListRow& row = rows.at(rowIndex);
    const QRect rowRect = scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset);
    m_renamingDeviceIndex = deviceIndex;
    m_deviceListNameEdit->setGeometry(deviceNameEditRect(rowRect.y(), row.groupIndex >= 0));
    m_deviceListNameEdit->setText(deviceDisplayName(g_devices.at(deviceIndex)));
    m_deviceListNameEdit->selectAll();
    m_deviceListNameEdit->show();
    m_deviceListNameEdit->raise();
    m_deviceListNameEdit->setFocus(Qt::MouseFocusReason);
    update(rowRect);
    return true;
// ===end====
}

void DeviceGrid::finishDeviceRename(bool saveText)
{
// =====wjy====
    if (!m_deviceListNameEdit || m_renamingDeviceIndex < 0) {
        return;
    }

    const int deviceIndex = m_renamingDeviceIndex;
    const int rowIndex = visualRowIndexForDeviceIndex(deviceIndex, m_deviceStatuses); // wjy: 结束编辑时按最新在线状态重新定位需要刷新的设备行。
    const QRect rowRect = rowIndex >= 0 ? scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset) : QRect();
    if (saveText && deviceIndex >= 0 && deviceIndex < g_devices.size()) {
        applyDeviceRename(deviceIndex, m_deviceListNameEdit->text().trimmed());
    }

    m_renamingDeviceIndex = -1;
    m_deviceListNameEdit->hide();
    if (rowRect.isValid()) {
        update(rowRect);
    } else {
        update();
    }
// ===end====
}

void DeviceGrid::saveNewDevice()
{
    const QString ip = m_deviceIpEdit->text().trimmed();
    const QString name = m_deviceNameEdit->text().trimmed().isEmpty()
        ? ip
        : m_deviceNameEdit->text().trimmed();
    const QString mac = m_deviceMacEdit->text().trimmed();
    if (ip.isEmpty()) {
        updateAddDeviceControls();
        return;
    }
    if (m_hideLocalDeviceEnabled
        && deviceIdentityMatchesLocal(name, ip, mac, g_localDeviceIdentityForList)) {
        updateAddDeviceControls();
        return; // wjy: 隐藏本机开启时手动新增同样不能把本机重新带回列表，关闭开关后由统一补回逻辑处理。
    }

    const bool addingFirstDevice = firstUnhiddenDeviceIndex() < 0; // wjy: 目录中只有隐藏本机时，手动添加远端仍属于第一台未隐藏设备。
    DeviceEntry newDevice;
    newDevice.name = name;
    newDevice.ip = ip;
    newDevice.mac = mac;
    newDevice.remark = m_deviceRemarkEdit->text().trimmed();
    if (!g_deviceCatalog.addDevice(std::move(newDevice))) {
        updateAddDeviceControls();
        return; // wjy: 目录统一校验 IPv4 和重复 IP，手动新增失败时不修改任何展示状态。
    }
    saveDevices();
    updateRealtimeConfiguredDevices(); // wjy: 手动新增后把新 IP 加入广播白名单，下一份心跳即可实时显示状态。
    m_deviceStatuses.remove(ip);
    m_deviceUpdateAvailability.remove(ip); // wjy: 新增同 IP 记录时清掉旧版本判断，等待下一轮目标状态刷新重新确认。
    m_deviceRealtimeUpdateStates.remove(ip);
    m_deviceRemoteSessionCounts.remove(ip); // wjy: 新增同 IP 前清掉旧远控人数，避免误显示上一次会话徽标。
    m_deviceRemoteControllerNames.remove(ip);
    m_deviceIpEdit->clear();
    m_deviceNameEdit->clear();
    m_deviceMacEdit->clear();
    m_deviceRemarkEdit->clear();

    m_remoteAssistSelected = false;
    m_localInfoSelected = false;
    m_settingsSelected = false;
    m_settingsScrollOffset = 0;
    m_deviceGroupExpanded = true;
    setDesktopHoverActive(false);
    clearBottomActionHover();
    const int newDeviceIndex = g_deviceCatalog.deviceIndexForIp(ip); // wjy: 新设备的视觉下标由目录当前顺序重新解析，后续异步排序不会复用旧位置。

    m_selectedDeviceIndexes.clear();
    m_selectedDeviceIndexes.insert(
        newDeviceIndex);

    m_selectionAnchorDeviceIndex =
        newDeviceIndex;

    if (addingFirstDevice) { // wjy: Avoid duplicate detail UI when the first saved device becomes both previous and current index 0.
        m_selectedDeviceIndex = newDeviceIndex;
        m_previousDeviceIndex = newDeviceIndex;
        m_currentDeviceName = name;
        m_previousDeviceName = name;
        loadScriptUiStateForDevice(currentScriptUiDeviceIp()); // wjy: 手动新增第一台设备时，下方脚本 UI 初始化为这台设备自己的空状态。
        if (m_detailAnimationTimer) {
            m_detailAnimationTimer->stop();
        }
        update();
    } else {
        startDeviceSwitchAnimation(
            newDeviceIndex,
            name);
    }
    updateAddDeviceControls();
    updateLocalInfoControls();
    update(); // wjy: 新设备等待下一份 UDP 心跳；只有标题栏刷新按钮才执行一次全设备 TCP 校准。
}

void DeviceGrid::cancelNewDevice()
{
    m_deviceIpEdit->clear();
    m_deviceNameEdit->clear();
    m_deviceMacEdit->clear();
    m_deviceRemarkEdit->clear();
    m_remoteAssistSelected = false;
    m_localInfoSelected = false;
    updateAddDeviceControls();
    updateLocalInfoControls();
    update();
}

platform::DevicePresenceState DeviceGrid::devicePresenceForIndex(int index) const
{
    if (index < 0 || index >= g_devices.size()) {
        return platform::DevicePresenceState::Unknown;
    }

    const QString ip = g_devices.at(index).ip.trimmed();
    if (ip.isEmpty()) {
        return platform::DevicePresenceState::Offline;
    }
    return m_deviceStatuses.value(ip, platform::DevicePresenceState::Unknown);
}

bool DeviceGrid::devicePoweringOnForIndex(int index) const
{
    if (index < 0 || index >= g_devices.size()) {
        return false;
    }
    return m_poweringOnDeviceIps.contains(g_devices.at(index).ip.trimmed());
}

int DeviceGrid::devicePoweringOnRemainingSecondsForIndex(int index) const
{
    if (!devicePoweringOnForIndex(index)) {
        return 0;
    }

    const QString ip = g_devices.at(index).ip.trimmed();
    const qint64 startedAtMs = m_poweringOnStartedAtMs.value(ip, 0);
    if (startedAtMs <= 0) {
        return 80;
    }

    const qint64 elapsedMs = qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - startedAtMs);
    const int elapsedSeconds = static_cast<int>(elapsedMs / 1000);
    return qMax(0, 80 - elapsedSeconds);
}

void DeviceGrid::refreshDeviceStatuses()
{
// =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-status] refresh begin")); // wjy: BUG诊断日志统一写入文件，确认 Release 是否进入状态刷新函数。
    if (m_statusRefreshInProgress) {
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] refresh skipped inProgress=1")); // wjy: 如果上一次刷新还没结束，记录跳过原因。
        return;
    }
// ===end====

    m_statusRefreshInProgress = true;
    m_refreshClock.restart();
    if (!m_refreshTimer->isActive()) {
        m_refreshTimer->start();
    }
    update(refreshRect().adjusted(-2, -2, 2, 2));

    QStringList ips;
    ips.reserve(g_devices.size());
    for (const DeviceEntry& device : g_devices) {
        const QString ip = device.ip.trimmed();
        if (!ip.isEmpty() && !ips.contains(ip)) {
            ips.append(ip);
        }
    }
// =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-status] ip count=%1 values=%2")
        .arg(ips.size())
        .arg(ips.join(QStringLiteral(",")))); // wjy: 记录本次从 devices.json 加载出的待探测 IP，判断崩溃是否和某个设备有关。
// ===end====

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, ips] {
// =====wjy====
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] background thread start ipCount=%1").arg(ips.size())); // wjy: 后台总线程启动日志写入文件，确认是否进入线程阶段。
// ===end====
        platform::DeviceStatusRefreshResult refreshResult; // wjy: 后台线程只产出一个不可变语义上的结果包，UI 线程再决定如何归并到实时服务或旧缓存。
        refreshResult.reserve(ips.size());
        for (const QString& ip : ips) {
            refreshResult.devices.insert(ip, platform::DeviceStatusInfo{}); // wjy: 每轮刷新先默认离线、无会话，只有本次查询明确返回才覆盖旧值。
            refreshResult.updateAvailability.insert(ip, false); // wjy: 离线、旧协议或查询失败默认不显示更新按钮。
        }
        std::mutex resultMutex;
        std::atomic_int nextIndex = 0;
        const int workerCount = std::max(1, std::min<int>(8, ips.size()));
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
// =====wjy====
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] worker count=%1").arg(workerCount)); // wjy: 记录本次会创建几个并发探测线程。
// ===end====

        for (int worker = 0; worker < workerCount; ++worker) {
            workers.emplace_back([&, worker] { // wjy: 捕获 worker 副本用于日志，避免并发日志读取循环变量引用。
// =====wjy====
                writeDeviceGridStartupLog(QStringLiteral("[wjy-status] worker start worker=%1").arg(worker)); // wjy: 每个 worker 启动时记录编号，判断是否创建线程后崩溃。
// ===end====
                while (true) {
                    const int index = nextIndex.fetch_add(1);
                    if (index >= ips.size()) {
                        break;
                    }
                    const QString ip = ips.at(index);
// =====wjy====
                    writeDeviceGridStartupLog(QStringLiteral("[wjy-status] probe begin worker=%1 index=%2 ip=%3")
                        .arg(worker)
                        .arg(index)
                        .arg(ip)); // wjy: 单个 IP 探测前记录，崩溃时可定位是否卡在某台设备。
// ===end====
                    platform::DeviceStatusInfo info = platform::DeviceStatusService::query(ip); // wjy: 先收集目标端完整状态，再在结果包边界规范化会话名称和人数。
                    bool updateAvailable = false;
                    if (info.state == platform::DevicePresenceState::Online
                        || info.state == platform::DevicePresenceState::Busy) {
                        const platform::RemoteUpdateStatus updateStatus =
                            platform::DeviceCommandService::queryUpdateStatus(ip, nullptr, 49102, 500); // wjy: 复用目标端已有 update_status，只在统一设备刷新线程查询，不接触远控流回调。
                        updateAvailable = updateStatus == platform::RemoteUpdateStatus::Idle
                            || updateStatus == platform::RemoteUpdateStatus::Failed; // wjy: idle 表示发现新版；上次准备失败后仍允许用户从标题栏重试。
                    }
// =====wjy====
                    writeDeviceGridStartupLog(QStringLiteral("[wjy-status] probe done worker=%1 index=%2 ip=%3 state=%4")
                        .arg(worker)
                        .arg(index)
                        .arg(ip)
                        .arg(static_cast<int>(info.state))); // wjy: 单个 IP 探测后记录状态枚举值，判断 query 是否顺利返回。
// ===end====
                    std::lock_guard lock(resultMutex);
                    info.remoteSessionCount = qBound(0, info.remoteSessionCount, 10); // wjy: 在结果包边界统一限制会话人数，避免 UI 层重复防御。
                    info.remoteControllerNames = info.remoteControllerNames.trimmed();
                    refreshResult.devices.insert(ip, info); // wjy: 一个 DeviceStatusInfo 保留同一台设备的状态、名称、网络、脚本和远控会话字段。
                    refreshResult.updateAvailability.insert(ip, updateAvailable); // wjy: 更新可用性和状态结果使用同一个 IP 键，避免平行表遗漏。
                }
// =====wjy====
                writeDeviceGridStartupLog(QStringLiteral("[wjy-status] worker end worker=%1").arg(worker)); // wjy: worker 结束日志，判断线程是否正常跑完。
// ===end====
            });
        }

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
// =====wjy====
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] all workers joined statusCount=%1").arg(refreshResult.devices.size())); // wjy: 所有 worker 汇合后记录结果数量。
// ===end====

        if (!self) {
// =====wjy====
            writeDeviceGridStartupLog(QStringLiteral("[wjy-status] widget destroyed before invoke")); // wjy: 如果界面已销毁，记录后直接退出后台线程。
// ===end====
            return;
        }

// =====wjy====
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] invoke ui post begin")); // wjy: 准备把后台探测结果投递回 UI 线程；如果有 begin 没有 end，说明崩在投递附近。
        const bool invokeQueued = QMetaObject::invokeMethod(self, [self, refreshResult = std::move(refreshResult)]() mutable { // wjy: 把同一设备的状态、网络、脚本和远控字段作为一个结果包投递到 UI 线程。
// =====wjy====
            writeDeviceGridStartupLog(QStringLiteral("[wjy-status] invoke ui begin")); // wjy: 回到 UI 线程前半段日志，判断崩溃是否发生在 UI 更新阶段。
// ===end====
            if (!self) {
// =====wjy====
                writeDeviceGridStartupLog(QStringLiteral("[wjy-status] widget destroyed inside invoke")); // wjy: UI 回调执行时控件已销毁，记录并退出。
// ===end====
                return;
            }
            DeviceGrid* grid = self.data();
            grid->applyDeviceStatusRefreshResult(std::move(refreshResult)); // wjy: UI 回调只负责生命周期校验和转交，状态归并集中到单一方法维护。
// =====wjy====
            writeDeviceGridStartupLog(QStringLiteral("[wjy-status] invoke ui end")); // wjy: UI 状态写入和重绘请求完成日志。
// ===end====
        }, Qt::QueuedConnection);
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] invoke ui post end queued=%1").arg(invokeQueued)); // wjy: 记录投递是否成功排队，继续定位关闭/启动偶发异常发生点。
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] background thread end")); // wjy: 后台总线程即将退出，和 DeviceGrid 析构日志对照。
// ===end====
    });
}

// =====wjy====
void DeviceGrid::applyDeviceStatusRefreshResult(platform::DeviceStatusRefreshResult refreshResult)
{
    const bool realtimeAvailable = m_realtimeStateService
        && m_realtimeStateService->isRunning(); // wjy: 实时广播服务运行时交给归并器决定新旧状态优先级。
    if (!realtimeAvailable) {
        m_deviceUpdateAvailability.clear();
    }
    if (realtimeAvailable) {
        for (auto it = refreshResult.devices.cbegin(); it != refreshResult.devices.cend(); ++it) {
            m_realtimeStateService->applyManualCalibration(it.key(), it.value()); // wjy: 手动刷新结果进入实时归并器，避免一次超时覆盖已收到的新鲜广播。
        }
    } else {
        m_deviceStatuses.clear();
        m_deviceRemoteSessionCounts.clear();
        m_deviceRemoteControllerNames.clear();
        for (auto it = refreshResult.devices.cbegin(); it != refreshResult.devices.cend(); ++it) {
            m_deviceStatuses.insert(it.key(), it.value().state); // wjy: 无实时服务时保留原有离线缓存兜底。
            m_deviceRemoteSessionCounts.insert(it.key(), it.value().remoteSessionCount); // wjy: 同一结果包同步更新远控会话数。
            m_deviceRemoteControllerNames.insert(it.key(), it.value().remoteControllerNames); // wjy: 同一结果包同步更新控制端名称。
        }
    }
    for (auto it = refreshResult.updateAvailability.cbegin(); it != refreshResult.updateAvailability.cend(); ++it) {
        const platform::DeviceRealtimeUpdateState realtimeUpdate = m_deviceRealtimeUpdateStates.value(it.key());
        const bool hasRealtimeVersion = realtimeAvailable && !realtimeUpdate.installedVersion.trimmed().isEmpty();
        setRemoteUpdateAvailability(it.key(), hasRealtimeVersion
                ? realtimeUpdateAvailable(realtimeUpdate)
                : it.value()); // wjy: 新版目标继续以实时版本为准；旧目标缺少扩展字段时手动刷新仍可用 TCP 校准。
    }
    // wjy: 状态刷新只更新设备状态，不根据一次 Offline 探测关闭远控窗口；窗口生命周期由专门流程管理。
    bool deviceRecordChanged = false;
    for (const DeviceEntry& device : g_devices) {
        DeviceEntry updatedDevice = device; // wjy: 先在副本中归并远端字段，最后按稳定 ID 一次提交目录。
        bool currentDeviceChanged = false;
        const QString ip = device.ip.trimmed();
        const platform::DeviceStatusInfo refreshedInfo = refreshResult.devices.value(ip);
        const QString remoteMac = refreshedInfo.mac.trimmed();
        const QString remoteBroadcastIp = refreshedInfo.broadcastIp.trimmed();
        if (!ip.isEmpty() && device.mac.trimmed().isEmpty() && !remoteMac.isEmpty()) {
            qWarning().noquote() << QStringLiteral("[wjy-status] sync device mac ip=%1 mac=%2")
                .arg(ip)
                .arg(remoteMac); // wjy: 保留原有诊断日志，便于定位自动补齐 MAC 的来源。
            updatedDevice.mac = remoteMac; // wjy: 仅补齐本地缺失的 MAC，避免覆盖用户已有值。
            currentDeviceChanged = true;
        }
        if (!ip.isEmpty() && device.broadcastIp.trimmed().isEmpty() && !remoteBroadcastIp.isEmpty()) {
            updatedDevice.broadcastIp = remoteBroadcastIp; // wjy: 仅补齐本地缺失的广播地址。
            currentDeviceChanged = true;
        }
        const QString remoteName = refreshedInfo.deviceName.trimmed();
        const QString pendingRemoteRename = m_pendingRemoteRenameNames.value(ip).trimmed(); // wjy: 改名等待重启生效期间保护本地新名称。
        bool allowRemoteNameSync = true;
        if (!pendingRemoteRename.isEmpty()) {
            if (remoteName == pendingRemoteRename) {
                m_pendingRemoteRenameNames.remove(ip); // wjy: 远端已上报新名称，解除保护。
            } else {
                allowRemoteNameSync = false; // wjy: 远端仍返回旧名称时不覆盖本地待生效名称。
            }
        }
        if (allowRemoteNameSync && !ip.isEmpty() && !remoteName.isEmpty() && device.name.trimmed() != remoteName) {
            qWarning().noquote() << QStringLiteral("[wjy-status] sync device name ip=%1 old=%2 new=%3")
                .arg(ip)
                .arg(device.name.trimmed())
                .arg(remoteName); // wjy: 保留原有诊断日志，便于定位远端名称同步。
            updatedDevice.name = remoteName; // wjy: 远端真实设备名变化时同步本地目录。
            currentDeviceChanged = true;
        }
        if (currentDeviceChanged && g_deviceCatalog.updateDevice(device.id, std::move(updatedDevice))) {
            deviceRecordChanged = true; // wjy: 使用稳定 deviceId 提交，避免刷新期间数组重排造成错写。
        }
    }
    if (deviceRecordChanged) {
        saveDevices(); // wjy: 设备元数据修正后立即持久化，保证下次启动继续使用最新信息。
        if (m_selectedDeviceIndex >= 0 && m_selectedDeviceIndex < g_devices.size()) {
            m_currentDeviceName = deviceDisplayName(g_devices.at(m_selectedDeviceIndex));
            m_previousDeviceName = m_currentDeviceName;
        }
    }
    if (!realtimeAvailable) {
        for (auto it = refreshResult.devices.cbegin(); it != refreshResult.devices.cend(); ++it) {
            if (!it.value().scriptRuntime.supported) {
                continue;
            }
            applyRemoteScriptRuntimeState(it.key(), it.value().terminalUser.trimmed(), it.value().scriptRuntime); // wjy: 无实时服务时仍用手动刷新恢复目标脚本运行态。
        }
    }
    for (auto it = m_poweringOnDeviceIps.begin(); it != m_poweringOnDeviceIps.end();) {
        const platform::DevicePresenceState state = m_deviceStatuses.value(*it, platform::DevicePresenceState::Offline); // wjy: 开机探测成功后结束视觉提示。
        if (state != platform::DevicePresenceState::Offline) {
            m_poweringOnStartedAtMs.remove(*it);
            it = m_poweringOnDeviceIps.erase(it);
        } else {
            ++it;
        }
    }
    m_statusRefreshInProgress = false; // wjy: UI 应用完成后才允许下一轮刷新进入。
    update(); // wjy: 一次性请求设备列表和详情区域重绘。
}
// ===end====

void DeviceGrid::probePoweringOnDevices()
{
    if (m_wakeProbeInProgress || m_poweringOnDeviceIps.isEmpty()) {
        return;
    }

    QStringList ips;
    ips.reserve(m_poweringOnDeviceIps.size());
    for (const QString& ip : m_poweringOnDeviceIps) {
        const QString trimmed = ip.trimmed();
        if (!trimmed.isEmpty()) {
            ips.append(trimmed);
        }
    }
    if (ips.isEmpty()) {
        return;
    }

    m_wakeProbeInProgress = true;
    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, ips] {
        QHash<QString, platform::DevicePresenceState> statuses;
        statuses.reserve(ips.size());
        for (const QString& ip : ips) {
            statuses.insert(ip, platform::DeviceStatusService::probe(ip));
        }

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, statuses = std::move(statuses)]() mutable {
            if (!self) {
                return;
            }

            DeviceGrid* grid = self.data();
            grid->m_wakeProbeInProgress = false;
            if (grid->m_realtimeStateService && grid->m_realtimeStateService->isRunning()) {
                for (auto it = statuses.cbegin(); it != statuses.cend(); ++it) {
                    platform::DeviceStatusInfo calibration;
                    calibration.state = it.value();
                    grid->m_realtimeStateService->applyManualCalibration(it.key(), calibration); // wjy: 唤醒探测也进入归并器，新鲜广播状态不会被一次 TCP 结果回滚。
                }
            } else {
                for (auto it = statuses.cbegin(); it != statuses.cend(); ++it) {
                    grid->m_deviceStatuses.insert(it.key(), it.value());
                }
            }
            for (auto it = grid->m_poweringOnDeviceIps.begin(); it != grid->m_poweringOnDeviceIps.end();) {
                const platform::DevicePresenceState state = grid->m_deviceStatuses.value(*it, platform::DevicePresenceState::Offline);
                if (state != platform::DevicePresenceState::Offline) {
                    grid->m_poweringOnStartedAtMs.remove(*it);
                    it = grid->m_poweringOnDeviceIps.erase(it);
                } else {
                    ++it;
                }
            }
            if (grid->m_poweringOnDeviceIps.isEmpty()) {
                grid->m_lastWakeProbeAtMs = 0;
            }
            grid->update();
        }, Qt::QueuedConnection);
    });
}

void DeviceGrid::showDeviceMenu()
{
    QMenu menu(this);
    menu.setAttribute(Qt::WA_TranslucentBackground);
    menu.setWindowFlag(Qt::NoDropShadowWindowHint, true);
    menu.setStyleSheet(QStringLiteral(
        "QMenu{background:#FFFFFF;border:1px solid #E6EAF0;border-radius:6px;padding:8px 0;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QMenu::item{height:34px;padding:0 34px 0 38px;background:transparent;}"
        "QMenu::item:selected{background:#F3F7FF;color:#040B18;}"
        "QMenu::icon{padding-left:12px;}"));

    const bool offlineOnly = devicePresenceForIndex(m_selectedDeviceIndex) == platform::DevicePresenceState::Offline;
    QAction* terminalAction = nullptr;
    QAction* shutdownAction = nullptr;
    QAction* restartAction = nullptr;
    if (!offlineOnly) {
        terminalAction = menu.addAction(menuIcon(QStringLiteral("terminal.svg")), zh("\xE7\xBB\x88\xE7\xAB\xAF"));
        shutdownAction = menu.addAction(menuIcon(QStringLiteral("shutdown.svg")), zh("\xE5\x85\xB3\xE6\x9C\xBA")); // wjy: 设备更多菜单里的关机也统一使用红色关闭图标，避免不同入口视觉不一致。
        restartAction = menu.addAction(menuIcon(QStringLiteral("restart.svg")), zh("\xE9\x87\x8D\xE5\x90\xAF"));
    }
    QAction* renameAction = menu.addAction(menuIcon(QStringLiteral("rename.svg")), zh("\xE9\x87\x8D\xE5\x91\xBD\xE5\x90\x8D"));
    QAction* deleteAction = menu.addAction(menuIcon(QStringLiteral("delete.svg")), zh("\xE5\x88\xA0\xE9\x99\xA4\xE8\xAE\xBE\xE5\xA4\x87"));
    if (terminalAction) {
        connect(terminalAction, &QAction::triggered, this, &DeviceGrid::openCurrentDeviceTerminal);
    }
    if (shutdownAction) {
        connect(shutdownAction, &QAction::triggered, this, &DeviceGrid::shutdownCurrentDevice);
    }
    if (restartAction) {
        connect(restartAction, &QAction::triggered, this, &DeviceGrid::restartCurrentDevice);
    }
    connect(renameAction, &QAction::triggered, this, &DeviceGrid::renameCurrentDevice);
    connect(deleteAction, &QAction::triggered, this, &DeviceGrid::deleteCurrentDevice);

    const bool isRemoteControlled = deviceBadgeIndexes().contains(m_selectedDeviceIndex);
    const QRectF moreRect = moreActionRect(isRemoteControlled);
    const QSize menuSize = menu.sizeHint();
    const int menuX = qRound(moreRect.center().x() - menuSize.width() / 2.0);
    const int menuY = qRound(moreRect.bottom() + 6);
    menu.exec(mapToGlobal(QPoint(menuX, menuY)));
}

// =====wjy====
void DeviceGrid::showDeviceContextMenuForIndexes(
    int deviceIndex,
    const QVector<int>& targetDeviceIndexes,
    const QPoint& globalPosition)
{
    if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return; // wjy: 设备已删除或下标失效时不显示可能控制错误目标的菜单。
    }

    QVector<QString> requestedTargetIds;
    for (const int targetIndex : targetDeviceIndexes) {
        if (targetIndex >= 0 && targetIndex < g_devices.size()) {
            requestedTargetIds.append(g_devices.at(targetIndex).id); // wjy: UI 下标只在菜单入口转换一次，后续目标集合以稳定设备 ID 传递。
        }
    }
    const QVector<QString> targetDeviceIds = platform::DeviceActionTargetResolver::normalizeDeviceIds(
        g_deviceCatalog,
        requestedTargetIds,
        g_devices.at(deviceIndex).id); // wjy: 过滤重复、过期目标并在多选失效时回退触发菜单的设备。
    const QVector<int> validTargetIndexes = platform::DeviceActionTargetResolver::indexesForDeviceIds(
        g_deviceCatalog,
        targetDeviceIds); // wjy: 兼容旧批量入口仍使用瞬时下标，但下标由稳定 ID 在当前目录顺序重新解析。
    const bool batchDeviceMenu = validTargetIndexes.size() > 1; // wjy: 只有设备列表多选才显示“批量”文案，单个远控窗口始终控制一台设备。

    QMenu menu(this); // wjy: 两种入口复用同一 QMenu，后续顺序、图标或动作调整只需要维护这一处。
    QMenu* scriptMenu = menu.addMenu(QString::fromUtf8("执行脚本"));
    populateCachedScriptFolderMenu(scriptMenu); // wjy: 远控标题栏使用后台加载的树快照，弹出菜单时不访问共享目录。
    QAction* stopScriptsAction = menu.addAction(QString::fromUtf8("停止脚本")); // wjy: 单设备、多选设备和远控标题栏菜单共用同一个停止入口。
    menu.addSeparator();
    QMenu* systemMenu = menu.addMenu(menuIcon(QStringLiteral("settings.svg")), QString::fromUtf8("系统设置"));
    QAction* wakeAction = systemMenu->addAction(menuIcon(QStringLiteral("power_on.svg")), batchDeviceMenu ? QString::fromUtf8("批量开机") : QString::fromUtf8("开机"));
    QAction* shutdownAction = systemMenu->addAction(menuIcon(QStringLiteral("shutdown.svg")), batchDeviceMenu ? QString::fromUtf8("批量关机") : QString::fromUtf8("关机"));
    QAction* restartAction = systemMenu->addAction(menuIcon(QStringLiteral("restart.svg")), batchDeviceMenu ? QString::fromUtf8("批量重启") : QString::fromUtf8("重启"));
    QAction* updateAction = systemMenu->addAction(menuIcon(QStringLiteral("update.svg")), batchDeviceMenu ? QString::fromUtf8("批量更新") : QString::fromUtf8("更新"));
    QAction* terminalAction = systemMenu->addAction(menuIcon(QStringLiteral("terminal.svg")), QString::fromUtf8("终端"));
    QAction* renameAction = systemMenu->addAction(menuIcon(QStringLiteral("rename.svg")), QString::fromUtf8("重命名")); // wjy: 设备双击改为远控后，把原地重命名入口移动到右键“系统设置”末尾。
    // =====wjy====
    QAction* createGroupAction = nullptr;
    QVector<QAction*> existingGroupActions; // wjy: 按菜单创建时的分组顺序保存动作指针，不能占用脚本动作使用的 data 字段。
    if (!validTargetIndexes.isEmpty()) {
        QMenu* addGroupMenu = menu.addMenu(QString::fromUtf8("添加分组")); // wjy: 单设备和多设备都复用同一归组子菜单，目标集合由稳定设备 ID 统一解析。
        createGroupAction = addGroupMenu->addAction(QString::fromUtf8("新建分组")); // wjy: 新建入口固定为子菜单首行，下面才显示当前分组名。
        if (!g_deviceGroupNames.isEmpty()) {
            addGroupMenu->addSeparator(); // wjy: 用分隔线区分创建动作和现有目标组，组名顺序保持侧栏顺序。
        }
        existingGroupActions.reserve(g_deviceGroupNames.size());
        for (const QString& groupName : std::as_const(g_deviceGroupNames)) {
            existingGroupActions.append(addGroupMenu->addAction(groupName)); // wjy: 悬浮子菜单实时列出全部当前分组名称。
        }
    }
    // ===end====
    menu.addSeparator();
    QAction* deleteDeviceAction = menu.addAction(menuIcon(QStringLiteral("delete.svg")), QString::fromUtf8("删除设备")); // wjy: 设备列表和远控标题栏共享删除入口，只从本机列表移除触发菜单的这台设备。

    QAction* selectedAction = menu.exec(globalPosition); // wjy: 设备列表使用列表右键屏幕坐标，远控窗口使用其标题栏右键屏幕坐标。
    // =====wjy====
    const int selectedGroupIndex = existingGroupActions.indexOf(selectedAction); // wjy: 动作数组下标与菜单创建时的真实分组下标一一对应。
    if (selectedAction == createGroupAction && createGroupAction) {
        const int newGroupIndex = createDefaultDeviceGroup(); // wjy: 先追加带默认名和稳定 ID 的新组，再把本次菜单目标一次性归入。
        if (assignDevicesToGroup(validTargetIndexes, newGroupIndex)) {
            revealDeviceGroup(newGroupIndex, true); // wjy: 保存完成后跳到新组，并让默认组名进入全选输入状态。
        }
    } else if (selectedGroupIndex >= 0) {
        if (assignDevicesToGroup(validTargetIndexes, selectedGroupIndex)) {
            revealDeviceGroup(selectedGroupIndex, false); // wjy: 已在该组时不重复写归属，但仍展开并滚动到目标组标题。
        }
    // ===end====
    // =====wjy====
    } else if (selectedAction == stopScriptsAction) {
        stopDeviceScriptsForIndexes(validTargetIndexes); // wjy: 停止动作一次遍历全部菜单目标，并聚合没有运行脚本时的反馈。
    } else if (selectedAction && selectedAction->data().isValid()) {
        const QString scriptEntryPath = selectedAction->data().toString(); // wjy: 保存菜单绑定的具体入口文件，单设备和多设备必须执行同一个用户选择。
        if (batchDeviceMenu) {
            batchExecuteDeviceScriptFolder(
                validTargetIndexes,
                scriptEntryPath,
                QString::fromUtf8("选中的设备中没有可执行设备。")); // wjy: 多选时传入完整有效目标集合，不能再只使用触发右键菜单的 deviceIndex。
        } else {
            executeDeviceScriptFolder(deviceIndex, scriptEntryPath, true); // wjy: 单设备及远控标题栏菜单保留原来的详细失败提示。
        }
    // ===end====
    } else if (selectedAction == wakeAction) {
        batchWakeDevices(validTargetIndexes);
    } else if (selectedAction == shutdownAction) {
        batchShutdownDevices(validTargetIndexes);
    } else if (selectedAction == restartAction) {
        batchRestartDevices(validTargetIndexes);
    } else if (selectedAction == updateAction) {
        if (batchDeviceMenu) {
            batchUpdateDevices(validTargetIndexes);
        } else {
            updateDeviceForIndex(deviceIndex, true); // wjy: 单个远控窗口显示更新受理、已最新或失败反馈。
        }
    } else if (selectedAction == terminalAction) {
        batchOpenDeviceTerminals(validTargetIndexes);
    } else if (selectedAction == renameAction) {
        beginDeviceRename(deviceIndex); // wjy: 多选菜单仍只重命名触发右键的设备，避免一次输入误改多台名称。
    } else if (selectedAction == deleteDeviceAction) {
        deleteDeviceForIndex(deviceIndex); // wjy: 使用菜单绑定的真实设备下标，远控标题栏右键不会误删主界面当前选择。
    }
}

void DeviceGrid::showRemoteWindowDeviceMenu(const QString& hostIp, const QPoint& globalPosition)
{
    const QString targetIp = hostIp.trimmed();
    if (targetIp.isEmpty()) {
        return;
    }
    for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        if (g_devices.at(deviceIndex).ip.trimmed().compare(targetIp, Qt::CaseInsensitive) == 0) {
            showDeviceContextMenuForIndexes(deviceIndex, QVector<int>{deviceIndex}, globalPosition); // wjy: IP 匹配后显式传入单设备集合，绝不继承主界面的多选状态。
            return;
        }
    }
}

void DeviceGrid::updateRemoteWindowDevice(const QString& hostIp)
{
    const QString targetIp = hostIp.trimmed();
    if (targetIp.isEmpty()) {
        return;
    }
    for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        if (g_devices.at(deviceIndex).ip.trimmed().compare(targetIp, Qt::CaseInsensitive) != 0) {
            continue;
        }
        updateDeviceForIndex(deviceIndex, true); // wjy: 标题栏按钮与设备右键“更新”调用同一个入口，共享受理提示、失败处理和窗口保持状态机。
        return;
    }
}
// ===end====

QVector<int> DeviceGrid::deviceIndexesForGroup(int groupIndex) const
{
// =====wjy====
    QVector<int> result;
    if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return result;
    }
    const QString groupId = g_deviceGroupIds.at(groupIndex);
    const QVector<int> catalogIndexes = platform::DeviceActionTargetResolver::indexesForDeviceIds(
        g_deviceCatalog,
        platform::DeviceActionTargetResolver::deviceIdsForGroup(g_deviceCatalog, groupId)); // wjy: 分组批量操作先按稳定 groupId 收集，再恢复当前展示下标，避免名称和旧位置漂移。
    result.reserve(catalogIndexes.size());
    for (const int deviceIndex : catalogIndexes) {
        if (deviceIndex < 0
            || deviceIndex >= g_devices.size()
            || deviceHiddenByLocalPreference(g_devices.at(deviceIndex))) {
            continue; // wjy: 隐藏本机时，分组右键的脚本、电源、终端和远控动作都只作用于界面可见设备。
        }
        result.append(deviceIndex);
    }
    return result;
// ===end====
}

QVector<int> DeviceGrid::contextDeviceIndexesForRightClick(int clickedDeviceIndex) const
{
// =====wjy====
    QVector<int> result;
    QSet<int> seen;
    const bool keepSelection = m_selectedDeviceIndexes.contains(clickedDeviceIndex);
    if (keepSelection) {
        for (int deviceIndex : m_selectedDeviceIndexes) {
            if (deviceIndex >= 0 && deviceIndex < g_devices.size() && !seen.contains(deviceIndex)) {
                result.append(deviceIndex);
                seen.insert(deviceIndex);
            }
        }
        std::sort(result.begin(), result.end());
    }

    if (result.isEmpty() && clickedDeviceIndex >= 0 && clickedDeviceIndex < g_devices.size()) {
        result.append(clickedDeviceIndex);
    }
    return result;
// ===end====
}

QVector<int> DeviceGrid::normalizedBatchDeviceIndexes(const QVector<int>& deviceIndexes) const
{
    QVector<QString> requestedIds;
    requestedIds.reserve(deviceIndexes.size());
    for (const int deviceIndex : deviceIndexes) {
        if (deviceIndex >= 0 && deviceIndex < g_devices.size()) {
            requestedIds.append(g_devices.at(deviceIndex).id); // wjy: 仅在 UI 入口读取一次下标，异步动作后续依赖稳定设备 ID。
        }
    }
    const QVector<QString> normalizedIds = platform::DeviceActionTargetResolver::normalizeDeviceIds(
        g_deviceCatalog,
        requestedIds); // wjy: 统一去重并过滤菜单打开后已经失效的目标。
    return platform::DeviceActionTargetResolver::indexesForDeviceIds(
        g_deviceCatalog,
        normalizedIds); // wjy: 兼容现有单设备函数接口，但每次执行前都按当前目录顺序重新解析下标。
}

void DeviceGrid::batchWakeDevices(const QVector<int>& deviceIndexes)
{
// =====wjy====
    for (const int deviceIndex : normalizedBatchDeviceIndexes(deviceIndexes)) {
        wakeDeviceForIndex(deviceIndex, false);
    }
// ===end====
}

void DeviceGrid::batchShutdownDevices(const QVector<int>& deviceIndexes)
{
// =====wjy====
    scheduleDevicePowerActions(normalizedBatchDeviceIndexes(deviceIndexes), false, false); // wjy: 整批设备由一个后台任务顺序发送关机命令，UI 不按设备数量累计等待。
// ===end====
}

void DeviceGrid::batchRestartDevices(const QVector<int>& deviceIndexes)
{
// =====wjy====
    scheduleDevicePowerActions(normalizedBatchDeviceIndexes(deviceIndexes), true, false); // wjy: 重启批次和关机批次复用相同异步归并，只改变发送的命令类型。
// ===end====
}

// =====wjy====
void DeviceGrid::batchUpdateDevices(const QVector<int>& deviceIndexes)
{
    scheduleDeviceUpdateRequests(normalizedBatchDeviceIndexes(deviceIndexes), false); // wjy: 批量更新在一个后台任务内逐台请求，主线程只接收最终结果包。
}
// ===end====

void DeviceGrid::batchOpenDeviceTerminals(const QVector<int>& deviceIndexes)
{
// =====wjy====
    scheduleOpenTerminals(normalizedBatchDeviceIndexes(deviceIndexes), false); // wjy: 整批终端共用一个后台任务，弱网设备不会逐台冻结右键菜单和主窗口。
// ===end====
}

bool DeviceGrid::openTerminalForDeviceIndex(int deviceIndex, bool showMessages)
{
    return scheduleOpenTerminals(QVector<int>{deviceIndex}, showMessages); // wjy: 单设备终端入口立即返回已安排状态，不在鼠标事件中等待 TCP 或 ssh-keygen。
}

// =====wjy====
bool DeviceGrid::scheduleOpenTerminals(const QVector<int>& deviceIndexes, bool showMessages)
{
    struct TerminalTarget {
        QString ip;
    };
    struct TerminalOutcome {
        QString ip;
        bool opened = false;
        QString error;
    };

    QVector<TerminalTarget> targets;
    for (const int deviceIndex : deviceIndexes) {
        if (deviceIndex < 0 || deviceIndex >= g_devices.size()
            || !platform::isDeviceActionAllowed(
                platform::DeviceActionKind::Terminal,
                devicePresenceForIndex(deviceIndex))) {
            continue; // wjy: 终端仍只拒绝明确离线，Unknown 交给后台状态端口返回真实结果。
        }
        const QString ip = g_devices.at(deviceIndex).ip.trimmed();
        if (ip.isEmpty() || m_pendingTerminalOpenIps.contains(ip)) {
            continue;
        }
        bool duplicate = false;
        for (const TerminalTarget& target : targets) {
            duplicate = duplicate || target.ip.compare(ip, Qt::CaseInsensitive) == 0;
        }
        if (duplicate) {
            continue;
        }
        targets.append(TerminalTarget{ip});
        m_pendingTerminalOpenIps.insert(ip); // wjy: 后台预检查开始前占位，同一 IP 的连续双击和批量操作只执行一次。
    }
    if (targets.isEmpty()) {
        return false;
    }

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, targets, showMessages] {
        QVector<TerminalOutcome> outcomes;
        outcomes.reserve(targets.size());
        QString publicKeyError;
        const QString publicKey = platform::PortableOpenSshManager::instance().clientPublicKey(&publicKeyError); // wjy: 本机 OpenSSH 准备和密钥读取整批只执行一次，并且完全离开 UI 线程。
        for (const TerminalTarget& target : targets) {
            TerminalOutcome outcome;
            outcome.ip = target.ip;
            if (publicKey.isEmpty()) {
                outcome.error = publicKeyError.isEmpty()
                    ? QString::fromUtf8("无法读取本机远程终端公钥。")
                    : publicKeyError;
                outcomes.append(std::move(outcome));
                continue;
            }

            const QString loginUser = platform::DeviceStatusService::terminalUser(target.ip); // wjy: 最多 900ms 的状态等待只阻塞本后台批次，不影响窗口刷新和鼠标输入。
            if (loginUser.isEmpty()) {
                outcome.error = QString::fromUtf8("无法建立远程终端连接。");
                outcomes.append(std::move(outcome));
                continue;
            }

            if (!platform::DeviceCommandService::authorizeTerminalKey(target.ip, publicKey, &outcome.error)) {
                outcome.error = outcome.error.isEmpty()
                    ? QString::fromUtf8("无法在目标设备登记远程终端密钥。")
                    : QString::fromUtf8("无法在目标设备登记远程终端密钥：%1").arg(outcome.error); // wjy: 目标未升级或 49102 不可达时保留准确原因，不再出现 UI 卡住后黑窗闪退。
                outcomes.append(std::move(outcome));
                continue;
            }

            outcome.opened = platform::PortableOpenSshManager::instance().openTerminal(
                target.ip, loginUser, &outcome.error); // wjy: CreateProcess 和 Job 绑定也留在同一串行后台任务，批量终端不会竞争客户端 Job 句柄。
            if (!outcome.opened && outcome.error.isEmpty()) {
                outcome.error = QString::fromUtf8("无法建立远程终端连接。");
            }
            outcomes.append(std::move(outcome));
        }

        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, outcomes, showMessages] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            for (const TerminalOutcome& outcome : outcomes) {
                grid->m_pendingTerminalOpenIps.remove(outcome.ip); // wjy: 每台目标无论成功失败都释放占位，网络恢复后允许重新打开。
                if (showMessages && !outcome.opened) {
                    QMessageBox::warning(grid, QString(), outcome.error); // wjy: 单设备入口继续提供明确反馈，批量入口不因多个失败连续弹窗。
                }
            }
        }, Qt::QueuedConnection);
    });
    return true;
}
// ===end====

void DeviceGrid::openCurrentDeviceTerminal()
{
    openTerminalForDeviceIndex(m_selectedDeviceIndex, true);
}

void DeviceGrid::executeCurrentDeviceScriptFolder(const QString& scriptEntryPath)
{
// =====wjy====
    executeDeviceScriptFolder(m_selectedDeviceIndex, scriptEntryPath, true); // wjy: 当前设备执行用户明确选择的入口文件，父目录由执行函数负责复制。
// ===end====
}

bool DeviceGrid::executeDeviceScriptFolder(int deviceIndex, const QString& scriptEntryPath, bool showMessages)
{
// =====wjy====
    if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return false;
    }

    const DeviceEntry device = g_devices.at(deviceIndex);
    const QString targetIp = device.ip.trimmed();
    const QString preflightIdentity = device.id.trimmed().isEmpty() ? targetIp : device.id.trimmed();
    const QString preflightKey = preflightIdentity
        + QLatin1Char('\x1f')
        + QDir::fromNativeSeparators(scriptEntryPath).trimmed().toLower(); // wjy: 稳定设备身份和具体入口路径共同组成预检查键，同目录不同脚本可独立安排。
    const ScriptUiState existingState = m_scriptUiStateStore.state(targetIp);
    if (existingState.outputRunning && existingState.localLaunchInProgress) {
        m_scriptLaunchPreflightResults.remove(preflightKey); // wjy: 另一轮预检查返回前脚本已经启动时清理旧结果，未来重试必须重新确认远端状态。
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Information,
                QString(),
                QString::fromUtf8("该设备已有脚本正在执行。"),
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }
        return false; // wjy: 本控制端仍在启动/执行 SSH 任务时直接拦截，避免活动清单写入前的短暂空窗触发第二次执行。
    }

    if (!m_scriptLaunchPreflightResults.contains(preflightKey)) {
        if (m_pendingScriptLaunchKeys.contains(preflightKey)) {
            return true; // wjy: 同一设备和脚本的后台检查已经在进行，连续点击视为同一次已安排操作。
        }
        m_pendingScriptLaunchKeys.insert(preflightKey);
        const QString deviceId = device.id;
        QPointer<DeviceGrid> self(this);
        try {
            runBackgroundTask([self, deviceId, targetIp, scriptEntryPath, preflightKey, showMessages] {
                ScriptLaunchPreflightResult preflight;
                const QFileInfo entryScript = scriptEntryFileForSelection(scriptEntryPath); // wjy: 后台只验证用户点选的具体入口，避免同目录多个文件时重新猜入口。
                preflight.entryAvailable = entryScript.exists()
                    && !scriptRunCommandForFile(entryScript).trimmed().isEmpty();
                if (preflight.entryAvailable) {
                    preflight.entryScriptPath = entryScript.absoluteFilePath();
                    QString hashError;
                    preflight.entryScriptHash = scriptEntryContentHash(entryScript, &hashError); // wjy: 在后台线程计算入口脚本内容 Hash，脚本版本变化会落到新的本地工作区。
                    if (preflight.entryScriptHash.isEmpty()) {
                        preflight.entryAvailable = false;
                        preflight.errorMessage = hashError.isEmpty()
                            ? QString::fromUtf8("无法计算入口脚本 Hash。")
                            : hashError;
                    }
                }
                if (preflight.entryAvailable) {
                    preflight.remoteStatus = platform::DeviceStatusService::query(targetIp); // wjy: 一次后台 49101 查询同时取得终端用户和权威脚本状态。
                    const platform::RemoteScriptRuntimeInfo& runtime = preflight.remoteStatus.scriptRuntime;
                    const bool statusAlreadyBlocksLaunch = runtime.supported
                        && (!runtime.statusKnown || runtime.running); // wjy: 远端状态未知或正在运行时无需继续访问 49102 做密钥授权。
                    if (!statusAlreadyBlocksLaunch) {
                        const QString loginUser = preflight.remoteStatus.terminalUser.trimmed();
                        if (loginUser.isEmpty()) {
                            preflight.errorMessage = QString::fromUtf8("无法获取目标设备终端用户。");
                        } else {
                            QString authorizationError;
                            const QString publicKey = platform::PortableOpenSshManager::instance().clientPublicKey(&authorizationError); // wjy: 首次 OpenSSH 准备和可能的 ssh-keygen 等待也离开 UI 线程。
                            if (publicKey.isEmpty()) {
                                preflight.errorMessage = authorizationError.isEmpty()
                                    ? QString::fromUtf8("无法建立目标设备脚本执行授权。")
                                    : authorizationError;
                            } else {
                                preflight.authorizationSucceeded = platform::DeviceCommandService::authorizeTerminalKey(
                                    targetIp, publicKey, &authorizationError); // wjy: 最多 1.5 秒的目标密钥登记等待只占用后台任务。
                                if (!preflight.authorizationSucceeded) {
                                    preflight.errorMessage = authorizationError.isEmpty()
                                        ? QString::fromUtf8("无法建立目标设备脚本执行授权。")
                                        : authorizationError;
                                }
                            }
                        }
                    }
                }

                if (!self) {
                    return;
                }
                QMetaObject::invokeMethod(self, [self, deviceId, targetIp, scriptEntryPath, preflightKey, showMessages, preflight] {
                    if (!self) {
                        return;
                    }
                    DeviceGrid* grid = self.data();
                    grid->m_pendingScriptLaunchKeys.remove(preflightKey);
                    if (grid->m_shuttingDown) {
                        return;
                    }
                    const int currentDeviceIndex = deviceId.trimmed().isEmpty()
                        ? deviceIndexForIp(targetIp)
                        : g_deviceCatalog.deviceIndexForId(deviceId); // wjy: 后台返回后按稳定 ID 重新解析下标，预检查期间排序或同步不会串到其它设备。
                    if (currentDeviceIndex < 0) {
                        return; // wjy: 设备已被删除时直接丢弃网络结果，不创建无归属脚本状态。
                    }
                    grid->m_scriptLaunchPreflightResults.insert(preflightKey, preflight);
                    grid->executeDeviceScriptFolder(currentDeviceIndex, scriptEntryPath, showMessages); // wjy: 原脚本状态机仍只在主线程运行，本次递归只消费已完成的具体入口预检。
                }, Qt::QueuedConnection);
            });
        } catch (...) {
            m_pendingScriptLaunchKeys.remove(preflightKey);
            if (showMessages) {
                QMessageBox::warning(this, QString(), QString::fromUtf8("无法创建脚本预检查后台任务。"));
            }
            return false; // wjy: 系统线程资源不足时恢复去重状态并保留主程序，不让异常越出鼠标事件。
        }
        return true;
    }

    const ScriptLaunchPreflightResult preflight = m_scriptLaunchPreflightResults.take(preflightKey); // wjy: 后台结果只允许消费一次，下一次执行必须重新确认脚本目录和远端状态。
    const QFileInfo entryScript(preflight.entryScriptPath);
    if (!preflight.entryAvailable) {
        if (showMessages) {
            QMessageBox::information(
                this,
                QString(),
                preflight.errorMessage.isEmpty()
                    ? QString::fromUtf8("无可用脚本")
                    : preflight.errorMessage); // wjy: Hash 读取失败等预检错误直接反馈具体原因，不伪装成脚本不存在。
        }
        return false; // wjy: 后台确认没有受支持入口后不创建 work，也不发任何 SSH 执行命令。
    }

    const platform::DeviceStatusInfo remoteStatus = preflight.remoteStatus;
    if (remoteStatus.scriptRuntime.supported && remoteStatus.scriptRuntime.statusKnown) {
        applyRemoteScriptRuntimeState(targetIp, remoteStatus.terminalUser, remoteStatus.scriptRuntime); // wjy: 远端确认运行时恢复 Logo/停止元数据，确认空闲时清理旧控制端留下的陈旧状态。
    }
    if (remoteStatus.scriptRuntime.supported && !remoteStatus.scriptRuntime.statusKnown) {
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Warning,
                QString(),
                QString::fromUtf8("无法确认目标设备当前脚本状态，请刷新设备状态后重试。"),
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec(); // wjy: 清单损坏或进程不可检查时阻止新任务，避免在未知状态下叠加第二套脚本进程。
        }
        return false;
    }
    if (remoteStatus.scriptRuntime.supported && remoteStatus.scriptRuntime.running) {
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Information,
                QString(),
                QString::fromUtf8("该设备已有脚本正在执行。"),
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }
        return false; // wjy: 远端运行状态优先于新进程空白的本地 QHash，杜绝重启后的重复执行。
    }
    if (!remoteStatus.scriptRuntime.supported && existingState.outputRunning) {
        return false; // wjy: 旧目标端没有权威字段时保留原本的本地防重复行为，不因协议兼容而放宽正在执行的设备。
    }
    const QString loginUser = remoteStatus.terminalUser;
    if (loginUser.isEmpty()) {
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Warning,
                QString(),
                QString::fromUtf8("无法获取目标设备终端用户。"),
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }
        return false;
    }

    if (!preflight.authorizationSucceeded) {
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Warning,
                QString(),
                preflight.errorMessage.isEmpty()
                    ? QString::fromUtf8("无法建立目标设备脚本执行授权。")
                    : preflight.errorMessage,
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }
        return false; // wjy: 脚本执行复用远程终端密钥授权，目标设备未准备好时提前提示。
    }

    const QString sourcePath = QDir::toNativeSeparators(entryScript.absolutePath());
    const QString targetName = deviceDisplayName(device);
    const QString scriptName = entryScript.fileName();
    const QString scriptWorkName = scriptWorkspaceName(sourcePath, preflight.entryScriptHash); // wjy: 目录路径 Hash 和入口脚本内容 Hash 同时决定工作区，任一变化都会创建新副本。
    const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces); // wjy: 每次执行生成唯一 ID，远端完成/停止只能清理属于本次运行的活动清单。
    const bool targetIsCurrent = currentScriptUiDeviceIp() == targetIp;
    ScriptUiState state;
    state.outputVisible = true;
    state.outputRunning = true;
    state.outputFailed = false;
    // =====wjy====
    state.localLaunchInProgress = true;
    state.remoteStatusConfirmed = false;
    state.remoteRunId = runId;
    state.remoteControllerPid = 0;
    state.remoteStartedAtEpochMs = 0; // wjy: 清单尚未由目标端写入前标记为本地启动中，周期刷新不能把短暂空闲误当成执行结束。
    // ===end====
    state.outputScrollOffset = 0;
    state.outputAutoScroll = true;
    state.outputDirty = false;
    state.outputFilePath = scriptOutputTempFilePath();
    state.cancelRequested = std::make_shared<std::atomic_bool>(false);
    state.outputTitle = QString::fromUtf8("%1 - %2").arg(targetName, scriptName);
    state.outputText = QString::fromUtf8("$ 执行脚本 %1\n目标设备: %2\n状态: 正在复制并执行...\n")
        .arg(scriptName, targetName); // wjy: 点击脚本后立即显示右侧终端面板，让用户知道远端任务已经开始。
    state.lastScriptEntryPath = entryScript.absoluteFilePath(); // wjy: 保存精确入口文件路径，切换设备或再次点击执行时仍使用同一个文件。
    state.editorVisible = true;
    state.editorLoading = true;
    state.editorSaving = false;
    state.editorTitle = QString::fromUtf8("本地文件");
    state.editorRemotePath.clear();
    state.editorDeviceIp = targetIp;
    state.editorLoginUser = loginUser;
    state.editorWorkName = scriptWorkName;
    state.editorText = QString::fromUtf8("正在等待脚本文件复制到目标设备本地 work 目录...");
    state.editorModified = false;
    writeScriptOutputFile(state.outputFilePath, state.outputText, QIODevice::Truncate);
    m_scriptUiStateStore.setState(targetIp, state); // wjy: 指定设备执行脚本时先写入对应 IP 状态，分组批量执行不会抢当前设备 UI。
    update(deviceListViewportRect(m_deviceGroupExpanded)); // wjy: 脚本启动后立刻刷新设备列表，单台和分组批量执行都能马上显示运行图标。
    if (targetIsCurrent) {
        m_deviceDetailTab = DeviceDetailTab::ScriptLog;
        m_lastScriptEntryPath = state.lastScriptEntryPath;
        m_scriptOutputVisible = state.outputVisible;
        m_scriptOutputRunning = state.outputRunning;
        m_scriptOutputFailed = state.outputFailed;
        m_scriptOutputScrollOffset = state.outputScrollOffset;
        m_scriptOutputAutoScroll = state.outputAutoScroll;
        m_scriptOutputDirty = state.outputDirty;
        m_scriptOutputFilePath = state.outputFilePath;
        m_scriptCancelRequested = state.cancelRequested;
        m_scriptOutputTitle = state.outputTitle;
        m_scriptOutputText = state.outputText;
        m_scriptEditorVisible = state.editorVisible;
        m_scriptEditorLoading = state.editorLoading;
        m_scriptEditorSaving = state.editorSaving;
        m_scriptEditorTitle = state.editorTitle;
        m_scriptEditorRemotePath = state.editorRemotePath;
        m_scriptEditorDeviceIp = state.editorDeviceIp;
        m_scriptEditorLoginUser = state.editorLoginUser;
        m_scriptEditorWorkName = state.editorWorkName;
        if (m_scriptFileEdit) {
            m_scriptFileEdit->setPlainText(state.editorText);
            m_scriptFileEdit->document()->setModified(false); // wjy: 新脚本启动时重置编辑器状态，后续只编辑目标设备本地 work 里的副本。
        }
        updateScriptFileEditorControls();
        update();
    }
    QTimer::singleShot(900, this, [this, deviceIp = device.ip, loginUser, scriptWorkName] {
        const ScriptUiState state = m_scriptUiStateStore.state(deviceIp);
        if (state.editorWorkName == scriptWorkName) {
            loadScriptFileEditor(deviceIp, loginUser, scriptWorkName); // wjy: 复制开始后短暂延迟读取，让编辑框尽快显示 work 子目录里的 json/txt。
        }
    });

    const QString remotePowerShellScript = QStringLiteral(R"($ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONIOENCODING = 'utf-8'
$env:PYTHONUTF8 = '1'
$fsremoteExe = Get-Process FSRemote -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Path
if ([string]::IsNullOrWhiteSpace($fsremoteExe)) {
    Write-Error 'cannot locate FSRemote.exe on target device'
    exit 9009
}
$fsremoteDir = Split-Path -Parent $fsremoteExe
$workRoot = Join-Path $fsremoteDir 'work'
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
$scriptWorkName = '%4'
$work = Join-Path $workRoot $scriptWorkName
$source = '%1'
if (-not (Test-Path -LiteralPath $work)) {
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $log = Join-Path $work 'fsremote_robocopy.log'
    & robocopy $source $work /E /R:1 /W:1 /NFL /NDL /NJH /NJS "/LOG:$log" | Out-Null
    $copyExit = $LASTEXITCODE
    if ($copyExit -ge 8) {
        if (Test-Path -LiteralPath $log) {
            Get-Content -LiteralPath $log -Raw
        }
        exit $copyExit
    }
    Write-Output ('FSRemote created script workspace: ' + $work) # wjy: 两个 Hash 首次组合出新工作区时才复制共享目录，配置副本从此在本地 work 中独立保存。
} else {
    Write-Output ('FSRemote reused script workspace: ' + $work) # wjy: 目录路径 Hash 和入口脚本内容 Hash 均未变化时复用旧副本，避免覆盖本地修改过的 control.txt。
}
Set-Location -LiteralPath $work
$entry = Join-Path $work '%2'
if (-not (Test-Path -LiteralPath $entry)) {
    Write-Error ('entry script missing in local work folder: ' + $entry)
    exit 9011
}
$suffix = '%3'
$runLog = Join-Path $work 'fsremote_script_run.log'
$exitCodeFile = Join-Path $work 'fsremote_script_exit_code.txt'
$finishedFile = Join-Path $work 'fsremote_script_finished.txt'
$controllerPidFile = Join-Path $work 'fsremote_script_controller_pid.txt'
$stopRequestFile = Join-Path $work 'fsremote_script_stop_requested.txt'
$activeStateFile = Join-Path $workRoot 'fsremote_active_script.json'
$activeTempFile = Join-Path $workRoot ('fsremote_active_script.' + $PID + '.tmp')
$runId = '%5'
Remove-Item -LiteralPath $stopRequestFile -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $finishedFile -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $exitCodeFile -Force -ErrorAction SilentlyContinue
('PID: ' + $PID) | Set-Content -LiteralPath $controllerPidFile -Encoding UTF8
$startedAtEpochMs = [DateTimeOffset]::new((Get-Process -Id $PID).StartTime).ToUnixTimeMilliseconds()
# wjy: 清单记录 PowerShell 真实创建时间而不是复制完成时间，脚本目录复制很久时仍能通过 C++ 的 PID 复用校验。
$activeState = [ordered]@{
    version = 1
    state = 'running'
    runId = $runId
    workName = $scriptWorkName
    scriptName = '%2'
    controllerPid = [int64]$PID
    startedAtEpochMs = [int64]$startedAtEpochMs
}
$activeJson = $activeState | ConvertTo-Json -Compress
[System.IO.File]::WriteAllText($activeTempFile, $activeJson, (New-Object System.Text.UTF8Encoding($false)))
Move-Item -LiteralPath $activeTempFile -Destination $activeStateFile -Force
@(
    'FSRemote script execution started'
    ('Time: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
    ('Entry: ' + $entry)
    ('Type: ' + $suffix)
    ''
) | Set-Content -LiteralPath $runLog -Encoding UTF8
$scriptExit = 0
switch ($suffix) {
    'py' { & python -u $entry *>&1 | Tee-Object -FilePath $runLog -Append; $scriptExit = $LASTEXITCODE; break }
    'ps1' { & powershell -NoProfile -ExecutionPolicy Bypass -File $entry *>&1 | Tee-Object -FilePath $runLog -Append; $scriptExit = $LASTEXITCODE; break }
    'bat' { & cmd.exe /d /c ('call "' + $entry + '"') *>&1 | Tee-Object -FilePath $runLog -Append; $scriptExit = $LASTEXITCODE; break }
    'cmd' { & cmd.exe /d /c ('call "' + $entry + '"') *>&1 | Tee-Object -FilePath $runLog -Append; $scriptExit = $LASTEXITCODE; break }
    'exe' { & $entry *>&1 | Tee-Object -FilePath $runLog -Append; $scriptExit = $LASTEXITCODE; break }
    default { Write-Error ('unsupported script type: ' + $suffix); exit 9010 }
}
if ($null -eq $scriptExit) {
    $scriptExit = 0
}
('ExitCode: ' + $scriptExit) | Set-Content -LiteralPath $exitCodeFile -Encoding UTF8
('Finished: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) | Set-Content -LiteralPath $finishedFile -Encoding UTF8
('ExitCode: ' + $scriptExit) | Add-Content -LiteralPath $runLog -Encoding UTF8
Remove-Item -LiteralPath $controllerPidFile -Force -ErrorAction SilentlyContinue
if (Test-Path -LiteralPath $activeStateFile) {
    try {
        $currentActiveState = Get-Content -LiteralPath $activeStateFile -Raw | ConvertFrom-Json
        if ([string]$currentActiveState.runId -eq $runId) {
            Remove-Item -LiteralPath $activeStateFile -Force -ErrorAction SilentlyContinue
        }
    } catch {
        Write-Output ('FSRemote active state cleanup skipped: ' + $_.Exception.Message)
    }
}
Get-Content -LiteralPath $runLog -Tail 80
exit $scriptExit
)").arg(
        escapedPowerShellSingleQuoted(sourcePath),
        escapedPowerShellSingleQuoted(scriptName),
        escapedPowerShellSingleQuoted(entryScript.suffix().toLower()),
        escapedPowerShellSingleQuoted(scriptWorkName),
        escapedPowerShellSingleQuoted(runId)); // wjy: 目标包装器原子写入全局活动清单，正常结束时按 runId 条件删除，旧任务不会误删新任务状态。
    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, deviceIp = device.ip, loginUser, runId, remotePowerShellScript, targetName, scriptName, scriptWorkName, outputFilePath = state.outputFilePath, cancelRequested = state.cancelRequested] {
        QString outputText;
        QString runError;
        const bool ok = platform::PortableOpenSshManager::instance().runRemotePowerShellScript(
            deviceIp,
            loginUser,
            remotePowerShellScript,
            &outputText,
            &runError,
            0,
            [self, deviceIp, runId, outputFilePath](const QString& chunk) {
                if (!self || chunk.isEmpty()) {
                    return;
                }
                writeScriptOutputFile(outputFilePath, stripTerminalControlSequences(chunk), QIODevice::Append);
                QMetaObject::invokeMethod(self, [self, deviceIp, runId, outputFilePath] {
                    if (!self) {
                        return;
                    }
                    DeviceGrid* grid = self.data();
                    const QString targetIp = deviceIp.trimmed();
                    ScriptUiState state = grid->m_scriptUiStateStore.state(targetIp);
                    if (state.outputFilePath != outputFilePath
                        || (!state.remoteRunId.isEmpty() && state.remoteRunId != runId)) {
                        return;
                    }
                    state.outputDirty = true;
                    grid->m_scriptUiStateStore.setState(targetIp, state); // wjy: 输出分片先标记到目标设备状态，切到其它设备时不触发当前终端刷新。
                    const bool targetIsCurrent = grid->currentScriptUiDeviceIp() == targetIp;
                    if (targetIsCurrent) {
                        grid->m_scriptOutputDirty = true;
                    }
                    if (targetIsCurrent && !grid->m_scriptOutputFlushTimer->isActive()) {
                        grid->m_scriptOutputFlushTimer->start();
                    }
                }, Qt::QueuedConnection);
            },
            [cancelRequested] {
                return cancelRequested && cancelRequested->load();
            }); // wjy: 长业务脚本改为分块写入远端临时 ps1 再执行，避免 EncodedCommand 超过 8191 字符并过滤上传回显。

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, ok, deviceIp, loginUser, runId, targetName, scriptName, scriptWorkName, outputText, runError, outputFilePath, cancelRequested] {
            if (!self) {
                return;
            }

            DeviceGrid* grid = self.data();
            const QString targetIp = deviceIp.trimmed();
            ScriptUiState state = grid->m_scriptUiStateStore.state(targetIp);
            if (state.outputFilePath != outputFilePath
                || (!state.remoteRunId.isEmpty() && state.remoteRunId != runId)) {
                return;
            }
            const QString resultText = runError.trimmed().isEmpty() ? outputText.trimmed() : runError.trimmed();
            const bool canceled = cancelRequested && cancelRequested->load();
            writeScriptOutputFile(
                outputFilePath,
                QString::fromUtf8("\n状态: %1%2\n")
                    .arg(
                        canceled ? QString::fromUtf8("已停止") : (ok ? QString::fromUtf8("已完成") : QString::fromUtf8("执行失败")),
                        canceled
                            ? QString()
                            : ((!ok && !resultText.isEmpty()) ? QString::fromUtf8("\n%1").arg(resultText) : QString())),
                QIODevice::Append);
            state.outputVisible = true;
            state.outputRunning = false;
            state.outputFailed = !ok && !canceled;
            state.localLaunchInProgress = false;
            state.remoteStatusConfirmed = true;
            state.remoteRunId.clear();
            state.remoteControllerPid = 0;
            state.remoteStartedAtEpochMs = 0; // wjy: SSH 执行回调结束说明本次远端控制进程已退出，本地缓存同步收敛到已结束状态。
            state.outputScrollOffset = 0;
            state.outputAutoScroll = true;
            state.outputDirty = false;
            state.outputTitle = QString::fromUtf8("%1 - %2").arg(targetName, scriptName);
            state.outputText = stripTerminalControlSequences(readScriptOutputFileTail(outputFilePath));
            grid->m_scriptUiStateStore.setState(targetIp, state); // wjy: 结束状态写回目标设备，后台跑完后切回该设备能看到已完成/失败/停止。
            grid->update(deviceListViewportRect(grid->m_deviceGroupExpanded)); // wjy: 无论目标设备是否是当前详情页，都立即移除它在左侧列表里的运行图标。

            const bool targetIsCurrent = grid->currentScriptUiDeviceIp() == targetIp;
            if (targetIsCurrent) {
                grid->m_deviceDetailTab = DeviceDetailTab::ScriptLog;
                grid->m_scriptOutputVisible = state.outputVisible;
                grid->m_scriptOutputRunning = state.outputRunning;
                grid->m_scriptOutputFailed = state.outputFailed;
                grid->m_scriptOutputScrollOffset = state.outputScrollOffset;
                grid->m_scriptOutputAutoScroll = state.outputAutoScroll;
                grid->m_scriptOutputDirty = state.outputDirty;
                grid->m_scriptOutputTitle = state.outputTitle;
                grid->m_scriptOutputText = state.outputText;
                grid->update(scriptTerminalPanelRect().toAlignedRect().adjusted(-2, -2, 2, 2));
            }
            const bool editorModified = targetIsCurrent
                ? (grid->m_scriptFileEdit && grid->m_scriptFileEdit->document()->isModified())
                : state.editorModified;
            if (!editorModified) {
                grid->loadScriptFileEditor(deviceIp, loginUser, scriptWorkName); // wjy: 脚本结束后再刷新一次，保证首次复制较慢时编辑器也能拿到本地 json/txt。
            }
        }, Qt::QueuedConnection);
    });
    return true;
// ===end====
}

// =====wjy====
void DeviceGrid::batchExecuteDeviceScriptFolder(
    const QVector<int>& deviceIndexes,
    const QString& scriptEntryPath,
    const QString& noExecutableMessage)
{
    const QString validationPath = QDir::fromNativeSeparators(scriptEntryPath).trimmed().toLower();
    if (validationPath.isEmpty() || m_pendingScriptBatchValidationPaths.contains(validationPath)) {
        return; // wjy: 空路径或同一共享目录已经在验证时不重复创建批量后台任务。
    }

    QVector<QString> targetDeviceIds;
    for (const int deviceIndex : normalizedBatchDeviceIndexes(deviceIndexes)) {
        if (deviceIndex >= 0 && deviceIndex < g_devices.size()) {
            targetDeviceIds.append(g_devices.at(deviceIndex).id); // wjy: 后台验证期间只保存稳定 ID，设备同步排序不会改变最终目标集合。
        }
    }
    m_pendingScriptBatchValidationPaths.insert(validationPath);
    QPointer<DeviceGrid> self(this);
    try {
        runBackgroundTask([self, targetDeviceIds, scriptEntryPath, noExecutableMessage, validationPath] {
            const QFileInfo entryScript = scriptEntryFileForSelection(scriptEntryPath); // wjy: 批量入口预检只确认用户点选的文件，不再从目录中自动挑选其它脚本。
            const bool entryAvailable = entryScript.exists()
                && !scriptRunCommandForFile(entryScript).trimmed().isEmpty(); // wjy: 批量入口的唯一共享目录预检在后台执行，网盘无响应不会卡住菜单。
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(self, [self, targetDeviceIds, scriptEntryPath, noExecutableMessage, validationPath, entryAvailable] {
                if (!self) {
                    return;
                }
                DeviceGrid* grid = self.data();
                grid->m_pendingScriptBatchValidationPaths.remove(validationPath);
                if (!entryAvailable) {
                    QMessageBox::information(grid, QString(), QString::fromUtf8("无可用脚本"));
                    return; // wjy: 后台确认无入口后整批停止，只显示一次提示且不访问任何目标设备。
                }

                int scheduledCount = 0;
                for (const QString& deviceId : targetDeviceIds) {
                    const int currentIndex = g_deviceCatalog.deviceIndexForId(deviceId);
                    if (currentIndex >= 0
                        && grid->executeDeviceScriptFolder(currentIndex, scriptEntryPath, false)) {
                        ++scheduledCount; // wjy: 每个目标再执行自己的后台状态和授权预检查，脚本运行状态仍按设备 IP 完全隔离。
                    }
                }
                if (scheduledCount <= 0) {
                    QMessageBox::warning(grid, QString(), noExecutableMessage); // wjy: 所有目标已离线、正在运行或被去重时保留原批量提示。
                } else {
                    grid->update();
                }
            }, Qt::QueuedConnection);
        });
    } catch (...) {
        m_pendingScriptBatchValidationPaths.remove(validationPath);
        QMessageBox::warning(this, QString(), QString::fromUtf8("无法创建批量脚本检查后台任务。"));
    }
}
// ===end====

void DeviceGrid::executeDeviceGroupScriptFolder(int groupIndex, const QString& scriptEntryPath)
{
// =====wjy====
    if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return;
    }
    batchExecuteDeviceScriptFolder(
        deviceIndexesForGroup(groupIndex),
        scriptEntryPath,
        QString::fromUtf8("分组内没有可执行设备。")); // wjy: 分组入口也按稳定分组 ID 取得设备，并复用与多选设备完全相同的批量执行逻辑。
// ===end====
}

void DeviceGrid::openRemoteDesktopWindow()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    openRemoteDesktopWindowForDevice(m_selectedDeviceIndex); // wjy: 单设备首次远控使用窗口自身计算的屏幕中央位置。
}

// =====wjy====
void DeviceGrid::authorizeRemoteControlDevices(
    const QVector<QString>& deviceIds,
    bool showMessages,
    std::function<void(const QVector<QString>&)> completion)
{
    struct AuthorizationTarget {
        QString deviceId;
        QString ip;
    };
    struct AuthorizationOutcome {
        QString deviceId;
        QString ip;
        bool authorized = false;
        QString error;
    };

    QVector<QString> normalizedIds;
    QVector<AuthorizationTarget> targets;
    bool waitingForExistingTask = false;
    for (const QString& requestedId : deviceIds) {
        const QString deviceId = requestedId.trimmed();
        const int deviceIndex = g_deviceCatalog.deviceIndexForId(deviceId);
        if (deviceId.isEmpty() || deviceIndex < 0 || normalizedIds.contains(deviceId)) {
            continue;
        }
        const QString ip = g_devices.at(deviceIndex).ip.trimmed();
        if (ip.isEmpty()) {
            continue;
        }
        normalizedIds.append(deviceId);
        if (m_authorizedRemoteControlIps.contains(ip)) {
            continue; // wjy: 本进程已经成功登记过公钥的目标直接复用，不再重复连接 49102。
        }
        if (m_pendingRemoteControlAuthorizationIps.contains(ip)) {
            waitingForExistingTask = true;
            continue; // wjy: 其它普通或平铺入口正在授权该 IP，本次请求等待其完成后统一重试。
        }
        targets.append(AuthorizationTarget{deviceId, ip});
    }

    if (waitingForExistingTask) {
        QPointer<DeviceGrid> self(this);
        QTimer::singleShot(250, this, [self, normalizedIds, showMessages, completion = std::move(completion)]() mutable {
            if (self) {
                self->authorizeRemoteControlDevices(normalizedIds, showMessages, std::move(completion)); // wjy: 低频重试只检查内存去重状态，不访问网络也不阻塞事件循环。
            }
        });
        return;
    }

    if (targets.isEmpty()) {
        if (completion) {
            completion(normalizedIds); // wjy: 全部目标已经授权时同步进入窗口创建阶段，不额外启动空后台任务。
        }
        return;
    }

    for (const AuthorizationTarget& target : targets) {
        m_pendingRemoteControlAuthorizationIps.insert(target.ip); // wjy: 工作线程启动前登记所有 IP，三个远控入口共享同一去重集合。
    }
    QPointer<DeviceGrid> self(this);
    try {
        runBackgroundTask([self, normalizedIds, targets, showMessages, completion = std::move(completion)]() mutable {
            QVector<AuthorizationOutcome> outcomes;
            outcomes.reserve(targets.size());
            QString publicKeyError;
            const QString publicKey = platform::PortableOpenSshManager::instance().clientPublicKey(&publicKeyError); // wjy: 整批远控只准备和读取一次本机公钥，首次密钥生成不再冻结窗口。
            for (const AuthorizationTarget& target : targets) {
                AuthorizationOutcome outcome;
                outcome.deviceId = target.deviceId;
                outcome.ip = target.ip;
                if (publicKey.isEmpty()) {
                    outcome.error = publicKeyError;
                } else {
                    outcome.authorized = platform::DeviceCommandService::authorizeTerminalKey(
                        target.ip, publicKey, &outcome.error); // wjy: 每台设备的 49102 同步等待在单个后台批次中顺序执行，主线程只等待完成事件。
                }
                outcomes.append(std::move(outcome));
            }

            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(self, [self, normalizedIds, outcomes, showMessages, completion = std::move(completion)]() mutable {
                if (!self) {
                    return;
                }
                DeviceGrid* grid = self.data();
                QStringList failureDetails;
                for (const AuthorizationOutcome& outcome : outcomes) {
                    grid->m_pendingRemoteControlAuthorizationIps.remove(outcome.ip);
                    const int currentIndex = g_deviceCatalog.deviceIndexForId(outcome.deviceId);
                    const bool targetStillMatches = currentIndex >= 0
                        && g_devices.at(currentIndex).ip.trimmed().compare(outcome.ip, Qt::CaseInsensitive) == 0;
                    if (outcome.authorized && targetStillMatches) {
                        grid->m_authorizedRemoteControlIps.insert(outcome.ip); // wjy: 成功结果回到主线程后才发布缓存，窗口创建不会读取跨线程状态。
                    } else {
                        failureDetails.append(outcome.error.trimmed());
                    }
                }

                QVector<QString> authorizedIds;
                for (const QString& deviceId : normalizedIds) {
                    const int deviceIndex = g_deviceCatalog.deviceIndexForId(deviceId);
                    if (deviceIndex >= 0
                        && grid->m_authorizedRemoteControlIps.contains(g_devices.at(deviceIndex).ip.trimmed())) {
                        authorizedIds.append(deviceId); // wjy: 授权期间被删除或换 IP 的设备不会进入后续窗口创建。
                    }
                }
                if (showMessages && authorizedIds.size() < normalizedIds.size()) {
                    const QString firstDetail = failureDetails.value(0).trimmed();
                    const QString message = firstDetail.isEmpty()
                        ? QString::fromUtf8("无法在部分目标设备登记远控公钥，请确认设备在线且 49102 端口可用。")
                        : QString::fromUtf8("无法在部分目标设备登记远控公钥：%1").arg(firstDetail);
                    QMessageBox::warning(grid, QString(), message); // wjy: 批量授权失败只显示一次汇总，成功设备仍继续打开。
                }
                if (completion) {
                    completion(authorizedIds);
                }
            }, Qt::QueuedConnection);
        });
    } catch (...) {
        for (const AuthorizationTarget& target : targets) {
            m_pendingRemoteControlAuthorizationIps.remove(target.ip);
        }
        if (showMessages) {
            QMessageBox::warning(this, QString(), QString::fromUtf8("无法创建远控授权后台任务。"));
        }
    }
}
// ===end====

void DeviceGrid::openRemoteDesktopWindowForDevice(int deviceIndex)
{
    if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return;
    }

    const QString deviceIp = g_devices.at(deviceIndex).ip.trimmed();
    if (deviceIp.isEmpty()) {
        return;
    }
    QPointer<RemoteDesktopWindow> existingWindow = m_remoteWindowCoordinator->normalWindow(deviceIp); // wjy: 普通远控入口通过协调器按 IP 去重，避免 DeviceGrid 直接维护窗口哈希。
    if (existingWindow && !existingWindow->isClosingConnection()) {
        if (existingWindow->isMinimized()) {
            existingWindow->showNormal();
        }
        existingWindow->show();
        existingWindow->raise();
        existingWindow->activateWindow();
        existingWindow->setFocus(Qt::ActiveWindowFocusReason);
        rememberRemoteWindowActivation(existingWindow.data());
        return;
    }
    if (!m_authorizedRemoteControlIps.contains(deviceIp)) {
        const QString deviceId = g_devices.at(deviceIndex).id;
        authorizeRemoteControlDevices(QVector<QString>{deviceId}, true,
            [this, deviceId](const QVector<QString>& authorizedIds) {
                if (!authorizedIds.contains(deviceId)) {
                    return;
                }
                const int currentIndex = g_deviceCatalog.deviceIndexForId(deviceId);
                if (currentIndex >= 0) {
                    openRemoteDesktopWindowForDevice(currentIndex); // wjy: 后台授权成功后重新解析稳定 ID，再回到原窗口创建入口并命中授权缓存。
                }
            });
        return; // wjy: 当前点击只安排后台授权，鼠标事件不等待公钥准备或 49102 响应。
    }

    auto* remoteWindow = new RemoteDesktopWindow(
        deviceDisplayName(g_devices.at(deviceIndex)),
        deviceIp,
        m_remoteViewerLifecycleManager.get(),
        &m_remoteInputBroadcastCoordinator); // wjy: 普通远控窗口同时接入共享生命周期管理器和唯一键鼠同步协调器。
    m_remoteWindowCoordinator->registerNormalWindow(deviceIp, remoteWindow); // wjy: 新普通窗口登记到协调器，后续重复打开只激活它。
    publishRemoteControllerTarget(remoteWindow, deviceDisplayName(g_devices.at(deviceIndex)), deviceIp); // wjy: 窗口建立即发布本机正在控制的目标租约，目标人数仍只看目标主机自己的会话表。
    connect(remoteWindow, &QObject::destroyed, this, [this, deviceIp, remoteWindow] {
        removeRemoteControllerTarget(remoteWindow); // wjy: 异常关闭或正常关闭都由 QObject 生命周期删除同一租约。
        m_remoteWindowCoordinator->removeWindow(deviceIp, remoteWindow); // wjy: 销毁回调统一清理普通映射、激活顺序和平铺恢复几何。
    });
    connect(remoteWindow, &RemoteDesktopWindow::activated, this, &DeviceGrid::rememberRemoteWindowActivation);
    // =====wjy====
    connect(remoteWindow, &RemoteDesktopWindow::titleBarContextMenuRequested,
        this, &DeviceGrid::showRemoteWindowDeviceMenu); // wjy: 普通远控窗口右键标题栏时按该窗口 IP 打开共享设备菜单。
    connect(remoteWindow, &RemoteDesktopWindow::titleBarUpdateRequested,
        this, &DeviceGrid::updateRemoteWindowDevice); // wjy: 普通远控窗口更新按钮按自身 IP 复用单设备更新逻辑。
    remoteWindow->setRemoteUpdateAvailable(m_deviceUpdateAvailability.value(deviceIp, false)); // wjy: 窗口创建时立即使用最近一次状态刷新结果，不等待下一轮定时刷新。
    registerRemoteQualityWindow(remoteWindow); // wjy: 普通窗口纳入控制端统一质量预算，最终在线数量不设上限。
    // ===end====
    connect(remoteWindow, &RemoteDesktopWindow::shortcutFullscreenRequested, this, [this] { triggerShortcutAction(0); });
    connect(remoteWindow, &RemoteDesktopWindow::shortcutTileRequested, this, [this] { triggerShortcutAction(1); });
    connect(remoteWindow, &RemoteDesktopWindow::shortcutCloseTopmostRequested, this, [this] { triggerShortcutAction(2); });
    connect(remoteWindow, &RemoteDesktopWindow::shortcutCloseAllRequested, this, [this] { triggerShortcutAction(3); });
    connect(remoteWindow, &RemoteDesktopWindow::shortcutClipboardSyncRequested, this, [this] { triggerShortcutAction(4); });
    remoteWindow->show();
    remoteWindow->raise();
    remoteWindow->activateWindow();
    remoteWindow->setFocus(Qt::ActiveWindowFocusReason);
    rememberRemoteWindowActivation(remoteWindow);
}

void DeviceGrid::launchSelectedRemoteDesktopWindows()
{
    QVector<int> launchIndexes;
    const QVector<DeviceListRow> rows = visibleDeviceRows(m_deviceStatuses); // wjy: 批量远控目标按当前在线优先自然排序后的可见顺序收集。
    for (const DeviceListRow& row : rows) {
        if (row.type == DeviceListRow::Type::Device
            && row.deviceIndex >= 0
            && row.deviceIndex < g_devices.size()
            && m_selectedDeviceIndexes.contains(row.deviceIndex)) {
            launchIndexes.append(row.deviceIndex);
        }
    }
    if (launchIndexes.isEmpty()
        && m_selectedDeviceIndex >= 0
        && m_selectedDeviceIndex < g_devices.size()) {
        launchIndexes.append(m_selectedDeviceIndex);
    }

    QVector<QString> launchDeviceIds;
    for (const int deviceIndex : launchIndexes) {
        if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
            continue;
        }
        if (devicePresenceForIndex(deviceIndex) == platform::DevicePresenceState::Offline
            && !devicePoweringOnForIndex(deviceIndex)) {
            wakeDeviceForIndex(deviceIndex, false); // wjy: 多选入口直接按真实下标安排唤醒，不再临时修改用户当前详情选择。
        }
        launchDeviceIds.append(g_devices.at(deviceIndex).id);
    }
    authorizeRemoteControlDevices(launchDeviceIds, true,
        [this](const QVector<QString>& authorizedIds) {
            for (const QString& deviceId : authorizedIds) {
                const int currentIndex = g_deviceCatalog.deviceIndexForId(deviceId);
                if (currentIndex >= 0) {
                    openRemoteDesktopWindowForDevice(currentIndex); // wjy: 整批授权结束后按稳定 ID 创建普通窗口，设备排序变化不会打开错误目标。
                }
            }
        }); // wjy: 多选打开整批只生成一个后台授权任务，目标数量不再线性冻结主窗口。
}

void DeviceGrid::openDeviceGroupTiledWindows(int groupIndex)
{
// =====wjy====
    if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return; // wjy: 分组下标无效时直接返回，避免右键菜单持有的旧下标导致越界。
    }

    const QString groupName = g_deviceGroupNames.at(groupIndex).trimmed(); // wjy: 用分组名筛选设备，和 devices.json 里的 group 字段保持一致。
    if (groupName.isEmpty()) {
        return; // wjy: 空分组名没有稳定匹配依据，不打开任何窗口。
    }

    QVector<QString> groupDeviceIds;
    for (const int deviceIndex : deviceIndexesForGroup(groupIndex)) {
        groupDeviceIds.append(g_devices.at(deviceIndex).id); // wjy: 平铺只收集当前界面可见设备，隐藏本机不会在后台被重新打开远控窗口。
    }
    if (groupDeviceIds.isEmpty()) {
        return; // wjy: 空分组暂时不弹提示，点击设备平铺没有可打开目标就静默返回。
    }

    authorizeRemoteControlDevices(groupDeviceIds, true,
        [this](const QVector<QString>& authorizedIds) {
            openAuthorizedTiledWindows(authorizedIds); // wjy: 整批公钥登记完成后才关闭旧平铺并创建新网格，授权期间原窗口保持可用。
        });
// ===end====
}

// =====wjy====
void DeviceGrid::openAuthorizedTiledWindows(const QVector<QString>& deviceIds)
{
    QVector<int> groupDeviceIndexes;
    for (const QString& deviceId : deviceIds) {
        const int deviceIndex = g_deviceCatalog.deviceIndexForId(deviceId);
        if (deviceIndex >= 0) {
            groupDeviceIndexes.append(deviceIndex); // wjy: 后台授权结果回到主线程后重新解析当前真实下标，被删除设备自动跳过。
        }
    }
    if (groupDeviceIndexes.isEmpty()) {
        return;
    }

    m_remoteWindowCoordinator->closeTiledWindows(); // wjy: 只有至少一台设备授权成功时才替换上一批平铺，弱网失败不会把原窗口全部关掉。

    QScreen* screen = window() ? window()->screen() : QGuiApplication::primaryScreen(); // wjy: 优先使用主窗口所在屏幕，多屏时窗口会铺在当前程序所在显示器。
    if (!screen) {
        screen = QGuiApplication::primaryScreen(); // wjy: 防御性兜底，避免极端情况下没有窗口屏幕对象。
    }
    const QRect availableRect = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720); // wjy: 获取去掉任务栏后的可用屏幕区域，用它计算平铺位置。
    const int deviceCount = groupDeviceIndexes.size();
    const int gridSize = qMax(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(deviceCount))))); // wjy: 设备数量开方取上整，形成 1x1、2x2、3x3 这种方阵网格。
    const int columnCount = gridSize; // wjy: 列数等于方阵边长，保证布局是 2x2、3x3 这类平铺。
    const int rowCount = gridSize; // wjy: 行数也等于方阵边长，设备不足时只是后面的格子空着。
    const int tileWidth = qMax(1, availableRect.width() / columnCount); // wjy: 完全按可用屏幕宽度均分，不再用 320px 下限把多列窗口顶到重叠。
    const int tileHeight = qMax(1, availableRect.height() / rowCount); // wjy: 完全按可用屏幕高度均分，12 台等大分组也能保持在屏幕范围内。

    for (int tileIndex = 0; tileIndex < groupDeviceIndexes.size(); ++tileIndex) {
        const int deviceIndex = groupDeviceIndexes.at(tileIndex); // wjy: 当前要打开的真实设备下标。
        if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
            continue; // wjy: 防御性跳过异常下标。
        }

        const int row = tileIndex / columnCount; // wjy: 计算当前窗口位于第几行。
        const int column = tileIndex % columnCount; // wjy: 计算当前窗口位于第几列。
        QRect targetRect(
            availableRect.x() + column * tileWidth,
            availableRect.y() + row * tileHeight,
            tileWidth,
            tileHeight);
        if (column == columnCount - 1) {
            targetRect.setRight(availableRect.right()); // wjy: 最后一列贴齐屏幕右边缘，吸收整除误差。
        }
        if (row == rowCount - 1) {
            targetRect.setBottom(availableRect.bottom()); // wjy: 最后一行贴齐屏幕底边缘，吸收整除误差。
        }

        auto* remoteWindow = new RemoteDesktopWindow(
            deviceDisplayName(g_devices.at(deviceIndex)),
            g_devices.at(deviceIndex).ip,
            m_remoteViewerLifecycleManager.get(),
            &m_remoteInputBroadcastCoordinator); // wjy: 平铺窗口与普通窗口处于同一同步成员集合，任一标题栏按钮都能原子切换主控。
        remoteWindow->setRememberGeometryEnabled(false);
        m_remoteWindowCoordinator->registerTiledWindow(remoteWindow); // wjy: 记录本次平铺创建的窗口，下一次设备平铺前统一关闭并重排。
        publishRemoteControllerTarget(remoteWindow, deviceDisplayName(g_devices.at(deviceIndex)), g_devices.at(deviceIndex).ip); // wjy: 每个平铺窗口拥有独立诊断租约，不会与普通窗口覆盖。
        connect(remoteWindow, &QObject::destroyed, this, [this, remoteWindow] {
            removeRemoteControllerTarget(remoteWindow);
            m_remoteWindowCoordinator->removeWindow(remoteWindow); // wjy: 平铺窗口销毁时统一清理平铺集合、激活顺序和恢复几何。
        });
        connect(remoteWindow, &RemoteDesktopWindow::activated, this, &DeviceGrid::rememberRemoteWindowActivation);
        // =====wjy====
        connect(remoteWindow, &RemoteDesktopWindow::titleBarContextMenuRequested,
            this, &DeviceGrid::showRemoteWindowDeviceMenu); // wjy: 分组平铺创建的远控窗口也按各自 IP 弹出菜单，不会串到其它格子设备。
        connect(remoteWindow, &RemoteDesktopWindow::titleBarUpdateRequested,
            this, &DeviceGrid::updateRemoteWindowDevice); // wjy: 平铺窗口同样只更新自身绑定设备。
        remoteWindow->setRemoteUpdateAvailable(m_deviceUpdateAvailability.value(g_devices.at(deviceIndex).ip.trimmed(), false)); // wjy: 平铺创建后同步显示该设备最近确认的更新状态。
        registerRemoteQualityWindow(remoteWindow); // wjy: 平铺小窗口按实际显示高度降分辨率并优先保持FPS，所有窗口仍持续不断流。
        // ===end====
        connect(remoteWindow, &RemoteDesktopWindow::shortcutFullscreenRequested, this, [this] { triggerShortcutAction(0); });
        connect(remoteWindow, &RemoteDesktopWindow::shortcutTileRequested, this, [this] { triggerShortcutAction(1); });
        connect(remoteWindow, &RemoteDesktopWindow::shortcutCloseTopmostRequested, this, [this] { triggerShortcutAction(2); });
        connect(remoteWindow, &RemoteDesktopWindow::shortcutCloseAllRequested, this, [this] { triggerShortcutAction(3); });
        // wjy: 不再为平铺窗口设置 240x180 最小尺寸，targetRect 的网格尺寸可以被 QWidget 原样采用。
        remoteWindow->setGeometry(targetRect); // wjy: 按网格设置窗口位置和大小，形成 2x2、3x3 等平铺效果。
        remoteWindow->show();
        remoteWindow->raise();
        remoteWindow->activateWindow();
        remoteWindow->setFocus(Qt::ActiveWindowFocusReason);
        rememberRemoteWindowActivation(remoteWindow);
    }
// ===end====
}

QVector<QPointer<RemoteDesktopWindow>> DeviceGrid::openedRemoteWindows() const
{
    return m_remoteWindowCoordinator->openedWindows(); // wjy: 普通/平铺/激活窗口的去重和 closing 过滤由协调器统一完成。
}

void DeviceGrid::setRemoteUpdateAvailability(const QString& hostIp, bool available)
{
    const QString targetIp = hostIp.trimmed();
    if (targetIp.isEmpty()) {
        return;
    }
    const QVector<QPointer<RemoteDesktopWindow>> windows = openedRemoteWindows();
    bool updateActive = false;
    for (const QPointer<RemoteDesktopWindow>& window : windows) {
        if (window && window->hostIp().compare(targetIp, Qt::CaseInsensitive) == 0
            && window->isRemoteUpdateActive()) {
            updateActive = true;
            break; // wjy: 查询结果可能晚于用户点击，更新状态机已启动时禁止旧结果重新点亮按钮。
        }
    }
    const bool effectiveAvailable = available && !updateActive;
    m_deviceUpdateAvailability.insert(targetIp, effectiveAvailable); // wjy: 缓存供稍后新建的普通或平铺远控窗口直接使用。
    for (const QPointer<RemoteDesktopWindow>& window : windows) {
        if (window && window->hostIp().compare(targetIp, Qt::CaseInsensitive) == 0) {
            window->setRemoteUpdateAvailable(effectiveAvailable); // wjy: 同一设备可能同时存在多个窗口，状态刷新时统一显示或隐藏按钮。
        }
    }
}

bool DeviceGrid::realtimeUpdateAvailable(const platform::DeviceRealtimeUpdateState& updateState) const
{
    const QString confirmedRelease = m_availableUpdateVersion.trimmed();
    const QString installedVersion = updateState.installedVersion.trimmed();
    if (confirmedRelease.isEmpty() || installedVersion.isEmpty()) {
        return false;
    }
    const int comparison = platform::UpdateService::compareSemanticVersions(confirmedRelease, installedVersion);
    return comparison > 0 || (comparison == 0 && updateState.runtimeRepairRequired);
}

void DeviceGrid::refreshRealtimeUpdateAvailability()
{
    for (auto it = m_deviceRealtimeUpdateStates.cbegin(); it != m_deviceRealtimeUpdateStates.cend(); ++it) {
        setRemoteUpdateAvailability(it.key(), realtimeUpdateAvailable(it.value()));
    }
}

void DeviceGrid::rememberRemoteWindowActivation(RemoteDesktopWindow* window)
{
    m_remoteWindowCoordinator->rememberActivation(window); // wjy: 激活顺序由协调器维护，快捷键不再依赖 DeviceGrid 容器。
    requestRemoteQualityEvaluation(); // wjy: 激活变化只触发一次焦点策略重算，高质量保留者由真实焦点决定。
}

RemoteDesktopWindow* DeviceGrid::topmostRemoteWindow() const
{
    return m_remoteWindowCoordinator->topmostWindow(); // wjy: 协调器优先返回最近激活且仍可用的窗口。
}

void DeviceGrid::toggleTopmostRemoteWindowFullscreen()
{
    RemoteDesktopWindow* window = topmostRemoteWindow();
    if (!window) {
        return;
    }
    rememberRemoteWindowActivation(window);
    window->isFullScreen() ? window->showNormal() : window->showFullScreen();
    window->raise();
    window->activateWindow();
}

void DeviceGrid::toggleRemoteWindowTiling()
{
    QVector<QPointer<RemoteDesktopWindow>> windows = openedRemoteWindows();
    for (auto it = windows.begin(); it != windows.end();) {
        if (!*it || (*it)->isClosingConnection()) {
            it = windows.erase(it);
        } else {
            ++it;
        }
    }
    if (windows.isEmpty()) {
        return;
    }

    // =====wjy====
    QCollator naturalCollator; // wjy: 使用系统排序规则并启用数字模式，让设备名中的 4 排在 15 前面。
    naturalCollator.setCaseSensitivity(Qt::CaseInsensitive); // wjy: 字母排序忽略大小写，避免同一名称因大小写分散。
    naturalCollator.setNumericMode(true); // wjy: 连续数字按数值比较，而不是按字符串逐字符比较。
    std::sort(windows.begin(), windows.end(), [&naturalCollator](const QPointer<RemoteDesktopWindow>& left,
                                                                  const QPointer<RemoteDesktopWindow>& right) {
        if (!left || !right) {
            return static_cast<bool>(left); // wjy: 有效窗口优先，保证后续布局索引始终对应有效对象。
        }
        const QString leftName = left->deviceName();
        const QString rightName = right->deviceName();
        const bool leftStartsWithDigit = !leftName.isEmpty() && leftName.front().isDigit();
        const bool rightStartsWithDigit = !rightName.isEmpty() && rightName.front().isDigit();
        if (leftStartsWithDigit != rightStartsWithDigit) {
            return leftStartsWithDigit; // wjy: 数字开头设备整体排在字母开头设备之前。
        }
        const int nameCompare = naturalCollator.compare(leftName, rightName);
        if (nameCompare != 0) {
            return nameCompare < 0; // wjy: 同一类别内按自然名称顺序排列，确保从左到右再从上到下稳定落位。
        }
        const int exactNameCompare = QString::compare(leftName, rightName, Qt::CaseSensitive);
        if (exactNameCompare != 0) {
            return exactNameCompare < 0; // wjy: 忽略大小写相同的名称再按原始文本确定顺序。
        }
        return QString::compare(left->hostIp(), right->hostIp(), Qt::CaseInsensitive) < 0; // wjy: 同名设备按 IP 地址排序，避免使用随机顺序。
    });
    // ===end====

    if (m_remoteWindowCoordinator->windowsTiled()) {
        for (const QPointer<RemoteDesktopWindow>& window : windows) {
            if (!window) {
                continue;
            }
            window->showNormal();
            const QRect restoreGeometry = m_remoteWindowCoordinator->restoreGeometry(window.data());
            if (restoreGeometry.isValid()) {
                window->setGeometry(restoreGeometry);
            }
            window->setRememberGeometryEnabled(true);
            window->show();
        }
        m_remoteWindowCoordinator->clearRestoreGeometries();
        m_remoteWindowCoordinator->setWindowsTiled(false);
        return;
    }

    QScreen* screen = window() ? window()->screen() : QGuiApplication::primaryScreen();
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    m_remoteWindowCoordinator->clearRestoreGeometries();
    QHash<QString, QVector<QPointer<RemoteDesktopWindow>>> desktopGroups;
    for (const QPointer<RemoteDesktopWindow>& window : windows) {
        if (!window) {
            continue;
        }
#if defined(Q_OS_WIN)
        const QString desktopKey = virtualDesktopKey(window.data()); // wjy: 按 Windows 虚拟桌面归属拆分窗口，避免跨桌面总数量污染网格列数。
#else
        const QString desktopKey = QStringLiteral("default");
#endif
        desktopGroups[desktopKey].append(window);
    }
    for (auto groupIt = desktopGroups.begin(); groupIt != desktopGroups.end(); ++groupIt) {
        const QVector<QPointer<RemoteDesktopWindow>>& group = groupIt.value();
        if (group.isEmpty()) {
            continue;
        }
        QScreen* groupScreen = group.first()->screen();
        const QRect availableRect = groupScreen ? groupScreen->availableGeometry() : QRect(0, 0, 1280, 720);
        const int count = group.size();
        const int columnCount = qMax(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))))); // wjy: 每个虚拟桌面独立计算列数。
        const int rowCount = qMax(1, static_cast<int>(std::ceil(count / static_cast<double>(columnCount)))); // wjy: 每个虚拟桌面独立计算行数。
        const int tileWidth = qMax(1, availableRect.width() / columnCount);
        const int tileHeight = qMax(1, availableRect.height() / rowCount);
        for (int i = 0; i < group.size(); ++i) {
            RemoteDesktopWindow* remoteWindow = group.at(i).data();
            if (!remoteWindow) {
                continue;
            }
            m_remoteWindowCoordinator->setRestoreGeometry(remoteWindow, remoteWindow->geometry());
            remoteWindow->setRememberGeometryEnabled(false);
            remoteWindow->showNormal();
            const int row = i / columnCount;
            const int column = i % columnCount;
            QRect target(availableRect.x() + column * tileWidth, availableRect.y() + row * tileHeight, tileWidth, tileHeight);
            if (column == columnCount - 1) target.setRight(availableRect.right());
            if (row == rowCount - 1) target.setBottom(availableRect.bottom());
            remoteWindow->setGeometry(target);
            remoteWindow->show();
            remoteWindow->raise();
            remoteWindow->activateWindow();
            remoteWindow->setFocus(Qt::ActiveWindowFocusReason);
        }
    }
    m_remoteWindowCoordinator->setWindowsTiled(true);
}

void DeviceGrid::closeTopmostRemoteWindow()
{
    RemoteDesktopWindow* window = topmostRemoteWindow();
    if (!window) {
        return;
    }
    window->close();
}

void DeviceGrid::closeAllRemoteWindows()
{
    m_remoteWindowCoordinator->closeAllWindows(); // wjy: close-all 的窗口遍历、去重和状态清理由协调器集中负责。
}

void DeviceGrid::shutdownCurrentDevice()
{
    shutdownDeviceForIndex(m_selectedDeviceIndex, true);
}

bool DeviceGrid::shutdownDeviceForIndex(int deviceIndex, bool showMessages)
{
    return scheduleDevicePowerActions(QVector<int>{deviceIndex}, false, showMessages); // wjy: 单设备关机也走后台批次，调用方立即返回“已安排”而不是等待 TCP。
}

void DeviceGrid::restartCurrentDevice()
{
    restartDeviceForIndex(m_selectedDeviceIndex, true);
}

bool DeviceGrid::restartDeviceForIndex(int deviceIndex, bool showMessages)
{
    return scheduleDevicePowerActions(QVector<int>{deviceIndex}, true, showMessages); // wjy: 单设备重启与批量重启共用后台发送和 UI 结果归并。
}

// =====wjy====
bool DeviceGrid::scheduleDevicePowerActions(const QVector<int>& deviceIndexes, bool restart, bool showMessages)
{
    QStringList targetIps;
    const platform::DeviceActionKind actionKind = restart
        ? platform::DeviceActionKind::Restart
        : platform::DeviceActionKind::Shutdown; // wjy: UI 线程先按当前权威状态筛选目标，后台线程只接收不可变 IP 列表。
    for (const int deviceIndex : deviceIndexes) {
        if (deviceIndex < 0 || deviceIndex >= g_devices.size()
            || !platform::isDeviceActionAllowed(actionKind, devicePresenceForIndex(deviceIndex))) {
            continue;
        }
        const QString ip = g_devices.at(deviceIndex).ip.trimmed();
        if (ip.isEmpty() || m_pendingPowerActionIps.contains(ip) || targetIps.contains(ip)) {
            continue; // wjy: 空 IP、当前已有电源命令或批次内重复目标都不能再次发送。
        }
        targetIps.append(ip);
        m_pendingPowerActionIps.insert(ip); // wjy: 后台任务登记前先占位，用户连续点击也只能形成一份命令。
    }
    if (targetIps.isEmpty()) {
        return false;
    }

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, targetIps, restart, showMessages] {
        QStringList succeededIps;
        QStringList failedIps;
        const platform::DeviceControlAction action = restart
            ? platform::DeviceControlAction::Restart
            : platform::DeviceControlAction::Shutdown;
        for (const QString& ip : targetIps) {
            (platform::DeviceCommandService::send(ip, action) ? succeededIps : failedIps).append(ip); // wjy: TCP 等待全部留在单个后台批次，弱网耗时不再占用 Qt 主事件循环。
        }
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, targetIps, succeededIps, failedIps, restart, showMessages] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            for (const QString& ip : targetIps) {
                grid->m_pendingPowerActionIps.remove(ip); // wjy: 成功和失败都释放去重占位，网络恢复后可以再次操作。
            }
            for (const QString& ip : succeededIps) {
                if (deviceIndexForIp(ip) < 0) {
                    continue; // wjy: 命令等待期间设备已被删除时不重新创建任何按 IP 的状态缓存。
                }
                grid->m_deviceStatuses.insert(ip, platform::DevicePresenceState::Offline);
                grid->m_deviceRemoteSessionCounts.insert(ip, 0);
                grid->m_deviceRemoteControllerNames.insert(ip, {});
                grid->m_deviceRealtimeScriptStates.insert(ip, platform::RealtimeScriptState::Unknown); // wjy: 命令已受理后立即清除在线会话和脚本缓存，等待目标重启后的新快照。
            }
            grid->update();
            if (showMessages && !failedIps.isEmpty()) {
                QMessageBox::warning(grid, QString(), restart
                        ? QString::fromUtf8("无法发送远程重启命令。")
                        : QString::fromUtf8("无法发送远程关机命令。")); // wjy: 单设备入口保留原失败提示，批量入口继续静默处理个别离线目标。
            }
        }, Qt::QueuedConnection);
    });
    return true;
}
// ===end====

// =====wjy====
bool DeviceGrid::updateDeviceForIndex(int deviceIndex, bool showMessages)
{
    return scheduleDeviceUpdateRequests(QVector<int>{deviceIndex}, showMessages); // wjy: 单设备更新只负责安排后台请求，结果提示和远控遮罩统一回到主线程处理。
}

bool DeviceGrid::scheduleDeviceUpdateRequests(const QVector<int>& deviceIndexes, bool showMessages)
{
    struct UpdateTarget {
        QString ip;
        QString expectedVersion;
    };
    struct UpdateOutcome {
        QString ip;
        platform::RemoteUpdateRequestResult result = platform::RemoteUpdateRequestResult::Failed;
        QString error;
    };

    QVector<UpdateTarget> targets;
    for (const int deviceIndex : deviceIndexes) {
        if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
            continue;
        }
        const QString ip = g_devices.at(deviceIndex).ip.trimmed();
        if (ip.isEmpty() || m_pendingUpdateRequestIps.contains(ip)) {
            continue;
        }
        bool duplicate = false;
        for (const UpdateTarget& target : targets) {
            duplicate = duplicate || target.ip.compare(ip, Qt::CaseInsensitive) == 0;
        }
        if (duplicate) {
            continue;
        }
        targets.append(UpdateTarget{ip, m_availableUpdateVersion.trimmed()});
        m_pendingUpdateRequestIps.insert(ip); // wjy: 请求发送前占用目标 IP，列表菜单和远控标题栏连续点击不会并发请求同一设备。
    }
    if (targets.isEmpty()) {
        return false;
    }

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, targets, showMessages] {
        QVector<UpdateOutcome> outcomes;
        outcomes.reserve(targets.size());
        for (const UpdateTarget& target : targets) {
            UpdateOutcome outcome;
            outcome.ip = target.ip;
            outcome.result = platform::DeviceCommandService::requestUpdate(
                target.ip, &outcome.error, 49102, 1500, target.expectedVersion); // wjy: 控制端确认版本随明确请求传递，目标缓存未刷新时仍由共享目录完成最终校验。
            outcomes.append(std::move(outcome));
        }
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, outcomes, showMessages] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            for (const UpdateOutcome& outcome : outcomes) {
                grid->m_pendingUpdateRequestIps.remove(outcome.ip);
                if (deviceIndexForIp(outcome.ip) < 0) {
                    continue; // wjy: 后台请求返回前设备已删除时丢弃结果，旧 IP 不会重新出现在更新缓存或窗口状态中。
                }
                if (outcome.result == platform::RemoteUpdateRequestResult::Accepted) {
                    bool remoteWindowNotified = false;
                    grid->setRemoteUpdateAvailability(outcome.ip, false); // wjy: 目标受理后立即隐藏同 IP 的全部更新按钮，等待窗口进入统一更新状态机。
                    for (const QPointer<RemoteDesktopWindow>& window : grid->openedRemoteWindows()) {
                        if (!window || window->hostIp() != outcome.ip) {
                            continue;
                        }
                        window->beginRemoteUpdateWait();
                        remoteWindowNotified = true;
                    }
                    if (showMessages && !remoteWindowNotified) {
                        QMessageBox::information(grid, QString(), QString::fromUtf8("目标设备已受理更新请求，准备完成后将自动退出并重启。"));
                    }
                    continue;
                }
                if (outcome.result == platform::RemoteUpdateRequestResult::UpToDate) {
                    grid->setRemoteUpdateAvailability(outcome.ip, false);
                    if (showMessages) {
                        QMessageBox::information(grid, QString(), QString::fromUtf8("目标设备已经是最新版本。"));
                    }
                    continue;
                }
                if (showMessages) {
                    const QString detail = outcome.error.trimmed().isEmpty()
                        ? QString::fromUtf8("无法向目标设备发送更新请求。")
                        : QString::fromUtf8("无法向目标设备发送更新请求：%1").arg(outcome.error.trimmed());
                    QMessageBox::warning(grid, QString(), detail); // wjy: 单设备入口保留具体错误，批量入口不会因个别目标失败连续弹窗。
                }
            }
        }, Qt::QueuedConnection);
    });
    return true;
}
// ===end====

void DeviceGrid::applyDeviceRename(int deviceIndex, const QString& newName)
{
    if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return;
    }
    if (newName.trimmed().isEmpty()) {
        return;
    }

    const QString normalizedNewName = newName.trimmed();
    const QString oldName = g_devices.at(deviceIndex).name.trimmed(); // wjy: 保存旧名字，远端改名命令明确失败时用于回滚本地显示。
    if (normalizedNewName == oldName) {
        return; // wjy: 名字没有变化时不写 devices.json，也不发送远端改名命令。
    }

    const QString targetIp = g_devices.at(deviceIndex).ip.trimmed();
    const QString targetDeviceId = g_devices.at(deviceIndex).id; // wjy: 异步重命名回调携带稳定设备 ID，设备排序后仍能回滚原目标。
    const bool shouldRenameRemote = !targetIp.isEmpty()
        && devicePresenceForIndex(deviceIndex) != platform::DevicePresenceState::Offline; // wjy: 离线设备只改本地显示名，在线设备才尝试同步远端电脑名。
    if (!g_deviceCatalog.renameDevice(targetDeviceId, normalizedNewName)) {
        return; // wjy: 目录拒绝无效稳定 ID 时不保存半成品名称，避免 UI 下标和权威数据分叉。
    }
    saveDevices();
    if (deviceIndex == m_selectedDeviceIndex) {
        m_currentDeviceName = normalizedNewName;
        m_previousDeviceName = normalizedNewName;
    }
    update();

    if (shouldRenameRemote) {
        m_pendingRemoteRenameNames.insert(targetIp, normalizedNewName); // wjy: Windows 改电脑名通常要重启才从状态服务返回新名，先阻止自动刷新用旧名覆盖。
        QPointer<DeviceGrid> self(this);
        runBackgroundTask([self, targetDeviceId, targetIp, oldName, normalizedNewName] {
            QString errorMessage;
            const bool renamed = platform::DeviceCommandService::renameDevice(targetIp, normalizedNewName, &errorMessage);
            qWarning().noquote() << QStringLiteral("[rename-device] remote ip=%1 renamed=%2 error=%3")
                .arg(targetIp)
                .arg(renamed)
                .arg(errorMessage.trimmed()); // wjy: 本地名称先保存，远端电脑名修改结果写日志；Windows 通常需要重启后完全生效。
            if (!self) {
                return;
            }
            if (renamed) {
                return; // wjy: 远端已接受改名请求，等待目标系统重启后状态服务上报新电脑名。
            }
            QMetaObject::invokeMethod(self, [self, targetDeviceId, targetIp, oldName, normalizedNewName, errorMessage] {
                if (!self) {
                    return;
                }

                DeviceGrid* grid = self.data();
                if (grid->m_pendingRemoteRenameNames.value(targetIp).trimmed() == normalizedNewName) {
                    grid->m_pendingRemoteRenameNames.remove(targetIp); // wjy: 命令明确失败后取消待生效保护，让后续刷新恢复正常同步。
                }

                const bool reverted = g_deviceCatalog.renameDevice(targetDeviceId, oldName); // wjy: 远端失败时按原稳定 ID 回滚，不能按 IP 或旧数组下标误改其它设备。
                if (reverted) {
                    saveDevices();
                    if (grid->m_selectedDeviceIndex >= 0 && grid->m_selectedDeviceIndex < g_devices.size()) {
                        grid->m_currentDeviceName = deviceDisplayName(g_devices.at(grid->m_selectedDeviceIndex));
                        grid->m_previousDeviceName = grid->m_currentDeviceName;
                    }
                    grid->update();
                }

                QMessageBox messageBox(
                    QMessageBox::Warning,
                    QString(),
                    errorMessage.trimmed().isEmpty()
                        ? QString::fromUtf8("目标设备重命名失败。")
                        : QString::fromUtf8("目标设备重命名失败：%1").arg(errorMessage.trimmed()),
                    QMessageBox::NoButton,
                    grid);
                messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
                messageBox.exec();
            }, Qt::QueuedConnection);
        });
    }
}

void DeviceGrid::renameCurrentDevice()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    beginDeviceRename(m_selectedDeviceIndex); // wjy: 设备重命名改为左侧列表原地编辑，和分组双击重命名保持一致。
}

void DeviceGrid::deleteCurrentDevice()
{
    deleteDeviceForIndex(m_selectedDeviceIndex); // wjy: 详情页删除按钮和 Delete 键统一调用按下标删除入口。
}

void DeviceGrid::deleteDeviceForIndex(int deviceIndex)
{
    // =====wjy====
    if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return;
    }

    const int previousSelectedIndex = m_selectedDeviceIndex; // wjy: 删除目标可能来自远控标题栏，先保存主界面当前选择以便删除后校正下标。
    const QString removedIp = g_devices.at(deviceIndex).ip.trimmed();
    const QString removedDeviceId = g_devices.at(deviceIndex).id; // wjy: 删除操作先记录稳定设备 ID，后续目录移除不依赖数组位置。
    saveCurrentScriptUiState(); // wjy: 删除当前设备前先收拢编辑框未保存文本和终端状态，随后按 IP 清理缓存。
    g_deviceCatalog.removeDevice(removedDeviceId); // wjy: 只从目录设备集合移除，不向目标设备发送任何关机、卸载或删除文件命令。
    saveDevices();
    updateRealtimeConfiguredDevices(); // wjy: 删除后立即拒绝该 IP 的后续广播并清理服务端序号/TTL 状态。
    m_deviceStatuses.remove(removedIp);
    m_deviceUpdateAvailability.remove(removedIp); // wjy: 删除设备时同步清理更新按钮缓存，避免以后复用 IP 继承旧状态。
    m_deviceRealtimeUpdateStates.remove(removedIp);
    m_deviceRemoteSessionCounts.remove(removedIp); // wjy: 删除设备时同步清掉远控人数缓存。
    m_deviceRemoteControllerNames.remove(removedIp);
    m_deviceRealtimeScriptStates.remove(removedIp); // wjy: 删除设备时同步清掉实时脚本三态，未来复用该 IP 不继承旧 Logo。
    m_authorizedRemoteControlIps.remove(removedIp); // wjy: 删除后清除本进程授权缓存，未来其它设备复用该 IP 时必须重新登记公钥。
    m_pendingRemoteControlAuthorizationIps.remove(removedIp);
    m_pendingTerminalOpenIps.remove(removedIp);
    m_pendingPowerActionIps.remove(removedIp);
    m_pendingUpdateRequestIps.remove(removedIp); // wjy: 删除动作只清理 UI 去重占位；仍在后台的结果会通过设备存在性检查自动丢弃。
    m_poweringOnDeviceIps.remove(removedIp);
    m_poweringOnStartedAtMs.remove(removedIp);
    m_scriptUiStateStore.removeState(removedIp); // wjy: 被删除设备的脚本 UI 不再保留，避免后续同 IP 之外的设备误用旧状态。
    if (g_devices.isEmpty()) {
            m_selectedDeviceIndex = 0;
            m_previousDeviceIndex = 0;
            m_selectedDeviceIndexes.clear();
            m_draggingDeviceIndexes.clear();
            m_selectionAnchorDeviceIndex = -1;
            m_currentDeviceName.clear();
            m_previousDeviceName.clear();
            m_settingsSelected = true;
            m_remoteAssistSelected = false;
            m_settingsAddDeviceExpanded = true;
            m_settingsScrollOffset = maxSettingsScrollOffset(m_settingsLocalInfoExpanded, m_settingsAddDeviceExpanded);
            updateAddDeviceControls();
            updateLocalInfoControls();
            updateSettingsControls();
            loadScriptUiStateForDevice(QString());
            update();
            return;
    }

    const int nextIndex = previousSelectedIndex == deviceIndex
        ? qMin(deviceIndex, g_devices.size() - 1)
        : qBound(0, previousSelectedIndex - (previousSelectedIndex > deviceIndex ? 1 : 0), g_devices.size() - 1); // wjy: 删除非当前设备时保留原详情目标；删除当前设备时优先选择其后一台或最后一台。

    m_selectedDeviceIndex = nextIndex;
    m_previousDeviceIndex = nextIndex;

    m_selectedDeviceIndexes.clear();
    m_selectedDeviceIndexes.insert(nextIndex); // wjy: 删除后收敛为一个明确主选择，清除可能因下标移动而失效的多选集合。

    m_draggingDeviceIndexes.clear();

    m_selectionAnchorDeviceIndex = nextIndex;
    m_currentDeviceName = deviceDisplayName(g_devices.at(nextIndex));
    m_previousDeviceName = m_currentDeviceName;
    loadScriptUiStateForDevice(currentScriptUiDeviceIp()); // wjy: 删除后详情切到下一台设备时，下方脚本 UI 同步切到下一台设备。
    setDesktopHoverActive(false);
    clearBottomActionHover();
    m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset, maxDeviceListScrollOffset()); // wjy: 删除设备后列表变短，立即把滚动位置限制回有效范围。
    pruneHiddenDeviceSelections(); // wjy: 删除最后一台远端后若目录只剩隐藏本机，立即清空主选择并保持设置页可用。
    update();
    // ===end====
}

void DeviceGrid::startDeviceWakeVisual(const QString& ip)
{
    const QString trimmedIp = ip.trimmed();
    if (trimmedIp.isEmpty()) {
        return;
    }

    m_poweringOnDeviceIps.insert(trimmedIp);
    m_poweringOnStartedAtMs.insert(trimmedIp, QDateTime::currentMSecsSinceEpoch());
    m_wakeVisualClock.restart();
    m_lastWakeProbeAtMs = 0;
    if (!m_wakeVisualTimer->isActive()) {
        m_wakeVisualTimer->start();
    }
    update();
}

void DeviceGrid::startCurrentDeviceWakeVisual()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    const QString ip = g_devices.at(m_selectedDeviceIndex).ip.trimmed();
    if (ip.isEmpty()
        || !platform::isDeviceActionAllowed(
            platform::DeviceActionKind::Wake,
            devicePresenceForIndex(m_selectedDeviceIndex))) { // wjy: 开机动画与真正的 Wake 动作复用同一资格策略，避免视觉入口和命令入口分叉。
        return;
    }

    startDeviceWakeVisual(ip);
}

void DeviceGrid::wakeCurrentDevice()
{
    wakeDeviceForIndex(m_selectedDeviceIndex, true);
}

bool DeviceGrid::wakeDeviceForIndex(int deviceIndex, bool showMessages)
{
    if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return false;
    }

    const DeviceEntry& device = g_devices.at(deviceIndex);
    if (!platform::isDeviceActionAllowed(
            platform::DeviceActionKind::Wake,
            devicePresenceForIndex(deviceIndex))) { // wjy: 开机只允许明确离线设备，在线或占用目标不重复发送唤醒包。
        return false;
    }

    const QString targetIp = device.ip.trimmed();
    const QString mac = device.mac.trimmed();
    if (mac.isEmpty()) {
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Warning,
                QString(),
                zh("\xE8\xAF\xB7\xE5\x85\x88\xE4\xB8\xBA\xE8\xAF\xA5\xE8\xAE\xBE\xE5\xA4\x87\xE5\xA1\xAB\xE5\x86\x99 MAC \xE5\x9C\xB0\xE5\x9D\x80\xEF\xBC\x8C\xE6\x89\x8D\xE8\x83\xBD\xE8\xBF\x9B\xE8\xA1\x8C\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA\xE3\x80\x82"),
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }
        return false;
    }

    struct WakeProxyCandidate {
        QString name;
        QString ip;
        platform::DevicePresenceState state = platform::DevicePresenceState::Offline;
    };

    QVector<WakeProxyCandidate> candidates;
    candidates.reserve(g_devices.size());
    for (int i = 0; i < g_devices.size(); ++i) {
        if (i == deviceIndex) {
            continue;
        }

        const DeviceEntry& candidateDevice = g_devices.at(i);
        const QString candidateIp = candidateDevice.ip.trimmed();
        if (candidateIp.isEmpty()) {
            continue;
        }

        const platform::DevicePresenceState state = devicePresenceForIndex(i);
        if (!proxyWakeCapableState(state)) {
            continue;
        }

        candidates.append({deviceDisplayName(candidateDevice), candidateIp, state});
    }

    if (candidates.isEmpty()) {
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Warning,
                QString(),
                zh("\xE5\xBD\x93\xE5\x89\x8D\xE8\xAE\xBE\xE5\xA4\x87\xE5\x88\x97\xE8\xA1\xA8\xE4\xB8\xAD\xE6\xB2\xA1\xE6\x9C\x89\xE5\x9C\xA8\xE7\xBA\xBF\xE8\xAE\xBE\xE5\xA4\x87\xEF\xBC\x8C\xE6\x97\xA0\xE6\xB3\x95\xE4\xBB\xA3\xE5\x8F\x91\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA\xE5\x8C\x85\xE3\x80\x82"),
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }
        return false;
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const WakeProxyCandidate& left, const WakeProxyCandidate& right) {
        return std::tie(left.state, left.name) < std::tie(right.state, right.name);
    });

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, targetIp, mac, candidates = std::move(candidates), showMessages] {
        QString proxyDeviceName;
        QString proxyIp;
        for (const WakeProxyCandidate& candidate : candidates) {
            const platform::DeviceStatusInfo info = platform::DeviceStatusService::query(candidate.ip);
            if (!proxyWakeCapableState(info.state)) {
                continue;
            }
            if (!isSameSubnet(targetIp, info.localIp, info.subnetMask)) {
                continue;
            }

            proxyDeviceName = candidate.name;
            proxyIp = candidate.ip;
            break;
        }

        if (!self) {
            return;
        }

        if (proxyIp.isEmpty()) {
            QMetaObject::invokeMethod(self, [self, showMessages] {
                if (!self) {
                    return;
                }
                if (!showMessages) {
                    return;
                }

                QMessageBox messageBox(
                    QMessageBox::Warning,
                    QString(),
                    zh("\xE5\xBD\x93\xE5\x89\x8D\xE8\xAE\xBE\xE5\xA4\x87\xE5\x88\x97\xE8\xA1\xA8\xE4\xB8\xAD\xE6\xB2\xA1\xE6\x9C\x89\xE4\xB8\x8E\xE7\x9B\xAE\xE6\xA0\x87\xE8\xAE\xBE\xE5\xA4\x87\xE5\xA4\x84\xE4\xBA\x8E\xE5\x90\x8C\xE4\xB8\x80\xE5\xB1\x80\xE5\x9F\x9F\xE7\xBD\x91\xE4\xB8\x94\xE5\x9C\xA8\xE7\xBA\xBF\xE7\x9A\x84\xE8\xAE\xBE\xE5\xA4\x87\xEF\xBC\x8C\xE6\x97\xA0\xE6\xB3\x95\xE4\xBB\xA3\xE5\x8F\x91\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA\xE5\x8C\x85\xE3\x80\x82"),
                    QMessageBox::NoButton,
                    self);
                messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
                messageBox.exec();
            }, Qt::QueuedConnection);
            return;
        }

        QString errorMessage;
        const bool sent = platform::DeviceCommandService::sendWakeProxy(proxyIp, mac, &errorMessage);
        QMetaObject::invokeMethod(self, [self, targetIp, sent, proxyDeviceName, errorMessage, showMessages] {
            if (!self) {
                return;
            }

            if (sent) {
                self->startDeviceWakeVisual(targetIp);
                return; // wjy: 唤醒动画已有目标 IP 的轻量探测；不再借机启动一次全设备 TCP 状态轮询。
            }

            if (showMessages) {
                QMessageBox messageBox(
                    QMessageBox::Warning,
                    QString(),
                    errorMessage.trimmed().isEmpty()
                        ? zh("\xE5\x90\x8C\xE7\xBD\x91\xE6\xAE\xB5\xE4\xBB\xA3\xE7\x90\x86\xE8\xAE\xBE\xE5\xA4\x87\xE5\xBC\x80\xE6\x9C\xBA\xE5\x8C\x85\xE4\xBB\xA3\xE5\x8F\x91\xE5\xA4\xB1\xE8\xB4\xA5\xE3\x80\x82")
                        : zh("\xE5\x90\x8C\xE7\xBD\x91\xE6\xAE\xB5\xE8\xAE\xBE\xE5\xA4\x87\xE3\x80\x8C") + proxyDeviceName + zh("\xE3\x80\x8D\xE4\xBB\xA3\xE5\x8F\x91\xE5\xBC\x80\xE6\x9C\xBA\xE5\x8C\x85\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x9A") + errorMessage.trimmed(),
                    QMessageBox::NoButton,
                    self);
                messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
                messageBox.exec();
            }
        }, Qt::QueuedConnection);
    });
    return true;
}

void DeviceGrid::toggleRemoteWakeup()
{
    if (m_remoteWakeupEnabled) {
        m_remoteWakeupEnabled = false;
        platform::AppSettings::setRemoteWakeupEnabled(false);
        update();
        return;
    }

    if (m_wolDetectionInProgress) {
        return;
    }

    m_wolDetectionInProgress = true;
    update();

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self] {
        const platform::WolApplyResult result = platform::WolDetector::enable();
        const bool enabled = result.success;
        const bool permissionDenied = result.permission_denied;

        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, enabled, permissionDenied] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            grid->m_wolDetectionInProgress = false;
            if (enabled) {
                grid->m_remoteWakeupEnabled = true;
                platform::AppSettings::setRemoteWakeupEnabled(true);
                grid->update();
                return;
            }

            grid->m_remoteWakeupEnabled = false;
            platform::AppSettings::setRemoteWakeupEnabled(false);
            grid->update();

            if (permissionDenied) {
                QMessageBox messageBox(QMessageBox::Warning, QString(), zh("\xE5\xBC\x80\xE5\x90\xAF\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x8C\xE9\x9C\x80\xE8\xA6\x81\xE4\xBB\xA5\xE7\xAE\xA1\xE7\x90\x86\xE5\x91\x98\xE6\x9D\x83\xE9\x99\x90\xE8\xBF\x90\xE8\xA1\x8C\xE8\xBD\xAF\xE4\xBB\xB6\xE6\x89\x8D\xE8\x83\xBD\xE4\xBF\xAE\xE6\x94\xB9\xE7\xBD\x91\xE5\x8D\xA1\xE5\x94\xA4\xE9\x86\x92\xE8\xAE\xBE\xE7\xBD\xAE\xE3\x80\x82"), QMessageBox::NoButton, grid);
                messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
                messageBox.exec();
                return;
            }

            QMessageBox messageBox(QMessageBox::Warning, QString(), zh("\xE6\x9C\xAC\xE8\xAE\xBE\xE5\xA4\x87\xE7\x9A\x84\xE7\xBD\x91\xE5\x8D\xA1\xE4\xB8\x8D\xE6\x94\xAF\xE6\x8C\x81\xE7\xBD\x91\xE7\xBB\x9C\xE5\x94\xA4\xE9\x86\x92\xEF\xBC\x8C\xE6\x97\xA0\xE6\xB3\x95\xE9\x80\x9A\xE8\xBF\x87\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA\xE5\x90\xAF\xE5\x8A\xA8\xE3\x80\x82"), QMessageBox::NoButton, grid);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }, Qt::QueuedConnection);
    });
}

void DeviceGrid::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    syncResponsiveLayoutState();

// =====wjy====
    static int s_paintLogCount = 0; // wjy: 只记录前几次绘制，避免 paintEvent 高频触发导致日志文件过大。
    const int paintLogIndex = s_paintLogCount++; // wjy: 给每次被记录的绘制分配编号，方便判断 begin/end 是否配对。
    const bool shouldLogPaint = paintLogIndex < 5; // wjy: 只跟踪启动阶段最关键的前 5 次绘制。
    if (shouldLogPaint) {
        writeDeviceGridStartupLog(QStringLiteral("[wjy-paint] begin index=%1").arg(paintLogIndex)); // wjy: 如果有 begin 没有 end，说明崩在 paintEvent 内部。
    }
// ===end====

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // =====wjy====
    painter.setCompositionMode(QPainter::CompositionMode_Source); // wjy: 先直接覆盖上一帧像素，确保圆角外不会残留历史不透明底色。
    painter.fillRect(rect(), Qt::transparent); // wjy: 将完整客户区清空为透明，四个直角区域不再被任何矩形背景填充。
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver); // wjy: 清空完成后恢复正常 Alpha 混合，再绘制窗口主体内容。
    QPainterPath windowShape;
    windowShape.addRoundedRect(QRectF(rect()), 6, 6); // wjy: 主背景与现有 6px 圆角边框共用同一窗口外形，避免填充和描边轮廓不一致。
    painter.setClipPath(windowShape); // wjy: 后续标题栏、侧栏和内容背景全部限制在圆角区域内，圆角外像素保持透明。
    // ===end====

    painter.fillPath(windowShape, QColor(QStringLiteral("#F8FAFC"))); // wjy: 使用圆角路径填充主内容底色，替代原先覆盖四角的整矩形填充。
    painter.fillRect(QRectF(0, 0, width(), kTitleBarHeight), QColor(QStringLiteral("#EEF3F7"))); //标题栏
    painter.fillRect(QRectF(0, kTitleBarHeight, kSidebarWidth, height() - kTitleBarHeight), QColor(QStringLiteral("#EEF3F7"))); // wjy: 详情栏收起时设备栏仍完整保留，不再随 << 消失。

    painter.setPen(QPen(QColor(QStringLiteral("#BFC7D1")), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 6, 6);
    painter.setPen(QPen(QColor(QStringLiteral("#D8DEE5")), 1));
    painter.drawLine(kSidebarWidth, kTitleBarHeight, kSidebarWidth, height()); // wjy: 分割线固定区分 240px 设备栏与详情/按钮窄条。

// =====wjy====
    const QRect titleWordmarkRect = titleBarCenteredRect(18, 116); // wjy: 左上角标题名使用统一标题栏视觉高度，不再从 y=15 开始下沉。
    const QRect settingsIconRect = titleBarCenteredRect(titlebarSettingsRect().x() + 14, 20, 20); // wjy: 设置按钮只保留图标，放在刷新按钮左侧并垂直居中。
    const QRect refreshIconRect = titleBarCenteredRect(refreshRect().x() + 10, 28, 22); // wjy: 刷新图标保留原有资源高度 22，并由标题栏高度统一居中。
    const QRect minimizeIconRect = titleBarCenteredRect(minimizeRect().x() + 6, 24, 24); // wjy: 24px 最小化图标在 36px 独立热区内水平居中。
    const QRect closeIconRect = titleBarCenteredRect(closeRect().x() + 19, 10, 10); // wjy: 10px 关闭图标在 48px 关闭热区内水平居中，紧凑标题栏左右留白一致。
    const qreal separatorTop = (kTitleBarHeight - kTitleBarVisualHeight) / 2.0; // wjy: 竖杠顶部也跟随统一视觉高度，避免越过标题栏下边界。
    const qreal separatorBottom = separatorTop + kTitleBarVisualHeight; // wjy: 竖杠底部由顶部加统一高度得到，和左上标题名保持同一高度。

    painter.drawPixmap(titleWordmarkRect, uupix(QStringLiteral("titlebar/title_wordmark.png"))); // wjy: 实际绘制标题名时复用统一矩形，保证左上标题与右侧按钮同高。
    // =====wjy====
    // wjy: 版本号显示在“丰实远程控制”右侧，默认 1.1.1，每次发布 patch+1。
    QRect versionRect;
    {
        QFont versionFont(QStringLiteral("Microsoft YaHei UI"));
        versionFont.setPixelSize(11);
        painter.setFont(versionFont);
        painter.setPen(QColor(QStringLiteral("#6B7280")));
        const QString versionText = QStringLiteral("v%1").arg(platform::UpdateService::displayVersion());
        const QFontMetrics versionMetrics(versionFont);
        const int versionWidth = versionMetrics.horizontalAdvance(versionText) + 4;
        versionRect = QRect(titleWordmarkRect.right() + 8, titleWordmarkRect.y(), versionWidth, titleWordmarkRect.height()); // wjy: 保存版本号最终矩形，网络状态必须从它的右边开始布局。
        painter.drawText(QRectF(versionRect), Qt::AlignVCenter | Qt::AlignLeft, versionText);
    }
    {
        constexpr int kMaximumIdentityReservation = 252; // wjy: 为右侧本机名、IP 和按钮保留现有最大宽度，网络文字不能覆盖身份信息。
        const int networkLeft = versionRect.right() + 10; // wjy: 用户要求带宽监控紧跟在版本号右侧，并保留 10px 视觉间距。
        const int identityBoundary = (m_updateAvailable ? titlebarUpdateRect().x() : titlebarSettingsRect().x())
            - kMaximumIdentityReservation;
        const QRect networkRect(
            networkLeft,
            titleWordmarkRect.y(),
            qMax(0, identityBoundary - networkLeft - 8),
            titleWordmarkRect.height()); // wjy: 窗口变窄或更新按钮出现时自动压缩可用宽度，紧凑态不会与窗口按钮重叠。
        if (networkRect.width() >= 34) {
            QFont networkFont(QStringLiteral("Microsoft YaHei UI"));
            networkFont.setPixelSize(11);
            painter.setFont(networkFont);
            QColor networkColor(QStringLiteral("#6B7280"));
            QString networkText = QString::fromUtf8("网 --"); // wjy: 第一次只有计数基线，明确显示采样中占位而不是错误的 0 Mbps。
            if (m_titlebarBandwidthSample.valid) {
                const auto formatMbps = [](double value) {
                    return QString::number(value, 'f', value < 10.0 ? 1 : 0); // wjy: 低速保留一位小数，高速取整以节省标题栏宽度。
                };
                const QString receiveText = formatMbps(m_titlebarBandwidthSample.receive.currentMbps);
                const QString headroomText = formatMbps(m_titlebarBandwidthSample.receive.headroomMbps);
                networkText = m_titlebarBandwidthSample.receive.capacityMbps > 0.0
                    ? QString::fromUtf8("↓%1M 余%2M").arg(receiveText, headroomText)
                    : QString::fromUtf8("↓%1M").arg(receiveText); // wjy: 驱动没有容量时只展示真实速率，不伪造剩余带宽。
                const bool adapterDropping = m_titlebarBandwidthSample.receiveDiscardsDelta > 0
                    || m_titlebarBandwidthSample.receiveErrorsDelta > 0;
                if (adapterDropping
                    || m_titlebarBandwidthSample.receive.risk == platform::LocalNetworkBandwidthRisk::High
                    || m_titlebarBandwidthSample.receive.risk == platform::LocalNetworkBandwidthRisk::Saturated) {
                    networkColor = QColor(QStringLiteral("#DC2626")); // wjy: 高利用率或入站丢弃/错误使用红色，提示带宽或接收队列风险。
                } else if (m_titlebarBandwidthSample.receive.risk == platform::LocalNetworkBandwidthRisk::Attention) {
                    networkColor = QColor(QStringLiteral("#D97706")); // wjy: 70%-85% 使用琥珀色，提醒继续观察但不宣称已经拥塞。
                } else {
                    networkColor = QColor(QStringLiteral("#2563EB")); // wjy: 正常采样使用克制蓝色，与更新入口颜色体系保持一致。
                }
            }
            const QFontMetrics networkMetrics(networkFont);
            if (networkMetrics.horizontalAdvance(networkText) > networkRect.width()
                && m_titlebarBandwidthSample.valid) {
                networkText = QString::fromUtf8("↓%1M").arg(
                    QString::number(
                        m_titlebarBandwidthSample.receive.currentMbps,
                        'f',
                        m_titlebarBandwidthSample.receive.currentMbps < 10.0 ? 1 : 0)); // wjy: 宽度不足时优先保留当前接收速率，余量只在空间充足时显示。
            }
            painter.setPen(networkColor);
            painter.drawText(
                QRectF(networkRect),
                Qt::AlignVCenter | Qt::AlignLeft,
                networkMetrics.elidedText(networkText, Qt::ElideRight, networkRect.width())); // wjy: 极窄边界最终省略，绝不覆盖本机名称、更新或窗口控制按钮。
        }
    }
    // ===end====
    if (!m_detailPanelCollapsed && m_settingsSelected) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#DDE6EF")));
        painter.drawRoundedRect(QRectF(titlebarSettingsRect()).adjusted(8, 4, -8, -4), 4, 4);
    }
    // =====wjy====
    if (!m_detailPanelCollapsed && m_updateAvailable) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
        painter.drawRoundedRect(QRectF(titlebarUpdateRect()), 4, 4); // wjy: 仅检测到远端版本更高时，在设置按钮左侧绘制主动更新入口。
        QFont updateFont(QStringLiteral("Microsoft YaHei UI"));
        updateFont.setPixelSize(12);
        updateFont.setBold(true);
        painter.setFont(updateFont);
        painter.setPen(QColor(QStringLiteral("#FFFFFF")));
        painter.drawText(titlebarUpdateRect(), Qt::AlignCenter,
            m_updatePreparing ? QString::fromUtf8("准备中") : QString::fromUtf8("更新")); // wjy: 用户点击后显示准备状态，并阻止重复创建更新任务。
    }
    // ===end====
    if (!m_detailPanelCollapsed) {
        QFont identityFont(QStringLiteral("Microsoft YaHei UI"));
        identityFont.setPixelSize(12);
        painter.setFont(identityFont);
        painter.setPen(QColor(QStringLiteral("#4B5563")));
        const int identityRight = (m_updateAvailable ? titlebarUpdateRect().x() : titlebarSettingsRect().x()) - 10; // wjy: 更新按钮出现时，本机名称和 IP 自动向左让位，避免文字重叠。
        // =====wjy====
        // wjy: 完整标题栏保持“本机名 本机IP 设置”；紧凑态整体跳过，避免文字与窗口控制按钮重叠。
        const QFontMetrics identityMetrics(identityFont);
        const QString localNameText = m_localDeviceInfo.name.trimmed();
        const QString localIpText = m_localDeviceInfo.ip.trimmed();
        const int localIpWidth = qMin(114, identityMetrics.horizontalAdvance(localIpText));
        const int localNameWidth = qMin(118, identityMetrics.horizontalAdvance(localNameText));
        const int identityY = (kTitleBarHeight - kTitleBarVisualHeight) / 2;
        const QRect localIpRect(identityRight - localIpWidth, identityY, localIpWidth, kTitleBarVisualHeight);
        const QRect localNameRect(localIpRect.x() - 10 - localNameWidth, identityY, localNameWidth, kTitleBarVisualHeight);
        painter.drawText(QRectF(localNameRect), Qt::AlignVCenter | Qt::AlignRight, identityMetrics.elidedText(localNameText, Qt::ElideRight, localNameWidth));
        painter.drawText(QRectF(localIpRect), Qt::AlignVCenter | Qt::AlignRight, identityMetrics.elidedText(localIpText, Qt::ElideLeft, localIpWidth));
        // ===end====
        // =====wjy====
        drawRotatedUiIcon(
            painter,
            settingsIconRect,
            QStringLiteral("settings.svg"),
            m_settingsGearRotation); // wjy: 齿轮 SVG 圆心正好位于 20×20 画布的 (10,10)，使用默认 0.5/0.5 浮点支点原地旋转。
        drawRotatedUiIcon(
            painter,
            refreshIconRect,
            QStringLiteral("refresh.svg"),
            m_refreshRotation,
            QPointF(14.5 / 28.0, 10.5 / 22.0)); // wjy: 刷新圆弧真实圆心为 SVG 的 (14.5,10.5)，不能按 28×22 画布中心或 QRect 整数中心旋转。
        // ===end====
    }
    painter.setPen(QPen(QColor(QStringLiteral("#D8DEE5")), 1));
    painter.drawLine(QPointF(minimizeRect().x() - 0.5, separatorTop), QPointF(minimizeRect().x() - 0.5, separatorBottom)); // wjy: 竖杠高度统一为标题栏视觉高度，和标题名对齐。
    drawUiIcon(painter, minimizeIconRect, QStringLiteral("minimize.svg")); // wjy: 最小化位置由统一居中矩形控制。
    drawUiIcon(painter, closeIconRect, QStringLiteral("close.svg")); // wjy: 关闭位置由统一居中矩形控制。
// ===end====

    QFont textFont(QStringLiteral("Microsoft YaHei UI"));
    textFont.setPixelSize(14);

// =====wjy====
    const QSet<int> badges = deviceBadgeIndexes(); // wjy: 远程控制角标仍然按真实设备下标判断，避免分组行影响设备下标。
    m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset, maxDeviceListScrollOffset()); // wjy: 详情栏收起后仍校正设备滚动偏移，设备栏交互不受影响。
    const QVector<DeviceListRow> deviceRows = visibleDeviceRows(m_deviceStatuses); // wjy: 设备栏绘制使用在线优先、组内自然名称排序后的唯一可见行快照。
    const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: “我的设备”内部滚动视口始终限制在 240px 设备栏内。
    if (m_deviceGroupExpanded) {
        painter.save();
        painter.setClipRect(deviceListClip); // wjy: 内容超出视口时直接裁掉，避免设备行压到下面的设备管理区域。
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#E8EDF3")));
        for (int rowIndex = 0; rowIndex < deviceRows.size(); ++rowIndex) {
            const DeviceListRow& row = deviceRows.at(rowIndex);
            if (row.type != DeviceListRow::Type::Group || row.groupIndex < 0) {
                continue;
            }

            int firstDeviceRow = -1;
            int lastDeviceRow = -1;
            for (int childRowIndex = rowIndex + 1; childRowIndex < deviceRows.size(); ++childRowIndex) {
                const DeviceListRow& childRow = deviceRows.at(childRowIndex);
                if (childRow.type == DeviceListRow::Type::Group) {
                    break; // wjy: 到下一个分组时结束当前分组内容背景计算。
                }
                if (childRow.type == DeviceListRow::Type::Device && childRow.groupIndex == row.groupIndex) {
                    if (firstDeviceRow < 0) {
                        firstDeviceRow = childRowIndex;
                    }
                    lastDeviceRow = childRowIndex;
                }
            }

            if (firstDeviceRow < 0 || lastDeviceRow < firstDeviceRow) {
                continue;
            }

            QRect groupContentRect = scrolledVisibleDeviceRowRect(firstDeviceRow, m_deviceListScrollOffset);
            groupContentRect = groupContentRect.united(scrolledVisibleDeviceRowRect(lastDeviceRow, m_deviceListScrollOffset));
            if (!groupContentRect.intersects(deviceListClip)) {
                continue;
            }
            painter.drawRoundedRect(QRectF(groupContentRect), 5, 5); // wjy: 分组内设备背景按整块绘制，把设备行之间的缝隙也填成同一片灰色。
        }

        for (int rowIndex = 0; rowIndex < deviceRows.size(); ++rowIndex) { // wjy: 每一行都来自 visibleDeviceRows，包含无分组设备、分组行、分组内设备。
            const DeviceListRow& row = deviceRows.at(rowIndex); // wjy: row 保存这一行到底是设备还是分组，以及对应的真实下标。
            const QRect rowRect = scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset); // wjy: 行坐标减去滚动偏移，得到当前真正显示在屏幕上的位置。
            const int rowY = rowRect.y();
            if (!rowRect.intersects(deviceListClip)) {
                continue; // wjy: 当前行完全滚出视口时跳过绘制，节省绘制并保证不越界显示。
            }

            if (row.type == DeviceListRow::Type::Device) {
                const int deviceIndex = row.deviceIndex; // wjy: 设备行使用真实设备下标读取名称、在线状态和选中状态。
                if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
                    continue; // wjy: 防御性跳过异常设备行，避免手动编辑 JSON 后出现越界。
                }

                const bool deviceInsideGroup = row.groupIndex >= 0; // wjy: 分组内设备稍微右移，视觉上表示它属于上方分组。
                const QString deviceIp = g_devices.at(deviceIndex).ip.trimmed(); // wjy: 运行状态按设备 IP 查询，分组排序或视觉行变化不会串到其它设备。
                const bool scriptRunning = !deviceIp.isEmpty()
                    && m_deviceRealtimeScriptStates.value(deviceIp, platform::RealtimeScriptState::Unknown)
                        == platform::RealtimeScriptState::Running; // wjy: Logo 只由目标设备广播的 Running 驱动；Offline/Unknown 隐藏但不删除可恢复脚本 UI 元数据。
                const bool deviceSelected =
                    m_selectedDeviceIndexes.contains(deviceIndex);

                if (!m_remoteAssistSelected
                    && !m_localInfoSelected
                    && deviceSelected) {

                    painter.setPen(Qt::NoPen);

                    // 当前右侧详情对应的主设备颜色稍深，
                    // 其他批量选中的设备颜色稍浅。
                    painter.setBrush(
                        deviceIndex == m_selectedDeviceIndex
                            ? QColor(QStringLiteral("#BFD3F7"))
                            : QColor(QStringLiteral("#D7E4FA")));

                    painter.drawRoundedRect(
                        QRectF(rowRect),
                        5,
                        5);

                    // 蓝色竖条只画在主设备旁边，
                    // 用来表示右侧详情正在显示哪台设备。
                    if (deviceIndex == m_selectedDeviceIndex) {
                        painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
                        painter.drawRoundedRect(QRectF(4, rowY + 10, 4, 17),2,2);
                    }
                }

                const int iconX = deviceInsideGroup ? 50 : 30;
                const int textX = deviceInsideGroup ? 76 : 56;
                // =====wjy====
                const platform::DevicePresenceState deviceState = devicePresenceForIndex(deviceIndex);
                // wjy: 远控人数来自状态刷新缓存；无字段的旧目标在 busy 时回退为 1。
                int remoteControllerCount = qBound(0, m_deviceRemoteSessionCounts.value(deviceIp, 0), 10);
                if (remoteControllerCount <= 0 && deviceState == platform::DevicePresenceState::Busy) {
                    remoteControllerCount = 1;
                }
                const bool showRemoteControlIcon = remoteControllerCount > 0;
                // wjy: 脚本 logo 固定 x=166，远控数字 logo 固定 x=186，两格并排不随是否显示互换位置。
                const int textWidth = deviceInsideGroup ? 62 : 82;
                // ===end====
                constexpr qreal statusDotSize = 6.0; // wjy: 左侧列表状态点改小，贴近用户示例图里的轻量提示样式。
                const qreal statusDotX = iconX - 23.0+6.0;
                const qreal statusDotY = rowY + 9 + (20.0 - statusDotSize) / 2.0; // wjy: 状态点移动到设备图标左侧，并和图标垂直居中。

                const bool deviceOnlineLike = deviceState == platform::DevicePresenceState::Online
                    || deviceState == platform::DevicePresenceState::Busy; // wjy: 左侧列表只有在线/占用才显示状态圆点；离线和未检测不再画灰色圆点。
                painter.setPen(Qt::NoPen);
                if (deviceOnlineLike) {
                    painter.setBrush(deviceListStatusAccentColor(deviceState)); // wjy: 左侧列表里占用设备继续使用在线绿点，不影响右侧占用胶囊。
                    painter.drawEllipse(QRectF(statusDotX, statusDotY, statusDotSize, statusDotSize));
                }
                drawDeviceTileIcon(
                    painter,
                    iconX,
                    rowY + 9,
                    20,
                    deviceOnlineLike
                        ? QColor(QStringLiteral("#3A7BFC"))
                        : QColor(QStringLiteral("#9CA3AF"))); // wjy: 离线或未检测时把左侧设备图标底色改成灰色，替代右侧灰色状态点。

                painter.setFont(textFont);
                painter.setPen(QColor(QStringLiteral("#111827")));
                if (m_renamingDeviceIndex != deviceIndex) {
                    painter.drawText(QRectF(textX, rowY + 7, textWidth, 22), Qt::AlignVCenter | Qt::AlignLeft, deviceDisplayName(g_devices.at(deviceIndex))); // wjy: 显示真实设备名，分组排序变化不会改错名字。
                }
                // =====wjy====
                if (scriptRunning) {
                    drawScriptRunningIcon(painter, QRectF(166, rowY + 9, 18, 18)); // wjy: 脚本状态固定左侧槽位。
                }
                if (showRemoteControlIcon) {
                    drawRemoteControlCountIcon(painter, QRectF(186, rowY + 9, 18, 18), remoteControllerCount); // wjy: 远控人数固定右侧槽位，与脚本 logo 并排。
                }
                // ===end====
                if (badges.contains(deviceIndex)) {
                    drawRemoteBadge(painter, 208, rowY + 12); // wjy: Wi-Fi 角标固定在双状态徽标右侧。
                }
                continue;
            }

            if (row.type == DeviceListRow::Type::Group) {
                const int groupIndex = row.groupIndex; // wjy: 分组行使用真实分组下标读取名称和展开状态。
                if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
                    continue; // wjy: 防御性跳过异常分组行。
                }

                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(QStringLiteral("#fcfdff"))); // wjy: 分组背景颜色，和普通设备行区分开。
                painter.drawRoundedRect(QRectF(rowRect), 5, 5);
                drawResourceIcon(painter, QRect(12, rowY + 8, 20, 20), QStringLiteral("settings/remote_wakeup.svg")); // wjy: 分组栏文字左侧改用远程唤醒图标，强调这一行是可集中管理的分组入口。
                drawUiIcon(
                    painter,
                    QRect(203, rowY + 8, 24, 20),
                    deviceGroupExpandedForIndex(groupIndex) ? QStringLiteral("chevron_up.svg") : QStringLiteral("chevron_down.svg")); // wjy: 根据分组展开状态绘制上箭头或下箭头。

                painter.setFont(textFont);
                painter.setPen(QColor(QStringLiteral("#000000"))); //分组字体颜色
                if (m_renamingDeviceGroupIndex != groupIndex) {
                    const QRect groupTextRect = groupNameTextRect(rowY, textFont);
                    const QString groupNameText = QFontMetrics(textFont).elidedText(
                        g_deviceGroupNames.at(groupIndex),
                        Qt::ElideRight,
                        groupTextRect.width());
                    painter.drawText(QRectF(groupTextRect), Qt::AlignVCenter | Qt::AlignLeft, groupNameText); // wjy: 分组文字限制到十字符视觉宽度，超长时省略。
                }
            }
        }
        const QRect blankRect = scrolledDeviceGroupReservedBlankRect(m_deviceListScrollOffset);
        const QRect visibleBlankRect = blankRect.intersected(deviceListClip);
        if (!visibleBlankRect.isEmpty()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#F3F6FA"))); // wjy: 底部空白落点使用更淡的灰色，并延伸覆盖原设置栏区域。
            painter.drawRoundedRect(QRectF(blankRect), 4, 4);
        }
        painter.restore();

        const int maxScrollOffset = maxDeviceListScrollOffset(); // wjy: 大于 0 表示内容高度超过视口，需要显示滚动条。
        if (maxScrollOffset > 0 && deviceListClip.height() > 0) {
            const QRect scrollThumb = deviceListScrollbarThumbRect(m_deviceListScrollOffset); // wjy: 绘制直接复用拖拽命中的滑块矩形，滚动位置只有一个权威计算来源。
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_draggingDeviceListScrollbar ? QColor(120, 139, 161, 190) : QColor(172, 184, 198, 120)); // wjy: 拖拽期间加深滑块，明确反馈当前抓取状态。
            painter.drawRoundedRect(QRectF(scrollThumb), 1.5, 1.5);
        }
    }
// ===end====

    if (!m_detailPanelCollapsed) {
        painter.save();
        //右侧内容裁剪区域
        const bool deviceDetailPage = !m_settingsSelected && !m_remoteAssistSelected && !m_localInfoSelected; // wjy: 设备详情页使用底部安全边界，避免内容压住折叠按钮。
        const bool settingsPage = m_settingsSelected && !m_remoteAssistSelected && !m_localInfoSelected; // wjy: 设置页也参照设备详情页，使用同一条底部绘制边界。
        painter.setClipRect((deviceDetailPage || settingsPage) ? deviceDetailContentClipRect() : contentClipRect()); // wjy: 详情栏展开时才创建右侧裁剪区域，紧凑态完全跳过详情绘制。
        if (m_settingsSelected) {
            const int settingsMaxScrollOffset = m_settingsTab == SettingsTab::Keyboard
                ? keyboardShortcutMaxScrollOffset()
                : maxSettingsScrollOffset(m_settingsLocalInfoExpanded, m_settingsAddDeviceExpanded);
            m_settingsScrollOffset = qBound(0, m_settingsScrollOffset, settingsMaxScrollOffset);
        drawSettingsPage(
            painter,
            textFont,
            m_autoRunEnabled,
            m_remoteWakeupEnabled,
            m_preventSleepEnabled,
            m_periodicDeviceDiscoveryEnabled,
            m_hideLocalDeviceEnabled,
            m_wallpaperRotationEnabled,
            m_wallpaperRotationStatusText,
            m_rollbackVersionCombo && !m_rollbackVersionCombo->currentData().toString().isEmpty(), // wjy: 手绘按钮可用状态直接取真实下拉框目标数据，视觉与点击条件保持一致。
            m_rollbackPreparing,
            m_publishPreparing, // wjy: 发布状态进入同一帧设置页绘制，后台任务开始和结束都能立即反馈按钮状态。
            m_updatePreparing || m_rollbackPreparing || m_publishPreparing, // wjy: 三类版本操作共用一个视觉禁用条件，按钮不会在其它事务期间呈现可点击蓝色。
            m_settingsLocalInfoExpanded,
            m_settingsAddDeviceExpanded,
            m_settingsTab == SettingsTab::Keyboard,
            m_settingsTab == SettingsTab::RemoteControl,
            m_localDeviceInfo,
            m_settingsScrollOffset);
        } else if (!m_remoteAssistSelected
        && !m_localInfoSelected
        && m_deviceDetailTab == DeviceDetailTab::Local) {
        drawLocalSystemInfoPage(
            painter,
            m_localSystemInfo,
            m_localMemoryUsage,
            m_localCpuUsagePercent,
            m_localGpuUsagePercent,
            m_localMemoryUsagePercent,
            textFont); // wjy: 本机页完全替代远端设备卡片，避免控制端信息和当前选中设备信息混在同一页面。
        } else if (m_detailAnimationTimer->isActive()) {
        const qreal eased = easeOutCubic(m_detailAnimationProgress);
        drawDeviceDetail(
            painter,
            m_previousDeviceName,
            devicePresenceForIndex(m_previousDeviceIndex),
            devicePoweringOnForIndex(m_previousDeviceIndex),
            devicePoweringOnRemainingSecondsForIndex(m_previousDeviceIndex),
            badges.contains(m_previousDeviceIndex),
            0.0,
            0.0,
            BottomAction::None,
            -24.0 * eased,
            1.0 - eased,
            false,
            textFont);
        drawDeviceDetail(
            painter,
            m_currentDeviceName,
            devicePresenceForIndex(m_selectedDeviceIndex),
            devicePoweringOnForIndex(m_selectedDeviceIndex),
            devicePoweringOnRemainingSecondsForIndex(m_selectedDeviceIndex),
            badges.contains(m_selectedDeviceIndex),
            m_desktopHoverProgress,
            m_wakeVisualRotation,
            BottomAction::None,
            32.0 * (1.0 - eased),
            eased,
            false,
            textFont);
        } else {
        drawDeviceDetail(
            painter,
            m_currentDeviceName,
            devicePresenceForIndex(m_selectedDeviceIndex),
            devicePoweringOnForIndex(m_selectedDeviceIndex),
            devicePoweringOnRemainingSecondsForIndex(m_selectedDeviceIndex),
            badges.contains(m_selectedDeviceIndex),
            m_desktopHoverProgress,
            m_wakeVisualRotation,
            m_hoveredBottomAction,
            0,
            1.0,
            false,
            textFont);
    }

        if (!m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected) {
        drawDeviceDetailTabs(
            painter,
            m_deviceDetailTab == DeviceDetailTab::Local,
            m_deviceDetailTab == DeviceDetailTab::ScriptLog,
            m_deviceDetailTab == DeviceDetailTab::Config,
            m_deviceDetailTab == DeviceDetailTab::Search,
            textFont); // wjy: 四个详情页签使用同一状态源绘制；查找页虽暂时隐藏，仍保留独立选中状态供后续恢复。
    }

        if (!m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && m_deviceDetailTab == DeviceDetailTab::Config) {
        drawScriptFileEditorPanel(
            painter,
            m_scriptEditorTitle,
            m_scriptEditorLoading,
            !m_scriptEditorRemotePath.trimmed().isEmpty()); // wjy: 左侧白色编辑面板只在设备详情页显示，避免盖住设置/新增设备页面。
    }

        if (!m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && m_deviceDetailTab == DeviceDetailTab::ScriptLog) {
        drawScriptTerminalPanel(
            painter,
            m_scriptOutputTitle.trimmed().isEmpty() ? QString::fromUtf8("脚本日志") : m_scriptOutputTitle,
            m_scriptOutputVisible ? m_scriptOutputText : QString(),
            m_scriptOutputRunning,
            m_scriptOutputFailed,
            m_scriptOutputScrollOffset,
            !m_lastScriptEntryPath.trimmed().isEmpty() && !m_scriptOutputRunning); // wjy: 只有存在具体入口文件且当前未运行时显示执行按钮。
    }
        painter.restore();
    }
    drawDetailPanelToggleButton(painter, m_detailPanelCollapsed); // wjy: << / >> 始终绘制在设备栏右侧专用窄条，不覆盖设备内容。
    updateScriptFileEditorControls(); // wjy: 绘制完成后同步真实 QTextEdit/QPushButton 的显隐和层级，保证页面切换时不会残留在其它页面。

// =====wjy====
    if (m_draggingDevice && !m_draggingDeviceIndexes.isEmpty()) {
        const bool deleteUngroupedDevices = deviceIndexesAreAllUngrouped(m_draggingDeviceIndexes); // wjy: 绘制提示和松开动作复用同一判断，避免界面写“删除”但实际只移出分组。
        drawTopDragDropZone(
            painter,
            deleteUngroupedDevices
                ? QString::fromUtf8("删除设备")
                : QString::fromUtf8("移出分组")); // wjy: 根部设备显示删除入口；包含分组设备的拖拽继续显示原有移出分组入口。
    } else if (m_draggingGroup) {
        drawTopDragDropZone(painter, QString::fromUtf8("解散分组"));
    }

    if (m_draggingDevice
        && !m_draggingDeviceIndexes.isEmpty()
        && m_draggingDeviceIndex >= 0
        && m_draggingDeviceIndex
               < g_devices.size()) {
        const QRect ghostRect = deviceDragGhostRect(m_deviceDragCurrentPos, size()); // wjy: 绘制和释放落点共用同一个虚影矩形，避免鼠标越界时判断和视觉不一致。
        const int ghostX = ghostRect.x();
        const int ghostY = ghostRect.y();
        const int ghostWidth = ghostRect.width();
        const int draggingDeviceCount =
            m_draggingDeviceIndexes.size();

        const QString ghostName =
            draggingDeviceCount > 1
                ? QString::fromUtf8("%1 台设备")
                      .arg(draggingDeviceCount)
                : deviceDisplayName(
                      g_devices.at(
                          m_draggingDeviceIndex));
        painter.save();
        painter.setOpacity(0.72); // wjy: 半透明效果表示“正在拖动的临时虚影”，不是真实列表行。
        painter.setPen(QPen(QColor(QStringLiteral("#B9C3D0")), 1));
        painter.setBrush(QColor(QStringLiteral("#F7FAFE")));
        painter.drawRoundedRect(QRectF(ghostRect), 6, 6);
        painter.setPen(Qt::NoPen);
        painter.setBrush(deviceListStatusAccentColor(devicePresenceForIndex(m_draggingDeviceIndex)));
        painter.drawEllipse(QRectF(ghostX + 18, ghostY + 15, 6, 6));
        drawDeviceTileIcon(painter, ghostX + 34, ghostY + 8, 20);
        painter.setOpacity(0.88); // wjy: 文字和图标比背景稍清楚，拖动时仍能看出是哪台设备。
        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#111827")));
        painter.drawText(QRectF(ghostX + 66, ghostY + 7, ghostWidth - 78, 22), Qt::AlignVCenter | Qt::AlignLeft, ghostName);
        painter.restore();
    }

    if (m_draggingGroup
        && m_draggingGroupIndex >= 0
        && m_draggingGroupIndex < g_deviceGroupNames.size()) {
        const QRect ghostRect = groupDragGhostRect(m_groupDragCurrentPos, size());
        painter.save();
        painter.setOpacity(0.76);
        painter.setPen(QPen(QColor(QStringLiteral("#B9C3D0")), 1));
        painter.setBrush(QColor(QStringLiteral("#FCFDFF")));
        painter.drawRoundedRect(QRectF(ghostRect), 6, 6);
        drawResourceIcon(painter, QRect(ghostRect.x() + 14, ghostRect.y() + 8, 20, 20), QStringLiteral("settings/remote_wakeup.svg"));
        painter.setOpacity(0.9);
        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#111827")));
        painter.drawText(
            QRectF(ghostRect.x() + 44, ghostRect.y() + 7, ghostRect.width() - 56, 22),
            Qt::AlignVCenter | Qt::AlignLeft,
            QFontMetrics(textFont).elidedText(g_deviceGroupNames.at(m_draggingGroupIndex), Qt::ElideRight, ghostRect.width() - 56));
        painter.restore();
    }
// ===end====

// =====wjy====
    if (shouldLogPaint) {
        writeDeviceGridStartupLog(QStringLiteral("[wjy-paint] end index=%1").arg(paintLogIndex)); // wjy: 记录本次绘制完整结束，和 begin 配对判断首次绘制是否正常。
    }
// ===end====
}

// =====wjy====
void DeviceGrid::refreshLocalSystemInfoTab()
{
    m_localSystemInfo = platform::LocalSystemInfoService::local(); // wjy: 仅在用户进入本机页时读取静态信息，启动和普通设备切换不增加系统查询。
    m_localDiskRefreshTick = 0; // wjy: 完整快照已经包含最新磁盘容量，从此重新累计十秒再做第一次轻量刷新。
    if (m_localCpuUsageAnimation) {
        m_localCpuUsageAnimation->stop();
    }
    if (m_localGpuUsageAnimation) {
        m_localGpuUsageAnimation->stop();
    }
    if (m_localMemoryUsageAnimation) {
        m_localMemoryUsageAnimation->stop(); // wjy: 重新进入本机页时停止三类旧过渡，所有圆环从新的页面快照重新开始。
    }
    m_localCpuUsageSampler.reset();
    m_localGpuUsageSampler.reset();
    m_localCpuUsagePercent = m_localCpuUsageSampler.samplePercent(); // wjy: CPU 首次采样只建立累计时间基线。
    m_localGpuUsagePercent = m_localGpuUsageSampler.samplePercent(); // wjy: GPU 首次采集创建通配符查询并建立性能计数器基线。
    m_localMemoryUsagePercent = -1.0;
    m_localMemoryUsage = m_localMemoryUsageSampler.sample(); // wjy: 首次进入立即取得已用、总量、可用容量和负载百分比。
    if (m_localMemoryUsage.percent >= 0) {
        animateLocalUsageTo(
            m_localMemoryUsageAnimation,
            m_localMemoryUsagePercent,
            m_localMemoryUsage.percent); // wjy: 内存无需双样本，进入页面后立即从零平滑显示当前负载。
    }
}

void DeviceGrid::animateLocalUsageTo(QVariantAnimation* animation, qreal& displayPercent, int targetPercent)
{
    const qreal boundedTarget = qBound<qreal>(0.0, targetPercent, 100.0); // wjy: 三类动画目标统一限制在合法百分比范围，显示层不信任外部采样值。
    const qreal startPercent = displayPercent < 0.0
        ? 0.0
        : qBound<qreal>(0.0, displayPercent, 100.0); // wjy: 第一个有效样本从零增长，后续从各自圆环当前角度自然衔接。
    if (!animation) {
        displayPercent = boundedTarget; // wjy: 单个动画对象异常缺失时仍显示对应真实样本，不拖累另外两类资源。
        update(scriptFileEditorRect()); // wjy: 降级路径同样只刷新本机详情区域。
        return;
    }

    animation->stop(); // wjy: 新样本到来时只接管对应资源的旧动画，不会停止另外两枚圆环。
    if (qAbs(startPercent - boundedTarget) < 0.01) {
        displayPercent = boundedTarget; // wjy: 数值未变化时直接保持目标，避免创建无位移动画和额外帧。
        update(scriptFileEditorRect());
        return;
    }
    animation->setStartValue(startPercent); // wjy: 起点使用当前屏幕数值，上一段动画未结束时更新也不会回跳。
    animation->setEndValue(boundedTarget); // wjy: 终点使用对应资源最新真实样本。
    animation->start(); // wjy: valueChanged 逐帧同时驱动该资源圆弧和中心数字。
}

void DeviceGrid::updateLocalSystemMonitorState()
{
    if (!m_localSystemInfoTimer) {
        return;
    }
    const bool localPageVisible = !m_detailPanelCollapsed
        && !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && m_deviceDetailTab == DeviceDetailTab::Local;
    if (localPageVisible) {
        if (!m_localSystemInfoTimer->isActive()) {
            m_localSystemInfoTimer->start(); // wjy: 只有本机页真实可见时启动一秒采样，重复刷新不会创建多个定时器。
        }
        return;
    }
    const bool hadLocalMonitorState = m_localSystemInfoTimer->isActive()
        || m_localCpuUsagePercent >= 0.0
        || m_localGpuUsagePercent >= 0.0
        || m_localMemoryUsagePercent >= 0.0; // wjy: 任意一类资源已采样都视为存在本机监控状态。
    if (m_localSystemInfoTimer->isActive()) {
        m_localSystemInfoTimer->stop();
    }
    m_localDiskRefreshTick = 0; // wjy: 页面主动切走时定时器可能来不及再次回调，统一在停止入口清空磁盘刷新计数。
    if (m_localCpuUsageAnimation) {
        m_localCpuUsageAnimation->stop();
    }
    if (m_localGpuUsageAnimation) {
        m_localGpuUsageAnimation->stop();
    }
    if (m_localMemoryUsageAnimation) {
        m_localMemoryUsageAnimation->stop(); // wjy: 离开本机页时三枚圆环立即停止过渡，隐藏页面不再继续刷新。
    }
    if (hadLocalMonitorState) {
        m_localCpuUsageSampler.reset();
        m_localGpuUsageSampler.reset(); // wjy: 离开页面立即丢弃 CPU 基线并关闭 GPU 查询，隐藏期间不继续消耗资源。
        m_localCpuUsagePercent = -1.0;
        m_localGpuUsagePercent = -1.0;
        m_localMemoryUsagePercent = -1.0;
        m_localMemoryUsage = {}; // wjy: 离开本机页同步清除容量快照，重新进入后等待新的同一时刻采样。
    }
}

void DeviceGrid::showLocalSystemInfoTab()
{
    m_settingsSelected = false;
    m_remoteAssistSelected = false;
    m_localInfoSelected = false;
    m_deviceDetailTab = DeviceDetailTab::Local; // wjy: 本机页属于设备详情导航，不使用旧的独立 localInfoSelected 页面状态。
    if (m_detailAnimationTimer) {
        m_detailAnimationTimer->stop();
    }
    refreshLocalSystemInfoTab();
    setDesktopHoverActive(false);
    clearBottomActionHover();
    updateAddDeviceControls();
    updateLocalInfoControls();
    updateSettingsControls();
    updateScriptFileEditorControls(); // wjy: 隐藏脚本/配置控件并同步启动本机三类资源定时器，防止页面控件重叠。
    update();
}

void DeviceGrid::setupDeviceSearchPanel()
{
    if (m_deviceSearchPanel) {
        return;
    }

    m_deviceSearchPanel = new DeviceSearchPanel(this); // wjy: 面板只创建一次并挂在 DeviceGrid 下，避免每次查找重新构造整套输入框和表格。
    m_deviceSearchPanel->deviceActivated = [this](const QString& deviceId) {
        selectDeviceFromSearch(deviceId); // wjy: 面板仅回传稳定 ID，主界面负责最新目录解析、展开分组和详情页切换。
    };
    m_deviceSearchPanel->hide();
}

void DeviceGrid::updateDeviceSearchPanel()
{
    if (!m_deviceSearchPanel) {
        return;
    }

    syncResponsiveLayoutState();
    const bool visible = !m_detailPanelCollapsed
        && !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && m_deviceDetailTab == DeviceDetailTab::Search; // wjy: 查找页逻辑状态可保留，但详情栏收起时真实面板必须隐藏并禁用。
    m_deviceSearchPanel->setGeometry(deviceSearchPanelRect()); // wjy: 每次重绘或缩放都使用配置页相同矩形，侧栏展开/收起后不会错位。
    m_deviceSearchPanel->setVisible(visible);
    m_deviceSearchPanel->setEnabled(visible);
    if (visible) {
        m_deviceSearchPanel->raise(); // wjy: 真实输入框和表格覆盖详情区手绘背景，但不遮挡上方页签栏。
    }
}

void DeviceGrid::showDeviceSearchPanel()
{
    setupDeviceSearchPanel();

    QVector<DeviceSearchItem> searchItems;
    searchItems.reserve(g_devices.size()); // wjy: 每次进入查找页都复制当前展示字段，面板过滤不持有目录引用也不修改设备数据。
    for (const DeviceEntry& device : g_devices) {
        if (deviceHiddenByLocalPreference(device)) {
            continue; // wjy: 隐藏本机后搜索面板也不能通过名称或 IP 找到并重新选择本机。
        }
        const int groupIndex = g_deviceCatalog.groupIndexForId(device.groupId);
        const QString groupName = groupIndex >= 0 && groupIndex < g_deviceGroupNames.size()
            ? g_deviceGroupNames.at(groupIndex).trimmed()
            : device.group.trimmed(); // wjy: 优先通过稳定 groupId 取得最新组名，兼容尚未迁移完成的旧分组名称记录。
        searchItems.append(DeviceSearchItem{
            device.id,
            deviceDisplayName(device),
            groupName,
            device.mac.trimmed(),
            device.ip.trimmed(),
        });
    }

    m_deviceSearchPanel->setDevices(std::move(searchItems)); // wjy: 使用最新设备快照刷新候选，同时保留用户此前输入的查找条件。
    m_settingsSelected = false;
    m_remoteAssistSelected = false;
    m_localInfoSelected = false;
    m_deviceDetailTab = DeviceDetailTab::Search; // wjy: 页签点击和 Ctrl+F 统一切换同一个嵌入式查找状态。
    if (m_detailAnimationTimer) {
        m_detailAnimationTimer->stop();
    }
    setDesktopHoverActive(false);
    clearBottomActionHover();
    updateAddDeviceControls();
    updateLocalInfoControls();
    updateSettingsControls();
    updateScriptFileEditorControls(); // wjy: 隐藏脚本/配置真实控件并显示查找面板，避免两个 QWidget 页面重叠接收鼠标。
    m_deviceSearchPanel->focusPrimaryInput();
    update();
}

void DeviceGrid::selectDeviceFromSearch(const QString& deviceId)
{
    const int deviceIndex = g_deviceCatalog.deviceIndexForId(deviceId);
    if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return; // wjy: 查找期间设备若被同步删除，安全忽略过期结果，不使用面板快照中的旧数组位置。
    }

    finishDeviceGroupRename(true);
    finishDeviceRename(true); // wjy: 展开分组会改变左侧行坐标，先提交现有行内编辑，避免输入框停在错误设备上。
    if (m_deviceListScrollbarAnimation) {
        m_deviceListScrollbarAnimation->stop(); // wjy: 取消旧的轨道点击动画，防止定位完成后又被上一段动画拉回原位置。
    }

    const DeviceEntry& device = g_devices.at(deviceIndex);
    int groupIndex = g_deviceCatalog.groupIndexForId(device.groupId);
    if (groupIndex < 0) {
        groupIndex = g_deviceCatalog.groupIndexForName(device.group); // wjy: 旧设备缺少 groupId 时按兼容组名展开，保证迁移数据也能被搜索定位。
    }
    if (groupIndex >= 0 && groupIndex < g_deviceGroupIds.size()) {
        g_deviceCatalog.setGroupExpanded(g_deviceGroupIds.at(groupIndex), true); // wjy: 仅改变本机列表展开状态，不保存或同步任何设备字段。
    }

    setDetailPanelCollapsed(false); // wjy: 搜索结果需要显示配置详情时展开右侧区域，设备栏本身从不再被隐藏。
    m_deviceGroupExpanded = true; // wjy: 确保“我的设备”展开，让目标设备在左侧列表中可见。
    syncResponsiveLayoutState();
    m_settingsSelected = false;
    m_remoteAssistSelected = false;
    m_localInfoSelected = false; // wjy: 从设置、本机信息或协助页返回普通设备详情，右侧立即显示搜索目标。
    m_deviceDetailTab = DeviceDetailTab::Config; // wjy: 双击候选后离开查找页，直接在配置页查看已定位设备。
    m_selectedDeviceIndexes.clear();
    m_selectedDeviceIndexes.insert(deviceIndex);
    m_draggingDeviceIndexes.clear();
    m_selectionAnchorDeviceIndex = deviceIndex; // wjy: 搜索定位收敛为单选，并把 Shift 范围选择锚点同步到目标设备。

    const int rowIndex = visualRowIndexForDeviceIndex(deviceIndex, m_deviceStatuses); // wjy: 搜索定位遵循用户当前看到的在线优先自然排序位置。
    if (rowIndex >= 0) {
        const QRect viewport = deviceListViewportRect(true);
        const QRect currentRowRect = scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset);
        int targetOffset = m_deviceListScrollOffset;
        if (currentRowRect.top() < viewport.top()) {
            targetOffset = visibleDeviceRowRect(rowIndex).top() - viewport.top(); // wjy: 目标在视口上方时把行顶移到列表顶部。
        } else if (currentRowRect.bottom() > viewport.bottom()) {
            targetOffset = visibleDeviceRowRect(rowIndex).bottom() - viewport.bottom(); // wjy: 目标在视口下方时只滚动到完整露出该行。
        }
        m_deviceListScrollOffset = qBound(0, targetOffset, maxDeviceListScrollOffset());
    }

    setDesktopHoverActive(false);
    clearBottomActionHover();
    updateAddDeviceControls();
    updateLocalInfoControls();
    updateSettingsControls();
    startDeviceSwitchAnimation(deviceIndex, deviceDisplayName(device)); // wjy: 复用现有设备切换入口，脚本面板和右侧详情状态随目标设备一起恢复。
    updateScriptFileEditorControls(); // wjy: 目标设备状态恢复后再显示配置控件，避免短暂沿用上一台设备的编辑内容。
    update();
}
// ===end====

void DeviceGrid::startDeviceSwitchAnimation(int newIndex, const QString& newName)
{
    if (newIndex == m_selectedDeviceIndex && newName == m_currentDeviceName) {
        update();
        return;
    }

    saveCurrentScriptUiState(); // wjy: 切换设备前保存当前设备的脚本终端和文件编辑器状态，避免显示串到下一台设备。
    m_previousDeviceIndex = m_selectedDeviceIndex;
    m_previousDeviceName = m_currentDeviceName;
    m_selectedDeviceIndex = newIndex;
    m_currentDeviceName = newName;
    loadScriptUiStateForDevice(currentScriptUiDeviceIp()); // wjy: 新设备详情页加载它自己的脚本 UI；没有历史状态时下方面板保持空白。
    m_detailAnimationProgress = 1.0; // wjy: 用户不需要设备详情从下往上弹出的过渡，切换时直接显示目标设备。
    if (m_detailAnimationTimer) {
        m_detailAnimationTimer->stop(); // wjy: 不再启动详情切换动画，避免 paintEvent 进入滑动绘制分支。
    }
    update();
}

// =====wjy====
void DeviceGrid::pruneHiddenDeviceSelections()
{
    QSet<int> unhiddenDeviceIndexes; // wjy: 这里只排除本机隐藏策略，不把折叠分组误判成需要清除的选择。
    int fallbackDeviceIndex = -1; // wjy: 当前主设备是本机时，用第一台未隐藏设备作为详情页兜底目标。
    int firstSelectedUnhiddenDeviceIndex = -1; // wjy: 如果多选里还有未隐藏设备，优先用它作为新的主设备。

    for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        if (deviceHiddenByLocalPreference(g_devices.at(deviceIndex))) {
            continue; // wjy: 开关开启后，本机不能继续留在主选择、多选或拖拽快照中。
        }

        unhiddenDeviceIndexes.insert(deviceIndex);
        if (fallbackDeviceIndex < 0) {
            fallbackDeviceIndex = deviceIndex; // wjy: 保留目录中的第一台未隐藏设备作为兜底，兼容折叠分组仍可保持详情的旧行为。
        }
        if (firstSelectedUnhiddenDeviceIndex < 0
            && m_selectedDeviceIndexes.contains(deviceIndex)) {
            firstSelectedUnhiddenDeviceIndex = deviceIndex; // wjy: 保留第一台仍未隐藏的已选设备。
        }
    }

    for (auto it = m_selectedDeviceIndexes.begin(); it != m_selectedDeviceIndexes.end();) {
        if (!unhiddenDeviceIndexes.contains(*it)) {
            it = m_selectedDeviceIndexes.erase(it); // wjy: 本机被隐藏或下标失效后，不再保留在多选集合里。
        } else {
            ++it;
        }
    }

    for (auto it = m_draggingDeviceIndexes.begin(); it != m_draggingDeviceIndexes.end();) {
        if (!unhiddenDeviceIndexes.contains(*it)) {
            it = m_draggingDeviceIndexes.erase(it); // wjy: 隐藏本机也不能留在后续批量拖拽快照里。
        } else {
            ++it;
        }
    }

    if (m_selectionAnchorDeviceIndex >= 0
        && !unhiddenDeviceIndexes.contains(m_selectionAnchorDeviceIndex)) {
        m_selectionAnchorDeviceIndex = firstSelectedUnhiddenDeviceIndex; // wjy: Shift 锚点是隐藏本机时，改成仍未隐藏的选中设备；没有则清空。
    }

    if (m_selectedDeviceIndex >= 0
        && m_selectedDeviceIndex < g_devices.size()
        && unhiddenDeviceIndexes.contains(m_selectedDeviceIndex)) {
        return; // wjy: 右侧详情主设备未被本机策略隐藏时，只需要完成上面的集合清理。
    }

    const int nextDeviceIndex = firstSelectedUnhiddenDeviceIndex >= 0
        ? firstSelectedUnhiddenDeviceIndex
        : fallbackDeviceIndex; // wjy: 主设备是隐藏本机时，优先切到未隐藏的选中设备，否则切到第一台未隐藏设备。
    if (nextDeviceIndex < 0) {
        saveCurrentScriptUiState(); // wjy: 没有未隐藏设备前先保存当前脚本 UI，避免清空详情时丢掉本机状态。
        m_selectedDeviceIndexes.clear(); // wjy: 当前没有任何未隐藏设备时，清空左侧选择，避免本机继续高亮或被拖拽。
        m_draggingDeviceIndexes.clear();
        m_selectedDeviceIndex = -1; // wjy: 隐藏本机可能让目录仍有记录但列表没有可见设备，主选择必须显式清空。
        m_previousDeviceIndex = -1;
        m_selectionAnchorDeviceIndex = -1;
        m_currentDeviceName.clear();
        m_previousDeviceName.clear();
        loadScriptUiStateForDevice(QString());
        return;
    }

    saveCurrentScriptUiState(); // wjy: 主设备是隐藏本机时先保存旧设备脚本 UI，再切换到未隐藏设备。
    m_selectedDeviceIndex = nextDeviceIndex; // wjy: 将右侧详情主设备同步到未隐藏设备，避免详情继续指向本机。
    m_previousDeviceIndex = nextDeviceIndex;
    m_currentDeviceName = deviceDisplayName(g_devices.at(nextDeviceIndex));
    m_previousDeviceName = m_currentDeviceName;
    loadScriptUiStateForDevice(currentScriptUiDeviceIp()); // wjy: 恢复新主设备自己的脚本 UI，避免沿用隐藏本机的面板。
    m_selectedDeviceIndexes.insert(nextDeviceIndex); // wjy: 新主设备必须在左侧多选集合里，保证视觉选中态一致。
    if (m_selectionAnchorDeviceIndex < 0) {
        m_selectionAnchorDeviceIndex = nextDeviceIndex; // wjy: 没有可用 Shift 锚点时，用新主设备作为下一次范围选择起点。
    }
    if (m_detailAnimationTimer) {
        m_detailAnimationTimer->stop(); // wjy: 隐藏本机引发的主设备兜底切换不播放详情页切换动画。
    }
}
// ===end====

void DeviceGrid::setDesktopHoverActive(bool active)
{
    if (m_desktopHovered == active) {
        return;
    }

    m_desktopHovered = active;
    m_desktopHoverStartProgress = m_desktopHoverProgress;
    m_desktopHoverClock.restart();
    m_desktopHoverTimer->start();
}

void DeviceGrid::updateDesktopHover(const QPoint& position)
{
    Q_UNUSED(position)
    setDesktopHoverActive(false); // wjy: 进入桌面改为双击左侧设备行触发，右侧桌面图不再显示可点击悬停态。
}

void DeviceGrid::updateBottomActionHover(const QPoint& position)
{
    Q_UNUSED(position)
    BottomAction nextAction = BottomAction::None;

    if (m_hoveredBottomAction == nextAction) {
        return;
    }

    m_hoveredBottomAction = nextAction;
    update();
}

void DeviceGrid::clearBottomActionHover()
{
    if (m_hoveredBottomAction == BottomAction::None) {
        return;
    }

    m_hoveredBottomAction = BottomAction::None;
    update();
}

// =====wjy====
void DeviceGrid::animateDeviceListScrollTo(int targetOffset)
{
    const int boundedTarget = qBound(0, targetOffset, maxDeviceListScrollOffset()); // wjy: 动画开始前先限制目标，列表数量或窗口高度变化时不会滑出有效范围。
    if (!m_deviceListScrollbarAnimation) {
        m_deviceListScrollbarAnimation = new QVariantAnimation(this);
        m_deviceListScrollbarAnimation->setDuration(140); // wjy: 140 毫秒足够看见滑动方向，同时保持轨道点击的快速响应感。
        m_deviceListScrollbarAnimation->setEasingCurve(QEasingCurve::OutCubic); // wjy: 前段快速、末端柔和减速，避免线性移动显得生硬。
        connect(m_deviceListScrollbarAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            m_deviceListScrollOffset = qBound(0, value.toInt(), maxDeviceListScrollOffset()); // wjy: 每帧重新夹紧，动画期间分组折叠或窗口缩放也不会产生越界偏移。
            update(deviceListViewportRect(true));
        });
    }

    m_deviceListScrollbarAnimation->stop(); // wjy: 连续点击轨道时从当前画面重新起步，不让多个动画同时争抢同一个偏移。
    if (boundedTarget == m_deviceListScrollOffset) {
        return;
    }
    m_deviceListScrollbarAnimation->setStartValue(m_deviceListScrollOffset);
    m_deviceListScrollbarAnimation->setEndValue(boundedTarget);
    m_deviceListScrollbarAnimation->start();
}

void DeviceGrid::animateSettingsScrollTo(int targetOffset)
{
    const int maxOffset = m_settingsTab == SettingsTab::Keyboard
        ? keyboardShortcutMaxScrollOffset()
        : maxSettingsScrollOffset(m_settingsLocalInfoExpanded, m_settingsAddDeviceExpanded);
    const int boundedTarget = qBound(0, targetOffset, maxOffset); // wjy: 设置卡片展开状态决定当前滚动上限，动画目标必须使用本帧真实范围。
    if (!m_settingsScrollbarAnimation) {
        m_settingsScrollbarAnimation = new QVariantAnimation(this);
        m_settingsScrollbarAnimation->setDuration(140); // wjy: 与设备区保持相同速度，两个滚动条的点击手感一致。
        m_settingsScrollbarAnimation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_settingsScrollbarAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            const int currentMax = m_settingsTab == SettingsTab::Keyboard
                ? keyboardShortcutMaxScrollOffset()
                : maxSettingsScrollOffset(m_settingsLocalInfoExpanded, m_settingsAddDeviceExpanded);
            m_settingsScrollOffset = qBound(0, value.toInt(), currentMax); // wjy: 动画每帧同步设置页偏移，避免折叠卡片后保留无效位置。
            updateSettingsControls();
            updateAddDeviceControls();
            updateLocalInfoControls();
            update(); // wjy: 手绘卡片、滚动条和真实输入控件共同滑动，不出现控件瞬移或落后一帧。
        });
    }

    m_settingsScrollbarAnimation->stop();
    if (boundedTarget == m_settingsScrollOffset) {
        return;
    }
    m_settingsScrollbarAnimation->setStartValue(m_settingsScrollOffset);
    m_settingsScrollbarAnimation->setEndValue(boundedTarget);
    m_settingsScrollbarAnimation->start();
}
// ===end====

//鼠标时间处理函数
void DeviceGrid::mousePressEvent(QMouseEvent* event)
{
    syncResponsiveLayoutState();
// =====wjy====
    if (event->button() == Qt::LeftButton) {
        beginWindowResize(event->pos(), event->globalPosition().toPoint());
        if (m_resizingWindow) {
            event->accept();
            return;
        }
    }

    // =====wjy====
    if (event->button() == Qt::LeftButton && m_deviceGroupExpanded) {
        const int maxScrollOffset = maxDeviceListScrollOffset();
        const QRect track = deviceListScrollbarTrackRect();
        const QRect thumb = deviceListScrollbarThumbRect(m_deviceListScrollOffset);
        const QRect trackHitRect = track.adjusted(-4, -2, 1, 2).intersected(deviceListViewportRect(true)); // wjy: 轨道空白也纳入命中区，点击后让滑块中心跳到鼠标位置。
        const bool thumbHit = thumb.isValid() && thumb.adjusted(-4, -2, 1, 2).intersected(deviceListViewportRect(true)).contains(event->pos());
        if (maxScrollOffset > 0 && trackHitRect.contains(event->pos())) {
            finishDeviceGroupRename(true); // wjy: 滚动会改变行坐标，抓取前先提交并关闭正在显示的分组编辑框。
            finishDeviceRename(true);
            m_draggingDeviceListScrollbar = true; // wjy: 从此处起本次左键手势只控制滚动条。
            m_deviceListScrollbarGrabOffsetY = thumbHit
                ? qBound(0, event->pos().y() - thumb.top(), qMax(0, thumb.height() - 1))
                : thumb.height() / 2; // wjy: 点击滑块保留原抓取点，点击轨道空白则按滑块中心定位，避免跳到鼠标上方或下方。
            m_groupDragCandidateActive = false; // wjy: 清除任何残留行拖拽候选，滚动条手势不能移动分组。
            m_deviceDragCandidateActive = false;
            m_draggingGroup = false;
            m_draggingDevice = false;
            if (thumbHit) {
                if (m_deviceListScrollbarAnimation) m_deviceListScrollbarAnimation->stop(); // wjy: 用户直接抓住滑块时立即接管，停止尚未完成的轨道点击动画。
            } else {
                const int targetOffset = deviceListScrollOffsetForThumbTop(event->pos().y() - m_deviceListScrollbarGrabOffsetY);
                animateDeviceListScrollTo(targetOffset); // wjy: 点击设备轨道空白时不再闪现，用短促缓动把滑块中心移动到鼠标位置。
            }
            setCursor(Qt::ClosedHandCursor); // wjy: 闭合手型表示滑块已经被抓住。
            update(deviceListViewportRect(true)); // wjy: 按下时立即切换抓取颜色，实际偏移由动画或后续拖拽持续刷新。
            event->accept();
            return;
        }
    }
    // ===end====

    // =====wjy====
    if (event->button() == Qt::LeftButton
        && !m_detailPanelCollapsed
        && m_settingsSelected
        && (m_settingsTab == SettingsTab::General || m_settingsTab == SettingsTab::Keyboard)) {
        const int maxScrollOffset = m_settingsTab == SettingsTab::Keyboard
            ? keyboardShortcutMaxScrollOffset()
            : maxSettingsScrollOffset(m_settingsLocalInfoExpanded, m_settingsAddDeviceExpanded);
        const QRect track = settingsVerticalScrollbarTrackRect();
        const QRect thumb = settingsVerticalScrollbarThumbRect(m_settingsScrollOffset, maxScrollOffset);
        const QRect trackHitRect = track.adjusted(-5, -2, 5, 2); // wjy: 设置滚动条视觉较窄，扩大横向和纵向命中区后更容易拖动且不改变绘制宽度。
        const bool thumbHit = thumb.isValid() && thumb.adjusted(-5, -2, 5, 2).contains(event->pos());
        if (maxScrollOffset > 0 && trackHitRect.contains(event->pos())) {
            m_draggingSettingsScrollbar = true; // wjy: 常规页和键盘页共用滚动条，按下后独占当前左键手势，不会误触发卡片或快捷键输入框。
            m_settingsScrollbarGrabOffsetY = thumbHit
                ? qBound(0, event->pos().y() - thumb.top(), qMax(0, thumb.height() - 1))
                : thumb.height() / 2; // wjy: 轨道空白点击按滑块中心跳转，直接点击滑块则保留原抓取点。
            if (thumbHit) {
                if (m_settingsScrollbarAnimation) m_settingsScrollbarAnimation->stop(); // wjy: 直接拖动优先于特效，滑块必须立即完全跟随鼠标。
            } else {
                const int targetOffset = settingsScrollOffsetForThumbTop(
                    event->pos().y() - m_settingsScrollbarGrabOffsetY, maxScrollOffset);
                animateSettingsScrollTo(targetOffset); // wjy: 常规页轨道空白点击使用与设备区相同的快速缓动特效。
            }
            update(); // wjy: 按下反馈立即绘制，偏移变化由动画逐帧同步真实控件。
            setCursor(Qt::ClosedHandCursor); // wjy: 明确反馈设置页滚动条已经抓取。
            event->accept();
            return;
        }
    }
    // ===end====

    if (event->button() == Qt::LeftButton
        && !m_detailPanelCollapsed
        && !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && m_deviceDetailTab == DeviceDetailTab::ScriptLog
        && (scriptTerminalExecuteButtonRect().contains(event->position())
            || scriptTerminalStopButtonRect().contains(event->position()))) {
        if (scriptTerminalStopButtonRect().contains(event->position())) {
            stopCurrentDeviceScript(); // wjy: 停止时先要求目标设备 taskkill 脚本进程树，再让本地 SSH 执行会话退出。
        } else if (!m_scriptOutputRunning && !m_lastScriptEntryPath.trimmed().isEmpty()) {
            executeCurrentDeviceScriptFolder(m_lastScriptEntryPath);
        }
        event->accept();
        return;
    }

    if (m_periodicDeviceDiscoveryIntervalEdit
        && m_periodicDeviceDiscoveryIntervalEdit->hasFocus()
        && !m_periodicDeviceDiscoveryIntervalEdit->geometry().contains(event->pos())) {
        m_periodicDeviceDiscoveryIntervalEdit->clearFocus();
        setFocus(Qt::MouseFocusReason); // wjy: 点击周期检查输入框外部时立即保存秒数并重启定时器。
    }
    if (m_wallpaperRotationIntervalEdit
        && m_wallpaperRotationIntervalEdit->hasFocus()
        && !m_wallpaperRotationIntervalEdit->geometry().contains(event->pos())) {
        m_wallpaperRotationIntervalEdit->clearFocus();
        setFocus(Qt::MouseFocusReason); // wjy: 点击分钟输入框外部时立即保存新周期并从当前时刻重新计时。
    }
    if (m_batchSubnetEdit
        && m_batchSubnetEdit->hasFocus()
        && !m_batchSubnetEdit->geometry().contains(event->pos())) {
        m_batchSubnetEdit->clearFocus(); // wjy: 批量新增网段输入框点击设置页空白后主动失焦，行为和自动刷新秒数输入框一致。
        setFocus(Qt::MouseFocusReason);
    }

    if (m_deviceGroupNameEdit
        && m_deviceGroupNameEdit->isVisible()
        && !m_deviceGroupNameEdit->geometry().contains(event->pos())) { // wjy: 点击分组输入框外部时，提交当前名字并关闭输入框。
        finishDeviceGroupRename(true);
    }
    if (m_deviceListNameEdit
        && m_deviceListNameEdit->isVisible()
        && !m_deviceListNameEdit->geometry().contains(event->pos())) {
        finishDeviceRename(true);
    }

    if (event->button() == Qt::RightButton && m_deviceGroupExpanded) {
        const QVector<DeviceListRow> rows = visibleDeviceRows(m_deviceStatuses); // wjy: 设备右键命中复用在线优先自然排序后的可见行，保证菜单对应真实设备。
        const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded);
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const DeviceListRow& row = rows.at(rowIndex);
            if (row.type != DeviceListRow::Type::Device) {
                continue; // wjy: 这里专门处理设备行，分组行仍交给下面已有的分组右键菜单。
            }

            const QRect rowRect = scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset);
            const QRect hitRect = rowRect.intersected(deviceListClip);
            if (!hitRect.contains(event->pos())) {
                continue;
            }

            const int deviceIndex = row.deviceIndex;
            if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
                continue;
            }

            const bool keepMultiSelection = m_selectedDeviceIndexes.contains(deviceIndex);
            if (!keepMultiSelection) {
                m_selectedDeviceIndexes.clear();
                m_selectedDeviceIndexes.insert(deviceIndex);
                m_selectionAnchorDeviceIndex = deviceIndex;
            } else if (m_selectionAnchorDeviceIndex < 0) {
                m_selectionAnchorDeviceIndex = deviceIndex;
            }
            m_settingsSelected = false;
            m_remoteAssistSelected = false;
            m_localInfoSelected = false;
            setDesktopHoverActive(false);
            clearBottomActionHover();
            updateAddDeviceControls();
            updateLocalInfoControls();
            updateSettingsControls();
            startDeviceSwitchAnimation(deviceIndex, deviceDisplayName(g_devices.at(deviceIndex))); // wjy: 右键某台设备时先让右侧详情和后续脚本目标统一到这台设备。
            const QVector<int> targetDeviceIndexes = contextDeviceIndexesForRightClick(deviceIndex);
            // =====wjy====
            showDeviceContextMenuForIndexes(deviceIndex, targetDeviceIndexes, mapToGlobal(event->pos())); // wjy: 设备列表右键改为调用共享菜单，和远控标题栏保证完全相同的动作顺序与目标分发。
            // ===end====
            event->accept();
            return;
        }
    }

    if (event->button() == Qt::RightButton && m_deviceGroupExpanded) { // wjy: 详情栏收起时分组右键仍可用，设备栏交互不受影响。
        const QVector<DeviceListRow> rows = visibleDeviceRows(m_deviceStatuses); // wjy: 分组右键命中也复用当前状态排序快照，滚动后坐标和绘制保持一致。
        const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 只允许右键当前可见视口里的分组行。
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const DeviceListRow& row = rows.at(rowIndex);
            if (row.type != DeviceListRow::Type::Group) {
                continue; // wjy: 设备行暂时不弹出这个分组菜单，只处理分组下拉框本身。
            }

            const QRect rowRect =
                scrolledVisibleDeviceRowRect(
                    rowIndex,
                    m_deviceListScrollOffset);

            const QRect hitRect =
                rowRect.intersected(deviceListClip);

            if (!hitRect.contains(event->pos())) {
                continue;
            }

            QMenu menu(this); // wjy: 创建分组右键菜单，用来放分组相关操作入口。
            QMenu* scriptMenu = menu.addMenu(QString::fromUtf8("启动脚本")); // wjy: 分组右键用“启动脚本”明确表示将对组内设备批量启动所选脚本，底层批量执行逻辑保持不变。
            populateCachedScriptFolderMenu(scriptMenu); // wjy: 分组菜单复用缓存目录，右键操作不会再触发 SMB 等待。
            QAction* stopScriptsAction = menu.addAction(QString::fromUtf8("停止脚本")); // wjy: 分组右键用“停止脚本”与启动入口成对显示，点击后仍遍历组内设备停止全部脚本进程树。
            menu.addSeparator(); // wjy: 系统子菜单上方加横杠，和脚本/停止动作分组显示。
            QMenu* systemMenu = menu.addMenu(menuIcon(QStringLiteral("settings.svg")), QString::fromUtf8("系统设置"));
            QAction* wakeGroupAction = systemMenu->addAction(menuIcon(QStringLiteral("power_on.svg")), QString::fromUtf8("批量开机")); // wjy: 分组开机与设备开机共用绿色启动图标。
            QAction* shutdownGroupAction = systemMenu->addAction(menuIcon(QStringLiteral("shutdown.svg")), QString::fromUtf8("批量关机")); // wjy: 分组关机与设备关机共用红色关闭图标。
            QAction* restartGroupAction = systemMenu->addAction(menuIcon(QStringLiteral("restart.svg")), QString::fromUtf8("批量重启"));
            QAction* updateGroupAction = systemMenu->addAction(menuIcon(QStringLiteral("update.svg")), QString::fromUtf8("批量更新")); // wjy: 分组更新共用蓝色循环箭头，菜单顺序保持不变。
            QAction* terminalGroupAction = systemMenu->addAction(menuIcon(QStringLiteral("terminal.svg")), QString::fromUtf8("终端"));
            QAction* deleteGroupAction = menu.addAction(QString::fromUtf8("删除分组")); // wjy: 删除分组菜单项先显示出来，后续再补真正删除分组和设备归属处理。
            const QAction* selectedAction = menu.exec(mapToGlobal(event->pos())); // wjy: 在分组行右键位置弹出菜单。
            const QVector<int> groupDeviceIndexes = deviceIndexesForGroup(row.groupIndex);
            if (selectedAction && selectedAction->data().isValid()) {
                executeDeviceGroupScriptFolder(row.groupIndex, selectedAction->data().toString()); // wjy: 点击脚本叶子目录时，对该分组内所有设备启动同一个脚本。
            } else if (selectedAction == stopScriptsAction) {
                stopDeviceGroupScripts(row.groupIndex); // wjy: 点击批量停止时，对分组内所有正在运行脚本的设备发远端 taskkill。
            } else if (selectedAction == wakeGroupAction) {
                batchWakeDevices(groupDeviceIndexes); // wjy: 分组系统菜单批量开机只针对该分组内设备。
            } else if (selectedAction == shutdownGroupAction) {
                batchShutdownDevices(groupDeviceIndexes); // wjy: 分组系统菜单批量关机只针对该分组内设备。
            } else if (selectedAction == restartGroupAction) {
                batchRestartDevices(groupDeviceIndexes); // wjy: 分组系统菜单批量重启只针对该分组内设备。
            } else if (selectedAction == updateGroupAction) {
                batchUpdateDevices(groupDeviceIndexes); // wjy: 分组更新向每台在线设备发送异步请求，目标机各自判断是否需要更新。
            } else if (selectedAction == terminalGroupAction) {
                batchOpenDeviceTerminals(groupDeviceIndexes); // wjy: 分组系统菜单终端会为该分组内可连接设备打开终端。
            } else if (selectedAction == deleteGroupAction) {
                const int groupIndex = row.groupIndex; // wjy: 记录要删除的真实分组下标，不能用界面行号删除数据。
                if (groupIndex >= 0 && groupIndex < g_deviceGroupNames.size()) {
                    const QString deletedGroupName = g_deviceGroupNames.at(groupIndex).trimmed(); // wjy: 保存被删除的分组名，用来释放这个分组里的设备。
                    const QString deletedGroupId = g_deviceGroupIds.at(groupIndex);
                    g_deviceCatalog.removeGroup(deletedGroupId); // wjy: 目录同时删除分组并把组内设备移回根部，避免 UI 手工维护三组数组。
                    if (m_renamingDeviceGroupIndex == groupIndex) {
                        m_deviceGroupNameEdit->hide(); // wjy: 如果正好在重命名这个分组，删除后隐藏输入框。
                        m_renamingDeviceGroupIndex = -1; // wjy: 清空重命名状态，避免保存时再访问已删除分组。
                    } else if (m_renamingDeviceGroupIndex > groupIndex) {
                        --m_renamingDeviceGroupIndex; // wjy: 删除前面的分组后，后面正在编辑的分组下标需要前移一位。
                    }

                    m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset, maxDeviceListScrollOffset()); // wjy: 删除分组后列表变短，滚动位置要回到有效范围。
                    saveDevices(); // wjy: 保存删除后的 groups 和设备 group 字段，重启后保持删除结果。
                    update(); // wjy: 立即重绘，让分组消失并显示被释放到无分组的设备。
                }
            }
            event->accept();
            return;
        }
    }

    const QRect blankHitRect =
        scrolledDeviceGroupReservedBlankRect(
            m_deviceListScrollOffset)
            .intersected(
                deviceListViewportRect(
                    m_deviceGroupExpanded));

    if (event->button() == Qt::RightButton
        && m_deviceGroupExpanded
        && blankHitRect.contains(event->pos())) { // wjy: 确认右键点在滚动后的预留空白区域里。
        QMenu menu(this); // wjy: 创建右键菜单，父对象设为当前控件，交给 Qt 管理生命周期。
        QAction* createGroupAction = menu.addAction(QString::fromUtf8("新建分组")); // wjy: 保存菜单项指针，用来判断用户是否真的点击了“新建分组”。
        const QAction* selectedAction = menu.exec(mapToGlobal(event->pos())); // wjy: 在鼠标当前位置弹出菜单，点空白取消时返回空指针。
        if (selectedAction == createGroupAction) {
            createDefaultDeviceGroup(); // wjy: 空白区继续只创建空分组，但命名、稳定 ID 和展开规则与多选菜单共用同一入口。
            saveDevices(); // wjy: 保存 groups 字段，让新建分组关闭程序后还能恢复。
            update(); // wjy: 新增分组后重绘左侧列表，让分组立即显示在设备下面。
        }
        event->accept();
        return;
    }
// ===end====

    if (event->button() == Qt::LeftButton
        && event->pos().y() >= 0
        && event->pos().y() < kTitleBarHeight //窗口拖动
        && !(!m_detailPanelCollapsed && m_updateAvailable && titlebarUpdateRect().contains(event->pos())) // wjy: 只有完整标题栏中真实可见的更新按钮排除拖窗命中。
        && !(!m_detailPanelCollapsed && titlebarSettingsRect().contains(event->pos()))
        && !(!m_detailPanelCollapsed && refreshRect().contains(event->pos())) // wjy: 紧凑态隐藏设置和刷新后，对应旧矩形重新成为可拖动标题栏空白。
        && !minimizeRect().contains(event->pos())
        && !closeRect().contains(event->pos())) {
        cancelScreenEdgeAutoHide(); // wjy: 用户重新抓住标题栏即退出自动停靠；本次松开若仍贴近顶部或左右边缘会重新建立停靠状态。
        m_draggingWindow = true;
        m_dragOffset = event->globalPosition().toPoint() - window()->frameGeometry().topLeft();
        event->accept();
        return;
    }

// =====wjy====
    if (event->button() == Qt::LeftButton && m_deviceGroupExpanded) { // wjy: 详情栏收起时设备拖拽仍从完整设备栏正常开始。
        const QVector<DeviceListRow> rows = visibleDeviceRows(m_deviceStatuses); // wjy: 拖拽按下使用在线优先自然排序后的当前可见行，避免设备下标错位。
        const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 只允许在当前可见滚动视口内开始拖拽。
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const DeviceListRow& row = rows.at(rowIndex); // wjy: rowIndex 是界面行号，row.deviceIndex 才是真实设备下标。
            const QRect rowRect =
                scrolledVisibleDeviceRowRect(
                    rowIndex,
                    m_deviceListScrollOffset);

            const QRect hitRect =
                rowRect.intersected(deviceListClip);

            if (!hitRect.contains(event->pos())) {
                continue;
            }

            if (row.type == DeviceListRow::Type::Group) {
                if (row.groupIndex < 0 || row.groupIndex >= g_deviceGroupNames.size()) {
                    continue;
                }
                m_groupDragCandidateActive = true;
                m_draggingGroup = false;
                m_draggingGroupIndex = row.groupIndex;
                m_groupDragStartPos = event->pos();
                m_groupDragCurrentPos = event->pos();
                m_deviceDragCandidateActive = false;
                m_draggingDevice = false;
                m_draggingDeviceIndex = -1;
                m_draggingDeviceIndexes.clear();
                break;
            }

            if (row.type != DeviceListRow::Type::Device) {
                continue;
            }

            const int deviceIndex = row.deviceIndex;

            if (deviceIndex < 0
                || deviceIndex >= g_devices.size()) {
                continue;
            }

            m_deviceDragCandidateActive = true;
            m_draggingDevice = false;
            m_draggingDeviceIndex = deviceIndex;

            // 每一次按下设备，都重新生成本次拖拽快照。
            m_draggingDeviceIndexes.clear();

            if (m_selectedDeviceIndexes.contains(
                    deviceIndex)) {

                // 按在已经选中的设备上：
                // 拖动当前可见且已选中的设备。
                for (const DeviceListRow& selectedRow : rows) {
                    if (selectedRow.type == DeviceListRow::Type::Device
                        && selectedRow.deviceIndex >= 0
                        && selectedRow.deviceIndex < g_devices.size()
                        && m_selectedDeviceIndexes.contains(selectedRow.deviceIndex)) {
                        m_draggingDeviceIndexes.insert(selectedRow.deviceIndex); // wjy: 只把当前可见设备写入拖拽快照，折叠隐藏的选中设备不会被批量移动。
                    }
                }
                if (m_draggingDeviceIndexes.isEmpty()) {
                    m_draggingDeviceIndexes.insert(deviceIndex); // wjy: 极端情况下可见过滤为空时，至少拖动鼠标按下的这台设备。
                }
            } else {
                // 按在一个没有被选中的设备上：
                // 本次只拖动这一台，避免误移动原来的多选设备。
                m_draggingDeviceIndexes.insert(
                    deviceIndex);
            }

            m_doubleClickRemoteDeviceIndexes = m_draggingDeviceIndexes; // wjy: 第一次按下时保留原多选目标，双击事件发生前的单击释放即使改变选择也不会丢失批量远控范围。

            m_deviceDragStartPos = event->pos();
            m_deviceDragCurrentPos = event->pos();

            writeDeviceGridStartupLog(
                QStringLiteral(
                    "[wjy-drag] candidate "
                    "deviceIndex=%1 device=%2")
                    .arg(deviceIndex)
                    .arg(deviceDisplayName(
                        g_devices.at(deviceIndex))));
            break;
        }
    }
// ===end====

    QFrame::mousePressEvent(event);
}

void DeviceGrid::mouseMoveEvent(QMouseEvent* event)
{
    syncResponsiveLayoutState();
    if (m_resizingWindow && (event->buttons() & Qt::LeftButton)) {
        updateWindowResize(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    // =====wjy====
    if (m_draggingDeviceListScrollbar) {
        const int maxScrollOffset = maxDeviceListScrollOffset(); // wjy: 分组展开或窗口尺寸可能在拖动中变化，每次移动都读取最新滚动范围。
        if (!(event->buttons() & Qt::LeftButton) || maxScrollOffset <= 0) {
            m_draggingDeviceListScrollbar = false; // wjy: 左键状态丢失或内容不再溢出时立即结束，避免除以零和卡住抓取光标。
            m_deviceListScrollbarGrabOffsetY = 0;
            m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset, qMax(0, maxScrollOffset));
            unsetCursor();
            update(deviceListViewportRect(true));
            event->accept();
            return;
        }

        if (m_deviceListScrollbarAnimation) m_deviceListScrollbarAnimation->stop(); // wjy: 用户按住轨道后主动移动鼠标时立即结束缓动，切换为完全跟手的直接拖拽。
        const int requestedThumbTop = event->pos().y() - m_deviceListScrollbarGrabOffsetY; // wjy: 保留按下点到滑块顶部的距离，拖动不会跳位。
        const int newOffset = deviceListScrollOffsetForThumbTop(requestedThumbTop); // wjy: 鼠标越过轨道上下端时映射函数会直接夹到 0 或最大偏移。
        if (newOffset != m_deviceListScrollOffset) {
            m_deviceListScrollOffset = newOffset; // wjy: 直接更新手绘列表偏移，拖到最底端即可立即看到最后内容。
            update(deviceListViewportRect(true));
        }
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    // ===end====

    // =====wjy====
    if (m_draggingSettingsScrollbar) {
        const int maxScrollOffset = m_settingsTab == SettingsTab::Keyboard
            ? keyboardShortcutMaxScrollOffset()
            : maxSettingsScrollOffset(m_settingsLocalInfoExpanded, m_settingsAddDeviceExpanded); // wjy: 卡片展开或窗口尺寸变化后始终使用当前页最新滚动范围。
        if (!(event->buttons() & Qt::LeftButton) || maxScrollOffset <= 0) {
            m_draggingSettingsScrollbar = false;
            m_settingsScrollbarGrabOffsetY = 0;
            m_settingsScrollOffset = qBound(0, m_settingsScrollOffset, qMax(0, maxScrollOffset)); // wjy: 左键状态丢失或内容不再溢出时安全结束并夹紧偏移。
            unsetCursor();
            updateSettingsControls();
            updateAddDeviceControls();
            updateLocalInfoControls();
            update();
            event->accept();
            return;
        }

        if (m_settingsScrollbarAnimation) m_settingsScrollbarAnimation->stop(); // wjy: 鼠标移动表示用户接管滑块，特效立即让位给实时拖拽。
        const int requestedThumbTop = event->pos().y() - m_settingsScrollbarGrabOffsetY; // wjy: 保留按下点相对滑块顶部的距离，拖动过程中滑块不会突然跳位。
        const int newOffset = settingsScrollOffsetForThumbTop(requestedThumbTop, maxScrollOffset);
        if (newOffset != m_settingsScrollOffset) {
            m_settingsScrollOffset = newOffset;
            updateSettingsControls();
            updateAddDeviceControls();
            updateLocalInfoControls();
            update(); // wjy: 常规页手绘内容、下拉框和输入框全部跟随滑块实时移动。
        }
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    // ===end====

    if (m_draggingWindow && (event->buttons() & Qt::LeftButton)) {
        window()->move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }

// =====wjy====
    if (m_groupDragCandidateActive && (event->buttons() & Qt::LeftButton)) {
        m_groupDragCurrentPos = event->pos();
        const int movedDistance = (event->pos() - m_groupDragStartPos).manhattanLength();
        if (!m_draggingGroup && movedDistance >= QApplication::startDragDistance()) {
            m_draggingGroup = true;
            const QString groupName = (m_draggingGroupIndex >= 0 && m_draggingGroupIndex < g_deviceGroupNames.size())
                ? g_deviceGroupNames.at(m_draggingGroupIndex)
                : QString();
            writeDeviceGridStartupLog(QStringLiteral("[wjy-group-drag] start groupIndex=%1 group=%2 distance=%3")
                .arg(m_draggingGroupIndex)
                .arg(groupName)
                .arg(movedDistance));
        }
        if (m_draggingGroup) {
            setCursor(Qt::ClosedHandCursor);
            update();
            event->accept();
            return;
        }
    }

    if (m_deviceDragCandidateActive && (event->buttons() & Qt::LeftButton)) { // wjy: 只有按住左键移动时，才判断设备拖拽。
        m_deviceDragCurrentPos = event->pos(); // wjy: 拖拽候选期间持续记录鼠标位置，正式拖拽后虚影才能跟着走。
        const int movedDistance = (event->pos() - m_deviceDragStartPos).manhattanLength(); // wjy: 用曼哈顿距离判断移动是否超过 Qt 推荐拖拽阈值。
        if (!m_draggingDevice && movedDistance >= QApplication::startDragDistance()) {
            m_draggingDevice = true; // wjy: 超过阈值后正式进入设备拖拽识别状态。
            const QString deviceName = (m_draggingDeviceIndex >= 0 && m_draggingDeviceIndex < g_devices.size())
                ? deviceDisplayName(g_devices.at(m_draggingDeviceIndex))
                : QString(); // wjy: 防御性获取真实设备名，避免日志访问越界。
            writeDeviceGridStartupLog(QStringLiteral("[wjy-drag] start deviceIndex=%1 device=%2 distance=%3")
                .arg(m_draggingDeviceIndex)
                .arg(deviceName)
                .arg(movedDistance)); // wjy: 拖拽开始日志写入文件，不再输出到 Qt Creator 控制台。
        }
        if (m_draggingDevice) {
            setCursor(Qt::ClosedHandCursor); // wjy: 拖拽识别中给一个抓取光标反馈，但不绘制拖拽动画。
            update(); // wjy: 拖拽过程中持续重绘，让半透明设备虚影跟随鼠标移动。
            event->accept();
            return;
        }
    }
// ===end====

    if (m_detailPanelCollapsed) {
        setDesktopHoverActive(false);
        clearBottomActionHover(); // wjy: 紧凑窗口没有右侧桌面卡片或底部动作，不保留不可见悬停状态。
    } else {
        updateDesktopHover(event->pos());
        updateBottomActionHover(event->pos());
    }
    // =====wjy====
    // wjy: 悬停远控数字徽标时用气泡显示控制端设备名；扩大命中区并强制弹出 tooltip。
    {
        QString tip;
        if (m_deviceGroupExpanded) {
            const QVector<DeviceListRow> deviceRows = visibleDeviceRows(m_deviceStatuses); // wjy: 徽标悬停命中和当前在线优先绘制顺序保持一致。
            const QRect listClip = deviceListViewportRect(m_deviceGroupExpanded);
            for (int rowIndex = 0; rowIndex < deviceRows.size(); ++rowIndex) {
                const DeviceListRow& row = deviceRows.at(rowIndex);
                if (row.type != DeviceListRow::Type::Device) continue;
                const QRect rowRect = scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset);
                if (!listClip.intersects(rowRect)) continue;
                if (row.deviceIndex < 0 || row.deviceIndex >= g_devices.size()) continue;
                const QString ip = g_devices.at(row.deviceIndex).ip.trimmed();
                int count = qBound(0, m_deviceRemoteSessionCounts.value(ip, 0), 10);
                if (count <= 0 && devicePresenceForIndex(row.deviceIndex) == platform::DevicePresenceState::Busy) {
                    count = 1;
                }
                if (count <= 0) continue;
                // wjy: 命中区略放大，避免 18px 图标难悬停。
                const QRect iconRect(182, rowRect.y() + 6, 26, 24);
                if (!iconRect.contains(event->pos())) continue;
                const QString names = m_deviceRemoteControllerNames.value(ip).trimmed();
                if (names.isEmpty()) {
                    tip = QString::fromUtf8("正在被 %1 台设备控制").arg(count);
                } else {
                    const QStringList list = names.split(QLatin1Char(','), Qt::SkipEmptyParts);
                    tip = QString::fromUtf8("控制端：\n%1").arg(list.join(QStringLiteral("\n")));
                }
                break;
            }
        }
        if (toolTip() != tip) {
            setToolTip(tip);
            if (!tip.isEmpty()) {
                QToolTip::showText(event->globalPosition().toPoint(), tip, this);
            } else {
                QToolTip::hideText();
            }
        } else if (!tip.isEmpty() && !QToolTip::isVisible()) {
            QToolTip::showText(event->globalPosition().toPoint(), tip, this);
        }
    }
    // ===end====
    const int resizeEdges = windowResizeEdgesAt(event->pos(), size(), !m_detailPanelCollapsed); // wjy: 紧凑态不显示左右缩放光标，窗口宽度始终固定。
    if (resizeEdges == (kResizeLeft | kResizeTop) || resizeEdges == (kResizeRight | kResizeBottom)) {
        setCursor(Qt::SizeFDiagCursor);
        QFrame::mouseMoveEvent(event);
        return;
    }
    if (resizeEdges == (kResizeRight | kResizeTop) || resizeEdges == (kResizeLeft | kResizeBottom)) {
        setCursor(Qt::SizeBDiagCursor);
        QFrame::mouseMoveEvent(event);
        return;
    }
    if (resizeEdges & (kResizeLeft | kResizeRight)) {
        setCursor(Qt::SizeHorCursor);
        QFrame::mouseMoveEvent(event);
        return;
    }
    if (resizeEdges & (kResizeTop | kResizeBottom)) {
        setCursor(Qt::SizeVerCursor);
        QFrame::mouseMoveEvent(event);
        return;
    }
    // =====wjy====
    const int deviceListMaxScrollOffset = maxDeviceListScrollOffset();
    const bool deviceListScrollbarHovered = m_deviceGroupExpanded
        && deviceListMaxScrollOffset > 0
        && deviceListScrollbarTrackRect().adjusted(-4, -2, 1, 2).intersected(deviceListViewportRect(true)).contains(event->pos()); // wjy: 整条设备轨道都显示可抓取手型，滑块和空白位置使用相同点击语义。
    const int settingsMaxScrollOffset = m_settingsTab == SettingsTab::Keyboard
        ? keyboardShortcutMaxScrollOffset()
        : maxSettingsScrollOffset(m_settingsLocalInfoExpanded, m_settingsAddDeviceExpanded);
    const bool settingsScrollbarHovered = !m_detailPanelCollapsed
        && m_settingsSelected
        && (m_settingsTab == SettingsTab::General || m_settingsTab == SettingsTab::Keyboard)
        && settingsMaxScrollOffset > 0
        && settingsVerticalScrollbarTrackRect().adjusted(-5, -2, 5, 2).contains(event->pos()); // wjy: 常规页和键盘页整条滚动轨道均可拖拽或点击跳转，悬停时提前给出手型反馈。
    // ===end====
    const bool detailPanelButtonHovered = detailPanelToggleButtonRect(m_detailPanelCollapsed).contains(event->pos());
    const bool titlebarButtonHovered = !m_detailPanelCollapsed
        && ((m_updateAvailable && titlebarUpdateRect().contains(event->pos()))
            || titlebarSettingsRect().contains(event->pos())
            || refreshRect().contains(event->pos())); // wjy: 左上标题恢复为普通拖动区域，只有设置、更新和刷新入口显示按钮手型。
    const bool wakeButtonHovered = false;
    const SettingsLayoutSnapshot settingsLayout = settingsLayoutSnapshot(
        m_settingsLocalInfoExpanded,
        m_settingsAddDeviceExpanded,
        m_settingsScrollOffset,
        m_settingsTab == SettingsTab::Keyboard); // wjy: 命中测试与绘制、子控件几何共用同一设置页快照。
    const bool settingsSwitchHovered = !m_detailPanelCollapsed
        && m_settingsSelected
        && m_settingsTab == SettingsTab::General
        && (settingsLayout.containsPoint(settingsAutoRunSwitchRect(), event->pos())
            || settingsLayout.containsPoint(settingsRemoteWakeupSwitchRect(), event->pos())
            || settingsLayout.containsPoint(settingsPreventSleepSwitchRect(), event->pos())
            || settingsLayout.containsPoint(settingsPeriodicDeviceDiscoverySwitchRect(), event->pos())
            || settingsLayout.containsPoint(settingsHideLocalDeviceSwitchRect(), event->pos())
            || settingsLayout.containsPoint(settingsWallpaperRotationSwitchRect(), event->pos()) // wjy: 本机隐藏和壁纸开关都提供与其它设置一致的手型反馈。
            || (m_rollbackVersionCombo
                && !m_rollbackPreparing
                && !m_updatePreparing
                && !m_publishPreparing
                && !m_rollbackVersionCombo->currentData().toString().isEmpty()
                && settingsLayout.containsPoint(settingsRollbackButtonRect(), event->pos())) // wjy: 只有存在合法目标且未在准备时，回撤按钮才显示可点击手型。
            || (platform::UpdateService::canPublishCurrentBuild()
                && !m_publishPreparing
                && !m_updatePreparing
                && !m_rollbackPreparing
                && settingsLayout.containsPoint(settingsPublishUpdateButtonRect(), event->pos())) // wjy: 只有构建版本的可见发布按钮提供点击光标。
            || settingsLayout.containsPoint(settingsLocalInfoHeaderRect(), event->pos())
            || settingsLayout.containsPoint(settingsAddDeviceHeaderRect(m_settingsLocalInfoExpanded), event->pos()));
    const bool settingsTabHovered = !m_detailPanelCollapsed
        && m_settingsSelected
        && (settingsGeneralTabRect().contains(event->pos())
            || settingsKeyboardTabRect().contains(event->pos())
            || settingsRemoteControlTabRect().contains(event->pos())); // wjy: 第三个远控画质标签使用相同手型反馈和点击语义。
    const bool detailTabHovered = !m_detailPanelCollapsed
        && !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && (detailLocalTabRect().contains(event->pos())
            || detailConfigTabRect().contains(event->pos())
            || detailScriptLogTabRect().contains(event->pos())
            // || detailSearchTabRect().contains(event->pos()) // wjy: 查找页隐藏期间同步关闭不可见区域的手型命中，后续启用时取消注释。
        );
    const bool scriptButtonHovered = !m_detailPanelCollapsed
        && !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && m_deviceDetailTab == DeviceDetailTab::ScriptLog
        && (scriptTerminalExecuteButtonRect().contains(event->position())
            || scriptTerminalStopButtonRect().contains(event->position()));
    if (deviceListScrollbarHovered || settingsScrollbarHovered) {
        setCursor(Qt::OpenHandCursor); // wjy: 两套滚动条未按下时统一用张开手型提示滑块和轨道都可操作。
    } else if (m_desktopHovered
        || detailPanelButtonHovered
        || titlebarButtonHovered
        || wakeButtonHovered
        || m_hoveredBottomAction != BottomAction::None
        || settingsSwitchHovered
        || settingsTabHovered
        || detailTabHovered
        || scriptButtonHovered) {
        setCursor(Qt::PointingHandCursor);
    } else {
        unsetCursor();
    }
    QFrame::mouseMoveEvent(event);
}

void DeviceGrid::mouseDoubleClickEvent(QMouseEvent* event)
{
// =====wjy====
    if (event->button() == Qt::LeftButton && m_deviceGroupExpanded) { // wjy: 紧凑窗口也支持双击设备远控；分组双击仍保留原地重命名。
        const QVector<DeviceListRow> rows = visibleDeviceRows(m_deviceStatuses); // wjy: 双击命中使用在线优先自然排序行，设备位置始终跟随 UI 绘制变化。
        const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 双击只识别当前可见视口内的列表行。
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const DeviceListRow& row = rows.at(rowIndex);
            const QRect rowRect =
                scrolledVisibleDeviceRowRect(
                    rowIndex,
                    m_deviceListScrollOffset);

            const QRect hitRect =
                rowRect.intersected(deviceListClip);

            if (!hitRect.contains(event->pos())) {
                continue;
            }

            if (row.type == DeviceListRow::Type::Device) {
                const int deviceIndex = row.deviceIndex;
                if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
                    continue;
                }

                QSet<int> launchIndexes = m_doubleClickRemoteDeviceIndexes; // wjy: 先恢复第一次按下时的多选快照，普通双击不会被首个单击释放收敛成一台。
                launchIndexes.unite(m_selectedDeviceIndexes); // wjy: 同时合并首个释放事件产生的 Shift/Ctrl 新选择，按住修饰键直接双击也能覆盖完整目标。
                if (!launchIndexes.contains(deviceIndex)) {
                    launchIndexes.clear();
                    launchIndexes.insert(deviceIndex); // wjy: 双击未选设备时只控制命中的这一台，不继承其它设备的旧选择。
                }
                m_selectedDeviceIndexes = launchIndexes;
                m_selectionAnchorDeviceIndex = deviceIndex;
                update(deviceListViewportRect(m_deviceGroupExpanded)); // wjy: 启动批量远控前恢复多选高亮，让界面显示的目标和实际打开窗口一致。
                launchSelectedRemoteDesktopWindows(); // wjy: 单选双击打开一台；Shift/Ctrl 已形成多选时复用现有批量授权和开窗流程。
                m_doubleClickRemoteDeviceIndexes.clear();
                event->accept();
                return;
            }

            if (row.type != DeviceListRow::Type::Group) {
                continue;
            }

            beginDeviceGroupRename(row.groupIndex);
            event->accept();
            return;
        }
    }
// ===end====

    QFrame::mouseDoubleClickEvent(event);
}

void DeviceGrid::wheelEvent(QWheelEvent* event)
{
    syncResponsiveLayoutState();
// =====wjy====
    if (m_scriptOutputVisible
        && !m_detailPanelCollapsed
        && !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && m_deviceDetailTab == DeviceDetailTab::ScriptLog
        && scriptTerminalOutputRect().contains(event->position())) {
        QFont terminalFont(QStringLiteral("Consolas"));
        terminalFont.setPixelSize(12);
        const QFontMetrics metrics(terminalFont);
        const QRectF content = scriptTerminalOutputRect();
        if (m_scriptOutputDirty) {
            m_scriptOutputText = stripTerminalControlSequences(readScriptOutputFileTail(m_scriptOutputFilePath));
            m_scriptOutputDirty = false;
        }
        const int maxOffset = maxTerminalScrollOffset(m_scriptOutputText, metrics, qRound(content.height()));
        const int wheelSteps = qMax(1, qAbs(event->angleDelta().y()) / 120);
        const int lineStep = wheelSteps * 3;
        if (event->angleDelta().y() > 0) {
            m_scriptOutputScrollOffset = qBound(0, m_scriptOutputScrollOffset + lineStep, maxOffset);
        } else {
            m_scriptOutputScrollOffset = qBound(0, m_scriptOutputScrollOffset - lineStep, maxOffset);
        }
        m_scriptOutputAutoScroll = (m_scriptOutputScrollOffset == 0);
        saveCurrentScriptUiState(); // wjy: 每台设备保留自己的终端滚动位置，切到别的设备不会改变当前设备的查看位置。
        update(scriptTerminalPanelRect().toAlignedRect().adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    const int maxSettingsOffset = m_settingsTab == SettingsTab::Keyboard
        ? keyboardShortcutMaxScrollOffset()
        : maxSettingsScrollOffset(m_settingsLocalInfoExpanded, m_settingsAddDeviceExpanded);
    if (!m_detailPanelCollapsed
        && m_settingsSelected
        && (m_settingsTab == SettingsTab::General || m_settingsTab == SettingsTab::Keyboard)
        && maxSettingsOffset > 0
        && settingsScrollViewportRect().contains(event->position().toPoint())) {
        const int wheelDelta = !event->pixelDelta().isNull()
            ? event->pixelDelta().y()
            : event->angleDelta().y() / 3;
        if (wheelDelta != 0) {
            if (m_settingsScrollbarAnimation) m_settingsScrollbarAnimation->stop(); // wjy: 滚轮输入优先级高于轨道点击特效，避免两种偏移同时生效。
            const int oldOffset = m_settingsScrollOffset;
            m_settingsScrollOffset = qBound(0, m_settingsScrollOffset - wheelDelta, maxSettingsOffset);
            if (m_settingsScrollOffset != oldOffset) {
                updateSettingsControls();
                updateAddDeviceControls();
                updateLocalInfoControls();
                update();
                event->accept();
                return;
            }
        }
    }

    const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 只有鼠标位于“我的设备”列表视口内，滚轮才控制设备列表。
    const int maxScrollOffset = maxDeviceListScrollOffset(); // wjy: 为 0 表示内容没有超过视口，不需要滚动。
    if (m_deviceGroupExpanded && maxScrollOffset > 0 && deviceListClip.contains(event->position().toPoint())) {
        const int wheelDelta = !event->pixelDelta().isNull()
            ? event->pixelDelta().y()
            : event->angleDelta().y() / 3; // wjy: 普通鼠标一格通常是 120，除以 3 后约等于滚动一行 40 像素。
        if (wheelDelta != 0) {
            if (m_deviceListScrollbarAnimation) m_deviceListScrollbarAnimation->stop(); // wjy: 用户滚轮操作时停止设备滑块动画，列表立即按滚轮方向响应。
            const int oldOffset = m_deviceListScrollOffset;
            m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset - wheelDelta, maxScrollOffset); // wjy: 向下滚时偏移增大，向上滚时偏移减小，并限制在有效范围内。
            if (m_deviceListScrollOffset != oldOffset) {
                finishDeviceGroupRename(true); // wjy: 滚动时提交并关闭分组输入框，避免输入框停留在旧位置。
                finishDeviceRename(true);
                update();
                event->accept();
                return;
            }
        }
    }
// ===end====

    QFrame::wheelEvent(event);
}

void DeviceGrid::keyPressEvent(QKeyEvent* event)
{
// =====wjy====
    if (matchesShortcut(event, platform::AppSettings::deviceShortcutDelete())
        && !event->isAutoRepeat()
        && !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && m_selectedDeviceIndex >= 0
        && m_selectedDeviceIndex < g_devices.size()) {
        deleteCurrentDevice(); // wjy: 主界面设备详情处于选中状态时按用户设置的快捷键删除当前设备；长按不会连续误删多台。
        event->accept();
        return;
    }
    if (matchesShortcut(event, platform::AppSettings::remoteShortcutFullscreen())) {
        triggerShortcutAction(0);
        event->accept();
        return;
    }
    if (matchesShortcut(event, platform::AppSettings::remoteShortcutTile())) {
        triggerShortcutAction(1);
        event->accept();
        return;
    }
    if (matchesShortcut(event, platform::AppSettings::remoteShortcutCloseAll())) {
        triggerShortcutAction(3);
        event->accept();
        return;
    }
    if (matchesShortcut(event, platform::AppSettings::remoteShortcutCloseTopmost())) {
        triggerShortcutAction(2);
        event->accept();
        return;
    }
// ===end====
    QFrame::keyPressEvent(event);
}

bool DeviceGrid::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#if defined(Q_OS_WIN)
    MSG* nativeMessage = reinterpret_cast<MSG*>(message);
    if (nativeMessage && nativeMessage->message == WM_HOTKEY) {
        const int shortcutIndex = shortcutIndexForGlobalShortcutId(static_cast<int>(nativeMessage->wParam));
        if (shortcutIndex >= 0) {
            triggerShortcutAction(shortcutIndex);
            if (result) {
                *result = 0;
            }
            return true;
        }
    }
#endif
    return QFrame::nativeEvent(eventType, message, result);
}

void DeviceGrid::resizeEvent(QResizeEvent* event)
{
    QFrame::resizeEvent(event);
    syncResponsiveLayoutState();
    updateAddDeviceControls();
    updateLocalInfoControls();
    updateSettingsControls();
    updateScriptFileEditorControls();
}

// =====wjy====
void DeviceGrid::showEvent(QShowEvent* event)
{
    QFrame::showEvent(event);
    if (m_screenEdgeDock == ScreenEdgeDock::None || !m_screenEdgeAutoHidden) {
        return;
    }
    QTimer::singleShot(0, this, [this] {
        if (m_screenEdgeDock != ScreenEdgeDock::None && m_screenEdgeAutoHidden && !m_shuttingDown) {
            setScreenEdgeAutoHidden(false); // wjy: 从托盘或最小化恢复时沿原停靠方向完整滑出，避免窗口仍停留在屏幕外。
        }
    });
}
// ===end====

void DeviceGrid::leaveEvent(QEvent* event)
{
    setDesktopHoverActive(false);
    clearBottomActionHover();
    if (!m_draggingDeviceListScrollbar && !m_draggingSettingsScrollbar) {
        unsetCursor(); // wjy: 任一滚动条处于隐式鼠标抓取时都保持闭合手型，直到真正松开左键。
    }
    QFrame::leaveEvent(event);
}

//鼠标事件
void DeviceGrid::mouseReleaseEvent(QMouseEvent* event)
{
    syncResponsiveLayoutState();
    if (event->button() == Qt::LeftButton) {
        if (m_resizingWindow) {
            finishWindowResize();
            event->accept();
            return;
        }
        // =====wjy====
        if (m_draggingSettingsScrollbar) {
            m_draggingSettingsScrollbar = false; // wjy: 左键松开结束常规页滚动条独占手势，恢复卡片和按钮的普通点击逻辑。
            m_settingsScrollbarGrabOffsetY = 0;
            unsetCursor();
            update(); // wjy: 保留最终设置页偏移，仅恢复滚动条普通悬停视觉。
            event->accept();
            return;
        }
        // ===end====
        // =====wjy====
        if (m_draggingDeviceListScrollbar) {
            m_draggingDeviceListScrollbar = false; // wjy: 左键松开结束滚动条独占手势，后续点击恢复设备和分组交互。
            m_deviceListScrollbarGrabOffsetY = 0;
            unsetCursor();
            update(deviceListViewportRect(true)); // wjy: 恢复普通颜色的滑块并保留最终列表偏移。
            event->accept();
            return;
        }
        // ===end====
        // =====wjy====
        const bool finishedWindowDrag = m_draggingWindow;
        m_draggingWindow = false;
        if (finishedWindowDrag) {
            updateScreenEdgeAutoHideAfterWindowDrag(event->globalPosition().toPoint()); // wjy: 只在真实标题栏拖窗结束时判断顶部、左侧或右侧吸附，普通控件点击不会启用自动隐藏。
            event->accept();
            return;
        }
        // ===end====

// =====wjy====
        if (m_draggingGroup) {
            const int sourceGroupIndex = m_draggingGroupIndex;
            const bool dropOnDissolveZone =
                groupDragGhostRect(m_groupDragCurrentPos, size()).intersects(topDragDropZoneRect());
            if (dropOnDissolveZone) {
                if (sourceGroupIndex >= 0 && sourceGroupIndex < g_deviceGroupNames.size()) {
                    const QString dissolvedGroupName = g_deviceGroupNames.at(sourceGroupIndex).trimmed();
                    const QString dissolvedGroupId = g_deviceGroupIds.at(sourceGroupIndex);
                    if (!dissolvedGroupName.isEmpty()) {
                        g_deviceCatalog.removeGroup(dissolvedGroupId); // wjy: 解散分组由目录完成，组内设备按稳定 groupId 回到根部。
                    }
                    if (m_renamingDeviceGroupIndex == sourceGroupIndex) {
                        m_deviceGroupNameEdit->hide();
                        m_renamingDeviceGroupIndex = -1;
                    } else if (m_renamingDeviceGroupIndex > sourceGroupIndex) {
                        --m_renamingDeviceGroupIndex;
                    }
                    m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset, maxDeviceListScrollOffset());
                    saveDevices();
                    writeDeviceGridStartupLog(QStringLiteral("[wjy-group-drag] dissolved group=%1 index=%2")
                        .arg(dissolvedGroupName)
                        .arg(sourceGroupIndex));
                }

                m_groupDragCandidateActive = false;
                m_draggingGroup = false;
                m_draggingGroupIndex = -1;
                unsetCursor();
                update();
                event->accept();
                return;
            }

            int targetInsertionIndex = g_deviceGroupNames.size();
            if (m_deviceGroupExpanded) {
                const QVector<DeviceListRow> rows = visibleDeviceRows(m_deviceStatuses); // wjy: 分组拖拽落点按当前状态排序后的真实行位置计算。
                const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded);
                targetInsertionIndex = 0;
                for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
                    const DeviceListRow& row = rows.at(rowIndex);
                    if (row.type != DeviceListRow::Type::Group
                        || row.groupIndex < 0
                        || row.groupIndex >= g_deviceGroupNames.size()) {
                        continue;
                    }

                    const QRect rowRect = scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset);
                    if (!rowRect.intersects(deviceListClip)) {
                        continue;
                    }

                    if (event->pos().y() < rowRect.center().y()) {
                        targetInsertionIndex = row.groupIndex;
                        break;
                    }
                    targetInsertionIndex = row.groupIndex + 1;
                }
            }

            bool groupOrderChanged = false;
            if (sourceGroupIndex >= 0 && sourceGroupIndex < g_deviceGroupNames.size()) {
                int insertIndex = qBound(0, targetInsertionIndex, g_deviceGroupNames.size());
                if (insertIndex > sourceGroupIndex) {
                    --insertIndex;
                }

                if (insertIndex != sourceGroupIndex) {
                    const QString movedGroupId = g_deviceGroupIds.at(sourceGroupIndex);
                    const QString movedGroupName = g_deviceGroupNames.at(sourceGroupIndex);
                    groupOrderChanged = g_deviceCatalog.moveGroup(movedGroupId, insertIndex); // wjy: 目录把名称、稳定 ID 和展开状态作为一个实体重排，UI 只保留动画下标。
                    if (groupOrderChanged) {
                        saveDevices();
                    }
                    writeDeviceGridStartupLog(QStringLiteral("[wjy-group-drag] moved group=%1 from=%2 to=%3")
                        .arg(movedGroupName)
                        .arg(sourceGroupIndex)
                        .arg(insertIndex));
                }
            }

            m_groupDragCandidateActive = false;
            m_draggingGroup = false;
            m_draggingGroupIndex = -1;
            unsetCursor();
            if (groupOrderChanged) {
                m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset, maxDeviceListScrollOffset());
            }
            update();
            event->accept();
            return;
        }
        if (m_groupDragCandidateActive) {
            m_groupDragCandidateActive = false;
            m_draggingGroupIndex = -1;
        }

        if (m_draggingDevice) { // wjy: 如果本次鼠标操作已经进入设备拖拽状态，松开时只输出落点日志。
            QString targetType = QStringLiteral("none"); // wjy: 默认表示没有落到可识别的分组目标。
            QString targetGroup;
            const bool deleteUngroupedDevices = deviceIndexesAreAllUngrouped(m_draggingDeviceIndexes); // wjy: 只有整个拖拽快照都是根部设备时，顶部危险区域才具备删除含义。
            if (m_deviceGroupExpanded) {
                const QVector<DeviceListRow> rows = visibleDeviceRows(m_deviceStatuses); // wjy: 设备拖拽落点按在线优先自然排序后的可见行识别，与绘制顺序一致。
                const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 拖拽落点只在当前可见滚动视口内识别。
                const QRect ghostRect = deviceDragGhostRect(m_deviceDragCurrentPos, size());
                if (ghostRect.intersects(topDragDropZoneRect())) {
                    targetType = deleteUngroupedDevices
                        ? QStringLiteral("delete")
                        : QStringLiteral("rootBlank"); // wjy: 根部设备拖进顶部区域执行删除；分组设备仍执行原来的移出分组。
                } else if (ghostRect.intersects(rootDeviceDropZoneRect())) {
                    targetType = QStringLiteral("rootBlank"); // wjy: 移出分组按受限后的拖拽虚影判断，鼠标跑到标题栏上方时仍以可见方框位置为准。
                } else {
                    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
                        const DeviceListRow& row = rows.at(rowIndex);
                        const QRect rowRect =
                            scrolledVisibleDeviceRowRect(
                                rowIndex,
                                m_deviceListScrollOffset);

                        const QRect hitRect =
                            rowRect.intersected(deviceListClip);

                        if (!hitRect.contains(event->pos())) {
                            continue;
                        }
                        if (row.type == DeviceListRow::Type::Group) {
                            targetType = QStringLiteral("group"); // wjy: 分组标题只代表该分组本身，不再兼作移出分组区域。
                            targetGroup = g_deviceGroupNames.at(row.groupIndex);
                            break;
                        }
                        if (row.type == DeviceListRow::Type::Device && row.groupIndex >= 0 && row.groupIndex < g_deviceGroupNames.size()) {
                            targetType = QStringLiteral("group"); // wjy: 鼠标松开在分组内的设备行上，也视为拖入这个设备所属的分组。
                            targetGroup = g_deviceGroupNames.at(row.groupIndex);
                            break;
                        }
                        if (row.type == DeviceListRow::Type::Device && row.groupIndex < 0) {
                            targetType = QStringLiteral("rootBlank"); // wjy: 鼠标松开在无分组设备行上，也视为拖回“我的设备”根部。
                            break;
                        }
                    }
                }
                const QRect blankHitRect =
                    scrolledDeviceGroupReservedBlankRect(
                        m_deviceListScrollOffset)
                        .intersected(deviceListClip);

                if (targetType == QStringLiteral("none")
                    && blankHitRect.contains(event->pos())) {

                    targetType = QStringLiteral("rootBlank");
                }
            }
            const QString deviceName = (m_draggingDeviceIndex >= 0 && m_draggingDeviceIndex < g_devices.size())
                ? deviceDisplayName(g_devices.at(m_draggingDeviceIndex))
                : QString(); // wjy: 防御性获取真实设备名，避免拖拽过程中设备列表变化导致越界。
            writeDeviceGridStartupLog(QStringLiteral("[wjy-drag] drop deviceIndex=%1 device=%2 targetType=%3 targetGroup=%4")
                .arg(m_draggingDeviceIndex)
                .arg(deviceName)
                .arg(targetType)
                .arg(targetGroup)); // wjy: 拖拽落点日志写入文件，不再输出到调试控制台。
            //在这里添加写入group的代码
            // =====wjy====
            const QString normalizedTargetGroup =
                targetGroup.trimmed();
            QString targetGroupId;
            if (targetType == QStringLiteral("group")) {
                const int targetGroupIndex = g_deviceCatalog.groupIndexForName(normalizedTargetGroup);
                if (targetGroupIndex >= 0) {
                    targetGroupId = g_deviceGroupIds.at(targetGroupIndex); // wjy: 拖拽目标先由名称解析为稳定 groupId，真正写入不依赖名称字符串。
                }
            }

            QVector<QString> draggedDeviceIds;
            QVector<int> draggedDeviceIndexes = m_draggingDeviceIndexes.values();
            std::sort(draggedDeviceIndexes.begin(), draggedDeviceIndexes.end()); // wjy: 多选删除按当前显示顺序保存稳定 ID，行为可预测且不依赖 QSet 的无序遍历。
            for (const int deviceIndex : draggedDeviceIndexes) {
                if (deviceIndex >= 0 && deviceIndex < g_devices.size()) {
                    draggedDeviceIds.append(g_devices.at(deviceIndex).id); // wjy: 拖拽快照中的展示下标只用于提取稳定 ID，排序变化不会串设备。
                }
            }
            if (targetType == QStringLiteral("delete")) {
                for (const QString& deviceId : draggedDeviceIds) {
                    const int currentDeviceIndex = g_deviceCatalog.deviceIndexForId(deviceId); // wjy: 每删除一台后数组下标都会变化，因此逐次通过稳定 ID 重新定位真实设备。
                    if (currentDeviceIndex >= 0) {
                        deleteDeviceForIndex(currentDeviceIndex); // wjy: 拖拽删除完整复用右键菜单入口，设备保存、状态缓存和当前选择清理逻辑保持一致。
                    }
                }
                writeDeviceGridStartupLog(QStringLiteral("[wjy-drag] deleted ungrouped devices count=%1")
                    .arg(draggedDeviceIds.size())); // wjy: 记录顶部拖拽删除数量，便于确认多选操作是否完整执行。
            }
            const bool deviceGroupChanged =
                (targetType == QStringLiteral("group") && !targetGroupId.isEmpty())
                    ? g_deviceCatalog.assignDevicesToGroup(draggedDeviceIds, targetGroupId)
                    : (targetType == QStringLiteral("rootBlank")
                        ? g_deviceCatalog.assignDevicesToGroup(draggedDeviceIds, {})
                        : false); // wjy: 归组和回根都复用目录批量操作，保持 group/groupId 两个兼容字段同步。
            //保存逻辑
            if (deviceGroupChanged) {
                saveDevices();
                writeDeviceGridStartupLog(QStringLiteral("[wjy-drag] save group deviceIndex=%1 device=%2 group=%3")
                                         .arg(m_draggingDeviceIndex)
                                         .arg(deviceName)
                                         .arg(m_draggingDeviceIndex >= 0 && m_draggingDeviceIndex < g_devices.size()
                                                  ? g_devices.at(m_draggingDeviceIndex).group
                                                  : QString())); // wjy: 分组保存结果写入文件，便于和启动/关闭日志统一查看。
            }
            // ===end====
            m_deviceDragCandidateActive = false;
            m_draggingDevice = false;
            m_draggingDeviceIndex = -1;
            m_draggingDeviceIndexes.clear();

            unsetCursor();
            update();
            event->accept();
            return;
        }
        if (m_deviceDragCandidateActive) {
            m_deviceDragCandidateActive = false;
            m_draggingDeviceIndex = -1;
            m_draggingDeviceIndexes.clear();
        }
// ===end====

        if (detailPanelToggleButtonRect(m_detailPanelCollapsed).contains(event->pos())) {
            finishDeviceGroupRename(true);
            finishDeviceRename(true);
            setDetailPanelCollapsed(!m_detailPanelCollapsed); // wjy: << 收起右侧详情并缩窄窗口，>> 恢复详情和收起前宽度。
            event->accept();
            return;
        }

        // =====wjy====
        if (!m_detailPanelCollapsed && m_updateAvailable && titlebarUpdateRect().contains(event->pos())) {
            if (!m_updatePreparing && !m_rollbackPreparing && !m_publishPreparing) {
                m_updatePreparing = true; // wjy: 三类版本事务互斥，后台升级期间不能同时发布或回撤同一套运行文件。
                update(titlebarUpdateRect().adjusted(-2, -2, 2, 2));
                QPointer<DeviceGrid> self(this);
                runBackgroundTask([self] {
                    QString error;
                    const bool prepared = platform::UpdateService::instance().applyRemoteUpdate(&error); // wjy: 共享版本读取、整包复制和校验全部在可取消后台线程执行，主窗口不再等待 SMB。
                    if (!self) {
                        return;
                    }
                    QMetaObject::invokeMethod(self, [self, prepared, error] {
                        if (!self || prepared) {
                            return; // wjy: 成功时更新服务会通知主线程有序退出；窗口已销毁时也不能访问旧 UI。
                        }
                        DeviceGrid* grid = self.data();
                        grid->m_updatePreparing = false; // wjy: 只有真实准备失败才恢复按钮，周期更新信号不会在任务中途解除互斥。
                        grid->update(titlebarUpdateRect().adjusted(-2, -2, 2, 2));
                        QMessageBox::warning(grid, QString(), QString::fromUtf8("更新准备失败：%1").arg(error));
                    }, Qt::QueuedConnection);
                });
            }
            event->accept();
            return;
        }
        // ===end====

        if (!m_detailPanelCollapsed && titlebarSettingsRect().contains(event->pos())) {
            finishDeviceGroupRename(true);
            finishDeviceRename(true);
            m_settingsSelected = true;
            m_remoteAssistSelected = false;
            m_localInfoSelected = false;
            m_detailAnimationTimer->stop();
            // =====wjy====
            m_settingsGearRotation = 0.0;
            m_settingsGearClock.restart();
            if (m_settingsGearTimer && !m_settingsGearTimer->isActive()) {
                m_settingsGearTimer->start(); // wjy: 每次点击设置齿轮都从 0 转到 180 度。
            }
            // ===end====
            if (m_settingsLocalInfoExpanded) {
                refreshLocalDeviceInfo();
            }
            setDesktopHoverActive(false);
            clearBottomActionHover();
            updateAddDeviceControls();
            updateLocalInfoControls();
            updateSettingsControls();
            updateScriptFileEditorControls(); // wjy: 进入设置页立即隐藏脚本、配置和查找控件，并同步停止本机页三类资源定时器。
            if (m_settingsTab == SettingsTab::General) {
                refreshRollbackVersions(); // wjy: 设置页立即显示，历史版本由后台读取；30 秒内重复进入直接复用缓存。
            }
            update();
            event->accept();
            return;
        }

        if (!m_detailPanelCollapsed && refreshRect().contains(event->pos())) {
            refreshDeviceStatuses();
            event->accept();
            return;
        }

        if (minimizeRect().contains(event->pos())) {
            window()->showMinimized();
            event->accept();
            return;
        }

        if (closeRect().contains(event->pos())) {
            window()->close();
            event->accept();
            return;
        }

        if (m_deviceGroupExpanded) {
            const QVector<DeviceListRow> rows = visibleDeviceRows(m_deviceStatuses); // wjy: 普通点击使用在线优先自然排序后的可见行，保证命中位置与绘制一致。
            const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 普通点击也只命中当前滚动视口内的列表行。
// =====wjy====
            for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) { // wjy: 一次遍历当前 UI 的每一行，设备行和分组行按同一套坐标命中。
                const DeviceListRow& row = rows.at(rowIndex);
                const QRect rowRect =
                    scrolledVisibleDeviceRowRect(
                        rowIndex,
                        m_deviceListScrollOffset);

                const QRect hitRect =
                    rowRect.intersected(deviceListClip);

                if (!hitRect.contains(event->pos())) {
                    continue;
                }

                if (row.type == DeviceListRow::Type::Group) {
                    const int groupIndex = row.groupIndex; // wjy: 点击分组行时，使用真实分组下标切换展开状态。
                    if (m_renamingDeviceGroupIndex == groupIndex) {
                        event->accept(); // wjy: 双击进入重命名后的释放事件只关闭本次点击，不再切换分组箭头。
                        return;
                    }
                    const QString groupId = g_deviceGroupIds.at(groupIndex);
                    g_deviceCatalog.setGroupExpanded(groupId, !g_deviceGroupExpandedStates.at(groupIndex)); // wjy: 点击分组行按稳定 groupId 切换展开状态，数组下标只负责当前命中行。
                    saveDevices(); // wjy: 保存分组展开状态，重启后箭头方向保持一致。
                    update();
                    event->accept();
                    return;
                }

                if (row.type == DeviceListRow::Type::Device) {
                    const int deviceIndex = row.deviceIndex;

                    if (deviceIndex < 0
                        || deviceIndex >= g_devices.size()) {
                        continue;
                    }
                    if (m_renamingDeviceIndex == deviceIndex) {
                        event->accept();
                        return;
                    }

                    const bool shiftPressed =
                        event->modifiers().testFlag(
                            Qt::ShiftModifier);
                    const bool ctrlPressed =
                        event->modifiers().testFlag(
                            Qt::ControlModifier); // wjy: Ctrl+左键使用文件管理器式的单项加入/取消多选。
                    int detailDeviceIndex = deviceIndex; // wjy: 默认右侧详情跟随本次点击的设备，Ctrl 取消当前项时会重新选择一个仍可见的已选设备。

                    if (shiftPressed
                        && m_selectionAnchorDeviceIndex >= 0) {

                        // 找到上一次普通点击的设备，
                        // 当前在可见列表中的视觉行号。
                        int anchorRowIndex = -1;

                        for (int i = 0; i < rows.size(); ++i) {
                            const DeviceListRow& candidateRow =
                                rows.at(i);

                            if (candidateRow.type
                                    == DeviceListRow::Type::Device
                                && candidateRow.deviceIndex
                                       == m_selectionAnchorDeviceIndex) {

                                anchorRowIndex = i;
                                break;
                            }
                        }

                        if (anchorRowIndex >= 0) {
                            // 找到起点后，选中起点和当前点击位置之间
                            // 的所有设备行，分组行自动跳过。
                            const int firstRow =
                                qMin(anchorRowIndex, rowIndex);

                            const int lastRow =
                                qMax(anchorRowIndex, rowIndex);

                            m_selectedDeviceIndexes.clear();

                            for (int i = firstRow;
                                 i <= lastRow;
                                 ++i) {

                                const DeviceListRow& rangeRow =
                                    rows.at(i);

                                if (rangeRow.type
                                    != DeviceListRow::Type::Device) {
                                    continue;
                                }

                                const int rangeDeviceIndex =
                                    rangeRow.deviceIndex;

                                if (rangeDeviceIndex < 0
                                    || rangeDeviceIndex
                                           >= g_devices.size()) {
                                    continue;
                                }

                                m_selectedDeviceIndexes.insert(
                                    rangeDeviceIndex);
                            }
                        } else {
                            // 锚点设备目前不可见，例如所在分组已经收起。
                            // 此时退化为普通单选。
                            m_selectedDeviceIndexes.clear();
                            m_selectedDeviceIndexes.insert(
                                deviceIndex);

                            m_selectionAnchorDeviceIndex =
                                deviceIndex;
                        }
                    } else if (ctrlPressed) {
                        if (m_selectedDeviceIndexes.contains(deviceIndex)
                            && m_selectedDeviceIndexes.size() > 1) {
                            m_selectedDeviceIndexes.remove(deviceIndex); // wjy: Ctrl 点已选设备时从多选集合里移除，行为对齐文件管理器取消选择。

                            bool foundVisibleSelectedDevice = false; // wjy: 记录取消当前设备后，是否还能找到另一台可见已选设备接管详情页。
                            for (const DeviceListRow& visibleRow : rows) {
                                if (visibleRow.type == DeviceListRow::Type::Device
                                    && visibleRow.deviceIndex >= 0
                                    && visibleRow.deviceIndex < g_devices.size()
                                    && m_selectedDeviceIndexes.contains(visibleRow.deviceIndex)) {
                                    detailDeviceIndex = visibleRow.deviceIndex; // wjy: 被取消的是详情主设备时，切到第一台仍可见的已选设备。
                                    foundVisibleSelectedDevice = true;
                                    break;
                                }
                            }
                            if (!foundVisibleSelectedDevice) {
                                m_selectedDeviceIndexes.insert(deviceIndex); // wjy: 如果历史残留状态导致没有可见已选设备，就保留当前设备，避免详情指向未选中项。
                                detailDeviceIndex = deviceIndex;
                            }
                        } else {
                            m_selectedDeviceIndexes.insert(deviceIndex); // wjy: Ctrl 点未选设备时加入多选；如果只剩这一台，则保持至少一台被选中。
                        }

                        m_selectionAnchorDeviceIndex = deviceIndex; // wjy: Ctrl 点击也作为下一次 Shift 范围选择的新起点，贴近文件管理器操作习惯。
                    } else {
                        // 没有按 Shift：普通单选，
                        // 同时把这台设备设为下一次 Shift 的起点。
                        m_selectedDeviceIndexes.clear();
                        m_selectedDeviceIndexes.insert(
                            deviceIndex);

                        m_selectionAnchorDeviceIndex =
                            deviceIndex;
                    }

                    m_settingsSelected = false;
                    m_remoteAssistSelected = false;
                    m_localInfoSelected = false;

                    setDesktopHoverActive(false);
                    clearBottomActionHover();

                    updateAddDeviceControls();
                    updateLocalInfoControls();
                    updateSettingsControls();

                    // 右侧详情跟随当前点击或 Ctrl 取消后仍可见的已选设备。
                    startDeviceSwitchAnimation(
                        detailDeviceIndex,
                        deviceDisplayName(
                            g_devices.at(detailDeviceIndex)));

                    event->accept();
                    return;
                }
            }
// ===end====
        }

        if (!m_detailPanelCollapsed && m_settingsSelected) {
            const SettingsLayoutSnapshot settingsLayout = settingsLayoutSnapshot(
                m_settingsLocalInfoExpanded,
                m_settingsAddDeviceExpanded,
                m_settingsScrollOffset,
                m_settingsTab == SettingsTab::Keyboard); // wjy: 设置页点击命中与手绘/子控件布局共享同一滚动快照。
            if (settingsGeneralTabRect().contains(event->pos())
                || settingsKeyboardTabRect().contains(event->pos())
                || settingsRemoteControlTabRect().contains(event->pos())) {
                m_settingsTab = settingsRemoteControlTabRect().contains(event->pos())
                    ? SettingsTab::RemoteControl
                    : (settingsKeyboardTabRect().contains(event->pos())
                        ? SettingsTab::Keyboard
                        : SettingsTab::General); // wjy: 三个设置标签互斥切换，远控画质页不会误落入常规页点击处理。
                m_settingsScrollOffset = 0;
                if (m_settingsTab == SettingsTab::General) {
                    refreshRollbackVersions(); // wjy: 切回常规页时按缓存期限请求后台刷新，不在页签点击事件中同步遍历共享目录。
                }
                updateSettingsControls();
                updateAddDeviceControls();
                updateLocalInfoControls();
                update();
                event->accept();
                return;
            }
            if (m_settingsTab != SettingsTab::General) {
                event->accept();
                return;
            }
            if (settingsLayout.containsPoint(settingsLocalInfoHeaderRect(), event->pos())) {
                m_settingsLocalInfoExpanded = !m_settingsLocalInfoExpanded;
                if (m_settingsLocalInfoExpanded) {
                    refreshLocalDeviceInfo();
                }
                m_settingsScrollOffset = qBound(0, m_settingsScrollOffset, maxSettingsScrollOffset(m_settingsLocalInfoExpanded, m_settingsAddDeviceExpanded));
                updateSettingsControls();
                updateAddDeviceControls();
                updateLocalInfoControls();
                update();
                event->accept();
                return;
            }
            if (settingsLayout.containsPoint(settingsAddDeviceHeaderRect(m_settingsLocalInfoExpanded), event->pos())) {
                m_settingsAddDeviceExpanded = !m_settingsAddDeviceExpanded;
                m_settingsScrollOffset = qBound(0, m_settingsScrollOffset, maxSettingsScrollOffset(m_settingsLocalInfoExpanded, m_settingsAddDeviceExpanded));
                updateSettingsControls();
                updateAddDeviceControls();
                updateLocalInfoControls();
                update();
                event->accept();
                return;
            }
            if (settingsLayout.containsPoint(settingsAutoRunSwitchRect(), event->pos())) {
                platform::StartupManager::setEnabled(!m_autoRunEnabled);
                m_autoRunEnabled = platform::StartupManager::isEnabled();
                update();
                event->accept();
                return;
            }
            // =====wjy====
            if (settingsLayout.containsPoint(settingsRollbackButtonRect(), event->pos())) {
                const QString targetVersion = m_rollbackVersionCombo
                    ? m_rollbackVersionCombo->currentData().toString()
                    : QString();
                if (targetVersion.isEmpty() || m_rollbackPreparing || m_updatePreparing || m_publishPreparing) {
                    event->accept();
                    return; // wjy: 占位项、已有回撤、升级或发布事务都直接拦截，禁止并发修改同一套版本文件。
                }

                QMessageBox confirmation(
                    QMessageBox::Warning,
                    QString::fromUtf8("确认回撤版本"),
                    QString::fromUtf8("将从当前版本 v%1 回撤到 v%2。\n程序会自动关闭并在回撤完成后重新启动。\n旧版本可能无法识别新版本保存的部分设置，是否继续？")
                        .arg(platform::UpdateService::displayVersion(), targetVersion),
                    QMessageBox::NoButton,
                    this);
                QPushButton* rollbackButton = confirmation.addButton(QString::fromUtf8("回撤并重启"), QMessageBox::AcceptRole);
                confirmation.addButton(QString::fromUtf8("取消"), QMessageBox::RejectRole);
                confirmation.exec();
                if (confirmation.clickedButton() != rollbackButton) {
                    event->accept();
                    return; // wjy: 用户取消后不暂存文件、不启动更新器，也不改变当前安装和下拉框选择。
                }

                m_rollbackPreparing = true;
                updateSettingsControls();
                update(); // wjy: 确认后立即禁用下拉框并显示“准备中”，后台复制期间界面仍可继续响应。
                QPointer<DeviceGrid> self(this);
                runBackgroundTask([self, targetVersion] {
                    QString error;
                    const bool prepared = platform::UpdateService::instance().applyVersionRollback(targetVersion, &error); // wjy: 历史版本验证、暂存和更新器准备全部离开 UI 线程。
                    if (!self) {
                        return;
                    }
                    QMetaObject::invokeMethod(self, [self, prepared, error] {
                        if (!self || prepared) {
                            return; // wjy: 成功后由统一更新退出流程接管，当前设置页无需再改状态。
                        }
                        DeviceGrid* grid = self.data();
                        grid->m_rollbackPreparing = false;
                        grid->updateSettingsControls();
                        grid->update();
                        grid->refreshRollbackVersions(true); // wjy: 失败后后台重新验证目录，目标被删除或损坏会从下拉框中移除。
                        QMessageBox::warning(grid, QString(), QString::fromUtf8("版本回撤准备失败：%1").arg(error));
                    }, Qt::QueuedConnection);
                });
                event->accept();
                return;
            }
            // ===end====
            // =====wjy====
            if (platform::UpdateService::canPublishCurrentBuild()
                && settingsLayout.containsPoint(settingsPublishUpdateButtonRect(), event->pos())) { // wjy: 普通运行包即使点击原按钮坐标也不会触发发布。
                if (m_publishPreparing || m_updatePreparing || m_rollbackPreparing) {
                    event->accept();
                    return; // wjy: 任一版本事务运行时吞掉重复点击，不创建第二次共享目录复制或清理任务。
                }
                m_publishPreparing = true;
                update(); // wjy: 后台任务启动前立即把按钮切换为“发布中”并禁用手型反馈。
                QPointer<DeviceGrid> self(this);
                runBackgroundTask([self] {
                    QString error;
                    const bool published = platform::UpdateService::instance().publishCurrentBuild(&error); // wjy: 发布目录遍历、文件复制、校验和最终版本标记提交全部留在后台线程。
                    if (!self) {
                        return;
                    }
                    QMetaObject::invokeMethod(self, [self, published, error] {
                        if (!self) {
                            return;
                        }
                        DeviceGrid* grid = self.data();
                        grid->m_publishPreparing = false;
                        grid->update();
                        if (published) {
                            QMessageBox::information(grid, QString(), QString::fromUtf8("已发布到共享目录，其它设备将自动检测到更新。"));
                            grid->refreshRollbackVersions(true); // wjy: 发布成功后强制后台刷新，原版本立即成为可回撤目标且不受缓存影响。
                        } else {
                            QMessageBox::warning(grid, QString(), QString::fromUtf8("发布失败：%1").arg(error));
                        }
                    }, Qt::QueuedConnection);
                });
                event->accept();
                return;
            }
            // ===end====
            if (settingsLayout.containsPoint(settingsRemoteWakeupSwitchRect(), event->pos())) {
                toggleRemoteWakeup();
                event->accept();
                return;
            }
            if (settingsLayout.containsPoint(settingsPreventSleepSwitchRect(), event->pos())) {
                m_preventSleepEnabled = !m_preventSleepEnabled;
                platform::AppSettings::setPreventSleepEnabled(m_preventSleepEnabled);
                platform::PowerManager::setPreventSleepEnabled(m_preventSleepEnabled);
                update();
                event->accept();
                return;
            }
            // =====wjy====
            if (settingsLayout.containsPoint(settingsHideLocalDeviceSwitchRect(), event->pos())) {
                m_hideLocalDeviceEnabled = !m_hideLocalDeviceEnabled;
                platform::AppSettings::setHideLocalDeviceEnabled(m_hideLocalDeviceEnabled); // wjy: 先持久化用户选择，进程异常退出后下次启动仍使用相同可见性策略。
                applyHideLocalDeviceSetting(!m_hideLocalDeviceEnabled); // wjy: 开启立即隐藏并修正选择；关闭时目录缺少本机则立即补回。
                event->accept();
                return;
            }
            if (settingsLayout.containsPoint(settingsPeriodicDeviceDiscoverySwitchRect(), event->pos())) {
                m_periodicDeviceDiscoveryEnabled = !m_periodicDeviceDiscoveryEnabled;
                platform::AppSettings::setPeriodicDeviceDiscoveryEnabled(m_periodicDeviceDiscoveryEnabled);
                applyPeriodicDeviceDiscoverySetting(m_periodicDeviceDiscoveryEnabled); // wjy: 开启时立即扫描一次，之后按 60 秒周期；关闭时立即停表。
                update();
                event->accept();
                return;
            }
            if (settingsLayout.containsPoint(settingsWallpaperRotationSwitchRect(), event->pos())) {
                m_wallpaperRotationEnabled = !m_wallpaperRotationEnabled;
                platform::AppSettings::setDesktopWallpaperRotationEnabled(m_wallpaperRotationEnabled);
                applyDesktopWallpaperRotationSetting(m_wallpaperRotationEnabled); // wjy: 开启后立即后台切换首张并启动分钟周期；关闭后立即停表。
                event->accept();
                return;
            }
            // ===end====
        }

        if (!m_detailPanelCollapsed && !m_settingsSelected && !m_remoteAssistSelected && !m_localInfoSelected) {
            if (detailLocalTabRect().contains(event->pos())) {
                showLocalSystemInfoTab(); // wjy: 本机页签点击后刷新静态信息、建立 CPU/GPU 基线并启动三类资源一秒采样。
                event->accept();
                return;
            }
#if 0
            // wjy: 查找页签暂缓启用；保留点击切页逻辑，后续删除门禁即可恢复。
            if (detailSearchTabRect().contains(event->pos())) {
                showDeviceSearchPanel(); // wjy: 点击查找页签时刷新最新设备快照并把输入焦点交给面板。
                event->accept();
                return;
            }
#endif
            if (detailConfigTabRect().contains(event->pos()) || detailScriptLogTabRect().contains(event->pos())) {
                const bool scriptLogSelected = detailScriptLogTabRect().contains(event->pos());
                m_deviceDetailTab = scriptLogSelected
                    ? DeviceDetailTab::ScriptLog
                    : DeviceDetailTab::Config;
                if (scriptLogSelected && !m_scriptFolderTreeLoaded) {
                    requestScriptFolderTreeRefresh(); // wjy: 启动时离线的设备进入脚本页后可重新异步探测，不阻塞页签切换。
                }
                updateScriptFileEditorControls();
                update();
                event->accept();
                return;
            }
            clearBottomActionHover();
        }
    }

    QFrame::mouseReleaseEvent(event);
}

} // namespace ui
