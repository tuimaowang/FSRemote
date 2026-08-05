#include "ui/SettingsLayoutSnapshot.h"

#include <QtGlobal>

#include <utility>

namespace ui {

// =====wjy====
SettingsLayoutSnapshot::SettingsLayoutSnapshot(QRect viewport, int maxScrollOffset, int requestedScrollOffset)
    : m_viewport(std::move(viewport))
    , m_scrollOffset(qBound(0, requestedScrollOffset, qMax(0, maxScrollOffset))) // wjy: 构造时一次性夹紧偏移，所有消费者读取同一个稳定结果。
    , m_maxScrollOffset(qMax(0, maxScrollOffset)) // wjy: 负滚动上限统一归零，避免后续比例和范围计算出现异常。
{
}

const QRect& SettingsLayoutSnapshot::viewport() const
{
    return m_viewport;
}

int SettingsLayoutSnapshot::scrollOffset() const
{
    return m_scrollOffset;
}

int SettingsLayoutSnapshot::maxScrollOffset() const
{
    return m_maxScrollOffset;
}

QRect SettingsLayoutSnapshot::scrolled(const QRect& contentRect) const
{
    QRect result = contentRect;
    result.translate(0, -m_scrollOffset); // wjy: 手绘、命中测试和真实子控件都通过同一方法换算屏幕坐标。
    return result;
}

bool SettingsLayoutSnapshot::fullyVisible(const QRect& contentRect) const
{
    return m_viewport.contains(scrolled(contentRect)); // wjy: 真实控件只有完整位于视口内才显示，防止覆盖页签或底部安全区域。
}

bool SettingsLayoutSnapshot::containsPoint(const QRect& contentRect, const QPoint& point) const
{
    return scrolled(contentRect).contains(point); // wjy: 鼠标命中只换算目标矩形，不改变原有按钮和开关的点击范围。
}
// ===end====

} // namespace ui
