#include "ui/MainWindow.h"

#include "system/WjyDiagnosticLog.h"
#include "ui/DeviceGrid.h"

#include <QApplication>
#include <QCloseEvent>
#include <QIcon>

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
    setMinimumSize(720, 520);
    resize(920, 680);
    writeWindowStartupLog(QStringLiteral("[wjy-window] window basics set"));

    DeviceGrid* deviceGrid = new DeviceGrid(this); // wjy: Main window always owns the real DeviceGrid now that startup isolation is complete.
    writeWindowStartupLog(QStringLiteral("[wjy-window] after DeviceGrid create"));
    setCentralWidget(deviceGrid); // wjy: Restore the normal central widget path and remove the placeholder isolation branch.

    writeWindowStartupLog(QStringLiteral("[wjy-window] MainWindow ctor end"));
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    writeWindowStartupLog(QStringLiteral("[wjy-window] closeEvent begin"));
    if (auto* deviceGrid = qobject_cast<DeviceGrid*>(centralWidget())) {
        deviceGrid->prepareForApplicationExit();
    }
    QMainWindow::closeEvent(event);
    QApplication::quit();
    writeWindowStartupLog(QStringLiteral("[wjy-window] closeEvent quit requested"));
}

} // namespace ui
