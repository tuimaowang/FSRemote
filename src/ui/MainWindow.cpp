#include "ui/MainWindow.h"

#include "system/WjyDiagnosticLog.h"
#include "ui/DeviceGrid.h"

#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QTimer>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

    // =====wjy====
    setupTrayIcon();
    // ===end====

    writeWindowStartupLog(QStringLiteral("[wjy-window] MainWindow ctor end"));
}

// =====wjy====
MainWindow::~MainWindow()
{
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
    // wjy: 托盘菜单未挂到 this，需手动释放。
    delete m_trayMenu;
    m_trayMenu = nullptr;
}

void MainWindow::setupTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        writeWindowStartupLog(QStringLiteral("[wjy-window] system tray unavailable"));
        return;
    }

    // wjy: 托盘菜单不挂到可能被 hide 的主窗口上，避免 Windows 上右键菜单第二次失效。
    m_trayMenu = new QMenu();
    m_trayMenu->setAttribute(Qt::WA_QuitOnClose, false);
    m_trayShowAction = m_trayMenu->addAction(QString::fromUtf8("打开主窗口"));
    m_trayQuitAction = m_trayMenu->addAction(QString::fromUtf8("退出"));
    connect(m_trayShowAction, &QAction::triggered, this, &MainWindow::showFromTray, Qt::UniqueConnection);
    connect(m_trayQuitAction, &QAction::triggered, this, [this] {
        // wjy: 直接强制退出，不走“关闭进托盘”分支。
        m_forceQuit = true;
        if (m_trayIcon) {
            m_trayIcon->hide();
        }
        if (auto* deviceGrid = qobject_cast<DeviceGrid*>(centralWidget())) {
            deviceGrid->prepareForApplicationExit();
        }
        QApplication::quit();
    }, Qt::UniqueConnection);

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(windowIcon());
    m_trayIcon->setToolTip(windowTitle());
    m_trayIcon->setContextMenu(m_trayMenu);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        // wjy: Windows 上单击/双击都恢复主窗口；右键交给系统托盘菜单。
        if (reason == QSystemTrayIcon::Trigger
            || reason == QSystemTrayIcon::DoubleClick
            || reason == QSystemTrayIcon::MiddleClick) {
            // 延迟一拍，避开托盘消息与窗口激活抢焦点。
            QTimer::singleShot(0, this, [this] { showFromTray(); });
        }
    }, Qt::UniqueConnection);
    m_trayIcon->show();
}

void MainWindow::showFromTray()
{
    if (isHidden()) {
        show();
    }
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    showNormal();
    raise();
    activateWindow();
#if defined(Q_OS_WIN)
    // wjy: Windows 托盘恢复时强制置前，否则常被其它窗口压住看起来“没打开”。
    ::SetForegroundWindow(reinterpret_cast<HWND>(winId()));
#endif
}

void MainWindow::hideToTray()
{
    // wjy: 仅标题栏关闭按钮调用——隐藏窗口但保留托盘；任务栏按钮随之消失是 hide 的正常行为。
    // 最小化请走系统 minimize，保留任务栏状态。
    if (m_trayIcon && m_trayIcon->isVisible()) {
        hide();
        return;
    }
    showMinimized();
}

bool MainWindow::event(QEvent* event)
{
    // wjy: 不再在最小化时 hide()，确保任务栏按钮保留。
    return QMainWindow::event(event);
}
// ===end====

void MainWindow::closeEvent(QCloseEvent* event)
{
    writeWindowStartupLog(QStringLiteral("[wjy-window] closeEvent begin forceQuit=%1")
        .arg(m_forceQuit ? 1 : 0));
    // =====wjy====
    if (!m_forceQuit && m_trayIcon && m_trayIcon->isVisible()) {
        event->ignore();
        hideToTray(); // wjy: 点关闭默认隐藏到托盘；托盘“退出”才真正结束进程。
        return;
    }
    // ===end====
    if (auto* deviceGrid = qobject_cast<DeviceGrid*>(centralWidget())) {
        deviceGrid->prepareForApplicationExit();
    }
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
    event->accept();
    QMainWindow::closeEvent(event);
    QApplication::quit();
    writeWindowStartupLog(QStringLiteral("[wjy-window] closeEvent quit requested"));
}

} // namespace ui
