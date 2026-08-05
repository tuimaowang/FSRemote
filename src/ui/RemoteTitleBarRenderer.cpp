#include "ui/RemoteTitleBarRenderer.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPixmap>

#include <algorithm>
#include <cmath>

namespace ui {
namespace {

QPixmap titleBarIcon(const QString& name)
{
    return QPixmap(QStringLiteral(":/UUGuest/resource/images/titlebar/") + name);
}

void drawWindowControl(QPainter& painter, const QRect& hitRect, const QString& iconName, bool closeButton, const QPoint& hoveredPosition, int barHeight)
{
    if (hitRect.isEmpty()) return;
    if (hitRect.contains(hoveredPosition)) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(closeButton
                ? QColor(QStringLiteral("#FCE8E6"))
                : QColor(QStringLiteral("#DCE4EC")));
        painter.drawRect(hitRect);
    }
    const QPixmap raw = titleBarIcon(iconName);
    if (raw.isNull()) return;
    const int maxHeight = std::max(16, barHeight - 8);
    const int maxWidth = std::max(16, hitRect.width() - 12);
    QSize target = raw.size();
    target.scale(maxWidth, maxHeight, Qt::KeepAspectRatio);
    painter.drawPixmap(QRect(
        hitRect.center().x() - target.width() / 2,
        hitRect.center().y() - target.height() / 2,
        target.width(),
        target.height()), raw);
}

// =====wjy====
QImage createBandImage(int logicalWidth, int logicalHeight, qreal devicePixelRatio)
{
    if (logicalWidth <= 0 || logicalHeight <= 0) return {};
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatio);
    const QSize physicalSize(
        std::max(1, static_cast<int>(std::ceil(logicalWidth * dpr))),
        std::max(1, static_cast<int>(std::ceil(logicalHeight * dpr))));
    QImage image(physicalSize, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return {};
    image.setDevicePixelRatio(dpr);
    image.fill(QColor(QStringLiteral("#E9EEF2"))); // wjy: 两个分段共用同一不透明底色，拼接处不会出现色差或透明缝。
    return image;
}

void paintTitleBarButtons(
    QPainter& painter,
    const RemoteTitleBarVisualState& state,
    const RemoteTitleBarLayoutSnapshot& layout,
    const QPoint& hoveredPosition,
    int barHeight)
{
    if (!layout.update.isEmpty()) {
        const bool hovered = layout.update.contains(hoveredPosition);
        painter.setPen(Qt::NoPen);
        painter.setBrush(hovered ? QColor(QStringLiteral("#2F6FE4")) : QColor(QStringLiteral("#3A7BFC")));
        painter.drawRoundedRect(QRectF(layout.update), 4, 4);
        QFont font(QStringLiteral("Microsoft YaHei UI"));
        font.setPixelSize(12);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(layout.update, Qt::AlignCenter, QString::fromUtf8("更新"));
    }

    if (!layout.mouseBackend.isEmpty()) {
        const bool hovered = layout.mouseBackend.contains(hoveredPosition);
        QColor background = hovered
            ? state.mouseBackendAccent.lighter(180)
            : state.mouseBackendAccent.lighter(205);
        if (state.mouseBackendPressed) background = background.darker(112);
        painter.setPen(QPen(state.mouseBackendAccent, 1));
        painter.setBrush(background);
        painter.drawRoundedRect(QRectF(layout.mouseBackend), 4, 4);
        QFont font(QStringLiteral("Microsoft YaHei UI"));
        font.setPixelSize(11);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(state.mouseBackendAccent.darker(115));
        painter.drawText(layout.mouseBackend, Qt::AlignCenter, state.mouseBackendText);
    }

    // wjy: 质量策略不再绘制为标题栏控件，实际质量仍由后台协调器和协议层控制。
    if (!layout.inputSync.isEmpty()) {
        QColor background = state.inputSyncBackground;
        if (layout.inputSync.contains(hoveredPosition)) background = background.darker(104);
        if (state.inputSyncPressed) background = background.darker(112);
        painter.setPen(QPen(state.inputSyncAccent, 1.1));
        painter.setBrush(background);
        painter.drawRoundedRect(QRectF(layout.inputSync).adjusted(1, 3, -1, -3), 6, 6);
        QFont font(QStringLiteral("Microsoft YaHei UI"));
        font.setPixelSize(11);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(state.inputSyncAccent.darker(112));
        painter.drawText(layout.inputSync, Qt::AlignCenter, state.inputSyncText);
    }

    if (!layout.audio.isEmpty()) {
        drawWindowControl(
            painter,
            layout.audio,
            state.audioEnabled ? QStringLiteral("rd_audio_on.svg") : QStringLiteral("rd_audio_off.svg"),
            false,
            hoveredPosition,
            barHeight);
    }

    if (!layout.clipboard.isEmpty()) {
        const QColor accent = state.clipboardEnabled
            ? QColor(QStringLiteral("#3A7BFC"))
            : QColor(QStringLiteral("#9CA3AF"));
        painter.setPen(QPen(accent, 1.2));
        painter.setBrush(state.clipboardEnabled
                ? QColor(QStringLiteral("#EAF2FF"))
                : QColor(QStringLiteral("#F3F4F6")));
        painter.drawRoundedRect(QRectF(layout.clipboard).adjusted(4, 5, -4, -5), 4, 4);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(
            layout.clipboard.center().x() - 4,
            layout.clipboard.center().y() - 5,
            8, 10), 1.5, 1.5);
        painter.drawLine(
            QPointF(layout.clipboard.center().x() - 2, layout.clipboard.center().y() - 2),
            QPointF(layout.clipboard.center().x() + 2, layout.clipboard.center().y() - 2));
    }

    drawWindowControl(painter, layout.minimize, QStringLiteral("rd_minimize.svg"), false, hoveredPosition, barHeight);
    drawWindowControl(painter, layout.close, QStringLiteral("rd_close.svg"), true, hoveredPosition, barHeight);
}

// =====wjy====
QRect mouseLockStatusRect(const RemoteTitleBarVisualState& state)
{
    if (state.mouseLockText.isEmpty() || state.layout.mouseBackend.isEmpty()) return {};

    constexpr int kStatusWidth = 68;
    constexpr int kStatusGap = 8;
    int controlsLeft = state.layout.mouseBackend.left();
    if (!state.layout.update.isEmpty()) {
        controlsLeft = std::min(controlsLeft, state.layout.update.left()); // wjy: 有更新按钮时把文字放到整个按钮组左侧，禁止覆盖可点击区域。
    }
    const int statusLeft = controlsLeft - kStatusGap - kStatusWidth;
    if (statusLeft < state.identityRight + kStatusGap) {
        return {}; // wjy: 窄窗口优先保留设备身份与可点击按钮，空间不足时不绘制也不产生隐形占位。
    }
    return QRect(statusLeft, 0, kStatusWidth, state.logicalHeight);
}

void paintMouseLockStatus(QPainter& painter, const RemoteTitleBarVisualState& state, const QRect& statusRect)
{
    if (statusRect.isEmpty()) return;
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(12);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(state.mouseLockColor.isValid()
            ? state.mouseLockColor
            : QColor(QStringLiteral("#DC2626")));
    painter.drawText(statusRect, Qt::AlignCenter, state.mouseLockText); // wjy: F2 锁定状态使用静态红色文字，不闪烁、不响应鼠标，也不加入按钮子表面。
}
// ===end====

void paintTitleBarSessionText(
    QPainter& painter,
    const RemoteTitleBarVisualState& state,
    int titleTextRight,
    int barHeight)
{
    // =====wjy====
    const int elapsedX = state.identityRight + 14;
    const int secondaryX = elapsedX + 78;
    const int contentRight = titleTextRight - 8;
    const auto paintElapsed = [&] {
        if (elapsedX + 70 > contentRight) return;
        QFont elapsedFont(QStringLiteral("Microsoft YaHei UI"));
        elapsedFont.setPixelSize(12);
        painter.setFont(elapsedFont);
        painter.setPen(QColor(QStringLiteral("#4B4B4C")));
        painter.drawText(QRectF(elapsedX, 0, 70, barHeight),
            Qt::AlignVCenter | Qt::AlignLeft, state.elapsedText);
    };
    const auto paintPriorityStatus = [&](const QString& text, const QColor& color, int textWidth) {
        const bool fitsAfterElapsed = secondaryX + textWidth <= contentRight;
        if (fitsAfterElapsed) paintElapsed(); // wjy: 空间充足时状态文本替代FPS但保留左侧会话时长。
        const int textX = fitsAfterElapsed ? secondaryX : elapsedX;
        if (textX + textWidth > contentRight) return;
        QFont statusFont(QStringLiteral("Microsoft YaHei UI"));
        statusFont.setPixelSize(12);
        statusFont.setWeight(QFont::DemiBold);
        painter.setFont(statusFont);
        painter.setPen(color);
        painter.drawText(QRectF(textX, 0, textWidth, barHeight),
            Qt::AlignVCenter | Qt::AlignLeft, text); // wjy: 窄窗口优先显示录制、回放或网络状态，必要时让状态占用计时位置。
    };

    if (!state.scriptStatusText.isEmpty()) {
        paintPriorityStatus(
            state.scriptStatusText,
            state.scriptStatusColor.isValid() ? state.scriptStatusColor : QColor(QStringLiteral("#DC2626")),
            72);
        return; // wjy: 脚本状态优先于性能文本，用户无需根据FPS变化猜测当前是否仍在录制或播放。
    }
    if (!state.networkWarningText.isEmpty()) {
        paintPriorityStatus(state.networkWarningText, QColor(QStringLiteral("#D97706")), 64);
        return;
    }

    paintElapsed();
    if (!state.performanceText.isEmpty() && secondaryX + 122 <= contentRight) {
        QFont performanceFont(QStringLiteral("Microsoft YaHei UI"));
        performanceFont.setPixelSize(12);
        painter.setFont(performanceFont);
        painter.setPen(QColor(QStringLiteral("#344054")));
        painter.drawText(QRectF(secondaryX, 0, 122, barHeight),
            Qt::AlignVCenter | Qt::AlignLeft, state.performanceText);
    }
    // ===end====
}
// ===end====

} // namespace

// =====wjy====
QImage RemoteTitleBarRenderer::renderIdentityBand(const RemoteTitleBarVisualState& state, int bandLogicalWidth)
{
    QImage image = createBandImage(bandLogicalWidth, state.logicalHeight, state.devicePixelRatio);
    if (image.isNull()) return {};

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const int barHeight = state.logicalHeight;
    const int logoY = (barHeight - 16) / 2;

    if (state.identityShowLogo) {
        painter.drawPixmap(QRect(12, logoY, 16, 16), titleBarIcon(QStringLiteral("fs_session_logo.svg")));
    }
    painter.setFont(state.identityFont);
    painter.setPen(QColor(QStringLiteral("#111820")));
    painter.drawText(QRectF(state.identityTextX, 0, state.identityNameWidth, barHeight),
        Qt::AlignVCenter | Qt::AlignLeft, state.deviceName);
    if (!state.hostIp.isEmpty() && state.identityIpWidth > 0) {
        painter.setPen(QColor(QStringLiteral("#667085")));
        painter.drawText(QRectF(state.identityIpX, 0, state.identityIpWidth, barHeight),
            Qt::AlignVCenter | Qt::AlignLeft, state.hostIp);
    }

    // wjy: 这一层按虚拟屏宽度预留，不知道按钮组当前落在哪里，因此始终绘制计时文本；
    // 窄窗口时按钮子表面叠在上层覆盖它，与旧实现"没有空间就不显示计时"的视觉结果一致。
    const QRect mouseLockRect = mouseLockStatusRect(state);
    int titleTextRight = state.logicalWidth - 6;
    for (const QRect& control : {state.layout.update, state.layout.mouseBackend,
             state.layout.inputSync, state.layout.audio, state.layout.clipboard, state.layout.minimize, state.layout.close}) {
        if (!control.isEmpty()) titleTextRight = std::min(titleTextRight, control.left());
    }
    if (!mouseLockRect.isEmpty()) titleTextRight = std::min(titleTextRight, mouseLockRect.left()); // wjy: 计时、性能和脚本状态不能延伸到“鼠标锁定”文字下面。
    paintTitleBarSessionText(painter, state, titleTextRight, barHeight); // wjy: 分段原生标题栏和完整DComp标题栏共用同一状态优先级与文字布局。
    paintMouseLockStatus(painter, state, mouseLockRect); // wjy: 身份带负责静态锁定文字，右侧按钮组仍只包含可交互控件。
    painter.end();
    return image;
}

QImage RemoteTitleBarRenderer::renderButtonGroup(
    const RemoteTitleBarVisualState& state,
    const RemoteTitleBarLayoutSnapshot& localLayout,
    int groupLogicalWidth,
    const QPoint& localHoveredPosition)
{
    QImage image = createBandImage(groupLogicalWidth, state.logicalHeight, state.devicePixelRatio);
    if (image.isNull()) return {};

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintTitleBarButtons(painter, state, localLayout, localHoveredPosition, state.logicalHeight);
    painter.end();
    return image;
}
// ===end====

QImage RemoteTitleBarRenderer::render(const RemoteTitleBarVisualState& state)
{
    if (state.logicalWidth <= 0 || state.logicalHeight <= 0) return {};
    const qreal dpr = std::max<qreal>(1.0, state.devicePixelRatio);
    const QSize physicalSize(
        std::max(1, static_cast<int>(std::ceil(state.logicalWidth * dpr))),
        std::max(1, static_cast<int>(std::ceil(state.logicalHeight * dpr))));
    QImage image(physicalSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(QColor(QStringLiteral("#E9EEF2")));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const int barHeight = state.logicalHeight;

    const int logoY = (barHeight - 16) / 2;
    if (state.identityShowLogo) {
        painter.drawPixmap(QRect(12, logoY, 16, 16), titleBarIcon(QStringLiteral("fs_session_logo.svg")));
    }
    painter.setFont(state.identityFont);
    painter.setPen(QColor(QStringLiteral("#111820")));
    painter.drawText(QRectF(state.identityTextX, 0, state.identityNameWidth, barHeight),
        Qt::AlignVCenter | Qt::AlignLeft, state.deviceName);
    if (!state.hostIp.isEmpty() && state.identityIpWidth > 0) {
        painter.setPen(QColor(QStringLiteral("#667085")));
        painter.drawText(QRectF(state.identityIpX, 0, state.identityIpWidth, barHeight),
            Qt::AlignVCenter | Qt::AlignLeft, state.hostIp);
    }

    const QRect mouseLockRect = mouseLockStatusRect(state);
    int titleTextRight = state.logicalWidth - 6;
    for (const QRect& control : {state.layout.update, state.layout.mouseBackend,
             state.layout.inputSync, state.layout.audio, state.layout.clipboard, state.layout.minimize, state.layout.close}) {
        if (!control.isEmpty()) titleTextRight = std::min(titleTextRight, control.left());
    }
    if (!mouseLockRect.isEmpty()) titleTextRight = std::min(titleTextRight, mouseLockRect.left());
    // =====wjy====
    paintTitleBarSessionText(painter, state, titleTextRight, barHeight);
    paintMouseLockStatus(painter, state, mouseLockRect); // wjy: 完整 DComp 路径与分段原生标题栏显示同一 F2 锁定状态。
    // ===end====

    paintTitleBarButtons(painter, state, state.layout, state.hoveredPosition, barHeight); // wjy: 与分段渲染共用同一按钮绘制实现，两条路径视觉不会漂移。

    // wjy: 设备身份最后覆盖低优先级控件，与旧实现保持相同的窄窗口优先级。
    if (state.identityRight > 1) {
        painter.fillRect(QRect(1, 1, state.identityRight - 1, std::max(0, barHeight - 2)),
            QColor(QStringLiteral("#E9EEF2")));
    }
    if (state.identityShowLogo) {
        painter.drawPixmap(QRect(12, logoY, 16, 16), titleBarIcon(QStringLiteral("fs_session_logo.svg")));
    }
    painter.setFont(state.identityFont);
    painter.setPen(QColor(QStringLiteral("#111820")));
    painter.drawText(QRectF(state.identityTextX, 0, state.identityNameWidth, barHeight),
        Qt::AlignVCenter | Qt::AlignLeft, state.deviceName);
    if (!state.hostIp.isEmpty()) {
        painter.setPen(QColor(QStringLiteral("#667085")));
        painter.drawText(QRectF(state.identityIpX, 0, state.identityIpWidth, barHeight),
            Qt::AlignVCenter | Qt::AlignLeft, state.hostIp);
    }
    painter.end();
    return image;
}

} // namespace ui
