#pragma once

#include "system/DeviceStatusService.h"

#include <QCollator>
#include <QString>

namespace ui {

// =====wjy====
struct DeviceListSortItem {
    QString displayName; // wjy: 排序只消费最终展示名，设备名为空时由调用方提前回退到 IP。
    platform::DevicePresenceState presence = platform::DevicePresenceState::Unknown; // wjy: 在线状态是第一排序键，Online 和 Busy 都代表设备当前可连接。
    int sourceIndex = -1; // wjy: 名称和状态完全相同时按稳定源下标收尾，避免列表在重复排序时抖动。
};

inline bool devicePresenceSortsOnlineFirst(platform::DevicePresenceState presence)
{
    return presence == platform::DevicePresenceState::Online
        || presence == platform::DevicePresenceState::Busy; // wjy: Busy 只是正在被控制，设备仍在线，所以和 Online 一起排在离线/未知设备之前。
}

class DeviceListNaturalLess final {
public:
    DeviceListNaturalLess()
    {
        m_collator.setCaseSensitivity(Qt::CaseInsensitive); // wjy: 英文名称忽略大小写后按字母顺序比较，避免大写设备被单独分段。
        m_collator.setNumericMode(true); // wjy: 连续数字按数值比较，让 2 排在 10 前面而不是按字符串首字符排序。
    }

    bool operator()(const DeviceListSortItem& left, const DeviceListSortItem& right) const
    {
        const bool leftOnline = devicePresenceSortsOnlineFirst(left.presence); // wjy: 第一排序键只区分“在线可用”和“非在线”。
        const bool rightOnline = devicePresenceSortsOnlineFirst(right.presence);
        if (leftOnline != rightOnline) {
            return leftOnline; // wjy: 任意在线/忙碌设备都优先于 Offline 和 Unknown，名称只在同一状态分区内比较。
        }

        const QString leftName = left.displayName.trimmed(); // wjy: 去掉首尾空格，避免不可见字符改变自然排序位置。
        const QString rightName = right.displayName.trimmed();
        const int naturalCompare = m_collator.compare(leftName, rightName); // wjy: 使用当前系统区域规则比较中文/英文，并对名称中的数字段启用自然数排序。
        if (naturalCompare != 0) {
            return naturalCompare < 0;
        }

        const int exactCompare = QString::compare(leftName, rightName, Qt::CaseSensitive); // wjy: 自然比较认为相等时用精确文本建立确定顺序。
        if (exactCompare != 0) {
            return exactCompare < 0;
        }
        return left.sourceIndex < right.sourceIndex; // wjy: 完全同名设备保持原目录顺序，新增或同步后不会随机换位。
    }

private:
    QCollator m_collator; // wjy: 一个排序批次复用同一比较器，避免每比较两台设备都重新创建区域排序对象。
};
// ===end====

} // namespace ui
