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

#include <chrono>
#include <thread>

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
    removeTrayIcon(); // wjy: 正常析构也复用同步注销逻辑，保证所有退出路径都向系统托盘发送删除消息。
    // wjy: 托盘菜单未挂到 this，需手动释放。
    delete m_trayMenu;
    m_trayMenu = nullptr;
}

void MainWindow::setupTrayIcon()
{
    if (m_trayIcon) {
        return; // wjy: 防止未来重复调用初始化时创建第二个 QSystemTrayIcon。
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        writeWindowStartupLog(QStringLiteral("[wjy-window] system tray unavailable"));
        return;
    }

    // wjy: 托盘菜单不挂到可能被 hide 的主窗口上，避免 Windows 上右键菜单第二次失效。
    m_trayMenu = new QMenu();
    m_trayMenu->setAttribute(Qt::WA_QuitOnClose, false);
    m_trayShowAction = m_trayMenu->addAction(QString::fromUtf8("打开主窗口"));
    m_trayQuitAction = m_trayMenu->addAction(QString::fromUtf8("退出"));
    // =====wjy====
    connect(m_trayMenu, &QMenu::triggered, this, [this](QAction* action) {
        writeWindowStartupLog(QStringLiteral("[wjy-tray] menu triggered action=%1 ptr=%2")
            .arg(action ? action->text() : QStringLiteral("<null>"))
            .arg(reinterpret_cast<quintptr>(action))); // wjy: 统一监听菜单级 triggered，绕开当前设备上单个 QAction 回调未送达的问题。
        if (action == m_trayQuitAction) {
            writeWindowStartupLog(QStringLiteral("[wjy-exit] tray quit action triggered"));
            requestApplicationExit(); // wjy: 只有精确匹配“退出”动作时进入统一退出流程，避免菜单关闭等事件误触发。
            return;
        }
        if (action == m_trayShowAction) {
            showFromTray(); // wjy: “打开主窗口”也由同一个菜单分发入口处理，保证两项行为一致可诊断。
        }
    });
    // ===end====

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(windowIcon());
    m_trayIcon->setToolTip(windowTitle());
    m_trayIcon->setContextMenu(m_trayMenu); // wjy: 恢复 Windows/Qt 原生右键菜单显示；动作执行仍由上面的 QMenu::triggered 统一接管。
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        writeWindowStartupLog(QStringLiteral("[wjy-tray] activated reason=%1").arg(static_cast<int>(reason))); // wjy: 记录 Windows 实际回传的托盘点击类型，右键应为 Context。
        if (reason == QSystemTrayIcon::Context) {
            writeWindowStartupLog(QStringLiteral("[wjy-tray] native context menu requested")); // wjy: 菜单显示由 setContextMenu 负责，避免手动 popup 在部分 Qt/Windows 组合下完全收不到右键事件。
            return; // wjy: 右键只负责菜单，不执行打开主窗口逻辑。
        }
        // wjy: Windows 上单击/双击仍恢复主窗口。
        if (reason == QSystemTrayIcon::Trigger
            || reason == QSystemTrayIcon::DoubleClick
            || reason == QSystemTrayIcon::MiddleClick) {
            // 延迟一拍，避开托盘消息与窗口激活抢焦点。
            QTimer::singleShot(0, this, [this] { showFromTray(); });
        }
    }, Qt::UniqueConnection);
    m_trayIcon->show();
    writeWindowStartupLog(QStringLiteral("[wjy-tray] tray icon shown ptr=%1")
        .arg(reinterpret_cast<quintptr>(m_trayIcon))); // wjy: 每个进程只应出现一条 shown，可据此判断是否重复创建托盘对象。
}

void MainWindow::removeTrayIcon()
{
    writeWindowStartupLog(QStringLiteral("[wjy-exit] remove tray begin ptr=%1 visible=%2")
        .arg(reinterpret_cast<quintptr>(m_trayIcon))
        .arg(m_trayIcon && m_trayIcon->isVisible() ? 1 : 0)); // wjy: 记录注销前对象和值，确认幽灵图标是否因未进入 hide/delete 产生。
    if (m_trayMenu) {
        m_trayMenu->hide(); // wjy: 退出动作由菜单触发时先收起弹出菜单，避免旧窗口消失后菜单短暂残留。
    }
    if (!m_trayIcon) {
        return;
    }
    m_trayIcon->setContextMenu(nullptr); // wjy: 先断开菜单关联，再销毁托盘对象，避免退出信号栈仍引用菜单时发生重入。
    m_trayIcon->hide(); // wjy: hide 会同步发送 Windows NIM_DELETE，必须在可能阻塞或强杀进程之前执行。
    delete m_trayIcon; // wjy: 立即析构而不是等待 MainWindow 析构，更新重启前系统托盘已确认移除旧图标。
    m_trayIcon = nullptr;
    writeWindowStartupLog(QStringLiteral("[wjy-exit] remove tray end")); // wjy: 有 begin 无 end 表示卡在 Windows 托盘注销或对象析构。
}

void MainWindow::requestApplicationExit()
{
    if (m_exitRequested) {
        writeWindowStartupLog(QStringLiteral("[wjy-exit] duplicate exit request ignored"));
        return; // wjy: 多个退出来源同时到达时只允许第一条路径负责清理。
    }
    writeWindowStartupLog(QStringLiteral("[wjy-exit] request begin"));
    m_exitRequested = true;
    m_forceQuit = true; // wjy: 若退出过程中触发 closeEvent，不再被“隐藏到托盘”逻辑拦截。
    removeTrayIcon(); // wjy: 第一优先级注销托盘图标，后续即使后台清理卡住也不会产生幽灵图标。

#if defined(Q_OS_WIN)
    std::thread([] {
        writeWindowStartupLog(QStringLiteral("[wjy-exit] watchdog thread armed timeoutMs=8000")); // wjy: 确认兜底线程确实创建并开始计时。
        std::this_thread::sleep_for(std::chrono::seconds(8)); // wjy: 正常清理最多保留 8 秒；托盘退出没有外部更新器，因此需要进程内兜底。
        writeWindowStartupLog(QStringLiteral("[wjy-exit] watchdog firing TerminateProcess")); // wjy: 若日志以此结尾，说明正常退出链路在此前某一步卡满 8 秒。
        ::TerminateProcess(::GetCurrentProcess(), 0); // wjy: 若代码还能执行到这里说明主进程仍未退出，强制结束避免托盘“退出”永久无效。
    }).detach();
#endif

    QApplication::quit(); // wjy: 事件循环已运行时立即登记退出，即便下面的清理短暂阻塞，返回后也不会重新进入普通运行状态。
    QTimer::singleShot(0, qApp, &QCoreApplication::quit); // wjy: 启动阶段检查到更新时 app.exec 尚未进入，用零延迟任务保证事件循环启动第一拍就正常退出。
    writeWindowStartupLog(QStringLiteral("[wjy-exit] quit requested before DeviceGrid prepare"));
    if (auto* deviceGrid = qobject_cast<DeviceGrid*>(centralWidget())) {
        writeWindowStartupLog(QStringLiteral("[wjy-exit] DeviceGrid prepare begin"));
        deviceGrid->prepareForApplicationExit(); // wjy: 在看门狗保护下尝试正常取消后台任务和关闭远控连接。
        writeWindowStartupLog(QStringLiteral("[wjy-exit] DeviceGrid prepare end"));
    }
    writeWindowStartupLog(QStringLiteral("[wjy-exit] request end"));
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
    event->accept();
    QMainWindow::closeEvent(event);
    requestApplicationExit(); // wjy: 真正关闭也复用托盘菜单和更新流程的统一退出入口。
    writeWindowStartupLog(QStringLiteral("[wjy-window] closeEvent quit requested"));
}

} // namespace ui
