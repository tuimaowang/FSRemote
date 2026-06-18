#include "ui/DeviceGrid.h"

#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "system/DeviceInfoService.h"
#include "system/DeviceStatusService.h"
#include "system/PowerManager.h"
#include "system/PortableOpenSshManager.h"
#include "system/StartupManager.h"
#include "system/WolDetector.h"
#include "ui/RemoteDesktopWindow.h"

#include <QAction>
#include <QAbstractSocket>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QComboBox>
#include <QHostAddress>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <tuple>
#include <thread>
#include <vector>

namespace ui {

namespace {

QString zh(const char* utf8)
{
    return QString::fromUtf8(utf8);
}

QPixmap uupix(const QString& name)
{
    return QPixmap(QStringLiteral(":/UUGuest/resource/images/") + name);
}

QIcon menuIcon(const QString& name)
{
    return QIcon(QStringLiteral(":/UUGuest/resource/images/menu/") + name);
}

void drawDeviceTileIcon(QPainter& painter, int x, int y, int size = 20)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
    painter.drawRoundedRect(QRectF(x, y, size, size), 4, 4);

    const qreal cell = size >= 20 ? 5.0 : 4.0;
    const qreal gap = size >= 20 ? 2.0 : 1.6;
    const qreal left = x + (size - cell * 2 - gap) / 2.0;
    const qreal top = y + (size - cell * 2 - gap) / 2.0;
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(QRectF(left, top, cell, cell), 1, 1);
    painter.drawRoundedRect(QRectF(left + cell + gap, top, cell, cell), 1, 1);
    painter.drawRoundedRect(QRectF(left, top + cell + gap, cell, cell), 1, 1);
    painter.drawRoundedRect(QRectF(left + cell + gap, top + cell + gap, cell, cell), 1, 1);
}

void drawRemoteBadge(QPainter& painter, int x, int y)
{
    painter.drawPixmap(
        QRect(x, y, 26, 16),
        uupix(QStringLiteral("titlebar/remote_badge.svg")));
}

void drawRoundedDesktopImage(QPainter& painter, const QRectF& target, const QPixmap& pixmap, qreal radius)
{
    QPainterPath path;
    path.addRoundedRect(target, radius, radius);

    QRect source = pixmap.rect();
    const qreal targetRatio = target.width() / target.height();
    const int coverHeight = qRound(source.width() / targetRatio);
    if (coverHeight < source.height()) {
        source.setY(20);
        source.setHeight(coverHeight);
    }

    painter.save();
    painter.setClipPath(path);
    painter.drawPixmap(target.toRect(), pixmap, source);
    painter.restore();
}

void drawUiIcon(QPainter& painter, const QRect& target, const QString& name)
{
    painter.drawPixmap(target, uupix(QStringLiteral("titlebar/") + name));
}

void drawResourceIcon(QPainter& painter, const QRect& target, const QString& name)
{
    painter.drawPixmap(target, uupix(name));
}

QRect minimizeRect()
{
    return QRect(836, 0, 48, 48);
}

QRect refreshRect()
{
    return QRect(788, 0, 48, 48);
}

QRect closeRect()
{
    return QRect(872, 0, 48, 48);
}

QRect deviceGroupHeaderRect()
{
    return QRect(0, 58, 236, 34);
}

struct DeviceEntry {
    QString name;
    QString ip;
    QString mac;
    QString broadcastIp;
    QString remark;
};

class RenameDeviceDialog final : public QDialog {
public:
    explicit RenameDeviceDialog(const QString& currentName, QWidget* parent = nullptr)
        : QDialog(parent)
    {
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedSize(443, 219);

        auto* title = new QLabel(zh("\xE4\xBF\xAE\xE6\x94\xB9\xE8\xAE\xBE\xE5\xA4\x87\xE5\x90\x8D\xE7\xA7\xB0"), this);
        title->setGeometry(23, 24, 180, 24);
        title->setStyleSheet(QStringLiteral(
            "QLabel{font-family:'Microsoft YaHei UI';font-size:16px;color:#000000;background:transparent;}"));

        m_nameEdit = new QLineEdit(this);
        m_nameEdit->setGeometry(23, 59, 400, 32);
        m_nameEdit->setMaxLength(40);
        m_nameEdit->setText(currentName);
        m_nameEdit->selectAll();
        m_nameEdit->setStyleSheet(QStringLiteral(
            "QLineEdit{background:#FFFFFF;border:1px solid #DADDE2;border-radius:3px;"
            "padding:0 30px 0 10px;font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
            "QLineEdit:focus{border:1px solid #006BFF;}"));
        auto* clearButton = new QToolButton(this);
        clearButton->setGeometry(395, 64, 22, 22);
        clearButton->setIcon(QIcon(QStringLiteral(":/UUGuest/resource/images/titlebar/close.svg")));
        clearButton->setIconSize(QSize(10, 10));
        clearButton->setCursor(Qt::PointingHandCursor);
        clearButton->setStyleSheet(QStringLiteral("QToolButton{border:0;background:transparent;padding:0;}"));
        clearButton->raise();
        connect(clearButton, &QToolButton::clicked, m_nameEdit, &QLineEdit::clear);

        auto* hint = new QLabel(zh("\xE6\x9C\x80\xE5\xA4\x9A\xE5\x8F\xAF\xE8\xBE\x93\xE5\x85\xA5" "40" "\xE4\xB8\xAA\xE5\xAD\x97\xE7\xAC\xA6"), this);
        hint->setGeometry(23, 99, 180, 18);
        hint->setStyleSheet(QStringLiteral(
            "QLabel{font-family:'Microsoft YaHei UI';font-size:12px;color:#666666;background:transparent;}"));

        m_countLabel = new QLabel(this);
        m_countLabel->setGeometry(367, 99, 56, 18);
        m_countLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_countLabel->setStyleSheet(QStringLiteral(
            "QLabel{font-family:'Microsoft YaHei UI';font-size:12px;color:#666666;background:transparent;}"));

        m_saveButton = new QPushButton(zh("\xE4\xBF\x9D\xE5\xAD\x98\xE6\x9B\xB4\xE6\x94\xB9"), this);
        m_saveButton->setGeometry(23, 164, 195, 31);
        m_saveButton->setCursor(Qt::PointingHandCursor);
        m_saveButton->setStyleSheet(QStringLiteral(
            "QPushButton{border:0;border-radius:3px;background:#3A7BFC;"
            "font-family:'Microsoft YaHei UI';font-size:14px;color:#FFFFFF;}"
            "QPushButton:hover{background:#2F6FEF;}"
            "QPushButton:disabled{background:#C9D0DA;color:#FFFFFF;}"));

        auto* cancelButton = new QPushButton(zh("\xE5\x8F\x96\xE6\xB6\x88"), this);
        cancelButton->setGeometry(227, 164, 196, 31);
        cancelButton->setCursor(Qt::PointingHandCursor);
        cancelButton->setStyleSheet(QStringLiteral(
            "QPushButton{border:1px solid #DADDE2;border-radius:3px;background:#FFFFFF;"
            "font-family:'Microsoft YaHei UI';font-size:14px;color:#000000;}"
            "QPushButton:hover{background:#F3F7FF;}"));

        connect(m_nameEdit, &QLineEdit::textChanged, this, &RenameDeviceDialog::updateState);
        connect(m_saveButton, &QPushButton::clicked, this, &QDialog::accept);
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
        updateState();
    }

    QString name() const
    {
        return m_nameEdit->text().trimmed();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QPainterPath path;
        path.addRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 4, 4);
        painter.setClipPath(path);
        painter.fillPath(path, Qt::white);
        painter.fillRect(QRectF(0, 139, width(), 80), QColor(QStringLiteral("#F6F8FA")));
        painter.setClipping(false);
        painter.setPen(QPen(QColor(QStringLiteral("#EEF1F5")), 1));
        painter.drawLine(QPointF(0, 138.5), QPointF(width(), 138.5));
        painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
        painter.drawPath(path);
    }

private:
    void updateState()
    {
        const int length = m_nameEdit->text().size();
        m_countLabel->setText(QStringLiteral("%1/40").arg(length));
        m_saveButton->setEnabled(!m_nameEdit->text().trimmed().isEmpty());
    }

    QLineEdit* m_nameEdit = nullptr;
    QLabel* m_countLabel = nullptr;
    QPushButton* m_saveButton = nullptr;
};

QString deviceDisplayName(const DeviceEntry& device)
{
    const QString name = device.name.trimmed();
    return name.isEmpty() ? device.ip.trimmed() : name;
}

bool proxyWakeCapableState(platform::DevicePresenceState state)
{
    return state == platform::DevicePresenceState::Online
        || state == platform::DevicePresenceState::Busy;
}

bool parseIpv4Address(const QString& text, quint32* outValue)
{
    if (!outValue) {
        return false;
    }

    QHostAddress address;
    if (!address.setAddress(text.trimmed()) || address.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }

    *outValue = address.toIPv4Address();
    return true;
}

bool isSameSubnet(const QString& targetIp, const QString& candidateIp, const QString& subnetMask)
{
    quint32 target = 0;
    quint32 candidate = 0;
    quint32 mask = 0;
    if (!parseIpv4Address(targetIp, &target)
        || !parseIpv4Address(candidateIp, &candidate)
        || !parseIpv4Address(subnetMask, &mask)) {
        return false;
    }

    return (target & mask) == (candidate & mask);
}

QVector<DeviceEntry> g_devices;

QString deviceStorePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data/devices.json"));
}

void saveDevices()
{
    QJsonArray array;
    for (const DeviceEntry& device : g_devices) {
        QJsonObject object;
        object.insert(QStringLiteral("name"), device.name);
        object.insert(QStringLiteral("ip"), device.ip);
        object.insert(QStringLiteral("mac"), device.mac);
        object.insert(QStringLiteral("broadcast_ip"), device.broadcastIp);
        object.insert(QStringLiteral("remark"), device.remark);
        array.append(object);
    }

    QFileInfo info(deviceStorePath());
    QDir().mkpath(info.absolutePath());

    QFile file(info.filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
}

void loadDevices()
{
    g_devices.clear();

    QFile file(deviceStorePath());
    if (!file.exists()) {
        g_devices.append({QStringLiteral("72"), QStringLiteral("192.168.3.27"), {}, {}, {}});
        saveDevices();
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray()) {
        return;
    }

    for (const QJsonValue& value : document.array()) {
        const QJsonObject object = value.toObject();
        const QString ip = object.value(QStringLiteral("ip")).toString().trimmed();
        if (ip.isEmpty()) {
            continue;
        }
        QString name = object.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            name = ip;
        }
        g_devices.append({
            name,
            ip,
            object.value(QStringLiteral("mac")).toString().trimmed(),
            object.value(QStringLiteral("broadcast_ip")).toString().trimmed(),
            object.value(QStringLiteral("remark")).toString()
        });
    }
}

QStringList deviceNames();

QRect remoteAssistGroupHeaderRect(bool deviceGroupExpanded)
{
    const int deviceRowsHeight = deviceGroupExpanded ? deviceNames().size() * 40 : 0;
    return QRect(0, deviceGroupHeaderRect().bottom() + 1 + deviceRowsHeight, 236, 34);
}

QRect remoteAssistStartRect(bool deviceGroupExpanded)
{
    return QRect(4, remoteAssistGroupHeaderRect(deviceGroupExpanded).bottom() + 1, 232, 36);
}

QRect localDeviceInfoRect(bool deviceGroupExpanded)
{
    const QRect addRect = remoteAssistStartRect(deviceGroupExpanded);
    return QRect(addRect.x(), addRect.y() + 40, addRect.width(), addRect.height());
}

QStringList deviceNames()
{
    QStringList names;
    names.reserve(g_devices.size());
    for (const DeviceEntry& device : g_devices) {
        names.append(deviceDisplayName(device));
    }
    return names;
}

QSet<int> deviceBadgeIndexes()
{
    return {};
}

QString infoValueText(const QString& value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("--") : trimmed;
}

QRect localInfoFieldRect(int index)
{
    switch (index) {
    case 0: return QRect(304, 250, 206, 34);
    case 1: return QRect(584, 250, 206, 34);
    case 2: return QRect(304, 332, 206, 34);
    case 3: return QRect(584, 332, 206, 34);
    case 4: return QRect(304, 414, 206, 34);
    case 5: return QRect(584, 414, 206, 34);
    default: return {};
    }
}

QRect localInfoCopyButtonRect(int index)
{
    const QRect field = localInfoFieldRect(index);
    return QRect(field.right() + 8, field.y() + 3, 38, 28);
}

QRect deviceRowRect(int index)
{
    return QRect(4, 96 + index * 40, 232, 36);
}

QColor deviceStatusDotColor(platform::DevicePresenceState state)
{
    switch (state) {
    case platform::DevicePresenceState::Online:
        return QColor(QStringLiteral("#28D13B"));
    case platform::DevicePresenceState::Busy:
        return QColor(QStringLiteral("#FFEC42"));
    case platform::DevicePresenceState::Offline:
        return QColor(QStringLiteral("#B0B3B8"));
    case platform::DevicePresenceState::Unknown:
    default:
        return QColor(QStringLiteral("#C7CDD6"));
    }
}

QPixmap deviceStatusBadgePixmap(platform::DevicePresenceState state)
{
    if (state == platform::DevicePresenceState::Offline) {
        return uupix(QStringLiteral("titlebar/device_status_offline.svg"));
    }
    if (state == platform::DevicePresenceState::Busy) {
        return uupix(QStringLiteral("titlebar/device_status_busy.svg"));
    }
    return {};
}

QSize deviceStatusBadgeSize(platform::DevicePresenceState state)
{
    if (state == platform::DevicePresenceState::Offline) {
        return QSize(60, 28);
    }
    if (state == platform::DevicePresenceState::Busy) {
        return QSize(72, 28);
    }
    return QSize(52, 23);
}

QString deviceStatusText(platform::DevicePresenceState state)
{
    switch (state) {
    case platform::DevicePresenceState::Busy:
        return zh("\xE8\xA2\xAB\xE5\x8D\xA0\xE7\x94\xA8");
    case platform::DevicePresenceState::Offline:
        return zh("\xE7\xA6\xBB\xE7\xBA\xBF");
    case platform::DevicePresenceState::Unknown:
        return zh("\xE6\x9C\xAA\xE6\xA3\x80\xE6\xB5\x8B");
    case platform::DevicePresenceState::Online:
    default:
        return zh("\xE5\x9C\xA8\xE7\xBA\xBF");
    }
}

QRectF desktopImageRect(bool isRemoteControlled)
{
    return QRectF(280, isRemoteControlled ? 189 : 124, 600, 240);
}

QRectF fileTransferActionRect(bool isRemoteControlled)
{
    const qreal cardTop = isRemoteControlled ? 189 : 124;
    return QRectF(281, cardTop + 240, 298, 50);
}

QRectF moreActionRect(bool isRemoteControlled)
{
    const qreal cardTop = isRemoteControlled ? 189 : 124;
    return QRectF(580, cardTop + 240, 299, 50);
}

QRectF wakeButtonRect(bool isRemoteControlled)
{
    const QRectF imageRect = desktopImageRect(isRemoteControlled);
    return QRectF(
        imageRect.center().x() - 36.0,
        imageRect.center().y() - 36.0,
        72.0,
        72.0);
}

qreal easeOutCubic(qreal value)
{
    const qreal inverse = 1.0 - value;
    return 1.0 - inverse * inverse * inverse;
}

void drawDeviceDetail(
    QPainter& painter,
    const QString& deviceName,
    platform::DevicePresenceState deviceState,
    bool poweringOn,
    int wakeRemainingSeconds,
    bool isRemoteControlled,
    qreal desktopHoverProgress,
    qreal wakeVisualRotation,
    BottomAction hoveredBottomAction,
    qreal yOffset,
    qreal opacity,
    const QFont& textFont)
{
    painter.save();
    painter.setOpacity(opacity);
    painter.translate(0, yOffset);

    constexpr int headerGroupX = 280;
    constexpr int headerGroupGap = 8;
    constexpr int headerGroupRight = 720;
    const bool offlineState = deviceState == platform::DevicePresenceState::Offline;
    const bool showWakeButton = offlineState;
    const bool showWakeLoading = offlineState && poweringOn;
    const bool dimDetailContent = offlineState;

    QFont deviceTitle(QStringLiteral("Microsoft YaHei UI"));
    deviceTitle.setPixelSize(24);
    deviceTitle.setBold(true);
    const QSize statusBadgeSize = deviceStatusBadgeSize(deviceState);
    const QRect statusRect(
        headerGroupX,
        deviceState == platform::DevicePresenceState::Online ? 76 : 73,
        statusBadgeSize.width(),
        statusBadgeSize.height());
    if (deviceState == platform::DevicePresenceState::Online || deviceState == platform::DevicePresenceState::Unknown) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(deviceState == platform::DevicePresenceState::Unknown
                ? QColor(QStringLiteral("#606266"))
                : QColor(QStringLiteral("#111111")));
        painter.drawRoundedRect(QRectF(statusRect), statusRect.height() / 2.0, statusRect.height() / 2.0);
        painter.setBrush(deviceState == platform::DevicePresenceState::Unknown
                ? QColor(QStringLiteral("#B0B3B8"))
                : QColor(QStringLiteral("#19E58B")));
        painter.drawEllipse(QRectF(statusRect.x() + 8, statusRect.y() + 7.5, 8, 8));
        QFont pillFont(QStringLiteral("Microsoft YaHei UI"));
        pillFont.setPixelSize(12);
        painter.setFont(pillFont);
        painter.setPen(Qt::white);
        painter.drawText(
            QRectF(statusRect.x() + 22, statusRect.y() + 1, statusRect.width() - 24, statusRect.height() - 2),
            Qt::AlignVCenter | Qt::AlignLeft,
            deviceStatusText(deviceState));
    } else {
        painter.drawPixmap(statusRect, deviceStatusBadgePixmap(deviceState));
    }

    const int nameX = statusRect.x() + statusRect.width() + headerGroupGap;
    painter.setFont(deviceTitle);
    painter.setPen(dimDetailContent ? QColor(QStringLiteral("#7A8595")) : QColor(QStringLiteral("#040B18")));
    painter.drawText(
        QRectF(nameX, 76, headerGroupRight - nameX, 25),
        Qt::AlignVCenter | Qt::AlignLeft,
        painter.fontMetrics().elidedText(deviceName, Qt::ElideRight, headerGroupRight - nameX));

    qreal cardTop = 124;
    if (isRemoteControlled) {
        const QRectF notice(280, 117, 600, 56);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#DDF0FF")));
        painter.drawRoundedRect(notice, 3, 3);

        drawUiIcon(painter, QRect(312, 135, 20, 20), QStringLiteral("controlled_notice.svg"));

        QFont noticeTitle(QStringLiteral("Microsoft YaHei UI"));
        noticeTitle.setPixelSize(14);
        noticeTitle.setBold(true);
        painter.setFont(noticeTitle);
        painter.setPen(QColor(QStringLiteral("#006BFF")));
        painter.drawText(
            QRectF(342, 136, 66, 20),
            Qt::AlignVCenter | Qt::AlignLeft,
            zh("\xE8\xA2\xAB\xE6\x8E\xA7\xE4\xB8\xAD"));

        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#006BFF")));
        painter.drawText(
            QRectF(427, 136, 250, 20),
            Qt::AlignVCenter | Qt::AlignLeft,
            zh("\xE5\xBD\x93\xE5\x89\x8D\xE6\x9C\x89 1 \xE5\x8F\xB0\xE6\x9C\xAC\xE8\xB4\xA6\xE5\x8F\xB7\xE8\xAE\xBE\xE5\xA4\x87\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x8E\xA7\xE5\x88\xB6\xE6\xAD\xA4\xE7\x94\xB5\xE8\x84\x91"));

        painter.setPen(QPen(QColor(QStringLiteral("#BCC8D8")), 1));
        painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
        painter.drawRoundedRect(QRectF(760, 133, 95, 24), 2, 2);
        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#040B18")));
        painter.drawText(
            QRectF(760, 133, 95, 24),
            Qt::AlignCenter,
            zh("\xE6\x96\xAD\xE5\xBC\x80\xE6\x89\x80\xE6\x9C\x89\xE8\xBF\x9C\xE6\x8E\xA7"));

        cardTop = 189;
    }

    const QRectF card(280, cardTop, 600, 291);
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(card, 4, 4);

    drawRoundedDesktopImage(painter, desktopImageRect(isRemoteControlled), uupix(QStringLiteral("desktop_bk_image.png")), 4);

    if (dimDetailContent) {
        painter.fillRect(desktopImageRect(isRemoteControlled), QColor(255, 255, 255, 112));
    }

    if (!showWakeButton) {
        QFont enterFont(QStringLiteral("Microsoft YaHei UI"));
        enterFont.setPixelSize(16);
        painter.setFont(enterFont);

        const qreal groupShift = 4.0 * desktopHoverProgress;
        const qreal arrowExtend = 5.0 * desktopHoverProgress;
        const qreal centerY = cardTop + 194.0;
        const QRectF enterTextRect(532 + groupShift, centerY - 12.0, 72, 24);
        const qreal arrowStartX = 610 + groupShift;
        const qreal arrowEndX = 627 + groupShift + arrowExtend;
        const qreal arrowHead = 6.0;

        painter.save();
        painter.setOpacity(0.18 * desktopHoverProgress);
        painter.setPen(QPen(QColor(QStringLiteral("#0B1220")), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawText(enterTextRect.translated(0, 1), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE8\xBF\x9B\xE5\x85\xA5\xE6\xA1\x8C\xE9\x9D\xA2"));
        painter.drawLine(QPointF(arrowStartX, centerY + 1), QPointF(arrowEndX, centerY + 1));
        painter.drawLine(QPointF(arrowEndX - arrowHead, centerY - arrowHead + 1), QPointF(arrowEndX, centerY + 1));
        painter.drawLine(QPointF(arrowEndX - arrowHead, centerY + arrowHead + 1), QPointF(arrowEndX, centerY + 1));
        painter.restore();

        painter.setOpacity(0.92 + 0.08 * desktopHoverProgress);
        painter.setPen(Qt::white);
        painter.drawText(enterTextRect, Qt::AlignVCenter | Qt::AlignLeft, zh("\xE8\xBF\x9B\xE5\x85\xA5\xE6\xA1\x8C\xE9\x9D\xA2"));
        painter.setPen(QPen(Qt::white, 1.45 + 0.2 * desktopHoverProgress, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(arrowStartX, centerY), QPointF(arrowEndX, centerY));
        painter.drawLine(QPointF(arrowEndX - arrowHead, centerY - arrowHead), QPointF(arrowEndX, centerY));
        painter.drawLine(QPointF(arrowEndX - arrowHead, centerY + arrowHead), QPointF(arrowEndX, centerY));
        painter.setOpacity(1.0);
    } else {
        const QRectF wakeRect = wakeButtonRect(isRemoteControlled);
        if (showWakeLoading) {
            painter.drawPixmap(wakeRect.toRect(), uupix(QStringLiteral("titlebar/wake_loading_bg.png")));
            painter.save();
            painter.translate(wakeRect.center());
            painter.rotate(wakeVisualRotation);
            painter.translate(-wakeRect.center());
            painter.drawPixmap(wakeRect.toRect(), uupix(QStringLiteral("titlebar/wake_loading_spinner.png")));
            painter.restore();

            QFont countdownFont(QStringLiteral("Microsoft YaHei UI"));
            countdownFont.setPixelSize(16);
            countdownFont.setBold(true);
            painter.setFont(countdownFont);
            painter.setPen(QColor(QStringLiteral("#3A7BFC")));
            painter.drawText(
                wakeRect.adjusted(0, 0, 0, -1),
                Qt::AlignCenter,
                QStringLiteral("%1秒").arg(qMax(0, wakeRemainingSeconds)));

            const QRectF wakeTagRect(
                wakeRect.center().x() - 41.0,
                wakeRect.bottom() + 14.0,
                82.0,
                24.0);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 242));
            painter.drawRoundedRect(wakeTagRect, 12, 12);
            painter.setPen(QPen(QColor(QStringLiteral("#D6E2FF")), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(wakeTagRect.adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
            QFont wakeTagFont(QStringLiteral("Microsoft YaHei UI"));
            wakeTagFont.setPixelSize(12);
            painter.setFont(wakeTagFont);
            painter.setPen(QColor(QStringLiteral("#3A7BFC")));
            painter.drawText(wakeTagRect, Qt::AlignCenter, zh("\xE6\xAD\xA3\xE5\x9C\xA8\xE5\xBC\x80\xE6\x9C\xBA"));
        } else {
            painter.drawPixmap(wakeRect.toRect(), uupix(QStringLiteral("titlebar/wake_power.svg")));
        }
    }

    painter.fillRect(QRectF(281, cardTop + 240, 598, 50), QColor(QStringLiteral("#FFFFFF")));
    if (dimDetailContent) {
        painter.fillRect(fileTransferActionRect(isRemoteControlled), QColor(QStringLiteral("#F5F7FA")));
    } else if (hoveredBottomAction == BottomAction::FileTransfer) {
        painter.fillRect(fileTransferActionRect(isRemoteControlled), QColor(QStringLiteral("#F3F7FF")));
        painter.fillRect(QRectF(281, cardTop + 240, 298, 2), QColor(QStringLiteral("#3A7BFC")));
    } else if (hoveredBottomAction == BottomAction::More) {
        painter.fillRect(moreActionRect(isRemoteControlled), QColor(QStringLiteral("#F3F7FF")));
        painter.fillRect(QRectF(580, cardTop + 240, 299, 2), QColor(QStringLiteral("#3A7BFC")));
    }

    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.drawLine(QPointF(579, cardTop + 255), QPointF(579, cardTop + 277));

    const bool fileTransferHovered = !dimDetailContent && hoveredBottomAction == BottomAction::FileTransfer;
    const bool moreHovered = hoveredBottomAction == BottomAction::More;
    const QColor normalActionText(QStringLiteral("#040B18"));
    const QColor hoveredActionText(QStringLiteral("#006BFF"));

    painter.save();
    painter.setOpacity(dimDetailContent ? 0.35 : 1.0);
    drawUiIcon(painter, QRect(392, qRound(cardTop + 255), 20, 20), QStringLiteral("file_transfer.svg"));
    painter.restore();
    painter.setFont(textFont);
    painter.setPen(dimDetailContent
            ? QColor(QStringLiteral("#A8B0BC"))
            : (fileTransferHovered ? hoveredActionText : normalActionText));
    painter.drawText(QRectF(420, cardTop + 257, 64, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE6\x96\x87\xE4\xBB\xB6\xE4\xBC\xA0\xE8\xBE\x93"));
    drawUiIcon(painter, QRect(704, qRound(cardTop + 257), 20, 20), QStringLiteral("more.svg"));
    painter.setPen(moreHovered ? hoveredActionText : normalActionText);
    painter.drawText(QRectF(733, cardTop + 257, 30, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE6\x9B\xB4\xE5\xA4\x9A"));

    painter.restore();
}

void drawAddDevicePage(QPainter& painter, const QFont& textFont)
{
    QFont titleFont(QStringLiteral("Microsoft YaHei UI"));
    titleFont.setPixelSize(24);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(
        QRectF(280, 75, 220, 32),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE6\x96\xB0\xE5\xA2\x9E\xE8\xAE\xBE\xE5\xA4\x87"));

    const QRectF formCard(280, 124, 600, 278);
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(formCard, 4, 4);

    QFont sectionFont(QStringLiteral("Microsoft YaHei UI"));
    sectionFont.setPixelSize(14);
    sectionFont.setBold(true);
    painter.setFont(sectionFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(
        QRectF(304, 144, 120, 20),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE8\xAE\xBE\xE5\xA4\x87\xE4\xBF\xA1\xE6\x81\xAF"));

    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(
        QRectF(304, 168, 360, 18),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE6\xB7\xBB\xE5\x8A\xA0\xE5\x90\x8E\xE5\x8F\xAF\xE4\xBB\xA5\xE5\x9C\xA8\xE5\xB7\xA6\xE4\xBE\xA7\xE8\xAE\xBE\xE5\xA4\x87\xE5\x88\x97\xE8\xA1\xA8\xE4\xB8\xAD\xE6\x89\xBE\xE5\x88\xB0\xE5\xAE\x83"));

    painter.setPen(QPen(QColor(QStringLiteral("#EEF1F5")), 1));
    painter.drawLine(280, 202, 880, 202);

    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(
        QRectF(304, 226, 120, 18),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE7\x9B\xAE\xE6\xA0\x87\xE6\x9C\xBA\xE5\x99\xA8IP"));
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(QRectF(304, 250, 252, 34), 4, 4);
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#A0A8B3")));
    painter.drawText(
        QRectF(318, 250, 210, 34),
        Qt::AlignVCenter | Qt::AlignLeft,
        QStringLiteral("192.168.1.100"));

    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(
        QRectF(584, 226, 120, 18),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE8\xAE\xBE\xE5\xA4\x87\xE5\x90\x8D\xE7\xA7\xB0"));
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(QRectF(584, 250, 252, 34), 4, 4);
    painter.setPen(QColor(QStringLiteral("#A0A8B3")));
    painter.drawText(
        QRectF(598, 250, 210, 34),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE4\xBE\x8B\xE5\xA6\x82\xEF\xBC\x9A\xE5\x8A\x9E\xE5\x85\xAC\xE5\xAE\xA4\xE4\xB8\xBB\xE6\x9C\xBA"));

    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(
        QRectF(304, 308, 120, 18),
        Qt::AlignVCenter | Qt::AlignLeft,
        QStringLiteral("MAC"));
    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(
        QRectF(584, 308, 120, 18),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE5\xA4\x87\xE6\xB3\xA8"));
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(QRectF(304, 332, 252, 34), 4, 4);
    painter.drawRoundedRect(QRectF(584, 332, 252, 34), 4, 4);
    painter.setPen(QColor(QStringLiteral("#A0A8B3")));
    painter.drawText(
        QRectF(318, 332, 210, 34),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE5\x8F\xAF\xE9\x80\x89"));
    painter.drawText(
        QRectF(598, 332, 210, 34),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE5\x8F\xAF\xE9\x80\x89"));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
    painter.drawRoundedRect(QRectF(712, 424, 124, 34), 4, 4);
    painter.setFont(textFont);
    painter.setPen(Qt::white);
    painter.drawText(QRectF(712, 424, 124, 34), Qt::AlignCenter, zh("\xE4\xBF\x9D\xE5\xAD\x98"));

    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(QRectF(572, 424, 124, 34), 4, 4);
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(572, 424, 124, 34), Qt::AlignCenter, zh("\xE5\x8F\x96\xE6\xB6\x88"));
}

void drawLocalDeviceInfoPage(QPainter& painter, const QFont& textFont, const platform::DeviceInfo& info)
{
    QFont titleFont(QStringLiteral("Microsoft YaHei UI"));
    titleFont.setPixelSize(24);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(
        QRectF(280, 75, 220, 32),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE6\x9C\xAC\xE6\x9C\xBA\xE4\xBF\xA1\xE6\x81\xAF"));

    const QRectF infoCard(280, 124, 600, 360);
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(infoCard, 4, 4);

    QFont sectionFont(QStringLiteral("Microsoft YaHei UI"));
    sectionFont.setPixelSize(14);
    sectionFont.setBold(true);
    painter.setFont(sectionFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(
        QRectF(304, 144, 120, 20),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE5\xBD\x93\xE5\x89\x8D\xE8\xAE\xBE\xE5\xA4\x87"));

    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(
        QRectF(304, 168, 420, 18),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE8\x87\xAA\xE5\x8A\xA8\xE8\xAF\xBB\xE5\x8F\x96\xE5\xBD\x93\xE5\x89\x8D\xE5\xB1\x80\xE5\x9F\x9F\xE7\xBD\x91\xE7\xBD\x91\xE5\x8D\xA1\xE4\xBF\xA1\xE6\x81\xAF"));

    painter.setPen(QPen(QColor(QStringLiteral("#EEF1F5")), 1));
    painter.drawLine(280, 202, 880, 202);

    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(QRectF(304, 226, 120, 18), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE8\xAE\xBE\xE5\xA4\x87\xE5\x90\x8D\xE7\xA7\xB0"));
    painter.drawText(QRectF(584, 226, 120, 18), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("IPv4"));
    painter.drawText(QRectF(304, 308, 120, 18), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("MAC"));
    painter.drawText(QRectF(584, 308, 120, 18), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE5\xB9\xBF\xE6\x92\xAD\xE5\x9C\xB0\xE5\x9D\x80"));
    painter.drawText(QRectF(304, 390, 120, 18), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE5\xAD\x90\xE7\xBD\x91\xE6\x8E\xA9\xE7\xA0\x81"));
    painter.drawText(QRectF(584, 390, 120, 18), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE9\xBB\x98\xE8\xAE\xA4\xE7\xBD\x91\xE5\x85\xB3"));

    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    for (int i = 0; i < 6; ++i) {
        painter.drawRoundedRect(QRectF(localInfoFieldRect(i)), 4, 4);
    }

    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(localInfoFieldRect(0).adjusted(14, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, infoValueText(info.name));
    painter.drawText(localInfoFieldRect(1).adjusted(14, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, infoValueText(info.ip));
    painter.drawText(localInfoFieldRect(2).adjusted(14, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, infoValueText(info.mac));
    painter.drawText(localInfoFieldRect(3).adjusted(14, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, infoValueText(info.broadcastIp));
    painter.drawText(localInfoFieldRect(4).adjusted(14, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, infoValueText(info.subnetMask));
    painter.drawText(localInfoFieldRect(5).adjusted(14, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, infoValueText(info.gateway));
}

void drawSettingsSwitch(QPainter& painter, int x, int y, bool checked)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(checked ? Qt::NoPen : QPen(QColor(QStringLiteral("#A9ADB3")), 1));
    painter.setBrush(checked ? QColor(QStringLiteral("#3A7BFC")) : QColor(QStringLiteral("#F4F5F7")));
    painter.drawRoundedRect(QRectF(x, y, 40, 20), 10, 10);
    painter.setPen(Qt::NoPen);
    painter.setBrush(checked ? QColor(QStringLiteral("#FFFFFF")) : QColor(QStringLiteral("#666666")));
    painter.drawEllipse(QRectF(checked ? x + 23 : x + 5, y + 4, 12, 12));
    painter.restore();
}

QRect settingsAutoRunSwitchRect()
{
    return QRect(780, 140, 82, 32);
}

QRect settingsRemoteWakeupSwitchRect()
{
    return QRect(780, 212, 82, 32);
}

QRect settingsPreventSleepSwitchRect()
{
    return QRect(780, 290, 82, 32);
}

QRect settingsAutoRefreshSwitchRect()
{
    return QRect(780, 368, 82, 32);
}

void drawSettingsPage(
    QPainter& painter,
    const QFont& textFont,
    bool autoRunEnabled,
    bool remoteWakeupEnabled,
    bool preventSleepEnabled,
    bool statusAutoRefreshEnabled)
{
    QFont titleFont(QStringLiteral("Microsoft YaHei UI"));
    titleFont.setPixelSize(24);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(270, 17, 80, 34), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE8\xAE\xBE\xE7\xBD\xAE"));

    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(270, 78, 40, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE5\xB8\xB8\xE8\xA7\x84"));
    painter.setPen(QPen(QColor(QStringLiteral("#3A7BFC")), 3, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(277, 103), QPointF(290, 103));
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(330, 78, 40, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE9\x94\xAE\xE7\x9B\x98"));

    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(QRectF(270.5, 120.5, 599, 143), 4, 4);
    painter.drawRoundedRect(QRectF(270.5, 268.5, 599, 71), 4, 4);
    painter.drawRoundedRect(QRectF(270.5, 344.5, 599, 71), 4, 4);
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.drawLine(QPointF(270, 192.5), QPointF(870, 192.5));

    drawResourceIcon(painter, QRect(289, 144, 24, 24), QStringLiteral("settings/auto_run.svg"));
    drawResourceIcon(painter, QRect(289, 290, 24, 24), QStringLiteral("settings/prevent_sleep.svg"));

    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(330, 146, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE5\xBC\x80\xE6\x9C\xBA\xE8\x87\xAA\xE5\x8A\xA8\xE5\x90\xAF\xE5\x8A\xA8"));
    painter.drawText(QRectF(330, 208, 210, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE5\x85\x81\xE8\xAE\xB8\xE9\x80\x9A\xE8\xBF\x87\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA\xE5\x90\xAF\xE5\x8A\xA8"));
    painter.drawText(QRectF(330, 284, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE9\x98\xB2\xE6\xAD\xA2\xE7\x94\xB5\xE8\x84\x91\xE4\xBC\x91\xE7\x9C\xA0"));
    painter.drawText(QRectF(330, 360, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE5\x88\x97\xE8\xA1\xA8\xE8\x87\xAA\xE5\x8A\xA8\xE5\x88\xB7\xE6\x96\xB0"));

    QFont subFont(QStringLiteral("Microsoft YaHei UI"));
    subFont.setPixelSize(12);
    painter.setFont(subFont);
    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(QRectF(330, 229, 300, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE5\x8D\x8F\xE5\x8A\xA9\xE9\x85\x8D\xE7\xBD\xAE\xE6\x9C\xAC\xE8\xAE\xBE\xE5\xA4\x87\xE8\xBF\x9B\xE8\xA1\x8C\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA"));
    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(QRectF(330, 305, 330, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE4\xBC\x91\xE7\x9C\xA0\xE5\xB0\x86\xE5\xAF\xBC\xE8\x87\xB4\xE7\x94\xB5\xE8\x84\x91\xE6\x97\xA0\xE6\xB3\x95\xE8\xBF\x9C\xE7\xA8\x8B\xE6\x8E\xA7\xE5\x88\xB6\xEF\xBC\x88\xE5\xBC\xBA\xE7\x83\x88\xE6\x8E\xA8\xE8\x8D\x90\xE5\xBC\x80\xE5\x90\xAF\xEF\xBC\x89"));
    painter.drawText(QRectF(330, 381, 330, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE6\x8C\x89\xE9\x80\x89\xE5\xAE\x9A\xE6\x97\xB6\xE9\x97\xB4\xE5\x91\xA8\xE6\x9C\x9F\xE6\xA3\x80\xE6\xB5\x8B\xE8\xAE\xBE\xE5\xA4\x87\xE5\x9C\xA8\xE7\xBA\xBF\xE7\x8A\xB6\xE6\x80\x81"));

    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(788, 146, 20, 20), Qt::AlignVCenter | Qt::AlignLeft, autoRunEnabled ? zh("\xE5\xBC\x80") : zh("\xE5\x85\xB3"));
    painter.drawText(QRectF(788, 216, 20, 20), Qt::AlignVCenter | Qt::AlignLeft, remoteWakeupEnabled ? zh("\xE5\xBC\x80") : zh("\xE5\x85\xB3"));
    painter.drawText(QRectF(788, 294, 20, 20), Qt::AlignVCenter | Qt::AlignLeft, preventSleepEnabled ? zh("\xE5\xBC\x80") : zh("\xE5\x85\xB3"));
    painter.drawText(QRectF(788, 372, 20, 20), Qt::AlignVCenter | Qt::AlignLeft, statusAutoRefreshEnabled ? zh("\xE5\xBC\x80") : zh("\xE5\x85\xB3"));
    drawSettingsSwitch(painter, 815, 146, autoRunEnabled);
    drawSettingsSwitch(painter, 815, 218, remoteWakeupEnabled);
    drawSettingsSwitch(painter, 815, 295, preventSleepEnabled);
    drawSettingsSwitch(painter, 815, 370, statusAutoRefreshEnabled);
}

} // namespace

DeviceGrid::DeviceGrid(QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("GameViewerMainWindow"));
    setFixedSize(920, 680);
    setMouseTracking(true);

    m_autoRunEnabled = platform::StartupManager::isEnabled();
    m_remoteWakeupEnabled = platform::AppSettings::remoteWakeupEnabled();
    m_preventSleepEnabled = platform::AppSettings::preventSleepEnabled();
    m_statusAutoRefreshEnabled = platform::AppSettings::statusAutoRefreshEnabled();
    m_statusAutoRefreshIntervalSeconds = platform::AppSettings::statusAutoRefreshIntervalSeconds();
    platform::PowerManager::setPreventSleepEnabled(m_preventSleepEnabled);
    loadDevices();
    m_localDeviceInfo = platform::DeviceInfoService::local();
    if (g_devices.isEmpty()) {
        m_remoteAssistSelected = true;
    } else {
        m_currentDeviceName = deviceDisplayName(g_devices.first());
    }
    m_previousDeviceName = m_currentDeviceName;
    setupAddDeviceControls();
    setupLocalInfoControls();
    setupSettingsControls();
    updateSettingsControls();
    updateLocalInfoControls();

    m_detailAnimationTimer = new QTimer(this);
    m_detailAnimationTimer->setTimerType(Qt::PreciseTimer);
    m_detailAnimationTimer->setInterval(16);
    connect(m_detailAnimationTimer, &QTimer::timeout, this, [this] {
        constexpr qint64 durationMs = 220;
        m_detailAnimationProgress = qMin<qreal>(1.0, m_detailAnimationClock.elapsed() / qreal(durationMs));
        if (m_detailAnimationProgress >= 1.0) {
            m_detailAnimationTimer->stop();
        }
        update();
    });

    m_desktopHoverTimer = new QTimer(this);
    m_desktopHoverTimer->setTimerType(Qt::PreciseTimer);
    m_desktopHoverTimer->setInterval(16);
    connect(m_desktopHoverTimer, &QTimer::timeout, this, [this] {
        constexpr qint64 durationMs = 180;
        const qreal target = m_desktopHovered ? 1.0 : 0.0;
        const qreal progress = qMin<qreal>(1.0, m_desktopHoverClock.elapsed() / qreal(durationMs));
        const qreal eased = easeOutCubic(progress);
        m_desktopHoverProgress = m_desktopHoverStartProgress
            + (target - m_desktopHoverStartProgress) * eased;

        if (progress >= 1.0) {
            m_desktopHoverProgress = target;
            m_desktopHoverTimer->stop();
        }
        update();
    });

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setTimerType(Qt::PreciseTimer);
    m_refreshTimer->setInterval(16);
    connect(m_refreshTimer, &QTimer::timeout, this, [this] {
        if (!m_statusRefreshInProgress) {
            m_refreshRotation = 0.0;
            m_refreshTimer->stop();
            update(refreshRect().adjusted(-2, -2, 2, 2));
            return;
        }

        constexpr qreal degreesPerSecond = 405.0;
        const qreal elapsedSeconds = m_refreshClock.elapsed() / 1000.0;
        m_refreshRotation = std::fmod(degreesPerSecond * elapsedSeconds, 360.0);
        update(refreshRect().adjusted(-2, -2, 2, 2));
    });

    m_statusAutoRefreshTimer = new QTimer(this);
    connect(m_statusAutoRefreshTimer, &QTimer::timeout, this, [this] {
        refreshDeviceStatuses();
    });

    m_wakeVisualTimer = new QTimer(this);
    m_wakeVisualTimer->setTimerType(Qt::PreciseTimer);
    m_wakeVisualTimer->setInterval(16);
    connect(m_wakeVisualTimer, &QTimer::timeout, this, [this] {
        if (m_poweringOnDeviceIps.isEmpty()) {
            m_wakeVisualRotation = 0.0;
            m_lastWakeProbeAtMs = 0;
            m_wakeVisualTimer->stop();
            update();
            return;
        }

        constexpr qreal degreesPerSecond = 180.0;
        const qint64 elapsedMs = m_wakeVisualClock.elapsed();
        const qreal elapsedSeconds = elapsedMs / 1000.0;
        m_wakeVisualRotation = std::fmod(degreesPerSecond * elapsedSeconds, 360.0);

        for (auto it = m_poweringOnDeviceIps.begin(); it != m_poweringOnDeviceIps.end();) {
            const qint64 startedAtMs = m_poweringOnStartedAtMs.value(*it, 0);
            if (startedAtMs > 0 && QDateTime::currentMSecsSinceEpoch() - startedAtMs >= 40000) {
                m_poweringOnStartedAtMs.remove(*it);
                it = m_poweringOnDeviceIps.erase(it);
            } else {
                ++it;
            }
        }

        if (!m_poweringOnDeviceIps.isEmpty()
            && !m_wakeProbeInProgress
            && (m_lastWakeProbeAtMs == 0 || elapsedMs - m_lastWakeProbeAtMs >= 2000)) {
            m_lastWakeProbeAtMs = elapsedMs;
            probePoweringOnDevices();
        }

        update();
    });

    applyStatusAutoRefreshSetting(false);
    refreshDeviceStatuses();
}

void DeviceGrid::setupAddDeviceControls()
{
    const QString inputStyle = QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;"
        "padding-left:12px;font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QLineEdit:focus{border:1px solid #3A7BFC;}");
    const QString buttonStyle = QStringLiteral(
        "QPushButton{border:1px solid #DDE3EA;border-radius:4px;background:#FFFFFF;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QPushButton:hover{background:#F3F7FF;}");
    const QString primaryButtonStyle = QStringLiteral(
        "QPushButton{border:0;border-radius:4px;background:#3A7BFC;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#FFFFFF;}"
        "QPushButton:hover{background:#2F6FEF;}"
        "QPushButton:disabled{background:#C9D0DA;color:#FFFFFF;}");

    m_deviceIpEdit = new QLineEdit(this);
    m_deviceIpEdit->setGeometry(304, 250, 252, 34);
    m_deviceIpEdit->setPlaceholderText(QStringLiteral("192.168.1.100"));
    m_deviceIpEdit->setStyleSheet(inputStyle);

    m_deviceNameEdit = new QLineEdit(this);
    m_deviceNameEdit->setGeometry(584, 250, 252, 34);
    m_deviceNameEdit->setPlaceholderText(zh("\xE4\xBE\x8B\xE5\xA6\x82\xEF\xBC\x9A\xE5\x8A\x9E\xE5\x85\xAC\xE5\xAE\xA4\xE4\xB8\xBB\xE6\x9C\xBA"));
    m_deviceNameEdit->setStyleSheet(inputStyle);

    m_deviceMacEdit = new QLineEdit(this);
    m_deviceMacEdit->setGeometry(304, 332, 252, 34);
    m_deviceMacEdit->setPlaceholderText(zh("\xE5\x8F\xAF\xE9\x80\x89"));
    m_deviceMacEdit->setStyleSheet(inputStyle);

    m_deviceRemarkEdit = new QLineEdit(this);
    m_deviceRemarkEdit->setGeometry(584, 332, 252, 34);
    m_deviceRemarkEdit->setPlaceholderText(zh("\xE5\x8F\xAF\xE9\x80\x89"));
    m_deviceRemarkEdit->setStyleSheet(inputStyle);

    m_cancelDeviceButton = new QPushButton(zh("\xE5\x8F\x96\xE6\xB6\x88"), this);
    m_cancelDeviceButton->setGeometry(572, 424, 124, 34);
    m_cancelDeviceButton->setCursor(Qt::PointingHandCursor);
    m_cancelDeviceButton->setStyleSheet(buttonStyle);

    m_saveDeviceButton = new QPushButton(zh("\xE4\xBF\x9D\xE5\xAD\x98"), this);
    m_saveDeviceButton->setGeometry(712, 424, 124, 34);
    m_saveDeviceButton->setCursor(Qt::PointingHandCursor);
    m_saveDeviceButton->setStyleSheet(primaryButtonStyle);

    connect(m_deviceIpEdit, &QLineEdit::textChanged, this, &DeviceGrid::updateAddDeviceControls);
    connect(m_deviceNameEdit, &QLineEdit::textChanged, this, &DeviceGrid::updateAddDeviceControls);
    connect(m_saveDeviceButton, &QPushButton::clicked, this, &DeviceGrid::saveNewDevice);
    connect(m_cancelDeviceButton, &QPushButton::clicked, this, &DeviceGrid::cancelNewDevice);

    updateAddDeviceControls();
}

void DeviceGrid::setupSettingsControls()
{
    m_statusRefreshIntervalCombo = new QComboBox(this);
    auto* popupView = new QListView(m_statusRefreshIntervalCombo);
    m_statusRefreshIntervalCombo->setGeometry(654, 364, 112, 32);
    m_statusRefreshIntervalCombo->addItem(zh("5 秒"), 5);
    m_statusRefreshIntervalCombo->addItem(zh("10 秒"), 10);
    m_statusRefreshIntervalCombo->addItem(zh("15 秒"), 15);
    m_statusRefreshIntervalCombo->addItem(zh("30 秒"), 30);
    m_statusRefreshIntervalCombo->addItem(zh("60 秒"), 60);
    m_statusRefreshIntervalCombo->setCursor(Qt::PointingHandCursor);
    m_statusRefreshIntervalCombo->setStyleSheet(QStringLiteral(
        "QComboBox{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;padding:0 28px 0 12px;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QComboBox:disabled{background:#F5F7FA;border:1px solid #DDE3EA;color:#687384;}"
        "QComboBox::drop-down{subcontrol-origin:padding;subcontrol-position:top right;width:24px;border:0;}"
        "QComboBox::down-arrow{image:url(:/UUGuest/resource/images/titlebar/chevron_down.svg);width:12px;height:12px;}"));
    popupView->setStyleSheet(QStringLiteral(
        "QListView{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;outline:0;padding:4px 0;}"
        "QListView::item{min-height:30px;padding:0 12px;color:#040B18;background:#FFFFFF;}"
        "QListView::item:selected{background:#F3F7FF;color:#040B18;}"
        "QListView::item:hover{background:#F7FAFF;color:#040B18;}"));
    popupView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    popupView->setTextElideMode(Qt::ElideNone);

    for (int i = 0; i < m_statusRefreshIntervalCombo->count(); ++i) {
        m_statusRefreshIntervalCombo->setItemData(i, QColor(QStringLiteral("#040B18")), Qt::ForegroundRole);
        m_statusRefreshIntervalCombo->setItemData(i, QSize(0, 30), Qt::SizeHintRole);
    }

    const int initialIndex = qMax(0, m_statusRefreshIntervalCombo->findData(m_statusAutoRefreshIntervalSeconds));
    m_statusRefreshIntervalCombo->setCurrentIndex(initialIndex);
    connect(m_statusRefreshIntervalCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0) {
            return;
        }
        m_statusAutoRefreshIntervalSeconds = m_statusRefreshIntervalCombo->itemData(index).toInt();
        platform::AppSettings::setStatusAutoRefreshIntervalSeconds(m_statusAutoRefreshIntervalSeconds);
        applyStatusAutoRefreshSetting(false);
    });

    updateSettingsControls();
}

void DeviceGrid::updateAddDeviceControls()
{
    const bool addPageVisible = m_remoteAssistSelected && !m_localInfoSelected && !m_settingsSelected;
    m_deviceIpEdit->setVisible(addPageVisible);
    m_deviceNameEdit->setVisible(addPageVisible);
    m_deviceMacEdit->setVisible(addPageVisible);
    m_deviceRemarkEdit->setVisible(addPageVisible);
    m_saveDeviceButton->setVisible(addPageVisible);
    m_cancelDeviceButton->setVisible(addPageVisible);

    m_saveDeviceButton->setEnabled(!m_deviceIpEdit->text().trimmed().isEmpty());
}

void DeviceGrid::setupLocalInfoControls()
{
    const QString buttonStyle = QStringLiteral(
        "QPushButton{border:1px solid #DDE3EA;border-radius:4px;background:#FFFFFF;"
        "font-family:'Microsoft YaHei UI';font-size:12px;color:#040B18;}"
        "QPushButton:hover{background:#F3F7FF;}");

    for (int i = 0; i < 6; ++i) {
        auto* button = new QPushButton(zh("\xE5\xA4\x8D\xE5\x88\xB6"), this);
        button->setGeometry(localInfoCopyButtonRect(i));
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(buttonStyle);
        connect(button, &QPushButton::clicked, this, [this, i] {
            QString value;
            switch (i) {
            case 0: value = m_localDeviceInfo.name.trimmed(); break;
            case 1: value = m_localDeviceInfo.ip.trimmed(); break;
            case 2: value = m_localDeviceInfo.mac.trimmed(); break;
            case 3: value = m_localDeviceInfo.broadcastIp.trimmed(); break;
            case 4: value = m_localDeviceInfo.subnetMask.trimmed(); break;
            case 5: value = m_localDeviceInfo.gateway.trimmed(); break;
            default: break;
            }

            if (!value.isEmpty()) {
                QApplication::clipboard()->setText(value);
            }
        });
        m_localInfoCopyButtons.append(button);
    }
}

void DeviceGrid::updateLocalInfoControls()
{
    const bool visible = m_localInfoSelected;
    for (QPushButton* button : m_localInfoCopyButtons) {
        if (button) {
            button->setVisible(visible);
        }
    }
}

void DeviceGrid::refreshLocalDeviceInfo()
{
    m_localDeviceInfo = platform::DeviceInfoService::local();
}

void DeviceGrid::updateSettingsControls()
{
    if (!m_statusRefreshIntervalCombo) {
        return;
    }
    const bool visible = m_settingsSelected;
    m_statusRefreshIntervalCombo->setVisible(visible);
    m_statusRefreshIntervalCombo->setEnabled(m_statusAutoRefreshEnabled);
}

void DeviceGrid::applyStatusAutoRefreshSetting(bool refreshImmediately)
{
    if (!m_statusAutoRefreshTimer) {
        return;
    }

    if (m_statusAutoRefreshEnabled) {
        const int intervalMs = qMax(1, m_statusAutoRefreshIntervalSeconds) * 1000;
        m_statusAutoRefreshTimer->start(intervalMs);
        if (refreshImmediately) {
            refreshDeviceStatuses();
        }
    } else {
        m_statusAutoRefreshTimer->stop();
    }

    updateSettingsControls();
}

void DeviceGrid::saveNewDevice()
{
    const QString ip = m_deviceIpEdit->text().trimmed();
    const QString name = m_deviceNameEdit->text().trimmed().isEmpty()
        ? ip
        : m_deviceNameEdit->text().trimmed();
    const QString mac = m_deviceMacEdit->text().trimmed();
    if (ip.isEmpty()) {
        updateAddDeviceControls();
        return;
    }

    g_devices.append({name, ip, mac, {}, m_deviceRemarkEdit->text().trimmed()});
    saveDevices();
    m_deviceStatuses.remove(ip);
    m_deviceIpEdit->clear();
    m_deviceNameEdit->clear();
    m_deviceMacEdit->clear();
    m_deviceRemarkEdit->clear();

    m_remoteAssistSelected = false;
    m_localInfoSelected = false;
    m_deviceGroupExpanded = true;
    setDesktopHoverActive(false);
    clearBottomActionHover();
    startDeviceSwitchAnimation(g_devices.size() - 1, name);
    updateAddDeviceControls();
    updateLocalInfoControls();
    refreshDeviceStatuses();
}

void DeviceGrid::cancelNewDevice()
{
    m_deviceIpEdit->clear();
    m_deviceNameEdit->clear();
    m_deviceMacEdit->clear();
    m_deviceRemarkEdit->clear();
    m_remoteAssistSelected = false;
    m_localInfoSelected = false;
    updateAddDeviceControls();
    updateLocalInfoControls();
    update();
}

platform::DevicePresenceState DeviceGrid::devicePresenceForIndex(int index) const
{
    if (index < 0 || index >= g_devices.size()) {
        return platform::DevicePresenceState::Unknown;
    }

    const QString ip = g_devices.at(index).ip.trimmed();
    if (ip.isEmpty()) {
        return platform::DevicePresenceState::Offline;
    }
    return m_deviceStatuses.value(ip, platform::DevicePresenceState::Unknown);
}

bool DeviceGrid::devicePoweringOnForIndex(int index) const
{
    if (index < 0 || index >= g_devices.size()) {
        return false;
    }
    return m_poweringOnDeviceIps.contains(g_devices.at(index).ip.trimmed());
}

int DeviceGrid::devicePoweringOnRemainingSecondsForIndex(int index) const
{
    if (!devicePoweringOnForIndex(index)) {
        return 0;
    }

    const QString ip = g_devices.at(index).ip.trimmed();
    const qint64 startedAtMs = m_poweringOnStartedAtMs.value(ip, 0);
    if (startedAtMs <= 0) {
        return 40;
    }

    const qint64 elapsedMs = qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - startedAtMs);
    const int elapsedSeconds = static_cast<int>(elapsedMs / 1000);
    return qMax(0, 40 - elapsedSeconds);
}

void DeviceGrid::refreshDeviceStatuses()
{
    if (m_statusRefreshInProgress) {
        return;
    }

    m_statusRefreshInProgress = true;
    m_refreshClock.restart();
    if (!m_refreshTimer->isActive()) {
        m_refreshTimer->start();
    }
    update(refreshRect().adjusted(-2, -2, 2, 2));

    QStringList ips;
    ips.reserve(g_devices.size());
    for (const DeviceEntry& device : g_devices) {
        const QString ip = device.ip.trimmed();
        if (!ip.isEmpty() && !ips.contains(ip)) {
            ips.append(ip);
        }
    }

    QPointer<DeviceGrid> self(this);
    std::thread([self, ips] {
        QHash<QString, platform::DevicePresenceState> statuses;
        std::mutex resultMutex;
        std::atomic_int nextIndex = 0;
        const int workerCount = std::max(1, std::min<int>(8, ips.size()));
        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (int worker = 0; worker < workerCount; ++worker) {
            workers.emplace_back([&] {
                while (true) {
                    const int index = nextIndex.fetch_add(1);
                    if (index >= ips.size()) {
                        break;
                    }
                    const QString ip = ips.at(index);
                    const platform::DevicePresenceState state = platform::DeviceStatusService::probe(ip);
                    std::lock_guard lock(resultMutex);
                    statuses.insert(ip, state);
                }
            });
        }

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, statuses = std::move(statuses)]() mutable {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            grid->m_deviceStatuses = std::move(statuses);
            for (auto it = grid->m_poweringOnDeviceIps.begin(); it != grid->m_poweringOnDeviceIps.end();) {
                const platform::DevicePresenceState state = grid->m_deviceStatuses.value(*it, platform::DevicePresenceState::Offline);
                if (state != platform::DevicePresenceState::Offline) {
                    grid->m_poweringOnStartedAtMs.remove(*it);
                    it = grid->m_poweringOnDeviceIps.erase(it);
                } else {
                    ++it;
                }
            }
            grid->m_statusRefreshInProgress = false;
            grid->update();
        }, Qt::QueuedConnection);
    }).detach();
}

void DeviceGrid::probePoweringOnDevices()
{
    if (m_wakeProbeInProgress || m_poweringOnDeviceIps.isEmpty()) {
        return;
    }

    QStringList ips;
    ips.reserve(m_poweringOnDeviceIps.size());
    for (const QString& ip : m_poweringOnDeviceIps) {
        const QString trimmed = ip.trimmed();
        if (!trimmed.isEmpty()) {
            ips.append(trimmed);
        }
    }
    if (ips.isEmpty()) {
        return;
    }

    m_wakeProbeInProgress = true;
    QPointer<DeviceGrid> self(this);
    std::thread([self, ips] {
        QHash<QString, platform::DevicePresenceState> statuses;
        statuses.reserve(ips.size());
        for (const QString& ip : ips) {
            statuses.insert(ip, platform::DeviceStatusService::probe(ip));
        }

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, statuses = std::move(statuses)]() mutable {
            if (!self) {
                return;
            }

            DeviceGrid* grid = self.data();
            grid->m_wakeProbeInProgress = false;
            for (auto it = statuses.cbegin(); it != statuses.cend(); ++it) {
                grid->m_deviceStatuses.insert(it.key(), it.value());
            }
            for (auto it = grid->m_poweringOnDeviceIps.begin(); it != grid->m_poweringOnDeviceIps.end();) {
                const platform::DevicePresenceState state = grid->m_deviceStatuses.value(*it, platform::DevicePresenceState::Offline);
                if (state != platform::DevicePresenceState::Offline) {
                    grid->m_poweringOnStartedAtMs.remove(*it);
                    it = grid->m_poweringOnDeviceIps.erase(it);
                } else {
                    ++it;
                }
            }
            if (grid->m_poweringOnDeviceIps.isEmpty()) {
                grid->m_lastWakeProbeAtMs = 0;
            }
            grid->update();
        }, Qt::QueuedConnection);
    }).detach();
}

void DeviceGrid::showDeviceMenu()
{
    QMenu menu(this);
    menu.setAttribute(Qt::WA_TranslucentBackground);
    menu.setWindowFlag(Qt::NoDropShadowWindowHint, true);
    menu.setStyleSheet(QStringLiteral(
        "QMenu{background:#FFFFFF;border:1px solid #E6EAF0;border-radius:6px;padding:8px 0;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QMenu::item{height:34px;padding:0 34px 0 38px;background:transparent;}"
        "QMenu::item:selected{background:#F3F7FF;color:#040B18;}"
        "QMenu::icon{padding-left:12px;}"));

    const bool offlineOnly = devicePresenceForIndex(m_selectedDeviceIndex) == platform::DevicePresenceState::Offline;
    QAction* terminalAction = nullptr;
    QAction* shutdownAction = nullptr;
    QAction* restartAction = nullptr;
    if (!offlineOnly) {
        terminalAction = menu.addAction(menuIcon(QStringLiteral("terminal.svg")), zh("\xE7\xBB\x88\xE7\xAB\xAF"));
        shutdownAction = menu.addAction(menuIcon(QStringLiteral("power.svg")), zh("\xE5\x85\xB3\xE6\x9C\xBA"));
        restartAction = menu.addAction(menuIcon(QStringLiteral("restart.svg")), zh("\xE9\x87\x8D\xE5\x90\xAF"));
    }
    QAction* renameAction = menu.addAction(menuIcon(QStringLiteral("rename.svg")), zh("\xE9\x87\x8D\xE5\x91\xBD\xE5\x90\x8D"));
    QAction* deleteAction = menu.addAction(menuIcon(QStringLiteral("delete.svg")), zh("\xE5\x88\xA0\xE9\x99\xA4\xE8\xAE\xBE\xE5\xA4\x87"));
    if (terminalAction) {
        connect(terminalAction, &QAction::triggered, this, &DeviceGrid::openCurrentDeviceTerminal);
    }
    if (shutdownAction) {
        connect(shutdownAction, &QAction::triggered, this, &DeviceGrid::shutdownCurrentDevice);
    }
    if (restartAction) {
        connect(restartAction, &QAction::triggered, this, &DeviceGrid::restartCurrentDevice);
    }
    connect(renameAction, &QAction::triggered, this, &DeviceGrid::renameCurrentDevice);
    connect(deleteAction, &QAction::triggered, this, &DeviceGrid::deleteCurrentDevice);

    const bool isRemoteControlled = deviceBadgeIndexes().contains(m_selectedDeviceIndex);
    const QRectF moreRect = moreActionRect(isRemoteControlled);
    const QSize menuSize = menu.sizeHint();
    const int menuX = qRound(moreRect.center().x() - menuSize.width() / 2.0);
    const int menuY = qRound(moreRect.bottom() + 6);
    menu.exec(mapToGlobal(QPoint(menuX, menuY)));
}

void DeviceGrid::openCurrentDeviceTerminal()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    const DeviceEntry& device = g_devices.at(m_selectedDeviceIndex);
    const QString loginUser = platform::DeviceStatusService::terminalUser(device.ip);
    if (loginUser.isEmpty()) {
        QMessageBox messageBox(
            QMessageBox::Warning,
            QString(),
            zh("\xE6\x97\xA0\xE6\xB3\x95\xE5\xBB\xBA\xE7\xAB\x8B\xE8\xBF\x9C\xE7\xA8\x8B\xE7\xBB\x88\xE7\xAB\xAF\xE8\xBF\x9E\xE6\x8E\xA5\xE3\x80\x82"),
            QMessageBox::NoButton,
            this);
        messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
        messageBox.exec();
        return;
    }

    QString errorMessage;
    if (platform::PortableOpenSshManager::instance().openTerminal(device.ip, loginUser, &errorMessage)) {
        return;
    }

    QMessageBox messageBox(
        QMessageBox::Warning,
        QString(),
        errorMessage.isEmpty()
            ? zh("\xE6\x97\xA0\xE6\xB3\x95\xE5\xBB\xBA\xE7\xAB\x8B\xE8\xBF\x9C\xE7\xA8\x8B\xE7\xBB\x88\xE7\xAB\xAF\xE8\xBF\x9E\xE6\x8E\xA5\xE3\x80\x82")
            : errorMessage,
        QMessageBox::NoButton,
        this);
    messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
    messageBox.exec();
}

void DeviceGrid::openRemoteDesktopWindow()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    auto* remoteWindow = new RemoteDesktopWindow(
        deviceDisplayName(g_devices.at(m_selectedDeviceIndex)),
        g_devices.at(m_selectedDeviceIndex).ip);
    remoteWindow->move(window()->frameGeometry().topLeft() + QPoint(42, 24));
    remoteWindow->show();
}

void DeviceGrid::shutdownCurrentDevice()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    const DeviceEntry& device = g_devices.at(m_selectedDeviceIndex);
    if (platform::DeviceCommandService::send(device.ip, platform::DeviceControlAction::Shutdown)) {
        m_deviceStatuses.insert(device.ip.trimmed(), platform::DevicePresenceState::Offline);
        update();
        return;
    }

    QMessageBox messageBox(
        QMessageBox::Warning,
        QString(),
        zh("\xE6\x97\xA0\xE6\xB3\x95\xE5\x8F\x91\xE9\x80\x81\xE8\xBF\x9C\xE7\xA8\x8B\xE5\x85\xB3\xE6\x9C\xBA\xE5\x91\xBD\xE4\xBB\xA4\xE3\x80\x82"),
        QMessageBox::NoButton,
        this);
    messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
    messageBox.exec();
}

void DeviceGrid::restartCurrentDevice()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    const DeviceEntry& device = g_devices.at(m_selectedDeviceIndex);
    if (platform::DeviceCommandService::send(device.ip, platform::DeviceControlAction::Restart)) {
        m_deviceStatuses.insert(device.ip.trimmed(), platform::DevicePresenceState::Offline);
        update();
        return;
    }

    QMessageBox messageBox(
        QMessageBox::Warning,
        QString(),
        zh("\xE6\x97\xA0\xE6\xB3\x95\xE5\x8F\x91\xE9\x80\x81\xE8\xBF\x9C\xE7\xA8\x8B\xE9\x87\x8D\xE5\x90\xAF\xE5\x91\xBD\xE4\xBB\xA4\xE3\x80\x82"),
        QMessageBox::NoButton,
        this);
    messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
    messageBox.exec();
}

void DeviceGrid::renameCurrentDevice()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    RenameDeviceDialog dialog(g_devices.at(m_selectedDeviceIndex).name, this);
    const QPoint topLeft = window()->frameGeometry().center() - QPoint(dialog.width() / 2, dialog.height() / 2);
    dialog.move(topLeft);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString newName = dialog.name();
    if (newName.isEmpty()) {
        return;
    }

    g_devices[m_selectedDeviceIndex].name = newName;
    saveDevices();
    m_currentDeviceName = newName;
    m_previousDeviceName = newName;
    update();
}

void DeviceGrid::deleteCurrentDevice()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    const QString removedIp = g_devices.at(m_selectedDeviceIndex).ip.trimmed();
    g_devices.removeAt(m_selectedDeviceIndex);
    saveDevices();
    m_deviceStatuses.remove(removedIp);
    m_poweringOnDeviceIps.remove(removedIp);
    m_poweringOnStartedAtMs.remove(removedIp);
        if (g_devices.isEmpty()) {
            m_selectedDeviceIndex = 0;
            m_previousDeviceIndex = 0;
            m_currentDeviceName.clear();
            m_previousDeviceName.clear();
            m_remoteAssistSelected = true;
            updateAddDeviceControls();
            updateLocalInfoControls();
            updateSettingsControls();
            update();
            return;
        }

    const int nextIndex = qMin(m_selectedDeviceIndex, g_devices.size() - 1);
    m_selectedDeviceIndex = nextIndex;
    m_previousDeviceIndex = nextIndex;
    m_currentDeviceName = deviceDisplayName(g_devices.at(nextIndex));
    m_previousDeviceName = m_currentDeviceName;
    setDesktopHoverActive(false);
    clearBottomActionHover();
    update();
}

void DeviceGrid::startDeviceWakeVisual(const QString& ip)
{
    const QString trimmedIp = ip.trimmed();
    if (trimmedIp.isEmpty()) {
        return;
    }

    m_poweringOnDeviceIps.insert(trimmedIp);
    m_poweringOnStartedAtMs.insert(trimmedIp, QDateTime::currentMSecsSinceEpoch());
    m_wakeVisualClock.restart();
    m_lastWakeProbeAtMs = 0;
    if (!m_wakeVisualTimer->isActive()) {
        m_wakeVisualTimer->start();
    }
    update();
}

void DeviceGrid::startCurrentDeviceWakeVisual()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    const QString ip = g_devices.at(m_selectedDeviceIndex).ip.trimmed();
    if (ip.isEmpty() || devicePresenceForIndex(m_selectedDeviceIndex) != platform::DevicePresenceState::Offline) {
        return;
    }

    startDeviceWakeVisual(ip);
}

void DeviceGrid::wakeCurrentDevice()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    const DeviceEntry& device = g_devices.at(m_selectedDeviceIndex);
    if (devicePresenceForIndex(m_selectedDeviceIndex) != platform::DevicePresenceState::Offline) {
        return;
    }

    const QString targetIp = device.ip.trimmed();
    const QString mac = device.mac.trimmed();
    if (mac.isEmpty()) {
        QMessageBox messageBox(
            QMessageBox::Warning,
            QString(),
            zh("\xE8\xAF\xB7\xE5\x85\x88\xE4\xB8\xBA\xE8\xAF\xA5\xE8\xAE\xBE\xE5\xA4\x87\xE5\xA1\xAB\xE5\x86\x99 MAC \xE5\x9C\xB0\xE5\x9D\x80\xEF\xBC\x8C\xE6\x89\x8D\xE8\x83\xBD\xE8\xBF\x9B\xE8\xA1\x8C\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA\xE3\x80\x82"),
            QMessageBox::NoButton,
            this);
        messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
        messageBox.exec();
        return;
    }

    struct WakeProxyCandidate {
        QString name;
        QString ip;
        platform::DevicePresenceState state = platform::DevicePresenceState::Offline;
    };

    QVector<WakeProxyCandidate> candidates;
    candidates.reserve(g_devices.size());
    for (int i = 0; i < g_devices.size(); ++i) {
        if (i == m_selectedDeviceIndex) {
            continue;
        }

        const DeviceEntry& candidateDevice = g_devices.at(i);
        const QString candidateIp = candidateDevice.ip.trimmed();
        if (candidateIp.isEmpty()) {
            continue;
        }

        const platform::DevicePresenceState state = devicePresenceForIndex(i);
        if (!proxyWakeCapableState(state)) {
            continue;
        }

        candidates.append({deviceDisplayName(candidateDevice), candidateIp, state});
    }

    if (candidates.isEmpty()) {
        QMessageBox messageBox(
            QMessageBox::Warning,
            QString(),
            zh("\xE5\xBD\x93\xE5\x89\x8D\xE8\xAE\xBE\xE5\xA4\x87\xE5\x88\x97\xE8\xA1\xA8\xE4\xB8\xAD\xE6\xB2\xA1\xE6\x9C\x89\xE5\x9C\xA8\xE7\xBA\xBF\xE8\xAE\xBE\xE5\xA4\x87\xEF\xBC\x8C\xE6\x97\xA0\xE6\xB3\x95\xE4\xBB\xA3\xE5\x8F\x91\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA\xE5\x8C\x85\xE3\x80\x82"),
            QMessageBox::NoButton,
            this);
        messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
        messageBox.exec();
        return;
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const WakeProxyCandidate& left, const WakeProxyCandidate& right) {
        return std::tie(left.state, left.name) < std::tie(right.state, right.name);
    });

    QPointer<DeviceGrid> self(this);
    std::thread([self, targetIp, mac, candidates = std::move(candidates)] {
        QString proxyDeviceName;
        QString proxyIp;
        for (const WakeProxyCandidate& candidate : candidates) {
            const platform::DeviceStatusInfo info = platform::DeviceStatusService::query(candidate.ip);
            if (!proxyWakeCapableState(info.state)) {
                continue;
            }
            if (!isSameSubnet(targetIp, info.localIp, info.subnetMask)) {
                continue;
            }

            proxyDeviceName = candidate.name;
            proxyIp = candidate.ip;
            break;
        }

        if (!self) {
            return;
        }

        if (proxyIp.isEmpty()) {
            QMetaObject::invokeMethod(self, [self] {
                if (!self) {
                    return;
                }

                QMessageBox messageBox(
                    QMessageBox::Warning,
                    QString(),
                    zh("\xE5\xBD\x93\xE5\x89\x8D\xE8\xAE\xBE\xE5\xA4\x87\xE5\x88\x97\xE8\xA1\xA8\xE4\xB8\xAD\xE6\xB2\xA1\xE6\x9C\x89\xE4\xB8\x8E\xE7\x9B\xAE\xE6\xA0\x87\xE8\xAE\xBE\xE5\xA4\x87\xE5\xA4\x84\xE4\xBA\x8E\xE5\x90\x8C\xE4\xB8\x80\xE5\xB1\x80\xE5\x9F\x9F\xE7\xBD\x91\xE4\xB8\x94\xE5\x9C\xA8\xE7\xBA\xBF\xE7\x9A\x84\xE8\xAE\xBE\xE5\xA4\x87\xEF\xBC\x8C\xE6\x97\xA0\xE6\xB3\x95\xE4\xBB\xA3\xE5\x8F\x91\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA\xE5\x8C\x85\xE3\x80\x82"),
                    QMessageBox::NoButton,
                    self);
                messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
                messageBox.exec();
            }, Qt::QueuedConnection);
            return;
        }

        QString errorMessage;
        const bool sent = platform::DeviceCommandService::sendWakeProxy(proxyIp, mac, &errorMessage);
        QMetaObject::invokeMethod(self, [self, targetIp, sent, proxyDeviceName, errorMessage] {
            if (!self) {
                return;
            }

            if (sent) {
                self->startDeviceWakeVisual(targetIp);
                self->refreshDeviceStatuses();
                return;
            }

            QMessageBox messageBox(
                QMessageBox::Warning,
                QString(),
                errorMessage.trimmed().isEmpty()
                    ? zh("\xE5\x90\x8C\xE7\xBD\x91\xE6\xAE\xB5\xE4\xBB\xA3\xE7\x90\x86\xE8\xAE\xBE\xE5\xA4\x87\xE5\xBC\x80\xE6\x9C\xBA\xE5\x8C\x85\xE4\xBB\xA3\xE5\x8F\x91\xE5\xA4\xB1\xE8\xB4\xA5\xE3\x80\x82")
                    : zh("\xE5\x90\x8C\xE7\xBD\x91\xE6\xAE\xB5\xE8\xAE\xBE\xE5\xA4\x87\xE3\x80\x8C") + proxyDeviceName + zh("\xE3\x80\x8D\xE4\xBB\xA3\xE5\x8F\x91\xE5\xBC\x80\xE6\x9C\xBA\xE5\x8C\x85\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x9A") + errorMessage.trimmed(),
                QMessageBox::NoButton,
                self);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }, Qt::QueuedConnection);
    }).detach();
}

void DeviceGrid::toggleRemoteWakeup()
{
    if (m_remoteWakeupEnabled) {
        m_remoteWakeupEnabled = false;
        platform::AppSettings::setRemoteWakeupEnabled(false);
        update();
        return;
    }

    if (m_wolDetectionInProgress) {
        return;
    }

    m_wolDetectionInProgress = true;
    update();

    QPointer<DeviceGrid> self(this);
    std::thread([self] {
        const platform::WolApplyResult result = platform::WolDetector::enable();
        const bool enabled = result.success;
        const bool permissionDenied = result.permission_denied;

        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, enabled, permissionDenied] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            grid->m_wolDetectionInProgress = false;
            if (enabled) {
                grid->m_remoteWakeupEnabled = true;
                platform::AppSettings::setRemoteWakeupEnabled(true);
                grid->update();
                return;
            }

            grid->m_remoteWakeupEnabled = false;
            platform::AppSettings::setRemoteWakeupEnabled(false);
            grid->update();

            if (permissionDenied) {
                QMessageBox messageBox(QMessageBox::Warning, QString(), zh("\xE5\xBC\x80\xE5\x90\xAF\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x8C\xE9\x9C\x80\xE8\xA6\x81\xE4\xBB\xA5\xE7\xAE\xA1\xE7\x90\x86\xE5\x91\x98\xE6\x9D\x83\xE9\x99\x90\xE8\xBF\x90\xE8\xA1\x8C\xE8\xBD\xAF\xE4\xBB\xB6\xE6\x89\x8D\xE8\x83\xBD\xE4\xBF\xAE\xE6\x94\xB9\xE7\xBD\x91\xE5\x8D\xA1\xE5\x94\xA4\xE9\x86\x92\xE8\xAE\xBE\xE7\xBD\xAE\xE3\x80\x82"), QMessageBox::NoButton, grid);
                messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
                messageBox.exec();
                return;
            }

            QMessageBox messageBox(QMessageBox::Warning, QString(), zh("\xE6\x9C\xAC\xE8\xAE\xBE\xE5\xA4\x87\xE7\x9A\x84\xE7\xBD\x91\xE5\x8D\xA1\xE4\xB8\x8D\xE6\x94\xAF\xE6\x8C\x81\xE7\xBD\x91\xE7\xBB\x9C\xE5\x94\xA4\xE9\x86\x92\xEF\xBC\x8C\xE6\x97\xA0\xE6\xB3\x95\xE9\x80\x9A\xE8\xBF\x87\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA\xE5\x90\xAF\xE5\x8A\xA8\xE3\x80\x82"), QMessageBox::NoButton, grid);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }, Qt::QueuedConnection);
    }).detach();
}

void DeviceGrid::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    painter.fillRect(rect(), QColor(QStringLiteral("#F8FAFC")));
    painter.fillRect(QRectF(0, 0, 920, 48), QColor(QStringLiteral("#EEF3F7")));
    painter.fillRect(QRectF(0, 48, 240, 632), QColor(QStringLiteral("#EEF3F7")));

    painter.setPen(QPen(QColor(QStringLiteral("#BFC7D1")), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(0.5, 0.5, 919, 679), 6, 6);
    painter.setPen(QPen(QColor(QStringLiteral("#D8DEE5")), 1));
    painter.drawLine(240, 48, 240, 680);

    painter.drawPixmap(QRect(18, 15, 116, 18), uupix(QStringLiteral("titlebar/title_wordmark.png")));

    const QRect refreshIconRect(798, 13, 28, 22);
    painter.save();
    painter.translate(refreshIconRect.center());
    painter.rotate(m_refreshRotation);
    painter.translate(-refreshIconRect.center());
    drawUiIcon(painter, refreshIconRect, QStringLiteral("refresh.svg"));
    painter.restore();
    painter.setPen(QPen(QColor(QStringLiteral("#D8DEE5")), 1));
    painter.drawLine(QPointF(835.5, 16), QPointF(835.5, 32));
    drawUiIcon(painter, QRect(848, 13, 24, 24), QStringLiteral("minimize.svg"));
    drawUiIcon(painter, QRect(897, 19, 10, 10), QStringLiteral("close.svg"));

    drawUiIcon(painter, QRect(17, 68, 20, 20), QStringLiteral("device_group.svg"));
    QFont textFont(QStringLiteral("Microsoft YaHei UI"));
    textFont.setPixelSize(14);
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(43, 66, 72, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE6\x88\x91\xE7\x9A\x84\xE8\xAE\xBE\xE5\xA4\x87"));
    drawUiIcon(
        painter,
        QRect(203, 64, 24, 20),
        m_deviceGroupExpanded ? QStringLiteral("chevron_up.svg") : QStringLiteral("chevron_down.svg"));

    const QStringList names = deviceNames();
    const QSet<int> badges = deviceBadgeIndexes();
    const int visibleDeviceBottom = remoteAssistGroupHeaderRect(m_deviceGroupExpanded).y();
    if (m_deviceGroupExpanded) {
        for (int i = 0; i < names.size(); ++i) {
            const int rowY = deviceRowRect(i).y();
            if (rowY + deviceRowRect(i).height() > visibleDeviceBottom) {
                continue;
            }

            if (!m_remoteAssistSelected && !m_localInfoSelected && i == m_selectedDeviceIndex) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(QStringLiteral("#DFE6EC")));
                painter.drawRoundedRect(QRectF(4, rowY, 232, 36), 5, 5);
                painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
                painter.drawRoundedRect(QRectF(4, rowY + 10, 4, 17), 2, 2);
            }

            painter.setPen(Qt::NoPen);
            painter.setBrush(deviceStatusDotColor(devicePresenceForIndex(i)));
            painter.drawEllipse(QRectF(26, rowY + 17, 6, 6));
            drawDeviceTileIcon(painter, 42, rowY + 9, 20);

            painter.setFont(textFont);
            painter.setPen(QColor(QStringLiteral("#111827")));
            painter.drawText(QRectF(74, rowY + 7, 116, 22), Qt::AlignVCenter | Qt::AlignLeft, names.at(i));
            if (badges.contains(i)) {
                drawRemoteBadge(painter, 202, rowY + 12);
            }
        }
    }

    const QRect assistHeader = remoteAssistGroupHeaderRect(m_deviceGroupExpanded);
    drawUiIcon(painter, QRect(17, assistHeader.y() + 10, 20, 20), QStringLiteral("remote_assist_group.svg"));
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(
        QRectF(43, assistHeader.y() + 8, 88, 20),
        Qt::AlignVCenter | Qt::AlignLeft,
        zh("\xE8\xAE\xBE\xE5\xA4\x87\xE7\xAE\xA1\xE7\x90\x86"));
    drawUiIcon(
        painter,
        QRect(203, assistHeader.y() + 6, 24, 20),
        m_remoteAssistExpanded ? QStringLiteral("chevron_up.svg") : QStringLiteral("chevron_down.svg"));

    if (m_remoteAssistExpanded) {
        const QRect assistRow = remoteAssistStartRect(m_deviceGroupExpanded);
        if (m_remoteAssistSelected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#DFE6EC")));
            painter.drawRoundedRect(QRectF(assistRow), 5, 5);
            painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
            painter.drawRoundedRect(QRectF(4, assistRow.y() + 10, 4, 17), 2, 2);
        }

        drawUiIcon(painter, QRect(42, assistRow.y() + 8, 20, 20), QStringLiteral("remote_assist_start.svg"));
        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#111827")));
        painter.drawText(
            QRectF(74, assistRow.y() + 7, 76, 22),
            Qt::AlignVCenter | Qt::AlignLeft,
            zh("\xE6\x96\xB0\xE5\xA2\x9E\xE8\xAE\xBE\xE5\xA4\x87"));

        const QRect localInfoRow = localDeviceInfoRect(m_deviceGroupExpanded);
        if (m_localInfoSelected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#DFE6EC")));
            painter.drawRoundedRect(QRectF(localInfoRow), 5, 5);
            painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
            painter.drawRoundedRect(QRectF(4, localInfoRow.y() + 10, 4, 17), 2, 2);
        }

        drawDeviceTileIcon(painter, 42, localInfoRow.y() + 8, 20);
        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#111827")));
        painter.drawText(
            QRectF(74, localInfoRow.y() + 7, 76, 22),
            Qt::AlignVCenter | Qt::AlignLeft,
            zh("\xE6\x9C\xAC\xE6\x9C\xBA\xE4\xBF\xA1\xE6\x81\xAF"));
    }

    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.drawLine(0, 623, 240, 623);
    if (m_settingsSelected) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#DFE6EC")));
        painter.drawRoundedRect(QRectF(4, 632, 232, 40), 5, 5);
        painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
        painter.drawRoundedRect(QRectF(4, 644, 4, 17), 2, 2);
    }
    drawUiIcon(painter, QRect(16, 642, 20, 20), QStringLiteral("settings.svg"));
    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(43, 641, 40, 22), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE8\xAE\xBE\xE7\xBD\xAE"));

    painter.save();
    painter.setClipRect(QRectF(240, 48, 680, 632));
    if (m_settingsSelected) {
        drawSettingsPage(
            painter,
            textFont,
            m_autoRunEnabled,
            m_remoteWakeupEnabled,
            m_preventSleepEnabled,
            m_statusAutoRefreshEnabled);
    } else if (m_localInfoSelected) {
        drawLocalDeviceInfoPage(painter, textFont, m_localDeviceInfo);
    } else if (m_remoteAssistSelected) {
        drawAddDevicePage(painter, textFont);
    } else if (m_detailAnimationTimer->isActive()) {
        const qreal eased = easeOutCubic(m_detailAnimationProgress);
        drawDeviceDetail(
            painter,
            m_previousDeviceName,
            devicePresenceForIndex(m_previousDeviceIndex),
            devicePoweringOnForIndex(m_previousDeviceIndex),
            devicePoweringOnRemainingSecondsForIndex(m_previousDeviceIndex),
            badges.contains(m_previousDeviceIndex),
            0.0,
            0.0,
            BottomAction::None,
            -24.0 * eased,
            1.0 - eased,
            textFont);
        drawDeviceDetail(
            painter,
            m_currentDeviceName,
            devicePresenceForIndex(m_selectedDeviceIndex),
            devicePoweringOnForIndex(m_selectedDeviceIndex),
            devicePoweringOnRemainingSecondsForIndex(m_selectedDeviceIndex),
            badges.contains(m_selectedDeviceIndex),
            m_desktopHoverProgress,
            m_wakeVisualRotation,
            BottomAction::None,
            32.0 * (1.0 - eased),
            eased,
            textFont);
    } else {
        drawDeviceDetail(
            painter,
            m_currentDeviceName,
            devicePresenceForIndex(m_selectedDeviceIndex),
            devicePoweringOnForIndex(m_selectedDeviceIndex),
            devicePoweringOnRemainingSecondsForIndex(m_selectedDeviceIndex),
            badges.contains(m_selectedDeviceIndex),
            m_desktopHoverProgress,
            m_wakeVisualRotation,
            m_hoveredBottomAction,
            0,
            1.0,
            textFont);
    }
    painter.restore();
}

void DeviceGrid::startDeviceSwitchAnimation(int newIndex, const QString& newName)
{
    if (newIndex == m_selectedDeviceIndex && newName == m_currentDeviceName) {
        update();
        return;
    }

    m_previousDeviceIndex = m_selectedDeviceIndex;
    m_previousDeviceName = m_currentDeviceName;
    m_selectedDeviceIndex = newIndex;
    m_currentDeviceName = newName;
    m_detailAnimationProgress = 0.0;
    m_detailAnimationClock.restart();
    m_detailAnimationTimer->start();
    update();
}

void DeviceGrid::setDesktopHoverActive(bool active)
{
    if (m_desktopHovered == active) {
        return;
    }

    m_desktopHovered = active;
    m_desktopHoverStartProgress = m_desktopHoverProgress;
    m_desktopHoverClock.restart();
    m_desktopHoverTimer->start();
}

void DeviceGrid::updateDesktopHover(const QPoint& position)
{
    if (m_settingsSelected || m_remoteAssistSelected || m_localInfoSelected || m_detailAnimationTimer->isActive()) {
        setDesktopHoverActive(false);
        return;
    }

    if (devicePresenceForIndex(m_selectedDeviceIndex) == platform::DevicePresenceState::Offline) {
        setDesktopHoverActive(false);
        return;
    }

    const bool isRemoteControlled = deviceBadgeIndexes().contains(m_selectedDeviceIndex);
    setDesktopHoverActive(desktopImageRect(isRemoteControlled).contains(position));
}

void DeviceGrid::updateBottomActionHover(const QPoint& position)
{
    BottomAction nextAction = BottomAction::None;
    if (!m_settingsSelected && !m_remoteAssistSelected && !m_localInfoSelected && !m_detailAnimationTimer->isActive()) {
        const bool isRemoteControlled = deviceBadgeIndexes().contains(m_selectedDeviceIndex);
        const bool offlineState = devicePresenceForIndex(m_selectedDeviceIndex) == platform::DevicePresenceState::Offline;
        if (!offlineState && fileTransferActionRect(isRemoteControlled).contains(position)) {
            nextAction = BottomAction::FileTransfer;
        } else if (moreActionRect(isRemoteControlled).contains(position)) {
            nextAction = BottomAction::More;
        }
    }

    if (m_hoveredBottomAction == nextAction) {
        return;
    }

    m_hoveredBottomAction = nextAction;
    update();
}

void DeviceGrid::clearBottomActionHover()
{
    if (m_hoveredBottomAction == BottomAction::None) {
        return;
    }

    m_hoveredBottomAction = BottomAction::None;
    update();
}

void DeviceGrid::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton
        && event->pos().y() >= 0
        && event->pos().y() < 48
        && !refreshRect().contains(event->pos())
        && !minimizeRect().contains(event->pos())
        && !closeRect().contains(event->pos())) {
        m_draggingWindow = true;
        m_dragOffset = event->globalPosition().toPoint() - window()->frameGeometry().topLeft();
        event->accept();
        return;
    }

    QFrame::mousePressEvent(event);
}

void DeviceGrid::mouseMoveEvent(QMouseEvent* event)
{
    if (m_draggingWindow && (event->buttons() & Qt::LeftButton)) {
        window()->move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }

    updateDesktopHover(event->pos());
    updateBottomActionHover(event->pos());
    const bool offlineState = !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && devicePresenceForIndex(m_selectedDeviceIndex) == platform::DevicePresenceState::Offline;
    const bool isRemoteControlled = deviceBadgeIndexes().contains(m_selectedDeviceIndex);
    const bool wakeButtonHovered = offlineState && wakeButtonRect(isRemoteControlled).contains(event->pos());
    const bool settingsSwitchHovered = m_settingsSelected
        && (settingsAutoRunSwitchRect().contains(event->pos())
            || settingsRemoteWakeupSwitchRect().contains(event->pos())
            || settingsPreventSleepSwitchRect().contains(event->pos())
            || settingsAutoRefreshSwitchRect().contains(event->pos()));
    if (m_desktopHovered
        || wakeButtonHovered
        || m_hoveredBottomAction != BottomAction::None
        || settingsSwitchHovered) {
        setCursor(Qt::PointingHandCursor);
    } else {
        unsetCursor();
    }
    QFrame::mouseMoveEvent(event);
}

void DeviceGrid::leaveEvent(QEvent* event)
{
    setDesktopHoverActive(false);
    clearBottomActionHover();
    unsetCursor();
    QFrame::leaveEvent(event);
}

void DeviceGrid::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_draggingWindow = false;

        if (refreshRect().contains(event->pos())) {
            refreshDeviceStatuses();
            event->accept();
            return;
        }

        if (minimizeRect().contains(event->pos())) {
            window()->showMinimized();
            event->accept();
            return;
        }

        if (closeRect().contains(event->pos())) {
            window()->close();
            event->accept();
            return;
        }

        if (deviceGroupHeaderRect().contains(event->pos())) {
            m_deviceGroupExpanded = !m_deviceGroupExpanded;
            update();
            event->accept();
            return;
        }

        if (remoteAssistGroupHeaderRect(m_deviceGroupExpanded).contains(event->pos())) {
            m_remoteAssistExpanded = !m_remoteAssistExpanded;
            update();
            event->accept();
            return;
        }

        if (m_remoteAssistExpanded && remoteAssistStartRect(m_deviceGroupExpanded).contains(event->pos())) {
            m_settingsSelected = false;
            m_remoteAssistSelected = true;
            m_localInfoSelected = false;
            setDesktopHoverActive(false);
            clearBottomActionHover();
            m_detailAnimationTimer->stop();
            updateAddDeviceControls();
            updateLocalInfoControls();
            updateSettingsControls();
            update();
            event->accept();
            return;
        }

        if (m_remoteAssistExpanded && localDeviceInfoRect(m_deviceGroupExpanded).contains(event->pos())) {
            m_settingsSelected = false;
            m_remoteAssistSelected = false;
            m_localInfoSelected = true;
            refreshLocalDeviceInfo();
            setDesktopHoverActive(false);
            clearBottomActionHover();
            m_detailAnimationTimer->stop();
            updateAddDeviceControls();
            updateLocalInfoControls();
            updateSettingsControls();
            update();
            event->accept();
            return;
        }

        if (m_deviceGroupExpanded) {
            const QStringList names = deviceNames();
            const int visibleDeviceBottom = remoteAssistGroupHeaderRect(m_deviceGroupExpanded).y();
            for (int i = 0; i < names.size(); ++i) {
                if (deviceRowRect(i).y() + deviceRowRect(i).height() > visibleDeviceBottom) {
                    continue;
                }
                if (deviceRowRect(i).contains(event->pos())) {
                    m_settingsSelected = false;
                    m_remoteAssistSelected = false;
                    m_localInfoSelected = false;
                    setDesktopHoverActive(false);
                    clearBottomActionHover();
                    updateAddDeviceControls();
                    updateLocalInfoControls();
                    updateSettingsControls();
                    startDeviceSwitchAnimation(i, names.at(i));
                    event->accept();
                    return;
                }
            }
        }

        if (QRect(0, 623, 240, 57).contains(event->pos())) {
            m_settingsSelected = true;
            m_remoteAssistSelected = false;
            m_localInfoSelected = false;
            m_detailAnimationTimer->stop();
            setDesktopHoverActive(false);
            clearBottomActionHover();
            updateAddDeviceControls();
            updateLocalInfoControls();
            updateSettingsControls();
            update();
            event->accept();
            return;
        }

        if (m_settingsSelected) {
            if (settingsAutoRunSwitchRect().contains(event->pos())) {
                platform::StartupManager::setEnabled(!m_autoRunEnabled);
                m_autoRunEnabled = platform::StartupManager::isEnabled();
                update();
                event->accept();
                return;
            }
            if (settingsRemoteWakeupSwitchRect().contains(event->pos())) {
                toggleRemoteWakeup();
                event->accept();
                return;
            }
            if (settingsPreventSleepSwitchRect().contains(event->pos())) {
                m_preventSleepEnabled = !m_preventSleepEnabled;
                platform::AppSettings::setPreventSleepEnabled(m_preventSleepEnabled);
                platform::PowerManager::setPreventSleepEnabled(m_preventSleepEnabled);
                update();
                event->accept();
                return;
            }
            if (settingsAutoRefreshSwitchRect().contains(event->pos())) {
                m_statusAutoRefreshEnabled = !m_statusAutoRefreshEnabled;
                platform::AppSettings::setStatusAutoRefreshEnabled(m_statusAutoRefreshEnabled);
                applyStatusAutoRefreshSetting(m_statusAutoRefreshEnabled);
                update();
                event->accept();
                return;
            }
        }

        if (!m_settingsSelected && !m_remoteAssistSelected && !m_localInfoSelected) {
            const bool isRemoteControlled = deviceBadgeIndexes().contains(m_selectedDeviceIndex);
            const bool offlineState = devicePresenceForIndex(m_selectedDeviceIndex) == platform::DevicePresenceState::Offline;
            if (offlineState && wakeButtonRect(isRemoteControlled).contains(event->pos())) {
                wakeCurrentDevice();
                event->accept();
                return;
            }

            if (!offlineState && desktopImageRect(isRemoteControlled).contains(event->pos())) {
                openRemoteDesktopWindow();
                event->accept();
                return;
            }

            if (moreActionRect(isRemoteControlled).contains(event->pos())) {
                showDeviceMenu();
                event->accept();
                return;
            }
        }
    }

    QFrame::mouseReleaseEvent(event);
}

} // namespace ui
