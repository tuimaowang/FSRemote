#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

#include <array>
#include <functional>

class QLabel;
class QLineEdit;
class QTableWidget;

namespace ui {

// =====wjy====
struct DeviceSearchItem {
    QString deviceId; // wjy: 激活结果时只把稳定设备 ID 交回主窗口，不携带可能变化的展示下标。
    QString deviceName;
    QString groupName;
    QString macAddress;
    QString ipAddress;
};

class DeviceSearchPanel final : public QWidget {
public:
    explicit DeviceSearchPanel(QWidget* parent = nullptr);

    void setDevices(QVector<DeviceSearchItem> devices); // wjy: 每次进入查找页时替换只读快照，并使用当前输入条件立即刷新候选。
    void focusPrimaryInput(); // wjy: 页签点击或 Ctrl+F 后把键盘焦点交给设备名输入框，方便立即输入。

    std::function<void(const QString&)> deviceActivated; // wjy: 双击候选时由主窗口按稳定 ID 重新定位，面板不直接访问设备目录。

private:
    void refreshResults();
    bool matchesFilters(const DeviceSearchItem& item) const;
    bool matchesIpFilters(const QString& ipAddress) const;
    void populateFields(const DeviceSearchItem& item);
    void activateResultRow(int row);

    QVector<DeviceSearchItem> m_devices; // wjy: 面板只保存进入查找页时的只读快照，搜索过程不会修改 DeviceCatalog。
    QVector<int> m_resultIndexes; // wjy: 表格行映射到当前快照下标，目录刷新后由 setDevices 整体重建。
    QLineEdit* m_deviceNameEdit = nullptr;
    QLineEdit* m_groupNameEdit = nullptr;
    QLineEdit* m_macEdit = nullptr;
    std::array<QLineEdit*, 4> m_ipEdits{}; // wjy: 四段输入与其它筛选框共用实时刷新，不再保留重复的手动查找按钮状态。
    QLabel* m_resultSummary = nullptr;
    QTableWidget* m_resultTable = nullptr;
    bool m_populatingFields = false; // wjy: 唯一结果或行选择回填时阻止 textChanged 再次递归过滤。
    bool m_hasAutoPopulatedResult = false; // wjy: 标记当前唯一结果是否已经回填，避免用户删除字段后被实时筛选立刻补回。
    QString m_lastAutoPopulatedDeviceId; // wjy: 只有唯一候选切换成另一台设备时才允许再次自动回填，同一设备可自由修改搜索条件。
};
// ===end====

} // namespace ui
