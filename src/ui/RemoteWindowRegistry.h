#pragma once

#include <QHash>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QVector>

class QObject;

namespace ui {

// =====wjy====
class RemoteWindowRegistry final {
public:
    QPointer<QObject> normalWindow(const QString& hostIp) const;
    void registerNormalWindow(const QString& hostIp, QObject* window);
    void registerTiledWindow(QObject* window);
    void removeNormalWindow(const QString& hostIp, QObject* window);
    void removeWindow(QObject* window);

    QVector<QPointer<QObject>> tiledWindows() const;
    QVector<QPointer<QObject>> allWindows() const;
    QVector<QPointer<QObject>> activationOrder() const;
    void clearTiledWindows();
    void clearActivationAndRestoreState();

    void rememberActivation(QObject* window);
    void setRestoreGeometry(QObject* window, const QRect& geometry);
    QRect restoreGeometry(QObject* window) const;
    void clearRestoreGeometries();

    bool windowsTiled() const;
    void setWindowsTiled(bool tiled);
    void clear();

private:
    QHash<QString, QPointer<QObject>> m_normalWindows; // wjy: 普通窗口按规范化 IP 保持唯一身份。
    QVector<QPointer<QObject>> m_tiledWindows; // wjy: 平铺批次单独保存，允许重新平铺时只关闭旧批次。
    QVector<QPointer<QObject>> m_activationOrder; // wjy: 激活顺序从旧到新排列，快捷键从末尾选择目标。
    QHash<QObject*, QRect> m_restoreGeometries; // wjy: QObject 销毁时由协调器移除，避免地址复用恢复错误几何。
    bool m_windowsTiled = false;
};
// ===end====

} // namespace ui
