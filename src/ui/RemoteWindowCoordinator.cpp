#include "ui/RemoteWindowCoordinator.h"

#include "ui/RemoteDesktopWindow.h"

namespace ui {

// =====wjy====
QPointer<RemoteDesktopWindow> RemoteWindowCoordinator::normalWindow(const QString& hostIp) const
{
    return QPointer<RemoteDesktopWindow>(
        qobject_cast<RemoteDesktopWindow*>(m_registry.normalWindow(hostIp).data())); // wjy: 纯注册表按规范化 IP 返回对象，协调器在真实窗口边界恢复强类型。
}

void RemoteWindowCoordinator::registerNormalWindow(const QString& hostIp, RemoteDesktopWindow* window)
{
    m_registry.registerNormalWindow(hostIp, window); // wjy: 普通窗口身份由可单测注册表统一去重。
}

void RemoteWindowCoordinator::registerTiledWindow(RemoteDesktopWindow* window)
{
    m_registry.registerTiledWindow(window); // wjy: 平铺批次状态不再与真实 close 调用耦合。
}

void RemoteWindowCoordinator::removeWindow(const QString& hostIp, RemoteDesktopWindow* window)
{
    m_registry.removeNormalWindow(hostIp, window); // wjy: 旧窗口销毁时仅删除仍指向自己的普通窗口映射。
    removeWindow(window);
}

void RemoteWindowCoordinator::removeWindow(RemoteDesktopWindow* window)
{
    m_registry.removeWindow(window); // wjy: 平铺、激活和恢复几何索引由注册表一次性清理。
}

void RemoteWindowCoordinator::closeTiledWindows()
{
    const QVector<QPointer<QObject>> tiledWindows = m_registry.tiledWindows();
    for (const QPointer<QObject>& object : tiledWindows) {
        QPointer<RemoteDesktopWindow> window(qobject_cast<RemoteDesktopWindow*>(object.data()));
        if (window && !window->isClosingConnection()) {
            window->close(); // wjy: 重新平铺前只关闭上一批平铺窗口，不影响普通远控窗口。
        }
    }
    m_registry.clearTiledWindows();
}

void RemoteWindowCoordinator::closeAllWindows()
{
    for (const QPointer<RemoteDesktopWindow>& window : openedWindows()) {
        if (window && !window->isClosingConnection()) {
            window->close(); // wjy: close-all 对每个仍可关闭的去重窗口只发送一次 close。
        }
    }
    m_registry.clearActivationAndRestoreState();
    m_registry.setWindowsTiled(false); // wjy: close-all 后统一复位快捷键目标、恢复几何和平铺标志。
}

QVector<QPointer<RemoteDesktopWindow>> RemoteWindowCoordinator::openedWindows() const
{
    QVector<QPointer<RemoteDesktopWindow>> windows;
    for (const QPointer<QObject>& object : m_registry.allWindows()) {
        QPointer<RemoteDesktopWindow> window(qobject_cast<RemoteDesktopWindow*>(object.data()));
        if (!window || window->isClosingConnection()) {
            continue;
        }
        windows.append(window); // wjy: 注册表已完成跨索引去重，协调器只过滤正在关闭的真实窗口。
    }
    return windows;
}

QVector<QPointer<RemoteDesktopWindow>> RemoteWindowCoordinator::shutdownWindows() const
{
    QVector<QPointer<RemoteDesktopWindow>> windows;
    for (const QPointer<QObject>& object : m_registry.allWindows()) {
        QPointer<RemoteDesktopWindow> window(qobject_cast<RemoteDesktopWindow*>(object.data()));
        if (window) {
            windows.append(window); // wjy: 退出快照故意保留 closing 窗口，等待其 Viewer stop 生命周期完成。
        }
    }
    return windows;
}

void RemoteWindowCoordinator::rememberActivation(RemoteDesktopWindow* window)
{
    m_registry.rememberActivation(window); // wjy: 激活去重和排序由纯注册表维护。
}

RemoteDesktopWindow* RemoteWindowCoordinator::topmostWindow() const
{
    const QVector<QPointer<QObject>> activationOrder = m_registry.activationOrder();
    for (int i = activationOrder.size() - 1; i >= 0; --i) {
        RemoteDesktopWindow* window = qobject_cast<RemoteDesktopWindow*>(activationOrder.at(i).data());
        if (window && !window->isClosingConnection()) {
            return window; // wjy: 最近激活且仍可用的窗口优先作为快捷键目标。
        }
    }
    const QVector<QPointer<RemoteDesktopWindow>> windows = openedWindows();
    return windows.isEmpty() ? nullptr : windows.last().data();
}

void RemoteWindowCoordinator::setRestoreGeometry(RemoteDesktopWindow* window, const QRect& geometry)
{
    m_registry.setRestoreGeometry(window, geometry); // wjy: 恢复几何与窗口对象身份一起交给注册表维护。
}

QRect RemoteWindowCoordinator::restoreGeometry(RemoteDesktopWindow* window) const
{
    return m_registry.restoreGeometry(window);
}

void RemoteWindowCoordinator::clearRestoreGeometries()
{
    m_registry.clearRestoreGeometries();
}

bool RemoteWindowCoordinator::windowsTiled() const
{
    return m_registry.windowsTiled();
}

void RemoteWindowCoordinator::setWindowsTiled(bool tiled)
{
    m_registry.setWindowsTiled(tiled); // wjy: 平铺标志与窗口集合使用同一个可测试状态所有者。
}

void RemoteWindowCoordinator::clear()
{
    m_registry.clear(); // wjy: 应用退出后一次性清空所有窗口生命周期索引。
}
// ===end====

} // namespace ui
