#pragma once

#include <QMainWindow>

class QCloseEvent;
class QEvent;
class QSystemTrayIcon;
class QMenu;
class QAction;

namespace ui {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    // =====wjy====
    ~MainWindow() override;
    void showFromTray();
    void hideToTray(); // wjy: 仅关闭按钮使用；最小化仍保留任务栏按钮。
    // ===end====

protected:
    void closeEvent(QCloseEvent* event) override;
    // =====wjy====
    bool event(QEvent* event) override;
    // ===end====

private:
    // =====wjy====
    void setupTrayIcon();
    QSystemTrayIcon* m_trayIcon = nullptr;
    QMenu* m_trayMenu = nullptr;
    QAction* m_trayShowAction = nullptr;
    QAction* m_trayQuitAction = nullptr;
    bool m_forceQuit = false;
    // ===end====
};

} // namespace ui
