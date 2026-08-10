#pragma once

#include <QRect>

#include <algorithm>

namespace ui {

// =====wjy====
struct RemoteTitleBarLayoutSnapshot {
    QRect update;
    QRect quality;
    QRect mouseBackend;
    QRect inputSync;
    QRect audio;
    QRect clipboard;
    QRect minimize;
    QRect close;
}; // wjy: 绘制、悬停和点击统一读取同一快照，视觉上被设备信息覆盖的按钮不会留下隐形热区。

inline QRect visibleRemoteTitleBarControl(const QRect& raw, int identityRight, int windowWidth)
{
    return raw.isValid() && raw.left() > identityRight && raw.left() >= 0 && raw.right() < windowWidth
        ? raw
        : QRect(); // wjy: 只有完整位于窗口内且处于设备信息右侧的控件才可见、可悬停和可点击。
}

struct RemoteTitleBarButtonGroupGeometry {
    int left = 0; // wjy: 按钮组在窗口坐标中的左边界，等于窗口宽度减去组宽度。
    int width = 0; // wjy: 组宽度只取决于可见按钮集合，与窗口宽度无关，因此缩放时只需平移不需重绘。
    RemoteTitleBarLayoutSnapshot localLayout; // wjy: 全部按钮矩形平移到以组左边界为原点的局部坐标，供独立子表面渲染。
    quint32 visibleSignature = 0; // wjy: 可见按钮集合的位掩码；只有它变化才需要重新渲染按钮表面。
}; // wjy: 把右侧按钮组抽象为可独立平移的图层，缩放期间标题栏无需按新宽度重排整条位图。

inline RemoteTitleBarButtonGroupGeometry remoteTitleBarButtonGroupGeometry(
    const RemoteTitleBarLayoutSnapshot& layout,
    int windowWidth)
{
    RemoteTitleBarButtonGroupGeometry geometry;
    int groupLeft = windowWidth;
    quint32 bit = 1;
    for (const QRect& control : {layout.update, layout.quality, layout.mouseBackend,
             layout.inputSync, layout.audio, layout.clipboard, layout.minimize, layout.close}) {
        if (!control.isEmpty()) {
            groupLeft = std::min(groupLeft, control.left());
            geometry.visibleSignature |= bit; // wjy: 窄窗口逐个隐藏按钮时签名改变，据此触发一次重新渲染。
        }
        bit <<= 1;
    }
    if (geometry.visibleSignature == 0) {
        return geometry; // wjy: 全部按钮都被设备信息挤出时组宽度为0，调用方直接隐藏按钮表面。
    }

    geometry.left = groupLeft;
    geometry.width = std::max(0, windowWidth - groupLeft); // wjy: 组右边界固定对齐窗口右边缘，末尾resizeMargin空隙由组背景一并覆盖。
    const int shift = -groupLeft;
    geometry.localLayout.update = layout.update.isEmpty() ? QRect() : layout.update.translated(shift, 0);
    geometry.localLayout.quality = layout.quality.isEmpty() ? QRect() : layout.quality.translated(shift, 0);
    geometry.localLayout.mouseBackend = layout.mouseBackend.isEmpty() ? QRect() : layout.mouseBackend.translated(shift, 0);
    geometry.localLayout.inputSync = layout.inputSync.isEmpty() ? QRect() : layout.inputSync.translated(shift, 0);
    geometry.localLayout.audio = layout.audio.isEmpty() ? QRect() : layout.audio.translated(shift, 0);
    geometry.localLayout.clipboard = layout.clipboard.isEmpty() ? QRect() : layout.clipboard.translated(shift, 0);
    geometry.localLayout.minimize = layout.minimize.isEmpty() ? QRect() : layout.minimize.translated(shift, 0);
    geometry.localLayout.close = layout.close.isEmpty() ? QRect() : layout.close.translated(shift, 0);
    return geometry;
}

inline RemoteTitleBarLayoutSnapshot remoteTitleBarLayoutSnapshot(
    int windowWidth,
    int titleBarHeight,
    int identityRight,
    bool updateAvailable,
    int resizeMargin)
{
    const int safeHeight = std::max(0, titleBarHeight);
    const QRect rawClose(windowWidth - 48, 0, 48 - resizeMargin, safeHeight);
    const QRect rawMinimize(rawClose.left() - 36, 0, 36, safeHeight);
    const QRect rawAudio(rawMinimize.left() - 32, 0, 28, safeHeight);
    const QRect rawClipboard(rawAudio.left() - 32, 0, 28, safeHeight);
    const QRect rawInputSync(rawClipboard.left() - 50, 0, 46, safeHeight);
    const QRect rawMouseBackend(rawInputSync.left() - 58, 3, 54, std::max(0, safeHeight - 6));
    const QRect rawQuality(rawMouseBackend.left() - 58, 3, 54, std::max(0, safeHeight - 6)); // wjy: 画质预览按钮紧贴系统/驱动键鼠按钮左侧，后续菜单不改变标题栏按钮组宽度规则。
    const QRect rawUpdate(rawQuality.left() - 58, 3, 54, std::max(0, safeHeight - 6));

    RemoteTitleBarLayoutSnapshot layout;
    layout.close = visibleRemoteTitleBarControl(rawClose, identityRight, windowWidth);
    layout.minimize = visibleRemoteTitleBarControl(rawMinimize, identityRight, windowWidth);
    layout.audio = visibleRemoteTitleBarControl(rawAudio, identityRight, windowWidth);
    layout.clipboard = visibleRemoteTitleBarControl(rawClipboard, identityRight, windowWidth);
    layout.inputSync = visibleRemoteTitleBarControl(rawInputSync, identityRight, windowWidth);
    layout.mouseBackend = visibleRemoteTitleBarControl(rawMouseBackend, identityRight, windowWidth);
    layout.quality = visibleRemoteTitleBarControl(rawQuality, identityRight, windowWidth);
    layout.update = updateAvailable
        ? visibleRemoteTitleBarControl(rawUpdate, identityRight, windowWidth)
        : QRect(); // wjy: 不可更新设备直接移除更新按钮的视觉和命中区域，其余按钮位置保持稳定。
    return layout;
}
// ===end====

} // namespace ui
