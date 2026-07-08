#include "ui/DeviceGrid.h"

#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "system/DeviceInfoService.h"
#include "system/DeviceStatusService.h"
#include "system/PowerManager.h"
#include "system/PortableOpenSshManager.h"
#include "system/StartupManager.h"
#include "system/WolDetector.h"
#include "system/WjyDiagnosticLog.h"
#include "ui/RemoteDesktopWindow.h"

#include <QAction>
#include <QAbstractSocket>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHostAddress>
#include <QIcon>
#include <QIntValidator>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QTextEdit>
#include <QTextDocument>
#include <QTextStream>
#include <QVector>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
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

// =====wjy====
void writeDeviceGridStartupLog(const QString& message)
{
    platform::writeWjyDiagnosticLog(message); // wjy: DeviceGrid 和后台线程日志统一走加锁写入，避免多线程日志交叉成半行。
}
// ===end====

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
    QString group; // wjy: 设备所属分组，空字符串表示设备仍在“我的设备”根部，只有拖入具体分组后才写分组名。
};

// =====wjy====
struct DeviceListRow {
    enum class Type {
        Device,
        Group
    };

    Type type = Type::Device;
    int deviceIndex = -1; // wjy: 真实设备下标，只有设备行有效。
    int groupIndex = -1;  // wjy: 真实分组下标，分组行有效；分组内设备也记录所属分组。
};
// ===end====

constexpr int kDeviceGroupReservedBlankHeight = 30; // wjy: 预留“我的设备”列表下方空白，后续可用于右键菜单命中区域。
constexpr int kDeviceListMaxViewportHeight = 420; // wjy: “我的设备”列表最多占用 420 像素，内容更多时在这块区域内滚动，避免压住设备管理。
constexpr const char* kRemoteScriptFolderPath = "\\\\192.168.1.100\\广告部工具\\远程脚本文件"; // wjy: 执行脚本入口当前只浏览这个固定共享目录，真正执行逻辑后续再接。
constexpr const char* kRemoteScriptWorkPath = "%FSREMOTE_DIR%work"; // wjy: 目标设备 FSRemote.exe 所在目录下的 work 文件夹，每次执行前重建。

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

// =====wjy====
QFileInfoList scriptChildDirectories(const QString& folderPath)
{
    QDir dir(folderPath);
    return dir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name | QDir::IgnoreCase); // wjy: 只读取目录并按名称排序，右键级联菜单暂不显示脚本文件本身。
}

void populateScriptFolderMenu(QMenu* menu, const QString& folderPath)
{
    if (!menu) {
        return;
    }

    const QDir dir(folderPath);
    if (!dir.exists()) {
        QAction* unavailableAction = menu->addAction(QString::fromUtf8("无法访问脚本目录"));
        unavailableAction->setEnabled(false); // wjy: 共享路径不可访问时直接在子菜单里显示原因，不弹额外窗口打断右键菜单。
        return;
    }

    const QFileInfoList childDirectories = scriptChildDirectories(folderPath);
    if (childDirectories.isEmpty()) {
        QAction* emptyAction = menu->addAction(QString::fromUtf8("无子文件夹"));
        emptyAction->setEnabled(false); // wjy: 当前目录没有下级文件夹时显示占位，后续执行逻辑再决定叶子节点点击行为。
        return;
    }

    for (const QFileInfo& childInfo : childDirectories) {
        const QString childPath = childInfo.absoluteFilePath();
        const QFileInfoList grandChildren = scriptChildDirectories(childPath);
        if (grandChildren.isEmpty()) {
            QAction* scriptFolderAction = menu->addAction(childInfo.fileName());
            scriptFolderAction->setData(childPath); // wjy: 叶子目录保存真实共享路径，菜单点击后用它复制文件并执行入口脚本。
            continue;
        }

        QMenu* childMenu = menu->addMenu(childInfo.fileName());
        populateScriptFolderMenu(childMenu, childPath); // wjy: 有子目录时创建级联菜单，鼠标悬浮即可继续展开下一层。
    }
}

QString escapedCmdPath(const QString& path)
{
    QString escaped = QDir::toNativeSeparators(path);
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return escaped; // wjy: 放入 cmd 引号中的路径只需处理双引号，空格和中文由外层引号保护。
}

QString escapedPowerShellSingleQuoted(const QString& value)
{
    QString escaped = QDir::toNativeSeparators(value);
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return escaped; // wjy: PowerShell 单引号字符串通过两个单引号转义，中文路径由 EncodedCommand 以 Unicode 传到目标设备。
}

QString powerShellEncodedCommand(const QString& script)
{
    const QByteArray utf16LittleEndian(
        reinterpret_cast<const char*>(script.utf16()),
        script.size() * int(sizeof(ushort)));
    return QString::fromLatin1(utf16LittleEndian.toBase64()); // wjy: powershell -EncodedCommand 要求 UTF-16LE 后再 Base64，避免中文 UNC 路径经过 cmd/ssh 时乱码。
}

QString scriptOutputTempFilePath()
{
    const QString fileName = QStringLiteral("fsremote_script_output_%1_%2.log")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch());
    return QDir(QDir::tempPath()).filePath(fileName); // wjy: Each script run owns one local temp output file, decoupling SSH chunks from the painted terminal.
}

bool writeScriptOutputFile(const QString& filePath, const QString& text, QIODevice::OpenMode mode)
{
    if (filePath.trimmed().isEmpty()) {
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | mode)) {
        return false;
    }
    file.write(text.toUtf8());
    return true;
}

QString readScriptOutputFileTail(const QString& filePath, qint64 maxBytes = 256 * 1024)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    if (file.size() > maxBytes) {
        file.seek(file.size() - maxBytes);
        file.readLine(); // wjy: Drop a possibly partial first line when reading a large output tail.
    }
    return QString::fromUtf8(file.readAll());
}

QFileInfo scriptEntryFile(const QString& folderPath)
{
    const QStringList priorityExtensions = {
        QStringLiteral("*.bat"),
        QStringLiteral("*.cmd"),
        QStringLiteral("*.ps1"),
        QStringLiteral("*.py"),
        QStringLiteral("*.exe"),
    }; // wjy: 优先使用批处理/PowerShell 作为入口，其次 Python，最后才直接运行 exe。

    const QDir dir(folderPath);
    for (const QString& pattern : priorityExtensions) {
        const QFileInfoList files = dir.entryInfoList(
            QStringList{pattern},
            QDir::Files,
            QDir::Name | QDir::IgnoreCase);
        if (!files.isEmpty()) {
            return files.first(); // wjy: 同类脚本多个时先取名称排序后的第一个，后续可再扩展为脚本选择菜单。
        }
    }
    return {};
}

QString scriptRunCommandForFile(const QFileInfo& scriptFile)
{
    const QString scriptName = scriptFile.fileName();
    const QString suffix = scriptFile.suffix().toLower();
    if (suffix == QStringLiteral("py")) {
        return QStringLiteral("python \"%1\"").arg(escapedCmdPath(scriptName)); // wjy: Python 脚本用目标设备命令行里的 python 解释器运行。
    }
    if (suffix == QStringLiteral("ps1")) {
        return QStringLiteral("powershell -NoProfile -ExecutionPolicy Bypass -File \"%1\"").arg(escapedCmdPath(scriptName)); // wjy: ps1 用 PowerShell 绕过本次进程策略执行。
    }
    if (suffix == QStringLiteral("bat") || suffix == QStringLiteral("cmd")) {
        return QStringLiteral("call \"%1\"").arg(escapedCmdPath(scriptName)); // wjy: 批处理用 call 执行，避免脚本返回后中断外层命令流。
    }
    if (suffix == QStringLiteral("exe")) {
        return QStringLiteral("\"%1\"").arg(escapedCmdPath(scriptName)); // wjy: exe 入口直接运行。
    }
    return {};
}
// ===end====

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

bool isWildcardSubnetPattern(const QString& text)
{
    const QStringList parts = text.trimmed().split('.');
    if (parts.size() != 4 || parts.at(3).trimmed() != QStringLiteral("*")) {
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        const int value = parts.at(i).trimmed().toInt(&ok);
        if (!ok || value < 0 || value > 255) {
            return false;
        }
    }
    return true;
}

QStringList wildcardSubnetScanIps(const QString& text)
{
    QStringList result;
    const QStringList parts = text.trimmed().split('.');
    if (parts.size() != 4 || parts.at(3).trimmed() != QStringLiteral("*")) {
        return result; // wjy: 非 192.168.3.* 格式时不生成扫描列表。
    }

    const QString prefix = QStringLiteral("%1.%2.%3.")
        .arg(parts.at(0).trimmed())
        .arg(parts.at(1).trimmed())
        .arg(parts.at(2).trimmed()); // wjy: 只遍历最后一段，前三段保持用户输入的网段。
    result.reserve(254);
    for (int host = 1; host <= 254; ++host) {
        result.append(prefix + QString::number(host)); // wjy: 跳过 .0 和 .255，避免扫描网络号和广播地址。
    }
    return result;
}

struct BatchAddResult {
    QString name;
    QString ip;
    QString mac;
    QString broadcastIp;
};

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
QVector<QString> g_deviceGroupNames; // wjy: 保存右键新建的分组名称，会写入 devices.json 的 groups 字段。
QVector<bool> g_deviceGroupExpandedStates; // wjy: 保存每个分组是否展开，会和分组名称一起持久化。

int deviceIndexForIp(const QString& ip)
{
    const QString trimmedIp = ip.trimmed();
    if (trimmedIp.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < g_devices.size(); ++i) {
        if (g_devices.at(i).ip.trimmed() == trimmedIp) {
            return i; // wjy: 按 IP 定位设备记录，批量扫描补齐 MAC 时复用同一份去重依据。
        }
    }
    return -1;
}

bool deviceIpExists(const QString& ip)
{
    return deviceIndexForIp(ip) >= 0; // wjy: 按 IP 去重，避免批量扫描把已有设备再次写入 devices.json。
}

QString deviceStorePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data/devices.json"));
}

void saveDevices()
{
// =====wjy====
    QJsonArray deviceArray; // wjy: devices 数组只保存真实设备，分组单独放到 groups 数组里，避免设备和分组混在一起。
    for (const DeviceEntry& device : g_devices) {
        QJsonObject object;
        object.insert(QStringLiteral("name"), device.name);
        object.insert(QStringLiteral("ip"), device.ip);
        object.insert(QStringLiteral("mac"), device.mac);
        object.insert(QStringLiteral("broadcast_ip"), device.broadcastIp);
        object.insert(QStringLiteral("remark"), device.remark);
        object.insert(QStringLiteral("group"), device.group); // wjy: 持久化设备所属分组；新增设备默认空分组，拖入分组后再写入分组名。
        deviceArray.append(object);
    }

    QJsonArray groupArray; // wjy: groups 数组保存分组名称和展开状态，下一次启动时可以恢复分组行。
    for (int i = 0; i < g_deviceGroupNames.size(); ++i) {
        const QString groupName = g_deviceGroupNames.at(i).trimmed();
        if (groupName.isEmpty()) {
            continue; // wjy: 空分组名不写入文件，避免下次加载出现不可见分组。
        }
        QJsonObject groupObject;
        groupObject.insert(QStringLiteral("name"), groupName);
        groupObject.insert(QStringLiteral("expanded"), i >= g_deviceGroupExpandedStates.size() || g_deviceGroupExpandedStates.at(i));
        groupArray.append(groupObject);
    }

    QJsonObject rootObject; // wjy: 新格式顶层是对象，devices 放设备，groups 放分组。
    rootObject.insert(QStringLiteral("devices"), deviceArray);
    rootObject.insert(QStringLiteral("groups"), groupArray);
// ===end====

    QFileInfo info(deviceStorePath());
    QDir().mkpath(info.absolutePath());

    QFile file(info.filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(rootObject).toJson(QJsonDocument::Indented)); // wjy: 写入新格式，保留设备并持久化分组。
}

void loadDevices()
{
    g_devices.clear();
    g_deviceGroupNames.clear(); // wjy: 重新加载文件前清空内存分组，避免重复追加。
    g_deviceGroupExpandedStates.clear(); // wjy: 重新加载文件前同步清空展开状态。

    QFile file(deviceStorePath());
    if (!file.exists()) {
        g_devices.append({QStringLiteral("72"), QStringLiteral("192.168.3.27"), {}, {}, {}, {}}); // wjy: 默认示例设备不属于任何分组，group 保持空字符串。
        saveDevices();
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray() && !document.isObject()) {
        return;
    }

// =====wjy====
    const QJsonObject rootObject = document.object(); // wjy: 新格式是对象；旧格式是数组时这里会是空对象。
    const QJsonArray deviceArray = document.isArray()
        ? document.array()
        : rootObject.value(QStringLiteral("devices")).toArray(); // wjy: 兼容旧数组格式和新 devices 字段格式。
    const QJsonArray groupArray = document.isObject()
        ? rootObject.value(QStringLiteral("groups")).toArray()
        : QJsonArray(); // wjy: 旧数组格式没有 groups，加载时保持分组为空。
// ===end====

    int deviceJsonIndex = -1;
    for (const QJsonValue& value : deviceArray) {
        ++deviceJsonIndex;
        const QJsonObject object = value.toObject();
        const QString ip = object.value(QStringLiteral("ip")).toString().trimmed();
        if (ip.isEmpty()) {
            qWarning().noquote() << QStringLiteral("[loadDevices] skip empty ip index=%1").arg(deviceJsonIndex);
            continue;
        }
        quint32 ipValue = 0;
        if (!parseIpv4Address(ip, &ipValue)) {
            qWarning().noquote() << QStringLiteral("[loadDevices] skip invalid ip index=%1 ip=%2")
                .arg(deviceJsonIndex)
                .arg(ip);
            continue;
        }
        qWarning().noquote() << QStringLiteral("[loadDevices] duplicate ip=%1 duplicated=%2")
            .arg(ip)
            .arg(g_devices.cend() != std::find_if(g_devices.cbegin(), g_devices.cend(), [&ip](const DeviceEntry& device) {
                return device.ip.trimmed() == ip;
            }));
        QString name = object.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            name = ip;
        }
        const QString mac = object.value(QStringLiteral("mac")).toString().trimmed();
        const QString broadcastIp = object.value(QStringLiteral("broadcast_ip")).toString().trimmed();
        const QString remark = object.value(QStringLiteral("remark")).toString();
        const QString group = object.value(QStringLiteral("group")).toString().trimmed();
        qWarning().noquote() << QStringLiteral("[loadDevices] append index=%1 name=%2 ip=%3 mac=%4 group=%5")
            .arg(deviceJsonIndex)
            .arg(name)
            .arg(ip)
            .arg(mac)
            .arg(group);
        g_devices.append({
            name,
            ip,
            mac,
            broadcastIp,
            remark,
            group // wjy: 读取设备所属分组；旧 devices.json 没有 group 时会得到空字符串。
        });
    }

// =====wjy====
    for (const QJsonValue& value : groupArray) { // wjy: 读取 groups 字段，恢复用户新建的分组行。
        const QJsonObject object = value.toObject();
        const QString groupName = object.value(QStringLiteral("name")).toString().trimmed();
        if (groupName.isEmpty() || g_deviceGroupNames.contains(groupName)) {
            continue; // wjy: 空名字和重复分组不加载，避免界面出现异常行。
        }
        g_deviceGroupNames.append(groupName);
        g_deviceGroupExpandedStates.append(object.value(QStringLiteral("expanded")).toBool(true));
    }
// ===end====
}

QStringList deviceNames();
int visibleDeviceListRowCount(); // wjy: 统计“我的设备”下拉框里的可见行数，包含设备行和新建分组行。
// =====wjy====self1
bool deviceGroupExpandedForIndex(int groupIndex);
QVector<DeviceListRow> visibleDeviceRows();
int rootDeviceRowCount();
QRect visibleDeviceRowRect(int rowIndex);
int visibleDeviceListContentHeight();
int visibleDeviceListViewportHeight(bool deviceGroupExpanded);
int maxDeviceListScrollOffset();
QRect deviceListViewportRect(bool deviceGroupExpanded);
QRect scrolledVisibleDeviceRowRect(int rowIndex, int scrollOffset);
QRect scrolledDeviceGroupReservedBlankRect(int scrollOffset);
// =====end====self1
QRect remoteAssistGroupHeaderRect(bool deviceGroupExpanded)
{
// =====wjy====
    const int deviceRowsHeight = visibleDeviceListViewportHeight(deviceGroupExpanded); // wjy: 设备很多时只按视口高度占位，超出的内容放在“我的设备”内部滚动，避免压住设备管理。
// ===end====
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

// QStringList deviceNames()
// {
//     QStringList names;
//     names.reserve(g_devices.size());
//     for (const DeviceEntry& device : g_devices) {
//         names.append(deviceDisplayName(device));
//     }
//     return names;
// }

// int visibleDeviceListRowCount()
// {
// // =====wjy====
//     return deviceNames().size() + g_deviceGroupNames.size(); // wjy: 当前可见行 = 真实设备行数量 + 新建分组行数量。
// // ===end====
// }

// bool deviceGroupExpandedForIndex(int groupIndex)
// {
// // =====wjy====
//     return groupIndex >= 0
//         && (groupIndex >= g_deviceGroupExpandedStates.size() || g_deviceGroupExpandedStates.at(groupIndex)); // wjy: 分组状态缺失时默认展开，避免状态数组不同步导致看不到展开箭头。
// // ===end====
// }

// int visualRowIndexForGroupIndex(int groupIndex)
// {
// // =====wjy====
//     if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
//         return -1; // wjy: 分组下标无效时返回 -1，调用方据此放弃显示输入框。
//     }
//     return deviceNames().size() + groupIndex; // wjy: 当前阶段分组行固定排在所有设备行后面。
// // ===end====
// }
// =====wjy====self
QStringList deviceNames()
{
    QStringList names;
    names.reserve(g_devices.size());
    for (const DeviceEntry& device : g_devices) {
        names.append(deviceDisplayName(device));
    }
    return names;
}

bool deviceGroupExpandedForIndex(int groupIndex)
{
    // =====wjy====
    return groupIndex >= 0
           && (groupIndex >= g_deviceGroupExpandedStates.size() || g_deviceGroupExpandedStates.at(groupIndex));
    // ===end====
}

// =====wjy====
int deviceGroupIndexByName(const QString& groupName)
{
    const QString normalizedGroupName = groupName.trimmed();
    if (normalizedGroupName.isEmpty()) {
        return -1;
    }

    for (int i = 0; i < g_deviceGroupNames.size(); ++i) {
        if (g_deviceGroupNames.at(i).trimmed() == normalizedGroupName) {
            return i;
        }
    }

    return -1;
}

QVector<DeviceListRow> visibleDeviceRows()
{
    QVector<DeviceListRow> rows;
    rows.reserve(g_devices.size() + g_deviceGroupNames.size());

    // 1. 先显示没有分组的设备。
    // 如果设备 group 指向一个已经不存在的分组，也先显示在根部，避免设备消失。
    for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        if (deviceGroupIndexByName(g_devices.at(deviceIndex).group) < 0) {
            rows.append({DeviceListRow::Type::Device, deviceIndex, -1});
        }
    }

    // 2. 再显示分组行。
    // 如果分组展开，就在分组行下面显示属于它的设备。
    for (int groupIndex = 0; groupIndex < g_deviceGroupNames.size(); ++groupIndex) {
        rows.append({DeviceListRow::Type::Group, -1, groupIndex});

        if (!deviceGroupExpandedForIndex(groupIndex)) {
            continue;
        }

        const QString groupName = g_deviceGroupNames.at(groupIndex).trimmed();
        for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
            if (g_devices.at(deviceIndex).group.trimmed() == groupName) {
                rows.append({DeviceListRow::Type::Device, deviceIndex, groupIndex});
            }
        }
    }

    return rows;
}
// ===end====

int visibleDeviceListRowCount()
{
    // =====wjy====
    return visibleDeviceRows().size();
    // ===end====
}

int rootDeviceRowCount()
{
// =====wjy====
    int count = 0; // wjy: 统计当前显示在“我的设备”根部的设备数量，用来决定空白区应该插在哪。
    for (const DeviceListRow& row : visibleDeviceRows()) {
        if (row.type == DeviceListRow::Type::Device && row.groupIndex < 0) {
            ++count; // wjy: 只有无分组设备才算根部行，分组行和分组内设备不算。
        }
    }
    return count;
// ===end====
}

int visualRowIndexForGroupIndex(int groupIndex)
{
    // =====wjy====
    const QVector<DeviceListRow> rows = visibleDeviceRows();
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const DeviceListRow& row = rows.at(rowIndex);
        if (row.type == DeviceListRow::Type::Group && row.groupIndex == groupIndex) {
            return rowIndex;
        }
    }

    return -1;
    // ===end====
}
// ===end====self

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

QRect visibleDeviceRowRect(int rowIndex)
{
// =====wjy====
    QRect rowRect = deviceRowRect(rowIndex); // wjy: 先按原来的每行 40 像素计算基础位置。
    if (rowIndex >= rootDeviceRowCount()) {
        rowRect.translate(0, kDeviceGroupReservedBlankHeight); // wjy: 从第一个分组行开始整体下移，给无分组设备区域和分组区域之间留空白。
    }
    return rowRect;
// ===end====
}

int visibleDeviceListContentHeight()
{
// =====wjy====
    return visibleDeviceListRowCount() * 40 + kDeviceGroupReservedBlankHeight; // wjy: 列表真实内容高度 = 所有可见行高度 + 根部预留空白高度。
// ===end====
}

int visibleDeviceListViewportHeight(bool deviceGroupExpanded)
{
// =====wjy====
    if (!deviceGroupExpanded) {
        return 0; // wjy: “我的设备”收起时，内部列表视口高度为 0，下面栏目贴着标题显示。
    }

    return qMin(visibleDeviceListContentHeight(), kDeviceListMaxViewportHeight); // wjy: 展开时最多占 420 像素，超出的设备和分组通过滚轮查看。
// ===end====
}

int maxDeviceListScrollOffset()
{
// =====wjy====
    return qMax(0, visibleDeviceListContentHeight() - visibleDeviceListViewportHeight(true)); // wjy: 最大滚动距离 = 真实内容高度 - 可见视口高度，小于 0 时不需要滚动。
// ===end====
}

QRect deviceListViewportRect(bool deviceGroupExpanded)
{
// =====wjy====
    return QRect(0, deviceGroupHeaderRect().bottom() + 1, 236, visibleDeviceListViewportHeight(deviceGroupExpanded)); // wjy: “我的设备”标题下方的可滚动视口，绘制和鼠标命中都限制在这里。
// ===end====
}

QRect scrolledVisibleDeviceRowRect(int rowIndex, int scrollOffset)
{
// =====wjy====
    QRect rowRect = visibleDeviceRowRect(rowIndex); // wjy: 先拿未滚动时的真实行位置。
    rowRect.translate(0, -scrollOffset); // wjy: 再减去滚动偏移，得到当前屏幕上的显示位置。
    return rowRect;
// ===end====
}

//新增下方矩形计算公式
QRect deviceGroupReservedBlankRect()
{
// =====wjy====
    const int blankTop = 96 + rootDeviceRowCount() * 40; // wjy: 空白区固定排在无分组设备后面；没有无分组设备时，就紧贴“我的设备”标题下方。
    return QRect(4, blankTop, 232, kDeviceGroupReservedBlankHeight); // wjy: 这个空白区既能右键新建分组，也能作为拖回“无分组”的落点。
// ===end====
}

QRect scrolledDeviceGroupReservedBlankRect(int scrollOffset)
{
// =====wjy====
    QRect blankRect = deviceGroupReservedBlankRect(); // wjy: 先取得未滚动时的根部空白区。
    blankRect.translate(0, -scrollOffset); // wjy: 列表滚动后，空白区也要跟着内容一起上移或下移。
    return blankRect;
// ===end====
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

// =====wjy====
constexpr qreal kDeviceDetailCardLeft = 280.0; // wjy: 设备详情卡片左侧固定位置，蓝色桌面图和底部按钮都以它为基准对齐。
constexpr qreal kDeviceDetailCardWidth = 600.0; // wjy: 设备详情卡片宽度保持不变，只压缩上方蓝色桌面预览高度。
constexpr qreal kDeviceDesktopImageHeight = 120.0; // wjy: 原来蓝色桌面预览高度是 240，这里缩到 120，让上方蓝色区域明显变小。
constexpr qreal kDeviceBottomActionHeight = 50.0; // wjy: 底部“文件传输/更多”操作区高度保持 50，避免按钮文字和图标被压缩。
constexpr qreal kDeviceDetailCardHeight = kDeviceDesktopImageHeight + kDeviceBottomActionHeight + 1.0; // wjy: 卡片总高度跟随蓝色图高度计算，防止白色底部区域位置错位。

qreal deviceDetailCardTop(bool isRemoteControlled)
{
    return isRemoteControlled ? 189.0 : 124.0; // wjy: 被控提示出现时，卡片整体下移，原有布局逻辑保持不变。
}

qreal deviceBottomActionTop(bool isRemoteControlled)
{
    return deviceDetailCardTop(isRemoteControlled) + kDeviceDesktopImageHeight; // wjy: 底部操作区紧贴缩小后的蓝色桌面图下边缘。
}

QRectF desktopImageRect(bool isRemoteControlled)
{
    return QRectF(kDeviceDetailCardLeft, deviceDetailCardTop(isRemoteControlled), kDeviceDetailCardWidth, kDeviceDesktopImageHeight); // wjy: 蓝色桌面图的点击热区和绘制高度同步缩小。
}

QRectF fileTransferActionRect(bool isRemoteControlled)
{
    return QRectF(281, deviceBottomActionTop(isRemoteControlled), 298, kDeviceBottomActionHeight); // wjy: 文件传输按钮跟随新的底部操作区位置。
}

QRectF moreActionRect(bool isRemoteControlled)
{
    return QRectF(580, deviceBottomActionTop(isRemoteControlled), 299, kDeviceBottomActionHeight); // wjy: 更多按钮跟随新的底部操作区位置。
}
// ===end====

QRectF wakeButtonRect(bool isRemoteControlled)
{
    const QRectF imageRect = desktopImageRect(isRemoteControlled);
    return QRectF(
        imageRect.center().x() - 36.0,
        imageRect.center().y() - 36.0,
        72.0,
        72.0);
}

QRectF scriptTerminalPanelRect()
{
    return QRectF(580, 308, 300, 368); // wjy: Place the script output terminal in the right-side blank area marked by the user.
}

QRect scriptFileEditorRect()
{
    return QRect(272, 312, 300, 363); // wjy: Left-side editor area requested by the user, paired with the script terminal panel.
}

QRect scriptFileEditorSaveButtonRect()
{
    const QRect rect = scriptFileEditorRect();
    return QRect(rect.right() - 68, rect.y() + 7, 56, 24);
}

QRect scriptFileEditorTextRect()
{
    return scriptFileEditorRect().adjusted(8, 38, -8, -8);
}

QRectF scriptTerminalActionButtonRect()
{
    const QRectF panel = scriptTerminalPanelRect();
    return QRectF(panel.right() - 86, panel.y() + 8, 58, 22); // wjy: Small run/stop button in the panel header at the position marked by the user.
}

QString stripTerminalControlSequences(const QString& text);

QString base64Utf8(const QString& text)
{
    return QString::fromLatin1(text.toUtf8().toBase64());
}

QString utf8FromBase64(const QString& text)
{
    return QString::fromUtf8(QByteArray::fromBase64(text.trimmed().toLatin1()));
}

QStringList terminalOutputLines(const QString& text)
{
    const QString normalized = text.trimmed().isEmpty()
        ? QString()
        : text.trimmed();
    QStringList compactedLines;
    bool previousLineEmpty = false;
    for (QString line : normalized.split(QLatin1Char('\n'))) {
        while (line.endsWith(QLatin1Char(' ')) || line.endsWith(QLatin1Char('\t'))) {
            line.chop(1);
        }
        const bool lineEmpty = line.trimmed().isEmpty();
        if (lineEmpty && previousLineEmpty) {
            continue;
        }
        compactedLines.append(line);
        previousLineEmpty = lineEmpty;
    }
    return compactedLines; // wjy: Collapse repeated blank lines from terminal repaint/control output while preserving JSON indentation.
}

int terminalVisibleLineCount(const QFontMetrics& metrics, int contentHeight)
{
    return qMax(1, contentHeight / qMax(1, metrics.lineSpacing()));
}

int maxTerminalScrollOffset(const QString& text, const QFontMetrics& metrics, int contentHeight)
{
    const QStringList lines = terminalOutputLines(stripTerminalControlSequences(text));
    return qMax(0, lines.size() - terminalVisibleLineCount(metrics, contentHeight));
}

QString terminalVisibleText(const QString& text, const QFontMetrics& metrics, int contentHeight, int scrollOffset)
{
    QStringList lines = terminalOutputLines(stripTerminalControlSequences(text));
    const int visibleLines = terminalVisibleLineCount(metrics, contentHeight);
    const int maxOffset = qMax(0, lines.size() - visibleLines);
    const int boundedOffset = qBound(0, scrollOffset, maxOffset);
    const int start = qMax(0, lines.size() - visibleLines - boundedOffset);
    if (lines.size() > visibleLines) {
        lines = lines.mid(start, visibleLines);
    }
    return lines.join(QLatin1Char('\n')); // wjy: Draw the selected terminal line window; offset 0 follows the newest output.
}

QString stripTerminalControlSequences(const QString& text)
{
    QString cleaned;
    cleaned.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch.unicode() == 0x1B) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('[')) {
                i += 2;
                while (i < text.size()) {
                    const ushort code = text.at(i).unicode();
                    if (code >= 0x40 && code <= 0x7E) {
                        break;
                    }
                    ++i;
                }
            }
            continue; // wjy: Drop ANSI escape sequences such as ESC[K and ESC[19;1H before painting the fake terminal.
        }
        if (ch == QLatin1Char('\r')) {
            if (i + 1 >= text.size() || text.at(i + 1) != QLatin1Char('\n')) {
                cleaned.append(QLatin1Char('\n'));
            }
            continue;
        }
        if (ch.unicode() < 0x20 && ch != QLatin1Char('\n') && ch != QLatin1Char('\t')) {
            continue;
        }
        cleaned.append(ch);
    }
    return cleaned;
}

void drawScriptTerminalPanel(
    QPainter& painter,
    const QString& title,
    const QString& text,
    bool running,
    bool failed,
    int scrollOffset)
{
    const QRectF panel = scriptTerminalPanelRect();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#263241")), 1));
    painter.setBrush(QColor(QStringLiteral("#0B1018")));
    painter.drawRoundedRect(panel, 4, 4);

    const QRectF header(panel.x(), panel.y(), panel.width(), 34);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#111827")));
    painter.drawRoundedRect(header, 4, 4);
    painter.fillRect(header.adjusted(0, 18, 0, 0), QColor(QStringLiteral("#111827")));

    painter.setBrush(failed
        ? QColor(QStringLiteral("#FF5C5C"))
        : (running ? QColor(QStringLiteral("#F5C542")) : QColor(QStringLiteral("#22C55E"))));
    painter.drawEllipse(QRectF(panel.x() + 12, panel.y() + 13, 8, 8));

    QFont titleFont(QStringLiteral("Microsoft YaHei UI"));
    titleFont.setPixelSize(12);
    painter.setFont(titleFont);
    painter.setPen(QColor(QStringLiteral("#DDE7F3")));
    painter.drawText(
        QRectF(panel.x() + 28, panel.y() + 7, panel.width() - 122, 20),
        Qt::AlignVCenter | Qt::AlignLeft,
        painter.fontMetrics().elidedText(title, Qt::ElideRight, int(panel.width() - 122)));

    const QRectF actionButton = scriptTerminalActionButtonRect();
    painter.setPen(QPen(running ? QColor(QStringLiteral("#F87171")) : QColor(QStringLiteral("#34D399")), 1));
    painter.setBrush(running ? QColor(QStringLiteral("#2A151A")) : QColor(QStringLiteral("#10231D")));
    painter.drawRoundedRect(actionButton, 3, 3);
    painter.setFont(titleFont);
    painter.setPen(running ? QColor(QStringLiteral("#FCA5A5")) : QColor(QStringLiteral("#A7F3D0")));
    painter.drawText(actionButton.adjusted(0, 0, 0, -1), Qt::AlignCenter, running ? QString::fromUtf8("停止") : QString::fromUtf8("执行"));

    const QRectF content = panel.adjusted(12, 46, -18, -12);
    QFont terminalFont(QStringLiteral("Consolas"));
    terminalFont.setPixelSize(12);
    painter.setFont(terminalFont);
    painter.setPen(failed ? QColor(QStringLiteral("#FFB4B4")) : QColor(QStringLiteral("#A7F3D0")));
    const int maxOffset = maxTerminalScrollOffset(text, painter.fontMetrics(), qRound(content.height()));
    painter.setClipRect(content);
    painter.drawText(
        content,
        Qt::AlignLeft | Qt::AlignTop,
        terminalVisibleText(text, painter.fontMetrics(), qRound(content.height()), scrollOffset));
    painter.setClipping(false);

    if (maxOffset > 0) {
        const QRectF track(panel.right() - 10, content.y(), 3, content.height());
        const int visibleLines = terminalVisibleLineCount(painter.fontMetrics(), qRound(content.height()));
        const int totalLines = terminalOutputLines(stripTerminalControlSequences(text)).size();
        const qreal thumbHeight = qMax<qreal>(26.0, track.height() * visibleLines / qMax(visibleLines, totalLines));
        const qreal travel = qMax<qreal>(0.0, track.height() - thumbHeight);
        const qreal normalizedFromTop = maxOffset == 0 ? 1.0 : 1.0 - (qBound(0, scrollOffset, maxOffset) / qreal(maxOffset));
        const QRectF thumb(track.x(), track.y() + travel * normalizedFromTop, track.width(), thumbHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(90, 109, 132, 150));
        painter.drawRoundedRect(track, 1.5, 1.5);
        painter.setBrush(QColor(186, 200, 218, 210));
        painter.drawRoundedRect(thumb, 1.5, 1.5);
    }
    painter.restore();
}

void drawScriptFileEditorPanel(
    QPainter& painter,
    const QString& title,
    bool loading,
    bool hasFile)
{
// =====wjy====
    const QRect panel = scriptFileEditorRect(); // wjy: 编辑器面板占用用户标出的左侧空白区域，真实文本编辑控件覆盖在正文区域。
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(QRectF(panel), 4, 4);

    const QRect header(panel.x(), panel.y(), panel.width(), 34);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#F8FAFC")));
    painter.drawRoundedRect(QRectF(header), 4, 4);
    painter.fillRect(header.adjusted(0, 18, 0, 0), QColor(QStringLiteral("#F8FAFC")));

    painter.setBrush(loading
        ? QColor(QStringLiteral("#F5C542"))
        : (hasFile ? QColor(QStringLiteral("#22C55E")) : QColor(QStringLiteral("#94A3B8"))));
    painter.drawEllipse(QRectF(panel.x() + 12, panel.y() + 13, 8, 8)); // wjy: 状态点沿用右侧终端的视觉语言，表示读取中/已找到/无文件。

    QFont titleFont(QStringLiteral("Microsoft YaHei UI"));
    titleFont.setPixelSize(12);
    painter.setFont(titleFont);
    painter.setPen(QColor(QStringLiteral("#334155")));
    const QString displayTitle = title.trimmed().isEmpty()
        ? QString::fromUtf8("本地文件")
        : title.trimmed(); // wjy: 标题显示当前 work 目录里正在编辑的 json/txt 文件名。
    painter.drawText(
        QRectF(panel.x() + 28, panel.y() + 7, panel.width() - 110, 20),
        Qt::AlignVCenter | Qt::AlignLeft,
        painter.fontMetrics().elidedText(displayTitle, Qt::ElideRight, panel.width() - 110));
    painter.restore();
// ===end====
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

    const QRectF card(kDeviceDetailCardLeft, cardTop, kDeviceDetailCardWidth, kDeviceDetailCardHeight); // wjy: 卡片高度由缩小后的蓝色桌面图高度自动计算。
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawRoundedRect(card, 4, 4);

    const QRectF imageRect = desktopImageRect(isRemoteControlled); // wjy: 缓存缩小后的蓝色桌面图区域，后续绘制、遮罩和文字定位共用同一块范围。
    drawRoundedDesktopImage(painter, imageRect, uupix(QStringLiteral("desktop_bk_image.png")), 4);

    if (dimDetailContent) {
        painter.fillRect(imageRect, QColor(255, 255, 255, 112));
    }

    if (!showWakeButton) {
        QFont enterFont(QStringLiteral("Microsoft YaHei UI"));
        enterFont.setPixelSize(16);
        painter.setFont(enterFont);

        const qreal groupShift = 4.0 * desktopHoverProgress;
        const qreal arrowExtend = 5.0 * desktopHoverProgress;
        const qreal centerY = imageRect.center().y(); // wjy: “进入桌面”文字改为跟随缩小后的蓝色图垂直居中。
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

    const qreal actionTop = deviceBottomActionTop(isRemoteControlled); // wjy: 操作区顶部跟随蓝色桌面图的新高度，避免留出旧的 240 像素空白。
    painter.fillRect(QRectF(281, actionTop, 598, kDeviceBottomActionHeight), QColor(QStringLiteral("#FFFFFF")));
    if (dimDetailContent) {
        painter.fillRect(fileTransferActionRect(isRemoteControlled), QColor(QStringLiteral("#F5F7FA")));
    } else if (hoveredBottomAction == BottomAction::FileTransfer) {
        painter.fillRect(fileTransferActionRect(isRemoteControlled), QColor(QStringLiteral("#F3F7FF")));
        painter.fillRect(QRectF(281, actionTop, 298, 2), QColor(QStringLiteral("#3A7BFC")));
    } else if (hoveredBottomAction == BottomAction::More) {
        painter.fillRect(moreActionRect(isRemoteControlled), QColor(QStringLiteral("#F3F7FF")));
        painter.fillRect(QRectF(580, actionTop, 299, 2), QColor(QStringLiteral("#3A7BFC")));
    }

    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.drawLine(QPointF(579, actionTop + 15), QPointF(579, actionTop + 37));

    const bool fileTransferHovered = !dimDetailContent && hoveredBottomAction == BottomAction::FileTransfer;
    const bool moreHovered = hoveredBottomAction == BottomAction::More;
    const QColor normalActionText(QStringLiteral("#040B18"));
    const QColor hoveredActionText(QStringLiteral("#006BFF"));

    painter.save();
    painter.setOpacity(dimDetailContent ? 0.35 : 1.0);
    drawUiIcon(painter, QRect(392, qRound(actionTop + 15), 20, 20), QStringLiteral("file_transfer.svg"));
    painter.restore();
    painter.setFont(textFont);
    painter.setPen(dimDetailContent
            ? QColor(QStringLiteral("#A8B0BC"))
            : (fileTransferHovered ? hoveredActionText : normalActionText));
    painter.drawText(QRectF(420, actionTop + 17, 64, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE6\x96\x87\xE4\xBB\xB6\xE4\xBC\xA0\xE8\xBE\x93"));
    drawUiIcon(painter, QRect(704, qRound(actionTop + 17), 20, 20), QStringLiteral("more.svg"));
    painter.setPen(moreHovered ? hoveredActionText : normalActionText);
    painter.drawText(QRectF(733, actionTop + 17, 30, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE6\x9B\xB4\xE5\xA4\x9A"));

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

QRect settingsBatchSubnetInputRect()
{
    return QRect(596, 448, 140, 32);
}

QRect settingsBatchAddButtonRect()
{
    return QRect(750, 448, 96, 32);
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
    painter.drawRoundedRect(QRectF(270.5, 420.5, 599, 88), 4, 4);
    painter.setPen(QPen(QColor(QStringLiteral("#DDE3EA")), 1));
    painter.drawLine(QPointF(270, 192.5), QPointF(870, 192.5));

    drawResourceIcon(painter, QRect(289, 144, 24, 24), QStringLiteral("settings/auto_run.svg"));
    drawResourceIcon(painter, QRect(289, 290, 24, 24), QStringLiteral("settings/prevent_sleep.svg"));
    drawDeviceTileIcon(painter, 291, 450, 20);

    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#040B18")));
    painter.drawText(QRectF(330, 146, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE5\xBC\x80\xE6\x9C\xBA\xE8\x87\xAA\xE5\x8A\xA8\xE5\x90\xAF\xE5\x8A\xA8"));
    painter.drawText(QRectF(330, 208, 210, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE5\x85\x81\xE8\xAE\xB8\xE9\x80\x9A\xE8\xBF\x87\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA\xE5\x90\xAF\xE5\x8A\xA8"));
    painter.drawText(QRectF(330, 284, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE9\x98\xB2\xE6\xAD\xA2\xE7\x94\xB5\xE8\x84\x91\xE4\xBC\x91\xE7\x9C\xA0"));
    painter.drawText(QRectF(330, 360, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE5\x88\x97\xE8\xA1\xA8\xE8\x87\xAA\xE5\x8A\xA8\xE5\x88\xB7\xE6\x96\xB0"));
    painter.drawText(QRectF(330, 444, 180, 20), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("批量新增设备"));

    QFont subFont(QStringLiteral("Microsoft YaHei UI"));
    subFont.setPixelSize(12);
    painter.setFont(subFont);
    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(QRectF(330, 229, 300, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE5\x8D\x8F\xE5\x8A\xA9\xE9\x85\x8D\xE7\xBD\xAE\xE6\x9C\xAC\xE8\xAE\xBE\xE5\xA4\x87\xE8\xBF\x9B\xE8\xA1\x8C\xE8\xBF\x9C\xE7\xA8\x8B\xE5\xBC\x80\xE6\x9C\xBA"));
    painter.setPen(QColor(QStringLiteral("#687384")));
    painter.drawText(QRectF(330, 305, 330, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE4\xBC\x91\xE7\x9C\xA0\xE5\xB0\x86\xE5\xAF\xBC\xE8\x87\xB4\xE7\x94\xB5\xE8\x84\x91\xE6\x97\xA0\xE6\xB3\x95\xE8\xBF\x9C\xE7\xA8\x8B\xE6\x8E\xA7\xE5\x88\xB6\xEF\xBC\x88\xE5\xBC\xBA\xE7\x83\x88\xE6\x8E\xA8\xE8\x8D\x90\xE5\xBC\x80\xE5\x90\xAF\xEF\xBC\x89"));
    painter.drawText(QRectF(330, 381, 330, 20), Qt::AlignVCenter | Qt::AlignLeft, zh("\xE6\x8C\x89\xE9\x80\x89\xE5\xAE\x9A\xE6\x97\xB6\xE9\x97\xB4\xE5\x91\xA8\xE6\x9C\x9F\xE6\xA3\x80\xE6\xB5\x8B\xE8\xAE\xBE\xE5\xA4\x87\xE5\x9C\xA8\xE7\xBA\xBF\xE7\x8A\xB6\xE6\x80\x81"));
    painter.drawText(QRectF(330, 465, 300, 20), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("输入 192.168.3.* 扫描在线受控端"));

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
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] DeviceGrid ctor begin")); // wjy: 进入 DeviceGrid 构造函数，定位 MainWindow 创建内部崩溃。
    setObjectName(QStringLiteral("DeviceGrid")); // wjy: 恢复正常 QObject 名称，便于样式、调试和对象树识别。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before setFixedSize after setObjectName")); // wjy: 对象名设置完成后继续记录窗口基础属性初始化。
    setFixedSize(920, 680);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after setFixedSize before setMouseTracking")); // wjy: 记录固定尺寸设置完成，继续判断是否崩在鼠标追踪设置。
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus); // wjy: 允许手绘区域被点击后接管焦点，数字输入框点击外部时才能真正失焦并保存。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after basic widget setup")); // wjy: 记录基础 QWidget 属性设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before app settings restore")); // wjy: 恢复读取用户设置前打点，若再次异常可以确认是否来自 QSettings/注册表。
    m_autoRunEnabled = platform::StartupManager::isEnabled(); // wjy: 读取当前开机自启状态，让设置页显示真实开关值。
    if (!m_autoRunEnabled) { //永远开机自启
        platform::StartupManager::setEnabled(true);
        m_autoRunEnabled = true;
    }
    m_remoteWakeupEnabled = platform::AppSettings::remoteWakeupEnabled(); // wjy: 读取远程开机设置，恢复用户上次选择。

    m_preventSleepEnabled = platform::AppSettings::preventSleepEnabled(); // wjy: 读取防睡眠设置，后续同步应用到系统执行状态。
    m_statusAutoRefreshEnabled = platform::AppSettings::statusAutoRefreshEnabled(); // wjy: 读取设备状态自动刷新开关。
    m_statusAutoRefreshIntervalSeconds = platform::AppSettings::statusAutoRefreshIntervalSeconds(); // wjy: 读取自动刷新间隔，非法值由 AppSettings 兜底为 60 秒。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before PowerManager restore")); // wjy: 应用防睡眠前打点，便于区分设置读取和系统 API 调用。
    platform::PowerManager::setPreventSleepEnabled(m_preventSleepEnabled); // wjy: 根据保存的设置恢复防睡眠，保证重启程序后行为一致。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before loadDevices restore")); // wjy: 加载设备文件前打点，验证 devices.json 读写路径是否稳定。
    loadDevices(); // wjy: 恢复读取已保存设备和分组，避免每次启动都丢失设备列表。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] defer DeviceInfoService::local")); // wjy: Release 堆损坏诊断：构造函数里先不读取网卡信息，避免窗口创建阶段触发 GetAdaptersAddresses。
    if (g_devices.isEmpty()) {
        m_remoteAssistSelected = true;

        m_selectedDeviceIndexes.clear();
        m_selectionAnchorDeviceIndex = -1;
    } else {
        m_selectedDeviceIndex = 0;
        m_selectedDeviceIndexes.insert(0);
        m_selectionAnchorDeviceIndex = 0;

        m_currentDeviceName =
            deviceDisplayName(g_devices.first());
    }
    m_previousDeviceName = m_currentDeviceName;
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after current device init")); // wjy: 记录当前设备选择状态初始化完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before setupAddDeviceControls")); // wjy: 记录新增设备输入控件创建前的位置。
    setupAddDeviceControls();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after setupAddDeviceControls")); // wjy: 记录新增设备输入控件创建完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before setupLocalInfoControls")); // wjy: 记录本机信息控件创建前的位置。
    setupLocalInfoControls();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after setupLocalInfoControls")); // wjy: 记录本机信息控件创建完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before setupSettingsControls")); // wjy: 记录设置页控件创建前的位置。
    setupSettingsControls();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after setupSettingsControls")); // wjy: 记录设置页控件创建完成。
    setupScriptFileEditor();
    updateSettingsControls();
    updateLocalInfoControls();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after update controls")); // wjy: 记录控件显隐和内容刷新完成。

// =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before group rename editor create")); // wjy: 记录分组原地重命名输入框创建前的位置。
    m_deviceGroupNameEdit = new QLineEdit(this); // wjy: 创建分组原地重命名输入框，平时隐藏，双击分组行时才显示。
    m_deviceGroupNameEdit->setVisible(false); // wjy: 初始不显示，避免覆盖左侧设备列表。
    m_deviceGroupNameEdit->setStyleSheet(QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #3A7BFC;border-radius:4px;"
        "padding:0 8px;font-family:'Microsoft YaHei UI';font-size:14px;color:#111827;}")); // wjy: 蓝色边框表示当前正在编辑分组名称。
    connect(m_deviceGroupNameEdit, &QLineEdit::returnPressed, this, [this] { // wjy: 用户按回车时保存当前输入。
        finishDeviceGroupRename(true);
    });
    connect(m_deviceGroupNameEdit, &QLineEdit::editingFinished, this, [this] { // wjy: 输入框失焦时也保存，满足点击外部关闭输入框的需求。
        finishDeviceGroupRename(true);
    });
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after group rename editor create")); // wjy: 记录分组原地重命名输入框创建和信号连接完成。
// ===end====

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before detail animation timer")); // wjy: 记录详情动画定时器创建前的位置。
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
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after detail animation timer")); // wjy: 记录详情动画定时器创建完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before desktop hover timer")); // wjy: 记录桌面悬停动画定时器创建前的位置。
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
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after desktop hover timer")); // wjy: 记录桌面悬停动画定时器创建完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before refresh timer")); // wjy: 记录刷新旋转动画定时器创建前的位置。
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
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after refresh timer")); // wjy: 记录刷新旋转动画定时器创建完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before status auto refresh timer")); // wjy: 记录自动刷新定时器创建前的位置。
    m_statusAutoRefreshTimer = new QTimer(this);
    connect(m_statusAutoRefreshTimer, &QTimer::timeout, this, [this] {
        refreshDeviceStatuses();
    });
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after status auto refresh timer")); // wjy: 记录自动刷新定时器创建完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before wake visual timer")); // wjy: 记录远程开机视觉定时器创建前的位置。
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
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after wake visual timer")); // wjy: 记录远程开机视觉定时器创建完成。

    m_scriptOutputFlushTimer = new QTimer(this);
    m_scriptOutputFlushTimer->setInterval(120);
    connect(m_scriptOutputFlushTimer, &QTimer::timeout, this, [this] {
        if (!m_scriptOutputDirty) {
            m_scriptOutputFlushTimer->stop();
            return;
        }
        m_scriptOutputDirty = false;
        m_scriptOutputText = stripTerminalControlSequences(readScriptOutputFileTail(m_scriptOutputFilePath));
        if (m_scriptOutputAutoScroll) {
            m_scriptOutputScrollOffset = 0;
        }
        saveCurrentScriptUiState(); // wjy: 当前设备输出被懒刷新后同步回设备状态缓存，切走再切回仍看到最新滚动位置和文本。
        update(scriptTerminalPanelRect().toAlignedRect().adjusted(-2, -2, 2, 2));
    }); // wjy: Batch script output repaint into a short timer, keeping the UI responsive while still feeling live.

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before applyStatusAutoRefreshSetting")); // wjy: 记录应用自动刷新设置前的位置。
    applyStatusAutoRefreshSetting(false);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after applyStatusAutoRefreshSetting")); // wjy: 记录自动刷新设置应用完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before delayed refreshDeviceStatuses setup")); // wjy: Release 堆损坏诊断：首次状态刷新不再卡在构造函数里立即启动。
    QTimer::singleShot(500, this, [this] { // wjy: 窗口创建后再读取本机 IP/MAC，隔离 DeviceInfoService::local 是否导致 Release 启动阶段堆损坏。
        writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] delayed before DeviceInfoService::local")); // wjy: 延迟读取本机信息前打点。
        refreshLocalDeviceInfo();
        updateLocalInfoControls();
        update();
        writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] delayed after DeviceInfoService::local")); // wjy: 延迟读取本机信息完成。
    });
    QTimer::singleShot(1000, this, [this] { // wjy: 等窗口 show 和首次绘制基本完成后再探测设备状态，隔离启动阶段并发线程是否触发 Release 偶发崩溃。
        writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] delayed before refreshDeviceStatuses")); // wjy: 延迟刷新真正开始前打点，确认崩溃是否发生在状态探测启动前后。
        refreshDeviceStatuses();
        writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] delayed after refreshDeviceStatuses call")); // wjy: 记录首次状态刷新已发起，不代表后台线程已完成。
    });
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after delayed refreshDeviceStatuses setup")); // wjy: 延迟刷新定时器已安排，DeviceGrid 构造可以先结束。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] DeviceGrid ctor end")); // wjy: DeviceGrid 构造函数正常结束。
}

// =====wjy====
DeviceGrid::~DeviceGrid()
{
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] DeviceGrid dtor begin")); // wjy: 记录 DeviceGrid 开始析构，和后台状态刷新线程日志对照判断关闭时序。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] dtor begin statusRefreshInProgress=%1 wakeProbeInProgress=%2")
        .arg(m_statusRefreshInProgress)
        .arg(m_wakeProbeInProgress)); // wjy: 关闭时输出后台刷新标志到日志文件，判断是否存在“界面销毁但线程仍在跑”的情况。
    // =====wjy====
    m_shuttingDown = true; // wjy: 析构开始后禁止再登记新的后台任务，避免关闭过程中继续投递 UI 回调。
    std::vector<std::thread> backgroundThreads; // wjy: 先把线程列表搬到局部变量，避免 join 时长时间持有互斥锁。
    {
        std::lock_guard lock(m_backgroundThreadsMutex); // wjy: 和 runBackgroundTask 的登记操作互斥，保证不会边遍历边修改 vector。
        backgroundThreads.swap(m_backgroundThreads); // wjy: 本次析构负责等待当前已经启动的后台线程。
    }
    for (std::thread& thread : backgroundThreads) {
        if (thread.joinable()) {
            thread.join(); // wjy: 等状态刷新/唤醒检测等线程结束，防止 Qt 对象销毁后仍访问 UI 或 Qt 资源。
        }
    }
    // ===end====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] DeviceGrid dtor end")); // wjy: 线程已经等待完成，DeviceGrid 可以安全继续析构。
}
// ===end====

// =====wjy====
void DeviceGrid::runBackgroundTask(std::function<void()> task)
{
    if (!task) {
        return; // wjy: 空任务没有运行意义，直接忽略。
    }

    std::lock_guard lock(m_backgroundThreadsMutex); // wjy: 后台线程登记和析构取走线程列表必须互斥。
    if (m_shuttingDown) {
        return; // wjy: 关闭阶段不再启动新线程，避免任务晚于 DeviceGrid 生命周期。
    }
    m_backgroundThreads.emplace_back(std::move(task)); // wjy: 不再 detach，析构时统一 join，减少关闭时堆损坏风险。
}
// ===end====

QString DeviceGrid::currentScriptUiDeviceIp() const
{
// =====wjy====
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return QString(); // wjy: 没有当前设备时，脚本 UI 没有归属设备。
    }
    return g_devices.at(m_selectedDeviceIndex).ip.trimmed(); // wjy: 以设备 IP 作为脚本 UI 状态 key，避免同名设备切换时串状态。
// ===end====
}

void DeviceGrid::saveCurrentScriptUiState()
{
// =====wjy====
    const QString deviceIp = currentScriptUiDeviceIp();
    if (deviceIp.isEmpty()) {
        return;
    }

    ScriptUiState state;
    state.outputVisible = m_scriptOutputVisible;
    state.outputRunning = m_scriptOutputRunning;
    state.outputFailed = m_scriptOutputFailed;
    state.outputScrollOffset = m_scriptOutputScrollOffset;
    state.outputAutoScroll = m_scriptOutputAutoScroll;
    state.outputDirty = m_scriptOutputDirty;
    state.outputTitle = m_scriptOutputTitle;
    state.outputText = m_scriptOutputText;
    state.outputFilePath = m_scriptOutputFilePath;
    state.lastScriptFolderPath = m_lastScriptFolderPath;
    state.cancelRequested = m_scriptCancelRequested;
    state.editorVisible = m_scriptEditorVisible;
    state.editorLoading = m_scriptEditorLoading;
    state.editorSaving = m_scriptEditorSaving;
    state.editorTitle = m_scriptEditorTitle;
    state.editorRemotePath = m_scriptEditorRemotePath;
    state.editorDeviceIp = m_scriptEditorDeviceIp;
    state.editorLoginUser = m_scriptEditorLoginUser;
    state.editorWorkName = m_scriptEditorWorkName;
    if (m_scriptFileEdit) {
        state.editorText = m_scriptFileEdit->toPlainText(); // wjy: 切走设备前保留未保存的编辑器文本，切回来时继续显示。
        state.editorModified = m_scriptFileEdit->document()->isModified();
    }
    m_scriptUiStates.insert(deviceIp, state);
// ===end====
}

void DeviceGrid::loadScriptUiStateForDevice(const QString& deviceIp)
{
// =====wjy====
    const QString key = deviceIp.trimmed();
    const ScriptUiState state = m_scriptUiStates.value(key);
    m_scriptOutputVisible = state.outputVisible;
    m_scriptOutputRunning = state.outputRunning;
    m_scriptOutputFailed = state.outputFailed;
    m_scriptOutputScrollOffset = state.outputScrollOffset;
    m_scriptOutputAutoScroll = state.outputAutoScroll;
    m_scriptOutputDirty = state.outputDirty;
    m_scriptOutputTitle = state.outputTitle;
    m_scriptOutputText = state.outputText;
    m_scriptOutputFilePath = state.outputFilePath;
    m_lastScriptFolderPath = state.lastScriptFolderPath;
    m_scriptCancelRequested = state.cancelRequested;
    if (m_scriptOutputVisible && !m_scriptOutputFilePath.trimmed().isEmpty()) {
        m_scriptOutputText = stripTerminalControlSequences(readScriptOutputFileTail(m_scriptOutputFilePath)); // wjy: 切回正在运行或已执行设备时，从临时输出文件补读最新内容。
        m_scriptOutputDirty = false;
        if (m_scriptOutputAutoScroll) {
            m_scriptOutputScrollOffset = 0;
        }
    }

    m_scriptEditorVisible = state.editorVisible;
    m_scriptEditorLoading = state.editorLoading;
    m_scriptEditorSaving = state.editorSaving;
    m_scriptEditorTitle = state.editorTitle;
    m_scriptEditorRemotePath = state.editorRemotePath;
    m_scriptEditorDeviceIp = state.editorDeviceIp.trimmed().isEmpty() ? key : state.editorDeviceIp;
    m_scriptEditorLoginUser = state.editorLoginUser;
    m_scriptEditorWorkName = state.editorWorkName;
    if (m_scriptFileEdit) {
        m_scriptFileEdit->setPlainText(state.editorText);
        m_scriptFileEdit->document()->setModified(state.editorModified);
    }
    updateScriptFileEditorControls();
    update(scriptFileEditorRect().adjusted(-2, -2, 2, 2).united(scriptTerminalPanelRect().toAlignedRect().adjusted(-2, -2, 2, 2)));
// ===end====
}

void DeviceGrid::setupScriptFileEditor()
{
// =====wjy====
    m_scriptFileEdit = new QTextEdit(this);
    m_scriptFileEdit->setGeometry(scriptFileEditorTextRect());
    m_scriptFileEdit->setAcceptRichText(false);
    m_scriptFileEdit->setLineWrapMode(QTextEdit::NoWrap);
    QFont editorFont(QStringLiteral("Consolas"));
    editorFont.setPixelSize(12);
    m_scriptFileEdit->setFont(editorFont);
    m_scriptFileEdit->setStyleSheet(QStringLiteral(
        "QTextEdit{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;"
        "padding:8px;color:#101828;font-family:Consolas;font-size:12px;}"
        "QTextEdit:focus{border:1px solid #3A7BFC;}"));

    m_scriptFileSaveButton = new QPushButton(QString::fromUtf8("保存"), this);
    m_scriptFileSaveButton->setGeometry(scriptFileEditorSaveButtonRect());
    m_scriptFileSaveButton->setCursor(Qt::PointingHandCursor);
    m_scriptFileSaveButton->setStyleSheet(QStringLiteral(
        "QPushButton{border:1px solid #3A7BFC;border-radius:4px;background:#FFFFFF;"
        "font-family:'Microsoft YaHei UI';font-size:12px;color:#006BFF;}"
        "QPushButton:hover{background:#F3F7FF;}"
        "QPushButton:disabled{border-color:#CBD5E1;color:#94A3B8;background:#F8FAFC;}"));
    connect(m_scriptFileSaveButton, &QPushButton::clicked, this, &DeviceGrid::saveScriptFileEditor);
    updateScriptFileEditorControls();
// ===end====
}

void DeviceGrid::updateScriptFileEditorControls()
{
// =====wjy====
    const bool visible =
        m_scriptEditorVisible
        && !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected;
    if (m_scriptFileEdit) {
        m_scriptFileEdit->setGeometry(scriptFileEditorTextRect());
        m_scriptFileEdit->setVisible(visible);
        m_scriptFileEdit->setEnabled(visible && !m_scriptEditorLoading && !m_scriptEditorSaving && !m_scriptEditorRemotePath.isEmpty());
        if (visible) {
            m_scriptFileEdit->raise();
        }
    }
    if (m_scriptFileSaveButton) {
        m_scriptFileSaveButton->setGeometry(scriptFileEditorSaveButtonRect());
        m_scriptFileSaveButton->setVisible(visible);
        m_scriptFileSaveButton->setEnabled(visible && !m_scriptEditorLoading && !m_scriptEditorSaving && !m_scriptEditorRemotePath.isEmpty());
        m_scriptFileSaveButton->setText(m_scriptEditorSaving ? QString::fromUtf8("保存中") : QString::fromUtf8("保存"));
        if (visible) {
            m_scriptFileSaveButton->raise();
        }
    }
// ===end====
}

void DeviceGrid::loadScriptFileEditor(const QString& deviceIp, const QString& loginUser, const QString& scriptWorkName)
{
// =====wjy====
    if (deviceIp.trimmed().isEmpty() || loginUser.trimmed().isEmpty() || scriptWorkName.trimmed().isEmpty()) {
        return;
    }
    const QString targetIp = deviceIp.trimmed();
    const bool targetIsCurrent = currentScriptUiDeviceIp() == targetIp;
    ScriptUiState loadingState = m_scriptUiStates.value(targetIp);
    loadingState.editorVisible = true;
    loadingState.editorLoading = true;
    loadingState.editorSaving = false;
    loadingState.editorTitle = QString::fromUtf8("本地文件");
    loadingState.editorRemotePath.clear();
    loadingState.editorDeviceIp = targetIp;
    loadingState.editorLoginUser = loginUser;
    loadingState.editorWorkName = scriptWorkName;
    loadingState.editorText = QString::fromUtf8("正在读取目标设备本地 json/txt 文件...");
    loadingState.editorModified = false;
    m_scriptUiStates.insert(targetIp, loadingState); // wjy: 开始读取时先更新目标设备状态，切走设备也不会污染当前编辑器。
    if (targetIsCurrent) {
        m_scriptEditorVisible = loadingState.editorVisible;
        m_scriptEditorLoading = loadingState.editorLoading;
        m_scriptEditorSaving = loadingState.editorSaving;
        m_scriptEditorTitle = loadingState.editorTitle;
        m_scriptEditorRemotePath = loadingState.editorRemotePath;
        m_scriptEditorDeviceIp = loadingState.editorDeviceIp;
        m_scriptEditorLoginUser = loadingState.editorLoginUser;
        m_scriptEditorWorkName = loadingState.editorWorkName;
        if (m_scriptFileEdit) {
            m_scriptFileEdit->setPlainText(loadingState.editorText);
            m_scriptFileEdit->document()->setModified(false);
        }
        updateScriptFileEditorControls();
    }

    const QString remotePowerShellScript = QStringLiteral(R"($ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$fsremoteExe = Get-Process FSRemote -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Path
if ([string]::IsNullOrWhiteSpace($fsremoteExe)) {
    Write-Output 'FSREMOTE_EDIT_NOFILE'
    exit 0
}
$fsremoteDir = Split-Path -Parent $fsremoteExe
$work = Join-Path (Join-Path $fsremoteDir 'work') '%1'
if (-not (Test-Path -LiteralPath $work)) {
    Write-Output 'FSREMOTE_EDIT_NOFILE'
    exit 0
}
$file = Get-ChildItem -LiteralPath $work -File | Where-Object { $_.Extension -in '.json', '.txt' } | Sort-Object Name | Select-Object -First 1
if ($null -eq $file) {
    Write-Output 'FSREMOTE_EDIT_NOFILE'
    exit 0
}
$content = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
Write-Output 'FSREMOTE_EDIT_PATH_BEGIN'
Write-Output ([Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($file.FullName)))
Write-Output 'FSREMOTE_EDIT_NAME_BEGIN'
Write-Output ([Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($file.Name)))
Write-Output 'FSREMOTE_EDIT_CONTENT_BEGIN'
Write-Output ([Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($content)))
Write-Output 'FSREMOTE_EDIT_END'
)").arg(escapedPowerShellSingleQuoted(scriptWorkName));
    const QStringList commands = {
        QStringLiteral("powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand %1 & exit %ERRORLEVEL%")
            .arg(powerShellEncodedCommand(remotePowerShellScript)),
    };

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, deviceIp, loginUser, scriptWorkName, commands] {
        QString outputText;
        QString errorMessage;
        const bool ok = platform::PortableOpenSshManager::instance().runRemoteCommands(
            deviceIp,
            loginUser,
            commands,
            &outputText,
            &errorMessage,
            60000);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, ok, deviceIp, loginUser, scriptWorkName, outputText, errorMessage] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            const QString targetIp = deviceIp.trimmed();
            const bool targetIsCurrent = grid->currentScriptUiDeviceIp() == targetIp;
            ScriptUiState state = grid->m_scriptUiStates.value(targetIp);
            const QString activeWorkName = targetIsCurrent ? grid->m_scriptEditorWorkName : state.editorWorkName;
            if (activeWorkName != scriptWorkName) {
                return;
            }
            QString nextRemotePath;
            QString nextTitle;
            QString nextText;
            const QString result = ok ? stripTerminalControlSequences(outputText) : errorMessage.trimmed();
            const QStringList lines = result.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
            const int pathMarker = lines.indexOf(QStringLiteral("FSREMOTE_EDIT_PATH_BEGIN"));
            const int nameMarker = lines.indexOf(QStringLiteral("FSREMOTE_EDIT_NAME_BEGIN"));
            const int contentMarker = lines.indexOf(QStringLiteral("FSREMOTE_EDIT_CONTENT_BEGIN"));
            const int endMarker = lines.indexOf(QStringLiteral("FSREMOTE_EDIT_END"));
            if (ok
                && pathMarker >= 0
                && nameMarker > pathMarker
                && contentMarker > nameMarker
                && endMarker > contentMarker
                && pathMarker + 1 < nameMarker
                && nameMarker + 1 < contentMarker) {
                QStringList pathBase64Parts;
                for (int i = pathMarker + 1; i < nameMarker; ++i) {
                    pathBase64Parts.append(lines.at(i).trimmed()); // wjy: 远端输出可能按终端宽度折行，路径 Base64 也按片段拼回完整字符串。
                }
                QStringList nameBase64Parts;
                for (int i = nameMarker + 1; i < contentMarker; ++i) {
                    nameBase64Parts.append(lines.at(i).trimmed()); // wjy: 文件名单独返回，避免标题从完整路径解析时受特殊字符影响而乱码。
                }
                QStringList contentBase64Parts;
                for (int i = contentMarker + 1; i < endMarker; ++i) {
                    contentBase64Parts.append(lines.at(i).trimmed()); // wjy: JSON/TXT 内容较长时 Base64 会被拆成多行，必须拼接全部片段后再解码。
                }
                nextRemotePath = utf8FromBase64(pathBase64Parts.join(QString()));
                const QString fileName = utf8FromBase64(nameBase64Parts.join(QString()));
                nextText = utf8FromBase64(contentBase64Parts.join(QString()));
                nextTitle = fileName.trimmed().isEmpty()
                    ? QFileInfo(nextRemotePath).fileName()
                    : fileName; // wjy: 标题优先使用远端直接返回的文件名，降低中文路径或控制字符导致的显示乱码概率。
            } else {
                nextText = ok
                    ? QString::fromUtf8("当前本地脚本目录没有 json/txt 文件。")
                    : QString::fromUtf8("读取失败：%1").arg(result);
            }
            state.editorVisible = true;
            state.editorLoading = false;
            state.editorSaving = false;
            state.editorTitle = nextTitle;
            state.editorRemotePath = nextRemotePath;
            state.editorDeviceIp = targetIp;
            state.editorLoginUser = loginUser; // wjy: 读取回调保存目标设备自己的 SSH 登录用户，避免切换设备后借用当前界面的用户。
            state.editorWorkName = scriptWorkName;
            state.editorText = nextText;
            state.editorModified = false;
            grid->m_scriptUiStates.insert(targetIp, state); // wjy: 文件读取结果先归档到目标设备自己的状态，切回来时能恢复对应编辑器。

            if (targetIsCurrent) {
                grid->m_scriptEditorVisible = state.editorVisible;
                grid->m_scriptEditorLoading = state.editorLoading;
                grid->m_scriptEditorSaving = state.editorSaving;
                grid->m_scriptEditorTitle = state.editorTitle;
                grid->m_scriptEditorRemotePath = state.editorRemotePath;
                grid->m_scriptEditorDeviceIp = state.editorDeviceIp;
                grid->m_scriptEditorLoginUser = state.editorLoginUser;
                grid->m_scriptEditorWorkName = state.editorWorkName;
                if (grid->m_scriptFileEdit) {
                    grid->m_scriptFileEdit->setPlainText(state.editorText);
                    grid->m_scriptFileEdit->document()->setModified(false); // wjy: 只刷新当前设备自己的编辑器，避免别的设备后台读取覆盖当前文本。
                }
                grid->updateScriptFileEditorControls();
                grid->update(scriptFileEditorRect().adjusted(-2, -2, 2, 2));
            }
        }, Qt::QueuedConnection);
    });
// ===end====
}

void DeviceGrid::saveScriptFileEditor()
{
// =====wjy====
    if (!m_scriptFileEdit
        || m_scriptEditorRemotePath.trimmed().isEmpty()
        || m_scriptEditorDeviceIp.trimmed().isEmpty()
        || m_scriptEditorLoginUser.trimmed().isEmpty()
        || m_scriptEditorSaving) {
        return;
    }
    m_scriptEditorSaving = true;
    updateScriptFileEditorControls();
    saveCurrentScriptUiState(); // wjy: 保存开始时把按钮状态写回当前设备缓存，切走后再回来仍能看到保存中的状态。
    const QString remotePath = m_scriptEditorRemotePath;
    const QString contentBase64 = base64Utf8(m_scriptFileEdit->toPlainText());
    const QString deviceIp = m_scriptEditorDeviceIp;
    const QString loginUser = m_scriptEditorLoginUser;
    const QString remotePowerShellScript = QStringLiteral(R"($ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$path = '%1'
$content = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%2'))
[System.IO.File]::WriteAllText($path, $content, [System.Text.UTF8Encoding]::new($false))
Write-Output 'FSREMOTE_EDIT_SAVE_OK'
)").arg(
        escapedPowerShellSingleQuoted(remotePath),
        contentBase64);
    const QStringList commands = {
        QStringLiteral("powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand %1 & exit %ERRORLEVEL%")
            .arg(powerShellEncodedCommand(remotePowerShellScript)),
    };

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, deviceIp, loginUser, remotePath, commands] {
        QString outputText;
        QString errorMessage;
        const bool ok = platform::PortableOpenSshManager::instance().runRemoteCommands(
            deviceIp,
            loginUser,
            commands,
            &outputText,
            &errorMessage,
            60000);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, ok, deviceIp, remotePath, outputText, errorMessage] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            const QString targetIp = deviceIp.trimmed();
            const bool targetIsCurrent = grid->currentScriptUiDeviceIp() == targetIp;
            ScriptUiState state = grid->m_scriptUiStates.value(targetIp);
            const QString activeRemotePath = targetIsCurrent ? grid->m_scriptEditorRemotePath : state.editorRemotePath;
            if (activeRemotePath != remotePath) {
                return;
            }
            state.editorSaving = false;
            if (ok) {
                state.editorModified = false; // wjy: 保存成功只清掉目标设备自己的编辑器脏标记，不影响其它设备正在编辑的文本。
            }
            grid->m_scriptUiStates.insert(targetIp, state);
            if (targetIsCurrent) {
                grid->m_scriptEditorSaving = false;
                if (ok && grid->m_scriptFileEdit) {
                    grid->m_scriptFileEdit->document()->setModified(false); // wjy: 保存成功后标记当前文本已经同步到被控机本地 work 文件。
                }
                if (grid->m_scriptFileSaveButton) {
                    grid->m_scriptFileSaveButton->setText(ok ? QString::fromUtf8("已保存") : QString::fromUtf8("失败"));
                    QTimer::singleShot(1200, grid, [grid] {
                        if (grid) {
                            grid->updateScriptFileEditorControls();
                        }
                    });
                }
            }
            if (!ok) {
                writeScriptOutputFile(
                    state.outputFilePath,
                    QString::fromUtf8("\n编辑器保存失败：%1\n").arg(errorMessage.trimmed().isEmpty() ? outputText.trimmed() : errorMessage.trimmed()),
                    QIODevice::Append);
                state.outputDirty = true;
                grid->m_scriptUiStates.insert(targetIp, state);
                if (targetIsCurrent) {
                    grid->m_scriptOutputDirty = true;
                }
                if (targetIsCurrent && !grid->m_scriptOutputFlushTimer->isActive()) {
                    grid->m_scriptOutputFlushTimer->start();
                }
            }
            if (targetIsCurrent) {
                grid->updateScriptFileEditorControls();
            }
        }, Qt::QueuedConnection);
    });
// ===end====
}

void DeviceGrid::stopCurrentDeviceScript()
{
// =====wjy====
    stopDeviceScriptForDeviceIndex(m_selectedDeviceIndex, true); // wjy: 当前面板的停止按钮也走指定设备停止逻辑，和分组批量停止保持一致。
// ===end====
}

bool DeviceGrid::stopDeviceScriptForDeviceIndex(int deviceIndex, bool showMessages)
{
// =====wjy====
    if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return false;
    }

    const QString deviceIp = g_devices.at(deviceIndex).ip.trimmed();
    ScriptUiState state = m_scriptUiStates.value(deviceIp);
    if (!state.outputRunning) {
        return false;
    }

    const QString loginUser = state.editorLoginUser.trimmed();
    const QString scriptWorkName = state.editorWorkName.trimmed();
    const QString outputFilePath = state.outputFilePath;
    const auto cancelRequested = state.cancelRequested;
    if (deviceIp.isEmpty() || loginUser.isEmpty() || scriptWorkName.isEmpty()) {
        if (cancelRequested) {
            cancelRequested->store(true); // wjy: 缺少远端定位信息时退回旧逻辑，至少断开当前 SSH 执行。
        }
        state.outputRunning = false;
        state.outputFailed = false;
        m_scriptUiStates.insert(deviceIp, state);
        if (currentScriptUiDeviceIp() == deviceIp) {
            loadScriptUiStateForDevice(deviceIp);
        }
        return true;
    }

    state.outputRunning = false;
    state.outputFailed = false;
    writeScriptOutputFile(outputFilePath, QString::fromUtf8("\n状态: 正在停止目标进程...\n"), QIODevice::Append);
    state.outputText = stripTerminalControlSequences(readScriptOutputFileTail(outputFilePath));
    m_scriptUiStates.insert(deviceIp, state); // wjy: 先把目标设备状态写成停止中，当前 UI 或切回该设备时都能看到停止状态。
    if (currentScriptUiDeviceIp() == deviceIp) {
        loadScriptUiStateForDevice(deviceIp);
    }

    const QString remotePowerShellScript = QStringLiteral(R"($ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$fsremoteExe = Get-Process FSRemote -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Path
if ([string]::IsNullOrWhiteSpace($fsremoteExe)) {
    Write-Output 'cannot locate FSRemote.exe on target device'
    exit 0
}
$fsremoteDir = Split-Path -Parent $fsremoteExe
$work = Join-Path (Join-Path $fsremoteDir 'work') '%1'
$pidFile = Join-Path $work 'fsremote_script_controller_pid.txt'
$stopRequestFile = Join-Path $work 'fsremote_script_stop_requested.txt'
('Stop requested: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) | Set-Content -LiteralPath $stopRequestFile -Encoding UTF8
if (-not (Test-Path -LiteralPath $pidFile)) {
    Write-Output 'remote script pid file not found'
    exit 0
}
$pidText = Get-Content -LiteralPath $pidFile -Raw
$match = [regex]::Match($pidText, '\d+')
if (-not $match.Success) {
    Write-Output 'remote script pid file has no pid'
    exit 0
}
$targetPid = [int]$match.Value
if ($targetPid -le 0) {
    Write-Output 'remote script pid is invalid'
    exit 0
}
Write-Output ('taskkill remote script tree pid=' + $targetPid)
& taskkill.exe /PID $targetPid /T /F 2>&1 | ForEach-Object { Write-Output $_ }
Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
exit 0
)").arg(escapedPowerShellSingleQuoted(scriptWorkName));
    const QStringList commands = {
        QStringLiteral("powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand %1 & exit %ERRORLEVEL%")
            .arg(powerShellEncodedCommand(remotePowerShellScript)),
    };

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, deviceIp, loginUser, commands, outputFilePath, cancelRequested] {
        QString outputText;
        QString errorMessage;
        const bool ok = platform::PortableOpenSshManager::instance().runRemoteCommands(
            deviceIp,
            loginUser,
            commands,
            &outputText,
            &errorMessage,
            15000);
        if (cancelRequested) {
            cancelRequested->store(true); // wjy: 远端 taskkill 发出后再让原 SSH 执行会话退出，避免只杀本地 ssh 留下目标子进程。
        }
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self, ok, deviceIp, outputFilePath, outputText, errorMessage] {
            if (!self) {
                return;
            }
            DeviceGrid* grid = self.data();
            const QString targetIp = deviceIp.trimmed();
            ScriptUiState state = grid->m_scriptUiStates.value(targetIp);
            if (state.outputFilePath != outputFilePath) {
                return;
            }
            const QString result = ok ? outputText.trimmed() : errorMessage.trimmed();
            if (!result.isEmpty()) {
                writeScriptOutputFile(outputFilePath, QString::fromUtf8("\n%1\n").arg(stripTerminalControlSequences(result)), QIODevice::Append);
            }
            state.outputDirty = true;
            grid->m_scriptUiStates.insert(targetIp, state);
            if (grid->currentScriptUiDeviceIp() == targetIp) {
                grid->m_scriptOutputDirty = true;
                if (!grid->m_scriptOutputFlushTimer->isActive()) {
                    grid->m_scriptOutputFlushTimer->start();
                }
            }
        }, Qt::QueuedConnection);
    });
    return true;
// ===end====
}

void DeviceGrid::stopDeviceGroupScripts(int groupIndex)
{
// =====wjy====
    if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return;
    }

    const QString groupName = g_deviceGroupNames.at(groupIndex).trimmed();
    int stoppedCount = 0;
    for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        if (g_devices.at(deviceIndex).group.trimmed() != groupName) {
            continue;
        }
        if (stopDeviceScriptForDeviceIndex(deviceIndex, false)) {
            ++stoppedCount; // wjy: 只停止组内确实正在运行脚本的设备，未运行的设备静默跳过。
        }
    }

    if (stoppedCount <= 0) {
        QMessageBox messageBox(
            QMessageBox::Information,
            QString(),
            QString::fromUtf8("分组内没有正在执行的脚本。"),
            QMessageBox::NoButton,
            this);
        messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
        messageBox.exec();
    } else {
        update(); // wjy: 当前详情设备属于该分组时，批量停止后立即刷新停止状态。
    }
// ===end====
}

void DeviceGrid::setupAddDeviceControls()
{
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] setupAddDeviceControls begin")); // wjy: 细分新增设备控件初始化入口，定位 Release 是否崩在控件创建阶段。
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
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] setupAddDeviceControls styles ready")); // wjy: 样式字符串创建完成，后续开始逐个创建输入框和按钮。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before device ip edit create")); // wjy: 判断是否崩在第一个新增设备 IP 输入框创建。
    m_deviceIpEdit = new QLineEdit(this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device ip edit create")); // wjy: IP 输入框对象创建完成。
    m_deviceIpEdit->setGeometry(304, 250, 252, 34);
    m_deviceIpEdit->setPlaceholderText(QStringLiteral("192.168.1.100"));
    m_deviceIpEdit->setStyleSheet(inputStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device ip edit setup")); // wjy: IP 输入框位置、占位文本和样式设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before device name edit create")); // wjy: 判断是否崩在设备名称输入框创建。
    m_deviceNameEdit = new QLineEdit(this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device name edit create")); // wjy: 设备名称输入框对象创建完成。
    m_deviceNameEdit->setGeometry(584, 250, 252, 34);
    m_deviceNameEdit->setPlaceholderText(zh("\xE4\xBE\x8B\xE5\xA6\x82\xEF\xBC\x9A\xE5\x8A\x9E\xE5\x85\xAC\xE5\xAE\xA4\xE4\xB8\xBB\xE6\x9C\xBA"));
    m_deviceNameEdit->setStyleSheet(inputStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device name edit setup")); // wjy: 设备名称输入框位置、中文占位文本和样式设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before device mac edit create")); // wjy: 判断是否崩在 MAC 输入框创建。
    m_deviceMacEdit = new QLineEdit(this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device mac edit create")); // wjy: MAC 输入框对象创建完成。
    m_deviceMacEdit->setGeometry(304, 332, 252, 34);
    m_deviceMacEdit->setPlaceholderText(zh("\xE5\x8F\xAF\xE9\x80\x89"));
    m_deviceMacEdit->setStyleSheet(inputStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device mac edit setup")); // wjy: MAC 输入框位置、占位文本和样式设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before device remark edit create")); // wjy: 判断是否崩在备注输入框创建。
    m_deviceRemarkEdit = new QLineEdit(this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device remark edit create")); // wjy: 备注输入框对象创建完成。
    m_deviceRemarkEdit->setGeometry(584, 332, 252, 34);
    m_deviceRemarkEdit->setPlaceholderText(zh("\xE5\x8F\xAF\xE9\x80\x89"));
    m_deviceRemarkEdit->setStyleSheet(inputStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after device remark edit setup")); // wjy: 备注输入框位置、占位文本和样式设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before cancel button create")); // wjy: 判断是否崩在取消按钮创建。
    m_cancelDeviceButton = new QPushButton(zh("\xE5\x8F\x96\xE6\xB6\x88"), this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after cancel button create")); // wjy: 取消按钮对象创建完成。
    m_cancelDeviceButton->setGeometry(572, 424, 124, 34);
    m_cancelDeviceButton->setCursor(Qt::PointingHandCursor);
    m_cancelDeviceButton->setStyleSheet(buttonStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after cancel button setup")); // wjy: 取消按钮位置、鼠标样式和样式表设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before save button create")); // wjy: 判断是否崩在保存按钮创建。
    m_saveDeviceButton = new QPushButton(zh("\xE4\xBF\x9D\xE5\xAD\x98"), this);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after save button create")); // wjy: 保存按钮对象创建完成。
    m_saveDeviceButton->setGeometry(712, 424, 124, 34);
    m_saveDeviceButton->setCursor(Qt::PointingHandCursor);
    m_saveDeviceButton->setStyleSheet(primaryButtonStyle);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after save button setup")); // wjy: 保存按钮位置、鼠标样式和主按钮样式设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before add device signal connects")); // wjy: 判断是否崩在新增设备控件信号连接。
    connect(m_deviceIpEdit, &QLineEdit::textChanged, this, &DeviceGrid::updateAddDeviceControls);
    connect(m_deviceNameEdit, &QLineEdit::textChanged, this, &DeviceGrid::updateAddDeviceControls);
    connect(m_saveDeviceButton, &QPushButton::clicked, this, &DeviceGrid::saveNewDevice);
    connect(m_cancelDeviceButton, &QPushButton::clicked, this, &DeviceGrid::cancelNewDevice);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after add device signal connects")); // wjy: 新增设备控件信号连接完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before updateAddDeviceControls")); // wjy: 判断是否崩在首次刷新新增设备控件显隐状态。
    updateAddDeviceControls();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] setupAddDeviceControls end")); // wjy: 新增设备控件初始化完整结束。
}

void DeviceGrid::setupSettingsControls()
{
    // =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] setupSettingsControls begin")); // wjy: 进入设置控件初始化，细分 Release 偶发崩溃发生在设置页的哪一步。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before status interval edit create")); // wjy: 判断是否崩在自动刷新间隔输入框创建。
    // ===end====
    m_statusRefreshIntervalEdit = new QLineEdit(this);
    // =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after status interval edit create")); // wjy: 自动刷新间隔输入框对象创建完成。
    // ===end====
    m_statusRefreshIntervalEdit->setGeometry(654, 364, 112, 32);
    m_statusRefreshIntervalEdit->setValidator(new QIntValidator(1, 86400, m_statusRefreshIntervalEdit)); // wjy: 只允许输入正整数秒数，避免用户输入字母或符号导致定时器间隔异常。
    m_statusRefreshIntervalEdit->setText(QString::number(qMax(1, m_statusAutoRefreshIntervalSeconds)));
    m_statusRefreshIntervalEdit->setAlignment(Qt::AlignCenter);
    m_statusRefreshIntervalEdit->setPlaceholderText(QStringLiteral("60"));
    m_statusRefreshIntervalEdit->setStyleSheet(QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;padding:0 10px;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QLineEdit:focus{border:1px solid #3A7BFC;}"
        "QLineEdit:disabled{background:#F5F7FA;border:1px solid #DDE3EA;color:#687384;}"));
    // =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after status interval edit basic setup")); // wjy: 输入框位置、校验器、初始值和样式设置完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before status interval edit signal connect")); // wjy: 判断是否崩在输入框保存逻辑信号连接。
    // ===end====
    const auto saveStatusRefreshInterval = [this] {
        if (!m_statusRefreshIntervalEdit) {
            return; // wjy: 防御性判空，避免关闭阶段信号触发访问已释放控件。
        }
        int seconds = m_statusRefreshIntervalEdit->text().trimmed().toInt();
        if (seconds <= 0) {
            seconds = 60; // wjy: 输入为空或非法时回到默认 60 秒。
            m_statusRefreshIntervalEdit->setText(QString::number(seconds));
        }
        m_statusAutoRefreshIntervalSeconds = seconds;
        platform::AppSettings::setStatusAutoRefreshIntervalSeconds(m_statusAutoRefreshIntervalSeconds);
        applyStatusAutoRefreshSetting(false); // wjy: 保存后立即重启自动刷新定时器，让新的秒数马上生效。
    };
    connect(m_statusRefreshIntervalEdit, &QLineEdit::editingFinished, this, saveStatusRefreshInterval);
    connect(m_statusRefreshIntervalEdit, &QLineEdit::returnPressed, this, saveStatusRefreshInterval);
    // =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after status interval edit signal connect")); // wjy: 自动刷新间隔输入框信号连接完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before batch add controls create")); // wjy: 开始创建批量新增真实控件，后续若异常可定位到设置页批量新增区域。
    m_batchSubnetEdit = new QLineEdit(this);
    m_batchSubnetEdit->setGeometry(settingsBatchSubnetInputRect());
    m_batchSubnetEdit->setText(QStringLiteral("192.168.3.*"));
    m_batchSubnetEdit->setPlaceholderText(QStringLiteral("192.168.3.*"));
    m_batchSubnetEdit->setAlignment(Qt::AlignCenter);
    m_batchSubnetEdit->setStyleSheet(QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:4px;padding:0 10px;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#040B18;}"
        "QLineEdit:focus{border:1px solid #3A7BFC;}"
        "QLineEdit:disabled{background:#F5F7FA;border:1px solid #DDE3EA;color:#687384;}")); // wjy: 复用设置页数字输入框的真实控件样式，只在点击按钮时校验 192.168.3.* 格式。
    m_batchAddButton = new QPushButton(QStringLiteral("批量新增"), this);
    m_batchAddButton->setGeometry(settingsBatchAddButtonRect());
    m_batchAddButton->setCursor(Qt::PointingHandCursor);
    m_batchAddButton->setStyleSheet(QStringLiteral(
        "QPushButton{border:0;border-radius:4px;background:#3A7BFC;"
        "font-family:'Microsoft YaHei UI';font-size:14px;color:#FFFFFF;}"
        "QPushButton:hover{background:#2F6FEF;}"
        "QPushButton:disabled{background:#C9D0DA;color:#FFFFFF;}")); // wjy: 扫描期间按钮禁用，避免用户重复启动多个批量扫描任务。
    connect(m_batchAddButton, &QPushButton::clicked, this, &DeviceGrid::startBatchAddDevices);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after batch add controls create")); // wjy: 批量新增输入框和按钮创建完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before updateSettingsControls in setup")); // wjy: 判断是否崩在首次刷新设置控件显隐状态。
    // ===end====

    updateSettingsControls();
    // =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] setupSettingsControls end")); // wjy: 设置控件初始化完整结束。
    // ===end====
}

void DeviceGrid::updateAddDeviceControls()
{
    if (!m_deviceIpEdit || !m_deviceNameEdit || !m_deviceMacEdit || !m_deviceRemarkEdit
        || !m_saveDeviceButton || !m_cancelDeviceButton) { // wjy: 子控件隔离测试期间不创建输入框/按钮，外部点击仍可能触发刷新函数，必须先判空。
        return; // wjy: 没有新增设备控件时直接跳过显隐/可用状态刷新，避免空指针崩溃干扰堆损坏定位。
    }

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
    if (!m_statusRefreshIntervalEdit) {
        return;
    }
// =====wjy====
    const bool visible =
        m_settingsSelected
        && m_statusAutoRefreshEnabled; // wjy: 自动刷新关闭时隐藏秒数输入框；打开后才显示，避免设置页出现无效输入区域。
    m_statusRefreshIntervalEdit->setVisible(visible);
    m_statusRefreshIntervalEdit->setEnabled(m_statusAutoRefreshEnabled);
    if (visible) {
        m_statusRefreshIntervalEdit->raise(); // wjy: 输入框是 Qt 子控件，显示时提升到手绘设置页上层，避免被父控件重绘视觉上盖住。
    }

    if (m_batchSubnetEdit && m_batchAddButton) {
        const bool batchVisible = m_settingsSelected; // wjy: 批量新增属于设置页功能，离开设置页时隐藏真实控件。
        m_batchSubnetEdit->setVisible(batchVisible);
        m_batchAddButton->setVisible(batchVisible);
        m_batchSubnetEdit->setEnabled(batchVisible && !m_batchAddInProgress);
        m_batchAddButton->setEnabled(batchVisible && !m_batchAddInProgress);
        m_batchAddButton->setText(m_batchAddInProgress ? QStringLiteral("扫描中") : QStringLiteral("批量新增"));
        if (batchVisible) {
            m_batchSubnetEdit->raise();
            m_batchAddButton->raise();
        }
    }

// ===end====
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

void DeviceGrid::startBatchAddDevices()
{
// =====wjy====
    if (!m_batchSubnetEdit || m_batchAddInProgress) {
        return; // wjy: 控件未创建或已有扫描正在运行时不重复启动。
    }

    const QString subnetText = m_batchSubnetEdit->text().trimmed();
    if (!isWildcardSubnetPattern(subnetText)) {
        qWarning().noquote() << QStringLiteral("[batch-add] invalid subnet=%1").arg(subnetText);
        m_batchSubnetEdit->setText(QStringLiteral("192.168.3.*")); // wjy: 非法输入回到默认网段，和设置页秒数输入框的兜底思路一致。
        m_batchSubnetEdit->selectAll();
        m_batchSubnetEdit->setFocus(Qt::MouseFocusReason);
        return;
    }

    const QStringList allScanIps = wildcardSubnetScanIps(subnetText);
    QStringList scanIps;
    scanIps.reserve(allScanIps.size());
    for (const QString& ip : allScanIps) {
        scanIps.append(ip); // wjy: 已存在设备也继续扫描，用返回的 MAC 补齐旧记录，避免远程开机时仍提示未填写 MAC。
    }
    if (scanIps.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[batch-add] no new ip to scan subnet=%1").arg(subnetText);
        return;
    }

    m_batchAddInProgress = true;
    updateSettingsControls();
    qWarning().noquote() << QStringLiteral("[batch-add] scan begin subnet=%1 count=%2")
        .arg(subnetText)
        .arg(scanIps.size());

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, scanIps] {
        QVector<BatchAddResult> results;
        std::mutex resultMutex;
        std::atomic_int nextIndex = 0;
        const int workerCount = std::max(1, std::min<int>(16, scanIps.size()));
        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (int worker = 0; worker < workerCount; ++worker) {
            workers.emplace_back([&, worker] {
                while (true) {
                    const int index = nextIndex.fetch_add(1);
                    if (index >= scanIps.size()) {
                        break;
                    }

                    const QString ip = scanIps.at(index);
                    const platform::DeviceStatusInfo info = platform::DeviceStatusService::query(ip, 49101, 250);
                    if (info.state == platform::DevicePresenceState::Offline
                        || info.state == platform::DevicePresenceState::Unknown) {
                        continue; // wjy: 只有状态服务有响应的远端才视为可批量新增设备。
                    }
                    if (info.localIp.trimmed() != ip || info.mac.trimmed().isEmpty()) {
                        qWarning().noquote() << QStringLiteral("[batch-add] skip weak match ip=%1 remoteIp=%2 mac=%3")
                            .arg(ip)
                            .arg(info.localIp.trimmed())
                            .arg(info.mac.trimmed()); // wjy: 必须拿到 FSRemote 状态服务返回的本机 IP 和 MAC，避免把无效端口响应误加成设备。
                        continue;
                    }

                    BatchAddResult result;
                    result.ip = ip;
                    result.name = info.deviceName.trimmed().isEmpty()
                        ? ip
                        : info.deviceName.trimmed(); // wjy: 设备名优先使用远端电脑名，取不到时用 IP 兜底，不再使用登录账户名。
                    result.mac = info.mac.trimmed();
                    result.broadcastIp = info.broadcastIp.trimmed();

                    std::lock_guard lock(resultMutex);
                    results.append(result);
                }
            });
        }

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        if (!self) {
            return; // wjy: 窗口关闭后不再回到 UI 线程追加设备。
        }

        QMetaObject::invokeMethod(self, [self, results = std::move(results)]() mutable {
            if (!self) {
                return; // wjy: queued 回调执行前窗口可能已经销毁。
            }

            DeviceGrid* grid = self.data();
            const bool wasEmpty = g_devices.isEmpty();
            int firstAddedIndex = -1;
            int addedCount = 0;
            int updatedCount = 0;
            QSet<QString> addedIps;
            for (const BatchAddResult& result : results) {
                const QString ip = result.ip.trimmed();
                if (ip.isEmpty() || addedIps.contains(ip)) {
                    continue; // wjy: UI 线程最终追加前再次去重，防止扫描期间用户手动新增同一 IP。
                }

                const int existingIndex = deviceIndexForIp(ip);
                if (existingIndex >= 0) {
                    bool deviceUpdated = false;
                    DeviceEntry& existingDevice = g_devices[existingIndex];
                    if (existingDevice.mac.trimmed().isEmpty() && !result.mac.trimmed().isEmpty()) {
                        existingDevice.mac = result.mac.trimmed(); // wjy: 批量扫描命中旧设备时补齐 MAC，让后续远程开机不再因为旧记录为空而被拦截。
                        deviceUpdated = true;
                    }
                    if (existingDevice.broadcastIp.trimmed().isEmpty() && !result.broadcastIp.trimmed().isEmpty()) {
                        existingDevice.broadcastIp = result.broadcastIp.trimmed(); // wjy: 同步保存广播地址，远程开机代理可直接复用扫描到的网段信息。
                        deviceUpdated = true;
                    }
                    grid->m_deviceStatuses.insert(ip, platform::DevicePresenceState::Online);
                    addedIps.insert(ip);
                    if (deviceUpdated) {
                        ++updatedCount;
                    }
                    continue;
                }

                const QString name = result.name.trimmed().isEmpty() ? ip : result.name.trimmed();
                g_devices.append({name, ip, result.mac.trimmed(), result.broadcastIp.trimmed(), QStringLiteral("批量增加"), {}});
                grid->m_deviceStatuses.insert(ip, platform::DevicePresenceState::Online);
                addedIps.insert(ip);
                if (firstAddedIndex < 0) {
                    firstAddedIndex = g_devices.size() - 1;
                }
                ++addedCount;
            }

            if (addedCount > 0 || updatedCount > 0) {
                saveDevices();
            }

            if (addedCount > 0) {
                grid->m_remoteAssistSelected = false;
                grid->m_localInfoSelected = false;
                grid->m_settingsSelected = false;
                grid->m_deviceGroupExpanded = true;
                grid->clearBottomActionHover();
                grid->setDesktopHoverActive(false);
                grid->m_selectedDeviceIndexes.clear();
                grid->m_selectedDeviceIndexes.insert(firstAddedIndex);
                grid->m_selectionAnchorDeviceIndex = firstAddedIndex;
                if (wasEmpty) {
                    grid->m_selectedDeviceIndex = firstAddedIndex;
                    grid->m_previousDeviceIndex = firstAddedIndex;
                    grid->m_currentDeviceName = deviceDisplayName(g_devices.at(firstAddedIndex));
                    grid->m_previousDeviceName = grid->m_currentDeviceName;
                    grid->loadScriptUiStateForDevice(grid->currentScriptUiDeviceIp()); // wjy: 批量新增第一台设备时初始化它自己的空脚本 UI。
                    if (grid->m_detailAnimationTimer) {
                        grid->m_detailAnimationTimer->stop();
                    }
                } else {
                    grid->startDeviceSwitchAnimation(firstAddedIndex, deviceDisplayName(g_devices.at(firstAddedIndex)));
                }
            }

            grid->m_batchAddInProgress = false;
            grid->updateSettingsControls();
            grid->updateAddDeviceControls();
            grid->updateLocalInfoControls();
            grid->update();
            qWarning().noquote() << QStringLiteral("[batch-add] scan end found=%1 added=%2 updated=%3")
                .arg(results.size())
                .arg(addedCount)
                .arg(updatedCount);
        }, Qt::QueuedConnection);
    });
// ===end====
}

void DeviceGrid::beginDeviceGroupRename(int groupIndex)
{
// =====wjy====
    if (!m_deviceGroupNameEdit || groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return; // wjy: 输入框不存在或分组下标无效时，不进入编辑状态。
    }

    const int rowIndex = visualRowIndexForGroupIndex(groupIndex); // wjy: 找到分组当前在左侧列表中的视觉行号。
    if (rowIndex < 0) {
        return;
    }

    const QRect rowRect = scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset); // wjy: 复用分组滚动后的可见行矩形，让输入框贴在当前屏幕位置。
    m_renamingDeviceGroupIndex = groupIndex; // wjy: 记录正在编辑的分组，绘制时隐藏底层文字。
    m_deviceGroupNameEdit->setGeometry(QRect(56, rowRect.y() + 5, 136, 26)); // wjy: 输入框覆盖分组文字区域，不挡住右侧箭头。
    m_deviceGroupNameEdit->setText(g_deviceGroupNames.at(groupIndex)); // wjy: 把当前分组名放进输入框，方便直接修改。
    m_deviceGroupNameEdit->selectAll(); // wjy: 双击后默认全选，用户可以直接输入新名字覆盖。
    m_deviceGroupNameEdit->show();
    m_deviceGroupNameEdit->raise();
    m_deviceGroupNameEdit->setFocus(Qt::MouseFocusReason); // wjy: 双击后立刻获得焦点，可以直接键入。
    update(rowRect);
// ===end====
}

void DeviceGrid::finishDeviceGroupRename(bool saveText)
{
// =====wjy====
    if (!m_deviceGroupNameEdit || m_renamingDeviceGroupIndex < 0) {
        return; // wjy: 没有正在编辑的分组时直接返回，防止 editingFinished 重复触发。
    }

    const int groupIndex = m_renamingDeviceGroupIndex; // wjy: 先保存下标，后面会清空编辑状态。
    const int rowIndex = visualRowIndexForGroupIndex(groupIndex);
    const QRect rowRect = rowIndex >= 0 ? scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset) : QRect();
    if (saveText && groupIndex >= 0 && groupIndex < g_deviceGroupNames.size()) {
        const QString oldName = g_deviceGroupNames.at(groupIndex).trimmed();
        const QString newName = m_deviceGroupNameEdit->text().trimmed();

        bool duplicated = false;
        for (int i = 0; i < g_deviceGroupNames.size(); ++i) {
            if (i != groupIndex
                && g_deviceGroupNames.at(i).trimmed() == newName) {
                duplicated = true;
                break;
            }
        }

        if (!newName.isEmpty() && !duplicated) {
            // 同步修改原来属于该分组的设备
            for (DeviceEntry& device : g_devices) {
                if (device.group.trimmed() == oldName) {
                    device.group = newName;
                }
            }

            g_deviceGroupNames[groupIndex] = newName;
            saveDevices();
        } else {
            // 空名字或者名字重复时恢复原名称
            m_deviceGroupNameEdit->setText(oldName);
        }
    }

    m_renamingDeviceGroupIndex = -1; // wjy: 清空编辑状态。
    m_deviceGroupNameEdit->hide(); // wjy: 隐藏输入框，恢复普通分组行显示。
    if (rowRect.isValid()) {
        update(rowRect);
    } else {
        update();
    }
// ===end====
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

    const bool addingFirstDevice = g_devices.isEmpty(); // wjy: First device has no previous detail page, so switching animation would draw the same device twice.
    g_devices.append({name, ip, mac, {}, m_deviceRemarkEdit->text().trimmed(), {}}); // wjy: 新增设备默认无分组，只有后续拖入具体分组时才写 group。
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
    const int newDeviceIndex =
        g_devices.size() - 1;

    m_selectedDeviceIndexes.clear();
    m_selectedDeviceIndexes.insert(
        newDeviceIndex);

    m_selectionAnchorDeviceIndex =
        newDeviceIndex;

    if (addingFirstDevice) { // wjy: Avoid duplicate detail UI when the first saved device becomes both previous and current index 0.
        m_selectedDeviceIndex = newDeviceIndex;
        m_previousDeviceIndex = newDeviceIndex;
        m_currentDeviceName = name;
        m_previousDeviceName = name;
        loadScriptUiStateForDevice(currentScriptUiDeviceIp()); // wjy: 手动新增第一台设备时，下方脚本 UI 初始化为这台设备自己的空状态。
        if (m_detailAnimationTimer) {
            m_detailAnimationTimer->stop();
        }
        update();
    } else {
        startDeviceSwitchAnimation(
            newDeviceIndex,
            name);
    }
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
// =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-status] refresh begin")); // wjy: BUG诊断日志统一写入文件，确认 Release 是否进入状态刷新函数。
    if (m_statusRefreshInProgress) {
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] refresh skipped inProgress=1")); // wjy: 如果上一次刷新还没结束，记录跳过原因。
        return;
    }
// ===end====

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
// =====wjy====
    writeDeviceGridStartupLog(QStringLiteral("[wjy-status] ip count=%1 values=%2")
        .arg(ips.size())
        .arg(ips.join(QStringLiteral(",")))); // wjy: 记录本次从 devices.json 加载出的待探测 IP，判断崩溃是否和某个设备有关。
// ===end====

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, ips] {
// =====wjy====
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] background thread start ipCount=%1").arg(ips.size())); // wjy: 后台总线程启动日志写入文件，确认是否进入线程阶段。
// ===end====
        QHash<QString, platform::DevicePresenceState> statuses;
        statuses.reserve(ips.size());
        for (const QString& ip : ips) {
            statuses.insert(ip, platform::DevicePresenceState::Offline); // wjy: 每轮刷新先默认离线，只有状态服务明确返回 online/busy 时才覆盖，避免沿用旧在线状态。
        }
        QHash<QString, QString> remoteDeviceNames; // wjy: 自动刷新时顺便记录远端真实设备名，用来同步 devices.json 的 name 字段。
        QHash<QString, QString> remoteDeviceMacs; // wjy: 状态服务返回 MAC 时用于补齐旧设备记录，避免远程开机仍读到空 MAC。
        QHash<QString, QString> remoteDeviceBroadcastIps; // wjy: 同步记录广播地址，后续远程开机代理可直接使用正确网段。
        std::mutex resultMutex;
        std::atomic_int nextIndex = 0;
        const int workerCount = std::max(1, std::min<int>(8, ips.size()));
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
// =====wjy====
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] worker count=%1").arg(workerCount)); // wjy: 记录本次会创建几个并发探测线程。
// ===end====

        for (int worker = 0; worker < workerCount; ++worker) {
            workers.emplace_back([&, worker] { // wjy: 捕获 worker 副本用于日志，避免并发日志读取循环变量引用。
// =====wjy====
                writeDeviceGridStartupLog(QStringLiteral("[wjy-status] worker start worker=%1").arg(worker)); // wjy: 每个 worker 启动时记录编号，判断是否创建线程后崩溃。
// ===end====
                while (true) {
                    const int index = nextIndex.fetch_add(1);
                    if (index >= ips.size()) {
                        break;
                    }
                    const QString ip = ips.at(index);
// =====wjy====
                    writeDeviceGridStartupLog(QStringLiteral("[wjy-status] probe begin worker=%1 index=%2 ip=%3")
                        .arg(worker)
                        .arg(index)
                        .arg(ip)); // wjy: 单个 IP 探测前记录，崩溃时可定位是否卡在某台设备。
// ===end====
                    const platform::DeviceStatusInfo info = platform::DeviceStatusService::query(ip);
// =====wjy====
                    writeDeviceGridStartupLog(QStringLiteral("[wjy-status] probe done worker=%1 index=%2 ip=%3 state=%4")
                        .arg(worker)
                        .arg(index)
                        .arg(ip)
                        .arg(static_cast<int>(info.state))); // wjy: 单个 IP 探测后记录状态枚举值，判断 query 是否顺利返回。
// ===end====
                    std::lock_guard lock(resultMutex);
                    statuses.insert(ip, info.state);
                    if (!info.deviceName.trimmed().isEmpty()) {
                        remoteDeviceNames.insert(ip, info.deviceName.trimmed()); // wjy: 只有新版远端返回了真实设备名时才参与本地 JSON 同步。
                    }
                    if (!info.mac.trimmed().isEmpty()) {
                        remoteDeviceMacs.insert(ip, info.mac.trimmed()); // wjy: 在线刷新拿到 MAC 后回填本地设备记录，修复旧批量新增留下的空值。
                    }
                    if (!info.broadcastIp.trimmed().isEmpty()) {
                        remoteDeviceBroadcastIps.insert(ip, info.broadcastIp.trimmed()); // wjy: 广播地址和 MAC 一起回填，保持远程开机参数完整。
                    }
                }
// =====wjy====
                writeDeviceGridStartupLog(QStringLiteral("[wjy-status] worker end worker=%1").arg(worker)); // wjy: worker 结束日志，判断线程是否正常跑完。
// ===end====
            });
        }

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
// =====wjy====
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] all workers joined statusCount=%1").arg(statuses.size())); // wjy: 所有 worker 汇合后记录结果数量。
// ===end====

        if (!self) {
// =====wjy====
            writeDeviceGridStartupLog(QStringLiteral("[wjy-status] widget destroyed before invoke")); // wjy: 如果界面已销毁，记录后直接退出后台线程。
// ===end====
            return;
        }

// =====wjy====
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] invoke ui post begin")); // wjy: 准备把后台探测结果投递回 UI 线程；如果有 begin 没有 end，说明崩在投递附近。
        const bool invokeQueued = QMetaObject::invokeMethod(self, [self, statuses = std::move(statuses), remoteDeviceNames = std::move(remoteDeviceNames), remoteDeviceMacs = std::move(remoteDeviceMacs), remoteDeviceBroadcastIps = std::move(remoteDeviceBroadcastIps)]() mutable {
// =====wjy====
            writeDeviceGridStartupLog(QStringLiteral("[wjy-status] invoke ui begin")); // wjy: 回到 UI 线程前半段日志，判断崩溃是否发生在 UI 更新阶段。
// ===end====
            if (!self) {
// =====wjy====
                writeDeviceGridStartupLog(QStringLiteral("[wjy-status] widget destroyed inside invoke")); // wjy: UI 回调执行时控件已销毁，记录并退出。
// ===end====
                return;
            }
            DeviceGrid* grid = self.data();
            grid->m_deviceStatuses = std::move(statuses);
            bool deviceRecordChanged = false;
            for (DeviceEntry& device : g_devices) {
                const QString ip = device.ip.trimmed();
                const QString remoteMac = remoteDeviceMacs.value(ip).trimmed();
                const QString remoteBroadcastIp = remoteDeviceBroadcastIps.value(ip).trimmed();
                if (!ip.isEmpty()
                    && device.mac.trimmed().isEmpty()
                    && !remoteMac.isEmpty()) {
                    qWarning().noquote() << QStringLiteral("[wjy-status] sync device mac ip=%1 mac=%2")
                        .arg(ip)
                        .arg(remoteMac); // wjy: 自动刷新发现旧设备没有 MAC 时补齐，避免远程开机入口继续弹未填写 MAC。
                    device.mac = remoteMac;
                    deviceRecordChanged = true;
                }
                if (!ip.isEmpty()
                    && device.broadcastIp.trimmed().isEmpty()
                    && !remoteBroadcastIp.isEmpty()) {
                    device.broadcastIp = remoteBroadcastIp; // wjy: 旧记录没有广播地址时顺手补齐，和批量新增保存结构保持一致。
                    deviceRecordChanged = true;
                }
                const QString remoteName = remoteDeviceNames.value(ip).trimmed();
                const QString pendingRemoteRename = grid->m_pendingRemoteRenameNames.value(ip).trimmed(); // wjy: 远端改名成功后 Windows 可能要重启才上报新电脑名，先记住等待生效的新名字。
                if (!pendingRemoteRename.isEmpty()) {
                    if (remoteName == pendingRemoteRename) {
                        grid->m_pendingRemoteRenameNames.remove(ip); // wjy: 远端已经上报新名字，说明重启或系统刷新后改名生效，可以恢复正常自动同步。
                    } else {
                        continue; // wjy: 远端仍上报旧名字时不要覆盖本地手动新名字，避免 53_TEST 改 53 后被刷新立刻改回去。
                    }
                }
                if (!ip.isEmpty()
                    && !remoteName.isEmpty()
                    && device.name.trimmed() != remoteName) {
                    qWarning().noquote() << QStringLiteral("[wjy-status] sync device name ip=%1 old=%2 new=%3")
                        .arg(ip)
                        .arg(device.name.trimmed())
                        .arg(remoteName); // wjy: 自动刷新发现 JSON 设备名和远端真实设备名不一致，按远端设备名修正本地记录。
                    device.name = remoteName;
                    deviceRecordChanged = true;
                }
            }
            if (deviceRecordChanged) {
                saveDevices(); // wjy: 设备名或 MAC/广播地址同步后立即保存 devices.json，保证下次启动仍使用修正后的记录。
                if (grid->m_selectedDeviceIndex >= 0 && grid->m_selectedDeviceIndex < g_devices.size()) {
                    grid->m_currentDeviceName = deviceDisplayName(g_devices.at(grid->m_selectedDeviceIndex));
                    grid->m_previousDeviceName = grid->m_currentDeviceName;
                }
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
            grid->m_statusRefreshInProgress = false;
            grid->update();
// =====wjy====
            writeDeviceGridStartupLog(QStringLiteral("[wjy-status] invoke ui end")); // wjy: UI 状态写入和重绘请求完成日志。
// ===end====
        }, Qt::QueuedConnection);
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] invoke ui post end queued=%1").arg(invokeQueued)); // wjy: 记录投递是否成功排队，继续定位关闭/启动偶发异常发生点。
        writeDeviceGridStartupLog(QStringLiteral("[wjy-status] background thread end")); // wjy: 后台总线程即将退出，和 DeviceGrid 析构日志对照。
// ===end====
    });
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
    runBackgroundTask([self, ips] {
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
    });
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

// =====wjy====
    QString errorMessage;
    const QString publicKey = platform::PortableOpenSshManager::instance().clientPublicKey(&errorMessage);
    if (publicKey.isEmpty()) {
        QMessageBox messageBox(
            QMessageBox::Warning,
            QString(),
            errorMessage.isEmpty()
                ? QStringLiteral("无法读取本机远程终端公钥。")
                : errorMessage,
            QMessageBox::NoButton,
            this);
        messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
        messageBox.exec();
        return; // wjy: 本机没有可用公钥时不启动 ssh.exe，避免弹出的终端窗口认证失败后立即关闭。
    }

    if (!platform::DeviceCommandService::authorizeTerminalKey(device.ip, publicKey, &errorMessage)) {
        QMessageBox messageBox(
            QMessageBox::Warning,
            QString(),
            errorMessage.isEmpty()
                ? QStringLiteral("无法在目标设备登记远程终端密钥。")
                : QStringLiteral("无法在目标设备登记远程终端密钥：%1").arg(errorMessage),
            QMessageBox::NoButton,
            this);
        messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
        messageBox.exec();
        return; // wjy: 目标设备未更新、命令端口不可达或授权写入失败时提前提示，不再让 SSH 黑窗一闪而过。
    }

    if (platform::PortableOpenSshManager::instance().openTerminal(device.ip, loginUser, &errorMessage)) {
        return;
    }
// ===end====

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

void DeviceGrid::executeCurrentDeviceScriptFolder(const QString& scriptFolderPath)
{
// =====wjy====
    executeDeviceScriptFolder(m_selectedDeviceIndex, scriptFolderPath, true); // wjy: 设备右键和终端执行按钮仍然按当前详情设备执行脚本。
// ===end====
}

bool DeviceGrid::executeDeviceScriptFolder(int deviceIndex, const QString& scriptFolderPath, bool showMessages)
{
// =====wjy====
    if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
        return false;
    }

    const QFileInfo entryScript = scriptEntryFile(scriptFolderPath);
    if (!entryScript.exists()) {
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Information,
                QString(),
                QString::fromUtf8("无可用脚本"),
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }
        return false; // wjy: 选中的文件夹没有 bat/cmd/ps1/py/exe 入口时只提示，不创建 work 也不发远程命令。
    }

    if (scriptRunCommandForFile(entryScript).trimmed().isEmpty()) {
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Information,
                QString(),
                QString::fromUtf8("无可用脚本"),
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }
        return false;
    }

    const DeviceEntry device = g_devices.at(deviceIndex);
    const QString targetIp = device.ip.trimmed();
    const ScriptUiState existingState = m_scriptUiStates.value(targetIp);
    if (existingState.outputRunning) {
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Information,
                QString(),
                QString::fromUtf8("该设备已有脚本正在执行。"),
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }
        return false; // wjy: 分组批量执行时跳过正在运行脚本的设备，避免同一设备堆出多套 conhost/cmd/python 进程。
    }
    const QString loginUser = platform::DeviceStatusService::terminalUser(device.ip);
    if (loginUser.isEmpty()) {
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Warning,
                QString(),
                QString::fromUtf8("无法获取目标设备终端用户。"),
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }
        return false;
    }

    QString errorMessage;
    const QString publicKey = platform::PortableOpenSshManager::instance().clientPublicKey(&errorMessage);
    if (publicKey.isEmpty()
        || !platform::DeviceCommandService::authorizeTerminalKey(device.ip, publicKey, &errorMessage)) {
        if (showMessages) {
            QMessageBox messageBox(
                QMessageBox::Warning,
                QString(),
                errorMessage.isEmpty()
                    ? QString::fromUtf8("无法建立目标设备脚本执行授权。")
                    : errorMessage,
                QMessageBox::NoButton,
                this);
            messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
            messageBox.exec();
        }
        return false; // wjy: 脚本执行复用远程终端密钥授权，目标设备未准备好时提前提示。
    }

    const QString sourcePath = QDir::toNativeSeparators(QFileInfo(scriptFolderPath).absoluteFilePath());
    const QString targetName = deviceDisplayName(device);
    const QString scriptName = entryScript.fileName();
    const QString scriptWorkName = entryScript.completeBaseName();
    const bool targetIsCurrent = currentScriptUiDeviceIp() == targetIp;
    ScriptUiState state;
    state.outputVisible = true;
    state.outputRunning = true;
    state.outputFailed = false;
    state.outputScrollOffset = 0;
    state.outputAutoScroll = true;
    state.outputDirty = false;
    state.outputFilePath = scriptOutputTempFilePath();
    state.cancelRequested = std::make_shared<std::atomic_bool>(false);
    state.outputTitle = QString::fromUtf8("%1 - %2").arg(targetName, scriptName);
    state.outputText = QString::fromUtf8("$ 执行脚本 %1\n目标设备: %2\n状态: 正在复制并执行...\n")
        .arg(scriptName, targetName); // wjy: 点击脚本后立即显示右侧终端面板，让用户知道远端任务已经开始。
    state.lastScriptFolderPath = scriptFolderPath;
    state.editorVisible = true;
    state.editorLoading = true;
    state.editorSaving = false;
    state.editorTitle = QString::fromUtf8("本地文件");
    state.editorRemotePath.clear();
    state.editorDeviceIp = targetIp;
    state.editorLoginUser = loginUser;
    state.editorWorkName = scriptWorkName;
    state.editorText = QString::fromUtf8("正在等待脚本文件复制到目标设备本地 work 目录...");
    state.editorModified = false;
    writeScriptOutputFile(state.outputFilePath, state.outputText, QIODevice::Truncate);
    m_scriptUiStates.insert(targetIp, state); // wjy: 指定设备执行脚本时先写入对应 IP 状态，分组批量执行不会抢当前设备 UI。
    if (targetIsCurrent) {
        m_lastScriptFolderPath = state.lastScriptFolderPath;
        m_scriptOutputVisible = state.outputVisible;
        m_scriptOutputRunning = state.outputRunning;
        m_scriptOutputFailed = state.outputFailed;
        m_scriptOutputScrollOffset = state.outputScrollOffset;
        m_scriptOutputAutoScroll = state.outputAutoScroll;
        m_scriptOutputDirty = state.outputDirty;
        m_scriptOutputFilePath = state.outputFilePath;
        m_scriptCancelRequested = state.cancelRequested;
        m_scriptOutputTitle = state.outputTitle;
        m_scriptOutputText = state.outputText;
        m_scriptEditorVisible = state.editorVisible;
        m_scriptEditorLoading = state.editorLoading;
        m_scriptEditorSaving = state.editorSaving;
        m_scriptEditorTitle = state.editorTitle;
        m_scriptEditorRemotePath = state.editorRemotePath;
        m_scriptEditorDeviceIp = state.editorDeviceIp;
        m_scriptEditorLoginUser = state.editorLoginUser;
        m_scriptEditorWorkName = state.editorWorkName;
        if (m_scriptFileEdit) {
            m_scriptFileEdit->setPlainText(state.editorText);
            m_scriptFileEdit->document()->setModified(false); // wjy: 新脚本启动时重置编辑器状态，后续只编辑目标设备本地 work 里的副本。
        }
        updateScriptFileEditorControls();
        update();
    }
    QTimer::singleShot(900, this, [this, deviceIp = device.ip, loginUser, scriptWorkName] {
        const ScriptUiState state = m_scriptUiStates.value(deviceIp.trimmed());
        if (state.editorWorkName == scriptWorkName) {
            loadScriptFileEditor(deviceIp, loginUser, scriptWorkName); // wjy: 复制开始后短暂延迟读取，让编辑框尽快显示 work 子目录里的 json/txt。
        }
    });

    const QString remotePowerShellScript = QStringLiteral(R"($ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONIOENCODING = 'utf-8'
$env:PYTHONUTF8 = '1'
$fsremoteExe = Get-Process FSRemote -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Path
if ([string]::IsNullOrWhiteSpace($fsremoteExe)) {
    Write-Error 'cannot locate FSRemote.exe on target device'
    exit 9009
}
$fsremoteDir = Split-Path -Parent $fsremoteExe
$workRoot = Join-Path $fsremoteDir 'work'
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
$scriptWorkName = '%4'
$work = Join-Path $workRoot $scriptWorkName
$workAlreadyExists = Test-Path -LiteralPath $work
if (-not $workAlreadyExists) {
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $source = '%1'
    $log = Join-Path $work 'fsremote_robocopy.log'
    & robocopy $source $work /E /R:1 /W:1 /NFL /NDL /NJH /NJS "/LOG:$log" | Out-Null
    $copyExit = $LASTEXITCODE
    if ($copyExit -ge 8) {
        if (Test-Path -LiteralPath $log) {
            Get-Content -LiteralPath $log -Raw
        }
        exit $copyExit
    }
} else {
    Write-Output ('FSRemote reuse existing work folder: ' + $work)
}
Set-Location -LiteralPath $work
$entry = Join-Path $work '%2'
if (-not (Test-Path -LiteralPath $entry)) {
    Write-Error ('entry script missing in local work folder: ' + $entry)
    exit 9011
}
$suffix = '%3'
$runLog = Join-Path $work 'fsremote_script_run.log'
$exitCodeFile = Join-Path $work 'fsremote_script_exit_code.txt'
$finishedFile = Join-Path $work 'fsremote_script_finished.txt'
$controllerPidFile = Join-Path $work 'fsremote_script_controller_pid.txt'
$stopRequestFile = Join-Path $work 'fsremote_script_stop_requested.txt'
Remove-Item -LiteralPath $stopRequestFile -Force -ErrorAction SilentlyContinue
('PID: ' + $PID) | Set-Content -LiteralPath $controllerPidFile -Encoding UTF8
@(
    'FSRemote script execution started'
    ('Time: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
    ('Entry: ' + $entry)
    ('Type: ' + $suffix)
    ''
) | Set-Content -LiteralPath $runLog -Encoding UTF8
$scriptExit = 0
switch ($suffix) {
    'py' { & python -u $entry *>&1 | Tee-Object -FilePath $runLog -Append; $scriptExit = $LASTEXITCODE; break }
    'ps1' { & powershell -NoProfile -ExecutionPolicy Bypass -File $entry *>&1 | Tee-Object -FilePath $runLog -Append; $scriptExit = $LASTEXITCODE; break }
    'bat' { & cmd.exe /d /c ('call "' + $entry + '"') *>&1 | Tee-Object -FilePath $runLog -Append; $scriptExit = $LASTEXITCODE; break }
    'cmd' { & cmd.exe /d /c ('call "' + $entry + '"') *>&1 | Tee-Object -FilePath $runLog -Append; $scriptExit = $LASTEXITCODE; break }
    'exe' { & $entry *>&1 | Tee-Object -FilePath $runLog -Append; $scriptExit = $LASTEXITCODE; break }
    default { Write-Error ('unsupported script type: ' + $suffix); exit 9010 }
}
if ($null -eq $scriptExit) {
    $scriptExit = 0
}
('ExitCode: ' + $scriptExit) | Set-Content -LiteralPath $exitCodeFile -Encoding UTF8
('Finished: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) | Set-Content -LiteralPath $finishedFile -Encoding UTF8
('ExitCode: ' + $scriptExit) | Add-Content -LiteralPath $runLog -Encoding UTF8
Remove-Item -LiteralPath $controllerPidFile -Force -ErrorAction SilentlyContinue
Get-Content -LiteralPath $runLog -Tail 80
exit $scriptExit
)").arg(
        escapedPowerShellSingleQuoted(sourcePath),
        escapedPowerShellSingleQuoted(scriptName),
        escapedPowerShellSingleQuoted(entryScript.suffix().toLower()),
        escapedPowerShellSingleQuoted(scriptWorkName));
    const QStringList commands = {
        QStringLiteral("powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand %1 & exit %ERRORLEVEL%")
            .arg(powerShellEncodedCommand(remotePowerShellScript)),
    }; // wjy: 远端复制/执行脚本整体通过 EncodedCommand 传输，避免中文共享路径经过 cmd 输入流后乱码。

    QPointer<DeviceGrid> self(this);
    runBackgroundTask([self, deviceIp = device.ip, loginUser, commands, targetName, scriptName, scriptWorkName, outputFilePath = state.outputFilePath, cancelRequested = state.cancelRequested] {
        QString outputText;
        QString runError;
        const bool ok = platform::PortableOpenSshManager::instance().runRemoteCommands(
            deviceIp,
            loginUser,
            commands,
            &outputText,
            &runError,
            0,
            [self, deviceIp, outputFilePath](const QString& chunk) {
                if (!self || chunk.isEmpty()) {
                    return;
                }
                writeScriptOutputFile(outputFilePath, stripTerminalControlSequences(chunk), QIODevice::Append);
                QMetaObject::invokeMethod(self, [self, deviceIp, outputFilePath] {
                    if (!self) {
                        return;
                    }
                    DeviceGrid* grid = self.data();
                    const QString targetIp = deviceIp.trimmed();
                    ScriptUiState state = grid->m_scriptUiStates.value(targetIp);
                    if (state.outputFilePath != outputFilePath) {
                        return;
                    }
                    state.outputDirty = true;
                    grid->m_scriptUiStates.insert(targetIp, state); // wjy: 输出分片先标记到目标设备状态，切到其它设备时不触发当前终端刷新。
                    const bool targetIsCurrent = grid->currentScriptUiDeviceIp() == targetIp;
                    if (targetIsCurrent) {
                        grid->m_scriptOutputDirty = true;
                    }
                    if (targetIsCurrent && !grid->m_scriptOutputFlushTimer->isActive()) {
                        grid->m_scriptOutputFlushTimer->start();
                    }
                }, Qt::QueuedConnection);
            },
            [cancelRequested] {
                return cancelRequested && cancelRequested->load();
            });

        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(self, [self, ok, deviceIp, loginUser, targetName, scriptName, scriptWorkName, outputText, runError, outputFilePath, cancelRequested] {
            if (!self) {
                return;
            }

            DeviceGrid* grid = self.data();
            const QString targetIp = deviceIp.trimmed();
            ScriptUiState state = grid->m_scriptUiStates.value(targetIp);
            if (state.outputFilePath != outputFilePath) {
                return;
            }
            const QString resultText = runError.trimmed().isEmpty() ? outputText.trimmed() : runError.trimmed();
            const bool canceled = cancelRequested && cancelRequested->load();
            writeScriptOutputFile(
                outputFilePath,
                QString::fromUtf8("\n状态: %1%2\n")
                    .arg(
                        canceled ? QString::fromUtf8("已停止") : (ok ? QString::fromUtf8("已完成") : QString::fromUtf8("执行失败")),
                        canceled
                            ? QString()
                            : ((!ok && !resultText.isEmpty()) ? QString::fromUtf8("\n%1").arg(resultText) : QString())),
                QIODevice::Append);
            state.outputVisible = true;
            state.outputRunning = false;
            state.outputFailed = !ok && !canceled;
            state.outputScrollOffset = 0;
            state.outputAutoScroll = true;
            state.outputDirty = false;
            state.outputTitle = QString::fromUtf8("%1 - %2").arg(targetName, scriptName);
            state.outputText = stripTerminalControlSequences(readScriptOutputFileTail(outputFilePath));
            grid->m_scriptUiStates.insert(targetIp, state); // wjy: 结束状态写回目标设备，后台跑完后切回该设备能看到已完成/失败/停止。

            const bool targetIsCurrent = grid->currentScriptUiDeviceIp() == targetIp;
            if (targetIsCurrent) {
                grid->m_scriptOutputVisible = state.outputVisible;
                grid->m_scriptOutputRunning = state.outputRunning;
                grid->m_scriptOutputFailed = state.outputFailed;
                grid->m_scriptOutputScrollOffset = state.outputScrollOffset;
                grid->m_scriptOutputAutoScroll = state.outputAutoScroll;
                grid->m_scriptOutputDirty = state.outputDirty;
                grid->m_scriptOutputTitle = state.outputTitle;
                grid->m_scriptOutputText = state.outputText;
                grid->update(scriptTerminalPanelRect().toAlignedRect().adjusted(-2, -2, 2, 2));
            }
            const bool editorModified = targetIsCurrent
                ? (grid->m_scriptFileEdit && grid->m_scriptFileEdit->document()->isModified())
                : state.editorModified;
            if (!editorModified) {
                grid->loadScriptFileEditor(deviceIp, loginUser, scriptWorkName); // wjy: 脚本结束后再刷新一次，保证首次复制较慢时编辑器也能拿到本地 json/txt。
            }
        }, Qt::QueuedConnection);
    });
    return true;
// ===end====
}

void DeviceGrid::executeDeviceGroupScriptFolder(int groupIndex, const QString& scriptFolderPath)
{
// =====wjy====
    if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return;
    }

    const QFileInfo entryScript = scriptEntryFile(scriptFolderPath);
    if (!entryScript.exists() || scriptRunCommandForFile(entryScript).trimmed().isEmpty()) {
        QMessageBox messageBox(
            QMessageBox::Information,
            QString(),
            QString::fromUtf8("无可用脚本"),
            QMessageBox::NoButton,
            this);
        messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
        messageBox.exec();
        return; // wjy: 分组批量执行前先校验一次脚本入口，避免组内每台设备重复弹窗。
    }

    const QString groupName = g_deviceGroupNames.at(groupIndex).trimmed();
    QVector<int> groupDeviceIndexes;
    for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        if (g_devices.at(deviceIndex).group.trimmed() == groupName) {
            groupDeviceIndexes.append(deviceIndex); // wjy: 以真实分组名收集设备下标，保证菜单行号变化不影响批量目标。
        }
    }

    int startedCount = 0;
    for (int deviceIndex : groupDeviceIndexes) {
        if (executeDeviceScriptFolder(deviceIndex, scriptFolderPath, false)) {
            ++startedCount; // wjy: 每台设备各自创建 SSH 任务和 UI 状态，互不共享输出文件或停止标志。
        }
    }

    if (startedCount <= 0) {
        QMessageBox messageBox(
            QMessageBox::Warning,
            QString(),
            QString::fromUtf8("分组内没有可执行设备。"),
            QMessageBox::NoButton,
            this);
        messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
        messageBox.exec();
    } else {
        update(); // wjy: 如果当前详情设备属于该分组，批量启动后立即刷新它的脚本面板。
    }
// ===end====
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

void DeviceGrid::openDeviceGroupTiledWindows(int groupIndex)
{
// =====wjy====
    if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return; // wjy: 分组下标无效时直接返回，避免右键菜单持有的旧下标导致越界。
    }

    const QString groupName = g_deviceGroupNames.at(groupIndex).trimmed(); // wjy: 用分组名筛选设备，和 devices.json 里的 group 字段保持一致。
    if (groupName.isEmpty()) {
        return; // wjy: 空分组名没有稳定匹配依据，不打开任何窗口。
    }

    for (const QPointer<RemoteDesktopWindow>& tiledWindow : m_tiledRemoteWindows) {
        if (tiledWindow && !tiledWindow->isClosingConnection()) {
            tiledWindow->close(); // wjy: 再次点击设备平铺时先关闭上一批平铺窗口，避免重复创建叠加的新窗口。
        }
    }
    m_tiledRemoteWindows.clear(); // wjy: 旧窗口已进入关闭流程，后续只记录本次重新平铺创建的新窗口。

    QVector<int> groupDeviceIndexes; // wjy: 保存这个分组里的真实设备下标，后面创建窗口时要用真实设备名和 IP。
    for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        if (g_devices.at(deviceIndex).group.trimmed() == groupName) {
            groupDeviceIndexes.append(deviceIndex); // wjy: 只收集属于当前分组的设备，无分组设备和其它分组设备不参与平铺。
        }
    }
    if (groupDeviceIndexes.isEmpty()) {
        return; // wjy: 空分组暂时不弹提示，点击设备平铺没有可打开目标就静默返回。
    }

    QScreen* screen = window() ? window()->screen() : QGuiApplication::primaryScreen(); // wjy: 优先使用主窗口所在屏幕，多屏时窗口会铺在当前程序所在显示器。
    if (!screen) {
        screen = QGuiApplication::primaryScreen(); // wjy: 防御性兜底，避免极端情况下没有窗口屏幕对象。
    }
    const QRect availableRect = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720); // wjy: 获取去掉任务栏后的可用屏幕区域，用它计算平铺位置。
    const int deviceCount = groupDeviceIndexes.size();
    const int gridSize = qMax(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(deviceCount))))); // wjy: 设备数量开方取上整，形成 1x1、2x2、3x3 这种方阵网格。
    const int columnCount = gridSize; // wjy: 列数等于方阵边长，保证布局是 2x2、3x3 这类平铺。
    const int rowCount = gridSize; // wjy: 行数也等于方阵边长，设备不足时只是后面的格子空着。
    const int tileWidth = qMax(320, availableRect.width() / columnCount); // wjy: 每个远程桌面窗口的宽度按列数均分，最低保留 320 避免窗口太窄。
    const int tileHeight = qMax(240, availableRect.height() / rowCount); // wjy: 每个远程桌面窗口的高度按行数均分，最低保留 240 避免窗口太矮。

    for (int tileIndex = 0; tileIndex < groupDeviceIndexes.size(); ++tileIndex) {
        const int deviceIndex = groupDeviceIndexes.at(tileIndex); // wjy: 当前要打开的真实设备下标。
        if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
            continue; // wjy: 防御性跳过异常下标。
        }

        const int row = tileIndex / columnCount; // wjy: 计算当前窗口位于第几行。
        const int column = tileIndex % columnCount; // wjy: 计算当前窗口位于第几列。
        QRect targetRect(
            availableRect.x() + column * tileWidth,
            availableRect.y() + row * tileHeight,
            tileWidth,
            tileHeight);
        if (column == columnCount - 1) {
            targetRect.setRight(availableRect.right()); // wjy: 最后一列贴齐屏幕右边缘，吸收整除误差。
        }
        if (row == rowCount - 1) {
            targetRect.setBottom(availableRect.bottom()); // wjy: 最后一行贴齐屏幕底边缘，吸收整除误差。
        }

        auto* remoteWindow = new RemoteDesktopWindow(
            deviceDisplayName(g_devices.at(deviceIndex)),
            g_devices.at(deviceIndex).ip); // wjy: 复用现有远程桌面窗口，每台设备各自启动自己的 viewer 连接。
        m_tiledRemoteWindows.append(remoteWindow); // wjy: 记录本次平铺创建的窗口，下一次设备平铺前统一关闭并重排。
        remoteWindow->setMinimumSize(240, 180); // wjy: 平铺模式允许窗口小于普通远程桌面的默认最小尺寸，确保 3x3 时能尽量塞进屏幕可用区域。
        remoteWindow->setGeometry(targetRect); // wjy: 按网格设置窗口位置和大小，形成 2x2、3x3 等平铺效果。
        remoteWindow->show();
    }
// ===end====
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

    const QString oldName = g_devices.at(m_selectedDeviceIndex).name.trimmed(); // wjy: 保存旧名字，远端改名命令明确失败时用于回滚本地显示。
    if (newName == oldName) {
        return; // wjy: 名字没有变化时不写 devices.json，也不发送远端改名命令。
    }

    const QString targetIp = g_devices.at(m_selectedDeviceIndex).ip.trimmed();
    const bool shouldRenameRemote = !targetIp.isEmpty()
        && devicePresenceForIndex(m_selectedDeviceIndex) != platform::DevicePresenceState::Offline; // wjy: 离线设备只改本地显示名，在线设备才尝试同步远端电脑名。
    g_devices[m_selectedDeviceIndex].name = newName;
    saveDevices();
    m_currentDeviceName = newName;
    m_previousDeviceName = newName;
    update();

    if (shouldRenameRemote) {
        m_pendingRemoteRenameNames.insert(targetIp, newName); // wjy: Windows 改电脑名通常要重启才从状态服务返回新名，先阻止自动刷新用旧名覆盖。
        QPointer<DeviceGrid> self(this);
        runBackgroundTask([self, targetIp, oldName, newName] {
            QString errorMessage;
            const bool renamed = platform::DeviceCommandService::renameDevice(targetIp, newName, &errorMessage);
            qWarning().noquote() << QStringLiteral("[rename-device] remote ip=%1 renamed=%2 error=%3")
                .arg(targetIp)
                .arg(renamed)
                .arg(errorMessage.trimmed()); // wjy: 本地名称先保存，远端电脑名修改结果写日志；Windows 通常需要重启后完全生效。
            if (!self) {
                return;
            }
            if (renamed) {
                return; // wjy: 远端已接受改名请求，等待目标系统重启后状态服务上报新电脑名。
            }
            QMetaObject::invokeMethod(self, [self, targetIp, oldName, newName, errorMessage] {
                if (!self) {
                    return;
                }

                DeviceGrid* grid = self.data();
                if (grid->m_pendingRemoteRenameNames.value(targetIp).trimmed() == newName) {
                    grid->m_pendingRemoteRenameNames.remove(targetIp); // wjy: 命令明确失败后取消待生效保护，让后续刷新恢复正常同步。
                }

                bool reverted = false;
                for (DeviceEntry& device : g_devices) {
                    if (device.ip.trimmed() == targetIp && device.name.trimmed() == newName) {
                        device.name = oldName; // wjy: 远端没有接受改名时回滚本地名，避免界面显示一个不会在目标机生效的名字。
                        reverted = true;
                        break;
                    }
                }
                if (reverted) {
                    saveDevices();
                    if (grid->m_selectedDeviceIndex >= 0 && grid->m_selectedDeviceIndex < g_devices.size()) {
                        grid->m_currentDeviceName = deviceDisplayName(g_devices.at(grid->m_selectedDeviceIndex));
                        grid->m_previousDeviceName = grid->m_currentDeviceName;
                    }
                    grid->update();
                }

                QMessageBox messageBox(
                    QMessageBox::Warning,
                    QString(),
                    errorMessage.trimmed().isEmpty()
                        ? QString::fromUtf8("目标设备重命名失败。")
                        : QString::fromUtf8("目标设备重命名失败：%1").arg(errorMessage.trimmed()),
                    QMessageBox::NoButton,
                    grid);
                messageBox.addButton(zh("\xE7\x9F\xA5\xE9\x81\x93\xE4\xBA\x86"), QMessageBox::AcceptRole);
                messageBox.exec();
            }, Qt::QueuedConnection);
        });
    }
}

void DeviceGrid::deleteCurrentDevice()
{
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= g_devices.size()) {
        return;
    }

    const QString removedIp = g_devices.at(m_selectedDeviceIndex).ip.trimmed();
    saveCurrentScriptUiState(); // wjy: 删除当前设备前先收拢编辑框未保存文本和终端状态，随后按 IP 清理缓存。
    g_devices.removeAt(m_selectedDeviceIndex);
    saveDevices();
    m_deviceStatuses.remove(removedIp);
    m_poweringOnDeviceIps.remove(removedIp);
    m_poweringOnStartedAtMs.remove(removedIp);
    m_scriptUiStates.remove(removedIp); // wjy: 被删除设备的脚本 UI 不再保留，避免后续同 IP 之外的设备误用旧状态。
        if (g_devices.isEmpty()) {
            m_selectedDeviceIndex = 0;
            m_previousDeviceIndex = 0;
            m_selectedDeviceIndexes.clear();
            m_draggingDeviceIndexes.clear();
            m_selectionAnchorDeviceIndex = -1;
            m_currentDeviceName.clear();
            m_previousDeviceName.clear();
            m_remoteAssistSelected = true;
            updateAddDeviceControls();
            updateLocalInfoControls();
            updateSettingsControls();
            loadScriptUiStateForDevice(QString());
            update();
            return;
        }

        const int nextIndex =
            qMin(m_selectedDeviceIndex,
                 g_devices.size() - 1);

        m_selectedDeviceIndex = nextIndex;
        m_previousDeviceIndex = nextIndex;

        m_selectedDeviceIndexes.clear();
        m_selectedDeviceIndexes.insert(nextIndex);

        m_draggingDeviceIndexes.clear();

        m_selectionAnchorDeviceIndex =
            nextIndex;
    m_currentDeviceName = deviceDisplayName(g_devices.at(nextIndex));
    m_previousDeviceName = m_currentDeviceName;
    loadScriptUiStateForDevice(currentScriptUiDeviceIp()); // wjy: 删除后详情切到下一台设备时，下方脚本 UI 同步切到下一台设备。
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
    runBackgroundTask([self, targetIp, mac, candidates = std::move(candidates)] {
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
    });
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
    runBackgroundTask([self] {
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
    });
}

void DeviceGrid::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

// =====wjy====
    static int s_paintLogCount = 0; // wjy: 只记录前几次绘制，避免 paintEvent 高频触发导致日志文件过大。
    const int paintLogIndex = s_paintLogCount++; // wjy: 给每次被记录的绘制分配编号，方便判断 begin/end 是否配对。
    const bool shouldLogPaint = paintLogIndex < 5; // wjy: 只跟踪启动阶段最关键的前 5 次绘制。
    if (shouldLogPaint) {
        writeDeviceGridStartupLog(QStringLiteral("[wjy-paint] begin index=%1").arg(paintLogIndex)); // wjy: 如果有 begin 没有 end，说明崩在 paintEvent 内部。
    }
// ===end====

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

// =====wjy====
    m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset, maxDeviceListScrollOffset()); // wjy: 每次绘制前校正滚动偏移，避免删除设备或折叠分组后停在无效滚动位置。
    const QVector<DeviceListRow> deviceRows = visibleDeviceRows(); // wjy: 左侧列表改为按“真实可见行”绘制，设备和分组的顺序由分组数据统一决定。
    const QSet<int> badges = deviceBadgeIndexes(); // wjy: 远程控制角标仍然按真实设备下标判断，避免分组行影响设备下标。
    const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: “我的设备”内部滚动视口，设备和分组只能画在这个范围内。
    if (m_deviceGroupExpanded) {
        painter.save();
        painter.setClipRect(deviceListClip); // wjy: 内容超出视口时直接裁掉，避免设备行压到下面的设备管理区域。
        for (int rowIndex = 0; rowIndex < deviceRows.size(); ++rowIndex) { // wjy: 每一行都来自 visibleDeviceRows，包含无分组设备、分组行、分组内设备。
            const DeviceListRow& row = deviceRows.at(rowIndex); // wjy: row 保存这一行到底是设备还是分组，以及对应的真实下标。
            const QRect rowRect = scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset); // wjy: 行坐标减去滚动偏移，得到当前真正显示在屏幕上的位置。
            const int rowY = rowRect.y();
            if (!rowRect.intersects(deviceListClip)) {
                continue; // wjy: 当前行完全滚出视口时跳过绘制，节省绘制并保证不越界显示。
            }

            if (row.type == DeviceListRow::Type::Device) {
                const int deviceIndex = row.deviceIndex; // wjy: 设备行使用真实设备下标读取名称、在线状态和选中状态。
                if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
                    continue; // wjy: 防御性跳过异常设备行，避免手动编辑 JSON 后出现越界。
                }

                const bool deviceSelected =
                    m_selectedDeviceIndexes.contains(deviceIndex);

                if (!m_remoteAssistSelected
                    && !m_localInfoSelected
                    && deviceSelected) {

                    painter.setPen(Qt::NoPen);

                    // 当前右侧详情对应的主设备颜色稍深，
                    // 其他批量选中的设备颜色稍浅。
                    painter.setBrush(
                        deviceIndex == m_selectedDeviceIndex
                            ? QColor(QStringLiteral("#BFD3F7"))
                            : QColor(QStringLiteral("#D7E4FA")));

                    painter.drawRoundedRect(
                        QRectF(rowRect),
                        5,
                        5);

                    // 蓝色竖条只画在主设备旁边，
                    // 用来表示右侧详情正在显示哪台设备。
                    if (deviceIndex == m_selectedDeviceIndex) {
                        painter.setBrush(QColor(QStringLiteral("#3A7BFC")));
                        painter.drawRoundedRect(QRectF(4, rowY + 10, 4, 17),2,2);
                    }
                }

                const bool deviceInsideGroup = row.groupIndex >= 0; // wjy: 分组内设备稍微右移，视觉上表示它属于上方分组。
                const int statusDotX = 200; // wjy: 根部设备保持旧坐标，分组内设备整体缩进。 这是原点坐标
                const int iconX = deviceInsideGroup ? 58 : 42;
                const int textX = deviceInsideGroup ? 90 : 74;
                const int textWidth = deviceInsideGroup ? 96 : 116; // wjy: 缩进后收窄文字区域，避免碰到右侧角标。

                painter.setPen(Qt::NoPen);
                painter.setBrush(deviceStatusDotColor(devicePresenceForIndex(deviceIndex))); // wjy: 在线圆点按真实设备状态绘制，不受视觉行号影响。
                painter.drawEllipse(QRectF(statusDotX, rowY + 11, 13, 13));
                drawDeviceTileIcon(painter, iconX, rowY + 9, 20);

                painter.setFont(textFont);
                painter.setPen(QColor(QStringLiteral("#111827")));
                painter.drawText(QRectF(textX, rowY + 7, textWidth, 22), Qt::AlignVCenter | Qt::AlignLeft, deviceDisplayName(g_devices.at(deviceIndex))); // wjy: 显示真实设备名，分组排序变化不会改错名字。
                if (badges.contains(deviceIndex)) {
                    drawRemoteBadge(painter, 202, rowY + 12); // wjy: 角标也使用真实设备下标，避免分组行插入后角标错位。
                }
                continue;
            }

            if (row.type == DeviceListRow::Type::Group) {
                const int groupIndex = row.groupIndex; // wjy: 分组行使用真实分组下标读取名称和展开状态。
                if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
                    continue; // wjy: 防御性跳过异常分组行。
                }

                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(QStringLiteral("#1E3A5F"))); // wjy: 分组行使用深蓝背景，和普通设备行区分开。
                painter.drawRoundedRect(QRectF(rowRect), 5, 5);
                drawUiIcon(
                    painter,
                    QRect(203, rowY + 8, 24, 20),
                    deviceGroupExpandedForIndex(groupIndex) ? QStringLiteral("chevron_up.svg") : QStringLiteral("chevron_down.svg")); // wjy: 根据分组展开状态绘制上箭头或下箭头。

                painter.setFont(textFont);
                painter.setPen(QColor(QStringLiteral("#FFFFFF"))); //分组字体颜色
                if (m_renamingDeviceGroupIndex != groupIndex) {
                    painter.drawText(QRectF(58, rowY + 7, 132, 22), Qt::AlignVCenter | Qt::AlignLeft, g_deviceGroupNames.at(groupIndex)); // wjy: 分组正在原地重命名时不画底层文字，避免和输入框重叠。
                }
            }
        }
        painter.restore();

        const int maxScrollOffset = maxDeviceListScrollOffset(); // wjy: 大于 0 表示内容高度超过视口，需要显示滚动条。
        if (maxScrollOffset > 0 && deviceListClip.height() > 0) {
            const QRect scrollTrack(deviceListClip.right() - 6, deviceListClip.y() + 5, 3, qMax(1, deviceListClip.height() - 10)); // wjy: 滚动条贴在左侧列表右边缘，不占用设备文字区域。
            const int thumbHeight = qMax(28, scrollTrack.height() * deviceListClip.height() / qMax(1, visibleDeviceListContentHeight())); // wjy: 滑块高度按可见比例计算，太短时固定最小高度方便观察。
            const int thumbTravel = qMax(0, scrollTrack.height() - thumbHeight);
            const int thumbY = scrollTrack.y() + (thumbTravel * m_deviceListScrollOffset / maxScrollOffset); // wjy: 当前滚动偏移映射成滑块在轨道中的位置。
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(172, 184, 198, 120));
            painter.drawRoundedRect(QRectF(scrollTrack.x(), thumbY, scrollTrack.width(), thumbHeight), 1.5, 1.5);
        }
    }
// ===end====

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

    if (!m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && m_scriptEditorVisible) {
        drawScriptFileEditorPanel(
            painter,
            m_scriptEditorTitle,
            m_scriptEditorLoading,
            !m_scriptEditorRemotePath.trimmed().isEmpty()); // wjy: 左侧白色编辑面板只在设备详情页显示，避免盖住设置/新增设备页面。
    }

    if (!m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && m_scriptOutputVisible) {
        drawScriptTerminalPanel(
            painter,
            m_scriptOutputTitle,
            m_scriptOutputText,
            m_scriptOutputRunning,
            m_scriptOutputFailed,
            m_scriptOutputScrollOffset); // wjy: Script output lives in the right-side blank area after the device detail card is painted.
    }
    painter.restore();
    updateScriptFileEditorControls(); // wjy: 绘制完成后同步真实 QTextEdit/QPushButton 的显隐和层级，保证页面切换时不会残留在其它页面。

// =====wjy====
    if (m_draggingDevice
        && !m_draggingDeviceIndexes.isEmpty()
        && m_draggingDeviceIndex >= 0
        && m_draggingDeviceIndex
               < g_devices.size()) {
        const int ghostWidth = 172; // wjy: 虚影宽度略小于左侧列表宽度，像一条被拖起的设备行。
        const int ghostHeight = 36;
        const int ghostX = qBound(8, m_deviceDragCurrentPos.x() - 86, width() - ghostWidth - 8); // wjy: 让虚影围绕鼠标居中，同时限制在窗口范围内。
        const int ghostY = qBound(52, m_deviceDragCurrentPos.y() - 18, height() - ghostHeight - 8); // wjy: 避免虚影跑出窗口顶部或底部。
        const QRectF ghostRect(ghostX, ghostY, ghostWidth, ghostHeight);
        const int draggingDeviceCount =
            m_draggingDeviceIndexes.size();

        const QString ghostName =
            draggingDeviceCount > 1
                ? QString::fromUtf8("%1 台设备")
                      .arg(draggingDeviceCount)
                : deviceDisplayName(
                      g_devices.at(
                          m_draggingDeviceIndex));
        painter.save();
        painter.setOpacity(0.72); // wjy: 半透明效果表示“正在拖动的临时虚影”，不是真实列表行。
        painter.setPen(QPen(QColor(QStringLiteral("#B9C3D0")), 1));
        painter.setBrush(QColor(QStringLiteral("#F7FAFE")));
        painter.drawRoundedRect(ghostRect, 6, 6);
        painter.setPen(Qt::NoPen);
        painter.setBrush(deviceStatusDotColor(devicePresenceForIndex(m_draggingDeviceIndex)));
        painter.drawEllipse(QRectF(ghostX + 18, ghostY + 15, 6, 6));
        drawDeviceTileIcon(painter, ghostX + 34, ghostY + 8, 20);
        painter.setOpacity(0.88); // wjy: 文字和图标比背景稍清楚，拖动时仍能看出是哪台设备。
        painter.setFont(textFont);
        painter.setPen(QColor(QStringLiteral("#111827")));
        painter.drawText(QRectF(ghostX + 66, ghostY + 7, ghostWidth - 78, 22), Qt::AlignVCenter | Qt::AlignLeft, ghostName);
        painter.restore();
    }
// ===end====

// =====wjy====
    if (shouldLogPaint) {
        writeDeviceGridStartupLog(QStringLiteral("[wjy-paint] end index=%1").arg(paintLogIndex)); // wjy: 记录本次绘制完整结束，和 begin 配对判断首次绘制是否正常。
    }
// ===end====
}

void DeviceGrid::startDeviceSwitchAnimation(int newIndex, const QString& newName)
{
    if (newIndex == m_selectedDeviceIndex && newName == m_currentDeviceName) {
        update();
        return;
    }

    saveCurrentScriptUiState(); // wjy: 切换设备前保存当前设备的脚本终端和文件编辑器状态，避免显示串到下一台设备。
    m_previousDeviceIndex = m_selectedDeviceIndex;
    m_previousDeviceName = m_currentDeviceName;
    m_selectedDeviceIndex = newIndex;
    m_currentDeviceName = newName;
    loadScriptUiStateForDevice(currentScriptUiDeviceIp()); // wjy: 新设备详情页加载它自己的脚本 UI；没有历史状态时下方面板保持空白。
    m_detailAnimationProgress = 1.0; // wjy: 用户不需要设备详情从下往上弹出的过渡，切换时直接显示目标设备。
    if (m_detailAnimationTimer) {
        m_detailAnimationTimer->stop(); // wjy: 不再启动详情切换动画，避免 paintEvent 进入滑动绘制分支。
    }
    update();
}

// =====wjy====
void DeviceGrid::pruneHiddenDeviceSelections()
{
    const QVector<DeviceListRow> rows = visibleDeviceRows(); // wjy: 以当前展开/折叠后的可见行作为唯一可信的选择范围。
    QSet<int> visibleDeviceIndexes; // wjy: 保存当前左侧列表里真实可见的设备下标，用来过滤隐藏设备。
    int firstVisibleDeviceIndex = -1; // wjy: 当前主设备被隐藏时，用第一个可见设备作为详情页兜底目标。
    int firstSelectedVisibleDeviceIndex = -1; // wjy: 如果多选里还有可见设备，优先用它作为新的主设备。

    for (const DeviceListRow& row : rows) {
        if (row.type != DeviceListRow::Type::Device
            || row.deviceIndex < 0
            || row.deviceIndex >= g_devices.size()) {
            continue; // wjy: 分组行和异常下标不参与选择集合计算。
        }

        visibleDeviceIndexes.insert(row.deviceIndex); // wjy: 记录当前仍显示在左侧列表里的真实设备。
        if (firstVisibleDeviceIndex < 0) {
            firstVisibleDeviceIndex = row.deviceIndex; // wjy: 保留视觉顺序里的第一台可见设备作为兜底。
        }
        if (firstSelectedVisibleDeviceIndex < 0
            && m_selectedDeviceIndexes.contains(row.deviceIndex)) {
            firstSelectedVisibleDeviceIndex = row.deviceIndex; // wjy: 保留视觉顺序里的第一台仍可见已选设备。
        }
    }

    for (auto it = m_selectedDeviceIndexes.begin(); it != m_selectedDeviceIndexes.end();) {
        if (!visibleDeviceIndexes.contains(*it)) {
            it = m_selectedDeviceIndexes.erase(it); // wjy: 设备被折叠隐藏后，不再保留在多选集合里。
        } else {
            ++it;
        }
    }

    for (auto it = m_draggingDeviceIndexes.begin(); it != m_draggingDeviceIndexes.end();) {
        if (!visibleDeviceIndexes.contains(*it)) {
            it = m_draggingDeviceIndexes.erase(it); // wjy: 隐藏设备也不能留在后续批量拖拽快照里。
        } else {
            ++it;
        }
    }

    if (m_selectionAnchorDeviceIndex >= 0
        && !visibleDeviceIndexes.contains(m_selectionAnchorDeviceIndex)) {
        m_selectionAnchorDeviceIndex = firstSelectedVisibleDeviceIndex; // wjy: Shift 锚点被折叠隐藏时，改成可见选中设备；没有则清空。
    }

    if (m_selectedDeviceIndex >= 0
        && m_selectedDeviceIndex < g_devices.size()
        && visibleDeviceIndexes.contains(m_selectedDeviceIndex)) {
        return; // wjy: 右侧详情主设备仍可见时，只需要完成上面的集合清理。
    }

    const int nextDeviceIndex = firstSelectedVisibleDeviceIndex >= 0
        ? firstSelectedVisibleDeviceIndex
        : firstVisibleDeviceIndex; // wjy: 主设备被隐藏时，优先切到可见选中设备，否则切到第一台可见设备。
    if (nextDeviceIndex < 0) {
        saveCurrentScriptUiState(); // wjy: 没有可见设备前先保存当前脚本 UI，避免折叠分组清空详情时丢掉该设备状态。
        m_selectedDeviceIndexes.clear(); // wjy: 当前没有任何可见设备时，清空左侧选择，避免隐藏设备继续高亮或被拖拽。
        m_draggingDeviceIndexes.clear();
        m_selectionAnchorDeviceIndex = -1;
        loadScriptUiStateForDevice(QString());
        return;
    }

    saveCurrentScriptUiState(); // wjy: 主设备被折叠隐藏时也属于设备切换，需要保存旧设备脚本 UI。
    m_selectedDeviceIndex = nextDeviceIndex; // wjy: 将右侧详情主设备同步到仍可见的设备，避免详情指向折叠隐藏项。
    m_previousDeviceIndex = nextDeviceIndex;
    m_currentDeviceName = deviceDisplayName(g_devices.at(nextDeviceIndex));
    m_previousDeviceName = m_currentDeviceName;
    loadScriptUiStateForDevice(currentScriptUiDeviceIp()); // wjy: 折叠后恢复新主设备自己的脚本 UI，避免沿用被隐藏设备的面板。
    m_selectedDeviceIndexes.insert(nextDeviceIndex); // wjy: 新主设备必须在左侧多选集合里，保证视觉选中态一致。
    if (m_selectionAnchorDeviceIndex < 0) {
        m_selectionAnchorDeviceIndex = nextDeviceIndex; // wjy: 没有可用 Shift 锚点时，用新主设备作为下一次范围选择起点。
    }
    if (m_detailAnimationTimer) {
        m_detailAnimationTimer->stop(); // wjy: 分组折叠引发的主设备兜底切换不播放详情页切换动画，避免隐藏项参与过渡。
    }
}
// ===end====

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

//鼠标时间处理函数
void DeviceGrid::mousePressEvent(QMouseEvent* event)
{
// =====wjy====
    if (event->button() == Qt::LeftButton
        && m_scriptOutputVisible
        && !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && scriptTerminalActionButtonRect().contains(event->position())) {
        if (m_scriptOutputRunning) {
            stopCurrentDeviceScript(); // wjy: 停止时先要求目标设备 taskkill 脚本进程树，再让本地 SSH 执行会话退出。
        } else if (!m_lastScriptFolderPath.trimmed().isEmpty()) {
            executeCurrentDeviceScriptFolder(m_lastScriptFolderPath);
        }
        event->accept();
        return;
    }

    if (m_statusRefreshIntervalEdit
        && m_statusRefreshIntervalEdit->hasFocus()
        && !m_statusRefreshIntervalEdit->geometry().contains(event->pos())) {
        m_statusRefreshIntervalEdit->clearFocus(); // wjy: 设置页是手绘区域，点击空白/开关不会天然抢焦点，这里主动让秒数输入框失焦并触发保存。
        setFocus(Qt::MouseFocusReason); // wjy: 父控件接管焦点，避免输入框清焦后马上继续接收键盘输入。
    }
    if (m_batchSubnetEdit
        && m_batchSubnetEdit->hasFocus()
        && !m_batchSubnetEdit->geometry().contains(event->pos())) {
        m_batchSubnetEdit->clearFocus(); // wjy: 批量新增网段输入框点击设置页空白后主动失焦，行为和自动刷新秒数输入框一致。
        setFocus(Qt::MouseFocusReason);
    }

    if (m_deviceGroupNameEdit
        && m_deviceGroupNameEdit->isVisible()
        && !m_deviceGroupNameEdit->geometry().contains(event->pos())) { // wjy: 点击分组输入框外部时，提交当前名字并关闭输入框。
        finishDeviceGroupRename(true);
    }

    if (event->button() == Qt::RightButton && m_deviceGroupExpanded) {
        const QVector<DeviceListRow> rows = visibleDeviceRows(); // wjy: 设备行右键命中使用当前可见行，保证滚动后菜单出现在真实设备上。
        const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded);
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const DeviceListRow& row = rows.at(rowIndex);
            if (row.type != DeviceListRow::Type::Device) {
                continue; // wjy: 这里专门处理设备行，分组行仍交给下面已有的分组右键菜单。
            }

            const QRect rowRect = scrolledVisibleDeviceRowRect(rowIndex, m_deviceListScrollOffset);
            const QRect hitRect = rowRect.intersected(deviceListClip);
            if (!hitRect.contains(event->pos())) {
                continue;
            }

            const int deviceIndex = row.deviceIndex;
            if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
                continue;
            }

            m_selectedDeviceIndexes.clear();
            m_selectedDeviceIndexes.insert(deviceIndex);
            m_selectionAnchorDeviceIndex = deviceIndex;
            m_settingsSelected = false;
            m_remoteAssistSelected = false;
            m_localInfoSelected = false;
            setDesktopHoverActive(false);
            clearBottomActionHover();
            updateAddDeviceControls();
            updateLocalInfoControls();
            updateSettingsControls();
            startDeviceSwitchAnimation(deviceIndex, deviceDisplayName(g_devices.at(deviceIndex))); // wjy: 右键某台设备时先让右侧详情和后续脚本目标统一到这台设备。

            QMenu menu(this); // wjy: 设备右键菜单样式参照分组右键菜单，使用 Qt 原生级联菜单外观。
            QMenu* scriptMenu = menu.addMenu(QString::fromUtf8("执行脚本")); // wjy: 鼠标悬浮“执行脚本”时展开脚本共享目录第一层文件夹。
            populateScriptFolderMenu(scriptMenu, QString::fromUtf8(kRemoteScriptFolderPath)); // wjy: 递归把共享目录下的文件夹转成多级子菜单，暂不绑定点击执行逻辑。
            // =====wjy====
            const bool offlineOnly = devicePresenceForIndex(m_selectedDeviceIndex) == platform::DevicePresenceState::Offline; // wjy: 设备右键菜单复用“更多”的在线判断，离线设备不显示需要远程连接的操作。
            QAction* terminalAction = nullptr; // wjy: 在线设备才创建终端入口，后面用指针判断用户点了哪一项。
            QAction* shutdownAction = nullptr; // wjy: 在线设备才允许发送关机命令。
            QAction* restartAction = nullptr; // wjy: 在线设备才允许发送重启命令。
            if (!offlineOnly) {
                terminalAction = menu.addAction(menuIcon(QStringLiteral("terminal.svg")), zh("\xE7\xBB\x88\xE7\xAB\xAF")); // wjy: 放在“执行脚本”下面，和底部“更多”菜单保持同一套终端功能。
                shutdownAction = menu.addAction(menuIcon(QStringLiteral("power.svg")), zh("\xE5\x85\xB3\xE6\x9C\xBA")); // wjy: 复用当前设备的远程关机逻辑。
                restartAction = menu.addAction(menuIcon(QStringLiteral("restart.svg")), zh("\xE9\x87\x8D\xE5\x90\xAF")); // wjy: 复用当前设备的远程重启逻辑。
            }
            QAction* renameAction = menu.addAction(menuIcon(QStringLiteral("rename.svg")), zh("\xE9\x87\x8D\xE5\x91\xBD\xE5\x90\x8D")); // wjy: 重命名不依赖在线状态，离线设备也可以改本地记录。
            QAction* deleteAction = menu.addAction(menuIcon(QStringLiteral("delete.svg")), zh("\xE5\x88\xA0\xE9\x99\xA4\xE8\xAE\xBE\xE5\xA4\x87")); // wjy: 删除设备也放进设备右键菜单，和底部“更多”保持一致。
            // ===end====
            const QAction* selectedAction = menu.exec(mapToGlobal(event->pos()));
            if (selectedAction && selectedAction->data().isValid()) {
                executeCurrentDeviceScriptFolder(selectedAction->data().toString()); // wjy: 只有点击具体脚本文件夹才触发复制和远程执行，中间层子菜单只负责展开。
            // =====wjy====
            } else if (selectedAction && selectedAction == terminalAction) {
                openCurrentDeviceTerminal(); // wjy: 设备右键点击“终端”时执行和底部“更多”相同的打开终端流程。
            } else if (selectedAction && selectedAction == shutdownAction) {
                shutdownCurrentDevice(); // wjy: 设备右键点击“关机”时发送当前设备关机命令。
            } else if (selectedAction && selectedAction == restartAction) {
                restartCurrentDevice(); // wjy: 设备右键点击“重启”时发送当前设备重启命令。
            } else if (selectedAction && selectedAction == renameAction) {
                renameCurrentDevice(); // wjy: 设备右键点击“重命名”时打开当前设备重命名弹窗。
            } else if (selectedAction && selectedAction == deleteAction) {
                deleteCurrentDevice(); // wjy: 设备右键点击“删除设备”时复用现有删除当前设备逻辑。
            // ===end====
            }
            event->accept();
            return;
        }
    }

    if (event->button() == Qt::RightButton && m_deviceGroupExpanded) { // wjy: 右键分组行时弹出分组菜单，先于空白区菜单判断，避免误触发“新建分组”。
        const QVector<DeviceListRow> rows = visibleDeviceRows(); // wjy: 分组右键命中也复用当前可见行，滚动后坐标和绘制保持一致。
        const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 只允许右键当前可见视口里的分组行。
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const DeviceListRow& row = rows.at(rowIndex);
            if (row.type != DeviceListRow::Type::Group) {
                continue; // wjy: 设备行暂时不弹出这个分组菜单，只处理分组下拉框本身。
            }

            const QRect rowRect =
                scrolledVisibleDeviceRowRect(
                    rowIndex,
                    m_deviceListScrollOffset);

            const QRect hitRect =
                rowRect.intersected(deviceListClip);

            if (!hitRect.contains(event->pos())) {
                continue;
            }

            QMenu menu(this); // wjy: 创建分组右键菜单，用来放分组相关操作入口。
            QAction* tileDevicesAction = menu.addAction(QString::fromUtf8("设备平铺")); // wjy: 设备平铺菜单项先提供入口，具体平铺逻辑后续再实现。
            QMenu* scriptMenu = menu.addMenu(QString::fromUtf8("执行脚本")); // wjy: 分组右键复用设备右键的脚本目录级联菜单，最终叶子节点批量执行组内设备。
            populateScriptFolderMenu(scriptMenu, QString::fromUtf8(kRemoteScriptFolderPath));
            QAction* stopScriptsAction = menu.addAction(QString::fromUtf8("批量停止")); // wjy: 分组级停止会遍历组内设备，停止所有正在运行的脚本进程树。
            QAction* deleteGroupAction = menu.addAction(QString::fromUtf8("删除分组")); // wjy: 删除分组菜单项先显示出来，后续再补真正删除分组和设备归属处理。
            const QAction* selectedAction = menu.exec(mapToGlobal(event->pos())); // wjy: 在分组行右键位置弹出菜单。
            if (selectedAction == tileDevicesAction) {
                openDeviceGroupTiledWindows(row.groupIndex); // wjy: 点击设备平铺时，打开该分组内所有设备的远程桌面窗口并按屏幕网格排列。
            } else if (selectedAction && selectedAction->data().isValid()) {
                executeDeviceGroupScriptFolder(row.groupIndex, selectedAction->data().toString()); // wjy: 点击脚本叶子目录时，对该分组内所有设备启动同一个脚本。
            } else if (selectedAction == stopScriptsAction) {
                stopDeviceGroupScripts(row.groupIndex); // wjy: 点击批量停止时，对分组内所有正在运行脚本的设备发远端 taskkill。
            } else if (selectedAction == deleteGroupAction) {
                const int groupIndex = row.groupIndex; // wjy: 记录要删除的真实分组下标，不能用界面行号删除数据。
                if (groupIndex >= 0 && groupIndex < g_deviceGroupNames.size()) {
                    const QString deletedGroupName = g_deviceGroupNames.at(groupIndex).trimmed(); // wjy: 保存被删除的分组名，用来释放这个分组里的设备。
                    for (DeviceEntry& device : g_devices) {
                        if (device.group.trimmed() == deletedGroupName) {
                            device.group.clear(); // wjy: 设备原来属于被删分组时，清空 group，让它回到“我的设备”的无分组区域。
                        }
                    }

                    g_deviceGroupNames.removeAt(groupIndex); // wjy: 从分组名称列表里删除这一项，界面上分组行会消失。
                    if (groupIndex < g_deviceGroupExpandedStates.size()) {
                        g_deviceGroupExpandedStates.removeAt(groupIndex); // wjy: 同步删除展开状态，避免状态数组和分组数组错位。
                    }
                    if (m_renamingDeviceGroupIndex == groupIndex) {
                        m_deviceGroupNameEdit->hide(); // wjy: 如果正好在重命名这个分组，删除后隐藏输入框。
                        m_renamingDeviceGroupIndex = -1; // wjy: 清空重命名状态，避免保存时再访问已删除分组。
                    } else if (m_renamingDeviceGroupIndex > groupIndex) {
                        --m_renamingDeviceGroupIndex; // wjy: 删除前面的分组后，后面正在编辑的分组下标需要前移一位。
                    }

                    m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset, maxDeviceListScrollOffset()); // wjy: 删除分组后列表变短，滚动位置要回到有效范围。
                    saveDevices(); // wjy: 保存删除后的 groups 和设备 group 字段，重启后保持删除结果。
                    update(); // wjy: 立即重绘，让分组消失并显示被释放到无分组的设备。
                }
            }
            event->accept();
            return;
        }
    }

    const QRect blankHitRect =
        scrolledDeviceGroupReservedBlankRect(
            m_deviceListScrollOffset)
            .intersected(
                deviceListViewportRect(
                    m_deviceGroupExpanded));

    if (event->button() == Qt::RightButton
        && m_deviceGroupExpanded
        && blankHitRect.contains(event->pos())) { // wjy: 确认右键点在滚动后的预留空白区域里。
        QMenu menu(this); // wjy: 创建右键菜单，父对象设为当前控件，交给 Qt 管理生命周期。
        QAction* createGroupAction = menu.addAction(QString::fromUtf8("新建分组")); // wjy: 保存菜单项指针，用来判断用户是否真的点击了“新建分组”。
        const QAction* selectedAction = menu.exec(mapToGlobal(event->pos())); // wjy: 在鼠标当前位置弹出菜单，点空白取消时返回空指针。
        if (selectedAction == createGroupAction) {
            int suffix = 1;
            QString newGroupName;

            do {
                newGroupName =
                    QString::fromUtf8("默认分组%1")
                        .arg(suffix);
                ++suffix;
            } while (g_deviceGroupNames.contains(
                newGroupName));

            g_deviceGroupNames.append(newGroupName);// wjy: 点击菜单后创建空分组，例如 默认分组1、默认分组2。
            g_deviceGroupExpandedStates.append(true); // wjy: 新建分组默认展开，所以初始显示上箭头。
            saveDevices(); // wjy: 保存 groups 字段，让新建分组关闭程序后还能恢复。
            update(); // wjy: 新增分组后重绘左侧列表，让分组立即显示在设备下面。
        }
        event->accept();
        return;
    }
// ===end====

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

// =====wjy====
    if (event->button() == Qt::LeftButton && m_deviceGroupExpanded) { // wjy: 左键按在“我的设备”的设备行上时，先记录为拖拽候选。
        const QVector<DeviceListRow> rows = visibleDeviceRows(); // wjy: 拖拽按下也使用当前可见行顺序，避免分组行插入后设备下标错位。
        const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 只允许在当前可见滚动视口内开始拖拽。
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const DeviceListRow& row = rows.at(rowIndex); // wjy: rowIndex 是界面行号，row.deviceIndex 才是真实设备下标。
            if (row.type != DeviceListRow::Type::Device) {
                continue; // wjy: 分组行只负责展开收起，不作为可拖拽设备。
            }
            const QRect rowRect =
                scrolledVisibleDeviceRowRect(
                    rowIndex,
                    m_deviceListScrollOffset);

            const QRect hitRect =
                rowRect.intersected(deviceListClip);

            if (!hitRect.contains(event->pos())) {
                continue;
            }

            const int deviceIndex = row.deviceIndex;

            if (deviceIndex < 0
                || deviceIndex >= g_devices.size()) {
                continue;
            }

            m_deviceDragCandidateActive = true;
            m_draggingDevice = false;
            m_draggingDeviceIndex = deviceIndex;

            // 每一次按下设备，都重新生成本次拖拽快照。
            m_draggingDeviceIndexes.clear();

            if (m_selectedDeviceIndexes.contains(
                    deviceIndex)) {

                // 按在已经选中的设备上：
                // 拖动当前可见且已选中的设备。
                for (const DeviceListRow& selectedRow : rows) {
                    if (selectedRow.type == DeviceListRow::Type::Device
                        && selectedRow.deviceIndex >= 0
                        && selectedRow.deviceIndex < g_devices.size()
                        && m_selectedDeviceIndexes.contains(selectedRow.deviceIndex)) {
                        m_draggingDeviceIndexes.insert(selectedRow.deviceIndex); // wjy: 只把当前可见设备写入拖拽快照，折叠隐藏的选中设备不会被批量移动。
                    }
                }
                if (m_draggingDeviceIndexes.isEmpty()) {
                    m_draggingDeviceIndexes.insert(deviceIndex); // wjy: 极端情况下可见过滤为空时，至少拖动鼠标按下的这台设备。
                }
            } else {
                // 按在一个没有被选中的设备上：
                // 本次只拖动这一台，避免误移动原来的多选设备。
                m_draggingDeviceIndexes.insert(
                    deviceIndex);
            }

            m_deviceDragStartPos = event->pos();
            m_deviceDragCurrentPos = event->pos();

            writeDeviceGridStartupLog(
                QStringLiteral(
                    "[wjy-drag] candidate "
                    "deviceIndex=%1 device=%2")
                    .arg(deviceIndex)
                    .arg(deviceDisplayName(
                        g_devices.at(deviceIndex))));
            break;
            // if (rowRect.contains(event->pos())) {
            //     const int deviceIndex = row.deviceIndex; // wjy: 记录真实设备下标，后面写 group 时才能改到正确设备。
            //     if (deviceIndex < 0 || deviceIndex >= g_devices.size()) {
            //         continue; // wjy: 防御性跳过异常设备行。
            //     }
            //     m_deviceDragCandidateActive = true; // wjy: 先标记候选，避免普通点击立即被当成拖拽。
            //     m_draggingDevice = false; // wjy: 鼠标还没移动超过阈值，所以此时还不是正式拖拽。
            //     m_draggingDeviceIndex = deviceIndex; // wjy: 保存真实设备下标，而不是界面行号。
            //     m_deviceDragStartPos = event->pos(); // wjy: 记录起点，后续用移动距离判断是否进入拖拽。
            //     m_deviceDragCurrentPos = event->pos(); // wjy: 初始化拖拽虚影位置，进入拖拽后从这个位置开始跟随鼠标。
            //     writeDeviceGridStartupLog(QStringLiteral("[wjy-drag] candidate deviceIndex=%1 device=%2")
            //         .arg(deviceIndex)
            //         .arg(deviceDisplayName(g_devices.at(deviceIndex)))); // wjy: 日志输出真实设备名到文件，方便确认拖拽对象。
            //     break;
            // }
        }
    }
// ===end====

    QFrame::mousePressEvent(event);
}

void DeviceGrid::mouseMoveEvent(QMouseEvent* event)
{
    if (m_draggingWindow && (event->buttons() & Qt::LeftButton)) {
        window()->move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }

// =====wjy====
    if (m_deviceDragCandidateActive && (event->buttons() & Qt::LeftButton)) { // wjy: 只有按住左键移动时，才判断设备拖拽。
        m_deviceDragCurrentPos = event->pos(); // wjy: 拖拽候选期间持续记录鼠标位置，正式拖拽后虚影才能跟着走。
        const int movedDistance = (event->pos() - m_deviceDragStartPos).manhattanLength(); // wjy: 用曼哈顿距离判断移动是否超过 Qt 推荐拖拽阈值。
        if (!m_draggingDevice && movedDistance >= QApplication::startDragDistance()) {
            m_draggingDevice = true; // wjy: 超过阈值后正式进入设备拖拽识别状态。
            const QString deviceName = (m_draggingDeviceIndex >= 0 && m_draggingDeviceIndex < g_devices.size())
                ? deviceDisplayName(g_devices.at(m_draggingDeviceIndex))
                : QString(); // wjy: 防御性获取真实设备名，避免日志访问越界。
            writeDeviceGridStartupLog(QStringLiteral("[wjy-drag] start deviceIndex=%1 device=%2 distance=%3")
                .arg(m_draggingDeviceIndex)
                .arg(deviceName)
                .arg(movedDistance)); // wjy: 拖拽开始日志写入文件，不再输出到 Qt Creator 控制台。
        }
        if (m_draggingDevice) {
            setCursor(Qt::ClosedHandCursor); // wjy: 拖拽识别中给一个抓取光标反馈，但不绘制拖拽动画。
            update(); // wjy: 拖拽过程中持续重绘，让半透明设备虚影跟随鼠标移动。
            event->accept();
            return;
        }
    }
// ===end====

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

void DeviceGrid::mouseDoubleClickEvent(QMouseEvent* event)
{
// =====wjy====
    if (event->button() == Qt::LeftButton && m_deviceGroupExpanded) { // wjy: 只有左键双击展开的“我的设备”列表时，才尝试重命名分组。
        const QVector<DeviceListRow> rows = visibleDeviceRows(); // wjy: 双击命中也使用可见行，分组位置会跟随 UI 绘制变化。
        const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 双击只识别当前可见视口内的分组行。
        for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const DeviceListRow& row = rows.at(rowIndex);
            if (row.type != DeviceListRow::Type::Group) {
                continue; // wjy: 只有分组行支持双击原地重命名。
            }
            const QRect rowRect =
                scrolledVisibleDeviceRowRect(
                    rowIndex,
                    m_deviceListScrollOffset);

            const QRect hitRect =
                rowRect.intersected(deviceListClip);

            if (!hitRect.contains(event->pos())) {
                continue;
            }

            beginDeviceGroupRename(row.groupIndex);
            event->accept();
            return;
        }
    }
// ===end====

    QFrame::mouseDoubleClickEvent(event);
}

void DeviceGrid::wheelEvent(QWheelEvent* event)
{
// =====wjy====
    if (m_scriptOutputVisible
        && !m_settingsSelected
        && !m_remoteAssistSelected
        && !m_localInfoSelected
        && scriptTerminalPanelRect().contains(event->position())) {
        QFont terminalFont(QStringLiteral("Consolas"));
        terminalFont.setPixelSize(12);
        const QFontMetrics metrics(terminalFont);
        const QRectF content = scriptTerminalPanelRect().adjusted(12, 46, -18, -12);
        if (m_scriptOutputDirty) {
            m_scriptOutputText = stripTerminalControlSequences(readScriptOutputFileTail(m_scriptOutputFilePath));
            m_scriptOutputDirty = false;
        }
        const int maxOffset = maxTerminalScrollOffset(m_scriptOutputText, metrics, qRound(content.height()));
        const int wheelSteps = qMax(1, qAbs(event->angleDelta().y()) / 120);
        const int lineStep = wheelSteps * 3;
        if (event->angleDelta().y() > 0) {
            m_scriptOutputScrollOffset = qBound(0, m_scriptOutputScrollOffset + lineStep, maxOffset);
        } else {
            m_scriptOutputScrollOffset = qBound(0, m_scriptOutputScrollOffset - lineStep, maxOffset);
        }
        m_scriptOutputAutoScroll = (m_scriptOutputScrollOffset == 0);
        saveCurrentScriptUiState(); // wjy: 每台设备保留自己的终端滚动位置，切到别的设备不会改变当前设备的查看位置。
        update(scriptTerminalPanelRect().toAlignedRect().adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 只有鼠标位于“我的设备”列表视口内，滚轮才控制设备列表。
    const int maxScrollOffset = maxDeviceListScrollOffset(); // wjy: 为 0 表示内容没有超过视口，不需要滚动。
    if (m_deviceGroupExpanded && maxScrollOffset > 0 && deviceListClip.contains(event->position().toPoint())) {
        const int wheelDelta = !event->pixelDelta().isNull()
            ? event->pixelDelta().y()
            : event->angleDelta().y() / 3; // wjy: 普通鼠标一格通常是 120，除以 3 后约等于滚动一行 40 像素。
        if (wheelDelta != 0) {
            const int oldOffset = m_deviceListScrollOffset;
            m_deviceListScrollOffset = qBound(0, m_deviceListScrollOffset - wheelDelta, maxScrollOffset); // wjy: 向下滚时偏移增大，向上滚时偏移减小，并限制在有效范围内。
            if (m_deviceListScrollOffset != oldOffset) {
                finishDeviceGroupRename(true); // wjy: 滚动时提交并关闭分组输入框，避免输入框停留在旧位置。
                update();
                event->accept();
                return;
            }
        }
    }
// ===end====

    QFrame::wheelEvent(event);
}

void DeviceGrid::leaveEvent(QEvent* event)
{
    setDesktopHoverActive(false);
    clearBottomActionHover();
    unsetCursor();
    QFrame::leaveEvent(event);
}

//鼠标事件
void DeviceGrid::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_draggingWindow = false;

// =====wjy====
        if (m_draggingDevice) { // wjy: 如果本次鼠标操作已经进入设备拖拽状态，松开时只输出落点日志。
            QString targetType = QStringLiteral("none"); // wjy: 默认表示没有落到可识别的分组目标。
            QString targetGroup;
            if (m_deviceGroupExpanded) {
                const QVector<DeviceListRow> rows = visibleDeviceRows(); // wjy: 拖拽落点按当前可见行识别，和 UI 绘制顺序保持一致。
                const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 拖拽落点只在当前可见滚动视口内识别。
                for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
                    const DeviceListRow& row = rows.at(rowIndex);
                    const QRect rowRect =
                        scrolledVisibleDeviceRowRect(
                            rowIndex,
                            m_deviceListScrollOffset);

                    const QRect hitRect =
                        rowRect.intersected(deviceListClip);

                    if (!hitRect.contains(event->pos())) {
                        continue;
                    }
                    if (row.type == DeviceListRow::Type::Group) {
                        targetType = QStringLiteral("group"); // wjy: 鼠标松开在分组行上，就把设备写入该分组。
                        targetGroup = g_deviceGroupNames.at(row.groupIndex);
                        break;
                    }
                    if (row.type == DeviceListRow::Type::Device && row.groupIndex >= 0 && row.groupIndex < g_deviceGroupNames.size()) {
                        targetType = QStringLiteral("group"); // wjy: 鼠标松开在分组内的设备行上，也视为拖入这个设备所属的分组。
                        targetGroup = g_deviceGroupNames.at(row.groupIndex);
                        break;
                    }
                    if (row.type == DeviceListRow::Type::Device && row.groupIndex < 0) {
                        targetType = QStringLiteral("rootBlank"); // wjy: 鼠标松开在无分组设备行上，也视为拖回“我的设备”根部。
                        break;
                    }
                }
                const QRect blankHitRect =
                    scrolledDeviceGroupReservedBlankRect(
                        m_deviceListScrollOffset)
                        .intersected(deviceListClip);

                if (targetType == QStringLiteral("none")
                    && blankHitRect.contains(event->pos())) {

                    targetType = QStringLiteral("rootBlank");
                }
            }
            const QString deviceName = (m_draggingDeviceIndex >= 0 && m_draggingDeviceIndex < g_devices.size())
                ? deviceDisplayName(g_devices.at(m_draggingDeviceIndex))
                : QString(); // wjy: 防御性获取真实设备名，避免拖拽过程中设备列表变化导致越界。
            writeDeviceGridStartupLog(QStringLiteral("[wjy-drag] drop deviceIndex=%1 device=%2 targetType=%3 targetGroup=%4")
                .arg(m_draggingDeviceIndex)
                .arg(deviceName)
                .arg(targetType)
                .arg(targetGroup)); // wjy: 拖拽落点日志写入文件，不再输出到调试控制台。
            //在这里添加写入group的代码
            // =====wjy====
            bool deviceGroupChanged = false;

            const QString normalizedTargetGroup =
                targetGroup.trimmed();

            // 遍历本次拖拽快照中的所有设备。
            for (int deviceIndex
                 : m_draggingDeviceIndexes) {

                if (deviceIndex < 0
                    || deviceIndex >= g_devices.size()) {
                    continue;
                }

                DeviceEntry& draggedDevice =
                    g_devices[deviceIndex];

                if (targetType
                    == QStringLiteral("group")) {

                    if (!normalizedTargetGroup.isEmpty()
                        && draggedDevice.group.trimmed()
                               != normalizedTargetGroup) {

                        draggedDevice.group =
                            normalizedTargetGroup;

                        deviceGroupChanged = true;
                    }

                } else if (
                    targetType
                    == QStringLiteral("rootBlank")) {

                    if (!draggedDevice.group
                             .trimmed()
                             .isEmpty()) {

                        draggedDevice.group.clear();
                        deviceGroupChanged = true;
                    }
                }
            }
            //保存逻辑
            if (deviceGroupChanged) {
                saveDevices();
                writeDeviceGridStartupLog(QStringLiteral("[wjy-drag] save group deviceIndex=%1 device=%2 group=%3")
                                         .arg(m_draggingDeviceIndex)
                                         .arg(deviceName)
                                         .arg(m_draggingDeviceIndex >= 0 && m_draggingDeviceIndex < g_devices.size()
                                                  ? g_devices.at(m_draggingDeviceIndex).group
                                                  : QString())); // wjy: 分组保存结果写入文件，便于和启动/关闭日志统一查看。
            }
            // ===end====
            m_deviceDragCandidateActive = false;
            m_draggingDevice = false;
            m_draggingDeviceIndex = -1;
            m_draggingDeviceIndexes.clear();

            unsetCursor();
            update();
            event->accept();
            return;
        }
        if (m_deviceDragCandidateActive) {
            m_deviceDragCandidateActive = false;
            m_draggingDeviceIndex = -1;
            m_draggingDeviceIndexes.clear();
        }
// ===end====

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
            const QVector<DeviceListRow> rows = visibleDeviceRows(); // wjy: 普通点击也使用可见行，保证点击位置和绘制出来的行一致。
            const QRect deviceListClip = deviceListViewportRect(m_deviceGroupExpanded); // wjy: 普通点击也只命中当前滚动视口内的列表行。
// =====wjy====
            for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) { // wjy: 一次遍历当前 UI 的每一行，设备行和分组行按同一套坐标命中。
                const DeviceListRow& row = rows.at(rowIndex);
                const QRect rowRect =
                    scrolledVisibleDeviceRowRect(
                        rowIndex,
                        m_deviceListScrollOffset);

                const QRect hitRect =
                    rowRect.intersected(deviceListClip);

                if (!hitRect.contains(event->pos())) {
                    continue;
                }

                if (row.type == DeviceListRow::Type::Group) {
                    const int groupIndex = row.groupIndex; // wjy: 点击分组行时，使用真实分组下标切换展开状态。
                    if (m_renamingDeviceGroupIndex == groupIndex) {
                        event->accept(); // wjy: 双击进入重命名后的释放事件只关闭本次点击，不再切换分组箭头。
                        return;
                    }
                    while (g_deviceGroupExpandedStates.size() <= groupIndex) {
                        g_deviceGroupExpandedStates.append(true); // wjy: 防御性补齐状态数组，缺失状态默认按展开处理。
                    }
                    g_deviceGroupExpandedStates[groupIndex] = !g_deviceGroupExpandedStates.at(groupIndex); // wjy: 点击分组行时切换展开状态，从而让箭头上下倒转。
                    pruneHiddenDeviceSelections(); // wjy: 分组折叠后立即移除隐藏设备选择，避免后续 Shift/拖拽继续带着不可见设备。
                    saveDevices(); // wjy: 保存分组展开状态，重启后箭头方向保持一致。
                    update();
                    event->accept();
                    return;
                }

                if (row.type == DeviceListRow::Type::Device) {
                    const int deviceIndex = row.deviceIndex;

                    if (deviceIndex < 0
                        || deviceIndex >= g_devices.size()) {
                        continue;
                    }

                    const bool shiftPressed =
                        event->modifiers().testFlag(
                            Qt::ShiftModifier);
                    const bool ctrlPressed =
                        event->modifiers().testFlag(
                            Qt::ControlModifier); // wjy: Ctrl+左键使用文件管理器式的单项加入/取消多选。
                    int detailDeviceIndex = deviceIndex; // wjy: 默认右侧详情跟随本次点击的设备，Ctrl 取消当前项时会重新选择一个仍可见的已选设备。

                    if (shiftPressed
                        && m_selectionAnchorDeviceIndex >= 0) {

                        // 找到上一次普通点击的设备，
                        // 当前在可见列表中的视觉行号。
                        int anchorRowIndex = -1;

                        for (int i = 0; i < rows.size(); ++i) {
                            const DeviceListRow& candidateRow =
                                rows.at(i);

                            if (candidateRow.type
                                    == DeviceListRow::Type::Device
                                && candidateRow.deviceIndex
                                       == m_selectionAnchorDeviceIndex) {

                                anchorRowIndex = i;
                                break;
                            }
                        }

                        if (anchorRowIndex >= 0) {
                            // 找到起点后，选中起点和当前点击位置之间
                            // 的所有设备行，分组行自动跳过。
                            const int firstRow =
                                qMin(anchorRowIndex, rowIndex);

                            const int lastRow =
                                qMax(anchorRowIndex, rowIndex);

                            m_selectedDeviceIndexes.clear();

                            for (int i = firstRow;
                                 i <= lastRow;
                                 ++i) {

                                const DeviceListRow& rangeRow =
                                    rows.at(i);

                                if (rangeRow.type
                                    != DeviceListRow::Type::Device) {
                                    continue;
                                }

                                const int rangeDeviceIndex =
                                    rangeRow.deviceIndex;

                                if (rangeDeviceIndex < 0
                                    || rangeDeviceIndex
                                           >= g_devices.size()) {
                                    continue;
                                }

                                m_selectedDeviceIndexes.insert(
                                    rangeDeviceIndex);
                            }
                        } else {
                            // 锚点设备目前不可见，例如所在分组已经收起。
                            // 此时退化为普通单选。
                            m_selectedDeviceIndexes.clear();
                            m_selectedDeviceIndexes.insert(
                                deviceIndex);

                            m_selectionAnchorDeviceIndex =
                                deviceIndex;
                        }
                    } else if (ctrlPressed) {
                        if (m_selectedDeviceIndexes.contains(deviceIndex)
                            && m_selectedDeviceIndexes.size() > 1) {
                            m_selectedDeviceIndexes.remove(deviceIndex); // wjy: Ctrl 点已选设备时从多选集合里移除，行为对齐文件管理器取消选择。

                            bool foundVisibleSelectedDevice = false; // wjy: 记录取消当前设备后，是否还能找到另一台可见已选设备接管详情页。
                            for (const DeviceListRow& visibleRow : rows) {
                                if (visibleRow.type == DeviceListRow::Type::Device
                                    && visibleRow.deviceIndex >= 0
                                    && visibleRow.deviceIndex < g_devices.size()
                                    && m_selectedDeviceIndexes.contains(visibleRow.deviceIndex)) {
                                    detailDeviceIndex = visibleRow.deviceIndex; // wjy: 被取消的是详情主设备时，切到第一台仍可见的已选设备。
                                    foundVisibleSelectedDevice = true;
                                    break;
                                }
                            }
                            if (!foundVisibleSelectedDevice) {
                                m_selectedDeviceIndexes.insert(deviceIndex); // wjy: 如果历史残留状态导致没有可见已选设备，就保留当前设备，避免详情指向未选中项。
                                detailDeviceIndex = deviceIndex;
                            }
                        } else {
                            m_selectedDeviceIndexes.insert(deviceIndex); // wjy: Ctrl 点未选设备时加入多选；如果只剩这一台，则保持至少一台被选中。
                        }

                        m_selectionAnchorDeviceIndex = deviceIndex; // wjy: Ctrl 点击也作为下一次 Shift 范围选择的新起点，贴近文件管理器操作习惯。
                    } else {
                        // 没有按 Shift：普通单选，
                        // 同时把这台设备设为下一次 Shift 的起点。
                        m_selectedDeviceIndexes.clear();
                        m_selectedDeviceIndexes.insert(
                            deviceIndex);

                        m_selectionAnchorDeviceIndex =
                            deviceIndex;
                    }

                    m_settingsSelected = false;
                    m_remoteAssistSelected = false;
                    m_localInfoSelected = false;

                    setDesktopHoverActive(false);
                    clearBottomActionHover();

                    updateAddDeviceControls();
                    updateLocalInfoControls();
                    updateSettingsControls();

                    // 右侧详情跟随当前点击或 Ctrl 取消后仍可见的已选设备。
                    startDeviceSwitchAnimation(
                        detailDeviceIndex,
                        deviceDisplayName(
                            g_devices.at(detailDeviceIndex)));

                    event->accept();
                    return;
                }
            }
// ===end====
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
