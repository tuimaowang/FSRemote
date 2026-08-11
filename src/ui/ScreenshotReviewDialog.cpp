#include "ui/ScreenshotReviewDialog.h"

#include "system/ScreenshotService.h"

#include <QCursor>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ui {

// =====wjy====
ScreenshotReviewDialog::ScreenshotReviewDialog(const QString& filePath, QWidget* parent)
    : QDialog(parent)
    , m_filePath(QDir::toNativeSeparators(filePath))
{
    setWindowTitle(QString::fromUtf8("截图确认"));
    setModal(true);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    QScreen* screen = parent && parent->screen()
        ? parent->screen()
        : QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QSize availableSize = screen ? screen->availableGeometry().size() : QSize(1280, 800);
    resize(qMin(1100, qMax(720, availableSize.width() * 4 / 5)),
        qMin(820, qMax(520, availableSize.height() * 4 / 5))); // wjy: 大图窗口限制在当前显示器80%范围，保留桌面边缘并允许滚动检查原始像素。

    const QPixmap screenshot(m_filePath);
    auto* imageLabel = new QLabel(this);
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setPixmap(screenshot); // wjy: 不缩放截图像素，用户通过滚动区域查看目标端保存的真实PNG细节。
    imageLabel->setMinimumSize(screenshot.size());

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidget(imageLabel);
    scrollArea->setWidgetResizable(false);
    scrollArea->setAlignment(Qt::AlignCenter);
    scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea{border:1px solid #DDE3EA;background:#161A20;}"
        "QScrollBar:horizontal,QScrollBar:vertical{background:#EEF1F5;}"));

    auto* nameLabel = new QLabel(QString::fromUtf8("文件名："), this);
    m_fileNameEdit = new QLineEdit(QFileInfo(m_filePath).completeBaseName(), this);
    m_fileNameEdit->setClearButtonEnabled(true);
    m_fileNameEdit->setStyleSheet(QStringLiteral(
        "QLineEdit{height:32px;border:1px solid #CBD5E1;border-radius:4px;padding:0 9px;"
        "font-family:'Microsoft YaHei UI';font-size:13px;color:#111827;background:#FFFFFF;}"
        "QLineEdit:focus{border-color:#3A7BFC;}"));
    auto* extensionLabel = new QLabel(QStringLiteral(".png"), this);

    auto* nameLayout = new QHBoxLayout();
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->addWidget(nameLabel);
    nameLayout->addWidget(m_fileNameEdit, 1);
    nameLayout->addWidget(extensionLabel);

    auto* buttons = new QDialogButtonBox(this);
    QPushButton* keepButton = buttons->addButton(QString::fromUtf8("确定"), QDialogButtonBox::AcceptRole);
    QPushButton* deleteButton = buttons->addButton(QString::fromUtf8("删除"), QDialogButtonBox::DestructiveRole);
    keepButton->setDefault(true);
    connect(keepButton, &QPushButton::clicked, this, [this] { keepScreenshot(); });
    connect(deleteButton, &QPushButton::clicked, this, &QDialog::reject); // wjy: 删除按钮关闭窗口，析构函数按未确认状态删除当前共享文件。

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(14, 14, 14, 14);
    rootLayout->setSpacing(10);
    rootLayout->addWidget(scrollArea, 1);
    rootLayout->addLayout(nameLayout);
    rootLayout->addWidget(buttons);
}

ScreenshotReviewDialog::~ScreenshotReviewDialog()
{
    if (!m_retained) deleteScreenshotFile(); // wjy: 点击删除、Esc、标题栏关闭或程序关闭都不保留截图，严格执行“确定才保留”。
}

void ScreenshotReviewDialog::keepScreenshot()
{
    if (!platform::ScreenshotService::isManagedScreenshotPath(m_filePath)) {
        QMessageBox::warning(this, QString::fromUtf8("无法保留"), QString::fromUtf8("截图文件不在固定共享目录中。"));
        return;
    }
    const QString requestedName = m_fileNameEdit ? m_fileNameEdit->text().trimmed() : QString();
    if (requestedName.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("文件名无效"), QString::fromUtf8("请输入截图文件名。"));
        return;
    }
    const QString safeName = platform::ScreenshotService::sanitizeFileNamePart(requestedName, QString());
    if (safeName != requestedName && m_fileNameEdit) {
        m_fileNameEdit->setText(safeName); // wjy: 将Windows非法字符替换结果直接展示，用户确认后再次点击确定即可保留可用名称。
        QMessageBox::information(this, QString::fromUtf8("文件名已调整"), QString::fromUtf8("文件名中的非法字符已替换，请确认后再次点击确定。"));
        return;
    }

    const QFileInfo currentInfo(m_filePath);
    const QString destination = QDir(currentInfo.absolutePath()).filePath(safeName + QStringLiteral(".png"));
    if (QDir::cleanPath(destination).compare(QDir::cleanPath(m_filePath), Qt::CaseInsensitive) != 0) {
        if (QFileInfo::exists(destination)) {
            QMessageBox::warning(this, QString::fromUtf8("无法重命名"), QString::fromUtf8("该文件名已经存在，请换一个名称。"));
            return;
        }
        if (!QFile::rename(m_filePath, destination)) {
            QMessageBox::warning(this, QString::fromUtf8("无法重命名"), QString::fromUtf8("共享目录中的截图重命名失败。"));
            return;
        }
        m_filePath = QDir::toNativeSeparators(destination); // wjy: 后续析构和保留状态始终指向重命名后的真实文件，不会误删旧路径。
    }

    m_retained = true;
    QDialog::accept(); // wjy: 只有文件名校验和可选重命名全部成功后才提交保留结果。
}

void ScreenshotReviewDialog::deleteScreenshotFile()
{
    if (platform::ScreenshotService::isManagedScreenshotPath(m_filePath)) {
        QFile::remove(m_filePath); // wjy: 删除范围受固定共享目录和PNG扩展名双重限制，远端返回值不能删除其它文件。
    }
}
// ===end====

} // namespace ui
