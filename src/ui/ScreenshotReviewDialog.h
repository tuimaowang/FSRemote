#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;

namespace ui {

// =====wjy====
class ScreenshotReviewDialog final : public QDialog {
public:
    explicit ScreenshotReviewDialog(const QString& filePath, QWidget* parent = nullptr);
    ~ScreenshotReviewDialog() override;

private:
    void keepScreenshot();
    void deleteScreenshotFile();

    QString m_filePath;
    QLineEdit* m_fileNameEdit = nullptr;
    bool m_retained = false;
}; // wjy: 对话框拥有未确认截图的删除责任，只有“确定”成功后才把文件所有权交还用户。
// ===end====

} // namespace ui
