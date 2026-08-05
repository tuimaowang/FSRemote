#include "ui/DeviceSearchPanel.h"

#include <QAbstractItemView>
#include <QColor>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <utility>

namespace ui {
namespace {

// =====wjy====
bool containsText(const QString& source, const QString& filter)
{
    const QString normalizedFilter = filter.trimmed();
    return normalizedFilter.isEmpty()
        || source.contains(normalizedFilter, Qt::CaseInsensitive); // wjy: 名称、分组、MAC 和单个 IP 段统一使用不区分大小写的包含匹配。
}

QString displayValue(const QString& value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("--") : trimmed; // wjy: 空分组或空 MAC 在结果表中明确显示占位，避免看起来像表格缺列。
}
// ===end====

} // namespace

// =====wjy====
DeviceSearchPanel::DeviceSearchPanel(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);
    QPalette panelPalette = palette();
    panelPalette.setColor(QPalette::Window, QColor(QStringLiteral("#F8FAFC"))); // wjy: 使用 DeviceGrid 详情区同色底板，透明空隙不会露出下面的设备卡片。
    setPalette(panelPalette);

    const QString editStyle = QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:5px;padding:0 10px;"
        "font-family:'Microsoft YaHei UI';font-size:13px;color:#111827;}"
        "QLineEdit:focus{border:1px solid #3A7BFC;}");

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(8, 8, 8, 8); // wjy: 与配置页内容区域保持一致的内边距，页签切换时左右边界不跳动。
    rootLayout->setSpacing(14);

    auto* fieldGrid = new QGridLayout();
    fieldGrid->setHorizontalSpacing(14);
    fieldGrid->setVerticalSpacing(6);
    m_deviceNameEdit = new QLineEdit(this);
    m_groupNameEdit = new QLineEdit(this);
    m_macEdit = new QLineEdit(this);
    m_deviceNameEdit->setPlaceholderText(QString::fromUtf8("输入设备名的一部分"));
    m_groupNameEdit->setPlaceholderText(QString::fromUtf8("输入组名的一部分"));
    m_macEdit->setPlaceholderText(QString::fromUtf8("输入 MAC 地址的一部分"));
    for (QLineEdit* edit : {m_deviceNameEdit, m_groupNameEdit, m_macEdit}) {
        edit->setFixedHeight(36);
        edit->setClearButtonEnabled(true);
        edit->setStyleSheet(editStyle);
    }

    fieldGrid->addWidget(new QLabel(QString::fromUtf8("设备名称"), this), 0, 0);
    fieldGrid->addWidget(new QLabel(QString::fromUtf8("分组名称"), this), 0, 1);
    fieldGrid->addWidget(new QLabel(QStringLiteral("MAC"), this), 0, 2);
    fieldGrid->addWidget(m_deviceNameEdit, 1, 0);
    fieldGrid->addWidget(m_groupNameEdit, 1, 1);
    fieldGrid->addWidget(m_macEdit, 1, 2);
    fieldGrid->setColumnStretch(0, 1);
    fieldGrid->setColumnStretch(1, 1);
    fieldGrid->setColumnStretch(2, 2);
    rootLayout->addLayout(fieldGrid);

    auto* ipRow = new QHBoxLayout();
    ipRow->setSpacing(7);
    auto* ipLabel = new QLabel(QStringLiteral("IP"), this);
    ipLabel->setFixedWidth(24);
    ipRow->addWidget(ipLabel);
    for (int index = 0; index < static_cast<int>(m_ipEdits.size()); ++index) {
        auto* edit = new QLineEdit(this);
        edit->setFixedSize(72, 36);
        edit->setAlignment(Qt::AlignCenter);
        edit->setMaxLength(3);
        edit->setPlaceholderText(QStringLiteral("*"));
        edit->setValidator(new QIntValidator(0, 255, edit)); // wjy: 每个输入框只接受合法 IPv4 单段范围，空值继续表示未知通配段。
        edit->setStyleSheet(editStyle);
        m_ipEdits[static_cast<size_t>(index)] = edit;
        ipRow->addWidget(edit);
        if (index + 1 < static_cast<int>(m_ipEdits.size())) {
            auto* dot = new QLabel(QStringLiteral("."), this);
            dot->setAlignment(Qt::AlignCenter);
            ipRow->addWidget(dot);
        }
    }
    ipRow->addStretch(1); // wjy: 所有输入框 textChanged 都会实时刷新结果，因此删除重复的查找按钮并让 IP 区自然占据左侧。
    rootLayout->addLayout(ipRow);

    m_resultSummary = new QLabel(this);
    m_resultSummary->setStyleSheet(QStringLiteral("font-family:'Microsoft YaHei UI';font-size:12px;color:#687384;"));
    rootLayout->addWidget(m_resultSummary);

    m_resultTable = new QTableWidget(this);
    m_resultTable->setColumnCount(4);
    m_resultTable->setHorizontalHeaderLabels({QString::fromUtf8("设备名称"), QString::fromUtf8("分组"), QStringLiteral("IP"), QStringLiteral("MAC")});
    m_resultTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_resultTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_resultTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_resultTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_resultTable->verticalHeader()->setVisible(false);
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultTable->setAlternatingRowColors(true);
    m_resultTable->setShowGrid(false);
    m_resultTable->setStyleSheet(QStringLiteral(
        "QTableWidget{background:#FFFFFF;border:1px solid #DDE3EA;border-radius:5px;"
        "font-family:'Microsoft YaHei UI';font-size:13px;color:#111827;alternate-background-color:#F8FAFC;}"
        "QTableWidget::item{padding:7px;}"
        "QTableWidget::item:selected{background:#EAF1FF;color:#111827;}"
        "QHeaderView::section{background:#F5F7FA;border:0;border-bottom:1px solid #DDE3EA;padding:7px;color:#475569;}"));
    rootLayout->addWidget(m_resultTable, 1);

    const auto connectFilter = [this](QLineEdit* edit) {
        connect(edit, &QLineEdit::textChanged, this, [this] {
            if (!m_populatingFields) refreshResults(); // wjy: 用户输入时实时收窄候选，自动回填期间通过标志阻止递归搜索。
        });
    };
    connectFilter(m_deviceNameEdit);
    connectFilter(m_groupNameEdit);
    connectFilter(m_macEdit);
    for (int index = 0; index < static_cast<int>(m_ipEdits.size()); ++index) {
        QLineEdit* edit = m_ipEdits[static_cast<size_t>(index)];
        connectFilter(edit);
        connect(edit, &QLineEdit::returnPressed, this, [this, index] {
            if (index + 1 < static_cast<int>(m_ipEdits.size())) {
                m_ipEdits[static_cast<size_t>(index + 1)]->setFocus(Qt::TabFocusReason); // wjy: 前三段回车顺序移动焦点，支持只用键盘连续输入记得的 IP 片段。
                m_ipEdits[static_cast<size_t>(index + 1)]->selectAll();
            } else {
                refreshResults(); // wjy: 最后一段回车主动刷新当前实时筛选结果，同时保持焦点仍在 IP 区域。
            }
        });
    }

    connect(m_resultTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (row >= 0 && row < m_resultIndexes.size()) populateFields(m_devices.at(m_resultIndexes.at(row))); // wjy: 多候选单击立即回填完整信息，但保留结果表供用户继续比较。
    });
    connect(m_resultTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        activateResultRow(row); // wjy: 双击是明确定位动作，单击只负责查看信息。
    });

    refreshResults();
}

void DeviceSearchPanel::setDevices(QVector<DeviceSearchItem> devices)
{
    m_devices = std::move(devices); // wjy: 快照整体替换，面板不持有 DeviceCatalog 引用，也不会把筛选结果写回目录。
    refreshResults();
}

void DeviceSearchPanel::focusPrimaryInput()
{
    m_deviceNameEdit->setFocus(Qt::ShortcutFocusReason); // wjy: 切换到查找页后立即允许键盘输入，不要求用户再点一次文本框。
    m_deviceNameEdit->selectAll();
}

bool DeviceSearchPanel::matchesFilters(const DeviceSearchItem& item) const
{
    return containsText(item.deviceName, m_deviceNameEdit->text())
        && containsText(item.groupName, m_groupNameEdit->text())
        && containsText(item.macAddress, m_macEdit->text())
        && matchesIpFilters(item.ipAddress); // wjy: 各类非空条件使用 AND 组合，增加任意字段都会继续缩小结果而不会覆盖其它条件。
}

bool DeviceSearchPanel::matchesIpFilters(const QString& ipAddress) const
{
    QStringList filters;
    filters.reserve(4);
    bool hasFilter = false;
    for (int index = 0; index < 4; ++index) {
        const QString filter = m_ipEdits[static_cast<size_t>(index)]->text().trimmed();
        filters.append(filter);
        hasFilter = hasFilter || !filter.isEmpty(); // wjy: 先判断用户是否真的填写 IP 条件，名称搜索不能被设备的空 IP 或异常 IP 误排除。
    }
    if (!hasFilter) {
        return true; // wjy: 四个 IP 框全空时不检查设备 IP 格式，设备名、分组名或 MAC 可以独立完成搜索。
    }

    const QStringList actualSegments = ipAddress.trimmed().split(QLatin1Char('.'));
    if (actualSegments.size() != 4) {
        return false; // wjy: 只有用户启用 IP 条件后，非标准 IPv4 记录才不参与四段匹配。
    }

    bool positionalMatch = true;
    for (int index = 0; index < 4; ++index) {
        const QString& filter = filters.at(index);
        if (filter.isEmpty()) continue;
        positionalMatch = positionalMatch && containsText(actualSegments.at(index), filter); // wjy: 用户点击明确的第几段输入时优先支持按位置匹配。
    }

    int actualIndex = 0;
    bool orderedMatch = true;
    for (const QString& filter : std::as_const(filters)) {
        if (filter.isEmpty()) continue;
        bool found = false;
        while (actualIndex < actualSegments.size()) {
            if (containsText(actualSegments.at(actualIndex), filter)) {
                found = true;
                ++actualIndex;
                break;
            }
            ++actualIndex;
        }
        if (!found) {
            orderedMatch = false;
            break;
        }
    }
    return positionalMatch || orderedMatch; // wjy: 输入 2、9 即可按顺序命中 192.168.2.9，同时仍兼容在第三、四框按位置填写。
}

void DeviceSearchPanel::refreshResults()
{
    if (m_populatingFields) return;

    QString previouslySelectedId;
    const int currentRow = m_resultTable->currentRow();
    if (currentRow >= 0) {
        if (const QTableWidgetItem* selectedItem = m_resultTable->item(currentRow, 0)) {
            previouslySelectedId = selectedItem->data(Qt::UserRole).toString(); // wjy: 表格刷新边界状态下首列可能尚未创建，空指针保护避免嵌入面板偶发崩溃。
        }
    }
    m_resultIndexes.clear();
    for (int index = 0; index < m_devices.size(); ++index) {
        if (matchesFilters(m_devices.at(index))) m_resultIndexes.append(index); // wjy: 结果只保存快照下标，不复制设备实体也不接触目录写接口。
    }

    m_resultTable->setRowCount(m_resultIndexes.size());
    int selectedRow = -1;
    for (int row = 0; row < m_resultIndexes.size(); ++row) {
        const DeviceSearchItem& item = m_devices.at(m_resultIndexes.at(row));
        const QStringList values{displayValue(item.deviceName), displayValue(item.groupName), displayValue(item.ipAddress), displayValue(item.macAddress)};
        for (int column = 0; column < values.size(); ++column) {
            auto* tableItem = new QTableWidgetItem(values.at(column));
            if (column == 0) tableItem->setData(Qt::UserRole, item.deviceId); // wjy: 稳定 ID 只绑定到首列，整行选择仍能可靠激活目标。
            m_resultTable->setItem(row, column, tableItem);
        }
        if (item.deviceId == previouslySelectedId) selectedRow = row;
    }

    m_resultSummary->setText(m_resultIndexes.isEmpty()
        ? QString::fromUtf8("没有找到符合条件的设备")
        : QString::fromUtf8("找到 %1 台设备，单击查看完整信息，双击定位").arg(m_resultIndexes.size()));
    if (selectedRow >= 0) m_resultTable->selectRow(selectedRow);
    if (m_resultIndexes.size() == 1) {
        m_resultTable->selectRow(0);
        const DeviceSearchItem& onlyItem = m_devices.at(m_resultIndexes.first());
        const bool alreadyPopulated = m_hasAutoPopulatedResult
            && m_lastAutoPopulatedDeviceId == onlyItem.deviceId; // wjy: 同一唯一设备只自动回填一次，后续删除或修改字段不会被下一次 textChanged 覆盖。
        if (!alreadyPopulated) {
            populateFields(onlyItem); // wjy: 候选首次收窄到唯一设备时仍自动补齐完整信息，保持原有快捷核对体验。
        }
    } else {
        m_hasAutoPopulatedResult = false;
        m_lastAutoPopulatedDeviceId.clear(); // wjy: 候选变成零个或多个后重置门禁，下次重新收窄到唯一设备可以再次自动回填。
    }
}

void DeviceSearchPanel::populateFields(const DeviceSearchItem& item)
{
    m_populatingFields = true;
    m_hasAutoPopulatedResult = true;
    m_lastAutoPopulatedDeviceId = item.deviceId; // wjy: 自动回填和用户单击回填都记录设备身份，随后允许用户自由删除任意输入内容。
    m_deviceNameEdit->setText(item.deviceName.trimmed());
    m_groupNameEdit->setText(item.groupName.trimmed());
    m_macEdit->setText(item.macAddress.trimmed());
    const QStringList segments = item.ipAddress.trimmed().split(QLatin1Char('.'));
    for (int index = 0; index < 4; ++index) {
        m_ipEdits[static_cast<size_t>(index)]->setText(segments.size() == 4 ? segments.at(index) : QString()); // wjy: 标准 IP 完整拆回四段，异常记录则清空 IP 框但保留其它可查看信息。
    }
    m_populatingFields = false;
}

void DeviceSearchPanel::activateResultRow(int row)
{
    if (row < 0 || row >= m_resultIndexes.size()) return;
    const QString deviceId = m_devices.at(m_resultIndexes.at(row)).deviceId;
    if (deviceActivated) deviceActivated(deviceId); // wjy: 主窗口收到稳定 ID 后重新解析最新目录，并负责切回配置页显示目标设备。
}
// ===end====

} // namespace ui
