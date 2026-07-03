#include "ui/MainWindow.h"

#include "system/WjyDiagnosticLog.h"
#include "ui/DeviceGrid.h"

#include <QIcon>
#include <QPainterPath>
#include <QRegion>

namespace {

void writeWindowStartupLog(const QString& message)
{
    platform::writeWjyDiagnosticLog(message); // wjy: Keep MainWindow startup diagnostics centralized during heap isolation.
}

} // namespace

namespace ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    writeWindowStartupLog(QStringLiteral("[wjy-window] MainWindow ctor begin"));

    setObjectName(QStringLiteral("MainWindow"));
    setWindowTitle(QString::fromUtf8("\xE4\xB8\xB0\xE5\xAE\x9E\xE8\xBF\x9C\xE7\xA8\x8B\xE6\x8E\xA7\xE5\x88\xB6"));
    setWindowIcon(QIcon(QStringLiteral(":/UUGuest/resource/images/titlebar/app_icon.ico")));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setFixedSize(920, 680);
    writeWindowStartupLog(QStringLiteral("[wjy-window] window basics set"));

    QPainterPath windowPath;
    windowPath.addRoundedRect(QRectF(0, 0, 920, 680), 6, 6);
    setMask(QRegion(windowPath.toFillPolygon().toPolygon()));
    writeWindowStartupLog(QStringLiteral("[wjy-window] rounded mask set"));

    DeviceGrid* deviceGrid = new DeviceGrid(this); // wjy: Main window always owns the real DeviceGrid now that startup isolation is complete.
    writeWindowStartupLog(QStringLiteral("[wjy-window] after DeviceGrid create"));
    setCentralWidget(deviceGrid); // wjy: Restore the normal central widget path and remove the placeholder isolation branch.

    writeWindowStartupLog(QStringLiteral("[wjy-window] MainWindow ctor end"));
}

} // namespace ui
