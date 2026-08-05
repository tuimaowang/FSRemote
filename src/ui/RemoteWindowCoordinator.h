#pragma once

#include <QPointer>
#include <QRect>
#include <QString>
#include <QVector>

#include "ui/RemoteWindowRegistry.h"

namespace ui {

class RemoteDesktopWindow;

// =====wjy====
class RemoteWindowCoordinator final {
public:
    QPointer<RemoteDesktopWindow> normalWindow(const QString& hostIp) const;
    void registerNormalWindow(const QString& hostIp, RemoteDesktopWindow* window);
    void registerTiledWindow(RemoteDesktopWindow* window);
    void removeWindow(const QString& hostIp, RemoteDesktopWindow* window);
    void removeWindow(RemoteDesktopWindow* window);

    void closeTiledWindows();
    void closeAllWindows();
    QVector<QPointer<RemoteDesktopWindow>> openedWindows() const;
    QVector<QPointer<RemoteDesktopWindow>> shutdownWindows() const;

    void rememberActivation(RemoteDesktopWindow* window);
    RemoteDesktopWindow* topmostWindow() const;

    void setRestoreGeometry(RemoteDesktopWindow* window, const QRect& geometry);
    QRect restoreGeometry(RemoteDesktopWindow* window) const;
    void clearRestoreGeometries();
    bool windowsTiled() const;
    void setWindowsTiled(bool tiled);
    void clear();

private:
    RemoteWindowRegistry m_registry; // wjy: 纯窗口身份、激活、平铺和几何状态交给可单测注册表；协调器只保留真实窗口关闭规则。
};
// ===end====

} // namespace ui
