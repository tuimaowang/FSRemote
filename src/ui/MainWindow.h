#pragma once

#include <QMainWindow>

class QCloseEvent;

namespace ui {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
};

} // namespace ui
