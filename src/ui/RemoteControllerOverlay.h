#pragma once

#include <QByteArray>
#include <QPoint>
#include <QStringList>
#include <QWidget>

class QPainter;
class QPaintEvent;
class QMouseEvent;
class QPropertyAnimation;
class QTimer;

namespace ui {

class RemoteControllerOverlay final : public QWidget {
public:
    explicit RemoteControllerOverlay(QWidget* parent = nullptr);
    void setControllers(const QStringList& controllers);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void pollMouseState();
    void beginDrag(const QPoint& cursor);
    void updateDrag(const QPoint& cursor);
    void finishDrag(const QPoint& cursor);
    void forwardClickThrough();
    void paintVortex(QPainter& painter);
    void updatePosition();
    void showExpanded(bool startAutoCollapse);
    void showCollapsed();
    void animateTo(const QRect& geometry, int durationMs);
    QRect dragGeometry(const QPoint& cursor) const;
    QRect expandedGeometry() const;
    QRect collapsedGeometry() const;
    QString collapsedTitle() const;

    QStringList m_controllers;
    QTimer* m_autoCollapseTimer = nullptr;
    QTimer* m_hoverExpandTimer = nullptr; // wjy: 收起徽标连续悬停达到意图阈值后才展开，快速划过时由该单次计时器自动取消。
    QTimer* m_hoverPollTimer = nullptr;
    QPropertyAnimation* m_geometryAnimation = nullptr;
    bool m_expanded = false;
    bool m_hoverExpanded = false;
    bool m_dragCandidate = false;
    bool m_dragging = false;
    bool m_waitForPostDragLeave = false;
    bool m_forwardingClick = false;
    QPoint m_dragPressPosition;
    qreal m_vortexAngle = 0.0;
    int m_dockedCenterY = -1;
};

} // namespace ui
