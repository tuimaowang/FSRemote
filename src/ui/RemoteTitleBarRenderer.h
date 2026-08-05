#pragma once

#include "ui/RemoteTitleBarLayout.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPoint>
#include <QString>

namespace ui {

// =====wjy====
struct RemoteTitleBarVisualState {
    int logicalWidth = 0;
    int logicalHeight = 0;
    qreal devicePixelRatio = 1.0;
    RemoteTitleBarLayoutSnapshot layout;
    QPoint hoveredPosition = QPoint(-1, -1);

    QString deviceName;
    QString hostIp;
    QString connectionStatus;
    QString elapsedText;
    QString performanceText;
    QString scriptStatusText; // wjy: F9录制和F10回放使用固定静态文案，不通过闪烁或逐帧动画制造额外标题栏重绘。
    QColor scriptStatusColor;
    QString networkWarningText; // wjy: 网络异常期间保存固定标题栏提示，恢复首帧后由窗口清空。
    bool updateAvailable = false;
    bool identityShowLogo = true;
    QFont identityFont;
    int identityTextX = 34;
    int identityNameWidth = 0;
    int identityIpX = 0;
    int identityIpWidth = 0;
    int identityRight = 0;

    QString mouseBackendText;
    QColor mouseBackendAccent;
    bool mouseBackendPressed = false;
    QString mouseLockText; // wjy: F2 手动锁定开启时显示固定“鼠标锁定”，不与 Host 自动 relative 状态混用。
    QColor mouseLockColor;

    QString inputSyncText;
    QColor inputSyncAccent;
    QColor inputSyncBackground;
    bool inputSyncPressed = false;

    bool audioEnabled = false;
    bool clipboardEnabled = false;
}; // wjy: 标题栏渲染只消费不可变值快照，窗口成员继续负责业务状态和点击行为，原生表面不会反向读取可变窗口对象。

class RemoteTitleBarRenderer final {
public:
    static QImage render(const RemoteTitleBarVisualState& state); // wjy: 在独立ARGB图像中一次性生成完整不透明标题栏，成功后才能交给原生DIB替换上一帧。
    // =====wjy====
    static QImage renderIdentityBand(const RemoteTitleBarVisualState& state, int bandLogicalWidth); // wjy: 只渲染背景与左侧身份信息，宽度按虚拟屏预留，缩放时无需重绘。
    static QImage renderButtonGroup(
        const RemoteTitleBarVisualState& state,
        const RemoteTitleBarLayoutSnapshot& localLayout,
        int groupLogicalWidth,
        const QPoint& localHoveredPosition); // wjy: 只渲染右侧按钮组局部图像，缩放时由子HWND平移贴住右边缘。
    // ===end====
};
// ===end====

} // namespace ui
