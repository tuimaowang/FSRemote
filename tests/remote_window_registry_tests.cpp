#include "ui/RemoteWindowRegistry.h"

#include <QObject>

#include <cassert>

int main()
{
    // =====wjy====
    ui::RemoteWindowRegistry registry;
    QObject first;
    QObject second;
    QObject replacement;

    registry.registerNormalWindow(QStringLiteral(" 192.168.1.10 "), &first);
    assert(registry.normalWindow(QStringLiteral("192.168.1.10")).data() == &first);
    registry.registerNormalWindow(QStringLiteral("192.168.1.10"), &replacement);
    registry.removeNormalWindow(QStringLiteral("192.168.1.10"), &first);
    assert(registry.normalWindow(QStringLiteral("192.168.1.10")).data() == &replacement); // wjy: 旧窗口迟到销毁不能删除替换后的新映射。

    registry.registerTiledWindow(&first);
    registry.registerTiledWindow(&second);
    registry.rememberActivation(&first);
    registry.rememberActivation(&second);
    registry.rememberActivation(&first);
    const auto activation = registry.activationOrder();
    assert(activation.size() == 2);
    assert(activation.at(0).data() == &second);
    assert(activation.at(1).data() == &first); // wjy: 再次激活的窗口必须移动到顺序末尾。

    const QRect restoreGeometry(12, 34, 640, 360);
    registry.setRestoreGeometry(&first, restoreGeometry);
    registry.setWindowsTiled(true);
    assert(registry.restoreGeometry(&first) == restoreGeometry);
    assert(registry.windowsTiled());

    const auto shutdownSnapshot = registry.allWindows();
    assert(shutdownSnapshot.size() == 3); // wjy: 普通、平铺和激活索引合并时自动去重，退出阶段每个窗口只处理一次。
    registry.removeWindow(&first);
    assert(registry.activationOrder().size() == 1);
    assert(registry.restoreGeometry(&first).isNull());

    registry.clear();
    assert(registry.allWindows().isEmpty());
    assert(!registry.windowsTiled());
    // ===end====
    return 0;
}
