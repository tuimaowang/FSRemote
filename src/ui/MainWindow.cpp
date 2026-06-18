#include "ui/MainWindow.h"

#include "ui/DeviceGrid.h"

#include <QIcon>
#include <QPainterPath>
#include <QRegion>

namespace ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("MainWindow"));
    setWindowTitle(QString::fromUtf8("\xE4\xB8\xB0\xE5\xAE\x9E\xE8\xBF\x9C\xE7\xA8\x8B\xE6\x8E\xA7\xE5\x88\xB6"));
    setWindowIcon(QIcon(QStringLiteral(":/UUGuest/resource/images/titlebar/app_icon.ico")));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setFixedSize(920, 680);

    QPainterPath windowPath;
    windowPath.addRoundedRect(QRectF(0, 0, 920, 680), 6, 6);
    setMask(QRegion(windowPath.toFillPolygon().toPolygon()));

    setCentralWidget(new DeviceGrid(this));
}

} // namespace ui
