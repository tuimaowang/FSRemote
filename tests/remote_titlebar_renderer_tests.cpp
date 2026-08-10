#include "ui/RemoteTitleBarRenderer.h"

#include <QGuiApplication>

#include <cassert>

namespace {

ui::RemoteTitleBarVisualState baseState(int width, qreal dpr)
{
    ui::RemoteTitleBarVisualState state;
    state.logicalWidth = width;
    state.logicalHeight = 28;
    state.devicePixelRatio = dpr;
    state.identityFont = QFont(QStringLiteral("Microsoft YaHei UI"));
    state.identityFont.setPixelSize(12);
    state.identityNameWidth = 72;
    state.identityIpX = 116;
    state.identityIpWidth = 84;
    state.identityRight = 206;
    state.deviceName = QStringLiteral("Remote-PC");
    state.hostIp = QStringLiteral("192.168.1.2");
    state.elapsedText = QStringLiteral("00:01:02");
    state.layout = ui::remoteTitleBarLayoutSnapshot(width, 28, state.identityRight, true, 6);
    state.mouseBackendText = QString::fromUtf8("系统");
    state.mouseBackendAccent = QColor(QStringLiteral("#3A7BFC"));
    state.inputSyncText = QString::fromUtf8("同步");
    state.inputSyncAccent = QColor(QStringLiteral("#98A2B3"));
    state.inputSyncBackground = QColor(QStringLiteral("#F2F4F7"));
    return state;
}

void verifyNormalAndDpiRendering()
{
    ui::RemoteTitleBarVisualState normalState = baseState(900, 1.0);
    assert(!normalState.layout.quality.isEmpty()); // wjy: 正常宽度必须为系统/驱动按钮左侧的前端画质入口分配完整热区。
    const QImage normal = ui::RemoteTitleBarRenderer::render(normalState);
    assert(!normal.isNull());
    assert(normal.width() == 900);
    assert(normal.height() == 28);

    normalState.qualityMenuOpen = true;
    const QImage menuOpen = ui::RemoteTitleBarRenderer::render(normalState);
    assert(!menuOpen.isNull());
    assert(menuOpen != normal); // wjy: 菜单展开期间按钮必须生成不同按压态像素，用户能确认下拉层来自哪个入口。

    const QImage scaled = ui::RemoteTitleBarRenderer::render(baseState(900, 1.5));
    assert(!scaled.isNull());
    assert(scaled.width() == 1350);
    assert(scaled.height() == 42); // wjy: 物理DIB尺寸必须跟随DPR，逻辑布局仍保持900x28。
}

void verifyNarrowVisibilityAndStateChanges()
{
    ui::RemoteTitleBarVisualState narrow = baseState(240, 1.0);
    assert(narrow.layout.close.isValid());
    assert(narrow.layout.update.isEmpty());
    const QImage first = ui::RemoteTitleBarRenderer::render(narrow);
    assert(!first.isNull());

    narrow.hoveredPosition = narrow.layout.close.center();
    narrow.clipboardEnabled = true;
    narrow.inputSyncPressed = true;
    const QImage changed = ui::RemoteTitleBarRenderer::render(narrow);
    assert(!changed.isNull());
    assert(changed != first); // wjy: 悬停、按下和状态色变化必须生成新的完整标题栏帧。
}

// =====wjy====
void verifyNetworkWarningRendering()
{
    ui::RemoteTitleBarVisualState normal = baseState(900, 1.0);
    normal.performanceText = QStringLiteral("60FPS · 30Mbps");
    const QImage normalImage = ui::RemoteTitleBarRenderer::render(normal);
    assert(!normalImage.isNull());

    normal.networkWarningText = QString::fromUtf8("网络不佳");
    const QImage warningImage = ui::RemoteTitleBarRenderer::render(normal);
    assert(!warningImage.isNull());
    assert(warningImage != normalImage); // wjy: 完整DComp标题栏进入网络异常状态后必须生成不同像素，不能继续残留FPS文本。

    const QImage warningBand = ui::RemoteTitleBarRenderer::renderIdentityBand(normal, 1200);
    assert(!warningBand.isNull());
    assert(warningBand.width() == 1200); // wjy: 分段原生标题栏也必须在固定身份带内稳定绘制网络提示。
}
// ===end====

// =====wjy====
void verifyInputScriptStatusRendering()
{
    ui::RemoteTitleBarVisualState normal = baseState(900, 1.0);
    normal.performanceText = QStringLiteral("60FPS · 30Mbps");
    const QImage normalImage = ui::RemoteTitleBarRenderer::render(normal);
    assert(!normalImage.isNull());

    normal.scriptStatusText = QString::fromUtf8("录制中");
    normal.scriptStatusColor = QColor(QStringLiteral("#DC2626"));
    const QImage recordingImage = ui::RemoteTitleBarRenderer::render(normal);
    assert(!recordingImage.isNull());
    assert(recordingImage != normalImage); // wjy: 录制状态必须替代性能区域生成明确不同的静态标题栏像素。

    normal.scriptStatusText = QString::fromUtf8("脚本播放");
    normal.scriptStatusColor = QColor(QStringLiteral("#2563EB"));
    const QImage playbackBand = ui::RemoteTitleBarRenderer::renderIdentityBand(normal, 1200);
    assert(!playbackBand.isNull());
    assert(playbackBand != ui::RemoteTitleBarRenderer::renderIdentityBand(baseState(900, 1.0), 1200)); // wjy: 分段原生标题栏路径也必须显示F10回放状态。
}
// ===end====

// =====wjy====
void verifyCompactSessionMetricsRendering()
{
    ui::RemoteTitleBarVisualState compact = baseState(760, 1.0);
    const QImage elapsedOnly = ui::RemoteTitleBarRenderer::render(compact);
    compact.performanceText = QStringLiteral("60FPS · 30Mbps"); // wjy: 新增54px画质入口后以760px验证全部右侧按钮与紧凑性能文本可以同时显示。
    const QImage compactImage = ui::RemoteTitleBarRenderer::render(compact);
    assert(!elapsedOnly.isNull());
    assert(!compactImage.isNull());
    assert(compactImage != elapsedOnly); // wjy: 紧凑布局必须在760px宽度下真正绘制FPS/码率，旧的122px固定占位会使两帧完全相同并导致测试失败。

    ui::RemoteTitleBarVisualState bandState = baseState(760, 1.0);
    const QImage elapsedBand = ui::RemoteTitleBarRenderer::renderIdentityBand(bandState, 1200);
    bandState.performanceText = QStringLiteral("60FPS · 30Mbps");
    const QImage compactBand = ui::RemoteTitleBarRenderer::renderIdentityBand(bandState, 1200);
    assert(compactBand != elapsedBand); // wjy: 当前生产使用的分段原生标题栏路径也必须应用同一紧凑布局。
}
// ===end====

// =====wjy====
void verifyMouseLockStatusRendering()
{
    ui::RemoteTitleBarVisualState normal = baseState(900, 1.0);
    const QImage normalImage = ui::RemoteTitleBarRenderer::render(normal);
    const QImage normalBand = ui::RemoteTitleBarRenderer::renderIdentityBand(normal, 1200);
    assert(!normalImage.isNull());
    assert(!normalBand.isNull());

    normal.mouseLockText = QString::fromUtf8("鼠标锁定");
    normal.mouseLockColor = QColor(QStringLiteral("#DC2626"));
    const QImage lockedImage = ui::RemoteTitleBarRenderer::render(normal);
    const QImage lockedBand = ui::RemoteTitleBarRenderer::renderIdentityBand(normal, 1200);
    assert(!lockedImage.isNull());
    assert(!lockedBand.isNull());
    assert(lockedImage != normalImage); // wjy: 完整标题栏必须在 F2 手动锁定开启后生成不同的静态状态像素。
    assert(lockedBand != normalBand); // wjy: 分段原生标题栏身份带也必须显示“鼠标锁定”，不能只在 DComp 路径生效。
}
// ===end====

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    verifyNormalAndDpiRendering();
    verifyNarrowVisibilityAndStateChanges();
    verifyNetworkWarningRendering();
    verifyInputScriptStatusRendering();
    verifyMouseLockStatusRendering();
    verifyCompactSessionMetricsRendering();
    return 0;
}
