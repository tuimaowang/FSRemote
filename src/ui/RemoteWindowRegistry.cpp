#include "ui/RemoteWindowRegistry.h"

#include <QSet>

namespace ui {

// =====wjy====
QPointer<QObject> RemoteWindowRegistry::normalWindow(const QString& hostIp) const
{
    return m_normalWindows.value(hostIp.trimmed()); // wjy: 查询与注册使用同一 IP 规范化规则。
}

void RemoteWindowRegistry::registerNormalWindow(const QString& hostIp, QObject* window)
{
    const QString normalizedIp = hostIp.trimmed();
    if (!normalizedIp.isEmpty() && window) {
        m_normalWindows.insert(normalizedIp, window); // wjy: 后注册窗口替换同 IP 的旧映射，防止重复普通窗口。
    }
}

void RemoteWindowRegistry::registerTiledWindow(QObject* window)
{
    if (window) {
        m_tiledWindows.append(window); // wjy: 保留创建批次顺序，关闭平铺窗口时沿原顺序提交关闭。
    }
}

void RemoteWindowRegistry::removeNormalWindow(const QString& hostIp, QObject* window)
{
    const QString normalizedIp = hostIp.trimmed();
    if (m_normalWindows.value(normalizedIp).data() == window) {
        m_normalWindows.remove(normalizedIp); // wjy: 旧窗口销毁不能误删同 IP 后注册的新窗口。
    }
}

void RemoteWindowRegistry::removeWindow(QObject* window)
{
    if (!window) {
        return;
    }
    for (auto it = m_tiledWindows.begin(); it != m_tiledWindows.end();) {
        if (!*it || it->data() == window) {
            it = m_tiledWindows.erase(it); // wjy: 同时清理目标窗口和已自动失效的 QPointer。
        } else {
            ++it;
        }
    }
    for (auto it = m_activationOrder.begin(); it != m_activationOrder.end();) {
        if (!*it || it->data() == window) {
            it = m_activationOrder.erase(it); // wjy: 已关闭窗口不能继续成为快捷键激活目标。
        } else {
            ++it;
        }
    }
    m_restoreGeometries.remove(window); // wjy: 窗口退出时同步丢弃平铺前几何。
}

QVector<QPointer<QObject>> RemoteWindowRegistry::tiledWindows() const
{
    QVector<QPointer<QObject>> windows;
    for (const QPointer<QObject>& window : m_tiledWindows) {
        if (window) {
            windows.append(window); // wjy: 返回快照时过滤已销毁 QObject，调用方无需接触悬空地址。
        }
    }
    return windows;
}

QVector<QPointer<QObject>> RemoteWindowRegistry::allWindows() const
{
    QVector<QPointer<QObject>> windows;
    QSet<QObject*> seen;
    const auto appendWindow = [&windows, &seen](const QPointer<QObject>& window) {
        if (!window || seen.contains(window.data())) {
            return;
        }
        seen.insert(window.data());
        windows.append(window); // wjy: 普通、平铺、激活索引合并后每个窗口只出现一次。
    };
    for (auto it = m_normalWindows.cbegin(); it != m_normalWindows.cend(); ++it) {
        appendWindow(it.value());
    }
    for (const QPointer<QObject>& window : m_tiledWindows) {
        appendWindow(window);
    }
    for (const QPointer<QObject>& window : m_activationOrder) {
        appendWindow(window);
    }
    return windows;
}

QVector<QPointer<QObject>> RemoteWindowRegistry::activationOrder() const
{
    QVector<QPointer<QObject>> windows;
    for (const QPointer<QObject>& window : m_activationOrder) {
        if (window) {
            windows.append(window); // wjy: 对外快照保持从旧到新的激活顺序。
        }
    }
    return windows;
}

void RemoteWindowRegistry::clearTiledWindows()
{
    m_tiledWindows.clear();
}

void RemoteWindowRegistry::clearActivationAndRestoreState()
{
    m_activationOrder.clear();
    m_restoreGeometries.clear(); // wjy: close-all 后不保留快捷键目标和旧平铺几何。
}

void RemoteWindowRegistry::rememberActivation(QObject* window)
{
    if (!window) {
        return;
    }
    for (auto it = m_activationOrder.begin(); it != m_activationOrder.end();) {
        if (!*it || it->data() == window) {
            it = m_activationOrder.erase(it); // wjy: 重复激活先删除旧位置，再追加到最新位置。
        } else {
            ++it;
        }
    }
    m_activationOrder.append(window);
}

void RemoteWindowRegistry::setRestoreGeometry(QObject* window, const QRect& geometry)
{
    if (window) {
        m_restoreGeometries.insert(window, geometry);
    }
}

QRect RemoteWindowRegistry::restoreGeometry(QObject* window) const
{
    return window ? m_restoreGeometries.value(window) : QRect();
}

void RemoteWindowRegistry::clearRestoreGeometries()
{
    m_restoreGeometries.clear();
}

bool RemoteWindowRegistry::windowsTiled() const
{
    return m_windowsTiled;
}

void RemoteWindowRegistry::setWindowsTiled(bool tiled)
{
    m_windowsTiled = tiled;
}

void RemoteWindowRegistry::clear()
{
    m_normalWindows.clear();
    m_tiledWindows.clear();
    m_activationOrder.clear();
    m_restoreGeometries.clear();
    m_windowsTiled = false; // wjy: 完整清理后恢复未平铺的空闲状态。
}
// ===end====

} // namespace ui
