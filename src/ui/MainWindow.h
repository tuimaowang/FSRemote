#pragma once

#include <QMainWindow>
#include <QStringList>

class QCloseEvent;
class QEvent;
class QSystemTrayIcon;
class QMenu;
class QAction;

namespace ui {

class RemoteControllerOverlay;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    // =====wjy====
    ~MainWindow() override;
    void showFromTray();
    void hideToTray(); // wjy: 仅关闭按钮使用；最小化仍保留任务栏按钮。
    void setRemoteControllerOverlayEntries(const QStringList& controllers); // wjy: 目标端主循环同步远控会话详情，驱动右下角提示层显示或隐藏。
    void requestApplicationRestart(); // wjy: 托盘“重启”先创建等待旧进程退出的新实例，再复用统一退出清理流程。
    void requestApplicationExit(); // wjy: 托盘退出和更新退出统一走这里，先注销托盘图标，再有界清理并退出进程。
    // ===end====

protected:
    void closeEvent(QCloseEvent* event) override;
    // =====wjy====
    bool event(QEvent* event) override;
    // ===end====

private:
    // =====wjy====
    void setupTrayIcon();
    void removeTrayIcon(); // wjy: 同步向 Windows 通知区域删除图标，避免更新强退后留下鼠标经过才消失的幽灵图标。
    QSystemTrayIcon* m_trayIcon = nullptr;
    QMenu* m_trayMenu = nullptr;
    QAction* m_trayShowAction = nullptr;
    QAction* m_trayRestartAction = nullptr;
    QAction* m_trayQuitAction = nullptr;
    RemoteControllerOverlay* m_remoteControllerOverlay = nullptr;
    bool m_forceQuit = false;
    bool m_exitRequested = false; // wjy: 防止托盘动作、窗口关闭和更新信号重复进入清理流程或重复创建退出看门狗。
    // ===end====
};

} // namespace ui
