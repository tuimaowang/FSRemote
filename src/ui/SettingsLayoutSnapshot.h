#pragma once

#include <QPoint>
#include <QRect>

namespace ui {

// =====wjy====
class SettingsLayoutSnapshot final {
public:
    SettingsLayoutSnapshot(QRect viewport, int maxScrollOffset, int requestedScrollOffset);

    const QRect& viewport() const;
    int scrollOffset() const;
    int maxScrollOffset() const;
    QRect scrolled(const QRect& contentRect) const;
    bool fullyVisible(const QRect& contentRect) const;
    bool containsPoint(const QRect& contentRect, const QPoint& point) const;

private:
    QRect m_viewport; // wjy: 保存本次布局计算使用的设置页安全视口，绘制和真实控件共享同一矩形。
    int m_scrollOffset = 0; // wjy: 保存夹紧后的实际滚动偏移，窗口缩放或卡片折叠后不会越界。
    int m_maxScrollOffset = 0; // wjy: 保存当前内容高度对应的滚动上限，滚动条和滚轮使用同一边界。
};
// ===end====

} // namespace ui
