#include "ui/RemoteDesktopWindow.h"

#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "stream/StreamRuntime.h"
#include "ui/D3D11FramePresenter.h"
#include "ui/NativeRemoteTitleBarSurface.h"
#include "ui/RemoteConnectionState.h"
#include "ui/RemoteClipboardCodec.h"
#include "ui/RemoteCursorShape.h"
#include "ui/RemoteTitleBarLayout.h"
#include "ui/RemoteTitleBarRenderer.h"
#include "ui/RemoteViewerLifecycleManager.h"

#include <QByteArray>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegion>
#include <QResizeEvent>
#include <QRubberBand>
#include <QScreen>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <exception>
#include <limits>
#include <vector>

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

// =====wjy====
struct RemoteDesktopViewerCallbackContext {
    QPointer<RemoteDesktopWindow> window; // wjy: QObject销毁后自动变空，原生线程迟到回调不会继续解引用已释放窗口。
    quint64 generation = 0; // wjy: 上下文固定记录创建它的viewer代际，重连时无需修改旧上下文本身。
};
// ===end====

namespace {

#if defined(Q_OS_WIN)
RemoteDesktopWindow* g_keyboardForwardTarget = nullptr;
HHOOK g_keyboardHook = nullptr;
QSet<int> g_hookPressedKeys;
QSet<int> g_hookConsumedKeys; // wjy: 仅记录当前钩子真正吞掉过KeyDown的键，KeyUp必须按相同归属决定留给远控还是放回本机。
RemoteDesktopWindow* g_rawInputMouseTarget = nullptr; // wjy: Windows 每类 Raw Input 设备在单进程内只有一个注册目标，记录当前活动远控窗口以安全交接所有权。

// =====wjy====
using DwmSetWindowAttributeFunction = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

bool applyNativeWindowCorners(HWND window, bool rounded)
{
    if (!window) {
        return false;
    }
    static const DwmSetWindowAttributeFunction setWindowAttribute = [] {
        HMODULE module = ::GetModuleHandleW(L"dwmapi.dll");
        if (!module) module = ::LoadLibraryW(L"dwmapi.dll"); // wjy: 动态解析避免为旧Windows额外增加硬链接依赖，进程结束时由系统统一释放模块。
        return module
            ? reinterpret_cast<DwmSetWindowAttributeFunction>(::GetProcAddress(module, "DwmSetWindowAttribute"))
            : nullptr;
    }();
    if (!setWindowAttribute) {
        return false;
    }

    constexpr DWORD kWindowCornerPreferenceAttribute = 33; // wjy: DWMWA_WINDOW_CORNER_PREFERENCE，使用数值兼容未声明该枚举的旧Windows SDK。
    constexpr DWORD kWindowBorderColorAttribute = 34; // wjy: DWMWA_BORDER_COLOR，用于取消无边框窗口可能残留的系统直角描边。
    constexpr int kDoNotRound = 1;
    constexpr int kRound = 2;
    const int preference = rounded ? kRound : kDoNotRound;
    const HRESULT cornerResult = setWindowAttribute(
        window,
        kWindowCornerPreferenceAttribute,
        &preference,
        sizeof(preference));
    if (FAILED(cornerResult)) {
        return false; // wjy: Windows 10等不支持该属性的平台继续使用下方QRegion兼容路径。
    }
    if (rounded) {
        constexpr COLORREF kNoBorderColor = 0xFFFFFFFE; // wjy: DWMWA_COLOR_NONE，避免系统边框在圆角外形成直角填充。
        setWindowAttribute(window, kWindowBorderColorAttribute, &kNoBorderColor, sizeof(kNoBorderColor));
    }
    return true;
}
// ===end====
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
constexpr int kTextureFailuresBeforeSoftwareFallback = 3; // wjy: 单个瞬时共享纹理失败先保留上一帧，连续三次失败才进入有上限的软件保活。
// =====wjy====
constexpr qsizetype kMaximumRecordedInputScriptEvents = 100000; // wjy: 与脚本存储层保持一致，限制停止录制时构造JSON造成的主线程内存与延迟峰值。
constexpr qint64 kMaximumRecordedInputScriptDurationMs = 24LL * 60LL * 60LL * 1000LL;
constexpr qint64 kAbsoluteMouseMoveMergeWindowMs = 8; // wjy: 高频绝对鼠标移动在8毫秒内只保留最新位置，明显压缩文件但不改变最终落点和按钮时序。
constexpr qint64 kRelativeMouseMoveMergeWindowMs = 4; // wjy: 相对鼠标只在极短窗口内累加位移，降低高回报率鼠标事件量且不丢失移动总量。
constexpr qsizetype kMaximumInputScriptEventsPerPlaybackTick = 128; // wjy: 即使脚本包含大量同时间戳事件，每轮也只发送有限批次，让F10停止和窗口消息持续获得事件循环机会。
constexpr int kMaximumInputScriptLoopIntervalMs = 60 * 60 * 1000; // wjy: F10轮间隔上限为一小时，既覆盖长等待自动化，也保持QTimer参数在明确安全范围内。
// ===end====
#define FSREMOTE_LEGACY_PARENT_TITLE_BAR 0 // wjy: 迁移期间保留旧父窗口标题栏绘制块供快速回退，生产默认只启用持久原生DIB表面。

// =====wjy====
struct RemoteInputPlaybackOptions {
    QString filePath;
    int loopCount = 1; // wjy: 0表示无限循环，正数表示包含第一次播放在内的总执行次数。
    int loopIntervalMs = 0; // wjy: 仅作用于两轮完整脚本之间；0表示上一轮结束后立即进入下一轮。
    double speedMultiplier = 1.0;
};

class RemoteInputPlaybackDialog final : public QDialog {
public:
    RemoteInputPlaybackDialog(
        const QString& scriptDirectory,
        const QString& initialFilePath,
        int initialLoopCount,
        int initialLoopIntervalMs,
        double initialSpeedMultiplier,
        QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(QString::fromUtf8("键鼠脚本播放设置"));
        setModal(true);
        setMinimumWidth(540);

        auto* description = new QLabel(
            QString::fromUtf8("选择本地键鼠脚本，并设置循环次数、每轮间隔与执行速度。"), this);
        description->setWordWrap(true);

        m_fileCombo = new QComboBox(this);
        const QFileInfoList scriptFiles = QDir(scriptDirectory).entryInfoList(
            QStringList{QStringLiteral("*.fsinput.json")},
            QDir::Files | QDir::Readable,
            QDir::Name | QDir::IgnoreCase); // wjy: F10窗口直接列出固定script目录中的合法扩展名文件，不再打开第二层文件浏览器。
        int initialFileIndex = -1;
        for (const QFileInfo& scriptFile : scriptFiles) {
            const QString absolutePath = scriptFile.absoluteFilePath();
            m_fileCombo->addItem(scriptFile.fileName(), absolutePath);
            if (QDir::cleanPath(absolutePath).compare(
                    QDir::cleanPath(initialFilePath), Qt::CaseInsensitive) == 0) {
                initialFileIndex = m_fileCombo->count() - 1;
            }
        }
        if (m_fileCombo->count() == 0) {
            m_fileCombo->addItem(QString::fromUtf8("script 文件夹中暂无键鼠脚本"));
            m_fileCombo->setEnabled(false);
        } else {
            m_fileCombo->setCurrentIndex(initialFileIndex >= 0 ? initialFileIndex : 0);
            m_fileCombo->setToolTip(QDir::toNativeSeparators(
                m_fileCombo->currentData().toString()));
            connect(m_fileCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
                m_fileCombo->setToolTip(QDir::toNativeSeparators(
                    m_fileCombo->currentData().toString())); // wjy: 下拉框显示短文件名，悬停提示始终展示当前脚本完整本地路径。
            });
        }

        m_loopCountSpin = new QSpinBox(this);
        m_loopCountSpin->setRange(0, 1000000);
        m_loopCountSpin->setSpecialValueText(QString::fromUtf8("无限循环"));
        m_loopCountSpin->setSuffix(QString::fromUtf8(" 次"));
        m_loopCountSpin->setValue(std::clamp(initialLoopCount, 0, 1000000)); // wjy: 正数是总执行次数，0通过特殊文本明确表示不会自然结束。

        m_loopIntervalSpin = new QSpinBox(this);
        m_loopIntervalSpin->setRange(0, kMaximumInputScriptLoopIntervalMs); // wjy: 单轮间隔最多一小时，避免输入溢出，同时足够覆盖长期自动化等待场景。
        m_loopIntervalSpin->setSingleStep(100);
        m_loopIntervalSpin->setSpecialValueText(QString::fromUtf8("无间隔"));
        m_loopIntervalSpin->setSuffix(QString::fromUtf8(" 毫秒"));
        m_loopIntervalSpin->setValue(std::clamp(initialLoopIntervalMs, 0, kMaximumInputScriptLoopIntervalMs)); // wjy: F10再次打开时保留本窗口上次使用的循环间隔。

        m_speedSpin = new QDoubleSpinBox(this);
        m_speedSpin->setRange(0.10, 10.00);
        m_speedSpin->setDecimals(2);
        m_speedSpin->setSingleStep(0.10);
        m_speedSpin->setSuffix(QString::fromUtf8(" 倍"));
        m_speedSpin->setValue(std::clamp(initialSpeedMultiplier, 0.10, 10.00)); // wjy: 小于1倍减速，大于1倍加速，1倍严格沿用原录制时间。

        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->addRow(QString::fromUtf8("脚本文件："), m_fileCombo);
        form->addRow(QString::fromUtf8("循环次数："), m_loopCountSpin);
        form->addRow(QString::fromUtf8("循环间隔："), m_loopIntervalSpin);
        form->addRow(QString::fromUtf8("执行速度："), m_speedSpin);

        auto* hint = new QLabel(
            QString::fromUtf8("循环次数填 0 时持续播放；循环间隔只在一轮完整脚本结束后等待，不影响脚本内部事件速度，也不会在第一次执行前等待。"), this);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color:#667085;"));

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        if (QPushButton* startButton = buttons->button(QDialogButtonBox::Ok)) {
            startButton->setText(QString::fromUtf8("开始播放"));
            startButton->setEnabled(m_fileCombo->isEnabled()); // wjy: script目录为空时不能提交无效播放请求，用户仍可取消关闭窗口。
        }
        if (QPushButton* cancelButton = buttons->button(QDialogButtonBox::Cancel)) {
            cancelButton->setText(QString::fromUtf8("取消"));
        }

        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->addWidget(description);
        rootLayout->addSpacing(4);
        rootLayout->addLayout(form);
        rootLayout->addWidget(hint);
        rootLayout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (!QFileInfo(m_fileCombo->currentData().toString()).isFile()) {
                QMessageBox::warning(this, QString::fromUtf8("无法播放"),
                    QString::fromUtf8("请先选择一个有效的键鼠脚本文件。"));
                return;
            }
            accept();
        });
    }

    RemoteInputPlaybackOptions options() const
    {
        RemoteInputPlaybackOptions result;
        result.filePath = QFileInfo(m_fileCombo->currentData().toString()).absoluteFilePath();
        result.loopCount = m_loopCountSpin->value();
        result.loopIntervalMs = m_loopIntervalSpin->value();
        result.speedMultiplier = m_speedSpin->value();
        return result;
    }

private:
    QComboBox* m_fileCombo = nullptr;
    QSpinBox* m_loopCountSpin = nullptr;
    QSpinBox* m_loopIntervalSpin = nullptr;
    QDoubleSpinBox* m_speedSpin = nullptr;
};
// ===end====

// =====wjy====
uint32_t viewerQualityModeValue(stream::RemoteQualityMode mode)
{
    switch (mode) {
    case stream::RemoteQualityMode::HighQualityLocked: return FSREMOTE_VIEWER_QUALITY_HIGH_LOCKED;
    case stream::RemoteQualityMode::Balanced: return FSREMOTE_VIEWER_QUALITY_BALANCED;
    case stream::RemoteQualityMode::Smooth: return FSREMOTE_VIEWER_QUALITY_SMOOTH;
    case stream::RemoteQualityMode::Automatic:
    case stream::RemoteQualityMode::FollowGlobal:
        return FSREMOTE_VIEWER_QUALITY_AUTOMATIC; // wjy: FollowGlobal在协调器中已解析；防御性回退仍发送自动模式，避免协议收到0。
    }
    return FSREMOTE_VIEWER_QUALITY_AUTOMATIC;
}

QString remoteQualityModeText(stream::RemoteQualityMode mode)
{
    switch (mode) {
    case stream::RemoteQualityMode::FollowGlobal: return QString::fromUtf8("自定义"); // wjy: 保留内部FollowGlobal兼容语义，但按产品文案在界面统一显示“自定义”。
    case stream::RemoteQualityMode::Automatic: return QString::fromUtf8("自动");
    case stream::RemoteQualityMode::HighQualityLocked: return QString::fromUtf8("高质量"); // wjy: 高质量对应原始分辨率/固定请求60 FPS，实际结果仍由Host和传输层回报。
    case stream::RemoteQualityMode::Balanced: return QString::fromUtf8("均衡");
    case stream::RemoteQualityMode::Smooth: return QString::fromUtf8("流畅");
    }
    return QString::fromUtf8("自动");
}

QString remoteQualityReasonText(RemoteQualityDegradationReason reason)
{
    switch (reason) {
    case RemoteQualityDegradationReason::None: return QString::fromUtf8("正常");
    case RemoteQualityDegradationReason::ModePreference: return QString::fromUtf8("模式预设");
    case RemoteQualityDegradationReason::Background: return QString::fromUtf8("后台低性能保活");
    case RemoteQualityDegradationReason::Minimized: return QString::fromUtf8("最小化保活");
    case RemoteQualityDegradationReason::LargestWindowBelowThreshold: return QString::fromUtf8("最大窗口未达到高清阈值");
    case RemoteQualityDegradationReason::PipelinePressure: return QString::fromUtf8("接收管线持续压力");
    case RemoteQualityDegradationReason::SeverePipelinePressure: return QString::fromUtf8("接收管线严重压力");
    case RemoteQualityDegradationReason::SoftwareFallback: return QString::fromUtf8("D3D11恢复中，软件保活");
    }
    return QString::fromUtf8("正常");
}

bool sameQualityPayload(const FsRemoteViewerQualityConfig& left, const FsRemoteViewerQualityConfig& right)
{
    return left.mode == right.mode
        && left.target_width == right.target_width
        && left.target_height == right.target_height
        && left.target_fps == right.target_fps
        && left.max_bitrate_kbps == right.max_bitrate_kbps
        && left.priority == right.priority; // wjy: request_id不参与去重，同一Viewer代际内只有有效参数变化才发送新请求。
}

bool sameQualityDecision(const RemoteQualityDecision& left, const RemoteQualityDecision& right)
{
    return left.effectiveMode == right.effectiveMode
        && left.resolution == right.resolution
        && left.targetWidth == right.targetWidth
        && left.targetHeight == right.targetHeight
        && left.targetFps == right.targetFps
        && left.maxBitrateKbps == right.maxBitrateKbps
        && left.priority == right.priority
        && left.reason == right.reason
        && left.active == right.active
        && left.fullScreen == right.fullScreen
        && left.minimized == right.minimized
        && left.softwareFallback == right.softwareFallback
        && left.audioEnabled == right.audioEnabled; // wjy: 独立音频按钮状态作为真实决策变化处理，不能被相同画质参数去重。
}
// ===end====

enum class NearbyWindowSnapSide {
    None,
    Above,
    Below,
    Left,
    Right,
};

// =====wjy====
enum class NearbyWindowSnapTrigger {
    None,
    WindowEdge,
    CursorEdge,
}; // wjy: 明确记录候选由窗口边缘还是鼠标位置触发，后续排序才能保证鼠标判定拥有绝对优先级。

struct NearbyWindowSnapCandidate {
    RemoteDesktopWindow* target = nullptr;
    NearbyWindowSnapSide side = NearbyWindowSnapSide::None;
    NearbyWindowSnapTrigger trigger = NearbyWindowSnapTrigger::None; // wjy: 候选来源与目标方向一起保存，预览和松手提交仍复用同一份最终几何结果。
    int distance = std::numeric_limits<int>::max();
    int pointerAxisDistance = std::numeric_limits<int>::max(); // wjy: 鼠标在目标窗口对应轴范围内时为 0，优先选择鼠标当前指向的那一格。
    int pointerCenterDistance = std::numeric_limits<int>::max(); // wjy: 鼠标不在任何目标范围内或位于公共边界时，用到目标中心的距离稳定判定。
};
// ===end====

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

// =====wjy====
struct RemoteTitleBarIdentityLayout {
    QFont font;
    bool showLogo = true;
    int textX = 34;
    int nameWidth = 0;
    int ipX = 0;
    int ipWidth = 0;
    int right = 0;
};

RemoteTitleBarIdentityLayout remoteTitleBarIdentityLayout(
    int windowWidth,
    const QString& deviceName,
    const QString& hostIp)
{
    RemoteTitleBarIdentityLayout layout;
    layout.font = QFont(QStringLiteral("Microsoft YaHei UI"));
    layout.font.setPixelSize(12);
    const auto textWidth = [&deviceName, &hostIp](const QFont& font) {
        const QFontMetrics metrics(font);
        return metrics.horizontalAdvance(deviceName)
            + (hostIp.isEmpty() ? 0 : 10 + metrics.horizontalAdvance(hostIp));
    };
    if (layout.textX + textWidth(layout.font) + 6 > windowWidth) {
        layout.showLogo = false;
        layout.textX = 6; // wjy: 窄窗口先移除非必要 Logo，为设备名和 IP 尽量保留显示宽度。
    }
    while (layout.font.pixelSize() > 7
        && textWidth(layout.font) > std::max(0, windowWidth - layout.textX - 6)) {
        layout.font.setPixelSize(layout.font.pixelSize() - 1); // wjy: 与现有绘制策略一致缩小设备信息，快照据此判断真实覆盖范围。
    }
    const QFontMetrics metrics(layout.font);
    layout.nameWidth = metrics.horizontalAdvance(deviceName);
    layout.ipX = layout.textX + layout.nameWidth + (hostIp.isEmpty() ? 0 : 10);
    layout.ipWidth = metrics.horizontalAdvance(hostIp);
    layout.right = std::min(windowWidth - 1, layout.ipX + layout.ipWidth + 6);
    return layout;
}
// ===end====

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
    // =====wjy====
    const auto consider = [&nearest](
                              RemoteDesktopWindow* target,
                              NearbyWindowSnapSide side,
                              NearbyWindowSnapTrigger trigger,
                              int distance,
                              int pointerAxisDistance,
                              int pointerCenterDistance) {
        if (distance > kWindowEdgeSnapDistance) return;
        const int triggerPriority = trigger == NearbyWindowSnapTrigger::CursorEdge ? 0 : 1; // wjy: 鼠标触发固定排在窗口边缘触发之前，不受两者具体距离大小影响。
        const int nearestTriggerPriority = nearest.trigger == NearbyWindowSnapTrigger::CursorEdge ? 0 : 1;
        const bool betterCandidate = !nearest.target
            || triggerPriority < nearestTriggerPriority
            || (triggerPriority == nearestTriggerPriority
                && distance < nearest.distance)
            || (triggerPriority == nearestTriggerPriority
                && distance == nearest.distance
                && pointerAxisDistance < nearest.pointerAxisDistance)
            || (triggerPriority == nearestTriggerPriority
                && distance == nearest.distance
                && pointerAxisDistance == nearest.pointerAxisDistance
                && pointerCenterDistance < nearest.pointerCenterDistance); // wjy: 同一触发来源内再按边缘距离、鼠标投影和目标中心距离排序，保留原有稳定候选选择。
        if (betterCandidate) {
            nearest.target = target;
            nearest.side = side;
            nearest.trigger = trigger; // wjy: 保存胜出候选的来源，使后续窗口候选无法覆盖已经命中的鼠标候选。
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
            if (pointerHorizontalDistance == 0) {
                consider(target, NearbyWindowSnapSide::Above,
                    NearbyWindowSnapTrigger::CursorEdge,
                    qAbs(cursorGlobal.y() - targetGeometry.top()),
                    pointerHorizontalDistance,
                    pointerHorizontalCenterDistance); // wjy: 鼠标靠近目标上边缘时独立触发“吸附到上方”，不要求拖动窗口自身边缘已进入阈值。
                consider(target, NearbyWindowSnapSide::Below,
                    NearbyWindowSnapTrigger::CursorEdge,
                    qAbs(cursorGlobal.y() - (targetGeometry.bottom() + 1)),
                    pointerHorizontalDistance,
                    pointerHorizontalCenterDistance); // wjy: 下边缘使用窗口外侧接触坐标，与最终 moveTop(bottom + 1) 的提交位置一致。
            }
            consider(target, NearbyWindowSnapSide::Above,
                NearbyWindowSnapTrigger::WindowEdge,
                qAbs(proposed.bottom() + 1 - targetGeometry.top()),
                pointerHorizontalDistance,
                pointerHorizontalCenterDistance);
            consider(target, NearbyWindowSnapSide::Below,
                NearbyWindowSnapTrigger::WindowEdge,
                qAbs(proposed.top() - targetGeometry.bottom() - 1),
                pointerHorizontalDistance,
                pointerHorizontalCenterDistance);
        }
        if (rangesOverlap(proposed.top(), proposed.bottom(), targetGeometry.top(), targetGeometry.bottom())) {
            const int pointerVerticalDistance = distanceToRange(
                cursorGlobal.y(), targetGeometry.top(), targetGeometry.bottom()); // wjy: 左右吸附只看鼠标纵坐标当前指向上方还是下方目标窗口。
            const int pointerVerticalCenterDistance = qAbs(
                cursorGlobal.y() - targetGeometry.center().y());
            if (pointerVerticalDistance == 0) {
                consider(target, NearbyWindowSnapSide::Left,
                    NearbyWindowSnapTrigger::CursorEdge,
                    qAbs(cursorGlobal.x() - targetGeometry.left()),
                    pointerVerticalDistance,
                    pointerVerticalCenterDistance); // wjy: 鼠标靠近目标左边缘时优先选择左侧吸附，即使其它窗口边缘候选距离更小也不覆盖它。
                consider(target, NearbyWindowSnapSide::Right,
                    NearbyWindowSnapTrigger::CursorEdge,
                    qAbs(cursorGlobal.x() - (targetGeometry.right() + 1)),
                    pointerVerticalDistance,
                    pointerVerticalCenterDistance); // wjy: 右边缘同样按外侧接触坐标判定，保持四个方向完全对称。
            }
            consider(target, NearbyWindowSnapSide::Left,
                NearbyWindowSnapTrigger::WindowEdge,
                qAbs(proposed.right() + 1 - targetGeometry.left()),
                pointerVerticalDistance,
                pointerVerticalCenterDistance);
            consider(target, NearbyWindowSnapSide::Right,
                NearbyWindowSnapTrigger::WindowEdge,
                qAbs(proposed.left() - targetGeometry.right() - 1),
                pointerVerticalDistance,
                pointerVerticalCenterDistance);
        }
    }
    // ===end====
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
    static QMutex logMutex;
    static bool sizeChecked = false;
    QMutexLocker locker(&logMutex); // wjy: 状态回调线程和Qt线程串行写完整行，避免多窗口诊断互相穿插。
    const QString dataDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
    QDir().mkpath(dataDir); // wjy: 发布目录首次运行时自动创建data文件夹，日志不再落到临时目录。
    const QString logPath = QDir(dataDir).filePath(QStringLiteral("remote_viewer_state.log"));
    if (!sizeChecked) {
        sizeChecked = true;
        if (QFileInfo(logPath).size() > 4 * 1024 * 1024) {
            QFile oversized(logPath);
            oversized.open(QIODevice::WriteOnly | QIODevice::Truncate); // wjy: 每进程最多检查一次，历史文件超过4MB时截断，长期多设备运行不会无限增长。
        }
    }
    QFile file(logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return; // wjy: logging must never block or crash the remote desktop UI.
    }
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
           << QStringLiteral(" qt tid=")
#if defined(Q_OS_WIN)
           << static_cast<qulonglong>(::GetCurrentThreadId())
#else
           << 0
#endif
           << QLatin1Char(' ')
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
            // =====wjy====
            if (info->flags & LLKHF_INJECTED) {
                return 1; // wjy: 保持原有注入事件隔离；直接放行会进入本窗口Qt按键兜底并再次转发远端，存在输入回环风险。
            }
            const int virtualKey = static_cast<int>(info->vkCode);
            RemoteDesktopWindow* target = g_keyboardForwardTarget;
            const bool wasWaitingShortcutRelease = target->isWaitingShortcutRelease();

            if (down) {
                updateTrackedHookKey(virtualKey, true); // wjy: 物理状态服务于组合键匹配和松键等待，与事件最终属于本机还是远控相互独立。
                g_hookConsumedKeys.insert(virtualKey); // wjy: 用原始键码分别记录左右修饰键，并在同步快捷键动作可能切换窗口前完成归属登记。
                if (wasWaitingShortcutRelease) {
                    return 1; // wjy: 快捷键尚未全部松开时继续吞掉新KeyDown，不让半个组合键进入本机或远端。
                }
                const Qt::KeyboardModifiers modifiers = trackedHookShortcutModifiers();
                if (target->handleLocalShortcutKey(virtualKey, modifiers)) {
                    return 1;
                }
                target->forwardNativeKey(virtualKey, true);
                return 1;
            }

            const bool consumedByThisHook = g_hookConsumedKeys.remove(virtualKey) > 0; // wjy: 按原始键码配对抬键；等待状态结束时可能重新安装或切换钩子目标。
            updateTrackedHookKey(virtualKey, false);
            if (wasWaitingShortcutRelease) {
                target->updateShortcutReleaseGuard(); // wjy: 无论KeyDown属于哪里都更新物理松键状态，保证等待门闩可以正常结束。
                if (consumedByThisHook) {
                    return 1; // wjy: 本钩子吞过的快捷键KeyDown配对吞掉KeyUp，远端已由beginShortcutReleaseGuard提前安全释放。
                }
                return CallNextHookEx(g_keyboardHook, code, wParam, lParam); // wjy: 钩子安装前本机已收到KeyDown时，必须把对应KeyUp交还Windows。
            }
            if (!consumedByThisHook) {
                return CallNextHookEx(g_keyboardHook, code, wParam, lParam); // wjy: 新激活远控窗口只看到抬键时既不转发远端也不吞掉，消除本机Ctrl残留。
            }
            target->forwardNativeKey(virtualKey, false);
            return 1;
            // ===end====
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

void installKeyboardHook(RemoteDesktopWindow* window)
{
    if (g_keyboardForwardTarget != window) {
        g_hookPressedKeys.clear();
        g_hookConsumedKeys.clear(); // wjy: 归属只属于单个活动远控窗口，切换目标后旧窗口吞过的按键不能影响新窗口KeyUp判定。
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
        g_hookConsumedKeys.clear(); // wjy: 卸载后下一次安装从空归属开始，钩子关闭期间发生的KeyDown一律由本机拥有。
    }
}
#endif

} // namespace

namespace {

void FSREMOTE_STREAM_CALL onRemoteFrame(void* user, int width, int height, const uint8_t* bgra, uint32_t bgraSize)
{
    try {
    // =====wjy====
    // appendViewerDebugLog(QStringLiteral("onRemoteFrame enter user=%1 size=%2x%3 bytes=%4")
    //     .arg(reinterpret_cast<quintptr>(user))
    //     .arg(width)
    //     .arg(height)
    //     .arg(bgraSize)); // wjy: per-frame log disabled to avoid synchronous file writes on the video path.
    // ===end====
    auto* context = static_cast<RemoteDesktopViewerCallbackContext*>(user); // wjy: 原生user不再直接指向窗口，而是指向随viewer stop保持存活的代际上下文。
    RemoteDesktopWindow* window = context ? context->window.data() : nullptr;
    if (!window || !window->acceptsViewerGeneration(context->generation) || width <= 0 || height <= 0 || !bgra) {
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
        window->enqueueRemoteFrame(std::move(copy), context->generation); // wjy: BGRA复制完成后再次由窗口校验同一代际，覆盖关闭/重连与复制过程并发的竞态。
    } catch (...) {
        return; // wjy: QImage复制或Qt容器分配失败只能丢弃当前帧，异常绝不能穿过C ABI回调终止解码线程或进程。
    }
}

int FSREMOTE_STREAM_CALL onRemoteTextureFrame(
    void* user,
    int width,
    int height,
    void* sharedHandle,
    uint64_t frameId,
    int64_t rtpTimestamp,
    int64_t renderTimeMs,
    uint64_t decodedAtUs,
    double encodedMbps)
{
    try {
        auto* context = static_cast<RemoteDesktopViewerCallbackContext*>(user); // wjy: 每个纹理回调携带固定代际，旧连接即使仍在解码也只能写入自己的上下文。
        RemoteDesktopWindow* window = context ? context->window.data() : nullptr;
        if (!window || !context || width <= 0 || height <= 0 || !sharedHandle) {
            return 0;
        }
        return window->enqueueRemoteTextureFrame(
            width,
            height,
            sharedHandle,
            frameId,
            rtpTimestamp,
            renderTimeMs,
            decodedAtUs,
            encodedMbps,
            context->generation); // wjy: 代际判断与真实解码时间轴一起交给窗口，旧Viewer仍只返回受控丢帧。
    } catch (...) {
        return 0;
    } // wjy: Qt投递或单槽更新异常只让该帧走软件回退，不得越过原生解码回调边界。
}

void FSREMOTE_STREAM_CALL onViewerStatus(void* user, int code, const char* message)
{
    try {
    auto* context = static_cast<RemoteDesktopViewerCallbackContext*>(user); // wjy: 状态回调和视频帧使用同一代际上下文，旧会话断线状态不会覆盖新会话UI。
    QPointer<RemoteDesktopWindow> window = context ? context->window : QPointer<RemoteDesktopWindow>();
    const quint64 generation = context ? context->generation : 0;
    if (!window || !window->acceptsViewerGeneration(generation)) {
        return;
    }

    const QString text = message ? QString::fromUtf8(message) : QString();
    // =====wjy====
    if (code != 60 && code != FSREMOTE_STATUS_CURSOR_SHAPE) {
        appendViewerDebugLog(QStringLiteral("viewer status code=%1 message=%2").arg(code).arg(text)); // wjy: 保留低频连接检查点，同时跳过逐帧码率和高频光标变化，避免鼠标经过窗口边缘时产生磁盘日志压力。
    }
    // ===end====
    QMetaObject::invokeMethod(window.data(), [window, generation, code, text] {
        if (!window || !window->acceptsViewerGeneration(generation) || window->isClosingConnection()) {
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
        // =====wjy====
        if (code == FSREMOTE_STATUS_CURSOR_SHAPE) {
            window->setRemoteCursorShape(text); // wjy: 原生层已严格验证协议；Qt 层仍按完整版本消息映射，避免状态码复用时误改光标。
            return;
        }
        if (code == FSREMOTE_STATUS_MOUSE_BACKEND) {
            window->setRemoteMouseBackendStatus(text); // wjy: 后端确认在 Qt 主线程提交，绘制、按钮 pending 和超时代际始终串行。
            return;
        }
        // ===end====
        if (code == FSREMOTE_STATUS_QUALITY_APPLIED) {
            window->refreshAppliedRemoteQualityStatus(); // wjy: 状态回调只通知“有新快照”，实际值通过稳定C ABI读取，避免解析脆弱文本。
            return;
        }
        // ===end====
        window->setConnectionStatus(code, text);
    }, Qt::QueuedConnection);
    } catch (...) {
        return; // wjy: 状态文本或Qt事件分配失败仅丢弃本次通知，Viewer工作线程继续运行且其它窗口不受影响。
    }
}

} // namespace

RemoteDesktopWindow::RemoteDesktopWindow(
    const QString& deviceName,
    const QString& hostIp,
    RemoteViewerLifecycleManager* lifecycleManager,
    RemoteInputBroadcastCoordinator* inputBroadcastCoordinator,
    QWidget* parent)
    : QWidget(parent)
    , m_deviceName(deviceName)
    , m_hostIp(hostIp)
    , m_lifecycleManager(lifecycleManager) // wjy: 生命周期管理器由DeviceGrid统一持有，保证窗口关闭和应用退出期间指针始终有效。
    , m_inputBroadcastCoordinator(inputBroadcastCoordinator) // wjy: 普通与平铺窗口借用同一个协调器，单主控约束覆盖全部远控窗口。
    , m_globalQualityConfiguration(platform::AppSettings::remoteQualityConfiguration()) // wjy: 新窗口仍读取全局FPS和安全边界参数，但模式默认由会话内“自动”决定。
{
    // =====wjy====
    appendViewerDebugLog(QStringLiteral("RemoteDesktopWindow ctor device=%1 host=%2").arg(deviceName, hostIp)); // wjy: mark each remote desktop window creation.
    stream::RemoteQualityMode savedDeviceMode = stream::RemoteQualityMode::Automatic;
    if (platform::AppSettings::remoteDeviceQualityMode(m_hostIp, &savedDeviceMode)) {
        m_qualityOverrideMode = savedDeviceMode; // wjy: 设备有历史画质时在首轮协调前恢复；首次设备继续使用成员默认的“自动”。
    }
    // ===end====
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_QuitOnClose, false);
    // =====wjy====
    // wjy: 不再启用WA_NoSystemBackground；Qt需保留顶层backing store，原生子表面短暂延迟时也不能让客户区直接看穿到桌面。
    setAttribute(Qt::WA_OpaquePaintEvent); // wjy: 本窗口 paintEvent 每次都会用纯黑覆盖整窗，明确声明不透明可阻止 Qt 额外传播或合成父级背景。
#if defined(Q_OS_WIN)
    const HWND remoteWindowHandle = reinterpret_cast<HWND>(winId()); // wjy: 顶层远控窗必须先取得稳定HWND，下面的子窗口裁剪样式才能在D3D Presenter创建前生效。
    if (remoteWindowHandle) {
        const LONG_PTR currentStyle = ::GetWindowLongPtrW(remoteWindowHandle, GWL_STYLE);
        ::SetWindowLongPtrW(
            remoteWindowHandle,
            GWL_STYLE,
            currentStyle | WS_CLIPCHILDREN); // wjy: Qt绘制标题栏时裁掉标题栏下的D3D11原生内容层，两个呈现层不会互相覆盖或抢刷新。
    }
#endif
    // ===end====
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
    // =====wjy====
    m_unifiedCompositor = std::make_unique<RemoteWindowCompositor>(); // wjy: 运行时开关只决定是否启用新状态/布局路径，不修改流协议或用户保存的设备设置。
    const QByteArray pixelProbe = qgetenv("FSREMOTE_RESIZE_PIXEL_PROBE").trimmed().toLower();
    m_resizePixelProbeEnabled = pixelProbe == "1"
        || pixelProbe == "true"
        || pixelProbe == "yes"
        || pixelProbe == "on"; // wjy: 采样开关独立于合成路径，便于旧路径与新路径做同屏对照。
    appendViewerDebugLog(QStringLiteral("remote compositor path=%1 enabled=%2")
        .arg(RemoteWindowCompositorConfig::activePathId())
        .arg(m_unifiedCompositor->isEnabled() ? 1 : 0)); // wjy: 日志明确当前窗口使用旧多表面还是新保留式合成路径。
    commitCompositorLayout(); // wjy: 在首个子表面创建前提交初始几何，后续标题栏、输入和DPI都复用同一快照。
    // ===end====
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
    m_frameStatsClock.start(); // wjy: Start a lightweight in-memory FPS meter for the remote desktop title bar.
    m_framePresentTimer = new QTimer(this);
    m_framePresentTimer->setTimerType(Qt::PreciseTimer);
    m_framePresentTimer->setInterval(16);
    connect(m_framePresentTimer, &QTimer::timeout, this, &RemoteDesktopWindow::flushPendingRemoteFrame);
    m_framePresentTimer->start(); // wjy: Fixed latest-frame presentation prevents full-screen repaint pressure from building visible latency.
    m_texturePresenter = new D3D11FramePresenter(this);
    if (m_unifiedCompositor && m_unifiedCompositor->isEnabled()) {
        m_texturePresenter->setCompositorHostWindow(
            reinterpret_cast<void*>(winId()),
            true); // wjy: 新路径的SwapChain绑定顶层窗口DComp目标，Presenter子控件保持隐藏，不再生成第二个可见视频表面。
        if (m_nativeTitleBarSurface && m_nativeTitleBarSurface->isCreated()) {
            m_nativeTitleBarSurface->setVisible(false); // wjy: DComp叠加层尚未首帧前也先隐藏旧标题栏，避免初始化阶段双重合成。
        }
    }
    // =====wjy====
#if !FSREMOTE_LEGACY_PARENT_TITLE_BAR
    m_nativeTitleBarSurface = std::make_unique<NativeRemoteTitleBarSurface>();
    m_nativeTitleBarSurface->create(winId()); // wjy: 标题栏只用一个固定不动的原生子HWND，身份段与按钮段由它内部合成，缩放期间不产生任何窗口操作。
#endif
    // ===end====
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
        updateTitleBarHover(parentPosition); // wjy: D3D内容区内连续鼠标移动最多在离开标题栏按钮时刷新一次，不再逐事件重画标题栏。
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
    // =====wjy====
    m_textureRecoveryTimer = new QTimer(this);
    m_textureRecoveryTimer->setSingleShot(true);
    connect(m_textureRecoveryTimer, &QTimer::timeout, this, [this] {
        if (m_closeInProgress || !m_texturePresenter) {
            return;
        }
        if (!m_textureFrameActive) {
            m_texturePresenter->reset(); // wjy: 软件帧已经在父窗口铺好后再释放隐藏的旧SwapChain，恢复过程不露出黑色背景。
        }
        m_texturePresentFailed.store(false, std::memory_order_release); // wjy: 只开放下一帧纹理尝试；若仍失败会再次进入有上限的软件回退。
        emit remoteQualityInputsChanged();
    });
    // ===end====
    m_sessionTimer = new QTimer(this);
    m_sessionTimer->setInterval(1000);
    connect(m_sessionTimer, &QTimer::timeout, this, [this] {
        requestTitleBarUpdate(); // wjy: 会话时间、FPS和码率共用一秒刷新节奏，统一更新原生标题栏表面。
    });
    m_sessionTimer->start();

    // =====wjy====
    m_inputScriptPlaybackTimer = new QTimer(this);
    m_inputScriptPlaybackTimer->setSingleShot(true);
    m_inputScriptPlaybackTimer->setTimerType(Qt::PreciseTimer); // wjy: 使用单次高精度定时器并按绝对录制时间反复校正，单次事件处理延迟不会累计成整段脚本漂移。
    connect(m_inputScriptPlaybackTimer, &QTimer::timeout,
        this, &RemoteDesktopWindow::processInputScriptPlaybackEvents);
    // ===end====

    // =====wjy====
    m_networkReconnectTimer = new QTimer(this);
    m_networkReconnectTimer->setSingleShot(true); // wjy: 同一个单次定时器分时承担3秒自恢复等待和后续退避重连，不创建常驻轮询线程。
    connect(m_networkReconnectTimer, &QTimer::timeout, this, [this] {
        if (!m_networkReconnectActive || m_closeInProgress || m_applicationExitInProgress
            || remoteUpdateActive()) {
            cancelNetworkReconnect();
            return;
        }
        if (m_networkRecoveryGraceActive) {
            m_networkRecoveryGraceActive = false;
            beginNetworkReconnect(); // wjy: 三秒内没有成功呈现新帧，旧会话视为不可用并进入安全stop流程。
            return;
        }
        attemptNetworkReconnect(); // wjy: 退避到期后只尝试一次，失败状态回调会再次安排下一档。
    });
    // ===end====

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
        if (m_texturePresenter && m_texturePresenter->usesCompositorSurface()) {
            presentCompositorOverlay(); // wjy: 更新动画和状态文本直接刷新唯一DComp叠加层，父窗口不再产生第二套延迟画面。
        }
    });

    // ===end====

    // =====wjy====
    if (m_inputBroadcastCoordinator) {
        m_inputBroadcastCoordinator->registerEndpoint(this, this); // wjy: 完成控件和定时器初始化后登记，角色回调只刷新已经可用的标题栏状态。
    }
    // ===end====
    updateNativeTitleBarSurface(true); // wjy: 构造完成后立即提交第一张完整标题栏，窗口首次显示不依赖父QWidget异步paintEvent。
    QTimer::singleShot(0, this, &RemoteDesktopWindow::startViewerConnection);
}

RemoteDesktopWindow::~RemoteDesktopWindow()
{
    // =====wjy====
    appendViewerDebugLog(QStringLiteral("RemoteDesktopWindow dtor begin")); // wjy: identify crashes during window teardown.
    // ===end====
    // =====wjy====
    cancelInputScriptRecording(QStringLiteral("window_destructed"));
    stopInputScriptPlayback(QStringLiteral("window_destructed"), true); // wjy: 异常析构也在注销同步端点和清空Viewer句柄前尽力释放脚本持有键鼠。
    // ===end====
    // =====wjy====
    if (m_inputBroadcastCoordinator) {
        m_inputBroadcastCoordinator->unregisterEndpoint(this); // wjy: 在清空 Viewer 句柄前兜底注销，使协调器仍有机会向可达会话补发释放事件。
    }
    // ===end====
    if (m_remoteUpdateTimer) {
        m_remoteUpdateTimer->stop(); // wjy: 窗口析构后停止更新状态轮询和遮罩动画。
    }
    if (m_networkReconnectTimer) {
        m_networkReconnectTimer->stop(); // wjy: QObject子对象析构前先取消待执行重试，避免析构期间再次申请Viewer。
    }
    invalidateViewerCallbacks(); // wjy: 析构开始即作废活动代际并丢弃尚未呈现的BGRA/纹理，旧原生回调无法再访问Presenter。
    // =====wjy====
    if (m_lifecycleManager) {
        m_lifecycleManager->cancelViewerStart(this, false); // wjy: 析构兜底移除等待/活动初始化记录，避免异常销毁的窗口永久占住4路启动名额。
    }
    m_viewerStartQueued = false;
    m_viewerStartAdmissionActive = false;
    // ===end====
    m_remoteMouseCaptureRequested = false;
    m_manualMouseLockActive = false;
    suspendRemoteMouseCapture(); // wjy: 析构必须无条件撤销实际 Raw Input；对象即将销毁，因此两类锁定意图也一并清空。
    releasePressedKeys();
    setKeyboardForwardingActive(false);
    // =====wjy====
    const FsRemoteStreamHandle handle = m_viewerHandle;
    m_viewerHandle = nullptr;
    auto callbackContext = std::move(m_viewerCallbackContext); // wjy: 原生stop返回前持续持有回调user上下文，避免析构窗口后DLL迟到访问悬空地址。
    if (handle) {
        const auto stopTask = [handle, callbackContext = std::move(callbackContext)] {
            stream::StreamRuntime::instance().stop(handle); // wjy: 非正常直接析构也优先进入共享工作池，不在Qt析构栈中长时间阻塞。
        };
        if (!m_lifecycleManager
            || !m_lifecycleManager->submitLifecycleTask(
                stopTask,
                RemoteViewerLifecycleManager::LifecycleTaskPriority::Cleanup)) {
            stopTask(); // wjy: 管理器已经封闭队列时同步兜底，宁可延迟当前析构也不能泄漏原生Viewer句柄。
        }
    }
    // ===end====
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
    const bool qualityVisibilityChanged = event
        && (event->type() == QEvent::WindowStateChange
            || event->type() == QEvent::Show
            || event->type() == QEvent::Hide
            || event->type() == QEvent::WindowActivate
            || event->type() == QEvent::WindowDeactivate); // wjy: 显隐、最小化和焦点状态变化统一触发一次焦点策略重算；音频按钮仍独立控制。
    const bool titleBarScaleChanged = event
        && event->type() == QEvent::DevicePixelRatioChange; // wjy: 跨不同DPI显示器后必须按新物理像素尺寸重建标题栏DIB，逻辑命中矩形保持不变。
    if (event && event->type() == QEvent::WindowActivate) {
        emit activated(this);
    }

    const bool handled = QWidget::event(event);
    if (windowStateChanged) {
        updateWindowMask(); // wjy: 进入全屏时清除圆角遮罩，退出全屏时恢复普通窗口圆角。
        updateTexturePresenterGeometry(); // wjy: 纹理直呈模式也要立即扩展到新的远控画面区域。
        ++m_titleBarVisualRevision;
        updateNativeTitleBarSurface(true); // wjy: 全屏切换显式隐藏或恢复标题栏子HWND，并按最终宽度与DPI提交完整新帧。
        update(); // wjy: 状态改变后重绘，确保旧标题栏不会残留在全屏画面顶部。
    }
    if (qualityVisibilityChanged && !m_closeInProgress) {
        emit remoteQualityInputsChanged(); // wjy: 显隐、最小化和窗口状态变化立即重算焦点窗口与后台FPS，不等待1秒轮询。
    }
    if (titleBarScaleChanged) {
        ++m_titleBarVisualRevision;
        commitCompositorLayout(); // wjy: DPI 切换与标题栏重建共享同一物理像素快照，输入区域不会继续使用旧缩放比例。
        updateNativeTitleBarSurface(true);
    }
    return handled;
}

bool RemoteDesktopWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#if defined(Q_OS_WIN)
    Q_UNUSED(eventType)
    const auto* nativeMessage = static_cast<MSG*>(message);
    // =====wjy====
    if (nativeMessage && nativeMessage->message == WM_ERASEBKGND) {
        if (m_resizingWindow) {
            appendResizeDebugTrace(QStringLiteral("parent.native.WM_ERASEBKGND")); // wjy: 记录Windows擦除消息是否在尺寸手势中先于D3D子窗口补帧到达。
        }
        if (result) *result = 1; // wjy: 父窗口和D3D11内容层都由各自的paint/Present完整覆盖，禁止Windows擦除阶段先改变标题栏颜色。
        return true; // wjy: 不在擦除消息中绘制整窗，避免标题栏控件在高频resize期间被清空后再重画造成闪烁。
    }
    if (nativeMessage
        && nativeMessage->message == WM_MOVING
        && m_draggingWindow
        && m_systemWindowOperationActive
        && nativeMessage->lParam) {
        const auto* movingRect = reinterpret_cast<const RECT*>(nativeMessage->lParam);
        const QRect proposedGeometry(
            movingRect->left,
            movingRect->top,
            movingRect->right - movingRect->left,
            movingRect->bottom - movingRect->top); // wjy: 直接读取Windows本轮将提交的窗口矩形，只计算吸附预览而不改写系统移动轨迹。
        updateSnapPreviewForGeometry(proposedGeometry, QCursor::pos());
    }
    if (nativeMessage
        && nativeMessage->message == WM_EXITSIZEMOVE
        && m_systemWindowOperationActive
        && (m_draggingWindow || m_resizingWindow)) {
        QMetaObject::invokeMethod(this, [this] {
            finishInteractiveWindowOperation(); // wjy: 退出原生模态移动循环后再恢复Mask并提交吸附，避免在DefWindowProc调用栈中重入SetWindowPos。
        }, Qt::QueuedConnection);
    }
    // ===end====
    if (nativeMessage
        && nativeMessage->message == WM_INPUT
        && m_remoteMouseCaptureActive
        && m_rawInputMouseCaptureActive
        && isActiveWindow()) {
        UINT inputBytes = 0;
        const HRAWINPUT rawHandle = reinterpret_cast<HRAWINPUT>(nativeMessage->lParam);
        if (::GetRawInputData(rawHandle, RID_INPUT, nullptr, &inputBytes, sizeof(RAWINPUTHEADER)) == 0
            && inputBytes >= sizeof(RAWINPUTHEADER)
            && inputBytes <= 64 * 1024) {
            std::vector<BYTE> storage(inputBytes); // wjy: 鼠标报告通常固定很小，但按系统返回长度分配并设置 64KB 上限，避免畸形长度消耗无界内存。
            UINT copiedBytes = inputBytes;
            if (::GetRawInputData(
                    rawHandle,
                    RID_INPUT,
                    storage.data(),
                    &copiedBytes,
                    sizeof(RAWINPUTHEADER)) == inputBytes) {
                const auto* raw = reinterpret_cast<const RAWINPUT*>(storage.data());
                if (raw->header.dwType == RIM_TYPEMOUSE
                    && (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    const int dx = static_cast<int>(raw->data.mouse.lLastX);
                    const int dy = static_cast<int>(raw->data.mouse.lLastY);
                    if (dx != 0 || dy != 0) {
                        sendRemoteRawMouseRelativeMove(dx, dy, QApplication::mouseButtons()); // wjy: 只读取实体设备相对计数；按钮仍由既有 Qt down/up 路径维护，避免同一按钮重复注入。
                        recenterRemoteMouseCapture(); // wjy: Raw Input 不依赖系统光标位置，但继续把隐藏光标留在远控画面中心，防止点击落到其它本机窗口。
                    }
                }
            }
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return QWidget::nativeEvent(eventType, message, result); // wjy: 不吞掉 WM_INPUT，让 Qt/DefWindowProc 完成 Windows 要求的前台原始输入清理。
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
    requestTitleBarUpdate(); // wjy: 版本入口变化只刷新标题栏；移动或缩放期间统一延迟。
}

// =====wjy====
void RemoteDesktopWindow::setGlobalQualityConfiguration(const stream::RemoteQualityConfiguration& configuration)
{
    const stream::RemoteQualityConfiguration normalized = stream::normalizedRemoteQualityConfiguration(configuration);
    if (m_globalQualityConfiguration == normalized) {
        return;
    }
    m_globalQualityConfiguration = normalized; // wjy: 每个窗口立即接收同一默认模式；局部覆盖继续直接使用自身固定预设。
    requestTitleBarUpdate(); // wjy: 全局策略反馈只存在于标题栏，不再让全屏或移动状态触发整窗重画。
    emit remoteQualityInputsChanged(); // wjy: 主设置保存后立即重算全部窗口，无需关闭重连或等待周期轮询。
}

stream::RemoteQualityMode RemoteDesktopWindow::qualityOverrideMode() const
{
    return m_qualityOverrideMode; // wjy: 只返回当前窗口内存状态，不从全局设置反推，保证“局部覆盖”语义清晰。
}

RemoteQualityWindowMetrics RemoteDesktopWindow::remoteQualityMetrics()
{
    RemoteQualityWindowMetrics metrics;
    metrics.windowId = reinterpret_cast<uintptr_t>(this); // wjy: 顶层窗口对象生命周期内地址稳定，关闭时协调器显式删除对应滞回状态。
    metrics.effectiveMode = effectiveQualityMode();
    metrics.visible = isVisible() && !m_closeInProgress;
    metrics.minimized = isMinimized();
    metrics.fullScreen = isFullScreen(); // wjy: 保留全屏状态用于标题栏/诊断；视频高性能现在只由真实焦点决定。
    metrics.softwareFallback = m_softwareFallbackActive; // wjy: 重试开放期间也保持BGRA回退硬上限，必须等纹理实际成功后才恢复。
    const QSize sourceSize = remoteFrameSize().isValid() ? remoteFrameSize() : QSize(1920, 1080);
    metrics.sourceWidth = sourceSize.width();
    metrics.sourceHeight = sourceSize.height();
    metrics.viewportWidth = qMax(1, width());
    metrics.viewportHeight = qMax(1, height() - titleBarHeight()); // wjy: 平铺小窗口按真实显示高度降低像素，优先把资源留给FPS。
    metrics.viewportArea = metrics.viewportWidth * metrics.viewportHeight;
    metrics.receiveFps = qMax(0.0, m_receiveFps);
    metrics.encodedMbps = qMax(0.0, m_encodedMbps);

    FsRemoteViewerPerformanceStats nativeStats = {};
    nativeStats.struct_size = sizeof(nativeStats);
    nativeStats.version = 1;
    if (m_viewerHandle
        && stream::StreamRuntime::instance().viewerPerformanceStats(m_viewerHandle, &nativeStats)) {
        RemotePerformanceCounters counters;
        counters.sampleTimeMs = nativeStats.sample_time_ms;
        counters.framesReceived = nativeStats.frames_received;
        counters.framesDecoded = nativeStats.frames_decoded;
        counters.framesDropped = nativeStats.frames_dropped;
        counters.freezeCount = nativeStats.freeze_count;
        counters.jitterBufferEmittedCount = nativeStats.jitter_buffer_emitted_count;
        counters.packetsReceived = nativeStats.packets_received;
        counters.packetsLost = nativeStats.packets_lost;
        counters.totalDecodeTimeMs = nativeStats.total_decode_time_ms;
        counters.totalProcessingDelayMs = nativeStats.total_processing_delay_ms;
        counters.totalFreezesDurationMs = nativeStats.total_freezes_duration_ms;
        counters.totalJitterBufferDelayMs = nativeStats.total_jitter_buffer_delay_ms;
        counters.roundTripTimeMs = nativeStats.round_trip_time_ms;
        counters.availableIncomingBitrateKbps = nativeStats.available_incoming_bitrate_kbps;
        metrics.performance = m_performanceSignalSampler.sample(counters); // wjy: 只有第二份单调快照开始产生有效压力，重连和旧 DLL 都不会虚假降档。
    }

    const quint64 totalDrops = m_pendingTextureFrames.replacedFrameCount(); // wjy: 本地Presenter压力统计单槽占用时由生产端直接回收的新纹理，不再跨线程替换旧pending。
    const qint64 nowMs = m_sessionClock.isValid() ? m_sessionClock.elapsed() : 0;
    if (m_lastPresenterSampleMs > 0 && nowMs > m_lastPresenterSampleMs
        && m_totalPresentedFrames >= m_lastPresenterSampleFrames
        && totalDrops >= m_lastPresenterSampleDrops) {
        const quint64 presentedDelta = m_totalPresentedFrames - m_lastPresenterSampleFrames;
        const quint64 droppedDelta = totalDrops - m_lastPresenterSampleDrops;
        const quint64 attempted = presentedDelta + droppedDelta;
        if (attempted > 0) {
            metrics.presenterDropRatio = static_cast<double>(droppedDelta) / attempted; // wjy: 单槽忙导致的本地丢帧是直接呈现压力，不与网络接收FPS混为一谈。
        }
    }
    m_lastPresenterSampleFrames = m_totalPresentedFrames;
    m_lastPresenterSampleDrops = totalDrops;
    m_lastPresenterSampleMs = nowMs;
    return metrics;
}

void RemoteDesktopWindow::applyRemoteQualityDecision(const RemoteQualityDecision& decision)
{
    if (decision.windowId != reinterpret_cast<uintptr_t>(this)) {
        return; // wjy: 防御性拒绝其它窗口的决策，批量评估结果不能串到错误Viewer。
    }
    const bool feedbackChanged = !m_hasRemoteQualityDecision
        || !sameQualityDecision(m_remoteQualityDecision, decision);
    m_remoteQualityDecision = decision; // wjy: 无论Viewer是否已创建都保存最新值，连接成功后只补发这一份。
    m_hasRemoteQualityDecision = true;
    sendCurrentRemoteQualityDecision();
    sendCurrentViewerAudioDecision(); // wjy: 质量请求变化时顺带补发本窗口独立音频状态，二者不共享所有权判定。
    if (feedbackChanged) {
        const bool backgroundRole = decision.effectiveMode != stream::RemoteQualityMode::HighQualityLocked; // wjy: 日志按实际质量角色判断后台，单窗口无焦点保持高清时不再误报background=1。
        appendViewerDebugLog(QStringLiteral("quality role host=%1 active=%2 fullscreen=%3 background=%4 audio=%5 resolution=%6 target=%7x%8 fps=%9 priority=%10 reason=%11")
            .arg(m_hostIp)
            .arg(decision.active ? 1 : 0)
            .arg(decision.fullScreen ? 1 : 0)
            .arg(backgroundRole ? 1 : 0)
            .arg(decision.audioEnabled ? 1 : 0)
            .arg(static_cast<int>(decision.resolution))
            .arg(decision.targetWidth)
            .arg(decision.targetHeight)
            .arg(decision.targetFps)
            .arg(decision.priority)
            .arg(static_cast<int>(decision.reason))); // wjy: 只在角色/档位状态变化时写data日志，额外记录目标尺寸和原因便于确认单窗口/焦点后台策略。
        requestTitleBarUpdate();
    }
}

void RemoteDesktopWindow::sendCurrentRemoteQualityDecision()
{
    if (!m_hasRemoteQualityDecision || !m_viewerHandle || m_closeInProgress) {
        return;
    }

    FsRemoteViewerQualityConfig config = {};
    config.struct_size = sizeof(config);
    config.version = 1;
    config.mode = viewerQualityModeValue(m_remoteQualityDecision.effectiveMode);
    config.target_width = static_cast<uint32_t>(qMax(0, m_remoteQualityDecision.targetWidth));
    config.target_height = static_cast<uint32_t>(qMax(0, m_remoteQualityDecision.targetHeight));
    config.target_fps = static_cast<uint32_t>(qBound(1, m_remoteQualityDecision.targetFps, 60));
    config.max_bitrate_kbps = static_cast<uint32_t>(qBound(512, m_remoteQualityDecision.maxBitrateKbps, 240000));
    config.priority = static_cast<uint32_t>(qBound(0, m_remoteQualityDecision.priority, 100));
    const quint64 currentGeneration = m_viewerGeneration.load(std::memory_order_acquire);
    if (m_hasLastSentQualityConfig
        && m_lastQualityViewerGeneration == currentGeneration
        && sameQualityPayload(m_lastSentQualityConfig, config)) {
        return; // wjy: 相同代际、相同有效参数每秒不重复发包；原因文字变化仍可独立刷新UI。
    }

    ++m_nextQualityRequestId;
    if (m_nextQualityRequestId == 0) {
        ++m_nextQualityRequestId; // wjy: 协议保留0为非法请求号，极端回绕时跳过0继续保持单调有效值。
    }
    config.request_id = m_nextQualityRequestId;
    if (!stream::StreamRuntime::instance().setViewerQuality(m_viewerHandle, config)) {
        m_qualityRequestPending = false;
        m_qualityProtocolUnavailable = true; // wjy: 旧运行库没有可选导出时只显示不支持，绝不停止当前视频和输入会话。
        requestTitleBarUpdate();
        return;
    }

    m_lastSentQualityConfig = config;
    m_hasLastSentQualityConfig = true;
    m_lastQualityViewerGeneration = currentGeneration; // wjy: 新连接代际即使参数相同也会重新发送，旧Host确认不能代替新会话。
    m_qualityRequestPending = true;
    m_qualityProtocolUnavailable = false;
}

void RemoteDesktopWindow::sendCurrentViewerAudioDecision()
{
    if (!m_hasRemoteQualityDecision || !m_viewerHandle || m_closeInProgress) {
        return;
    }
    const bool enabled = m_viewerAudioEnabled;
    const quint64 currentGeneration = m_viewerGeneration.load(std::memory_order_acquire);
    if (m_hasLastSentViewerAudioState
        && m_lastAudioViewerGeneration == currentGeneration
        && m_lastSentViewerAudioEnabled == enabled) {
        return; // wjy: 同一代际相同布尔状态不重复调用音频线程启停接口。
    }

    const bool supported = stream::StreamRuntime::instance().setViewerAudioEnabled(m_viewerHandle, enabled);
    m_lastSentViewerAudioEnabled = enabled;
    m_hasLastSentViewerAudioState = true;
    m_lastAudioViewerGeneration = currentGeneration; // wjy: 即使旧DLL缺失导出也锁存本次结果，避免一秒轮询持续尝试；新代际会重新探测。
    appendViewerDebugLog(QStringLiteral("audio state host=%1 enabled=%2 api_supported=%3 generation=%4")
        .arg(m_hostIp)
        .arg(enabled ? 1 : 0)
        .arg(supported ? 1 : 0)
        .arg(currentGeneration)); // wjy: 标题栏音频状态每次真实变化只记录一条data诊断。
}

void RemoteDesktopWindow::refreshAppliedRemoteQualityStatus()
{
    FsRemoteViewerQualityStatus status = {};
    status.struct_size = sizeof(status);
    status.version = 1;
    if (!stream::StreamRuntime::instance().viewerQualityStatus(m_viewerHandle, &status)) {
        return;
    }
    if (!m_hasLastSentQualityConfig || status.request_id != m_lastSentQualityConfig.request_id) {
        return; // wjy: 连续切换模式时只接受当前最新请求确认，迟到旧确认不覆盖标题栏实际状态。
    }
    m_appliedQualityStatus = status;
    m_hasAppliedQualityStatus = true;
    m_qualityRequestPending = false;
    m_qualityProtocolUnavailable = status.supported == 0
        || status.limitation == FSREMOTE_VIEWER_QUALITY_LIMIT_UNSUPPORTED; // wjy: Host不支持仅作为非致命反馈，远控流保持原样继续运行。
    requestTitleBarUpdate();
}

QString RemoteDesktopWindow::remoteResourceDiagnosticSummary()
{
    bool pendingBgra = false;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        pendingBgra = !m_pendingRemoteFrame.isNull(); // wjy: BGRA与纹理各自最多一帧，诊断只读取是否存在，不复制像素。
    }
    QString quality = remoteQualityStatusSummary();
    quality.replace(QLatin1Char('\n'), QStringLiteral(" | "));
    const long d3dReason = m_texturePresenter ? m_texturePresenter->lastDeviceRemovalReason() : 0;
    const QString compositor = m_unifiedCompositor
        ? QStringLiteral("%1/state=%2/layout_rev=%3/output=%4x%5 source=%6x%7 frame=%8 presented=%9 fallbacks=%10")
            .arg(RemoteWindowCompositorConfig::activePathId())
            .arg(static_cast<int>(m_unifiedCompositor->state()))
            .arg(static_cast<qulonglong>(m_unifiedCompositor->layout().revision))
            .arg(m_unifiedCompositor->layout().outputSize.width())
            .arg(m_unifiedCompositor->layout().outputSize.height())
            .arg(m_unifiedCompositor->layout().sourceSize.width())
            .arg(m_unifiedCompositor->layout().sourceSize.height())
            .arg(static_cast<qulonglong>(m_unifiedCompositor->lastFrameId()))
            .arg(static_cast<qulonglong>(m_unifiedCompositor->presentedFrameCount()))
            .arg(static_cast<qulonglong>(m_unifiedCompositor->fallbackCount()))
        : QStringLiteral("none");
    return QStringLiteral("host=%1 connected=%2 generation=%3 status=%4 bgra_pending=%5 texture_pending=%6 drain=%7 presenter_pending_replace=%8 fallback=%9 d3d=0x%10 fps=%11 encoded_mbps=%12 compositor=%13 quality={%14} stale_generation_drop=%15")
        .arg(m_hostIp)
        .arg(m_viewerHandle ? 1 : 0)
        .arg(m_viewerGeneration.load(std::memory_order_acquire))
        .arg(m_connectionStatusCode)
        .arg(pendingBgra ? 1 : 0)
        .arg(m_pendingTextureFrames.pendingCount())
        .arg(m_pendingTextureFrames.drainScheduled() ? 1 : 0)
        .arg(m_pendingTextureFrames.replacedFrameCount())
        .arg(m_softwareFallbackActive ? 1 : 0)
        .arg(static_cast<qulonglong>(static_cast<unsigned long>(d3dReason)), 0, 16)
        .arg(m_receiveFps, 0, 'f', 1)
        .arg(m_encodedMbps, 0, 'f', 2)
        .arg(compositor)
        .arg(quality)
        .arg(m_staleTextureFrameDrops.load(std::memory_order_relaxed)); // wjy: 单行快照便于20窗口soak后按host检索，迟到旧纹理与Presenter替换分开归因。
}
// ===end====

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

    cancelNetworkReconnect(); // wjy: 远程更新拥有独立重连状态机，开始更新前必须取消普通断网无限重试。
    // =====wjy====
    cancelInputScriptRecording(QStringLiteral("remote_update"));
    stopInputScriptPlayback(QStringLiteral("remote_update"), true); // wjy: 更新遮罩接管输入前先结束脚本并补发抬起，目标重启后不会继承旧按键状态。
    // ===end====
    m_remoteMouseCaptureRequested = false; // wjy: 更新会替换 Viewer 代际，旧 Host 的 relative 请求不能带入新会话。
    suspendRemoteMouseCapture(); // wjy: F2 手动意图仍保留，但更新遮罩期间必须立即释放本机光标和远端捕获。
    releaseForwardedKeys();
    m_waitingShortcutRelease = false;
    m_shortcutReleaseVirtualKeys.clear(); // wjy: 更新开始后不再等待本地组合键释放，确保键盘 hook 可以立即卸载。
    m_localShortcutReleaseKeys.clear(); // wjy: 更新遮罩不再保留自定义脚本快捷键的 Qt 松键门禁。
    setKeyboardForwardingActive(false);
    if (m_clipboardPollTimer) {
        m_clipboardPollTimer->stop();
    }

    m_remoteUpdateAvailable = false; // wjy: 请求被目标端受理后立即隐藏更新按钮，由中央遮罩接管反馈。
    m_remoteUpdateState = RemoteUpdateState::Preparing;
    if (m_inputBroadcastCoordinator) {
        m_inputBroadcastCoordinator->notifyEligibilityChanged(this); // wjy: 主控进入更新后立即关闭整组；跟随端更新只从当前可投递集合移除自身。
    }
    m_remoteUpdateFailure.clear();
    m_remoteUpdateClock.restart();
    m_nextRemoteUpdateProbeAtMs = 0;
    m_remoteUpdateProbeInProgress = false;
    m_remoteUpdateReconnectRequested = false;
    m_remoteUpdateSpinnerStep = 0;
    ++m_remoteUpdateGeneration;
    m_textureFrameActive = false;
    m_receiveFps = 0.0;
    m_encodedMbps = 0.0;
    m_frameStatsCount = 0;
    m_frameStatsBytes = 0;
    if (m_frameStatsClock.isValid()) m_frameStatsClock.restart(); // wjy: 更新等待期间清除旧的标题栏FPS/码率，避免把上一轮连接显示成当前实时状态。
    if (m_texturePresenter) {
        m_texturePresenter->setPresentationVisible(false); // wjy: 旧路径隐藏子HWND，新路径把DComp视频视觉透明化，中央遮罩不会被视频盖住。
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
    if (m_texturePresenter && m_texturePresenter->usesCompositorSurface()) {
        presentCompositorOverlay(); // wjy: 更新受理后立即用更新卡片替换连接提示，不等待下一次250ms定时器。
    }
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
    // =====wjy====
    const auto probeTask = [self, targetIp, updateGeneration] {
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
            if (window->m_texturePresenter && window->m_texturePresenter->usesCompositorSurface()) {
                window->presentCompositorOverlay(); // wjy: 查询线程返回的新阶段同步进入DComp，失败后即使定时器停止也能显示最终错误卡片。
            }
        }, Qt::QueuedConnection);
    };
    if (!m_lifecycleManager || !m_lifecycleManager->submitLifecycleTask(probeTask)) {
        m_remoteUpdateProbeInProgress = false; // wjy: 退出阶段队列已封闭时恢复探测标志，不再遗留一个永远“查询中”的窗口状态。
        m_nextRemoteUpdateProbeAtMs = 0;
    }
    // ===end====
}

void RemoteDesktopWindow::stopViewerConnectionAsync(bool deleteAfterStop)
{
    if (deleteAfterStop) {
        m_deleteAfterViewerStop = true;
    }
    if (m_viewerStopInProgress) {
        return;
    }

    // =====wjy====
    if (m_viewerStartQueued && m_lifecycleManager) {
        m_lifecycleManager->cancelViewerStart(this, false); // wjy: 尚未获得初始化名额的窗口关闭时直接从FIFO移除，不再启动一个马上要停止的Viewer。
        m_viewerStartQueued = false;
    }
    m_remoteMouseCaptureRequested = false; // wjy: 任意 Viewer 停止都会作废旧 Host 的自动 relative 请求，重连后必须由新代际重新上报。
    suspendRemoteMouseCapture(); // wjy: 在清空句柄前发送 CaptureRelease，并释放本机 Raw Input；F2 手动意图可在新会话恢复后重新生效。
    // ===end====
    const FsRemoteStreamHandle handle = m_viewerHandle;
    m_viewerHandle = nullptr;
    invalidateViewerCallbacks(); // wjy: 在启动停止线程前先推进代际，stop阻塞期间到达的旧视频和状态回调全部被窗口拒绝。
    auto callbackContext = std::move(m_viewerCallbackContext); // wjy: 回调上下文的生命周期转交给停止任务，原生Viewer析构完成前地址始终有效。
    if (!handle) {
        // =====wjy====
        releaseViewerStartupAdmission(); // wjy: 没有原生句柄说明初始化尚未开始或已经失败，可立即释放启动预算。
        if (!m_applicationExitInProgress && (m_deleteAfterViewerStop || m_closeInProgress)) {
            deleteLater();
        } else if (m_remoteUpdateReconnectRequested) {
            startViewerAfterUpdate();
        } else if (m_networkReconnectActive) {
            scheduleNetworkReconnect(); // wjy: 同步创建失败没有原生对象可清理，直接进入下一档退避且继续无限等待。
        }
        // ===end====
        return;
    }

    // =====wjy====
    m_viewerStopInProgress = true;
    QPointer<RemoteDesktopWindow> self(this);
    const auto stopTask = [self, handle, callbackContext = std::move(callbackContext)] {
        QString stopError;
        try {
            stream::StreamRuntime::instance().stop(handle); // wjy: 固定工作池执行可能阻塞的DLL停止，Qt线程和其它19个窗口继续响应。
        } catch (const std::exception& exception) {
            stopError = QString::fromUtf8(exception.what()); // wjy: 将单窗口stop异常转成可恢复状态，禁止异常越过线程入口终止整个进程。
        } catch (...) {
            stopError = QString::fromUtf8("停止远控连接时发生未知异常"); // wjy: 未知异常同样只归属当前Viewer，不影响其它会话。
        }
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, stopError] {
            if (!self) {
                return;
            }
            self->finishViewerStop(stopError); // wjy: 所有Qt状态变更集中回主线程，工作线程不直接触碰窗口成员。
        }, Qt::QueuedConnection);
    };
    if (m_lifecycleManager
        && m_lifecycleManager->submitLifecycleTask(
            stopTask,
            RemoteViewerLifecycleManager::LifecycleTaskPriority::Cleanup)) {
        return; // wjy: 正常路径由共享管理器持有任务并在应用退出时统一join。
    }

    QString stopError;
    try {
        stream::StreamRuntime::instance().stop(handle); // wjy: 仅当管理器已封闭或缺失时同步兜底，确保句柄绝不因任务拒绝而泄漏。
    } catch (const std::exception& exception) {
        stopError = QString::fromUtf8(exception.what());
    } catch (...) {
        stopError = QString::fromUtf8("停止远控连接时发生未知异常");
    }
    finishViewerStop(stopError);
    // ===end====
}

void RemoteDesktopWindow::finishViewerStop(const QString& errorMessage)
{
    // =====wjy====
    m_viewerStopInProgress = false;
    releaseViewerStartupAdmission(); // wjy: 初始化阶段被关闭的窗口把名额保留到原生stop真正返回，避免短时间突破4路硬上限。
    if (!errorMessage.trimmed().isEmpty() && !m_closeInProgress && !m_applicationExitInProgress) {
        setConnectionStatus(90, QString::fromUtf8("停止远控连接失败：%1").arg(errorMessage.trimmed())); // wjy: stop异常只显示在当前窗口，其他窗口和进程继续运行。
    }
    if (!m_applicationExitInProgress && (m_deleteAfterViewerStop || m_closeInProgress)) {
        deleteLater();
        return;
    }
    if (m_remoteUpdateReconnectRequested) {
        startViewerAfterUpdate(); // wjy: stop返回后再申请新的初始化名额，旧回调和新会话不会重叠。
    } else if (m_networkReconnectActive) {
        scheduleNetworkReconnect(); // wjy: 只有旧Viewer析构和工作线程join完成后才允许启动新代际。
    }
    // ===end====
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
    if (m_texturePresenter && m_texturePresenter->usesCompositorSurface()) {
        presentCompositorOverlay(); // wjy: 重连阶段切换后立即更新标题栏和中央卡片，清除旧连接状态快照。
    }
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
    if (m_inputBroadcastCoordinator) {
        m_inputBroadcastCoordinator->notifyEligibilityChanged(this); // wjy: 更新完成后恢复资格；若同步仍由其它窗口主控，本窗口从下一事件起自动重新跟随。
    }
    update();
    if (m_texturePresenter && m_texturePresenter->usesCompositorSurface()) {
        presentCompositorOverlay(); // wjy: 首帧确认更新结束时立即移除更新卡片，恢复标题栏实时状态。
    }
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
    if (m_pendingSnapGeometries == geometries) {
        return; // wjy: 系统移动循环可能连续报告同一候选，几何未变化时不重复show/raise顶层预览窗。
    }
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
        if (preview->geometry() != it.value()) {
            preview->setGeometry(it.value()); // wjy: 只有候选矩形变化才调用原生SetWindowPos，减少DWM合成扰动。
        }
        if (preview->isHidden()) {
            preview->show();
            preview->raise(); // wjy: 预览从隐藏切为可见时只提升一次，同一吸附候选内不再逐事件改变Z序。
        }
    }
    for (; previewIndex < m_snapPreviews.size(); ++previewIndex) {
        m_snapPreviews.at(previewIndex)->hide();
    }
}

void RemoteDesktopWindow::clearSnapPreviews()
{
    if (m_pendingSnapGeometries.isEmpty()) {
        return; // wjy: 未显示候选时不重复遍历和隐藏顶层预览窗。
    }
    for (QRubberBand* preview : m_snapPreviews) {
        if (preview) preview->hide();
    }
    m_pendingSnapGeometries.clear(); // wjy: 拖离吸附范围时同时撤销整组待提交数据，避免释放后应用过期布局。
}

// =====wjy====
bool RemoteDesktopWindow::startSystemWindowMove()
{
    if (m_systemWindowOperationAttempted) {
        return m_systemWindowOperationActive;
    }
    m_systemWindowOperationAttempted = true;
    QWindow* nativeWindow = windowHandle();
    m_systemWindowOperationActive = nativeWindow && nativeWindow->startSystemMove(); // wjy: 成功后由Windows/DWM移动现有合成表面，Qt不再逐像素调用move。
    return m_systemWindowOperationActive;
}

bool RemoteDesktopWindow::startSystemWindowResize()
{
    if (m_systemWindowOperationAttempted) {
        return m_systemWindowOperationActive; // wjy: 同一手势只请求一次系统缩放；失败后稳定走现有手动回退，禁止鼠标移动时反复重试。
    }
    m_systemWindowOperationAttempted = true;

    Qt::Edges nativeEdges = {}; // wjy: QFlags的默认构造不依赖未初始化栈内存，先明确从无边缘开始再逐项加入方向。
    if (m_resizeEdges & ResizeLeft) nativeEdges |= Qt::LeftEdge; // wjy: 把现有四方向位掩码逐项映射到QWindow原生缩放边缘。
    if (m_resizeEdges & ResizeRight) nativeEdges |= Qt::RightEdge;
    if (m_resizeEdges & ResizeTop) nativeEdges |= Qt::TopEdge;
    if (m_resizeEdges & ResizeBottom) nativeEdges |= Qt::BottomEdge;

    QWindow* nativeWindow = windowHandle();
    m_systemWindowOperationActive = nativeWindow
        && nativeEdges != Qt::Edges()
        && nativeWindow->startSystemResize(nativeEdges); // wjy: 成功后Windows接管鼠标捕获与窗口尺寸循环，Qt不再逐事件调用顶层setGeometry。
    return m_systemWindowOperationActive;
}

void RemoteDesktopWindow::updateSnapPreviewForGeometry(
    const QRect& proposedGeometry,
    const QPoint& cursorGlobal)
{
    const SnapGeometryResult snapResult = snappedDraggedWindowGeometry(
        this,
        proposedGeometry,
        cursorGlobal,
        remoteFrameSize(),
        titleBarHeight(),
        minimumSize()); // wjy: 系统移动和手动回退读取同一鼠标优先吸附结果，最终布局不会因移动实现不同而改变。
    if (snapResult.snapped) {
        showSnapPreviews(snapResult.geometries);
    } else {
        clearSnapPreviews();
    }
}

void RemoteDesktopWindow::finishInteractiveWindowOperation()
{
    const bool wasDraggingWindow = m_draggingWindow;
    const bool wasResizingWindow = m_resizingWindow;
    if (!wasDraggingWindow && !wasResizingWindow) {
        return;
    }

    const QHash<RemoteDesktopWindow*, QRect> snapGeometries = m_pendingSnapGeometries; // wjy: 在清理预览前保留系统移动循环最终确认的整组吸附矩形。
    clearSnapPreviews();
    m_draggingWindow = false;
    m_dragRestorePending = false;
    m_resizingWindow = false;
    m_resizeEdges = ResizeNone;
    m_systemWindowOperationActive = false;
    m_systemWindowOperationAttempted = false;

    if (wasResizingWindow) {
        appendResizeDebugTrace(QStringLiteral("gesture.finish.begin")); // wjy: 在释放状态清理前记录最后一次父/子窗口和SwapChain快照。
        if (m_texturePresenter) {
            m_texturePresenter->setInteractiveResize(false); // wjy: 松手后仅按最终客户区调整一次SwapChain，并立即用拖拽期间缓存的最新帧补画。
        }
        updateWindowMask(); // wjy: 系统缩放结束后仅按最终尺寸恢复一次圆角Region。
        updateTexturePresenterGeometry();
        if (m_unifiedCompositor && m_unifiedCompositor->isEnabled()) {
            m_unifiedCompositor->finalizeResize(compositorLayoutSnapshot()); // wjy: 最终几何、DPI和输入区域提交后才退出交互缩放状态。
        }
        appendResizeDebugTrace(QStringLiteral("gesture.finish.after_final_geometry")); // wjy: 确认最终尺寸调整和补帧完成后再写出整手势日志。
        flushResizeDebugTrace();
    }

    if (wasDraggingWindow && !snapGeometries.isEmpty()) {
        const QList<QWidget*> topLevelWindows = QApplication::topLevelWidgets();
        for (auto it = snapGeometries.cbegin(); it != snapGeometries.cend(); ++it) {
            if (it.key() && topLevelWindows.contains(it.key()) && it.value().isValid()) {
                it.key()->setGeometry(it.value()); // wjy: 系统移动完成后一次性应用吸附结果，不在移动过程中改写真实窗口尺寸。
            }
        }
        for (auto it = snapGeometries.cbegin(); it != snapGeometries.cend(); ++it) {
            if (it.key() && topLevelWindows.contains(it.key())) {
                it.key()->saveWindowGeometry();
            }
        }
    }

    if (wasDraggingWindow) {
        updateWindowMask(); // wjy: 旧Windows拖动期间临时移除的QRegion只在最终位置恢复；Windows 11继续沿用DWM原生圆角。
    }

    saveWindowGeometry();
    m_hoveredPos = mapFromGlobal(QCursor::pos());
    if (wasDraggingWindow && m_windowPaintingSuspendedForMove) {
        m_windowPaintingSuspendedForMove = false;
        setUpdatesEnabled(true); // wjy: 窗口和吸附几何完全稳定后再恢复父QWidget绘制，Qt只提交一次最终标题栏表面。
    }
    requestTitleBarUpdate(); // wjy: 交互过程中被抑制的标题栏状态只在最终位置和宽度稳定后完整提交一次。
    updateResizeCursor(m_hoveredPos);
}
// ===end====

// =====wjy====
bool RemoteDesktopWindow::restoreSavedGeometryForDrag(
    const QPoint& cursorGlobal,
    const QPoint& pressedPosition)
{
    const bool temporaryWindowLayout = isMaximized() || isFullScreen() || !m_rememberGeometry;
    if (!temporaryWindowLayout) {
        return false; // wjy: 普通窗口继续沿用当前几何，不重复读取 JSON 或改变用户正在使用的尺寸。
    }

    QScreen* cursorScreen = QGuiApplication::screenAt(cursorGlobal);
    if (!cursorScreen) {
        cursorScreen = screen();
    }
    if (!cursorScreen) {
        cursorScreen = QGuiApplication::primaryScreen(); // wjy: 多显示器边界无法定位时回退当前窗口或主屏，保证恢复尺寸始终有可用范围。
    }
    const QRect availableGeometry = cursorScreen
        ? cursorScreen->availableGeometry()
        : QRect(0, 0, 1280, 720);

    const QRect savedGeometry = normalizedSavedWindowGeometry(
        platform::AppSettings::remoteDesktopWindowGeometry(m_hostIp),
        minimumSize());
    QSize restoredSize = savedGeometry.isValid()
        ? savedGeometry.size()
        : QSize(1280, 720); // wjy: 已保存设备严格使用 JSON 宽高；首次设备没有记录时使用和构造阶段一致的安全默认尺寸。
    restoredSize.setWidth(qMin(
        qMax(minimumWidth(), restoredSize.width()),
        qMax(minimumWidth(), availableGeometry.width())));
    restoredSize.setHeight(qMin(
        qMax(minimumHeight(), restoredSize.height()),
        qMax(minimumHeight(), availableGeometry.height()))); // wjy: JSON 尺寸超过当前屏幕时只在本次恢复中收敛，不改写原配置值。

    const qreal horizontalRatio = width() > 0
        ? qBound<qreal>(0.0, pressedPosition.x() / qreal(width()), 1.0)
        : 0.5;
    const int grabX = qBound(0, qRound(restoredSize.width() * horizontalRatio), qMax(0, restoredSize.width() - 1));
    const int grabY = qBound(0, pressedPosition.y(), qMax(0, qMin(27, restoredSize.height() - 1))); // wjy: 横向按原抓取比例定位，纵向保持在恢复后的标题栏内，窗口不会从鼠标下突然跳开。

    const int maximumLeft = qMax(availableGeometry.left(), availableGeometry.right() - restoredSize.width() + 1);
    const int maximumTop = qMax(availableGeometry.top(), availableGeometry.bottom() - restoredSize.height() + 1);
    const QPoint restoredTopLeft(
        qBound(availableGeometry.left(), cursorGlobal.x() - grabX, maximumLeft),
        qBound(availableGeometry.top(), cursorGlobal.y() - grabY, maximumTop));

    if (isMaximized() || isFullScreen()) {
        showNormal(); // wjy: 先退出 Qt 最大化/全屏状态，否则后续 setGeometry 仍可能被窗口管理器的全屏矩形覆盖。
    }
    setGeometry(QRect(restoredTopLeft, restoredSize)); // wjy: 位置跟随鼠标重新计算，但宽高只取当前设备 JSON 中保存的普通窗口尺寸。
    m_rememberGeometry = true; // wjy: 用户主动把平铺窗口拖回普通状态后，释放鼠标即可保存新的普通位置和仍然相同的 JSON 尺寸。
    updateWindowMask();
    updateTexturePresenterGeometry();
    return true;
}
// ===end====

RemoteTitleBarLayoutSnapshot RemoteDesktopWindow::titleBarLayoutSnapshot() const
{
    const RemoteTitleBarIdentityLayout identity = remoteTitleBarIdentityLayout(width(), m_deviceName, m_hostIp);
    return ::ui::remoteTitleBarLayoutSnapshot(
        width(), titleBarHeight(), identity.right, m_remoteUpdateAvailable, kWindowResizeMargin); // wjy: 同一快照同时约束绘制和点击，任何被设备信息覆盖的控件都返回空矩形。
}

// =====wjy====
RemoteTitleBarVisualState RemoteDesktopWindow::titleBarVisualState() const
{
    RemoteTitleBarVisualState state;
    state.logicalWidth = width();
    state.logicalHeight = titleBarHeight();
    state.devicePixelRatio = devicePixelRatioF();
    state.layout = titleBarLayoutSnapshot();
    state.hoveredPosition = m_hoveredPos;
    state.deviceName = m_deviceName;
    state.hostIp = m_hostIp;
    state.connectionStatus = m_connectionStatus;
    state.networkWarningText = m_networkWarningVisible ? zh("网络不佳") : QString(); // wjy: 原生DIB和DComp完整标题栏都从同一快照取得静态网络提示。
    // =====wjy====
    if (m_inputScriptRecording) {
        state.scriptStatusText = QString::fromUtf8("录制中");
        state.scriptStatusColor = QColor(QStringLiteral("#DC2626")); // wjy: 录制使用稳定红色静态文字，不闪烁也不依赖一秒性能统计刷新。
    } else if (m_inputScriptPlaying) {
        state.scriptStatusText = QString::fromUtf8("脚本播放");
        state.scriptStatusColor = QColor(QStringLiteral("#2563EB")); // wjy: 回放使用稳定蓝色静态文字，与录制状态互斥且由F10停止后立即清除。
    }
    // ===end====
    state.updateAvailable = m_remoteUpdateAvailable; // wjy: 即使当前视觉由layout.update表示，快照仍完整记录标题栏更新入口来源状态，便于后续渲染演进不回读窗口。

    const qint64 elapsedSeconds = m_sessionClock.elapsed() / 1000;
    state.elapsedText = QStringLiteral("%1:%2:%3")
        .arg(elapsedSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((elapsedSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(elapsedSeconds % 60, 2, 10, QLatin1Char('0')); // wjy: 会话计时在状态快照阶段格式化，原生绘制器不读取窗口时钟。
    if (m_connectionStatusCode == FSREMOTE_STATUS_RECEIVING_VIDEO && !remoteUpdateActive()) {
        const QString fpsText = m_receiveFps > 0.0
            ? QString::number(qRound(m_receiveFps))
            : QStringLiteral("--");
        const QString bitrateText = m_encodedMbps > 0.0
            ? QString::number(qRound(m_encodedMbps))
            : QStringLiteral("--");
        state.performanceText = QStringLiteral("%1 FPS | %2 Mbps")
            .arg(fpsText)
            .arg(bitrateText); // wjy: 标题栏仅在收到视频时显示实际接收FPS和压缩码率。
    }

    const RemoteTitleBarIdentityLayout identity = remoteTitleBarIdentityLayout(width(), m_deviceName, m_hostIp);
    state.identityShowLogo = identity.showLogo;
    state.identityFont = identity.font;
    state.identityTextX = identity.textX;
    state.identityNameWidth = identity.nameWidth;
    state.identityIpX = identity.ipX;
    state.identityIpWidth = identity.ipWidth;
    state.identityRight = identity.right;

    state.mouseBackendAccent = m_remoteMouseBackend == RemoteMouseBackend::Faker
        ? QColor(QStringLiteral("#0F766E"))
        : QColor(QStringLiteral("#3A7BFC"));
    if (m_remoteMouseBackendFallback) state.mouseBackendAccent = QColor(QStringLiteral("#D97706"));
    if (m_remoteMouseBackendPending) state.mouseBackendAccent = QColor(QStringLiteral("#64748B"));
    state.mouseBackendPressed = m_mouseBackendButtonPressed;
    state.mouseBackendText = (m_remoteMouseBackendPending ? m_pendingRemoteMouseBackend : m_remoteMouseBackend)
            == RemoteMouseBackend::Faker
        ? zh("驱动")
        : zh("系统");
    if (m_remoteMouseBackendPending) {
        state.mouseBackendText += QString::fromUtf8("…");
    } else if (m_remoteMouseBackendFallback) {
        state.mouseBackendText += QLatin1Char('!');
    } else if (!m_remoteMouseBackendKnown) {
        state.mouseBackendText += QLatin1Char('?');
    }
    if (m_manualMouseLockActive) {
        state.mouseLockText = QString::fromUtf8("鼠标锁定");
        state.mouseLockColor = QColor(QStringLiteral("#DC2626")); // wjy: 仅显示 F2 手动意图；游戏自动 relative 不额外占用标题栏状态文字。
    }

    // wjy: 标题栏快照不再携带画质胶囊状态，避免把只读策略信息误认为手动操作入口。
    state.inputSyncText = QString::fromUtf8("同步");
    state.inputSyncAccent = QColor(QStringLiteral("#98A2B3"));
    state.inputSyncBackground = QColor(QStringLiteral("#F2F4F7"));
    if (m_inputSyncRole == RemoteInputSyncRole::Master) {
        state.inputSyncText = QString::fromUtf8("主控");
        state.inputSyncAccent = QColor(QStringLiteral("#2563EB"));
        state.inputSyncBackground = QColor(QStringLiteral("#E8F1FF"));
    } else if (m_inputSyncRole == RemoteInputSyncRole::Follower) {
        state.inputSyncText = QString::fromUtf8("跟随");
        state.inputSyncAccent = QColor(QStringLiteral("#D97706"));
        state.inputSyncBackground = QColor(QStringLiteral("#FFF3D6"));
    } else if (m_inputSyncRole == RemoteInputSyncRole::Excluded) {
        state.inputSyncText = QString::fromUtf8("已停");
        state.inputSyncAccent = QColor(QStringLiteral("#64748B"));
        state.inputSyncBackground = QColor(QStringLiteral("#E2E8F0"));
    }
    state.inputSyncPressed = m_inputSyncButtonPressed;
    state.audioEnabled = m_viewerAudioEnabled;
    state.clipboardEnabled = m_clipboardSyncEnabled;
    return state;
}

void RemoteDesktopWindow::updateNativeTitleBarSurfaceGeometry()
{
    if (!m_nativeTitleBarSurface || !m_nativeTitleBarSurface->isCreated()) return;
    if (m_texturePresenter && m_texturePresenter->usesCompositorSurface()) {
        presentCompositorOverlay();
        if (m_texturePresenter->hasCompositorOverlay()) {
            m_nativeTitleBarSurface->setVisible(false); // wjy: 叠加SwapChain可用时旧原生表面始终保持隐藏。
            return;
        }
    }
    if (isFullScreen()) {
        m_nativeTitleBarSurface->setVisible(false); // wjy: 全屏完全移除本地标题栏子HWND，远控内容占用整个客户区。
        return;
    }
    m_nativeTitleBarSurface->setLogicalGeometry(
        QRect(0, 0, width(), titleBarHeight()), devicePixelRatioF()); // wjy: 宽度固定按虚拟屏预留，窗口缩放永远命中内部去重直接返回，不产生SetWindowPos。
    m_nativeTitleBarSurface->setVisible(true);
    m_nativeTitleBarSurface->raise();
}

// =====wjy====
void RemoteDesktopWindow::updateNativeTitleBarButtonOrigin()
{
    if (!m_nativeTitleBarSurface || !m_nativeTitleBarSurface->isCreated() || isFullScreen()) return;

    const RemoteTitleBarButtonGroupGeometry group =
        remoteTitleBarButtonGroupGeometry(titleBarLayoutSnapshot(), width());
    if (group.width <= 0) {
        return; // wjy: 窄窗口把全部按钮挤出时保持上一次合成结果，由后续按新可见集合的重绘处理。
    }
    m_nativeTitleBarSurface->setButtonGroupOrigin(group.left, devicePixelRatioF()); // wjy: 缩放期间唯一的操作，只在合成缓冲内平移按钮段，不触碰任何HWND。
}

void RemoteDesktopWindow::updateNativeTitleBarButtonBand(bool forceRender)
{
    if (!m_nativeTitleBarSurface || !m_nativeTitleBarSurface->isCreated() || isFullScreen()) return;

    const RemoteTitleBarVisualState state = titleBarVisualState();
    const RemoteTitleBarButtonGroupGeometry group =
        remoteTitleBarButtonGroupGeometry(state.layout, width());
    if (group.width <= 0) return;

    const qreal dpr = devicePixelRatioF();
    const int barHeight = titleBarHeight();
    const bool changed = forceRender
        || m_committedButtonVisualRevision != m_titleBarVisualRevision
        || m_committedButtonVisibleSignature != group.visibleSignature
        || m_committedButtonGroupWidth != group.width
        || m_committedButtonBarHeight != barHeight
        || !qFuzzyCompare(m_committedButtonDevicePixelRatio, dpr); // wjy: 组宽度只随可见按钮集合变化，因此普通缩放不会命中任何条件。
    if (!changed) {
        updateNativeTitleBarButtonOrigin();
        return;
    }

    const QImage image = RemoteTitleBarRenderer::renderButtonGroup(
        state,
        group.localLayout,
        group.width,
        m_hoveredPos.x() >= 0 ? QPoint(m_hoveredPos.x() - group.left, m_hoveredPos.y()) : QPoint(-1, -1)); // wjy: 悬停位置换算到组局部坐标，命中判断仍由窗口坐标的layout快照负责。
    if (!image.isNull() && m_nativeTitleBarSurface->commitButtonGroup(image)) {
        m_committedButtonVisualRevision = m_titleBarVisualRevision;
        m_committedButtonVisibleSignature = group.visibleSignature;
        m_committedButtonGroupWidth = group.width;
        m_committedButtonBarHeight = barHeight;
        m_committedButtonDevicePixelRatio = dpr;
    }
    updateNativeTitleBarButtonOrigin();
}
// ===end====

void RemoteDesktopWindow::updateNativeTitleBarSurface(bool forceRender)
{
    if (m_texturePresenter && m_texturePresenter->usesCompositorSurface()) {
        presentCompositorOverlay();
        if (m_texturePresenter->hasCompositorOverlay()) {
            if (m_nativeTitleBarSurface && m_nativeTitleBarSurface->isCreated()) {
                m_nativeTitleBarSurface->setVisible(false); // wjy: 叠加SwapChain就绪后才隐藏旧DIB HWND，避免初始化失败时标题栏消失。
            }
            return;
        }
    }
    if (!m_nativeTitleBarSurface || !m_nativeTitleBarSurface->isCreated()) return;
    if (m_draggingWindow) {
        return; // wjy: 移动手势不改变任何标题栏尺寸，完全冻结两层即可。
    }
    if (isFullScreen()) {
        updateNativeTitleBarSurfaceGeometry();
        return;
    }

    const qreal dpr = devicePixelRatioF();
    const int barHeight = titleBarHeight();
    const RemoteTitleBarVisualState state = titleBarVisualState();
    // wjy: 身份层按虚拟屏宽度一次性渲染，并用身份布局的计算结果而不是窗口宽度做去重签名。
    // 只有窄窗口跨过隐藏Logo或缩小字号的阈值时才需要重绘，普通缩放完全不触发。
    const QSize identitySignature(
        state.identityRight * 4 + (state.identityShowLogo ? 2 : 0) + (state.hostIp.isEmpty() ? 1 : 0),
        state.identityFont.pixelSize());
    const bool changed = forceRender
        || m_committedTitleBarVisualRevision != m_titleBarVisualRevision
        || m_committedTitleBarLogicalSize != identitySignature
        || m_committedTitleBarBarHeight != barHeight
        || !qFuzzyCompare(m_committedTitleBarDevicePixelRatio, dpr);
    if (!changed) {
        updateNativeTitleBarSurfaceGeometry();
        updateNativeTitleBarButtonBand(false);
        return;
    }

    const QImage image = RemoteTitleBarRenderer::renderIdentityBand(state, nativeTitleBarBandLogicalWidth());
    if (!image.isNull() && m_nativeTitleBarSurface->commitIdentityBand(image)) {
        m_committedTitleBarVisualRevision = m_titleBarVisualRevision;
        m_committedTitleBarLogicalSize = identitySignature;
        m_committedTitleBarBarHeight = barHeight;
        m_committedTitleBarDevicePixelRatio = dpr; // wjy: 只有完整图像成功进入DIB后才提交版本，失败时原生表面继续显示上一帧。
    }
    updateNativeTitleBarSurfaceGeometry(); // wjy: 首帧先完成DIB提交再显示子窗口，普通更新则保持旧前台表面直到新帧复制完成。
    updateNativeTitleBarButtonBand(forceRender);
}

void RemoteDesktopWindow::presentCompositorOverlay()
{
    if (!m_texturePresenter
        || !m_texturePresenter->usesCompositorSurface()
        || width() <= 0
        || height() <= 0) {
        return;
    }

    const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
    const QSize physicalSize(
        qMax(1, qRound(width() * dpr)),
        qMax(1, qRound(height() * dpr)));
    QImage overlay(physicalSize, QImage::Format_ARGB32_Premultiplied);
    overlay.fill(Qt::transparent);

    QPainter painter(&overlay);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(dpr, dpr);
    const QRect contentRect = remoteContentRect();
    const QRect imageRect = remoteImageRect();
    if (!contentRect.isEmpty()) {
        painter.fillRect(contentRect, Qt::black); // wjy: 黑边按当前窗口内容区域重新生成，不再依赖旧硬件SwapChain中的黑色像素。
        if (imageRect.isValid()) {
            painter.setCompositionMode(QPainter::CompositionMode_Clear);
            painter.fillRect(imageRect, Qt::transparent); // wjy: 清出当前真实视频区域，让DComp视频视觉只覆盖内容，不被叠加层黑底遮挡。
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
    }

    if (!m_textureFrameActive && !m_remoteFrame.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(remoteImageRect(), m_remoteFrame); // wjy: 软件BGRA回退也进入同一DComp叠加SwapChain，不再依赖父backing store作为最终像素所有者。
    } else if (!m_textureFrameActive && m_remoteFrame.isNull() && !remoteUpdateActive()) {
        const QRect contentRect(0, isFullScreen() ? 0 : titleBarHeight(), width(),
            qMax(0, height() - (isFullScreen() ? 0 : titleBarHeight())));
        QFont titleFont(QStringLiteral("Microsoft YaHei UI"));
        titleFont.setPixelSize(18);
        titleFont.setWeight(QFont::DemiBold);
        painter.setFont(titleFont);
        painter.setPen(QColor(QStringLiteral("#FFFFFF")));
        painter.drawText(
            QRectF(contentRect.left(), contentRect.center().y() - 34, contentRect.width(), 26),
            Qt::AlignCenter,
            zh("正在连接 %1").arg(m_deviceName)); // wjy: DComp连接主标题使用固定中央文本行，与期望界面和Qt回退路径保持完全一致。
        QFont statusFont(QStringLiteral("Microsoft YaHei UI"));
        statusFont.setPixelSize(13);
        painter.setFont(statusFont);
        painter.setPen(m_connectionStatusCode == 90
                ? QColor(QStringLiteral("#FFB4B4"))
                : QColor(QStringLiteral("#B8C0CC")));
        painter.drawText(
            QRectF(contentRect.left() + 60, contentRect.center().y(), contentRect.width() - 120, 48),
            Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
            m_connectionStatus); // wjy: 副标题从中央主标题下方开始绘制，禁止再次使用内容区顶部导致文字跑到标题栏下方。
    }

    if (!isFullScreen()) {
        RemoteTitleBarVisualState titleState = titleBarVisualState();
        titleState.devicePixelRatio = 1.0;
        const QImage titleBar = RemoteTitleBarRenderer::render(titleState);
        if (!titleBar.isNull()) {
            painter.drawImage(QRect(0, 0, width(), titleBarHeight()), titleBar);
        }
    }

    if (remoteUpdateActive()) {
        const QRect contentRect(0, isFullScreen() ? 0 : titleBarHeight(), width(),
            qMax(0, height() - (isFullScreen() ? 0 : titleBarHeight())));
        painter.fillRect(contentRect, QColor(3, 10, 22, 196)); // wjy: DComp路径完整接管更新遮罩，父QWidget不再叠加第二套连接或更新文字。

        const int cardWidth = qMin(380, qMax(240, contentRect.width() - 48));
        const int cardHeight = 136;
        const QRect card(
            contentRect.center().x() - cardWidth / 2,
            contentRect.center().y() - cardHeight / 2,
            cardWidth,
            cardHeight);
        painter.setPen(QPen(QColor(255, 255, 255, 28), 1));
        painter.setBrush(QColor(17, 27, 45, 238));
        painter.drawRoundedRect(QRectF(card), 12, 12); // wjy: 更新卡片与旧Qt回退路径保持同一尺寸和配色，切换显示所有权时不会跳变。

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
            painter.drawArc(spinnerRect, startAngle, -210 * 16); // wjy: 合成层按同一250ms状态步进刷新圆环，DComp模式不再显示静止的更新文字。
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
    painter.end();
    overlay.setDevicePixelRatio(dpr); // wjy: 标记叠加图实际物理像素对应的顶层窗口DPR，供DComp按正确比例铺满整个客户区。
    m_texturePresenter->presentCompositorOverlay(overlay);
    sampleVisibleCompositorRegion(); // wjy: 叠加层提交后按需采样真实屏幕像素，和本次resize状态绑定。
}

// =====wjy====
int RemoteDesktopWindow::nativeTitleBarBandLogicalWidth() const
{
#if defined(Q_OS_WIN)
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
    const int physical = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    if (physical > 0) {
        return std::max(width(), static_cast<int>(std::ceil(physical / dpr))); // wjy: 身份层覆盖整个虚拟屏逻辑宽度，窗口横向拉长时不需要重新渲染即可继续提供底色。
    }
#endif
    return std::max(width(), 4096);
}
// ===end====
// ===end====

// =====wjy====
QRect RemoteDesktopWindow::titleBarHoverRectAt(const QPoint& position) const
{
    if (isFullScreen() || position.y() < 0 || position.y() >= titleBarHeight()) {
        return {};
    }
    const RemoteTitleBarLayoutSnapshot layout = titleBarLayoutSnapshot();
    for (const QRect& control : {layout.update, layout.mouseBackend,
             layout.inputSync, layout.audio, layout.clipboard, layout.minimize, layout.close}) {
        if (!control.isEmpty() && control.contains(position)) {
            return control; // wjy: 只把实际可见按钮视为悬停区域，设备文字和标题栏空白区移动不会产生绘制请求。
        }
    }
    return {};
}

void RemoteDesktopWindow::updateTitleBarHover(const QPoint& position)
{
    const QRect previousHoverRect = titleBarHoverRectAt(m_hoveredPos);
    const QRect nextHoverRect = titleBarHoverRectAt(position);
    m_hoveredPos = position;
    if (previousHoverRect == nextHoverRect) {
        return; // wjy: 鼠标在同一按钮或同一空白区域内移动时视觉没有变化，不重复触发paintEvent。
    }
    requestTitleBarUpdate(previousHoverRect.united(nextHoverRect)); // wjy: 仅清理旧按钮并点亮新按钮，避免普通鼠标移动刷新整条标题栏。
}

void RemoteDesktopWindow::requestTitleBarUpdate(const QRect& region)
{
#if !FSREMOTE_LEGACY_PARENT_TITLE_BAR
    if (m_texturePresenter && m_texturePresenter->usesCompositorSurface()) {
        Q_UNUSED(region)
        ++m_titleBarVisualRevision;
        if (!m_draggingWindow) {
            presentCompositorOverlay(); // wjy: DComp路径只重新提交统一Alpha层，标题栏状态变化不会再触发独立HWND重绘。
        }
        if (m_texturePresenter->hasCompositorOverlay()) {
            return;
        }
    }
#endif
#if FSREMOTE_LEGACY_PARENT_TITLE_BAR
    if (isFullScreen() || m_draggingWindow || m_resizingWindow) return;
    const QRect titleRect(0, 0, width(), titleBarHeight());
    const QRect dirtyRect = region.isEmpty() ? titleRect : region.intersected(titleRect);
    if (!dirtyRect.isEmpty()) update(dirtyRect); // wjy: 迁移回退开关启用时完整恢复旧Qt父窗口局部标题栏刷新路径。
#else
    Q_UNUSED(region)
    if (isFullScreen()) {
        updateNativeTitleBarSurfaceGeometry();
        return;
    }
    ++m_titleBarVisualRevision; // wjy: 先记录所有可见状态变化；系统移动期间延迟提交，交互缩放继续原子替换同一个持久DIB。
    if (m_draggingWindow) {
        return; // wjy: 系统移动期间窗口尺寸不变，状态只排队到收尾；交互缩放则继续原子提交到同一个原生标题栏表面。
    }
    updateNativeTitleBarSurface();
#endif
}
// ===end====

QRect RemoteDesktopWindow::remoteUpdateButtonRect() const
{
    return titleBarLayoutSnapshot().update;
}

QRect RemoteDesktopWindow::mouseInputModeRect() const
{
    return titleBarLayoutSnapshot().mouseBackend;
}

QRect RemoteDesktopWindow::clipboardSyncRect() const
{
    return titleBarLayoutSnapshot().clipboard;
}

QRect RemoteDesktopWindow::audioToggleRect() const
{
    return titleBarLayoutSnapshot().audio;
}

// =====wjy====
QRect RemoteDesktopWindow::inputSyncRect() const
{
    return titleBarLayoutSnapshot().inputSync;
}
// ===end====

QRect RemoteDesktopWindow::minimizeRect() const
{
    return titleBarLayoutSnapshot().minimize;
}

QRect RemoteDesktopWindow::closeRect() const
{
    return titleBarLayoutSnapshot().close;
}

bool RemoteDesktopWindow::isTitleBarBlankArea(const QPoint& position) const
{
    return !isFullScreen()
        && position.y() >= 0
        && position.y() < titleBarHeight()
        && !(m_remoteUpdateAvailable && remoteUpdateButtonRect().contains(position)) // wjy: 更新按钮可见时从拖动、双击和右键空白区中排除。
        && !mouseInputModeRect().contains(position) // wjy: 键鼠注入后端是本地标题栏开关，不能触发拖窗、右键设备菜单或远端输入。
        && !inputSyncRect().contains(position) // wjy: 键鼠同步是纯本地标题栏操作，不能落入拖窗或远端鼠标路径。
        && !audioToggleRect().contains(position)
        && !clipboardSyncRect().contains(position)
        && !minimizeRect().contains(position)
        && !closeRect().contains(position);
}

void RemoteDesktopWindow::toggleMaximizedState()
{
    isMaximized() ? showNormal() : showMaximized(); // wjy: 标题栏双击的唯一最大化入口只切换显示状态，不把系统最大化矩形写入设备窗口 JSON。
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
    requestTitleBarUpdate(clipboardSyncRect());
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

void RemoteDesktopWindow::toggleViewerAudio()
{
    m_viewerAudioEnabled = !m_viewerAudioEnabled;
    sendCurrentViewerAudioDecision();
    requestTitleBarUpdate(audioToggleRect());
    appendViewerDebugLog(QStringLiteral("audio toggle host=%1 enabled=%2")
        .arg(m_hostIp)
        .arg(m_viewerAudioEnabled ? 1 : 0));
}

stream::RemoteQualityMode RemoteDesktopWindow::effectiveQualityMode() const
{
    return m_qualityOverrideMode == stream::RemoteQualityMode::FollowGlobal
        ? m_globalQualityConfiguration.defaultMode
        : m_qualityOverrideMode; // wjy: 全局变化只影响FollowGlobal窗口，已有局部覆盖保持不变。
}

QString RemoteDesktopWindow::remoteQualityStatusSummary() const
{
    if (!m_hasRemoteQualityDecision) {
        return QString::fromUtf8("请求：智能切换\n实际：等待协调器\n状态：准备中");
    }

    const QString requestedResolution = m_remoteQualityDecision.targetWidth > 0
            && m_remoteQualityDecision.targetHeight > 0
        ? QStringLiteral("%1×%2").arg(m_remoteQualityDecision.targetWidth).arg(m_remoteQualityDecision.targetHeight)
        : QString::fromUtf8("原始分辨率");
    const QString requested = QString::fromUtf8("请求：%1 · %2 · %3 FPS · ≤%4 Mbps")
        .arg(remoteQualityModeText(m_remoteQualityDecision.effectiveMode))
        .arg(requestedResolution)
        .arg(m_remoteQualityDecision.targetFps)
        .arg(m_remoteQualityDecision.maxBitrateKbps / 1000.0, 0, 'f', 1);

    QString applied;
    if (m_qualityProtocolUnavailable) {
        applied = QString::fromUtf8("实际：Host/运行库不支持在线调节，当前流继续");
    } else if (m_qualityRequestPending) {
        applied = QString::fromUtf8("实际：正在在线应用（不断流）");
    } else if (m_hasAppliedQualityStatus) {
        const QString appliedResolution = m_appliedQualityStatus.applied_width > 0
                && m_appliedQualityStatus.applied_height > 0
            ? QStringLiteral("%1×%2").arg(m_appliedQualityStatus.applied_width).arg(m_appliedQualityStatus.applied_height)
            : QString::fromUtf8("原始分辨率");
        applied = QString::fromUtf8("实际：%1 · %2 FPS · %3 Mbps")
            .arg(appliedResolution)
            .arg(m_appliedQualityStatus.applied_fps)
            .arg(m_appliedQualityStatus.applied_bitrate_kbps / 1000.0, 0, 'f', 1);
        if (m_appliedQualityStatus.limitation != FSREMOTE_VIEWER_QUALITY_LIMIT_NONE) {
            applied += QString::fromUtf8("（受Host硬边界限制）"); // wjy: 高质量锁定是优先级而非无限保证，实际被夹紧时明确告诉用户。
        }
    } else {
        applied = QString::fromUtf8("实际：等待连接/确认");
    }

    QString state = QString::fromUtf8("状态：%1").arg(remoteQualityReasonText(m_remoteQualityDecision.reason));
    if (m_remoteQualityDecision.effectiveMode == stream::RemoteQualityMode::HighQualityLocked
        && remoteQualityIsDegraded()) {
        state += QString::fromUtf8("（实际结果受Host或传输边界限制）"); // wjy: 高质量请求保持原始/60，向下提示只解释实际应用或兼容限制，不再暗示控制端主动降档。
    }
    return requested + QLatin1Char('\n') + applied + QLatin1Char('\n') + state;
}

bool RemoteDesktopWindow::remoteQualityIsDegraded() const
{
    if (!m_hasRemoteQualityDecision) {
        return false;
    }
    if (m_remoteQualityDecision.reason == RemoteQualityDegradationReason::Background
        || m_remoteQualityDecision.reason == RemoteQualityDegradationReason::Minimized
        || m_remoteQualityDecision.reason == RemoteQualityDegradationReason::PipelinePressure
        || m_remoteQualityDecision.reason == RemoteQualityDegradationReason::SeverePipelinePressure
        || m_remoteQualityDecision.reason == RemoteQualityDegradationReason::SoftwareFallback
        || m_qualityProtocolUnavailable) {
        return true;
    }
    if (m_hasAppliedQualityStatus
        && m_appliedQualityStatus.limitation != FSREMOTE_VIEWER_QUALITY_LIMIT_NONE) {
        return true;
    }
    return false;
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
    const QByteArray encoded = RemoteClipboardCodec::encode(text);
    if (encoded.isEmpty()) {
        return;
    }
    m_lastLocalClipboardText = text;
    sendInputMessage(QByteArray("cb ") + encoded); // wjy: Viewer -> Host 文本剪贴板同步。
}

void RemoteDesktopWindow::applyRemoteClipboardPayload(const QString& encodedBase64)
{
    if (!m_clipboardSyncEnabled) {
        return;
    }
    QString text;
    if (!RemoteClipboardCodec::decode(encodedBase64, &text)
        || text == m_lastAppliedRemoteClipboardText) {
        return;
    }
    m_lastAppliedRemoteClipboardText = text;
    m_lastLocalClipboardText = text;
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text); // wjy: Host -> Viewer 文本剪贴板落地。
    }
}
// ===end====

QRect RemoteDesktopWindow::remoteContentRect() const
{
    const int barHeight = titleBarHeight();
    return QRect(0, barHeight, width(), qMax(0, height() - barHeight)); // wjy: 内容层包含视频和等比例黑边，但永远不覆盖自绘标题栏。
}

QRect RemoteDesktopWindow::remoteImageRect() const
{
    const QSize remoteSize = remoteFrameSize();
    if (!remoteSize.isValid()) {
        return {};
    }
    const QRect contentRect = remoteContentRect();
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
    if (m_unifiedCompositor && m_unifiedCompositor->isEnabled()
        && m_unifiedCompositor->layout().isValid()
        && m_unifiedCompositor->layout().devicePixelRatio > 0.0) {
        const RemoteWindowLayoutSnapshot& snapshot = m_unifiedCompositor->layout();
        const qreal dpr = snapshot.devicePixelRatio;
        const QRect logicalInput(
            qRound(snapshot.inputRect.left() / dpr),
            qRound(snapshot.inputRect.top() / dpr),
            qRound(snapshot.inputRect.width() / dpr),
            qRound(snapshot.inputRect.height() / dpr));
        return normalizedRemoteInputPoint(logicalInput, position, x, y); // wjy: 新路径输入命中使用已提交快照反推逻辑矩形，避免拖拽中读取旧子HWND几何。
    }
    return normalizedRemoteInputPoint(remoteImageRect(), position, x, y); // wjy: 远端画面内换算与黑边拒绝由独立函数统一，普通绘制和 D3D 转发得到完全相同结果。
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

bool RemoteDesktopWindow::sendInputMessage(const QByteArray& message)
{
    if (remoteUpdateActive() || !m_viewerHandle
        || !RemoteConnectionState::acceptsRemoteInput(m_connectionStatusCode)) {
        return false; // wjy: 更新、断网、退避重连或没有有效Viewer时统一丢弃输入，不向失效句柄积压键鼠和剪贴板消息。
    }
    const bool shouldLog = shouldLogInputMessage(message);
    const bool ok = stream::StreamRuntime::instance().sendInput(m_viewerHandle, message);
    if (shouldLog) {
        appendInputDebugLog(QStringLiteral("viewer send handle=%1 ok=%2 msg=\"%3\"")
            .arg(reinterpret_cast<quintptr>(m_viewerHandle))
            .arg(ok ? 1 : 0)
            .arg(QString::fromLatin1(message)));
    }
    return ok;
}

// =====wjy====
bool RemoteDesktopWindow::dispatchRemoteInputEvent(const RemoteInputEvent& event)
{
    // =====wjy====
    if (m_inputScriptPlaying && !m_dispatchingInputScriptPlayback) {
        return false; // wjy: 回放期间屏蔽人工键鼠进入远端，但F10仍在本地快捷键层优先处理并可立即停止。
    }

    bool sent = false;
    if (!m_inputBroadcastCoordinator) {
        sent = sendSynchronizedInputEvent(event); // wjy: 独立测试或兜底窗口没有共享协调器时保持原有单会话输入路径。
    } else {
        const std::uint64_t generation = m_inputBroadcastCoordinator->capturedGenerationFor(this);
        sent = m_inputBroadcastCoordinator->routeInput(this, event, generation); // wjy: 在同一 UI 调用栈捕获代际并发送，切换主控后的旧事件无法进入新组。
    }
    if (sent && m_inputScriptRecording && !m_dispatchingInputScriptPlayback) {
        recordRemoteInputEvent(event); // wjy: 仅记录来源窗口实际发送成功的语义事件；本地快捷键、失败输入和回放事件都不会污染脚本。
    }
    return sent;
    // ===end====
}

// =====wjy====
void RemoteDesktopWindow::toggleInputScriptRecording()
{
    if (m_inputScriptPlaying) {
        appendViewerDebugLog(QStringLiteral("input script record ignored while playback host=%1").arg(m_hostIp));
        QApplication::beep(); // wjy: 录制与回放互斥，播放中F9只给出本地提示且不改变正在执行的脚本。
        return;
    }
    if (m_inputScriptRecording || m_inputScriptRecordingStopQueued) {
        finishInputScriptRecording(false);
        return;
    }
    startInputScriptRecording();
}

void RemoteDesktopWindow::startInputScriptRecording()
{
    if (m_closeInProgress || remoteUpdateActive() || m_inputScriptPlaying || m_inputScriptRecording) {
        return;
    }
    if (!synchronizedInputEligible()) {
        setInputScriptDialogActive(true);
        QMessageBox::warning(this, QString::fromUtf8("无法录制"),
            QString::fromUtf8("当前远控连接尚未进入可操作状态。"));
        setInputScriptDialogActive(false);
        return;
    }

    m_recordedInputScriptEvents.clear();
    m_recordedInputScriptEvents.reserve(4096); // wjy: 为常见短脚本预留少量连续空间，长时间录制仍由QVector按需扩展和硬上限保护。
    m_inputScriptRecordingFrameSize = remoteFrameSize();
    m_inputScriptRecordingStopQueued = false;
    m_inputScriptRecordingClock.start();
    m_inputScriptRecording = true;
    requestTitleBarUpdate();
    appendViewerDebugLog(QStringLiteral("input script recording started host=%1 frame=%2x%3")
        .arg(m_hostIp)
        .arg(m_inputScriptRecordingFrameSize.width())
        .arg(m_inputScriptRecordingFrameSize.height())); // wjy: 只记录录制生命周期，不逐事件写磁盘，避免高频鼠标日志影响远控流畅度。
}

void RemoteDesktopWindow::finishInputScriptRecording(bool limitReached)
{
    if (!m_inputScriptRecording && !m_inputScriptRecordingStopQueued) {
        return;
    }

    m_inputScriptRecording = false;
    m_inputScriptRecordingStopQueued = false;
    const qint64 durationMs = m_inputScriptRecordingClock.isValid()
        ? m_inputScriptRecordingClock.elapsed()
        : 0;
    m_inputScriptRecordingClock.invalidate();
    QVector<RemoteInputScriptEvent> recordedEvents = std::move(m_recordedInputScriptEvents);
    m_recordedInputScriptEvents.clear();
    requestTitleBarUpdate(); // wjy: 先撤下“录制中”，再打开命名框，标题栏状态不会在模态窗口期间继续误报。

    const QString defaultName = QString::fromUtf8("键鼠脚本-%1")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    setInputScriptDialogActive(true);
    bool accepted = false;
    QString scriptName = QInputDialog::getText(
        this,
        limitReached ? QString::fromUtf8("录制已达到安全上限") : QString::fromUtf8("保存键鼠脚本"),
        QString::fromUtf8("脚本名称："),
        QLineEdit::Normal,
        defaultName,
        &accepted).trimmed();
    if (!accepted) {
        appendViewerDebugLog(QStringLiteral("input script recording discarded host=%1 events=%2 duration_ms=%3")
            .arg(m_hostIp)
            .arg(recordedEvents.size())
            .arg(durationMs));
        setInputScriptDialogActive(false);
        return;
    }
    if (scriptName.isEmpty()) scriptName = defaultName; // wjy: 用户确认但清空名称时仍使用时间戳默认名，避免生成不可辨认的空名脚本。

    RemoteInputScript script;
    script.name = scriptName;
    script.sourceHost = m_hostIp;
    script.sourceFrameSize = m_inputScriptRecordingFrameSize;
    script.events = std::move(recordedEvents);
    QString savedFilePath;
    QString errorMessage;
    if (!RemoteInputScriptStore::saveToDirectory(
            RemoteInputScriptStore::defaultDirectory(), script, &savedFilePath, &errorMessage)) {
        QMessageBox::warning(this, QString::fromUtf8("保存失败"), errorMessage);
        appendViewerDebugLog(QStringLiteral("input script save failed host=%1 events=%2 error=%3")
            .arg(m_hostIp)
            .arg(script.events.size())
            .arg(errorMessage));
        setInputScriptDialogActive(false);
        return;
    }
    appendViewerDebugLog(QStringLiteral("input script saved host=%1 events=%2 duration_ms=%3 path=%4")
        .arg(m_hostIp)
        .arg(script.events.size())
        .arg(durationMs)
        .arg(savedFilePath));
    setInputScriptDialogActive(false);
}

void RemoteDesktopWindow::cancelInputScriptRecording(const QString& reason)
{
    if (!m_inputScriptRecording && !m_inputScriptRecordingStopQueued) {
        return;
    }
    const qsizetype eventCount = m_recordedInputScriptEvents.size();
    m_inputScriptRecording = false;
    m_inputScriptRecordingStopQueued = false;
    m_inputScriptRecordingClock.invalidate();
    m_recordedInputScriptEvents.clear();
    requestTitleBarUpdate();
    appendViewerDebugLog(QStringLiteral("input script recording cancelled host=%1 events=%2 reason=%3")
        .arg(m_hostIp)
        .arg(eventCount)
        .arg(reason)); // wjy: 生命周期中断只写一次原因并丢弃半成品，不在关闭或断线流程中弹出命名窗口。
}

void RemoteDesktopWindow::recordRemoteInputEvent(const RemoteInputEvent& event)
{
    if (!m_inputScriptRecording || !m_inputScriptRecordingClock.isValid()) {
        return;
    }
    const qint64 elapsedMs = m_inputScriptRecordingClock.elapsed();
    if (elapsedMs > kMaximumRecordedInputScriptDurationMs) {
        m_inputScriptRecording = false;
        requestTitleBarUpdate();
        if (!m_inputScriptRecordingStopQueued) {
            m_inputScriptRecordingStopQueued = true;
            QMetaObject::invokeMethod(this, [this] {
                finishInputScriptRecording(true); // wjy: 上限停止通过Qt队列打开命名框，绝不在Windows低级键盘Hook回调栈内进入模态事件循环。
            }, Qt::QueuedConnection);
        }
        return;
    }

    if (!m_recordedInputScriptEvents.isEmpty()) {
        RemoteInputScriptEvent& previous = m_recordedInputScriptEvents.last();
        const qint64 gapMs = elapsedMs - previous.elapsedMs;
        if (event.type == RemoteInputEventType::AbsoluteMove
            && previous.input.type == RemoteInputEventType::AbsoluteMove
            && gapMs >= 0 && gapMs <= kAbsoluteMouseMoveMergeWindowMs) {
            previous.elapsedMs = elapsedMs;
            previous.input = event;
            return; // wjy: 绝对移动只需保留合并窗口末端位置，按钮、滚轮和键盘事件仍保持原始边界。
        }
        if (event.type == RemoteInputEventType::RelativeMove
            && previous.input.type == RemoteInputEventType::RelativeMove
            && previous.input.buttons == event.buttons
            && gapMs >= 0 && gapMs <= kRelativeMouseMoveMergeWindowMs) {
            const double relativeX = previous.input.relativeX + event.relativeX;
            const double relativeY = previous.input.relativeY + event.relativeY;
            const int fallbackX = previous.input.fallbackDeltaX + event.fallbackDeltaX;
            const int fallbackY = previous.input.fallbackDeltaY + event.fallbackDeltaY;
            if (relativeX >= -4.0 && relativeX <= 4.0 && relativeY >= -4.0 && relativeY <= 4.0
                && fallbackX >= -200 && fallbackX <= 200 && fallbackY >= -200 && fallbackY <= 200) {
                previous.elapsedMs = elapsedMs;
                previous.input.relativeX = relativeX;
                previous.input.relativeY = relativeY;
                previous.input.fallbackDeltaX = fallbackX;
                previous.input.fallbackDeltaY = fallbackY;
                return; // wjy: 相对移动累加而不是覆盖，确保高回报率鼠标压缩后远端总位移保持一致。
            }
        }
    }

    if (m_recordedInputScriptEvents.size() >= kMaximumRecordedInputScriptEvents) {
        m_inputScriptRecording = false;
        requestTitleBarUpdate();
        if (!m_inputScriptRecordingStopQueued) {
            m_inputScriptRecordingStopQueued = true;
            QMetaObject::invokeMethod(this, [this] {
                finishInputScriptRecording(true);
            }, Qt::QueuedConnection);
        }
        return;
    }
    m_recordedInputScriptEvents.push_back(RemoteInputScriptEvent{elapsedMs, event});
}

void RemoteDesktopWindow::toggleInputScriptPlayback()
{
    if (m_inputScriptPlaying) {
        stopInputScriptPlayback(QStringLiteral("user_f10"), true);
        return;
    }
    if (m_inputScriptRecording || m_inputScriptRecordingStopQueued) {
        appendViewerDebugLog(QStringLiteral("input script playback ignored while recording host=%1").arg(m_hostIp));
        QApplication::beep(); // wjy: 录制中F10不打断也不保存半成品，仍由第二次F9完成命名流程。
        return;
    }
    chooseAndStartInputScriptPlayback();
}

void RemoteDesktopWindow::chooseAndStartInputScriptPlayback()
{
    if (m_closeInProgress || remoteUpdateActive() || m_inputScriptRecording || m_inputScriptPlaying) {
        return;
    }
    if (!synchronizedInputEligible()) {
        setInputScriptDialogActive(true);
        QMessageBox::warning(this, QString::fromUtf8("无法回放"),
            QString::fromUtf8("当前远控连接尚未进入可操作状态。"));
        setInputScriptDialogActive(false);
        return;
    }

    QDir scriptDirectory(RemoteInputScriptStore::defaultDirectory());
    if (!scriptDirectory.mkpath(QStringLiteral("."))) {
        setInputScriptDialogActive(true);
        QMessageBox::warning(this, QString::fromUtf8("无法回放"),
            QString::fromUtf8("无法创建或访问本地script文件夹。"));
        setInputScriptDialogActive(false);
        return;
    }

    RemoteInputPlaybackDialog playbackDialog(
        scriptDirectory.absolutePath(),
        m_inputScriptPlaybackFilePath,
        m_inputScriptPlaybackLoopCount,
        m_inputScriptPlaybackLoopIntervalMs,
        m_inputScriptPlaybackSpeedMultiplier,
        this);
    setInputScriptDialogActive(true);
    if (playbackDialog.exec() != QDialog::Accepted) {
        setInputScriptDialogActive(false);
        return;
    }
    const RemoteInputPlaybackOptions options = playbackDialog.options();
    const QString filePath = options.filePath;
    m_inputScriptPlaybackFilePath = filePath;
    m_inputScriptPlaybackLoopCount = options.loopCount;
    m_inputScriptPlaybackLoopIntervalMs = options.loopIntervalMs;
    m_inputScriptPlaybackSpeedMultiplier = options.speedMultiplier; // wjy: 文件、循环次数、轮间隔和速度由同一个F10设置窗口一次性提交。

    RemoteInputScript script;
    QString errorMessage;
    if (!RemoteInputScriptStore::loadFromFile(filePath, &script, &errorMessage)) {
        QMessageBox::warning(this, QString::fromUtf8("脚本无效"), errorMessage);
        appendViewerDebugLog(QStringLiteral("input script load failed host=%1 path=%2 error=%3")
            .arg(m_hostIp, QDir::toNativeSeparators(filePath), errorMessage));
        setInputScriptDialogActive(false);
        return;
    }
    if (script.events.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("无法回放"),
            QString::fromUtf8("所选脚本中没有键鼠事件。"));
        setInputScriptDialogActive(false);
        return;
    }
    if (!synchronizedInputEligible()) {
        QMessageBox::warning(this, QString::fromUtf8("无法回放"),
            QString::fromUtf8("选择脚本期间远控连接已不可操作。"));
        setInputScriptDialogActive(false);
        return;
    }

    const QString scriptName = script.name;
    const qsizetype eventCount = script.events.size();
    const qint64 durationMs = script.events.last().elapsedMs;
    const qint64 adjustedDurationMs = remoteInputScriptPlaybackTimeMs(
        durationMs, m_inputScriptPlaybackSpeedMultiplier);
    m_inputScriptPlaybackEvents = std::move(script.events);
    m_inputScriptPlaybackIndex = 0;
    m_inputScriptPlaybackCompletedLoopCount = 0;
    m_inputScriptPlaybackWaitingForLoopInterval = false; // wjy: 第一次播放立即开始，循环间隔只能出现在已经完整执行过一轮之后。
    m_inputScriptPlaybackHeldKeys.clear();
    m_inputScriptPlaybackHeldButtons.clear();
    m_inputScriptPlaybackX = 32768;
    m_inputScriptPlaybackY = 32768;
    m_inputScriptPlaybackClock.start();
    m_inputScriptPlaying = true;
    requestTitleBarUpdate();
    appendViewerDebugLog(QStringLiteral("input script playback started host=%1 name=%2 events=%3 duration_ms=%4 adjusted_ms=%5 loops=%6 loop_interval_ms=%7 speed=%8 path=%9")
        .arg(m_hostIp, scriptName)
        .arg(eventCount)
        .arg(durationMs)
        .arg(adjustedDurationMs)
        .arg(m_inputScriptPlaybackLoopCount)
        .arg(m_inputScriptPlaybackLoopIntervalMs)
        .arg(QString::number(m_inputScriptPlaybackSpeedMultiplier, 'f', 2))
        .arg(QDir::toNativeSeparators(filePath)));
    setInputScriptDialogActive(false);
    processInputScriptPlaybackEvents(); // wjy: 时间戳为0的首批事件立即发送，其余事件再由单次高精度定时器调度。
}

void RemoteDesktopWindow::scheduleNextInputScriptPlaybackEvent()
{
    if (!m_inputScriptPlaying || !m_inputScriptPlaybackTimer) {
        return;
    }
    if (m_inputScriptPlaybackIndex >= m_inputScriptPlaybackEvents.size()) {
        completeInputScriptPlaybackLoop();
        return;
    }
    const qint64 elapsedMs = m_inputScriptPlaybackClock.isValid()
        ? m_inputScriptPlaybackClock.elapsed()
        : 0;
    const qint64 eventTimeMs = remoteInputScriptPlaybackTimeMs(
        m_inputScriptPlaybackEvents.at(m_inputScriptPlaybackIndex).elapsedMs,
        m_inputScriptPlaybackSpeedMultiplier);
    const qint64 delayMs = eventTimeMs - elapsedMs;
    const int timerDelayMs = static_cast<int>(std::clamp<qint64>(
        delayMs, 1, std::numeric_limits<int>::max()));
    m_inputScriptPlaybackTimer->start(timerDelayMs); // wjy: 每次都基于脚本绝对时间减去当前实耗重新计算，UI偶发卡顿只造成追帧而不会永久累积误差。
}

void RemoteDesktopWindow::processInputScriptPlaybackEvents()
{
    if (!m_inputScriptPlaying) {
        return;
    }
    if (!synchronizedInputEligible()) {
        stopInputScriptPlayback(QStringLiteral("connection_unavailable"), true);
        return;
    }
    if (m_inputScriptPlaybackWaitingForLoopInterval) {
        m_inputScriptPlaybackWaitingForLoopInterval = false;
        m_inputScriptPlaybackClock.start(); // wjy: 间隔结束后从已失效状态启动全新时间轴，等待时间不会导致下一轮事件被误判为全部逾期并瞬间发送。
    }

    qint64 elapsedMs = m_inputScriptPlaybackClock.elapsed();
    qsizetype processedThisTick = 0;
    while (m_inputScriptPlaybackIndex < m_inputScriptPlaybackEvents.size()
        && remoteInputScriptPlaybackTimeMs(
                m_inputScriptPlaybackEvents.at(m_inputScriptPlaybackIndex).elapsedMs,
                m_inputScriptPlaybackSpeedMultiplier) <= elapsedMs
        && processedThisTick < kMaximumInputScriptEventsPerPlaybackTick) {
        const RemoteInputEvent event = m_inputScriptPlaybackEvents.at(m_inputScriptPlaybackIndex).input;
        bool sent = false;
        {
            QScopedValueRollback<bool> playbackDispatch(m_dispatchingInputScriptPlayback, true);
            sent = dispatchRemoteInputEvent(event); // wjy: 回放复用正常语义分发，因此当前窗口若是同步主控，脚本会按现有规则同步到全部跟随设备。
        }
        if (sent || (event.type != RemoteInputEventType::KeyUp
                && event.type != RemoteInputEventType::ButtonUp)) {
            trackInputScriptPlaybackState(event); // wjy: 失败的按下按“可能已到达部分跟随端”保守记录；失败的抬起不清除持有态，停止时再补发一次最安全。
        }
        if (!sent) {
            stopInputScriptPlayback(QStringLiteral("send_failed"), true);
            return;
        }
        ++m_inputScriptPlaybackIndex;
        ++processedThisTick;
        elapsedMs = m_inputScriptPlaybackClock.elapsed(); // wjy: 一批同时间事件发送后重新取时钟，及时追上处理期间已经到期的后续事件。
    }

    if (m_inputScriptPlaybackIndex >= m_inputScriptPlaybackEvents.size()) {
        completeInputScriptPlaybackLoop();
        return;
    }
    if (processedThisTick >= kMaximumInputScriptEventsPerPlaybackTick
        && remoteInputScriptPlaybackTimeMs(
                m_inputScriptPlaybackEvents.at(m_inputScriptPlaybackIndex).elapsedMs,
                m_inputScriptPlaybackSpeedMultiplier) <= elapsedMs) {
        m_inputScriptPlaybackTimer->start(0);
        return; // wjy: 到期积压拆到下一轮事件循环继续追赶，恶意或异常脚本不能用同时间戳长循环卡死UI线程。
    }
    scheduleNextInputScriptPlaybackEvent();
}

void RemoteDesktopWindow::completeInputScriptPlaybackLoop()
{
    if (!m_inputScriptPlaying) return;

    if (m_inputScriptPlaybackCompletedLoopCount < std::numeric_limits<int>::max()) {
        ++m_inputScriptPlaybackCompletedLoopCount; // wjy: 无限循环长期运行时计数饱和而不发生有符号整数溢出，0无限语义不受影响。
    }
    releaseInputScriptPlaybackInputs(); // wjy: 每个完整循环都强制释放未配对键鼠和相对捕获，下一轮从干净输入状态开始。
    if (!remoteInputScriptShouldRepeat(
            m_inputScriptPlaybackLoopCount, m_inputScriptPlaybackCompletedLoopCount)) {
        stopInputScriptPlayback(QStringLiteral("completed"), false);
        return;
    }

    m_inputScriptPlaybackIndex = 0;
    if (m_inputScriptPlaybackTimer) {
        if (m_inputScriptPlaybackLoopIntervalMs > 0) {
            m_inputScriptPlaybackWaitingForLoopInterval = true;
            m_inputScriptPlaybackClock.invalidate(); // wjy: 等待期间暂停脚本时间轴，间隔不参与下一轮原始时间戳换算。
            m_inputScriptPlaybackTimer->start(m_inputScriptPlaybackLoopIntervalMs); // wjy: 复用同一个单次定时器，因此等待期间再次按 F10 可以立即 stop 并取消下一轮。
        } else {
            m_inputScriptPlaybackWaitingForLoopInterval = false;
            m_inputScriptPlaybackClock.restart(); // wjy: 无间隔时仍重新以0毫秒为绝对时间基准，前一轮处理耗时不会累积。
            m_inputScriptPlaybackTimer->start(0); // wjy: 即使全部事件时间戳都是0，无限循环也逐轮让出事件循环，F10始终可以及时停止。
        }
    }
}

void RemoteDesktopWindow::stopInputScriptPlayback(const QString& reason, bool releaseRemoteInputs)
{
    if (!m_inputScriptPlaying && m_inputScriptPlaybackEvents.isEmpty()
        && m_inputScriptPlaybackHeldKeys.isEmpty() && m_inputScriptPlaybackHeldButtons.isEmpty()) {
        if (m_inputScriptPlaybackTimer) m_inputScriptPlaybackTimer->stop();
        m_inputScriptPlaybackWaitingForLoopInterval = false;
        return; // wjy: 普通关闭或析构没有活动脚本时不额外向远端发送无意义的CaptureRelease。
    }
    const bool wasPlaying = m_inputScriptPlaying;
    const qsizetype completedEvents = m_inputScriptPlaybackIndex;
    const qsizetype totalEvents = m_inputScriptPlaybackEvents.size();
    if (m_inputScriptPlaybackTimer) m_inputScriptPlaybackTimer->stop();
    m_inputScriptPlaybackWaitingForLoopInterval = false; // wjy: F10、断线或关闭都取消尚未开始的下一轮，定时器不会在停止后重新唤醒脚本。
    m_inputScriptPlaying = false;
    if (releaseRemoteInputs) {
        releaseInputScriptPlaybackInputs(); // wjy: 先撤销播放门禁再补发抬起，F10、断线和自然结束都不会在远端留下按住状态。
    } else {
        m_inputScriptPlaybackHeldKeys.clear();
        m_inputScriptPlaybackHeldButtons.clear();
    }
    m_inputScriptPlaybackEvents.clear();
    m_inputScriptPlaybackIndex = 0;
    m_inputScriptPlaybackClock.invalidate();
    if (wasPlaying) {
        requestTitleBarUpdate();
        appendViewerDebugLog(QStringLiteral("input script playback stopped host=%1 event_index=%2 event_total=%3 completed_loops=%4 configured_loops=%5 loop_interval_ms=%6 speed=%7 reason=%8")
            .arg(m_hostIp)
            .arg(completedEvents)
            .arg(totalEvents)
            .arg(m_inputScriptPlaybackCompletedLoopCount)
            .arg(m_inputScriptPlaybackLoopCount)
            .arg(m_inputScriptPlaybackLoopIntervalMs)
            .arg(QString::number(m_inputScriptPlaybackSpeedMultiplier, 'f', 2))
            .arg(reason));
    }
}

void RemoteDesktopWindow::releaseInputScriptPlaybackInputs()
{
    const QSet<int> heldKeys = m_inputScriptPlaybackHeldKeys;
    const QSet<int> heldButtons = m_inputScriptPlaybackHeldButtons;
    m_inputScriptPlaybackHeldKeys.clear();
    m_inputScriptPlaybackHeldButtons.clear();

    for (int virtualKey : heldKeys) {
        RemoteInputEvent release;
        release.type = RemoteInputEventType::KeyUp;
        release.virtualKey = virtualKey;
        QScopedValueRollback<bool> playbackDispatch(m_dispatchingInputScriptPlayback, true);
        dispatchRemoteInputEvent(release);
    }
    for (int button : heldButtons) {
        RemoteInputEvent release;
        release.type = RemoteInputEventType::ButtonUp;
        release.button = button;
        release.normalizedX = m_inputScriptPlaybackX;
        release.normalizedY = m_inputScriptPlaybackY;
        QScopedValueRollback<bool> playbackDispatch(m_dispatchingInputScriptPlayback, true);
        dispatchRemoteInputEvent(release);
    }
    RemoteInputEvent captureRelease;
    captureRelease.type = RemoteInputEventType::CaptureRelease;
    QScopedValueRollback<bool> playbackDispatch(m_dispatchingInputScriptPlayback, true);
    dispatchRemoteInputEvent(captureRelease); // wjy: 即使脚本没有显式按钮按下，也兜底退出可能由相对移动触发的远端鼠标捕获。
}

void RemoteDesktopWindow::trackInputScriptPlaybackState(const RemoteInputEvent& event)
{
    if (event.type == RemoteInputEventType::AbsoluteMove
        || event.type == RemoteInputEventType::ButtonDown
        || event.type == RemoteInputEventType::ButtonUp
        || event.type == RemoteInputEventType::Wheel) {
        m_inputScriptPlaybackX = std::clamp(event.normalizedX, 0, 65535);
        m_inputScriptPlaybackY = std::clamp(event.normalizedY, 0, 65535);
    }
    if (event.type == RemoteInputEventType::KeyDown && event.virtualKey > 0) {
        m_inputScriptPlaybackHeldKeys.insert(event.virtualKey);
    } else if (event.type == RemoteInputEventType::KeyUp) {
        m_inputScriptPlaybackHeldKeys.remove(event.virtualKey);
    } else if (event.type == RemoteInputEventType::ButtonDown && event.button > 0) {
        m_inputScriptPlaybackHeldButtons.insert(event.button);
    } else if (event.type == RemoteInputEventType::ButtonUp) {
        m_inputScriptPlaybackHeldButtons.remove(event.button);
    }
}

void RemoteDesktopWindow::setInputScriptDialogActive(bool active)
{
    m_inputScriptDialogActive = active;
    setKeyboardForwardingActive(!active && !remoteUpdateActive()); // wjy: 模态框期间即使远控窗口仍是活动窗口也不转发键盘，关闭后再按连接和焦点资格恢复。
}
// ===end====

bool RemoteDesktopWindow::synchronizedInputEligible() const
{
    return m_viewerHandle != nullptr
        && RemoteConnectionState::acceptsRemoteInput(m_connectionStatusCode)
        && !m_closeInProgress
        && !remoteUpdateActive(); // wjy: 只有首帧已建立且未关闭、未更新的 Viewer 才能成为主控或接收跟随输入。
}

QSize RemoteDesktopWindow::synchronizedInputFrameSize() const
{
    return remoteFrameSize();
}

bool RemoteDesktopWindow::sendSynchronizedInputEvent(const RemoteInputEvent& event)
{
    const QByteArray message = serializeRemoteInputEvent(event, synchronizedInputFrameSize());
    return !message.isEmpty() && sendInputMessage(message); // wjy: 每个目标用自己的远端尺寸序列化同一语义事件，最终仍复用 fsremote_stream_send_input。
}

void RemoteDesktopWindow::synchronizedInputRoleChanged(RemoteInputSyncRole role)
{
    if (m_inputSyncRole == role) {
        return;
    }
    m_inputSyncRole = role;
    requestTitleBarUpdate(inputSyncRect()); // wjy: 主控切换只刷新三态按钮，系统移动期间延迟到最终位置。
}

void RemoteDesktopWindow::toggleInputSynchronization()
{
    if (m_inputBroadcastCoordinator) {
        m_inputBroadcastCoordinator->toggleMaster(this); // wjy: 协调器根据当前角色执行开启、切主控或关闭，并在内部完成释放屏障。
    }
}

void RemoteDesktopWindow::setRemoteMouseCaptureActive(bool active)
{
    // =====wjy====
    if (active && !synchronizedInputEligible()) {
        return; // wjy: 更新或断网重连期间忽略旧流迟到的relative状态，防止本机鼠标再次被隐藏或锁定。
    }
    m_remoteMouseCaptureRequested = active; // wjy: Host 状态只改变自动请求；F2 手动锁定开启时，Host 回到 absolute 也不能覆盖用户意图。
    updateRemoteMouseCaptureState();
    // ===end====
}

// =====wjy====
void RemoteDesktopWindow::toggleManualMouseLock()
{
    m_manualMouseLockActive = !m_manualMouseLockActive; // wjy: F2 维护独立手动意图，不伪造 Host 的游戏检测状态。
    updateRemoteMouseCaptureState(); // wjy: 当前连接且有焦点时立即切换；失焦或断线时只保存意图，恢复资格后自动生效。
    requestTitleBarUpdate(); // wjy: 标题栏静态“鼠标锁定”文字只在开关变化时刷新，不引入逐鼠标事件重绘。
    appendViewerDebugLog(QStringLiteral("manual mouse lock changed host=%1 enabled=%2 effective=%3 host_requested=%4 shortcut=%5")
        .arg(m_hostIp)
        .arg(m_manualMouseLockActive ? 1 : 0)
        .arg(m_remoteMouseCaptureActive ? 1 : 0)
        .arg(m_remoteMouseCaptureRequested ? 1 : 0)
        .arg(platform::AppSettings::remoteShortcutMouseLock().toString(QKeySequence::PortableText))); // wjy: 仅记录低频开关结果和当前配置到既有 data 日志，禁止记录 Raw Input 高频位移。
}

void RemoteDesktopWindow::updateRemoteMouseCaptureState()
{
    const bool shouldCapture = (m_remoteMouseCaptureRequested || m_manualMouseLockActive)
        && synchronizedInputEligible()
        && isActiveWindow(); // wjy: 只有当前活动且可输入的远控窗口能占用进程唯一 Raw Input 鼠标目标。
    applyRemoteMouseCaptureState(shouldCapture);
}

void RemoteDesktopWindow::suspendRemoteMouseCapture()
{
    applyRemoteMouseCaptureState(false); // wjy: 生命周期暂停只撤销实际捕获，Host 请求或 F2 手动意图由调用方按场景决定是否保留。
}

void RemoteDesktopWindow::applyRemoteMouseCaptureState(bool active)
{
    if (m_remoteMouseCaptureActive == active) {
        if (active) {
            setRawInputMouseCaptureEnabled(true); // wjy: 重复激活时允许上次注册失败的设备重试，仍失败则继续使用 Qt 中心差值回退路径。
            recenterRemoteMouseCapture();
        } else if (m_rawInputMouseCaptureActive) {
            setRawInputMouseCaptureEnabled(false); // wjy: 即使极端焦点切换造成实际状态与 Raw Input 标志短暂不同步，暂停和析构路径也会强制清理注册。
        }
        return;
    }

    m_remoteMouseCaptureActive = active;
    if (active) {
        setWindowAndPresenterCursor(Qt::BlankCursor); // wjy: 相对鼠标模式始终拥有最高优先级，远端形状更新只能缓存不能显示。
        setRawInputMouseCaptureEnabled(true); // wjy: Windows 活动窗口优先注册 Raw Input；失败不会改变捕获状态，后续 mouseMoveEvent 自动使用旧中心差值代码。
        recenterRemoteMouseCapture();
    } else {
        setRawInputMouseCaptureEnabled(false); // wjy: 退出相对捕获立即释放进程级 Raw Input 鼠标注册，普通桌面移动继续完全交给 Qt。
        updateResizeCursor(mapFromGlobal(QCursor::pos())); // wjy: 退出捕获后立即按当前位置恢复本地边框或已缓存的远端桌面光标。
        RemoteInputEvent event;
        event.type = RemoteInputEventType::CaptureRelease;
        QScopedValueRollback<bool> localCaptureRelease(m_dispatchingInputScriptPlayback, true);
        dispatchRemoteInputEvent(event); // wjy: 生命周期和 F2 产生的释放必须穿过播放门禁，但不应作为用户键鼠动作写入 F9 录制脚本。
    }
    appendViewerDebugLog(QStringLiteral("remote mouse capture changed host=%1 active=%2 manual=%3 host_requested=%4 focused=%5 eligible=%6")
        .arg(m_hostIp)
        .arg(m_remoteMouseCaptureActive ? 1 : 0)
        .arg(m_manualMouseLockActive ? 1 : 0)
        .arg(m_remoteMouseCaptureRequested ? 1 : 0)
        .arg(isActiveWindow() ? 1 : 0)
        .arg(synchronizedInputEligible() ? 1 : 0)); // wjy: 只记录实际捕获生命周期，便于排查多窗口焦点切换，不记录任何逐帧鼠标数据。
}
// ===end====

// =====wjy====
void RemoteDesktopWindow::setRemoteCursorShape(const QString& statusMessage)
{
    const std::optional<Qt::CursorShape> shape = remoteCursorShapeFromStatus(QStringView(statusMessage));
    if (!shape.has_value()) {
        return; // wjy: 非法、未知或未来版本消息不覆盖最后一个有效形状，保证当前会话显示稳定。
    }

    m_remoteCursorShape = *shape;
    if (!m_remoteMouseCaptureActive) {
        updateResizeCursor(mapFromGlobal(QCursor::pos())); // wjy: 状态到达时鼠标可能静止，主动刷新才能无需再次移动就看到远端窗口缩放光标。
    }
}

void RemoteDesktopWindow::setRemoteMouseBackendStatus(const QString& statusMessage)
{
    const QStringList parts = statusMessage.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() != 3 || parts.at(0) != QStringLiteral("__fsremote_mouse_backend_status")) {
        return; // wjy: 即使原生 DLL 已校验，Qt 层仍拒绝字段数或前缀异常的状态，旧值保持不变。
    }
    const bool system = parts.at(1) == QStringLiteral("system");
    const bool faker = parts.at(1) == QStringLiteral("faker");
    const bool ready = parts.at(2) == QStringLiteral("ready");
    const bool fallback = parts.at(2) == QStringLiteral("fallback");
    // =====wjy====
    const bool installing = parts.at(2) == QStringLiteral("installing");
    if ((!system && !faker) || (!ready && !fallback && !installing)
        || ((fallback || installing) && !system)) {
        return; // wjy: 当前协议只允许 system/faker ready、system fallback 与 system installing，驱动准备完成前不能显示 faker。
    }

    m_remoteMouseBackend = faker ? RemoteMouseBackend::Faker : RemoteMouseBackend::System;
    m_pendingRemoteMouseBackend = m_remoteMouseBackend;
    m_remoteMouseBackendKnown = true;
    m_remoteMouseBackendPending = installing; // wjy: 安装期间按钮保持禁用，避免重复请求并发启动多个 Windows Installer。
    m_remoteMouseBackendFallback = fallback;
    const quint64 requestGeneration = ++m_remoteMouseBackendRequestGeneration; // wjy: 使初始两秒超时失效；安装态再创建独立的长超时保护。
    if (installing) {
        m_remoteMouseBackendMessage = zh("目标端正在静默安装 FakerInput 并启动驱动服务…");
        QTimer::singleShot(6 * 60 * 1000, this, [this, requestGeneration] {
            if (requestGeneration != m_remoteMouseBackendRequestGeneration || !m_remoteMouseBackendPending) return;
            m_remoteMouseBackendPending = false;
            m_remoteMouseBackendFallback = true;
            m_remoteMouseBackendMessage = zh("目标端驱动安装等待超时，当前继续使用系统键鼠"); // wjy: 超时仅恢复控制端按钮，不中断目标端可能仍在提交的 MSI 事务。
            requestTitleBarUpdate(mouseInputModeRect());
        });
    } else if (fallback) {
        m_remoteMouseBackendMessage = zh("驱动键鼠不可用，已自动回退系统键鼠"); // wjy: 后端现已同时承载键盘和鼠标，回退提示不得继续误导为仅鼠标变化。
    } else if (faker) {
        m_remoteMouseBackendMessage = zh("驱动键鼠已启用（FakerInputBridge）");
    } else {
        m_remoteMouseBackendMessage = zh("系统键鼠已启用（SendInput）");
    }
    requestTitleBarUpdate(mouseInputModeRect());
    // ===end====
}
// ===end====

// =====wjy====
void RemoteDesktopWindow::toggleRemoteMouseBackend()
{
    if (m_remoteMouseBackendPending) return;
    requestRemoteMouseBackend(m_remoteMouseBackend == RemoteMouseBackend::System
            ? RemoteMouseBackend::Faker
            : RemoteMouseBackend::System); // wjy: 只根据 Host 最后确认值选择相反后端，不使用上一次未确认的点击结果。
}

void RemoteDesktopWindow::requestRemoteMouseBackend(RemoteMouseBackend backend)
{
    if (m_remoteMouseBackendPending) return;
    m_pendingRemoteMouseBackend = backend;
    m_remoteMouseBackendPending = true;
    m_remoteMouseBackendFallback = false;
    m_remoteMouseBackendMessage = backend == RemoteMouseBackend::Faker
        ? zh("正在请求驱动键鼠…")
        : zh("正在请求系统键鼠…"); // wjy: 保留现有兼容协议前缀，但界面明确一次切换会同时改变键盘和鼠标注入后端。
    const quint64 request_generation = ++m_remoteMouseBackendRequestGeneration;
    const QByteArray wire = backend == RemoteMouseBackend::Faker
        ? QByteArrayLiteral("__fsremote_mouse_backend faker")
        : QByteArrayLiteral("__fsremote_mouse_backend system");
    if (!sendInputMessage(wire)) {
        m_remoteMouseBackendPending = false;
        m_remoteMouseBackendMessage = zh("键鼠模式请求未发送，远控连接尚未就绪");
        requestTitleBarUpdate(mouseInputModeRect());
        return;
    }
    requestTitleBarUpdate(mouseInputModeRect());
    QTimer::singleShot(2000, this, [this, request_generation] {
        if (request_generation != m_remoteMouseBackendRequestGeneration || !m_remoteMouseBackendPending) return;
        m_remoteMouseBackendPending = false; // wjy: 旧 Host 不识别新协议时两秒后恢复按钮，当前真实模式继续按最后确认值或安全系统默认显示。
        m_remoteMouseBackendMessage = zh("目标端未确认键鼠模式，可能需要更新 FSRemote");
        requestTitleBarUpdate(mouseInputModeRect());
    });
}

void RemoteDesktopWindow::queryRemoteMouseBackend()
{
    if (m_remoteMouseBackendPending || !m_viewerHandle) return;
    m_pendingRemoteMouseBackend = m_remoteMouseBackend;
    m_remoteMouseBackendPending = true;
    m_remoteMouseBackendMessage = zh("正在读取目标端键鼠模式…");
    const quint64 request_generation = ++m_remoteMouseBackendRequestGeneration;
    if (!sendInputMessage(QByteArrayLiteral("__fsremote_mouse_backend query"))) {
        m_remoteMouseBackendPending = false;
        m_remoteMouseBackendMessage = zh("暂时无法读取目标端键鼠模式");
        requestTitleBarUpdate(mouseInputModeRect());
        return;
    }
    requestTitleBarUpdate(mouseInputModeRect());
    QTimer::singleShot(2000, this, [this, request_generation] {
        if (request_generation != m_remoteMouseBackendRequestGeneration || !m_remoteMouseBackendPending) return;
        m_remoteMouseBackendPending = false;
        m_remoteMouseBackendKnown = false; // wjy: 无确认时不把本地默认值冒充 Host 状态，按钮仍可尝试切换以兼容刚升级的目标端。
        m_remoteMouseBackendMessage = zh("目标端未返回键鼠模式，可能是旧版本 FSRemote");
        requestTitleBarUpdate(mouseInputModeRect());
    });
}

bool RemoteDesktopWindow::sendRemoteMouseMove(const QPoint& position, Qt::MouseButtons buttons)
{
    if (m_remoteMouseCaptureActive) {
        // =====wjy====
#if defined(Q_OS_WIN)
        if (m_rawInputMouseCaptureActive) {
            return true; // wjy: WM_INPUT 已承担相对位移时吞掉 Qt 的真实/回中心合成 MouseMove，禁止同一次物理移动发送两遍。
        }
#endif
        // ===end====
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
    RemoteInputEvent event;
    event.type = RemoteInputEventType::AbsoluteMove;
    event.normalizedX = x;
    event.normalizedY = y;
    event.buttons = remoteButtons;
    dispatchRemoteInputEvent(event); // wjy: 窗口尺寸和黑边只参与一次归一化，所有目标收到相同远端比例位置。
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
    RemoteInputEvent event;
    event.type = RemoteInputEventType::RelativeMove;
    event.relativeX = remoteSize.width() > 0 ? double(dx) / double(remoteSize.width()) : 0.0;
    event.relativeY = remoteSize.height() > 0 ? double(dy) / double(remoteSize.height()) : 0.0;
    event.fallbackDeltaX = dx;
    event.fallbackDeltaY = dy;
    event.buttons = remoteButtons;
    dispatchRemoteInputEvent(event); // wjy: 源端像素位移转换为远端尺寸比例，跟随窗口按各自分辨率还原并夹紧。
    recenterRemoteMouseCapture();
    return true;
}

// =====wjy====
bool RemoteDesktopWindow::sendRemoteRawMouseRelativeMove(int dx, int dy, Qt::MouseButtons buttons)
{
    if (dx == 0 && dy == 0) {
        return true;
    }

    const QSize remoteSize = remoteFrameSize();
    const int remoteButtons =
        (buttons & Qt::LeftButton ? 1 : 0) |
        (buttons & Qt::RightButton ? 2 : 0) |
        (buttons & Qt::MiddleButton ? 4 : 0);
    RemoteInputEvent event;
    event.type = RemoteInputEventType::RelativeMove;
    event.relativeX = remoteSize.width() > 0 ? double(dx) / double(remoteSize.width()) : 0.0; // wjy: 当前目标还原为原始设备计数；同步目标继续沿用既有按远端分辨率换算语义。
    event.relativeY = remoteSize.height() > 0 ? double(dy) / double(remoteSize.height()) : 0.0;
    event.fallbackDeltaX = dx; // wjy: 首帧远端尺寸未知时直接发送 Raw Input 计数，原有 ±200 协议保护仍在协调器和 Host 两端生效。
    event.fallbackDeltaY = dy;
    event.buttons = remoteButtons;
    dispatchRemoteInputEvent(event);
    return true;
}
// ===end====

// =====wjy====
bool RemoteDesktopWindow::setRawInputMouseCaptureEnabled(bool enabled)
{
#if defined(Q_OS_WIN)
    if (!enabled) {
        if (g_rawInputMouseTarget == this) {
            RAWINPUTDEVICE device{};
            device.usUsagePage = 0x01; // wjy: Generic Desktop Controls。
            device.usUsage = 0x02; // wjy: Mouse；RIDEV_REMOVE 时 hwndTarget 必须为空。
            device.dwFlags = RIDEV_REMOVE;
            device.hwndTarget = nullptr;
            ::RegisterRawInputDevices(&device, 1, sizeof(device)); // wjy: 即使系统返回失败也清除本对象状态，已销毁窗口不能继续被当作有效接收者。
            g_rawInputMouseTarget = nullptr;
        }
        m_rawInputMouseCaptureActive = false;
        return true;
    }

    if (m_rawInputMouseCaptureActive && g_rawInputMouseTarget == this) {
        return true;
    }
    if (!isActiveWindow()) {
        m_rawInputMouseCaptureActive = false;
        return false; // wjy: 后台远控窗口不抢占进程唯一的 Raw Input 鼠标目标，仍保留其远端连接和旧相对代码状态。
    }
    if (g_rawInputMouseTarget && g_rawInputMouseTarget != this) {
        g_rawInputMouseTarget->setRawInputMouseCaptureEnabled(false); // wjy: 活动远控窗口切换时先让旧窗口撤销注册，再把同一设备类安全交给本窗口。
    }

    const HWND targetWindow = reinterpret_cast<HWND>(winId());
    if (!targetWindow || !::IsWindow(targetWindow)) {
        m_rawInputMouseCaptureActive = false;
        return false;
    }
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01; // wjy: HID Generic Desktop Controls。
    device.usUsage = 0x02; // wjy: 仅注册鼠标原始位移；键盘继续使用现有低级钩子和快捷键门禁。
    device.dwFlags = 0; // wjy: 不使用 RIDEV_NOLEGACY，保留 Qt 按钮、滚轮和窗口交互消息，只有移动量改走 WM_INPUT。
    device.hwndTarget = targetWindow;
    if (!::RegisterRawInputDevices(&device, 1, sizeof(device))) {
        m_rawInputMouseCaptureActive = false;
        return false; // wjy: 注册失败时不影响远控相对模式，sendRemoteMouseMove 会自动回退原中心差值实现。
    }

    g_rawInputMouseTarget = this;
    m_rawInputMouseCaptureActive = true;
    return true;
#else
    Q_UNUSED(enabled)
    m_rawInputMouseCaptureActive = false;
    return false;
#endif
}
// ===end====

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
    } else if (active && !m_inputScriptDialogActive && !m_closeInProgress && !remoteUpdateActive() && isActiveWindow()
        && m_viewerHandle
        && RemoteConnectionState::acceptsRemoteInput(m_connectionStatusCode)) {
        installKeyboardHook(this); // wjy: 键鼠脚本命名或选择对话框打开时禁止重新安装转发Hook，用户输入只留在本地模态窗口。
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
    m_localShortcutReleaseKeys.clear();
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
        RemoteInputEvent event;
        event.type = RemoteInputEventType::KeyUp;
        event.virtualKey = key;
        dispatchRemoteInputEvent(event); // wjy: 焦点离开或关闭时，主控已广播的按键在所有跟随端同步抬起。
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
        RemoteInputEvent event;
        event.type = RemoteInputEventType::KeyUp;
        event.virtualKey = key;
        dispatchRemoteInputEvent(event); // wjy: 本地快捷键门禁释放继续走语义路径，避免同步组内残留 Ctrl、Shift 等修饰键。
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
        RemoteInputEvent event;
        event.type = RemoteInputEventType::KeyUp;
        event.virtualKey = key;
        dispatchRemoteInputEvent(event); // wjy: Windows 左右修饰键兜底抬起同样覆盖全部同步目标。
    }
#endif
}

void RemoteDesktopWindow::shutdownForApplicationExit()
{
    saveWindowGeometry();
    // =====wjy====
    cancelInputScriptRecording(QStringLiteral("application_exit"));
    stopInputScriptPlayback(QStringLiteral("application_exit"), true); // wjy: 进程批量退出仍在有效Viewer句柄清理前结束回放，避免远端留下脚本按住的键鼠。
    // ===end====
    m_closeInProgress = true;
    m_applicationExitInProgress = true; // wjy: 窗口删除权交给DeviceGrid，异步stop完成时不得通过deleteLater改变批量退出顺序。
    m_remoteUpdateState = RemoteUpdateState::None;
    m_remoteUpdateReconnectRequested = false;
    cancelNetworkReconnect(); // wjy: 应用退出后禁止任何退避定时器重新申请Viewer。
    if (m_remoteUpdateTimer) {
        m_remoteUpdateTimer->stop(); // wjy: 控制端自身退出时终止远端更新轮询，不再尝试恢复窗口。
    }
    m_waitingShortcutRelease = false;
    m_shortcutReleaseVirtualKeys.clear();
    hide();
    m_remoteMouseCaptureRequested = false;
    suspendRemoteMouseCapture(); // wjy: 应用退出不等待焦点事件，主动释放实际捕获；析构阶段会再做幂等兜底。
    releaseForwardedKeys();
    if (m_inputBroadcastCoordinator) {
        m_inputBroadcastCoordinator->unregisterEndpoint(this); // wjy: 应用退出隐藏窗口前注销成员，主控退出会同步关闭整组且补发剩余按钮释放。
    }
    setKeyboardForwardingActive(false);
    if (m_framePresentTimer) {
        m_framePresentTimer->stop();
    }
    invalidateViewerCallbacks(); // wjy: 应用批量退出时先作废回调并清空两类待呈现帧，禁止Presenter reset之后继续消费旧会话画面。
    if (m_sessionTimer) {
        m_sessionTimer->stop();
    }
    if (m_texturePresenter) {
        m_texturePresenter->reset();
    }
    // =====wjy====
    stopViewerConnectionAsync(false); // wjy: 批量退出只提交可等待stop；DeviceGrid随后统一join管理器，再按固定顺序删除全部窗口。
    // ===end====
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
        setWindowAndPresenterCursor(Qt::PointingHandCursor);
        return; // wjy: 更新入口使用手型指针；按钮远离 6px 缩放边缘，不影响窗口缩放命中。
    }
    if (!isFullScreen() && mouseInputModeRect().contains(position)) {
        setWindowAndPresenterCursor(Qt::PointingHandCursor);
        return; // wjy: 系统/驱动后端开关使用手型指针，全屏时该区域仍属于远端画面。
    }
    const int edges = resizeEdgesAt(position);
    if ((edges & ResizeLeft && edges & ResizeTop) || (edges & ResizeRight && edges & ResizeBottom)) {
        setWindowAndPresenterCursor(Qt::SizeFDiagCursor);
    } else if ((edges & ResizeRight && edges & ResizeTop) || (edges & ResizeLeft && edges & ResizeBottom)) {
        setWindowAndPresenterCursor(Qt::SizeBDiagCursor);
    } else if (edges & (ResizeLeft | ResizeRight)) {
        setWindowAndPresenterCursor(Qt::SizeHorCursor);
    } else if (edges & (ResizeTop | ResizeBottom)) {
        setWindowAndPresenterCursor(Qt::SizeVerCursor);
    } else if (remoteImageRect().contains(position)) {
        setWindowAndPresenterCursor(m_remoteCursorShape); // wjy: 只有远端图像区域跟随受控 Windows 光标，标题栏与黑边继续保持本地界面语义。
    } else {
        unsetWindowAndPresenterCursor();
    }
}

// =====wjy====
void RemoteDesktopWindow::setWindowAndPresenterCursor(Qt::CursorShape shape)
{
    setCursor(shape);
    if (m_texturePresenter) {
        m_texturePresenter->setCursor(shape); // wjy: D3D 子控件直接接收远控画面鼠标事件，显式同步可避免其系统箭头遮盖父窗口形状。
    }
}

void RemoteDesktopWindow::unsetWindowAndPresenterCursor()
{
    unsetCursor();
    if (m_texturePresenter) {
        m_texturePresenter->unsetCursor();
    }
}
// ===end====

void RemoteDesktopWindow::updateWindowMask()
{
    // =====wjy====
#if defined(Q_OS_WIN)
    const HWND windowHandle = reinterpret_cast<HWND>(winId());
    if (applyNativeWindowCorners(windowHandle, !isFullScreen())) {
        clearMask(); // wjy: 支持DWM圆角的Windows完全移除Qt QRegion，移动时不再重新合成带Region的标题栏表面。
        return;
    }
#endif
    // ===end====
    if (isFullScreen()) {
        clearMask(); // wjy: 全屏必须使用完整矩形窗口，避免普通窗口的圆角裁掉屏幕边缘像素。
        return;
    }

    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, width(), height()), 6, 6);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
}

RemoteWindowLayoutSnapshot RemoteDesktopWindow::compositorLayoutSnapshot() const
{
    RemoteWindowLayoutSnapshot snapshot;
    const qreal dpr = qMax<qreal>(1.0, devicePixelRatioF());
    const auto scaleRect = [dpr](const QRect& logical) {
        return QRect(
            qRound(logical.left() * dpr),
            qRound(logical.top() * dpr),
            qRound(logical.width() * dpr),
            qRound(logical.height() * dpr));
    };
    const QRect logicalContent = remoteContentRect();
    const QRect logicalImage = remoteImageRect();
    snapshot.physicalOutputRect = QRect(0, 0, qRound(width() * dpr), qRound(height() * dpr));
    snapshot.titleBarRect = isFullScreen()
        ? QRect()
        : scaleRect(QRect(0, 0, width(), titleBarHeight()));
    snapshot.contentRect = scaleRect(logicalContent);
    snapshot.inputRect = scaleRect(logicalImage.isValid() ? logicalImage : logicalContent);
    snapshot.outputSize = snapshot.contentRect.size();
    snapshot.sourceSize = m_remoteTextureSize.isValid()
        ? m_remoteTextureSize
        : m_remoteFrame.size();
    snapshot.devicePixelRatio = dpr;
    return snapshot;
}

void RemoteDesktopWindow::commitCompositorLayout()
{
    if (!m_unifiedCompositor || !m_unifiedCompositor->isEnabled()) {
        return;
    }
    const RemoteWindowLayoutSnapshot snapshot = compositorLayoutSnapshot();
    if (m_resizingWindow) {
        m_unifiedCompositor->updateInteractiveGeometry(snapshot);
    } else {
        m_unifiedCompositor->commitLayout(snapshot);
    }
}

void RemoteDesktopWindow::updateTexturePresenterGeometry()
{
    if (!m_texturePresenter) {
        return;
    }
    commitCompositorLayout(); // wjy: D3D 子表面和新合成器同时读取同一份几何快照，迁移期先不改变旧显示路径。
    const QRect target = remoteContentRect(); // wjy: 纹理子窗口覆盖完整内容区，视频和黑边由同一个SwapChain一次性合成。
    if (m_texturePresenter->usesCompositorSurface()) {
        m_texturePresenter->setCompositorOutputRect(
            compositorLayoutSnapshot().contentRect); // wjy: DirectComposition使用物理像素快照，避免高DPI下把Qt逻辑尺寸当成SwapChain尺寸。
        m_texturePresenter->hide(); // wjy: 隐藏Presenter控件本身，避免DComp视频与Qt子窗口形成重叠可见表面。
        return;
    }
    if (target.isEmpty() || !m_textureFrameActive) {
        m_texturePresenter->hide();
        return;
    }
    if (m_texturePresenter->geometry() != target) {
        m_texturePresenter->setGeometry(target); // wjy: 几何始终由Qt维护，原生WM_WINDOWPOSCHANGING阶段再注入SWP_NOREDRAW，避免Qt记录与实际HWND尺寸脱节。
    }
}

void RemoteDesktopWindow::beginResizeDebugTrace()
{
    ++m_resizeDebugTraceId; // wjy: 每次按下边缘都建立独立编号，连续测试可以在同一个日志文件中逐手势对照。
    m_resizeDebugClock.restart();
    m_lastResizePixelProbeMs = -1000;
    m_resizeVisibleSampleCount = 0;
    m_resizeDebugTrace.clear();
    m_resizeDebugTrace.reserve(256);
    appendResizeDebugTrace(QStringLiteral("gesture.begin")); // wjy: 首条记录固定标记手势起点，后续所有父/子窗口事件共享同一时间基准。
}

void RemoteDesktopWindow::appendResizeDebugTrace(const QString& stage)
{
    if (!m_resizeDebugClock.isValid() || m_resizeDebugTrace.size() >= 256) {
        return; // wjy: 诊断只保留有限内存样本，满载后不再分配，避免日志改变高频缩放时序。
    }

    const QRect parentGeometry = geometry();
    const QRect childGeometry = m_texturePresenter ? m_texturePresenter->geometry() : QRect();
    int parentClientWidth = width();
    int parentClientHeight = height();
#if defined(Q_OS_WIN)
    RECT parentClientRect = {};
    if (::GetClientRect(reinterpret_cast<HWND>(winId()), &parentClientRect)) {
        parentClientWidth = parentClientRect.right - parentClientRect.left;
        parentClientHeight = parentClientRect.bottom - parentClientRect.top;
    }
#endif
    const QString presenter = m_texturePresenter
        ? m_texturePresenter->resizeDebugSnapshot()
        : QStringLiteral("d3d{presenter=null}");
    m_resizeDebugTrace.append(QStringLiteral(
        "t=%1 stage=%2 parent=%3,%4 %5x%6 client=%7x%8 child=%9,%10 %11x%12 resizing=%13 texture_active=%14 visible=%15 %16")
        .arg(m_resizeDebugClock.elapsed())
        .arg(stage)
        .arg(parentGeometry.x())
        .arg(parentGeometry.y())
        .arg(parentGeometry.width())
        .arg(parentGeometry.height())
        .arg(parentClientWidth)
        .arg(parentClientHeight)
        .arg(childGeometry.x())
        .arg(childGeometry.y())
        .arg(childGeometry.width())
        .arg(childGeometry.height())
        .arg(m_resizingWindow ? 1 : 0)
        .arg(m_textureFrameActive ? 1 : 0)
        .arg(m_texturePresenter && m_texturePresenter->hasVisiblePresentation() ? 1 : 0)
        .arg(presenter)); // wjy: 同一行绑定父几何、子几何、显隐、纹理状态和D3D快照，直接判断哪一层先进入空档。
}

void RemoteDesktopWindow::flushResizeDebugTrace()
{
    if (!m_resizeDebugClock.isValid() || m_resizeDebugTrace.isEmpty()) {
        return;
    }

    const QString dataDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
    QDir().mkpath(dataDir);
    QFile file(QDir(dataDir).filePath(QStringLiteral("remote_resize_trace.log")));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
               << QStringLiteral(" host=") << m_hostIp
               << QStringLiteral(" trace=") << m_resizeDebugTraceId
               << Qt::endl;
        for (const QString& line : m_resizeDebugTrace) {
            stream << line << Qt::endl;
        }
        stream << QStringLiteral("-- end trace --") << Qt::endl;
    }
    m_resizeDebugTrace.clear();
    m_resizeDebugClock.invalidate(); // wjy: 写完后关闭当前手势时钟，下一次拖拽重新建立独立时间轴。
}

void RemoteDesktopWindow::sampleVisibleCompositorRegion()
{
    if (!m_resizePixelProbeEnabled
        || !m_resizingWindow
        || !m_resizeDebugClock.isValid()
        || !m_unifiedCompositor
        || !m_unifiedCompositor->isEnabled()) {
        return;
    }
    const qint64 elapsedMs = m_resizeDebugClock.elapsed();
    if (elapsedMs - m_lastResizePixelProbeMs < 100) {
        return; // wjy: 截图采样限频，避免诊断本身抢占拖拽和Present线程。
    }
    QScreen* screen = windowHandle() ? windowHandle()->screen() : nullptr;
    if (!screen) {
        screen = QGuiApplication::screenAt(frameGeometry().center());
    }
    if (!screen) {
        return;
    }
    const QRect globalRect = frameGeometry();
    const QPixmap screenshot = screen->grabWindow(
        0,
        globalRect.left(),
        globalRect.top(),
        globalRect.width(),
        globalRect.height());
    const QImage image = screenshot.toImage().convertToFormat(QImage::Format_RGB32);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return;
    }

    const int step = qMax(1, qMax(image.width(), image.height()) / 160);
    quint64 sampleCount = 0;
    quint64 blackCount = 0;
    quint64 luminanceSum = 0;
    for (int y = 0; y < image.height(); y += step) {
        for (int x = 0; x < image.width(); x += step) {
            const QColor color = image.pixelColor(x, y);
            const int luminance = (color.red() * 30 + color.green() * 59 + color.blue() * 11) / 100;
            ++sampleCount;
            luminanceSum += static_cast<quint64>(luminance);
            if (luminance <= 4) {
                ++blackCount;
            }
        }
    }
    if (sampleCount == 0) {
        return;
    }
    m_lastResizePixelProbeMs = elapsedMs;
    ++m_resizeVisibleSampleCount;
    const double blackRatio = static_cast<double>(blackCount) / static_cast<double>(sampleCount);
    const double averageLuminance = static_cast<double>(luminanceSum) / static_cast<double>(sampleCount);
    appendResizeDebugTrace(QStringLiteral(
        "visible.sample index=%1 size=%2x%3 samples=%4 black_ratio=%5 avg_luma=%6 state=%7 layout_rev=%8 frame=%9")
        .arg(static_cast<qulonglong>(m_resizeVisibleSampleCount))
        .arg(image.width())
        .arg(image.height())
        .arg(static_cast<qulonglong>(sampleCount))
        .arg(blackRatio, 0, 'f', 4)
        .arg(averageLuminance, 0, 'f', 1)
        .arg(static_cast<int>(m_unifiedCompositor->state()))
        .arg(static_cast<qulonglong>(m_unifiedCompositor->layout().revision))
        .arg(static_cast<qulonglong>(m_unifiedCompositor->lastFrameId()))); // wjy: 采样结果与同一时刻状态/布局/帧号关联，日志不再只记录WM_SIZE事件。
}

void RemoteDesktopWindow::enqueueRemoteFrame(QImage image, quint64 viewerGeneration)
{
    if (image.isNull() || !acceptsViewerGeneration(viewerGeneration)) {
        return;
    }

    QMutexLocker locker(&m_pendingFrameMutex);
    if (!acceptsViewerGeneration(viewerGeneration)) {
        return; // wjy: 等待BGRA锁期间若窗口已经关闭或重连，旧帧不能覆盖新会话pending。
    }
    m_pendingRemoteFrame = std::move(image); // wjy: Overwrite stale frames; the UI timer will pull the newest one when the event loop is ready.
}

int RemoteDesktopWindow::enqueueRemoteTextureFrame(
    int width,
    int height,
    void* sharedHandle,
    quint64 frameId,
    qint64 rtpTimestamp,
    qint64 renderTimeMs,
    quint64 decodedAtUs,
    double encodedMbps,
    quint64 viewerGeneration)
{
    if (!acceptsViewerGeneration(viewerGeneration)) {
        m_staleTextureFrameDrops.fetch_add(1, std::memory_order_relaxed); // wjy: 重连或关闭后的旧解码回调只记代际丢帧，不进入新Viewer队列。
        return FSREMOTE_TEXTURE_FRAME_DROPPED; // wjy: 让解码端立即归还生产者key并跳过BGRA回读，旧画面绝不进入新会话Presenter。
    }
    if (m_texturePresentFailed.load() || width <= 0 || height <= 0 || !sharedHandle) {
        return FSREMOTE_TEXTURE_FRAME_FALLBACK;
    }

    // =====wjy====
    TextureFramePushResult pushResult = m_pendingTextureFrames.push({
        width,
        height,
        sharedHandle,
        frameId,
        rtpTimestamp,
        renderTimeMs,
        decodedAtUs,
        encodedMbps,
        viewerGeneration,
    }); // wjy: 单槽为空才接管纹理；槽已占用时保留旧pending并让生产端回收本次新帧。
    if (pushResult.disposition == TextureFramePushDisposition::DroppedBecausePending) {
        return FSREMOTE_TEXTURE_FRAME_DROPPED; // wjy: 解码回调线程不再调用Qt Presenter，生产端在自己的D3D设备上直接ReleaseSync回key 0。
    }
    if (pushResult.disposition == TextureFramePushDisposition::AcceptedAndScheduleDrain
        && !QMetaObject::invokeMethod(this, &RemoteDesktopWindow::drainPendingRemoteTextureFrame, Qt::QueuedConnection)) {
        m_pendingTextureFrames.cancelScheduledDrain(); // wjy: Qt对象拒绝投递时释放唯一调度权，让原生解码器立即回退到安全BGRA路径。
        return FSREMOTE_TEXTURE_FRAME_FALLBACK;
    }
    // ===end====
    return FSREMOTE_TEXTURE_FRAME_ACCEPTED;
}

void RemoteDesktopWindow::discardPendingTextureFrame()
{
    const auto cancelledFrame = m_pendingTextureFrames.cancelPending(); // wjy: 从单槽原子取出被取消的已接受帧。
    if (cancelledFrame.has_value() && m_texturePresenter && cancelledFrame->sharedHandle) {
        m_texturePresenter->discardSharedTexture(cancelledFrame->sharedHandle); // wjy: Acquire消费者key后立即Release生产者key，不执行任何画面呈现。
    }
}

// =====wjy====
void RemoteDesktopWindow::drainPendingRemoteTextureFrame()
{
    const auto frame = m_pendingTextureFrames.takeLatest(); // wjy: 每次Qt任务直接取当前最新帧，积压期间被覆盖的旧桌面画面永远不会补播。
    if (frame.has_value()
        && acceptsViewerGeneration(frame->viewerGeneration)
        && !isClosingConnection()
        && !m_texturePresentFailed.load()
        && m_texturePresenter
        && remoteUpdateAcceptsFrames()) {
        // =====wjy====
        const bool hadSuccessfulTexture = m_textureFrameActive && m_texturePresenter->hasPresentedFrame(); // wjy: 记录尝试前是否已有可继续显示的D3D前台画面。
        m_textureFrameActive = true; // wjy: 首帧尝试前先设置子窗口几何，但暂不清空软件帧或隐藏旧纹理。
        updateTexturePresenterGeometry();
        bool texturePresented = false;
        try {
            texturePresented = m_texturePresenter->presentSharedTexture(frame->sharedHandle, frame->width, frame->height);
        } catch (...) {
            texturePresented = false; // wjy: D3D11共享设备创建中的分配异常也转换为本窗口软件回退，不允许异常逃出Qt队列任务。
        }
        if (!texturePresented) {
            if (m_unifiedCompositor && m_unifiedCompositor->isEnabled()) {
                if (m_texturePresenter->lastFailureWasDeviceLost()) {
                    m_unifiedCompositor->enterDeviceRecovery(); // wjy: 设备级失败进入恢复态，统一表面后续必须保留最后有效帧或软件帧。
                } else {
                    m_unifiedCompositor->enterHardwareFallback(); // wjy: 普通共享纹理失败只标记硬件回退，不立即清空当前可见布局。
                }
            }
            ++m_textureFailureCount;
            appendViewerDebugLog(QStringLiteral("D3D11 presenter failed host=%1 reason=0x%2 retry=%3")
                .arg(m_hostIp)
                .arg(static_cast<qulonglong>(static_cast<unsigned long>(m_texturePresenter->lastDeviceRemovalReason())), 0, 16)
                .arg(m_textureFailureCount)); // wjy: 记录窗口、HRESULT和连续次数，GPU故障只形成低频诊断而不抛出异常。
            const bool canContinueDisplayingTexture = hadSuccessfulTexture && m_texturePresenter->hasPresentedFrame(); // wjy: 设备移除路径会在Presenter内部撤销此状态，不能继续把已释放SwapChain当作可见上一帧。
            const bool keepLastFrame = canContinueDisplayingTexture
                && !m_texturePresenter->lastFailureWasDeviceLost()
                && m_textureFailureCount < kTextureFailuresBeforeSoftwareFallback; // wjy: 普通单帧句柄竞争不触发黑屏或昂贵BGRA回退，直接等待下一张最新纹理。
            if (keepLastFrame) {
                m_textureFrameActive = true;
                if (m_texturePresenter->usesCompositorSurface()) {
                    m_texturePresenter->setPresentationVisible(true); // wjy: DComp路径恢复视频视觉透明度，不显示备用子控件。
                } else if (!m_texturePresenter->isVisible()) {
                    m_texturePresenter->setPresentationVisible(true);
                    m_texturePresenter->raise(); // wjy: 只有旧SwapChain此前被隐藏时才恢复层级，连续失败帧不重复操作原生窗口Z序。
                }
            } else {
                m_texturePresentFailed.store(true); // wjy: 连续失败或真实设备移除只切换当前窗口软件保活，不影响其它十九路会话。
                m_softwareFallbackActive = true;
                m_textureFrameActive = canContinueDisplayingTexture; // wjy: BGRA首帧到达前仅保留仍有效的旧D3D画面，设备已移除时立即回到安全背景。
                if (!canContinueDisplayingTexture) {
                    m_texturePresenter->setPresentationVisible(false);
                }
                discardPendingTextureFrame(); // wjy: 软件回退取消排队纹理时同步归还keyed mutex槽位。
                const int retryDelayMs = qMin(10000, 1000 * (1 << qMin(m_textureFailureCount - 1, 3)));
                if (m_textureRecoveryTimer) {
                    m_textureRecoveryTimer->start(retryDelayMs); // wjy: 1/2/4/8/10秒退避重试，持续故障时不每帧重建设备。
                }
                emit remoteQualityInputsChanged(); // wjy: 连续失败才下发540p/24 FPS软件保活档，单帧抖动不再造成画质来回切换。
                requestTitleBarUpdate(); // wjy: 软件回退状态真正变化时刷新一次标题栏，不把单帧失败变成持续重绘。
            }
        } else {
            updatePresentedFrameStats(0); // wjy: 共享纹理成功Present后计入FPS；零拷贝路径不虚构BGRA内存吞吐。
            if (m_unifiedCompositor && m_unifiedCompositor->isEnabled()) {
                m_unifiedCompositor->markHardwareFrame(frame->frameId, QSize(frame->width, frame->height)); // wjy: 只有真正Present成功的共享纹理才推进统一合成器帧号。
            }
            m_remoteTextureSize = QSize(frame->width, frame->height); // wjy: 只有新纹理真正成功Present后才提交远端尺寸，失败帧不能污染缩放比例。
            m_remoteFrame = QImage(); // wjy: 成功切换到新纹理后才释放软件上一帧，整个提交过程不会提前露出黑色背景。
            m_encodedMbps = qMax(0.0, frame->encodedMbps); // wjy: 码率统计只跟随真正成功显示的新帧。
            const bool recoveredFromSoftwareFallback = m_softwareFallbackActive; // wjy: 普通单帧失败恢复不触发全局画质重算，只有真正退出BGRA保活才通知协调器。
            if (m_texturePresenter->usesCompositorSurface()
                && (!hadSuccessfulTexture || recoveredFromSoftwareFallback)) {
                presentCompositorOverlay(); // wjy: 仅首个硬件帧或软件转硬件时清理旧BGRA/连接内容；正常视频帧只Present视频SwapChain，不再逐帧重建整窗Overlay。
            }
            const bool connectionTitleChanged = m_connectionStatusCode != 50
                || m_connectionStatus != QString::fromUtf8("画面已接收")
                || m_networkWarningVisible; // wjy: 恢复首帧必须同时清除静态网络提示，后续正常视频帧不重复重画。
            if (m_textureFailureCount > 0) {
                m_textureFailureCount = 0;
                m_softwareFallbackActive = false;
                if (m_textureRecoveryTimer) m_textureRecoveryTimer->stop();
                if (recoveredFromSoftwareFallback) {
                    emit remoteQualityInputsChanged(); // wjy: D3D11从软件保活恢复后，协调器按先FPS后分辨率的滞回策略逐档恢复。
                }
            }
            m_connectionStatusCode = 50;
            m_connectionStatus = QString::fromUtf8("画面已接收");
            m_hasReceivedVideoInCurrentViewer = true;
            if (m_networkReconnectActive) {
                finishNetworkReconnect(); // wjy: 共享纹理已成功Present，至此才确认网络、解码和DComp整条路径恢复。
            } else {
                m_networkWarningVisible = false;
            }
            if (!m_remoteMouseBackendKnown && !m_remoteMouseBackendPending) queryRemoteMouseBackend(); // wjy: 纹理首帧若先于状态回调到达，也会在输入资格建立后立即同步 Host 全局后端。
            if (m_texturePresenter->usesCompositorSurface()) {
                m_texturePresenter->setPresentationVisible(true); // wjy: DComp首帧提交后只恢复视频视觉，不再把子控件带回可见层。
            } else if (!m_texturePresenter->isVisible()) {
                m_texturePresenter->setPresentationVisible(true);
                m_texturePresenter->raise(); // wjy: 首帧或BGRA回切D3D时才提升Presenter，正常60 FPS Present不再逐帧改变Z序。
            }
            if (m_remoteUpdateState == RemoteUpdateState::Reconnecting) {
                finishRemoteUpdateWait(); // wjy: 共享纹理首帧成功呈现后才移除更新遮罩并恢复输入。
            }
            updateRemoteMouseCaptureState(); // wjy: 即使状态回调先后顺序异常，只要共享纹理已成功显示，也会恢复仍开启的 F2 手动锁定。
            if (connectionTitleChanged || recoveredFromSoftwareFallback) {
                requestTitleBarUpdate(); // wjy: 60 FPS纹理呈现不再连带重画标题栏，只提交真实可见状态变化。
            }
        }
        // ===end====
    } else if (frame.has_value() && m_texturePresenter && frame->sharedHandle) {
        m_texturePresenter->discardSharedTexture(frame->sharedHandle); // wjy: 关闭、代际失效或更新遮罩拒绝已取出的帧时仍完成消费者key交接。
    }

    if (m_pendingTextureFrames.completeDrainAndShouldReschedule()) {
        if (!QMetaObject::invokeMethod(this, &RemoteDesktopWindow::drainPendingRemoteTextureFrame, Qt::QueuedConnection)) {
            const auto cancelledFrame = m_pendingTextureFrames.cancelScheduledDrain(); // wjy: 极端关闭竞态下清理调度标志并取回已经接受的pending帧。
            if (cancelledFrame.has_value() && m_texturePresenter && cancelledFrame->sharedHandle) {
                m_texturePresenter->discardSharedTexture(cancelledFrame->sharedHandle); // wjy: 续投失败的帧已由生产端交给消费者key，必须显式归还。
            }
        }
    }
}
// ===end====

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
    updateFrameStats(image); // wjy: Count received UI frames before repainting so the title bar reflects the latest decoded BGRA flow.
    const bool connectionTitleChanged = m_connectionStatusCode != FSREMOTE_STATUS_RECEIVING_VIDEO
        || m_connectionStatus != QString::fromUtf8("画面已接收")
        || m_networkWarningVisible; // wjy: 软件帧恢复与共享纹理恢复遵循同一标题栏清理条件。
    // =====wjy====
    const bool textureWasVisible = m_textureFrameActive
        && m_texturePresenter
        && m_texturePresenter->hasVisiblePresentation(); // wjy: 软件回退首帧到达时同时识别旧子HWND或DComp视觉树是否仍覆盖内容区。
    m_remoteFrame = image;
    m_textureFrameActive = false; // wjy: 先让父窗口绘制新的软件帧，但暂时不隐藏覆盖在上面的最后成功纹理。
    const QRect contentRect = remoteContentRect(); // wjy: 软件回退与D3D11路径共用完全相同的标题栏下内容边界。
    if (m_texturePresenter && m_texturePresenter->usesCompositorSurface()) {
        presentCompositorOverlay(); // wjy: 先把BGRA帧提交到同一个DComp叠加SwapChain，再隐藏硬件视觉，避免硬件到软件切换露出黑色中间帧。
        m_texturePresenter->setPresentationVisible(false);
    } else if (textureWasVisible) {
        repaint(contentRect); // wjy: 同步把BGRA首帧画到D3D子窗口背后，随后隐藏子窗口时不会短暂露出黑色背景。
        m_texturePresenter->setPresentationVisible(false);
    } else if (m_texturePresenter) {
        m_texturePresenter->setPresentationVisible(false);
    }
    // ===end====
    m_connectionStatusCode = 50;
    m_connectionStatus = QString::fromUtf8("画面已接收");
    m_hasReceivedVideoInCurrentViewer = true;
    if (m_networkReconnectActive) {
        finishNetworkReconnect(); // wjy: 软件帧真实进入显示路径后结束无限重连，行为与共享纹理路径一致。
    } else {
        m_networkWarningVisible = false;
    }
    if (!m_remoteMouseBackendKnown && !m_remoteMouseBackendPending) queryRemoteMouseBackend(); // wjy: BGRA 首帧与纹理路径使用相同查询门禁，事件先后顺序不同也只发送一次。
    if (m_remoteUpdateState == RemoteUpdateState::Reconnecting) {
        finishRemoteUpdateWait(); // wjy: 软件帧路径同样以首帧到达作为恢复正常远控的唯一完成点。
    }
    updateRemoteMouseCaptureState(); // wjy: BGRA 回退路径与共享纹理路径使用同一手动锁定恢复条件，不依赖状态回调到达顺序。
    if (!textureWasVisible) {
        update(contentRect); // wjy: 普通软件帧继续异步刷新；只有D3D→BGRA交接首帧使用一次同步重绘。
    }
    if (connectionTitleChanged) {
        requestTitleBarUpdate(); // wjy: 恢复时立即从标题栏移除“网络不佳”，不等待下一次一秒计时刷新。
    }
    // appendViewerDebugLog(QStringLiteral("setRemoteFrame update requested")); // wjy: per-frame repaint log disabled to avoid disk IO on every frame.
}

void RemoteDesktopWindow::updateFrameStats(const QImage& image)
{
    if (image.isNull()) {
        return;
    }
    updateFrameColorStats(image); // wjy: Keep RGB diagnostics visible while testing pure-black remote pages.
    updatePresentedFrameStats(static_cast<qint64>(image.sizeInBytes())); // wjy: 软件帧携带真实BGRA字节数，同时和共享纹理共用同一FPS时间窗。
    if (m_unifiedCompositor && m_unifiedCompositor->isEnabled()) {
        m_unifiedCompositor->markSoftwareFrame(m_totalPresentedFrames, image.size()); // wjy: BGRA回退沿用同一布局和帧计数，避免硬件/软件路径各自维护可见状态。
    }
}

void RemoteDesktopWindow::updatePresentedFrameStats(qint64 bgraBytes)
{
    if (!m_frameStatsClock.isValid()) {
        m_frameStatsClock.start(); // wjy: Defensive start in case a frame arrives before the constructor timer state is valid.
    }

    ++m_frameStatsCount; // wjy: 只统计成功进入软件显示或D3D11 Present的帧，协调器据此判断真实可见流畅度。
    ++m_totalPresentedFrames; // wjy: 长周期单调计数供协调器与纹理单槽拒绝数求差，不影响一秒标题栏统计。
    m_frameStatsBytes += qMax<qint64>(0, bgraBytes); // wjy: 仅软件帧累计BGRA字节，共享纹理保持零以反映零拷贝路径。
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

// =====wjy====
bool RemoteDesktopWindow::acceptsViewerGeneration(quint64 viewerGeneration) const
{
    return viewerGeneration != 0
        && viewerGeneration == m_viewerGeneration.load(std::memory_order_acquire); // wjy: 只有当前非零代际可进入复制、排队和UI状态路径，停止时递增即可一次性拒绝全部旧回调。
}

void RemoteDesktopWindow::invalidateViewerCallbacks()
{
    m_viewerGeneration.fetch_add(1, std::memory_order_acq_rel); // wjy: 先推进代际，原生线程从这一刻起无法再把旧帧或旧断线状态提交到窗口。
    m_lastQualityViewerGeneration = 0; // wjy: 下一次Viewer即使使用相同档位也必须重新补发，旧代际去重状态立即失效。
    m_lastAudioViewerGeneration = 0;
    m_hasLastSentViewerAudioState = false; // wjy: 停止或重连后必须向新Viewer重新下发当前唯一音频所有权。
    m_qualityRequestPending = false;
    m_hasAppliedQualityStatus = false;
    m_qualityProtocolUnavailable = false; // wjy: 重连后重新探测新Host能力，不能沿用上一连接的“不支持”结论。
    m_texturePresentFailed.store(false, std::memory_order_release);
    m_softwareFallbackActive = false;
    m_textureFailureCount = 0;
    if (m_textureRecoveryTimer) m_textureRecoveryTimer->stop(); // wjy: 新Viewer代际重新尝试纹理路径，不继承旧连接的D3D11失败退避。
    discardPendingTextureFrame(); // wjy: 清掉旧代际纹理并完成消费者交接，旧解码器停止前不会因槽位泄漏卡住。
    QMutexLocker locker(&m_pendingFrameMutex);
    m_pendingRemoteFrame = QImage(); // wjy: 软件回退路径也同步清空，重连首帧之前不会显示旧会话画面。
}
// ===end====

bool RemoteDesktopWindow::forwardNativeKey(int virtualKey, bool down)
{
    if (m_closeInProgress || remoteUpdateActive() || virtualKey <= 0) {
        return false;
    }
    if (m_inputScriptPlaying) {
        return true; // wjy: 设置页当前播放快捷键已在本地层优先消费，播放期间其余实体键盘事件直接吞掉且不进入远端持有集合。
    }
    if (down) {
        m_pressedKeys.insert(virtualKey);
    } else {
        m_pressedKeys.remove(virtualKey);
    }
    RemoteInputEvent event;
    event.type = down ? RemoteInputEventType::KeyDown : RemoteInputEventType::KeyUp;
    event.virtualKey = virtualKey;
    dispatchRemoteInputEvent(event); // wjy: 低级键盘钩子与 Qt 兜底路径统一经过语义事件，主控快捷键过滤后才会广播。
    return true;
}

bool RemoteDesktopWindow::handleLocalShortcutKey(int virtualKey, Qt::KeyboardModifiers modifiers)
{
#if defined(Q_OS_WIN)
    if (m_closeInProgress) {
        return false;
    }
    // =====wjy====
    const QKeySequence current = shortcutSequenceFromNativeVirtualKey(virtualKey, modifiers); // wjy: 低级键盘 Hook 把当前物理键转换为设置页同一套 QKeySequence。
    const QKeySequence mouseLockShortcut = platform::AppSettings::remoteShortcutMouseLock();
    if (matchesShortcut(current, mouseLockShortcut)) {
        beginShortcutReleaseGuard(mouseLockShortcut); // wjy: 自定义鼠标锁定组合的修饰键和主键都会进入释放门禁，避免被控端残留 Ctrl/Shift。
        QMetaObject::invokeMethod(this, [this] {
            toggleManualMouseLock(); // wjy: 通过 Qt 队列切换状态，低级键盘 Hook 回调栈内不直接注册 Raw Input 或刷新标题栏。
        }, Qt::QueuedConnection);
        return true;
    }
    if (remoteUpdateActive()) {
        return false; // wjy: 更新期间仍允许手动鼠标锁定修改保留意图，但录制、回放和其它远端快捷键继续保持禁用。
    }
    const QKeySequence recordingShortcut = platform::AppSettings::remoteShortcutInputScriptRecording();
    if (matchesShortcut(current, recordingShortcut)) {
        beginShortcutReleaseGuard(recordingShortcut); // wjy: F9 默认值现在来自设置页，自定义组合仍保持原有松键保护。
        QMetaObject::invokeMethod(this, [this] {
            toggleInputScriptRecording(); // wjy: 模态命名框只在 Qt 队列执行，绝不在低级 Hook 回调栈内打开。
        }, Qt::QueuedConnection);
        return true;
    }
    const QKeySequence playbackShortcut = platform::AppSettings::remoteShortcutInputScriptPlayback();
    if (matchesShortcut(current, playbackShortcut)) {
        beginShortcutReleaseGuard(playbackShortcut); // wjy: F10 默认值现在来自设置页，自定义组合也会先释放已转发的修饰键。
        QMetaObject::invokeMethod(this, [this] {
            toggleInputScriptPlayback(); // wjy: 播放设置窗口和再次按键停止逻辑继续复用原有入口。
        }, Qt::QueuedConnection);
        return true;
    }
    // ===end====
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

// =====wjy====
void RemoteDesktopWindow::beginNetworkRecoveryGracePeriod()
{
    if (m_closeInProgress || m_applicationExitInProgress || remoteUpdateActive()
        || m_hostIp.trimmed().isEmpty()
        || (!m_hasReceivedVideoInCurrentViewer && !m_networkReconnectActive)) {
        return; // wjy: 只为已经正常出画或已经处于恢复流程的窗口无限等待，首次配置/鉴权错误仍保留明确失败结果。
    }

    m_networkReconnectActive = true;
    m_networkRecoveryGraceActive = true;
    m_networkWarningVisible = true;
    if (m_networkReconnectTimer) {
        m_networkReconnectTimer->start(3000); // wjy: Disconnected或ICE刚恢复都等待最多三秒真实画面，短时抖动不销毁昂贵会话。
    }
    requestTitleBarUpdate();
}

void RemoteDesktopWindow::beginNetworkReconnect()
{
    if (m_closeInProgress || m_applicationExitInProgress || remoteUpdateActive()
        || m_hostIp.trimmed().isEmpty()
        || (m_lifecycleManager && m_lifecycleManager->isApplicationShuttingDown())) {
        cancelNetworkReconnect();
        return;
    }

    m_networkReconnectActive = true;
    m_networkRecoveryGraceActive = false;
    m_networkWarningVisible = true;
    if (m_networkReconnectTimer) m_networkReconnectTimer->stop();
    if (m_viewerHandle || m_viewerStopInProgress || m_viewerStartQueued || m_viewerStartAdmissionActive) {
        stopViewerConnectionAsync(false); // wjy: 旧Viewer及其回调上下文必须完全销毁，finishViewerStop之后才能安排新代际。
        return;
    }
    scheduleNetworkReconnect();
}

void RemoteDesktopWindow::scheduleNetworkReconnect()
{
    if (!m_networkReconnectActive || m_closeInProgress || m_applicationExitInProgress
        || remoteUpdateActive() || m_hostIp.trimmed().isEmpty()
        || (m_lifecycleManager && m_lifecycleManager->isApplicationShuttingDown())) {
        cancelNetworkReconnect();
        return;
    }
    if (m_networkReconnectTimer && m_networkReconnectTimer->isActive()
        && !m_networkRecoveryGraceActive) {
        return; // wjy: stop异常状态回调和finishViewerStop可能连续请求调度，同一轮只保留一个退避定时器且不重复增加次数。
    }

    static constexpr int kRetryDelaysMs[] = {1000, 2000, 4000, 8000, 10000};
    const int delayIndex = qMin(m_networkReconnectAttempt, 4);
    const int delayMs = kRetryDelaysMs[delayIndex];
    m_networkReconnectAttempt = qMin(m_networkReconnectAttempt + 1, 5); // wjy: 只保存当前退避档位；第五档后固定10秒无限重试，长期运行不会发生整数溢出。
    m_networkRecoveryGraceActive = false;
    m_networkWarningVisible = true;
    m_connectionStatusCode = FSREMOTE_STATUS_NETWORK_RECOVERING;
    m_connectionStatus = QString::fromUtf8("网络不佳，%1 秒后自动重连").arg(delayMs / 1000);
    if (m_networkReconnectTimer) {
        m_networkReconnectTimer->start(delayMs);
    }
    appendViewerDebugLog(QStringLiteral("network reconnect scheduled host=%1 attempt=%2 delay_ms=%3")
        .arg(m_hostIp)
        .arg(m_networkReconnectAttempt)
        .arg(delayMs)); // wjy: 低频记录每轮退避，现场日志可确认程序仍在无限等待而不是卡死。
    update(isFullScreen() ? rect() : QRect(0, titleBarHeight(), width(), height() - titleBarHeight()));
    requestTitleBarUpdate();
}

void RemoteDesktopWindow::attemptNetworkReconnect()
{
    if (!m_networkReconnectActive || m_closeInProgress || m_applicationExitInProgress
        || remoteUpdateActive() || m_hostIp.trimmed().isEmpty()
        || (m_lifecycleManager && m_lifecycleManager->isApplicationShuttingDown())) {
        cancelNetworkReconnect();
        return;
    }
    if (m_viewerHandle || m_viewerStopInProgress || m_viewerStartQueued || m_viewerStartAdmissionActive) {
        beginNetworkReconnect();
        return; // wjy: 极端迟到状态下先收敛旧生命周期，禁止同一个窗口并行创建两个Viewer。
    }

    m_connectionStatusCode = FSREMOTE_STATUS_NETWORK_RECOVERING;
    m_connectionStatus = QString::fromUtf8("网络不佳，正在重新连接");
    appendViewerDebugLog(QStringLiteral("network reconnect attempt host=%1 attempt=%2")
        .arg(m_hostIp)
        .arg(m_networkReconnectAttempt));
    update(isFullScreen() ? rect() : QRect(0, titleBarHeight(), width(), height() - titleBarHeight()));
    requestTitleBarUpdate();
    startViewerConnection(); // wjy: 继续复用四路初始化准入队列，多窗口同时恢复时不会瞬间压满CPU和网络线程。
}

void RemoteDesktopWindow::finishNetworkReconnect()
{
    if (!m_networkReconnectActive && !m_networkWarningVisible) {
        return;
    }
    if (m_networkReconnectTimer) m_networkReconnectTimer->stop();
    m_networkReconnectActive = false;
    m_networkRecoveryGraceActive = false;
    m_networkReconnectAttempt = 0;
    m_networkWarningVisible = false;
    if (m_clipboardSyncEnabled && m_clipboardPollTimer) {
        m_clipboardPollTimer->start();
    }
    if (isActiveWindow()) {
        setKeyboardForwardingActive(true); // wjy: 第一帧已经成功显示后才重新安装键盘hook，等待期间不会向失效句柄发送输入。
    }
    updateRemoteMouseCaptureState(); // wjy: 网络恢复以真实首帧为准，只有此时才允许 F2 手动锁定重新占用 Raw Input。
    if (m_inputBroadcastCoordinator) {
        m_inputBroadcastCoordinator->notifyEligibilityChanged(this);
    }
    appendViewerDebugLog(QStringLiteral("network reconnect recovered host=%1").arg(m_hostIp));
    requestTitleBarUpdate();
}

void RemoteDesktopWindow::cancelNetworkReconnect()
{
    if (m_networkReconnectTimer) m_networkReconnectTimer->stop();
    const bool warningChanged = m_networkWarningVisible;
    m_networkReconnectActive = false;
    m_networkRecoveryGraceActive = false;
    m_networkReconnectAttempt = 0;
    m_networkWarningVisible = false;
    if (warningChanged && !m_closeInProgress && !m_applicationExitInProgress) {
        requestTitleBarUpdate();
    }
}
// ===end====

void RemoteDesktopWindow::startViewerConnection()
{
    // =====wjy====
    appendViewerDebugLog(QStringLiteral("startViewerConnection request host=%1").arg(m_hostIp)); // wjy: 记录窗口申请共享初始化名额，而不是误认为已经进入原生连接。
    // ===end====
    if (m_viewerHandle || m_viewerStopInProgress || m_closeInProgress
        || m_viewerStartQueued || m_viewerStartAdmissionActive) {
        return; // wjy: 更新重连必须等待旧 viewer 完全停止，禁止同一窗口同时持有两个流会话。
    }

    // =====wjy====
    if (!m_lifecycleManager) {
        startViewerConnectionWithAdmission(); // wjy: 非DeviceGrid测试/兜底环境仍可运行，但正式多窗口路径始终使用共享管理器。
        return;
    }
    m_viewerStartQueued = true; // wjy: 先标记排队，管理器若立即授予名额会在受控入口同步改成active。
    m_connectionStatusCode = 5;
    m_connectionStatus = QString::fromUtf8("等待远控初始化资源"); // wjy: 第5个及后续窗口先显示等待状态，窗口本身已创建且可移动/关闭。
    if (m_texturePresenter && m_texturePresenter->usesCompositorSurface()) {
        presentCompositorOverlay(); // wjy: 排队状态也要同步到DComp叠加层，不能只刷新被其覆盖的Qt父窗口。
    }
    update();
    if (!m_lifecycleManager->requestViewerStart(this)) {
        m_viewerStartQueued = false;
        if (!m_closeInProgress) {
            setConnectionStatus(90, QString::fromUtf8("程序正在退出，未启动新的远控连接")); // wjy: 退出门禁拒绝新Viewer时只影响当前尚未连接的窗口。
        }
    }
    // ===end====
}

void RemoteDesktopWindow::startViewerConnectionWithAdmission()
{
    // =====wjy====
    m_viewerStartQueued = false;
    m_viewerStartAdmissionActive = true; // wjy: 从此刻到连接成功/失败/stop完成期间占用一个初始化预算。
    m_hasReceivedVideoInCurrentViewer = false; // wjy: 新Viewer独立判断是否曾正常出画，首次连接错误不能继承旧会话的网络警告依据。
    if (!m_networkReconnectActive) {
        m_networkWarningVisible = false; // wjy: 普通首次连接清理历史提示；无限重连中的新代际必须继续显示“网络不佳”。
    } else if (m_networkReconnectTimer) {
        m_networkRecoveryGraceActive = true;
        m_networkReconnectTimer->start(15000); // wjy: 重试Viewer取得初始化名额后最多等待15秒首帧，半连接状态不会永久卡在“正在连接”。
    }
    m_remoteCursorShape = Qt::ArrowCursor; // wjy: 替换 Viewer 会话不得继承旧 Host 最后的缩放形状；旧 Host 或暂未发送状态时保持普通箭头。
    m_remoteMouseBackend = RemoteMouseBackend::System;
    m_pendingRemoteMouseBackend = RemoteMouseBackend::System;
    m_remoteMouseBackendKnown = false;
    m_remoteMouseBackendPending = false;
    m_remoteMouseBackendFallback = false;
    m_mouseBackendButtonPressed = false;
    m_remoteMouseCaptureRequested = false; // wjy: 新 Viewer 从无自动 relative 请求开始，旧连接状态不得跨代际继承。
    ++m_remoteMouseBackendRequestGeneration; // wjy: 新 Viewer 代际从安全系统默认重新查询，旧连接迟到确认和超时都不能污染本次标题栏。
    m_remoteMouseBackendMessage = zh("等待目标端确认键鼠模式");
    if (!m_remoteMouseCaptureActive) {
        updateResizeCursor(mapFromGlobal(QCursor::pos())); // wjy: 鼠标静止在远端画面内时也立即清除上一会话遗留光标，无需等待下一次移动。
    }
    appendViewerDebugLog(QStringLiteral("startViewerConnection admitted host=%1").arg(m_hostIp));
    if (m_viewerHandle || m_viewerStopInProgress || m_closeInProgress) {
        releaseViewerStartupAdmission(); // wjy: 排队期间状态已变化时不再创建原生Viewer，并立即把名额交给下一窗口。
        return;
    }
    if (!m_hostIp.trimmed().isEmpty()) {
        const quint64 generation = m_viewerGeneration.fetch_add(1, std::memory_order_acq_rel) + 1; // wjy: 每次真正创建viewer都取得全新非零代际，旧连接任何迟到回调都无法命中新值。
        auto callbackContext = std::make_shared<RemoteDesktopViewerCallbackContext>();
        callbackContext->window = this; // wjy: QPointer在窗口销毁时自动清空，同时shared_ptr保证原生stop返回前上下文本身不被释放。
        callbackContext->generation = generation;
        m_viewerCallbackContext = callbackContext; // wjy: 当前窗口持有活动上下文，异步stop时会把所有权移动到停止任务中。
        try {
            m_viewerHandle = stream::StreamRuntime::instance().startViewer(
                m_hostIp.trimmed(),
                49100,
                onRemoteFrame,
                onRemoteTextureFrame,
                onViewerStatus,
                callbackContext.get()); // wjy: 真正的原生初始化只能从管理器授予名额后的入口发生，同时最多4路。
        } catch (const std::exception& exception) {
            m_viewerHandle = nullptr;
            setConnectionStatus(90, QString::fromUtf8("启动远控连接失败：%1")
                .arg(QString::fromUtf8(exception.what()))); // wjy: 单个Viewer分配/运行异常被限制在当前窗口，不能触发整个程序退出。
        } catch (...) {
            m_viewerHandle = nullptr;
            setConnectionStatus(90, QString::fromUtf8("启动远控连接时发生未知异常")); // wjy: 未知异常同样转成窗口级失败状态。
        }
        appendViewerDebugLog(QStringLiteral("startViewerConnection handle=%1").arg(reinterpret_cast<quintptr>(m_viewerHandle))); // wjy: record whether the DLL returned a handle.
        if (!m_viewerHandle) {
            invalidateViewerCallbacks(); // wjy: 原生入口创建失败时立即作废刚分配的代际，任何异常迟到回调都只能被拒绝。
            m_viewerCallbackContext.reset();
            if (m_connectionStatusCode != 90) {
                setConnectionStatus(90, stream::StreamRuntime::instance().lastError());
            }
            releaseViewerStartupAdmission(); // wjy: 创建失败不继续占用初始化预算，下一台设备可以立即补位。
        } else {
            sendCurrentRemoteQualityDecision(); // wjy: 窗口排队初始化期间若全局/局部策略已变化，只向新Viewer补发最终最新值。
            sendCurrentViewerAudioDecision(); // wjy: Viewer默认静音，创建成功后立即补发本窗口当前音频按钮状态。
        }
    } else {
        setConnectionStatus(90, QString::fromUtf8("设备 IP 为空"));
        releaseViewerStartupAdmission();
    }
    // ===end====
}

void RemoteDesktopWindow::releaseViewerStartupAdmission()
{
    // =====wjy====
    if (!m_viewerStartAdmissionActive) {
        return; // wjy: 状态回调可能重复到达，幂等门禁防止同一窗口重复释放并突破4路并发上限。
    }
    m_viewerStartAdmissionActive = false;
    if (m_lifecycleManager) {
        m_lifecycleManager->completeViewerStart(this); // wjy: 释放后由共享FIFO立即补启动下一台排队设备，不限制已经在线的窗口数量。
    }
    // ===end====
}

void RemoteDesktopWindow::setConnectionStatus(int code, const QString& message)
{
    // =====wjy====
    if (RemoteConnectionState::acceptsRemoteInput(m_connectionStatusCode)
        && !RemoteConnectionState::acceptsRemoteInput(code)) {
        cancelInputScriptRecording(QStringLiteral("connection_lost"));
        stopInputScriptPlayback(QStringLiteral("connection_lost"), true); // wjy: 在覆盖旧状态码之前补发脚本持有输入，此时旧会话仍有最后一次安全发送机会。
    }
    // ===end====
    const bool should_query_mouse_backend = code == FSREMOTE_STATUS_RECEIVING_VIDEO
        && m_connectionStatusCode != FSREMOTE_STATUS_RECEIVING_VIDEO
        && !m_networkReconnectActive; // wjy: 重连时状态50只代表解码器拿到首帧，必须等UI成功Present后再恢复输入并查询后端。
    // =====wjy====
    if (RemoteConnectionState::releasesViewerStartupAdmission(code)) {
        releaseViewerStartupAdmission(); // wjy: 首帧表示初始化完成；断开/失败表示本轮初始化终止，三种终态都立即补位下一窗口。
    }
    // ===end====
    if (code != FSREMOTE_STATUS_RECEIVING_VIDEO) {
        m_receiveFps = 0.0;
        m_encodedMbps = 0.0;
        m_frameStatsCount = 0;
        m_frameStatsBytes = 0;
        if (m_frameStatsClock.isValid()) m_frameStatsClock.restart(); // wjy: 断线/失败状态清空标题栏统计，下一次收到视频后从新的1秒窗口开始。
    }
    // =====wjy====
    if (remoteUpdateActive()) {
        if (m_remoteUpdateState == RemoteUpdateState::Reconnecting
            && code == FSREMOTE_STATUS_RECEIVING_VIDEO) {
            finishRemoteUpdateWait();
            return;
        }
        if (code == FSREMOTE_STATUS_REMOTE_CLOSED || code == FSREMOTE_STATUS_ERROR) {
            m_remoteUpdateState = RemoteUpdateState::Installing;
            m_remoteUpdateReconnectRequested = false;
            m_nextRemoteUpdateProbeAtMs = 0;
            stopViewerConnectionAsync(false); // wjy: 更新过程中的断线属于目标主进程退出，保持窗口并转入安装等待。
        }
        if (m_inputBroadcastCoordinator) {
            m_inputBroadcastCoordinator->notifyEligibilityChanged(this); // wjy: 更新遮罩分支提前返回前仍通知协调器移除不可输入目标。
        }
        update(isFullScreen() ? rect() : QRect(0, titleBarHeight(), width(), height() - titleBarHeight()));
        if (m_texturePresenter && m_texturePresenter->usesCompositorSurface()) {
            presentCompositorOverlay(); // wjy: 更新期断线只刷新更新卡片，不让旧“正在连接”文字重新进入DComp快照。
        }
        return; // wjy: 更新遮罩接管连接文字，不显示“连接失败/已断开”。
    }
    // ===end====
    // =====wjy====
    const bool wasReceivingVideo = m_hasReceivedVideoInCurrentViewer;
    const bool recoverableSession = wasReceivingVideo || m_networkReconnectActive;
    const bool transientNetworkProblem = recoverableSession
        && (code == FSREMOTE_STATUS_NETWORK_UNSTABLE
            || code == FSREMOTE_STATUS_NETWORK_RECOVERING);
    const bool terminalNetworkProblem = recoverableSession
        && (code == FSREMOTE_STATUS_REMOTE_CLOSED
            || code == FSREMOTE_STATUS_ERROR);
    if (transientNetworkProblem || terminalNetworkProblem) {
        m_remoteMouseCaptureRequested = false; // wjy: 网络异常使旧 Host 状态失效；F2 手动意图单独保留，恢复首帧后按资格重新启用。
        suspendRemoteMouseCapture();
        releaseForwardedKeys(); // wjy: 状态仍是50时先尽力向旧会话补发抬键，随后再关闭输入资格和键盘hook。
        setKeyboardForwardingActive(false);
        if (m_clipboardPollTimer) m_clipboardPollTimer->stop();
    }
    if (code == FSREMOTE_STATUS_RECEIVING_VIDEO) {
        m_hasReceivedVideoInCurrentViewer = true;
        if (!m_networkReconnectActive) {
            m_networkWarningVisible = false; // wjy: 普通首帧可立即清理提示；断网恢复必须等UI真正Present成功后再恢复输入。
        }
    } else if (transientNetworkProblem || terminalNetworkProblem) {
        m_networkWarningVisible = true;
    }
    // ===end====
    const bool deferRecoveredVideo = code == FSREMOTE_STATUS_RECEIVING_VIDEO
        && m_networkReconnectActive;
    m_connectionStatusCode = deferRecoveredVideo
        ? FSREMOTE_STATUS_NETWORK_RECOVERING
        : code;
    m_connectionStatus = deferRecoveredVideo
        ? QString::fromUtf8("网络已恢复，正在等待画面显示")
        : RemoteConnectionState::displayText(code, message); // wjy: 重连首帧只有真正Present后才提交状态50，避免同步输入提前恢复。
    updateRemoteMouseCaptureState(); // wjy: 普通连接状态建立后立即应用 Host/F2 捕获请求；断线状态会保持实际捕获关闭。
    if (m_inputBroadcastCoordinator) {
        m_inputBroadcastCoordinator->notifyEligibilityChanged(this); // wjy: 断线/失败会关闭主控或移除跟随端，连接成功后则从后续事件开始动态加入。
    }
    update(isFullScreen() ? rect() : QRect(0, titleBarHeight(), width(), height() - titleBarHeight())); // wjy: 连接状态在全屏覆盖整窗。
    requestTitleBarUpdate(); // wjy: 状态变化立即刷新DComp或原生DIB标题栏，网络提示不等待一秒计时器。
    if (transientNetworkProblem) {
        beginNetworkRecoveryGracePeriod(); // wjy: ICE暂断或刚恢复都继续等待真实新帧，三秒超时才销毁旧会话。
    } else if (terminalNetworkProblem) {
        beginNetworkReconnect(); // wjy: Failed/Closed或恢复流程中的运行错误立即停止旧Viewer并进入无限退避。
    }
    if (should_query_mouse_backend) queryRemoteMouseBackend(); // wjy: 先提交 code=50 再发送 query，sendInputMessage 的连接资格门禁此时已经满足。
}

void RemoteDesktopWindow::setEncodedBitrateMbps(double mbps)
{
    m_encodedMbps = qMax(0.0, mbps); // wjy: Clamp the title-bar value because transient decoder stats should never draw negative bitrate.
    update(remoteImageRect());
}

void RemoteDesktopWindow::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool fullScreen = isFullScreen();
    const bool nativeTextureVisible = m_textureFrameActive
        && m_texturePresenter
        && m_texturePresenter->hasVisiblePresentation(); // wjy: 统一确认旧D3D子表面或DComp视频视觉是否在前台，缩放期间据此保留父backing store的上一帧。
    const bool compositorOverlayOwnsLocalUi = m_texturePresenter
        && m_texturePresenter->usesCompositorSurface()
        && m_texturePresenter->hasCompositorOverlay(); // wjy: DComp叠加层建立后独占标题栏、连接提示和更新卡片，父QWidget不再重复绘制本地UI。
    const bool preserveNativeContentDuringResize = m_resizingWindow && nativeTextureVisible;

    if (m_resizingWindow) {
        appendResizeDebugTrace(preserveNativeContentDuringResize
                ? QStringLiteral("parent.paint.skip_black_fill")
                : QStringLiteral("parent.paint.before_black_fill")); // wjy: 记录缩放期间是否跳过整窗清黑，避免原生D3D子窗口合成空档时暴露黑帧。
    }
    if (!preserveNativeContentDuringResize) {
        painter.fillRect(rect(), QColor(QStringLiteral("#000000"))); // wjy: 非原生远控内容或非缩放状态仍完整填黑，保持连接提示和软件帧的原有背景行为。
    }
    if (m_resizingWindow && !preserveNativeContentDuringResize) {
        appendResizeDebugTrace(QStringLiteral("parent.paint.after_black_fill")); // wjy: 仅在实际清黑后记录，下一轮日志可直接确认黑底是否被跳过。
    }
    if (!nativeTextureVisible && !m_remoteFrame.isNull()) {
        const QRect target = remoteImageRect();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false); // wjy: Prefer low-latency full-screen remote drawing over expensive smooth scaling.
        painter.drawImage(target, m_remoteFrame);
        // wjy: 旧的RAW/RGB调试面板保持关闭，BGRA和D3D11路径共用同一组内存统计。
    } else if (!nativeTextureVisible && !compositorOverlayOwnsLocalUi) {
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
    // ===end====
    // =====wjy====
    if (remoteUpdateActive() && !compositorOverlayOwnsLocalUi) {
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
#if FSREMOTE_LEGACY_PARENT_TITLE_BAR
    const int barH = titleBarHeight();
    const RemoteTitleBarLayoutSnapshot titleLayout = titleBarLayoutSnapshot(); // wjy: 本次绘制只读取一份可见控件快照，避免同一帧中绘制与命中状态不一致。
    // ===end====

    painter.fillRect(QRectF(0, 0, width(), barH), QColor(QStringLiteral("#E9EEF2")));

    // wjy: 不再绘制覆盖整个远控窗口外沿的浅色圆角描边，避免黑色视频右侧出现明显白边；窗口缩放命中和圆角Mask保持不变。

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
    int titleTextRight = width() - 6;
    for (const QRect& control : {titleLayout.update, titleLayout.mouseBackend,
             titleLayout.inputSync, titleLayout.audio, titleLayout.clipboard, titleLayout.minimize, titleLayout.close}) {
        if (!control.isEmpty()) titleTextRight = qMin(titleTextRight, control.left());
    } // wjy: 标题辅助文字只让位给当前真正可见的最左侧控件，隐藏控件不会继续占用空白。
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

    if (!titleLayout.update.isEmpty()) {
        const QRect updateRect = titleLayout.update;
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

    if (!titleLayout.audio.isEmpty()) {
        const QPixmap audioIcon = icon(m_viewerAudioEnabled
                ? QStringLiteral("rd_audio_on.svg")
                : QStringLiteral("rd_audio_off.svg"));
        if (!audioIcon.isNull()) {
            const QRect audioRect = titleLayout.audio;
            if (audioRect.contains(m_hoveredPos)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(QStringLiteral("#DCE4EC")));
                painter.drawRect(audioRect);
            }
            QSize target = audioIcon.size();
            target.scale(qMax(16, audioRect.width() - 12), qMax(16, barH - 8), Qt::KeepAspectRatio);
            painter.drawPixmap(audioRect.center().x() - target.width() / 2,
                audioRect.center().y() - target.height() / 2,
                audioIcon);
        }
    }

    // =====wjy====
    const QRect mouseModeRect = titleLayout.mouseBackend;
    if (!mouseModeRect.isEmpty()) {
    const bool mouseModeHovered = mouseModeRect.contains(m_hoveredPos);
    QColor mouseModeAccent = m_remoteMouseBackend == RemoteMouseBackend::Faker
        ? QColor(QStringLiteral("#0F766E"))
        : QColor(QStringLiteral("#3A7BFC")); // wjy: 绿色表示虚拟 HID 驱动，蓝色表示原有 SendInput，两个后端不与相对捕获状态混色。
    if (m_remoteMouseBackendFallback) mouseModeAccent = QColor(QStringLiteral("#D97706"));
    if (m_remoteMouseBackendPending) mouseModeAccent = QColor(QStringLiteral("#64748B"));
    QColor mouseModeBackground = mouseModeHovered
        ? mouseModeAccent.lighter(180)
        : mouseModeAccent.lighter(205);
    if (m_mouseBackendButtonPressed) mouseModeBackground = mouseModeBackground.darker(112); // wjy: 只有按下并在同一热区释放才切换，按压色让拖出取消过程可见。
    painter.setPen(QPen(mouseModeAccent, 1));
    painter.setBrush(mouseModeBackground);
    painter.drawRoundedRect(QRectF(mouseModeRect), 4, 4);
    QFont mouseModeFont(QStringLiteral("Microsoft YaHei UI"));
    mouseModeFont.setPixelSize(11);
    mouseModeFont.setWeight(QFont::DemiBold);
    painter.setFont(mouseModeFont);
    painter.setPen(mouseModeAccent.darker(115));
    QString mouseModeText = (m_remoteMouseBackendPending ? m_pendingRemoteMouseBackend : m_remoteMouseBackend)
            == RemoteMouseBackend::Faker
        ? zh("驱动")
        : zh("系统");
    if (m_remoteMouseBackendPending) {
        mouseModeText += QString::fromUtf8("…");
    } else if (m_remoteMouseBackendFallback) {
        mouseModeText += QLatin1Char('!');
    } else if (!m_remoteMouseBackendKnown) {
        mouseModeText += QLatin1Char('?');
    }
    painter.drawText(mouseModeRect, Qt::AlignCenter, mouseModeText); // wjy: pending、回退和旧版本未确认分别使用省略号、感叹号和问号，避免把请求值冒充生效值。
    }
    // ===end====

    // =====wjy====
    const QRect syncRect = titleLayout.inputSync;
    if (!syncRect.isEmpty()) {
    const bool syncHovered = syncRect.contains(m_hoveredPos);
    QColor syncAccent(QStringLiteral("#98A2B3"));
    QColor syncBackground(QStringLiteral("#F2F4F7"));
    QString syncText = QString::fromUtf8("同步");
    if (m_inputSyncRole == RemoteInputSyncRole::Master) {
        syncAccent = QColor(QStringLiteral("#2563EB"));
        syncBackground = QColor(QStringLiteral("#E8F1FF"));
        syncText = QString::fromUtf8("主控");
    } else if (m_inputSyncRole == RemoteInputSyncRole::Follower) {
        syncAccent = QColor(QStringLiteral("#D97706"));
        syncBackground = QColor(QStringLiteral("#FFF3D6"));
        syncText = QString::fromUtf8("跟随");
    } else if (m_inputSyncRole == RemoteInputSyncRole::Excluded) {
        syncAccent = QColor(QStringLiteral("#64748B"));
        syncBackground = QColor(QStringLiteral("#E2E8F0"));
        syncText = QString::fromUtf8("已停");
    } // wjy: 四种角色直接显示文字和固定颜色，比原有双点箭头更容易确认当前设备状态。
    if (syncHovered) syncBackground = syncBackground.darker(104);
    if (m_inputSyncButtonPressed) syncBackground = syncBackground.darker(112); // wjy: 悬停和按下逐级加深，按下后拖出热区仍能看出按钮处于待释放状态。
    painter.setPen(QPen(syncAccent, 1.1));
    painter.setBrush(syncBackground);
    painter.drawRoundedRect(QRectF(syncRect).adjusted(1, 3, -1, -3), 6, 6);
    QFont syncFont(QStringLiteral("Microsoft YaHei UI"));
    syncFont.setPixelSize(11);
    syncFont.setWeight(QFont::DemiBold);
    painter.setFont(syncFont);
    painter.setPen(syncAccent.darker(112));
    painter.drawText(syncRect, Qt::AlignCenter, syncText);
    }
    // ===end====

    // wjy: 剪切板同步按钮在最小化左侧；开启用主蓝，关闭用灰色，中间画剪贴板简图。
    const QRect clipRect = titleLayout.clipboard;
    if (!clipRect.isEmpty()) {
    const QColor clipAccent = m_clipboardSyncEnabled ? QColor(QStringLiteral("#3A7BFC")) : QColor(QStringLiteral("#9CA3AF"));
    painter.setPen(QPen(clipAccent, 1.2));
    painter.setBrush(m_clipboardSyncEnabled ? QColor(QStringLiteral("#EAF2FF")) : QColor(QStringLiteral("#F3F4F6")));
    painter.drawRoundedRect(QRectF(clipRect).adjusted(4, 5, -4, -5), 4, 4);
    painter.setPen(QPen(clipAccent, 1.2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(clipRect.center().x() - 4, clipRect.center().y() - 5, 8, 10), 1.5, 1.5);
    painter.drawLine(QPointF(clipRect.center().x() - 2, clipRect.center().y() - 2), QPointF(clipRect.center().x() + 2, clipRect.center().y() - 2));
    }

    const auto drawTitleButton = [&](const QRect& hitRect, const QString& iconName, bool closeButton) {
        if (hitRect.isEmpty()) return; // wjy: 被设备信息覆盖或裁出窗口的按钮既不绘制，也不会通过其它路径留下可点击图标。
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
    drawTitleButton(titleLayout.minimize, QStringLiteral("rd_minimize.svg"), false);
    drawTitleButton(titleLayout.close, QStringLiteral("rd_close.svg"), true); // wjy: 标题栏只保留最小化和关闭按钮，最大化/还原统一通过双击空白区域完成。

    const RemoteTitleBarIdentityLayout identity = remoteTitleBarIdentityLayout(width(), m_deviceName, m_hostIp);
    if (identity.right > 1) {
        painter.fillRect(
            QRect(1, 1, identity.right - 1, qMax(0, barH - 2)),
            QColor(QStringLiteral("#E9EEF2"))); // wjy: 最后覆盖计时、更新入口和窗口按钮的视觉内容，为设备名/IP 保留最高绘制层级。
    }
    if (identity.showLogo) {
        painter.drawPixmap(QRect(12, logoY, 16, 16), icon(QStringLiteral("fs_session_logo.svg")));
    }
    painter.setFont(identity.font);
    painter.setPen(QColor(QStringLiteral("#111820")));
    painter.drawText(
        QRectF(identity.textX, 0, identity.nameWidth, barH),
        Qt::AlignVCenter | Qt::AlignLeft,
        m_deviceName); // wjy: 设备名在所有标题栏元素之后绘制，窗口再窄也不会被按钮覆盖。
    if (!m_hostIp.isEmpty()) {
        painter.setPen(QColor(QStringLiteral("#667085")));
        painter.drawText(
            QRectF(identity.ipX, 0, identity.ipWidth, barH),
            Qt::AlignVCenter | Qt::AlignLeft,
            m_hostIp); // wjy: IP 使用完整原文且不省略，允许覆盖右侧低优先级按钮和状态内容。
    }
    // ===end====
#endif // FSREMOTE_LEGACY_PARENT_TITLE_BAR
}

void RemoteDesktopWindow::closeEvent(QCloseEvent* event)
{
    // =====wjy====
    clearSnapPreviews(); // wjy: 拖拽过程中关闭窗口时立即清除整组吸附虚影和待提交布局。
    // ===end====
    saveWindowGeometry();
    // =====wjy====
    cancelInputScriptRecording(QStringLiteral("window_close"));
    stopInputScriptPlayback(QStringLiteral("window_close"), true); // wjy: 无论当前是否已有Viewer句柄，关闭路径都先终止脚本定时器和远端持有状态。
    // ===end====
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
        cancelNetworkReconnect(); // wjy: 退避等待阶段可能没有Viewer句柄，直接关闭时也必须先取消定时器。
        QWidget::closeEvent(event);
        return;
    }

    event->ignore();
    m_closeInProgress = true;
    m_localShortcutReleaseKeys.clear(); // wjy: 关闭窗口后不再等待已经失去焦点的自定义快捷键 KeyUp。
    cancelNetworkReconnect(); // wjy: 用户主动关闭后取消三秒等待和所有无限退避，stop完成不得重新创建会话。
    discardPendingTextureFrame(); // wjy: 关闭窗口时取消排队帧并归还共享纹理槽，随后再异步停止Viewer。
    if (m_remoteUpdateTimer) {
        m_remoteUpdateTimer->stop();
    }
    m_remoteMouseCaptureRequested = false;
    suspendRemoteMouseCapture(); // wjy: 关闭阶段在隐藏窗口和停止 Viewer 前释放捕获；手动意图无需额外刷新标题栏。
    releasePressedKeys();
    if (m_inputBroadcastCoordinator) {
        m_inputBroadcastCoordinator->unregisterEndpoint(this); // wjy: 用户关闭时在 Viewer stop 前注销，利用仍有效的句柄完成同步组释放屏障。
    }
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
        // =====wjy====
        if (!isFullScreen() && mouseInputModeRect().contains(event->pos())) {
            m_mouseBackendButtonPressed = true;
            requestTitleBarUpdate(mouseInputModeRect());
            event->accept();
            return; // wjy: 按下只进入视觉态，必须在同一按钮内释放才发送后端切换请求。
        }
        if (!isFullScreen() && inputSyncRect().contains(event->pos())) {
            m_inputSyncButtonPressed = true;
            requestTitleBarUpdate(inputSyncRect());
            event->accept();
            return; // wjy: 按下阶段只记录视觉状态，必须在同一热区释放后才真正切换主控。
        }
        if (!isFullScreen() && audioToggleRect().contains(event->pos())) {
            toggleViewerAudio();
            event->accept();
            return; // wjy: 音频按钮单击立即切换当前窗口播放器，默认静音且不参与拖窗。
        }
        // ===end====
        m_resizeEdges = resizeEdgesAt(event->pos());
        if (m_resizeEdges != ResizeNone) {
            // =====wjy====
            clearSnapPreviews(); // wjy: 开始手动缩放时取消上一次整组拖拽候选，缩放操作不参与吸附提交。
            m_resizingWindow = true;
            m_systemWindowOperationActive = false;
            m_systemWindowOperationAttempted = false;
            m_resizeStartGlobal = event->globalPosition().toPoint();
            m_resizeStartGeometry = frameGeometry(); // wjy: 固定记录本次手势起始几何，后续每个鼠标移动都从同一基准计算，避免增量误差造成边缘抖动。
            beginResizeDebugTrace(); // wjy: 从鼠标按下开始记录本次尺寸手势的父/子窗口时序。
            clearMask(); // wjy: 交互缩放开始时一次性移除圆角区域，拖拽期间使用完整矩形窗口避免每个像素尺寸都重建系统Region。
            if (m_unifiedCompositor && m_unifiedCompositor->isEnabled()) {
                m_unifiedCompositor->beginInteractiveResize(compositorLayoutSnapshot()); // wjy: 锁定最后有效帧和本地层缓存，后续中间几何不销毁可见表面。
            }
            if (m_texturePresenter) {
                m_texturePresenter->setInteractiveResize(true); // wjy: 拖动期间冻结BackBuffer尺寸但保持子HWND和网络新帧实时更新，避免逐像素ResizeBuffers造成闪烁。
            }
            // wjy: 缩放期间禁止冻结父QWidget绘制：与移动不同，窗口变大会暴露新的客户区，
            // 关闭绘制会让这块区域保留未定义的backing store内容并被DWM合成出来。
            updateNativeTitleBarSurface(); // wjy: 缩放开始仍由同一个原生DIB子表面持有标题栏像素，不再隐藏HWND或切换到Qt backing store。
            const bool nativeResizeStarted = startSystemWindowResize();
            appendResizeDebugTrace(nativeResizeStarted
                    ? QStringLiteral("resize.start_system.success")
                    : QStringLiteral("resize.start_system.fallback")); // wjy: 记录原生缩放是否真正接管，避免把手动回退误判成DWM路径。
            // wjy: 原生缩放成功时由DWM驱动尺寸事件；失败时标题栏和D3D内容层继续沿用现有手动回退路径。
            // ===end====
            event->accept();
            return;
        }

        if (isTitleBarBlankArea(event->pos())) { // wjy: 标题栏空白区域同时支持拖动和双击最大化，命中规则集中到同一个函数里维护。
            // =====wjy====
            clearSnapPreviews(); // wjy: 每次开始拖拽都从无候选状态重新判断，防止提交旧的整组吸附位置。
            const QPoint cursorGlobal = event->globalPosition().toPoint();
            m_dragRestorePending = isMaximized() || isFullScreen() || !m_rememberGeometry; // wjy: 只登记临时布局恢复候选，必须发生真实拖动后才改变窗口状态。
            m_dragPressGlobal = cursorGlobal;
            m_dragPressPosition = event->pos();
            m_draggingWindow = true;
            m_systemWindowOperationActive = false;
            m_systemWindowOperationAttempted = false;
            m_dragOffset = cursorGlobal - frameGeometry().topLeft();
            if (updatesEnabled()) {
                setUpdatesEnabled(false); // wjy: 完全阻止系统移动期间父QWidget收到的隐式paintEvent，标题栏直接复用拖动前已提交的backing-store像素。
                m_windowPaintingSuspendedForMove = true; // wjy: 只在本次手势确实关闭绘制时负责恢复，避免错误开启外部主动禁用的更新状态。
            }
            clearMask(); // wjy: 不支持DWM原生圆角的旧Windows在移动期间临时去掉SetWindowRgn，避免Region参与每一帧窗口合成。
            // ===end====
            event->accept();
            return;
        }
    }

    int x = 0;
    int y = 0;
    const int button = remoteButton(event->button());
    if (button && normalizedRemotePoint(event->pos(), &x, &y)) {
        setFocus(Qt::MouseFocusReason);
        RemoteInputEvent input;
        input.type = RemoteInputEventType::ButtonDown;
        input.button = button;
        input.normalizedX = x;
        input.normalizedY = y;
        dispatchRemoteInputEvent(input); // wjy: 记录按钮和归一化位置后同步扇出，协调器可在断线或切主控时补发对应抬起。
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
        // =====wjy====
        finishInteractiveWindowOperation(); // wjy: 第二次按下已建立拖动候选，双击前统一取消并恢复全部交互状态。
        // ===end====
        toggleMaximizedState(); // wjy: 删除最大化按钮后，双击标题栏空白处是 showMaximized/showNormal 的唯一标题栏操作。
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
// ===end====

void RemoteDesktopWindow::mouseMoveEvent(QMouseEvent* event)
{
    // =====wjy====
    if (m_resizingWindow && (event->buttons() & Qt::LeftButton)) {
        if (m_systemWindowOperationActive) {
            event->accept();
            return; // wjy: Windows已接管尺寸循环时禁止Qt再次setGeometry，避免两套缩放轨迹互相覆盖。
        }
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

        appendResizeDebugTrace(QStringLiteral("manual.resize.before_setGeometry target=%1x%2")
            .arg(next.width())
            .arg(next.height())); // wjy: 在父窗口提交前记录目标尺寸，和后续parent.resize/child.resize顺序对照。
        setGeometry(next); // wjy: 手动缩放只提交几何；标题栏显示由独立上一帧快照维持，D3D 子窗口继续呈现内容和黑边。
        appendResizeDebugTrace(QStringLiteral("manual.resize.after_setGeometry")); // wjy: 记录Qt setGeometry返回后的即时父子窗口状态。
        event->accept();
        return;
    }
    // ===end====

    // =====wjy====
    if (m_draggingWindow && (event->buttons() & Qt::LeftButton)) {
        const QPoint cursorGlobal = event->globalPosition().toPoint();
        if (!m_systemWindowOperationActive) {
            const int dragDistance = (cursorGlobal - m_dragPressGlobal).manhattanLength();
            if (dragDistance < QApplication::startDragDistance()) {
                event->accept();
                return; // wjy: 所有标题栏拖动都先经过系统阈值，单击和双击不提前进入原生移动循环。
            }
            if (m_dragRestorePending) {
                restoreSavedGeometryForDrag(cursorGlobal, m_dragPressPosition); // wjy: 平铺、最大化或全屏窗口仅在确认拖动后恢复保存的普通尺寸。
                m_dragRestorePending = false;
                m_dragOffset = cursorGlobal - frameGeometry().topLeft();
            }
            if (startSystemWindowMove()) {
                event->accept();
                return; // wjy: 后续位置变化由WM_MOVING提供吸附预览，当前函数不再逐像素move或重画标题栏。
            }
            const QPoint proposedTopLeft = cursorGlobal - m_dragOffset;
            const QRect proposedGeometry(proposedTopLeft, frameGeometry().size());
            move(proposedTopLeft); // wjy: 极少数平台系统移动接口不可用时保留原有手动回退，不影响基本拖窗能力。
            updateSnapPreviewForGeometry(proposedGeometry, cursorGlobal);
        }
        event->accept();
        return;
    }
    // ===end====

    updateTitleBarHover(event->pos()); // wjy: 普通鼠标移动只在跨越按钮边界时更新局部标题栏区域。

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
        // =====wjy====
        if (m_mouseBackendButtonPressed) {
            const bool activate = !isFullScreen() && mouseInputModeRect().contains(event->pos());
            m_mouseBackendButtonPressed = false;
            requestTitleBarUpdate(mouseInputModeRect());
            if (activate) toggleRemoteMouseBackend(); // wjy: 松开位置仍在热区才切换，拖出按钮等同取消且不会影响远端鼠标。
            event->accept();
            return;
        }
        if (m_inputSyncButtonPressed) {
            const bool activate = !isFullScreen() && inputSyncRect().contains(event->pos());
            m_inputSyncButtonPressed = false;
            requestTitleBarUpdate(inputSyncRect());
            if (activate) {
                toggleInputSynchronization(); // wjy: 主控切换由协调器原子完成，旧组释放后才发布新角色。
            }
            event->accept();
            return;
        }
        // ===end====
        if (m_draggingWindow || m_resizingWindow) {
            finishInteractiveWindowOperation(); // wjy: 手动回退和可能到达的Qt释放事件统一走与WM_EXITSIZEMOVE相同的收尾路径。
            event->accept();
            return; // wjy: 防止从右上角缩放窗口后，释放位置仍在关闭矩形内而误关闭远控窗口。
        }

        // =====wjy====
        if (!isFullScreen()) { // wjy: 全屏时顶部右侧也属于远端画面，释放鼠标不能误触发本地最小化或关闭。
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
            if (audioToggleRect().contains(event->pos())) {
                event->accept();
                return; // wjy: 音频在按下阶段已完成切换，释放阶段不重复切换。
            }
            if (minimizeRect().contains(event->pos())) {
                showMinimized();
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
        RemoteInputEvent input;
        input.type = RemoteInputEventType::ButtonUp;
        input.button = button;
        input.normalizedX = x;
        input.normalizedY = y;
        dispatchRemoteInputEvent(input); // wjy: 鼠标抬起与按下共享语义广播和每目标持有状态，切换主控时可可靠兜底。
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
        RemoteInputEvent input;
        input.type = RemoteInputEventType::Wheel;
        input.wheelDelta = event->angleDelta().y();
        input.normalizedX = x;
        input.normalizedY = y;
        dispatchRemoteInputEvent(input); // wjy: 滚轮方向和归一化指针位置同步到全部目标，不受各窗口本地尺寸影响。
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
    const QKeySequence current = shortcutSequenceFromKeyEvent(event); // wjy: Qt 兜底路径和 Windows Hook 共用设置页产生的完整组合键。
    const QKeySequence mouseLockShortcut = platform::AppSettings::remoteShortcutMouseLock();
    if (!event->isAutoRepeat() && matchesShortcut(current, mouseLockShortcut)) {
        beginShortcutReleaseGuard(mouseLockShortcut);
        m_localShortcutReleaseKeys.insert(event->key()); // wjy: 非 Windows 或 Hook 未接管时记录主键，KeyUp 也必须被本地快捷键层吞掉。
        QMetaObject::invokeMethod(this, [this] {
            toggleManualMouseLock(); // wjy: 自定义鼠标锁定组合仍在普通输入转发前消费按键。
        }, Qt::QueuedConnection);
        event->accept();
        return;
    }
    const QKeySequence recordingShortcut = platform::AppSettings::remoteShortcutInputScriptRecording();
    if (!event->isAutoRepeat() && matchesShortcut(current, recordingShortcut)) {
        beginShortcutReleaseGuard(recordingShortcut);
        m_localShortcutReleaseKeys.insert(event->key()); // wjy: 记录自定义录制快捷键主键，防止 Qt KeyUp 进入远端。
        QMetaObject::invokeMethod(this, [this] {
            toggleInputScriptRecording(); // wjy: 模态命名框仍通过队列打开，不在普通 Qt KeyPress 调用栈内重入。
        }, Qt::QueuedConnection);
        event->accept();
        return;
    }
    const QKeySequence playbackShortcut = platform::AppSettings::remoteShortcutInputScriptPlayback();
    if (!event->isAutoRepeat() && matchesShortcut(current, playbackShortcut)) {
        beginShortcutReleaseGuard(playbackShortcut);
        m_localShortcutReleaseKeys.insert(event->key()); // wjy: 记录自定义播放快捷键主键，等待 KeyUp 后再解除本地门禁。
        QMetaObject::invokeMethod(this, [this] {
            toggleInputScriptPlayback(); // wjy: F10 设置窗口和再次按键停止逻辑继续复用原有入口。
        }, Qt::QueuedConnection);
        event->accept();
        return;
    }
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

    if (m_inputScriptPlaying) {
        event->accept();
        return; // wjy: Qt兜底路径同样屏蔽播放期间的人工键盘输入，只有前面匹配到的播放快捷键可以穿过。
    }
    if (event->nativeVirtualKey() > 0) {
        m_pressedKeys.insert(event->nativeVirtualKey());
        RemoteInputEvent input;
        input.type = RemoteInputEventType::KeyDown;
        input.virtualKey = static_cast<int>(event->nativeVirtualKey());
        dispatchRemoteInputEvent(input); // wjy: Qt 按键兜底仅在本地快捷键未消费后进入同步路径。
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RemoteDesktopWindow::keyReleaseEvent(QKeyEvent* event)
{
    const bool localShortcutKeyUp = m_localShortcutReleaseKeys.remove(event->key()) > 0; // wjy: 自定义鼠标锁定/录制/播放快捷键主键的 Qt KeyUp 与录入时修饰键状态无关，按实际主键配对吞掉。
#if defined(Q_OS_WIN)
    const bool wasWaitingShortcutRelease = m_waitingShortcutRelease;
    updateShortcutReleaseGuard();
    if (wasWaitingShortcutRelease || m_waitingShortcutRelease) {
        event->accept();
        return;
    }
#endif
    if (localShortcutKeyUp) {
        event->accept();
        return; // wjy: Qt 兜底路径吞掉设置页自定义脚本快捷键的 KeyUp，远端不会收到没有配对 KeyDown 的主键抬起。
    }
    if (m_inputScriptPlaying) {
        event->accept();
        return; // wjy: 被播放门禁吞掉的实体KeyDown不向远端补发孤立KeyUp，脚本自己的持有状态由停止流程统一释放。
    }
    if (event->nativeVirtualKey() > 0) {
        m_pressedKeys.remove(event->nativeVirtualKey());
        RemoteInputEvent input;
        input.type = RemoteInputEventType::KeyUp;
        input.virtualKey = static_cast<int>(event->nativeVirtualKey());
        dispatchRemoteInputEvent(input); // wjy: 抬键按原始虚拟键码广播，与低级钩子路径保持完全一致。
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void RemoteDesktopWindow::focusInEvent(QFocusEvent* event)
{
    emit activated(this);
    // =====wjy====
    updateRemoteMouseCaptureState(); // wjy: 重新获得焦点时按 Host 请求或 F2 手动意图恢复实际捕获，并安全接管进程唯一 Raw Input 目标。
    // ===end====
    setKeyboardForwardingActive(!remoteUpdateActive()); // wjy: 更新遮罩获得焦点时也不安装全局键盘转发钩子。
    QWidget::focusInEvent(event);
}

void RemoteDesktopWindow::focusOutEvent(QFocusEvent* event)
{
    suspendRemoteMouseCapture(); // wjy: 失焦只暂停实际捕获，不清除 Host relative 请求或 F2 手动意图，切回本窗口时可直接恢复。
    m_localShortcutReleaseKeys.clear(); // wjy: 组合键在失焦时不再等待本窗口收到松键，避免下一次焦点进入误吞首个普通按键。
    releasePressedKeys();
    setKeyboardForwardingActive(false);
    QWidget::focusOutEvent(event);
}

void RemoteDesktopWindow::resizeEvent(QResizeEvent* event)
{
    if (m_resizingWindow) {
        appendResizeDebugTrace(QStringLiteral("parent.resize.begin old=%1x%2 new=%3x%4")
            .arg(event ? event->oldSize().width() : -1)
            .arg(event ? event->oldSize().height() : -1)
            .arg(event ? event->size().width() : -1)
            .arg(event ? event->size().height() : -1)); // wjy: 记录父QWidget收到的真实尺寸事件及其前后尺寸。
    }
    if (!m_resizingWindow) {
        updateWindowMask(); // wjy: 最大化、还原等普通变化立即更新圆角；手动拖拽已在开始时清除Mask并在释放时统一恢复。
    }
    updateTexturePresenterGeometry(); // wjy: 父窗口每次尺寸变化仍同步D3D子HWND；拖拽期间只由DWM缩放稳定BackBuffer，松手后再一次性调整最终缓冲尺寸。
    commitCompositorLayout(); // wjy: 父窗口尺寸变化先提交统一快照，标题栏和远端输入以后都从这里读取。
    updateNativeTitleBarSurface(); // wjy: 所有尺寸变化都只更新同一个原生标题栏表面；缩放时保留旧完整帧并在内存合成缓冲中移动按钮段。
    if (m_resizingWindow && event && event->oldSize().width() == event->size().width()
        && m_nativeTitleBarSurface && m_nativeTitleBarSurface->isCreated()) {
        m_nativeTitleBarSurface->refresh(); // wjy: 纯顶部/底部缩放没有按钮横向位移，仍主动Present一次完整前台缓冲，避免只移动父HWND时等待系统重绘。
    }
    if (m_resizingWindow) {
        appendResizeDebugTrace(QStringLiteral("parent.resize.after_children")); // wjy: 子窗口几何和标题栏同步后记录一次，定位父子表面是否出现尺寸滞后。
    }
    QWidget::resizeEvent(event);
}

void RemoteDesktopWindow::leaveEvent(QEvent* event)
{
    updateTitleBarHover(QPoint(-1, -1)); // wjy: 离开窗口只清理此前真正悬停的按钮，不再刷新整条标题栏。
    QWidget::leaveEvent(event);
}

} // namespace ui
