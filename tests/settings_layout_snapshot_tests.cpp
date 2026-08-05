#include "ui/SettingsLayoutSnapshot.h"

#include <cassert>

int main()
{
    // =====wjy====
    const ui::SettingsLayoutSnapshot layout(QRect(100, 100, 400, 300), 240, 80);
    assert(layout.viewport() == QRect(100, 100, 400, 300)); // wjy: 快照保留调用方提供的设置页安全视口。
    assert(layout.scrollOffset() == 80);
    assert(layout.maxScrollOffset() == 240);
    assert(layout.scrolled(QRect(120, 210, 100, 30)) == QRect(120, 130, 100, 30)); // wjy: 内容矩形按同一个偏移向上换算。
    assert(layout.fullyVisible(QRect(120, 210, 100, 30))); // wjy: 换算后的矩形完整位于视口内时允许显示真实控件。
    assert(layout.containsPoint(QRect(120, 210, 100, 30), QPoint(140, 145))); // wjy: 命中测试使用滚动后的屏幕位置。
    assert(!layout.containsPoint(QRect(120, 210, 100, 30), QPoint(140, 225)));

    const ui::SettingsLayoutSnapshot upperBound(QRect(0, 0, 100, 100), 60, 500);
    assert(upperBound.scrollOffset() == 60); // wjy: 内容缩短后旧偏移自动夹紧到新的滚动上限。
    const ui::SettingsLayoutSnapshot lowerBound(QRect(0, 0, 100, 100), -10, -20);
    assert(lowerBound.scrollOffset() == 0 && lowerBound.maxScrollOffset() == 0); // wjy: 异常负范围统一恢复到顶部空闲状态。
    // ===end====
    return 0;
}
