#include "ui/DeviceListSortPolicy.h"

#include <QCoreApplication>
#include <QLocale>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

// =====wjy====
void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n'; // wjy: Debug 和 Release 都保留失败输出，不使用可能被 NDEBUG 移除的 assert。
        std::exit(1);
    }
}

QStringList sortedNames(QVector<ui::DeviceListSortItem> items)
{
    std::stable_sort(items.begin(), items.end(), ui::DeviceListNaturalLess()); // wjy: 测试直接执行生产排序策略，避免复制另一套比较逻辑。
    QStringList result;
    for (const ui::DeviceListSortItem& item : items) {
        result.append(item.displayName);
    }
    return result;
}
// ===end====

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates)); // wjy: 固定英文区域验证字母顺序，数字自然排序不依赖开发机当前语言。

    using platform::DevicePresenceState;
    // =====wjy====
    require(sortedNames({
        {QStringLiteral("10"), DevicePresenceState::Online, 0},
        {QStringLiteral("2"), DevicePresenceState::Online, 1},
        {QStringLiteral("1"), DevicePresenceState::Online, 2},
        {QStringLiteral("20"), DevicePresenceState::Online, 3},
        {QStringLiteral("3"), DevicePresenceState::Online, 4},
        {QStringLiteral("11"), DevicePresenceState::Online, 5},
    }) == QStringList({QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("10"), QStringLiteral("11"), QStringLiteral("20")}),
        "numeric device names must use natural-number order"); // wjy: 锁定 2 必须排在 10 前，防止回退为字符串首字符排序。

    require(sortedNames({
        {QStringLiteral("charlie"), DevicePresenceState::Online, 0},
        {QStringLiteral("Alpha"), DevicePresenceState::Online, 1},
        {QStringLiteral("bravo"), DevicePresenceState::Online, 2},
    }) == QStringList({QStringLiteral("Alpha"), QStringLiteral("bravo"), QStringLiteral("charlie")}),
        "English device names must sort alphabetically without case buckets"); // wjy: 英文按首字母/完整名称顺序排列，不让大小写拆成两段。

    require(sortedNames({
        {QStringLiteral("1"), DevicePresenceState::Offline, 0},
        {QStringLiteral("20"), DevicePresenceState::Online, 1},
        {QStringLiteral("3"), DevicePresenceState::Busy, 2},
        {QStringLiteral("2"), DevicePresenceState::Unknown, 3},
    }) == QStringList({QStringLiteral("3"), QStringLiteral("20"), QStringLiteral("1"), QStringLiteral("2")}),
        "online and busy devices must precede offline and unknown devices"); // wjy: 在线优先级高于名称，两个状态分区内部再分别执行自然排序。
    // ===end====

    std::cout << "device list sort policy tests passed\n";
    return 0;
}
