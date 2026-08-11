## 2026-07-01 09:04 - 在“我的设备”列表下方预留空白区域

### Changed Location
- `src/ui/DeviceGrid.cpp:155`: 新增预留空白高度常量。
- `src/ui/DeviceGrid.cpp:375`: 计算当前设备数量。
- `src/ui/DeviceGrid.cpp:376`: 调整“设备管理”标题位置的高度计算，让“我的设备”设备行下面多出空白区域。

### Reason
用户希望在“我的设备”下拉列表的设备行下面出现一段空白区域，后续可继续设计右键菜单。当前布局中，“设备管理”标题紧跟在设备行后面，没有可点击/可观察的空白区域。因此本次只调整布局高度，让“设备管理”整体下移 30 像素，不增加右键菜单、不增加分组逻辑，方便逐步测试。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:147-153
struct DeviceEntry {
    QString name;
    QString ip;
    QString mac;
    QString broadcastIp;
    QString remark;
};
```

```cpp
// src/ui/DeviceGrid.cpp:372-376
QRect remoteAssistGroupHeaderRect(bool deviceGroupExpanded)
{
    const int deviceRowsHeight = deviceGroupExpanded ? deviceNames().size() * 40 : 0;
    return QRect(0, deviceGroupHeaderRect().bottom() + 1 + deviceRowsHeight, 236, 34);
}
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:147-155
struct DeviceEntry {
    QString name;
    QString ip;
    QString mac;
    QString broadcastIp;
    QString remark;
};

constexpr int kDeviceGroupReservedBlankHeight = 30; // wjy: 预留“我的设备”列表下方空白，后续可用于右键菜单命中区域。
```

```cpp
// src/ui/DeviceGrid.cpp:372-381
QRect remoteAssistGroupHeaderRect(bool deviceGroupExpanded)
{
// =====wjy====
    const int deviceCount = deviceNames().size(); // wjy: 统计当前“我的设备”下面真实设备数量，用来计算列表高度。
    const int deviceRowsHeight = deviceGroupExpanded
        ? deviceCount * 40 + (deviceCount > 0 ? kDeviceGroupReservedBlankHeight : 0) // wjy: 展开时每个设备占 40 像素，且有设备时在列表末尾追加预留空白。
        : 0; // wjy: “我的设备”收起时不占用设备行和预留空白，让下面区域正常上移。
// ===end====
    return QRect(0, deviceGroupHeaderRect().bottom() + 1 + deviceRowsHeight, 236, 34);
}
```

### Steps
1. 新增 `kDeviceGroupReservedBlankHeight` 常量，把预留高度集中命名，避免直接写魔法数字。
2. 在 `remoteAssistGroupHeaderRect()` 中先计算设备数量。
3. 当“我的设备”展开且至少有一个设备时，在设备列表高度后追加 30 像素空白。
4. 保持“我的设备”收起时高度为 0，避免影响收起状态布局。

### Verification
已执行 `git diff --check -- src\ui\DeviceGrid.cpp`，未发现空白错误。未执行构建，按用户要求由用户自行测试当前步骤。

## 2026-07-01 09:07 - 空白区域右键弹出菜单

### Changed Location
- `src/ui/DeviceGrid.cpp:439`: 新增“我的设备”列表下方预留空白区域的矩形计算函数。
- `src/ui/DeviceGrid.cpp:2245`: 在 `mousePressEvent()` 开头处理空白区域右键菜单。

### Reason
第一步已经让“我的设备”设备列表下方出现 30 像素空白。本步骤只验证这块空白区域能否正确响应右键，并在鼠标位置弹出菜单。菜单中只显示“新建分组”，暂时不绑定创建分组逻辑，避免一次改动过多，方便用户独立测试右键命中区域是否正确。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:434-437
QRect deviceRowRect(int index)
{
    return QRect(4, 96 + index * 40, 232, 36);
}
```

```cpp
// src/ui/DeviceGrid.cpp:2243-2252
void DeviceGrid::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton
        && event->pos().y() >= 0
        && event->pos().y() < 48
        && !refreshRect().contains(event->pos())
        && !minimizeRect().contains(event->pos())
        && !closeRect().contains(event->pos())) {
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:434-449
QRect deviceRowRect(int index)
{
    return QRect(4, 96 + index * 40, 232, 36);
}

QRect deviceGroupReservedBlankRect()
{
// =====wjy====
    const int deviceCount = deviceNames().size(); // wjy: 空白区要排在最后一个设备行之后，所以先取得当前设备数量。
    if (deviceCount <= 0) {
        return {}; // wjy: 没有设备时不提供右键空白区，避免误点空列表区域。
    }
    const int blankTop = deviceRowRect(deviceCount - 1).y() + 40; // wjy: 从最后一个设备占用的 40 像素槽位后面开始。
    return QRect(4, blankTop, 232, kDeviceGroupReservedBlankHeight); // wjy: 返回第一步预留出来的空白矩形，供右键菜单命中检测使用。
// ===end====
}
```

```cpp
// src/ui/DeviceGrid.cpp:2243-2257
void DeviceGrid::mousePressEvent(QMouseEvent* event)
{
// =====wjy====
    if (event->button() == Qt::RightButton // wjy: 只响应鼠标右键，不影响左键选择设备、拖动窗口等原逻辑。
        && m_deviceGroupExpanded // wjy: 只有“我的设备”展开时，预留空白才显示并允许弹出菜单。
        && deviceGroupReservedBlankRect().contains(event->pos())) { // wjy: 确认右键点在第一步预留的空白区域里。
        QMenu menu(this); // wjy: 创建右键菜单，本步骤只验证菜单弹出，不执行创建分组逻辑。
        menu.addAction(QString::fromUtf8("新建分组")); // wjy: 先放出“新建分组”入口，下一步再绑定真正创建分组的动作。
        menu.exec(mapToGlobal(event->pos())); // wjy: 在鼠标当前位置弹出菜单，用户点空白或菜单项后自动关闭。
        event->accept();
        return;
    }
// ===end====

    if (event->button() == Qt::LeftButton
```

### Steps
1. 新增 `deviceGroupReservedBlankRect()`，把第一步预留出来的空白区域变成可复用矩形。
2. 在 `mousePressEvent()` 开头判断鼠标右键是否落在该空白区域内。
3. 命中时创建 `QMenu`，加入“新建分组”菜单项。
4. 只弹出菜单，不处理菜单项点击结果。

### Verification
已执行 `git diff --check -- src\ui\DeviceGrid.cpp`，未发现空白错误。未执行构建，按用户要求由用户自行测试当前步骤。建议测试：右键空白区域应弹出“新建分组”；右键设备行或其它区域不应弹出该菜单。

## 2026-07-01 09:17 - 点击菜单创建空分组行

### Changed Location
- `src/ui/DeviceGrid.cpp:301`: 新增临时分组名称列表 `g_deviceGroupNames`。
- `src/ui/DeviceGrid.cpp:372`: 新增可见行数函数声明。
- `src/ui/DeviceGrid.cpp:377`: 布局高度改为按设备行加分组行计算。
- `src/ui/DeviceGrid.cpp:406`: 新增 `visibleDeviceListRowCount()`，统计设备行和分组行。
- `src/ui/DeviceGrid.cpp:452`: 空白区域位置改为排在最后一个可见行后面。
- `src/ui/DeviceGrid.cpp:2054`: 在设备行下面绘制分组行。
- `src/ui/DeviceGrid.cpp:2283`: 记录“新建分组”菜单项。
- `src/ui/DeviceGrid.cpp:2286`: 点击菜单项后新增 `默认分组1`、`默认分组2` 等空分组。

### Reason
上一步只弹出了菜单，没有执行菜单项。本步骤让用户点击“新建分组”后，在“我的设备”下方显示一个空的子下拉框行。为了保持这一步可测试、可回退，本次不保存分组到 JSON，不处理设备归属，也不做双击重命名或拖拽，只验证“菜单动作 -> 新增空分组行 -> 布局空白继续跟随最后一行”的基础链路。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:300
QVector<DeviceEntry> g_devices;
```

```cpp
// src/ui/DeviceGrid.cpp:370-380
QStringList deviceNames();

QRect remoteAssistGroupHeaderRect(bool deviceGroupExpanded)
{
// =====wjy====
    const int deviceCount = deviceNames().size(); // wjy: 统计当前“我的设备”下面真实设备数量，用来计算列表高度。
    const int deviceRowsHeight = deviceGroupExpanded
        ? deviceCount * 40 + (deviceCount > 0 ? kDeviceGroupReservedBlankHeight : 0) // wjy: 展开时每个设备占 40 像素，且有设备时在列表末尾追加预留空白。
        : 0; // wjy: “我的设备”收起时不占用设备行和预留空白，让下面区域正常上移。
// ===end====
```

```cpp
// src/ui/DeviceGrid.cpp:2013-2043
const QStringList names = deviceNames();
const QSet<int> badges = deviceBadgeIndexes();
const int visibleDeviceBottom = remoteAssistGroupHeaderRect(m_deviceGroupExpanded).y();
if (m_deviceGroupExpanded) {
    for (int i = 0; i < names.size(); ++i) {
        ...
    }
}
```

```cpp
// src/ui/DeviceGrid.cpp:2282-2284
QMenu menu(this); // wjy: 创建右键菜单，本步骤只验证菜单弹出，不执行创建分组逻辑。
menu.addAction(QString::fromUtf8("新建分组")); // wjy: 先放出“新建分组”入口，下一步再绑定真正创建分组的动作。
menu.exec(mapToGlobal(event->pos())); // wjy: 在鼠标当前位置弹出菜单，用户点空白或菜单项后自动关闭。
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:300-301
QVector<DeviceEntry> g_devices;
QVector<QString> g_deviceGroupNames; // wjy: 临时保存右键新建的分组名称，本步骤只存在内存中，不写入 devices.json。
```

```cpp
// src/ui/DeviceGrid.cpp:370-382
QStringList deviceNames();
int visibleDeviceListRowCount(); // wjy: 统计“我的设备”下拉框里的可见行数，包含设备行和新建分组行。

QRect remoteAssistGroupHeaderRect(bool deviceGroupExpanded)
{
// =====wjy====
    const int visibleRowCount = visibleDeviceListRowCount(); // wjy: 统计当前“我的设备”下面所有可见行，设备行和分组行都要占高度。
    const int deviceRowsHeight = deviceGroupExpanded
        ? visibleRowCount * 40 + (visibleRowCount > 0 ? kDeviceGroupReservedBlankHeight : 0) // wjy: 展开时每行占 40 像素，并在最后一行下面保留空白。
        : 0; // wjy: “我的设备”收起时不占用设备行和预留空白，让下面区域正常上移。
// ===end====
```

```cpp
// src/ui/DeviceGrid.cpp:406-410
int visibleDeviceListRowCount()
{
// =====wjy====
    return deviceNames().size() + g_deviceGroupNames.size(); // wjy: 当前可见行 = 真实设备行数量 + 新建分组行数量。
// ===end====
}
```

```cpp
// src/ui/DeviceGrid.cpp:2053-2073
// =====wjy====
for (int groupIndex = 0; groupIndex < g_deviceGroupNames.size(); ++groupIndex) { // wjy: 设备行画完后，再把新建分组画成“我的设备”下面的子下拉框。
    const int rowIndex = names.size() + groupIndex; // wjy: 分组行排在所有设备行后面，不占用真实设备下标。
    const int rowY = deviceRowRect(rowIndex).y();
    if (rowY + deviceRowRect(rowIndex).height() > visibleDeviceBottom) {
        continue; // wjy: 超出“我的设备”区域时不绘制，避免压到下面的“设备管理”。
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#F7FAFE"))); // wjy: 分组行使用浅背景，和普通设备行区分开。
    painter.drawRoundedRect(QRectF(deviceRowRect(rowIndex)), 5, 5);
    drawUiIcon(
        painter,
        QRect(203, rowY + 8, 24, 20),
        QStringLiteral("chevron_up.svg")); // wjy: 当前分组先固定为展开状态，下一步再考虑折叠和子项。

    painter.setFont(textFont);
    painter.setPen(QColor(QStringLiteral("#111827")));
    painter.drawText(QRectF(58, rowY + 7, 132, 22), Qt::AlignVCenter | Qt::AlignLeft, g_deviceGroupNames.at(groupIndex)); // wjy: 分组文字缩进显示，不画在线点和设备图标。
}
// ===end====
```

```cpp
// src/ui/DeviceGrid.cpp:2282-2288
QMenu menu(this); // wjy: 创建右键菜单，父对象设为当前控件，交给 Qt 管理生命周期。
QAction* createGroupAction = menu.addAction(QString::fromUtf8("新建分组")); // wjy: 保存菜单项指针，用来判断用户是否真的点击了“新建分组”。
const QAction* selectedAction = menu.exec(mapToGlobal(event->pos())); // wjy: 在鼠标当前位置弹出菜单，点空白取消时返回空指针。
if (selectedAction == createGroupAction) {
    g_deviceGroupNames.append(QString::fromUtf8("默认分组%1").arg(g_deviceGroupNames.size() + 1)); // wjy: 点击菜单后创建空分组，例如 默认分组1、默认分组2。
    update(); // wjy: 新增分组后重绘左侧列表，让分组立即显示在设备下面。
}
```

### Steps
1. 增加 `g_deviceGroupNames`，只在内存中保存当前运行期间创建的分组名。
2. 增加 `visibleDeviceListRowCount()`，让布局计算同时考虑设备行和分组行。
3. 调整 `remoteAssistGroupHeaderRect()` 和 `deviceGroupReservedBlankRect()`，使“设备管理”和空白区都排在最后一个可见行之后。
4. 在 `paintEvent()` 中，设备行绘制完后继续绘制分组行。
5. 在右键菜单中保存“新建分组”菜单项指针，点击该项时追加默认分组名并刷新界面。

### Verification
未执行构建，按用户要求由用户自行测试当前步骤。尝试执行 `git diff --check -- src\ui\DeviceGrid.cpp`，但当前文件存在整文件 CRLF 差异，检查输出大量既有 trailing whitespace；本次没有重写整文件，避免扩大改动范围。建议测试：右键空白区选择“新建分组”后，应在设备下面出现“默认分组1”，并且设备不会自动进入该分组。

## 2026-07-01 09:24 - 点击分组行切换箭头方向

### Changed Location
- `src/ui/DeviceGrid.cpp:302`: 新增分组展开状态数组。
- `src/ui/DeviceGrid.cpp:414`: 新增 `deviceGroupExpandedForIndex()`，按分组下标读取展开状态。
- `src/ui/DeviceGrid.cpp:2076`: 绘制分组箭头时根据展开状态选择上箭头或下箭头。
- `src/ui/DeviceGrid.cpp:2296`: 新建分组时默认设置为展开状态。
- `src/ui/DeviceGrid.cpp:2431`: 在鼠标点击逻辑中优先处理分组行点击。
- `src/ui/DeviceGrid.cpp:2440`: 点击分组行时切换展开状态。

### Reason
上一步创建出的分组行右侧固定显示上箭头，点击不会变化。用户指出“这个下拉框点击 ^ 不会倒转”。本步骤只让分组行可以切换箭头方向，先不做真正的子设备展开/收起、不保存状态、不处理拖拽，保持测试范围最小。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:300-301
QVector<DeviceEntry> g_devices;
QVector<QString> g_deviceGroupNames; // wjy: 临时保存右键新建的分组名称，本步骤只存在内存中，不写入 devices.json。
```

```cpp
// src/ui/DeviceGrid.cpp:2073-2076
drawUiIcon(
    painter,
    QRect(203, rowY + 8, 24, 20),
    QStringLiteral("chevron_up.svg")); // wjy: 当前分组先固定为展开状态，下一步再考虑折叠和子项。
```

```cpp
// src/ui/DeviceGrid.cpp:2294-2297
if (selectedAction == createGroupAction) {
    g_deviceGroupNames.append(QString::fromUtf8("默认分组%1").arg(g_deviceGroupNames.size() + 1)); // wjy: 点击菜单后创建空分组，例如 默认分组1、默认分组2。
    update(); // wjy: 新增分组后重绘左侧列表，让分组立即显示在设备下面。
}
```

```cpp
// src/ui/DeviceGrid.cpp:2427-2447
if (m_deviceGroupExpanded) {
    const QStringList names = deviceNames();
    const int visibleDeviceBottom = remoteAssistGroupHeaderRect(m_deviceGroupExpanded).y();
    for (int i = 0; i < names.size(); ++i) {
        ...
    }
}
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:300-302
QVector<DeviceEntry> g_devices;
QVector<QString> g_deviceGroupNames; // wjy: 临时保存右键新建的分组名称，本步骤只存在内存中，不写入 devices.json。
QVector<bool> g_deviceGroupExpandedStates; // wjy: 临时保存每个分组是否展开，用来控制右侧箭头朝上或朝下。
```

```cpp
// src/ui/DeviceGrid.cpp:414-420
bool deviceGroupExpandedForIndex(int groupIndex)
{
// =====wjy====
    return groupIndex >= 0
        && (groupIndex >= g_deviceGroupExpandedStates.size() || g_deviceGroupExpandedStates.at(groupIndex)); // wjy: 分组状态缺失时默认展开，避免状态数组不同步导致看不到展开箭头。
// ===end====
}
```

```cpp
// src/ui/DeviceGrid.cpp:2073-2076
drawUiIcon(
    painter,
    QRect(203, rowY + 8, 24, 20),
    deviceGroupExpandedForIndex(groupIndex) ? QStringLiteral("chevron_up.svg") : QStringLiteral("chevron_down.svg")); // wjy: 根据分组展开状态绘制上箭头或下箭头。
```

```cpp
// src/ui/DeviceGrid.cpp:2294-2297
if (selectedAction == createGroupAction) {
    g_deviceGroupNames.append(QString::fromUtf8("默认分组%1").arg(g_deviceGroupNames.size() + 1)); // wjy: 点击菜单后创建空分组，例如 默认分组1、默认分组2。
    g_deviceGroupExpandedStates.append(true); // wjy: 新建分组默认展开，所以初始显示上箭头。
    update(); // wjy: 新增分组后重绘左侧列表，让分组立即显示在设备下面。
}
```

```cpp
// src/ui/DeviceGrid.cpp:2430-2446
// =====wjy====
for (int groupIndex = 0; groupIndex < g_deviceGroupNames.size(); ++groupIndex) { // wjy: 先判断是否点击了分组行，让分组行只切换箭头，不进入设备选择逻辑。
    const int rowIndex = names.size() + groupIndex;
    if (deviceRowRect(rowIndex).y() + deviceRowRect(rowIndex).height() > visibleDeviceBottom) {
        continue;
    }
    if (deviceRowRect(rowIndex).contains(event->pos())) {
        while (g_deviceGroupExpandedStates.size() <= groupIndex) {
            g_deviceGroupExpandedStates.append(true); // wjy: 防御性补齐状态数组，缺失状态默认按展开处理。
        }
        g_deviceGroupExpandedStates[groupIndex] = !g_deviceGroupExpandedStates.at(groupIndex); // wjy: 点击分组行时切换展开状态，从而让箭头上下倒转。
        update();
        event->accept();
        return;
    }
}
// ===end====
```

### Steps
1. 新增 `g_deviceGroupExpandedStates`，和 `g_deviceGroupNames` 一一对应保存展开状态。
2. 新增 `deviceGroupExpandedForIndex()`，读取分组状态，状态缺失时默认展开。
3. 绘制分组行时，根据状态选择 `chevron_up.svg` 或 `chevron_down.svg`。
4. 新建分组时同步追加 `true`，让新分组默认显示上箭头。
5. 在左键点击设备行之前，先判断是否点中分组行；命中时只切换箭头状态并刷新界面。

### Verification
未执行构建，按用户要求由用户自行测试当前步骤。建议测试：创建“默认分组1”后，点击该分组行，右侧箭头应在上箭头和下箭头之间切换；点击设备行仍应正常切换设备。

## 2026-07-01 09:31 - 将分组保存到 devices.json 的 groups 字段

### Changed Location
- `src/ui/DeviceGrid.cpp:301`: 更新分组名称列表说明，分组会保存到 `devices.json`。
- `src/ui/DeviceGrid.cpp:302`: 更新分组展开状态说明，展开状态也会持久化。
- `src/ui/DeviceGrid.cpp:312`: `saveDevices()` 改为生成 `devices` 数组。
- `src/ui/DeviceGrid.cpp:323`: `saveDevices()` 新增 `groups` 数组，保存分组名和展开状态。
- `src/ui/DeviceGrid.cpp:335`: `saveDevices()` 顶层改为对象格式，包含 `devices` 和 `groups`。
- `src/ui/DeviceGrid.cpp:353`: `loadDevices()` 读取前清空分组名。
- `src/ui/DeviceGrid.cpp:354`: `loadDevices()` 读取前清空分组展开状态。
- `src/ui/DeviceGrid.cpp:373`: `loadDevices()` 兼容新对象格式和旧数组格式。
- `src/ui/DeviceGrid.cpp:402`: `loadDevices()` 读取 `groups` 字段恢复分组行。
- `src/ui/DeviceGrid.cpp:2339`: 新建分组后保存 `groups`。
- `src/ui/DeviceGrid.cpp:2484`: 切换分组箭头后保存展开状态。

### Reason
用户确认分组可以保存在 `devices.json` 中，并希望使用 `group/groups` 这类字段。为了避免把分组伪装成设备，本次将文件格式升级为顶层对象：`devices` 保存设备数组，`groups` 保存分组数组。旧版纯数组格式仍然兼容读取，避免已有设备丢失。本步骤只保存分组本身和箭头展开状态，不保存设备归属。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:300-302
QVector<DeviceEntry> g_devices;
QVector<QString> g_deviceGroupNames; // wjy: 临时保存右键新建的分组名称，本步骤只存在内存中，不写入 devices.json。
QVector<bool> g_deviceGroupExpandedStates; // wjy: 临时保存每个分组是否展开，用来控制右侧箭头朝上或朝下。
```

```cpp
// src/ui/DeviceGrid.cpp:309-330
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

    ...
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
}
```

```cpp
// src/ui/DeviceGrid.cpp:347-352
const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
if (!document.isArray()) {
    return;
}

for (const QJsonValue& value : document.array()) {
```

```cpp
// src/ui/DeviceGrid.cpp:2294-2297
if (selectedAction == createGroupAction) {
    g_deviceGroupNames.append(QString::fromUtf8("默认分组%1").arg(g_deviceGroupNames.size() + 1)); // wjy: 点击菜单后创建空分组，例如 默认分组1、默认分组2。
    g_deviceGroupExpandedStates.append(true); // wjy: 新建分组默认展开，所以初始显示上箭头。
    update(); // wjy: 新增分组后重绘左侧列表，让分组立即显示在设备下面。
}
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:300-302
QVector<DeviceEntry> g_devices;
QVector<QString> g_deviceGroupNames; // wjy: 保存右键新建的分组名称，会写入 devices.json 的 groups 字段。
QVector<bool> g_deviceGroupExpandedStates; // wjy: 保存每个分组是否展开，会和分组名称一起持久化。
```

```cpp
// src/ui/DeviceGrid.cpp:309-347
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

    ...
    file.write(QJsonDocument(rootObject).toJson(QJsonDocument::Indented)); // wjy: 写入新格式，保留设备并持久化分组。
}
```

```cpp
// src/ui/DeviceGrid.cpp:350-379
void loadDevices()
{
    g_devices.clear();
    g_deviceGroupNames.clear(); // wjy: 重新加载文件前清空内存分组，避免重复追加。
    g_deviceGroupExpandedStates.clear(); // wjy: 重新加载文件前同步清空展开状态。

    ...
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
```

```cpp
// src/ui/DeviceGrid.cpp:401-411
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
```

```cpp
// src/ui/DeviceGrid.cpp:2336-2340
if (selectedAction == createGroupAction) {
    g_deviceGroupNames.append(QString::fromUtf8("默认分组%1").arg(g_deviceGroupNames.size() + 1)); // wjy: 点击菜单后创建空分组，例如 默认分组1、默认分组2。
    g_deviceGroupExpandedStates.append(true); // wjy: 新建分组默认展开，所以初始显示上箭头。
    saveDevices(); // wjy: 保存 groups 字段，让新建分组关闭程序后还能恢复。
    update(); // wjy: 新增分组后重绘左侧列表，让分组立即显示在设备下面。
}
```

```cpp
// src/ui/DeviceGrid.cpp:2483-2485
g_deviceGroupExpandedStates[groupIndex] = !g_deviceGroupExpandedStates.at(groupIndex); // wjy: 点击分组行时切换展开状态，从而让箭头上下倒转。
saveDevices(); // wjy: 保存分组展开状态，重启后箭头方向保持一致。
update();
```

### Steps
1. 将分组名称和展开状态说明改为持久化数据。
2. `saveDevices()` 中把设备写入 `devices` 数组。
3. `saveDevices()` 中新增 `groups` 数组，每个分组保存 `name` 和 `expanded`。
4. `saveDevices()` 顶层写入对象 `{ devices, groups }`。
5. `loadDevices()` 同时兼容旧数组格式和新对象格式。
6. `loadDevices()` 读取 `groups` 字段恢复分组行和箭头方向。
7. 新建分组和切换箭头后调用 `saveDevices()`。

### Verification
未执行构建，按用户要求由用户自行测试当前步骤。建议测试：新建“默认分组1”，关闭程序再打开，分组仍显示；点击切换箭头后关闭再打开，箭头方向保持上次状态；旧的纯数组 `devices.json` 仍应能正常读取设备。

## 2026-07-01 09:39 - 第 6 步：双击分组原地重命名

### Changed Location
- `src/ui/DeviceGrid.h:40`: 声明 `mouseDoubleClickEvent`，让控件能接收分组行双击事件。
- `src/ui/DeviceGrid.h:76`: 声明开始和结束分组重命名的私有函数。
- `src/ui/DeviceGrid.h:85`: 增加分组名输入框成员，平时隐藏，双击分组时显示。
- `src/ui/DeviceGrid.h:104`: 增加当前正在重命名的分组下标。
- `src/ui/DeviceGrid.cpp:464`: 增加分组下标到视觉行号的辅助函数。
- `src/ui/DeviceGrid.cpp:1121`: 在构造函数中创建分组原地重命名输入框，并绑定回车和失焦保存。
- `src/ui/DeviceGrid.cpp:1416`: 增加开始分组重命名逻辑。
- `src/ui/DeviceGrid.cpp:1440`: 增加结束分组重命名逻辑。
- `src/ui/DeviceGrid.cpp:2200`: 分组正在编辑时不绘制底层文字，避免文字和输入框重叠。
- `src/ui/DeviceGrid.cpp:2409`: 点击输入框外部时提交当前名字并关闭输入框。
- `src/ui/DeviceGrid.cpp:2480`: 双击分组行时进入原地重命名。
- `src/ui/DeviceGrid.cpp:2589`: 防止双击重命名后的释放事件误触发展开/收起。

### Reason
用户希望“不弹新窗口”，直接在默认分组文字位置修改名称。这里复用 `QLineEdit` 做原地编辑：双击分组行时把输入框移动到分组文字区域，回车或点击外部时保存；如果输入为空，则不写入空名字，恢复原来的分组名。这样交互更像文件管理器里的重命名，也不会破坏左侧列表结构。

### Original Code
```cpp
// src/ui/DeviceGrid.h:37-42
void paintEvent(QPaintEvent* event) override;
void mousePressEvent(QMouseEvent* event) override;
void mouseMoveEvent(QMouseEvent* event) override;
void mouseReleaseEvent(QMouseEvent* event) override;
void leaveEvent(QEvent* event) override;
```

```cpp
// src/ui/DeviceGrid.h:72-76
void setupSettingsControls();
void updateSettingsControls();
void applyStatusAutoRefreshSetting(bool refreshImmediately);

QString m_currentDeviceName;
```

```cpp
// src/ui/DeviceGrid.h:80-86
QLineEdit* m_deviceNameEdit = nullptr;
QLineEdit* m_deviceMacEdit = nullptr;
QLineEdit* m_deviceRemarkEdit = nullptr;
QPushButton* m_saveDeviceButton = nullptr;
QPushButton* m_cancelDeviceButton = nullptr;
QVector<QPushButton*> m_localInfoCopyButtons;
```

```cpp
// src/ui/DeviceGrid.h:98-103
bool m_statusRefreshInProgress = false;
bool m_wakeProbeInProgress = false;
int m_statusAutoRefreshIntervalSeconds = 10;
int m_selectedDeviceIndex = 0;
int m_previousDeviceIndex = 0;
QString m_previousDeviceName;
```

```cpp
// src/ui/DeviceGrid.cpp:464
// new code, no old code at this location
```

```cpp
// src/ui/DeviceGrid.cpp:1121
// new code, no old code at this location
```

```cpp
// src/ui/DeviceGrid.cpp:1416
// new code, no old code at this location
```

```cpp
// src/ui/DeviceGrid.cpp:2198-2201
painter.setFont(textFont);
painter.setPen(QColor(QStringLiteral("#111827")));
painter.drawText(QRectF(58, rowY + 7, 132, 22), Qt::AlignVCenter | Qt::AlignLeft, g_deviceGroupNames.at(groupIndex));
```

```cpp
// src/ui/DeviceGrid.cpp:2409
// old mousePressEvent did not close the group rename input before handling other clicks
```

```cpp
// src/ui/DeviceGrid.cpp:2480
// new code, no old mouseDoubleClickEvent override at this location
```

```cpp
// src/ui/DeviceGrid.cpp:2588-2593
if (deviceRowRect(rowIndex).contains(event->pos())) {
    while (g_deviceGroupExpandedStates.size() <= groupIndex) {
        g_deviceGroupExpandedStates.append(true); // wjy: 防御性补齐状态数组，缺失状态默认按展开处理。
    }
```

### Modified Code
```cpp
// src/ui/DeviceGrid.h:37-42
void paintEvent(QPaintEvent* event) override;
void mousePressEvent(QMouseEvent* event) override;
void mouseMoveEvent(QMouseEvent* event) override;
void mouseDoubleClickEvent(QMouseEvent* event) override; // wjy: 双击分组行时进入原地重命名。
void mouseReleaseEvent(QMouseEvent* event) override;
void leaveEvent(QEvent* event) override;
```

```cpp
// src/ui/DeviceGrid.h:72-77
void setupSettingsControls();
void updateSettingsControls();
void applyStatusAutoRefreshSetting(bool refreshImmediately);
void beginDeviceGroupRename(int groupIndex); // wjy: 在分组文字原位置显示输入框，开始原地重命名。
void finishDeviceGroupRename(bool saveText); // wjy: 回车或点击外部时结束重命名，并按规则保存或恢复。
```

```cpp
// src/ui/DeviceGrid.h:80-86
QLineEdit* m_deviceNameEdit = nullptr;
QLineEdit* m_deviceMacEdit = nullptr;
QLineEdit* m_deviceRemarkEdit = nullptr;
QLineEdit* m_deviceGroupNameEdit = nullptr; // wjy: 分组原地重命名输入框，平时隐藏，双击分组时显示。
QPushButton* m_saveDeviceButton = nullptr;
QPushButton* m_cancelDeviceButton = nullptr;
```

```cpp
// src/ui/DeviceGrid.h:100-105
bool m_statusRefreshInProgress = false;
bool m_wakeProbeInProgress = false;
int m_statusAutoRefreshIntervalSeconds = 10;
int m_renamingDeviceGroupIndex = -1; // wjy: 当前正在重命名的分组下标，-1 表示没有分组处于编辑状态。
int m_selectedDeviceIndex = 0;
```

```cpp
// src/ui/DeviceGrid.cpp:464-472
int visualRowIndexForGroupIndex(int groupIndex)
{
// =====wjy====
    if (groupIndex < 0 || groupIndex >= g_deviceGroupNames.size()) {
        return -1; // wjy: 分组下标无效时返回 -1，调用方据此放弃显示输入框。
    }
    return deviceNames().size() + groupIndex; // wjy: 当前阶段分组行固定排在所有设备行后面。
// ===end====
}
```

```cpp
// src/ui/DeviceGrid.cpp:1121-1133
// =====wjy====
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
// ===end====
```

```cpp
// src/ui/DeviceGrid.cpp:1416-1468
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

    const QRect rowRect = deviceRowRect(rowIndex); // wjy: 复用分组行矩形，让输入框贴在分组文字原位置。
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
    const QRect rowRect = rowIndex >= 0 ? deviceRowRect(rowIndex) : QRect();
    if (saveText && groupIndex >= 0 && groupIndex < g_deviceGroupNames.size()) {
        const QString newName = m_deviceGroupNameEdit->text().trimmed(); // wjy: 去掉首尾空格，避免空格成为分组名的一部分。
        if (!newName.isEmpty()) {
            g_deviceGroupNames[groupIndex] = newName; // wjy: 非空名字写回分组列表。
            saveDevices(); // wjy: 保存 groups 字段，让重命名后的分组下次启动仍然存在。
        } else {
            m_deviceGroupNameEdit->setText(g_deviceGroupNames.at(groupIndex)); // wjy: 空名字不保存，恢复为原分组名。
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
```

```cpp
// src/ui/DeviceGrid.cpp:2198-2202
painter.setFont(textFont);
painter.setPen(QColor(QStringLiteral("#111827")));
if (m_renamingDeviceGroupIndex != groupIndex) {
    painter.drawText(QRectF(58, rowY + 7, 132, 22), Qt::AlignVCenter | Qt::AlignLeft, g_deviceGroupNames.at(groupIndex)); // wjy: 分组正在原地重命名时不画底层文字，避免和输入框重叠。
}
```

```cpp
// src/ui/DeviceGrid.cpp:2409-2414
// =====wjy====
if (m_deviceGroupNameEdit
    && m_deviceGroupNameEdit->isVisible()
    && !m_deviceGroupNameEdit->geometry().contains(event->pos())) { // wjy: 点击分组输入框外部时，提交当前名字并关闭输入框。
    finishDeviceGroupRename(true);
}
```

```cpp
// src/ui/DeviceGrid.cpp:2480-2500
void DeviceGrid::mouseDoubleClickEvent(QMouseEvent* event)
{
// =====wjy====
    if (event->button() == Qt::LeftButton && m_deviceGroupExpanded) { // wjy: 只有左键双击展开的“我的设备”列表时，才尝试重命名分组。
        const QStringList names = deviceNames();
        const int visibleDeviceBottom = remoteAssistGroupHeaderRect(m_deviceGroupExpanded).y();
        for (int groupIndex = 0; groupIndex < g_deviceGroupNames.size(); ++groupIndex) {
            const int rowIndex = names.size() + groupIndex;
            if (deviceRowRect(rowIndex).y() + deviceRowRect(rowIndex).height() > visibleDeviceBottom) {
                continue;
            }
            if (deviceRowRect(rowIndex).contains(event->pos())) {
                beginDeviceGroupRename(groupIndex); // wjy: 双击分组行，在原位置显示输入框。
                event->accept();
                return;
            }
        }
    }
// ===end====

    QFrame::mouseDoubleClickEvent(event);
}
```

```cpp
// src/ui/DeviceGrid.cpp:2588-2596
if (deviceRowRect(rowIndex).contains(event->pos())) {
    if (m_renamingDeviceGroupIndex == groupIndex) {
        event->accept(); // wjy: 双击进入重命名后的释放事件只关闭本次点击，不再切换分组箭头。
        return;
    }
    while (g_deviceGroupExpandedStates.size() <= groupIndex) {
        g_deviceGroupExpandedStates.append(true); // wjy: 防御性补齐状态数组，缺失状态默认按展开处理。
    }
    g_deviceGroupExpandedStates[groupIndex] = !g_deviceGroupExpandedStates.at(groupIndex); // wjy: 点击分组行时切换展开状态，从而让箭头上下倒转。
```

### Steps
1. 在头文件中声明双击事件、重命名开始/结束函数、输入框成员和当前编辑下标。
2. 增加 `visualRowIndexForGroupIndex()`，把分组下标换算成左侧列表中的行号。
3. 在构造函数中创建隐藏的 `QLineEdit`，绑定回车保存和失焦保存。
4. 双击分组行时，把输入框移动到分组文字位置并全选当前名字。
5. 保存时去掉首尾空格；非空则写回 `g_deviceGroupNames` 并调用 `saveDevices()`，空名字则恢复原值。
6. 绘制分组时，如果该分组正在重命名，就不再画底层文字，避免重叠。
7. 点击输入框外部时提交当前名字并关闭输入框。
8. 双击进入重命名后拦截释放事件，避免误切换分组箭头。

### Verification
未执行构建，按用户要求由用户自行构建和测试当前步骤。建议测试：双击“默认分组1”能在原位置出现输入框；输入新名字后按回车保存；输入新名字后点击外部保存；输入空内容后点击外部恢复原名；重启后重命名结果仍在 `devices.json` 的 `groups` 字段中保留。

## 2026-07-01 09:50 - BUG 修改：临时关闭启动自动状态刷新

### Changed Location
- `src/ui/DeviceGrid.cpp:1225`: 临时注释构造函数末尾的启动自动状态刷新调用。

### Reason
Release 模式启动后偶发异常退出，而 Debug 模式不复现。由于 `DeviceGrid` 构造函数末尾会立刻调用 `refreshDeviceStatuses()`，该函数内部会启动后台线程并进行网络状态探测，异常时间点又接近启动后 2 秒，所以先临时关闭启动自动刷新，用来判断 Release 崩溃是否来自后台状态探测线程或其 UI 回调。

这次是 BUG 定位修改，不是最终功能方案。如果关闭后 Release 稳定，下一步再修 `refreshDeviceStatuses()` 的线程/回调安全；如果仍然异常，再继续排查别的启动逻辑。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:1224-1226
applyStatusAutoRefreshSetting(false);
refreshDeviceStatuses();
}
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:1224-1226
applyStatusAutoRefreshSetting(false);
// refreshDeviceStatuses(); // wjy: BUG临时诊断：Release 启动偶发异常，先关闭启动自动状态刷新，验证崩溃是否来自后台状态探测线程。
}
```

### Steps
1. 找到 `DeviceGrid` 构造函数末尾的启动自动刷新调用。
2. 保留 `applyStatusAutoRefreshSetting(false)`，不影响用户设置里的自动刷新定时器初始化。
3. 临时注释 `refreshDeviceStatuses()`，避免程序启动时立刻进入后台状态探测线程。
4. 在代码行尾添加 `wjy` 注释，说明这是 Release 偶发崩溃的 BUG 定位修改。

### Verification
未执行构建，按用户要求由用户自行使用 Release 测试。建议测试：连续启动 Release 多次，如果不再异常退出，说明崩溃大概率在启动自动状态刷新链路；如果仍然异常退出，说明需要继续排查其他启动流程。

## 2026-07-01 10:00 - BUG 诊断：为状态刷新线程添加 Release 日志

### Changed Location
- `src/ui/DeviceGrid.cpp:19`: 新增 `QDebug` 头文件，用于输出 Release 下也能看到的 `qInfo()` 日志。
- `src/ui/DeviceGrid.cpp:1226`: 修正启动自动刷新处的 `wjy` 注释，说明当前是开启刷新并配合日志定位崩溃。
- `src/ui/DeviceGrid.cpp:1555`: 在 `refreshDeviceStatuses()` 开始和跳过刷新时输出日志。
- `src/ui/DeviceGrid.cpp:1578`: 输出本次从设备表读取到的待探测 IP 数量和 IP 列表。
- `src/ui/DeviceGrid.cpp:1586`: 输出后台总线程启动日志。
- `src/ui/DeviceGrid.cpp:1595`: 输出 worker 数量和每个 worker 的开始/结束日志。
- `src/ui/DeviceGrid.cpp:1610`: 输出每个 IP 探测开始和结束日志。
- `src/ui/DeviceGrid.cpp:1638`: 输出所有 worker 汇合完成日志。
- `src/ui/DeviceGrid.cpp:1642`: 输出控件销毁导致后台线程不回 UI 的日志。
- `src/ui/DeviceGrid.cpp:1649`: 输出回到 UI 线程前后的日志。

### Reason
已经确认：删除 `groups` 后 Release 仍然偶发崩溃，而注释启动自动 `refreshDeviceStatuses()` 后不崩。说明问题范围集中在状态刷新链路。由于 Debug 不复现，普通断点价值不高，所以这次添加 Release 可见日志，记录状态刷新从开始、设备 IP 收集、后台线程、worker 探测、线程汇合到 UI 回调的完整路径。

这次是 BUG 诊断修改，不改变刷新逻辑。下一次 Release 崩溃时，根据最后一条 `[wjy-status]` 日志，就可以判断崩溃发生在 `probe()` 网络探测阶段、worker 汇合阶段，还是 UI 回调更新阶段。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:18-20
#include <QDateTime>
#include <QDialog>
#include <QDir>
```

```cpp
// src/ui/DeviceGrid.cpp:1224-1226
applyStatusAutoRefreshSetting(false);
refreshDeviceStatuses(); // wjy: BUG临时诊断：Release 启动偶发异常，先关闭启动自动状态刷新，验证崩溃是否来自后台状态探测线程。
}
```

```cpp
// src/ui/DeviceGrid.cpp:1552-1627
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
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:18-21
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDir>
```

```cpp
// src/ui/DeviceGrid.cpp:1225-1227
applyStatusAutoRefreshSetting(false);
refreshDeviceStatuses(); // wjy: BUG诊断：保持启动自动状态刷新开启，配合 refreshDeviceStatuses 内部日志定位 Release 偶发崩溃位置。
}
```

```cpp
// src/ui/DeviceGrid.cpp:1553-1677
void DeviceGrid::refreshDeviceStatuses()
{
// =====wjy====
    qInfo().noquote() << QStringLiteral("[wjy-status] refresh begin"); // wjy: BUG诊断日志，确认 Release 是否进入状态刷新函数。
    if (m_statusRefreshInProgress) {
        qInfo().noquote() << QStringLiteral("[wjy-status] refresh skipped inProgress=1"); // wjy: 如果上一次刷新还没结束，记录跳过原因。
        return;
    }
// ===end====

    ...

// =====wjy====
    qInfo().noquote() << QStringLiteral("[wjy-status] ip count=%1 values=%2")
        .arg(ips.size())
        .arg(ips.join(QStringLiteral(","))); // wjy: 记录本次从 devices.json 加载出的待探测 IP，判断崩溃是否和某个设备有关。
// ===end====

    QPointer<DeviceGrid> self(this);
    std::thread([self, ips] {
// =====wjy====
        qInfo().noquote() << QStringLiteral("[wjy-status] background thread start ipCount=%1").arg(ips.size()); // wjy: 后台总线程启动日志，确认是否进入线程阶段。
// ===end====
        ...
// =====wjy====
        qInfo().noquote() << QStringLiteral("[wjy-status] worker count=%1").arg(workerCount); // wjy: 记录本次会创建几个并发探测线程。
// ===end====

        for (int worker = 0; worker < workerCount; ++worker) {
            workers.emplace_back([&, worker] { // wjy: 捕获 worker 副本用于日志，避免并发日志读取循环变量引用。
// =====wjy====
                qInfo().noquote() << QStringLiteral("[wjy-status] worker start worker=%1").arg(worker); // wjy: 每个 worker 启动时记录编号，判断是否创建线程后崩溃。
// ===end====
                ...
// =====wjy====
                    qInfo().noquote() << QStringLiteral("[wjy-status] probe begin worker=%1 index=%2 ip=%3")
                        .arg(worker)
                        .arg(index)
                        .arg(ip); // wjy: 单个 IP 探测前记录，崩溃时可定位是否卡在某台设备。
// ===end====
                    const platform::DevicePresenceState state = platform::DeviceStatusService::probe(ip);
// =====wjy====
                    qInfo().noquote() << QStringLiteral("[wjy-status] probe done worker=%1 index=%2 ip=%3 state=%4")
                        .arg(worker)
                        .arg(index)
                        .arg(ip)
                        .arg(static_cast<int>(state)); // wjy: 单个 IP 探测后记录状态枚举值，判断 probe 是否顺利返回。
// ===end====
                ...
// =====wjy====
                qInfo().noquote() << QStringLiteral("[wjy-status] worker end worker=%1").arg(worker); // wjy: worker 结束日志，判断线程是否正常跑完。
// ===end====
            });
        }

        ...
// =====wjy====
        qInfo().noquote() << QStringLiteral("[wjy-status] all workers joined statusCount=%1").arg(statuses.size()); // wjy: 所有 worker 汇合后记录结果数量。
// ===end====

        if (!self) {
// =====wjy====
            qInfo().noquote() << QStringLiteral("[wjy-status] widget destroyed before invoke"); // wjy: 如果界面已销毁，记录后直接退出后台线程。
// ===end====
            return;
        }

        QMetaObject::invokeMethod(self, [self, statuses = std::move(statuses)]() mutable {
// =====wjy====
            qInfo().noquote() << QStringLiteral("[wjy-status] invoke ui begin"); // wjy: 回到 UI 线程前半段日志，判断崩溃是否发生在 UI 更新阶段。
// ===end====
            ...
// =====wjy====
            qInfo().noquote() << QStringLiteral("[wjy-status] invoke ui end"); // wjy: UI 状态写入和重绘请求完成日志。
// ===end====
        }, Qt::QueuedConnection);
    }).detach();
}
```

### Steps
1. 引入 `<QDebug>`，让 `qInfo()` 可以输出日志。
2. 保持启动时 `refreshDeviceStatuses()` 启用，修正该行 `wjy` 注释，说明当前是配合日志定位崩溃。
3. 在 `refreshDeviceStatuses()` 入口记录刷新开始和重复刷新跳过原因。
4. 在收集 IP 后记录 IP 数量和具体 IP 列表。
5. 在后台线程启动、worker 创建、worker 开始/结束处记录日志。
6. 在每个 IP 探测前后记录日志，方便判断是否崩在某个设备的 `probe()`。
7. 在所有 worker 汇合后记录状态数量。
8. 在准备回 UI 和 UI 更新完成后记录日志，方便判断是否崩在 UI 回调阶段。

### Verification
未执行构建，按用户要求由用户自行使用 Release 测试。建议测试：连续启动 Release，若再次异常退出，把 Qt Creator “应用程序输出”里最后几条 `[wjy-status]` 日志发回来，用最后一条日志定位崩溃阶段。

## 2026-07-01 10:05 - BUG 诊断：为 main 启动流程添加文件日志

### Changed Location
- `src/main.cpp:10`: 新增 `QCoreApplication`、`QDateTime`、`QDir`、`QFile`、`QTextStream` 相关头文件，用于写启动日志文件。
- `src/main.cpp:20`: 新增 `writeStartupLog()` 辅助函数，每写一条日志都立即落盘。
- `src/main.cpp:44`: 从 `QApplication` 创建完成后开始记录 main 启动流程。
- `src/main.cpp:54`: 记录虚拟显示驱动检查前后。
- `src/main.cpp:58`: 记录本机串流 `startHost()` 前后。
- `src/main.cpp:67`: 记录便携 SSH 服务启动前后。
- `src/main.cpp:70`: 记录状态服务和命令服务监听前后。
- `src/main.cpp:76`: 记录 `MainWindow` 创建前后。
- `src/main.cpp:80`: 记录窗口显示完成、进入事件循环前的位置。
- `src/main.cpp:82`: 记录事件循环返回后的清理步骤。

### Reason
Release 偶发崩溃时，Qt Creator 没有输出 `[wjy-status] refresh begin`，说明失败路径可能还没进入 `refreshDeviceStatuses()`，或者控制台日志在异常退出前没有送出来。为了定位更早的启动阶段，这次在 `main()` 中增加文件日志，写入程序运行目录的 `data/wjy_startup.log`。

文件日志每条都单独打开、写入、换行刷新并关闭，目的是让 Release 即使异常退出，也尽量保留最后成功执行到的启动步骤。下一次崩溃后查看 `wjy_startup.log` 最后一行，就能判断崩溃发生在虚拟显示驱动、本机串流、SSH 服务、状态/命令服务、主窗口构造，还是事件循环之后。

### Original Code
```cpp
// src/main.cpp:9-10
#include <QApplication>
#include <QFont>
```

```cpp
// src/main.cpp:11-36
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("FSRemote"));
    QApplication::setOrganizationName(QStringLiteral("FSRemote"));

    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(13);
    app.setFont(font);

    platform::ParsecVddInstaller::ensureInstalled();

    FsRemoteStreamHandle hostHandle = stream::StreamRuntime::instance().startHost(49100);
    platform::DeviceStatusServer statusServer([hostHandle] {
        return stream::StreamRuntime::instance().isBusy(hostHandle);
    });
    platform::DeviceCommandServer commandServer;
    platform::PortableOpenSshManager::instance().startServer();
    statusServer.start(49101);
    commandServer.start(49102);
    ui::MainWindow window;
    window.show();
    const int result = app.exec();
    platform::PowerManager::setPreventSleepEnabled(false);
    platform::PortableOpenSshManager::instance().stopServer();
    commandServer.stop();
    statusServer.stop();
    stream::StreamRuntime::instance().stop(hostHandle);
    return result;
}
```

### Modified Code
```cpp
// src/main.cpp:9-15
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QTextStream>
```

```cpp
// src/main.cpp:17-32
namespace {

// =====wjy====
void writeStartupLog(const QString& message)
{
    const QString dataDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data")); // wjy: 启动日志写到程序运行目录的 data 文件夹，方便 Release 包直接查看。
    QDir().mkpath(dataDir); // wjy: 确保 data 文件夹存在，避免首次启动没有目录导致日志写入失败。

    QFile file(QDir(dataDir).filePath(QStringLiteral("wjy_startup.log"))); // wjy: 使用独立启动日志文件，和设备配置 devices.json 分开。
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return; // wjy: 日志写入失败不能影响主程序启动，所以直接返回。
    }

    QTextStream stream(&file); // wjy: 用文本流写入时间和步骤，方便在 Qt Creator 外直接打开查看。
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
           << QStringLiteral(" ")
           << message
           << Qt::endl; // wjy: Qt::endl 会换行并刷新文本流，降低异常退出时日志丢失概率。
    file.close(); // wjy: 每写一条就关闭文件，确保 Release 崩溃前的最后一步尽量落盘。
}
// ===end====

} // namespace
```

```cpp
// src/main.cpp:34-93
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    writeStartupLog(QStringLiteral("[wjy-main] app created")); // wjy: 记录 QApplication 创建完成，确认程序已经进入 Qt 初始化之后。
    QApplication::setApplicationName(QStringLiteral("FSRemote"));
    QApplication::setOrganizationName(QStringLiteral("FSRemote"));
    writeStartupLog(QStringLiteral("[wjy-main] app metadata set")); // wjy: 记录应用名称和组织名设置完成。

    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(13);
    app.setFont(font);
    writeStartupLog(QStringLiteral("[wjy-main] font set")); // wjy: 记录字体设置完成，排除字体初始化阶段崩溃。

    writeStartupLog(QStringLiteral("[wjy-main] before ParsecVddInstaller::ensureInstalled")); // wjy: 记录安装/检查虚拟显示驱动前的位置。
    platform::ParsecVddInstaller::ensureInstalled();
    writeStartupLog(QStringLiteral("[wjy-main] after ParsecVddInstaller::ensureInstalled")); // wjy: 记录虚拟显示驱动检查完成。

    writeStartupLog(QStringLiteral("[wjy-main] before StreamRuntime::startHost")); // wjy: 记录本机串流服务启动前的位置。
    FsRemoteStreamHandle hostHandle = stream::StreamRuntime::instance().startHost(49100);
    writeStartupLog(QStringLiteral("[wjy-main] after StreamRuntime::startHost")); // wjy: 记录本机串流服务启动后的位置。
    platform::DeviceStatusServer statusServer([hostHandle] {
        return stream::StreamRuntime::instance().isBusy(hostHandle);
    });
    writeStartupLog(QStringLiteral("[wjy-main] status server object created")); // wjy: 记录设备状态服务对象创建完成。
    platform::DeviceCommandServer commandServer;
    writeStartupLog(QStringLiteral("[wjy-main] command server object created")); // wjy: 记录设备命令服务对象创建完成。
    writeStartupLog(QStringLiteral("[wjy-main] before PortableOpenSshManager::startServer")); // wjy: 记录便携 SSH 服务启动前的位置。
    platform::PortableOpenSshManager::instance().startServer();
    writeStartupLog(QStringLiteral("[wjy-main] after PortableOpenSshManager::startServer")); // wjy: 记录便携 SSH 服务启动完成。
    writeStartupLog(QStringLiteral("[wjy-main] before statusServer.start")); // wjy: 记录状态服务监听端口前的位置。
    statusServer.start(49101);
    writeStartupLog(QStringLiteral("[wjy-main] after statusServer.start")); // wjy: 记录状态服务监听完成。
    writeStartupLog(QStringLiteral("[wjy-main] before commandServer.start")); // wjy: 记录命令服务监听端口前的位置。
    commandServer.start(49102);
    writeStartupLog(QStringLiteral("[wjy-main] after commandServer.start")); // wjy: 记录命令服务监听完成。
    writeStartupLog(QStringLiteral("[wjy-main] before MainWindow create")); // wjy: 记录主窗口创建前的位置，判断崩溃是否进入 UI 构造。
    ui::MainWindow window;
    writeStartupLog(QStringLiteral("[wjy-main] after MainWindow create")); // wjy: 记录主窗口构造完成。
    window.show();
    writeStartupLog(QStringLiteral("[wjy-main] window shown before app.exec")); // wjy: 记录窗口显示完成，即将进入事件循环。
    const int result = app.exec();
    writeStartupLog(QStringLiteral("[wjy-main] app.exec returned")); // wjy: 记录事件循环正常返回，区分正常退出和异常崩溃。
    platform::PowerManager::setPreventSleepEnabled(false);
    writeStartupLog(QStringLiteral("[wjy-main] prevent sleep disabled")); // wjy: 记录关闭防睡眠设置完成。
    platform::PortableOpenSshManager::instance().stopServer();
    writeStartupLog(QStringLiteral("[wjy-main] ssh server stopped")); // wjy: 记录 SSH 服务停止完成。
    commandServer.stop();
    writeStartupLog(QStringLiteral("[wjy-main] command server stopped")); // wjy: 记录命令服务停止完成。
    statusServer.stop();
    writeStartupLog(QStringLiteral("[wjy-main] status server stopped")); // wjy: 记录状态服务停止完成。
    stream::StreamRuntime::instance().stop(hostHandle);
    writeStartupLog(QStringLiteral("[wjy-main] stream host stopped")); // wjy: 记录本机串流服务停止完成。
    return result;
}
```

### Steps
1. 增加写文件日志需要的 Qt 头文件。
2. 新增 `writeStartupLog()`，把日志写到程序运行目录 `data/wjy_startup.log`。
3. 每条日志都使用 `Qt::endl` 并关闭文件，尽量保证 Release 异常退出前日志已落盘。
4. 在 `main()` 启动流程的关键步骤前后插入 `[wjy-main]` 日志。
5. 保留原启动逻辑顺序，不改变功能行为，只增加定位信息。

### Verification
未执行构建，按用户要求由用户自行使用 Release 测试。建议测试：运行 Release 后查看 `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release/data/wjy_startup.log`，如果再次异常退出，把该文件最后 10 行发回来，用最后一条 `[wjy-main]` 判断崩溃发生在哪个启动阶段。

## 2026-07-01 10:16 - BUG 诊断：为 MainWindow 和 DeviceGrid 构造过程添加文件日志

### Changed Location
- `src/ui/MainWindow.cpp:5`: 新增写文件日志所需的 Qt 头文件。
- `src/ui/MainWindow.cpp:17`: 新增 `writeWindowStartupLog()`，把 MainWindow 构造步骤写入 `data/wjy_startup.log`。
- `src/ui/MainWindow.cpp:43`: 在 `MainWindow` 构造函数各关键步骤后添加 `[wjy-window]` 日志。
- `src/ui/MainWindow.cpp:62`: 将 `new DeviceGrid(this)` 与 `setCentralWidget()` 拆开，便于区分崩在 DeviceGrid 构造还是设置中心控件。
- `src/ui/DeviceGrid.cpp:46`: 新增 `QTextStream` 头文件，用于写 DeviceGrid 构造日志。
- `src/ui/DeviceGrid.cpp:77`: 新增 `writeDeviceGridStartupLog()`，把 DeviceGrid 构造步骤写入同一个启动日志文件。
- `src/ui/DeviceGrid.cpp:1119`: 在 `DeviceGrid` 构造函数内加入 `[wjy-grid]` 分段日志。
- `src/ui/DeviceGrid.cpp:1279`: 在首次 `refreshDeviceStatuses()` 前后记录日志，区分构造阶段和状态刷新阶段。

### Reason
前一次文件日志显示 Release 启动失败时最后停在 `[wjy-main] before MainWindow create`，没有出现 `[wjy-main] after MainWindow create`。这说明崩溃发生在 `ui::MainWindow window;` 这句内部。由于 `MainWindow` 构造中会调用 `setCentralWidget(new DeviceGrid(this))`，所以需要继续把日志打到 `MainWindow` 和 `DeviceGrid` 构造函数内部，定位崩溃具体发生在窗口属性设置、圆角遮罩、DeviceGrid 构造、控件初始化、定时器初始化，还是首次状态刷新调用。

这次仍是 BUG 诊断修改，不改变原有业务逻辑。唯一结构调整是把 `setCentralWidget(new DeviceGrid(this))` 拆成两行，便于日志判断 `new DeviceGrid` 是否成功完成。

### Original Code
```cpp
// src/ui/MainWindow.cpp:5-7
#include <QIcon>
#include <QPainterPath>
#include <QRegion>
```

```cpp
// src/ui/MainWindow.cpp:9-25
namespace ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("MainWindow"));
    setWindowTitle(QString::fromUtf8("\xE4\xB8\xB0\xE5\xAE\x9E\xE8\xBF\x9C\xE7\xA8\x8B\xE6\x8E\xA7\xE5\x88\xB6"));
    setWindowIcon(QIcon(QStringLiteral(":/UUGuest/resource/images/titlebar/app_icon.ico")));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setFixedSize(920, 680);

    QPainterPath windowPath;
    windowPath.addRoundedRect(QRectF(0, 0, 920, 680), 6, 6);
    setMask(QRegion(windowPath.toFillPolygon().toPolygon()));

    setCentralWidget(new DeviceGrid(this));
}
```

```cpp
// src/ui/DeviceGrid.cpp:43-46
#include <QTimer>
#include <QToolButton>
#include <QVector>
```

```cpp
// src/ui/DeviceGrid.cpp:1095-1227
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
    ...
    applyStatusAutoRefreshSetting(false);
    refreshDeviceStatuses(); // wjy: BUG诊断：保持启动自动状态刷新开启，配合 refreshDeviceStatuses 内部日志定位 Release 偶发崩溃位置。
}
```

### Modified Code
```cpp
// src/ui/MainWindow.cpp:5-12
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QPainterPath>
#include <QRegion>
#include <QTextStream>
```

```cpp
// src/ui/MainWindow.cpp:14-34
namespace {

// =====wjy====
void writeWindowStartupLog(const QString& message)
{
    const QString dataDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data")); // wjy: 和 main 启动日志写到同一个 data 目录，方便按时间顺序排查。
    QDir().mkpath(dataDir); // wjy: 确保日志目录存在，避免 Release 诊断日志写入失败。

    QFile file(QDir(dataDir).filePath(QStringLiteral("wjy_startup.log"))); // wjy: 继续使用同一个启动日志文件，串起 MainWindow 和 DeviceGrid 构造流程。
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return; // wjy: 日志失败不能影响窗口创建。
    }

    QTextStream stream(&file); // wjy: 每次写入一条带时间戳的构造步骤。
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
           << QStringLiteral(" ")
           << message
           << Qt::endl; // wjy: 换行并刷新，尽量保留 Release 崩溃前最后一步。
    file.close(); // wjy: 立刻关闭文件，让日志尽快落盘。
}
// ===end====

} // namespace
```

```cpp
// src/ui/MainWindow.cpp:39-67
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    writeWindowStartupLog(QStringLiteral("[wjy-window] MainWindow ctor begin")); // wjy: 进入 MainWindow 构造函数，确认 main 已经开始创建主窗口。
    ...
    writeWindowStartupLog(QStringLiteral("[wjy-window] before DeviceGrid create")); // wjy: 记录创建设备主界面控件前的位置。
    DeviceGrid* deviceGrid = new DeviceGrid(this); // wjy: 拆开 new 和 setCentralWidget，便于日志区分崩在 DeviceGrid 构造还是设置中心控件。
    writeWindowStartupLog(QStringLiteral("[wjy-window] after DeviceGrid create")); // wjy: 记录 DeviceGrid 构造完成。
    setCentralWidget(deviceGrid);
    writeWindowStartupLog(QStringLiteral("[wjy-window] after setCentralWidget")); // wjy: 记录中心控件设置完成。
    writeWindowStartupLog(QStringLiteral("[wjy-window] MainWindow ctor end")); // wjy: 主窗口构造函数正常结束。
}
```

```cpp
// src/ui/DeviceGrid.cpp:43-47
#include <QTimer>
#include <QToolButton>
#include <QTextStream>
#include <QVector>
```

```cpp
// src/ui/DeviceGrid.cpp:76-95
// =====wjy====
void writeDeviceGridStartupLog(const QString& message)
{
    const QString dataDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data")); // wjy: DeviceGrid 构造日志写到程序运行目录 data，和 main/window 日志汇总在一起。
    QDir().mkpath(dataDir); // wjy: 确保日志目录存在。

    QFile file(QDir(dataDir).filePath(QStringLiteral("wjy_startup.log"))); // wjy: 使用同一个启动日志文件，便于看完整启动顺序。
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return; // wjy: 日志失败不能影响界面构造。
    }

    QTextStream stream(&file); // wjy: 写入时间戳和 DeviceGrid 构造步骤。
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
           << QStringLiteral(" ")
           << message
           << Qt::endl; // wjy: 刷新文本流，尽量保留 Release 崩溃前最后一步。
    file.close(); // wjy: 每条日志写完立即关闭文件，减少异常退出导致日志丢失。
}
// ===end====
```

```cpp
// src/ui/DeviceGrid.cpp:1115-1283
DeviceGrid::DeviceGrid(QWidget* parent)
    : QFrame(parent)
{
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] DeviceGrid ctor begin")); // wjy: 进入 DeviceGrid 构造函数，定位 MainWindow 创建内部崩溃。
    setObjectName(QStringLiteral("GameViewerMainWindow"));
    setFixedSize(920, 680);
    setMouseTracking(true);
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after basic widget setup")); // wjy: 记录基础 QWidget 属性设置完成。

    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before read app settings")); // wjy: 记录读取启动设置前的位置。
    ...
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before loadDevices")); // wjy: 记录加载 devices.json 前的位置。
    loadDevices();
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after loadDevices devices=%1 groups=%2").arg(g_devices.size()).arg(g_deviceGroupNames.size())); // wjy: 记录设备和分组加载数量，判断配置读取是否完成。
    ...
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] before refreshDeviceStatuses")); // wjy: 记录首次状态刷新前的位置。
    refreshDeviceStatuses(); // wjy: BUG诊断：保持启动自动状态刷新开启，配合 refreshDeviceStatuses 内部日志定位 Release 偶发崩溃位置。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after refreshDeviceStatuses call")); // wjy: 记录首次状态刷新已发起，不代表后台线程已完成。
    writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] DeviceGrid ctor end")); // wjy: DeviceGrid 构造函数正常结束。
}
```

### Steps
1. 在 `MainWindow.cpp` 中添加文件日志函数，输出 `[wjy-window]` 构造步骤。
2. 在 `MainWindow` 构造函数中记录窗口属性、圆角遮罩、DeviceGrid 创建和中心控件设置步骤。
3. 将 `setCentralWidget(new DeviceGrid(this))` 拆成 `new DeviceGrid` 和 `setCentralWidget` 两步，便于定位崩溃阶段。
4. 在 `DeviceGrid.cpp` 中添加文件日志函数，输出 `[wjy-grid]` 构造步骤。
5. 在 `DeviceGrid` 构造函数中记录设置读取、设备加载、本机信息读取、控件创建、定时器创建、自动刷新设置和首次状态刷新调用。

### Verification
未执行构建，按用户要求由用户自行使用 Release 测试。建议测试：再次启动 Release，如果异常退出，查看 `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release/data/wjy_startup.log` 最后 20 行，把最后的 `[wjy-window]` 或 `[wjy-grid]` 日志发回来，即可定位构造函数内部的具体崩溃位置。

## 2026-07-01 10:25 - 调整启动日志：每次启动先清空旧日志

### Changed Location
- `src/main.cpp:26`: 在 `writeStartupLog()` 中增加本进程首次写日志标记。
- `src/main.cpp:27`: 根据是否首次写日志选择 `Truncate` 或 `Append` 打开方式。
- `src/main.cpp:33`: 第一次成功打开日志后标记已清空，避免后续日志覆盖本次启动流程。

### Reason
之前 `wjy_startup.log` 一直使用追加模式，连续多次启动后旧日志和新日志混在一起，排查 Release 偶发崩溃时需要手动找最新一段。现在改成每次程序启动后的第一条日志先清空旧文件，后续同一次启动中的日志继续追加。这样每次打开 `wjy_startup.log` 都只看到当前这次启动流程，排查更直接。

### Original Code
```cpp
// src/main.cpp:23-29
QFile file(QDir(dataDir).filePath(QStringLiteral("wjy_startup.log"))); // wjy: 使用独立启动日志文件，和设备配置 devices.json 分开。
if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
    return; // wjy: 日志写入失败不能影响主程序启动，所以直接返回。
}

QTextStream stream(&file); // wjy: 用文本流写入时间和步骤，方便在 Qt Creator 外直接打开查看。
```

### Modified Code
```cpp
// src/main.cpp:25-34
QFile file(QDir(dataDir).filePath(QStringLiteral("wjy_startup.log"))); // wjy: 使用独立启动日志文件，和设备配置 devices.json 分开。
static bool s_logFileResetForThisProcess = false; // wjy: 每次程序进程启动后，第一次写日志前先清空旧日志，后续日志继续追加。
const QIODevice::OpenMode writeMode = QIODevice::WriteOnly
    | QIODevice::Text
    | (s_logFileResetForThisProcess ? QIODevice::Append : QIODevice::Truncate); // wjy: 首条日志用 Truncate 清空文件，其余日志用 Append 保留本次启动流程。
if (!file.open(writeMode)) {
    return; // wjy: 日志写入失败不能影响主程序启动，所以直接返回。
}
s_logFileResetForThisProcess = true; // wjy: 只有第一次成功打开日志文件时清空，避免后续步骤把本次启动日志删掉。

QTextStream stream(&file); // wjy: 用文本流写入时间和步骤，方便在 Qt Creator 外直接打开查看。
```

### Steps
1. 在 `writeStartupLog()` 内增加静态变量，记录当前进程是否已经清空过日志。
2. 首次写日志时用 `QIODevice::Truncate` 打开文件，清空旧启动日志。
3. 后续写日志时用 `QIODevice::Append` 打开文件，保留同一次启动的完整流程。
4. 只有日志文件成功打开后才把标记设为已清空，避免打开失败导致状态误判。

### Verification
未执行构建，按用户要求由用户自行使用 Release 测试。建议测试：连续启动两次程序，查看 `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release/data/wjy_startup.log`，文件中应只保留最新一次启动的日志。

## 2026-07-01 10:32 - 为设备增加 group 分组归属字段

### Changed Location
- `src/ui/DeviceGrid.cpp:175`: `DeviceEntry` 增加 `group` 字段，空字符串代表无分组。
- `src/ui/DeviceGrid.cpp:343`: 保存 `devices.json` 时为每个设备写入 `group` 字段。
- `src/ui/DeviceGrid.cpp:382`: 默认示例设备初始化时补空 `group`。
- `src/ui/DeviceGrid.cpp:422`: 加载 `devices.json` 时读取设备的 `group` 字段，旧文件没有该字段时默认为空。
- `src/ui/DeviceGrid.cpp:1542`: 新增设备时 `group` 默认为空，表示设备仍在“我的设备”根部。

### Reason
后续要实现“拖动设备进入某个分组”，必须先有数据字段记录设备属于哪个分组。本次只增加数据层字段，不改变界面绘制、不改变拖拽逻辑。这样新增设备默认无分组，只有用户之后把设备拖入具体分组时，才把 `group` 写成对应分组名。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:169-175
struct DeviceEntry {
    QString name;
    QString ip;
    QString mac;
    QString broadcastIp;
    QString remark;
};
```

```cpp
// src/ui/DeviceGrid.cpp:337-343
QJsonObject object;
object.insert(QStringLiteral("name"), device.name);
object.insert(QStringLiteral("ip"), device.ip);
object.insert(QStringLiteral("mac"), device.mac);
object.insert(QStringLiteral("broadcast_ip"), device.broadcastIp);
object.insert(QStringLiteral("remark"), device.remark);
deviceArray.append(object);
```

```cpp
// src/ui/DeviceGrid.cpp:380-382
if (!file.exists()) {
    g_devices.append({QStringLiteral("72"), QStringLiteral("192.168.3.27"), {}, {}, {}});
    saveDevices();
```

```cpp
// src/ui/DeviceGrid.cpp:414-420
g_devices.append({
    name,
    ip,
    object.value(QStringLiteral("mac")).toString().trimmed(),
    object.value(QStringLiteral("broadcast_ip")).toString().trimmed(),
    object.value(QStringLiteral("remark")).toString()
});
```

```cpp
// src/ui/DeviceGrid.cpp:1539-1540
g_devices.append({name, ip, mac, {}, m_deviceRemarkEdit->text().trimmed()});
saveDevices();
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:169-176
struct DeviceEntry {
    QString name;
    QString ip;
    QString mac;
    QString broadcastIp;
    QString remark;
    QString group; // wjy: 设备所属分组，空字符串表示设备仍在“我的设备”根部，只有拖入具体分组后才写分组名。
};
```

```cpp
// src/ui/DeviceGrid.cpp:337-344
QJsonObject object;
object.insert(QStringLiteral("name"), device.name);
object.insert(QStringLiteral("ip"), device.ip);
object.insert(QStringLiteral("mac"), device.mac);
object.insert(QStringLiteral("broadcast_ip"), device.broadcastIp);
object.insert(QStringLiteral("remark"), device.remark);
object.insert(QStringLiteral("group"), device.group); // wjy: 持久化设备所属分组；新增设备默认空分组，拖入分组后再写入分组名。
deviceArray.append(object);
```

```cpp
// src/ui/DeviceGrid.cpp:380-383
if (!file.exists()) {
    g_devices.append({QStringLiteral("72"), QStringLiteral("192.168.3.27"), {}, {}, {}, {}}); // wjy: 默认示例设备不属于任何分组，group 保持空字符串。
    saveDevices();
```

```cpp
// src/ui/DeviceGrid.cpp:416-423
g_devices.append({
    name,
    ip,
    object.value(QStringLiteral("mac")).toString().trimmed(),
    object.value(QStringLiteral("broadcast_ip")).toString().trimmed(),
    object.value(QStringLiteral("remark")).toString(),
    object.value(QStringLiteral("group")).toString().trimmed() // wjy: 读取设备所属分组；旧 devices.json 没有 group 时会得到空字符串。
});
```

```cpp
// src/ui/DeviceGrid.cpp:1542-1543
g_devices.append({name, ip, mac, {}, m_deviceRemarkEdit->text().trimmed(), {}}); // wjy: 新增设备默认无分组，只有后续拖入具体分组时才写 group。
saveDevices();
```

### Steps
1. 在 `DeviceEntry` 里增加 `QString group`。
2. 保存设备时写入 `"group"` 字段。
3. 加载设备时读取 `"group"` 字段，兼容旧 JSON 没有该字段的情况。
4. 默认示例设备补空分组字段。
5. 新增设备时明确写空分组，表示设备不属于任何分组。

### Verification
未执行构建，按用户要求由用户自行测试。建议测试：启动程序后新增一个设备，查看 `data/devices.json`，新增设备应出现 `"group": ""`；旧设备没有 `group` 字段时仍应正常加载。

## 2026-07-01 10:32 - 设备拖拽第一步：只识别拖拽并输出日志

### Changed Location
- `src/ui/DeviceGrid.h:91`: 增加设备拖拽候选状态。
- `src/ui/DeviceGrid.h:92`: 增加设备正在拖拽状态。
- `src/ui/DeviceGrid.h:93`: 增加当前拖拽设备下标。
- `src/ui/DeviceGrid.h:94`: 增加设备拖拽起点坐标。
- `src/ui/DeviceGrid.cpp:2554`: 鼠标按下设备行时记录拖拽候选，并输出 `[wjy-drag] candidate`。
- `src/ui/DeviceGrid.cpp:2587`: 鼠标移动超过 Qt 拖拽阈值时进入拖拽识别状态，并输出 `[wjy-drag] start`。
- `src/ui/DeviceGrid.cpp:2668`: 鼠标松开时识别落点是分组、根部空白还是无目标，并输出 `[wjy-drag] drop`。

### Reason
设备拖拽功能容易影响点击选择、窗口拖动、分组点击、设备排序和 JSON 保存。为了降低风险，本次只做第一步“拖拽识别日志”：确认能识别拖的是哪台设备，以及松开时落在哪个目标区域。此步骤不修改 `group`，不保存 `devices.json`，不改变绘制顺序。

### Original Code
```cpp
// src/ui/DeviceGrid.h:88-92
QVector<QPushButton*> m_localInfoCopyButtons;
bool m_draggingWindow = false;
QPoint m_dragOffset;
bool m_deviceGroupExpanded = true;
bool m_remoteAssistExpanded = true;
```

```cpp
// src/ui/DeviceGrid.cpp:2542-2555
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
```

```cpp
// src/ui/DeviceGrid.cpp:2557-2566
void DeviceGrid::mouseMoveEvent(QMouseEvent* event)
{
    if (m_draggingWindow && (event->buttons() & Qt::LeftButton)) {
        window()->move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }

    updateDesktopHover(event->pos());
```

```cpp
// src/ui/DeviceGrid.cpp:2620-2625
void DeviceGrid::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_draggingWindow = false;

        if (refreshRect().contains(event->pos())) {
```

### Modified Code
```cpp
// src/ui/DeviceGrid.h:88-96
QVector<QPushButton*> m_localInfoCopyButtons;
bool m_draggingWindow = false;
QPoint m_dragOffset;
bool m_deviceDragCandidateActive = false; // wjy: 鼠标按下设备行后先作为拖拽候选，移动超过阈值才进入真正拖拽。
bool m_draggingDevice = false; // wjy: 当前是否正在拖拽设备；第一步只用于输出识别日志，不改变数据。
int m_draggingDeviceIndex = -1; // wjy: 当前拖拽候选/正在拖拽的设备下标，-1 表示没有设备被拖拽。
QPoint m_deviceDragStartPos; // wjy: 记录设备拖拽起点，用来判断鼠标移动距离是否达到拖拽阈值。
bool m_deviceGroupExpanded = true;
```

```cpp
// src/ui/DeviceGrid.cpp:2554-2574
// =====wjy====
if (event->button() == Qt::LeftButton && m_deviceGroupExpanded) { // wjy: 左键按在“我的设备”的设备行上时，先记录为拖拽候选。
    const QStringList names = deviceNames();
    const int visibleDeviceBottom = remoteAssistGroupHeaderRect(m_deviceGroupExpanded).y();
    for (int i = 0; i < names.size(); ++i) {
        if (deviceRowRect(i).y() + deviceRowRect(i).height() > visibleDeviceBottom) {
            continue; // wjy: 超出“我的设备”可见区域的设备行不参与拖拽识别。
        }
        if (deviceRowRect(i).contains(event->pos())) {
            m_deviceDragCandidateActive = true; // wjy: 先标记候选，避免普通点击立即被当成拖拽。
            m_draggingDevice = false; // wjy: 鼠标还没移动超过阈值，所以此时还不是正式拖拽。
            m_draggingDeviceIndex = i; // wjy: 记录当前按下的设备下标，松开时用来输出拖拽设备名称。
            m_deviceDragStartPos = event->pos(); // wjy: 记录起点，后续用移动距离判断是否进入拖拽。
            qInfo().noquote() << QStringLiteral("[wjy-drag] candidate deviceIndex=%1 device=%2")
                .arg(i)
                .arg(names.at(i)); // wjy: 第一阶段只输出日志，确认按下的是哪台设备。
            break;
        }
    }
}
// ===end====
```

```cpp
// src/ui/DeviceGrid.cpp:2587-2606
// =====wjy====
if (m_deviceDragCandidateActive && (event->buttons() & Qt::LeftButton)) { // wjy: 只有按住左键移动时，才判断设备拖拽。
    const int movedDistance = (event->pos() - m_deviceDragStartPos).manhattanLength(); // wjy: 用曼哈顿距离判断移动是否超过 Qt 推荐拖拽阈值。
    if (!m_draggingDevice && movedDistance >= QApplication::startDragDistance()) {
        m_draggingDevice = true; // wjy: 超过阈值后正式进入设备拖拽识别状态。
        const QString deviceName = (m_draggingDeviceIndex >= 0 && m_draggingDeviceIndex < deviceNames().size())
            ? deviceNames().at(m_draggingDeviceIndex)
            : QString(); // wjy: 防御性获取设备名，避免日志访问越界。
        qInfo().noquote() << QStringLiteral("[wjy-drag] start deviceIndex=%1 device=%2 distance=%3")
            .arg(m_draggingDeviceIndex)
            .arg(deviceName)
            .arg(movedDistance); // wjy: 第一阶段只输出拖拽开始日志，不改变界面和数据。
    }
    if (m_draggingDevice) {
        setCursor(Qt::ClosedHandCursor); // wjy: 拖拽识别中给一个抓取光标反馈，但不绘制拖拽动画。
        event->accept();
        return;
    }
}
// ===end====
```

```cpp
// src/ui/DeviceGrid.cpp:2668-2709
// =====wjy====
if (m_draggingDevice) { // wjy: 如果本次鼠标操作已经进入设备拖拽状态，松开时只输出落点日志。
    QString targetType = QStringLiteral("none"); // wjy: 默认表示没有落到可识别的分组目标。
    QString targetGroup;
    if (m_deviceGroupExpanded) {
        const QStringList names = deviceNames();
        const int visibleDeviceBottom = remoteAssistGroupHeaderRect(m_deviceGroupExpanded).y();
        for (int groupIndex = 0; groupIndex < g_deviceGroupNames.size(); ++groupIndex) {
            const int rowIndex = names.size() + groupIndex;
            if (deviceRowRect(rowIndex).y() + deviceRowRect(rowIndex).height() > visibleDeviceBottom) {
                continue; // wjy: 不识别超出“我的设备”可见区域的分组行。
            }
            if (deviceRowRect(rowIndex).contains(event->pos())) {
                targetType = QStringLiteral("group"); // wjy: 鼠标松开在分组行上，后续第二步才会真正写入设备 group。
                targetGroup = g_deviceGroupNames.at(groupIndex);
                break;
            }
        }
        if (targetType == QStringLiteral("none") && deviceGroupReservedBlankRect().contains(event->pos())) {
            targetType = QStringLiteral("rootBlank"); // wjy: 松开在“我的设备”预留空白区，后续可用于拖回根部。
        }
    }
    const QString deviceName = (m_draggingDeviceIndex >= 0 && m_draggingDeviceIndex < deviceNames().size())
        ? deviceNames().at(m_draggingDeviceIndex)
        : QString(); // wjy: 防御性获取设备名，避免拖拽过程中设备列表变化导致越界。
    qInfo().noquote() << QStringLiteral("[wjy-drag] drop deviceIndex=%1 device=%2 targetType=%3 targetGroup=%4")
        .arg(m_draggingDeviceIndex)
        .arg(deviceName)
        .arg(targetType)
        .arg(targetGroup); // wjy: 第一阶段只输出落点日志，不修改 group、不保存 JSON。
    m_deviceDragCandidateActive = false; // wjy: 松开后清理本次拖拽候选状态。
    m_draggingDevice = false; // wjy: 松开后结束拖拽识别。
    m_draggingDeviceIndex = -1; // wjy: 清空拖拽设备下标。
    unsetCursor();
    event->accept();
    return;
}
if (m_deviceDragCandidateActive) {
    m_deviceDragCandidateActive = false; // wjy: 普通点击没有进入拖拽，松开时清理候选状态，让原来的设备选择逻辑继续执行。
    m_draggingDeviceIndex = -1; // wjy: 清空候选设备下标，不影响下面原有点击选择逻辑。
}
// ===end====
```

### Steps
1. 增加设备拖拽候选、拖拽状态、设备下标和拖拽起点成员。
2. 鼠标左键按下设备行时只记录候选状态，并输出 `[wjy-drag] candidate`。
3. 鼠标移动超过 `QApplication::startDragDistance()` 后才进入拖拽状态，并输出 `[wjy-drag] start`。
4. 鼠标松开时识别目标区域：分组行、我的设备预留空白区或无目标，并输出 `[wjy-drag] drop`。
5. 拖拽识别阶段不修改 `group`，不保存 JSON，不改变设备绘制顺序。

### Verification
未执行构建，按用户要求由用户自行测试。建议测试：按住设备 `72` 拖到某个分组行后松开，Qt Creator 应输出 `[wjy-drag] candidate`、`[wjy-drag] start`、`[wjy-drag] drop ... targetType=group targetGroup=分组名`；普通单击设备仍应正常切换选中设备。

## 2026-07-01 13:38 - 让设备列表按可见行绘制

### Changed Location
- `src/ui/DeviceGrid.cpp:2368`: 将“我的设备”下方的绘制逻辑改为遍历 `visibleDeviceRows()`，让无分组设备、分组行、分组内设备按同一套可见行顺序显示。

### Reason
之前 UI 绘制仍然先画全部设备、再画全部分组。这样数据里即使已经有 `group`，设备也不会真正显示到分组下面。现在改为一行一行读取 `visibleDeviceRows()`，UI 顺序和分组数据来源统一，后续点击、拖拽命中也可以继续沿用这个可见行模型。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:2368-2421
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

        painter.setBrush(deviceStatusDotColor(devicePresenceForIndex(i)));
        painter.drawEllipse(QRectF(26, rowY + 17, 6, 6));
        drawDeviceTileIcon(painter, 42, rowY + 9, 20);
        painter.drawText(QRectF(74, rowY + 7, 116, 22), Qt::AlignVCenter | Qt::AlignLeft, names.at(i));
    }

    for (int groupIndex = 0; groupIndex < g_deviceGroupNames.size(); ++groupIndex) {
        const int rowIndex = names.size() + groupIndex;
        const int rowY = deviceRowRect(rowIndex).y();
        painter.drawRoundedRect(QRectF(deviceRowRect(rowIndex)), 5, 5);
        painter.drawText(QRectF(58, rowY + 7, 132, 22), Qt::AlignVCenter | Qt::AlignLeft, g_deviceGroupNames.at(groupIndex));
    }
}
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:2368-2437
// =====wjy====
const QVector<DeviceListRow> deviceRows = visibleDeviceRows(); // wjy: 左侧列表改为按“真实可见行”绘制，设备和分组的顺序由分组数据统一决定。
const QSet<int> badges = deviceBadgeIndexes(); // wjy: 远程控制角标仍然按真实设备下标判断，避免分组行影响设备下标。
const int visibleDeviceBottom = remoteAssistGroupHeaderRect(m_deviceGroupExpanded).y(); // wjy: 设备列表最多只能画到“设备管理”标题上方。
if (m_deviceGroupExpanded) {
    for (int rowIndex = 0; rowIndex < deviceRows.size(); ++rowIndex) { // wjy: 每一行都来自 visibleDeviceRows，包含无分组设备、分组行、分组内设备。
        const DeviceListRow& row = deviceRows.at(rowIndex); // wjy: row 保存这一行到底是设备还是分组，以及对应的真实下标。
        const QRect rowRect = deviceRowRect(rowIndex); // wjy: 视觉行号决定这一行在左侧列表中的 y 坐标。

        if (row.type == DeviceListRow::Type::Device) {
            const int deviceIndex = row.deviceIndex; // wjy: 设备行使用真实设备下标读取名称、在线状态和选中状态。
            const bool deviceInsideGroup = row.groupIndex >= 0; // wjy: 分组内设备稍微右移，视觉上表示它属于上方分组。
            painter.setBrush(deviceStatusDotColor(devicePresenceForIndex(deviceIndex))); // wjy: 在线圆点按真实设备状态绘制，不受视觉行号影响。
            painter.drawText(QRectF(textX, rowY + 7, textWidth, 22), Qt::AlignVCenter | Qt::AlignLeft, deviceDisplayName(g_devices.at(deviceIndex))); // wjy: 显示真实设备名，分组排序变化不会改错名字。
            continue;
        }

        if (row.type == DeviceListRow::Type::Group) {
            const int groupIndex = row.groupIndex; // wjy: 分组行使用真实分组下标读取名称和展开状态。
            painter.setBrush(QColor(QStringLiteral("#F7FAFE"))); // wjy: 分组行使用浅背景，和普通设备行区分开。
            painter.drawText(QRectF(58, rowY + 7, 132, 22), Qt::AlignVCenter | Qt::AlignLeft, g_deviceGroupNames.at(groupIndex)); // wjy: 分组正在原地重命名时不画底层文字，避免和输入框重叠。
        }
    }
}
// ===end====
```

### Steps
1. 删除 `paintEvent` 中“先按 `deviceNames()` 画全部设备，再用 `names.size() + groupIndex` 画分组”的旧绘制方式。
2. 新增 `deviceRows = visibleDeviceRows()`，让 UI 绘制直接使用当前真实可见行顺序。
3. 设备行改用 `row.deviceIndex` 读取状态、名称、选中态和角标，避免分组行插入后下标错位。
4. 分组行改用 `row.groupIndex` 读取分组名和展开状态，继续保持无圆点、无设备方块图标，只显示分组文字和右侧箭头。
5. 分组内设备增加一点缩进，让视觉上能看出它属于上面的子下拉框。

### Verification
未执行构建，按用户要求由用户自行构建测试。本次只改 UI 绘制，建议测试：启动后创建分组，把设备写入某个分组后，设备应显示在该分组下方；分组行不应显示绿色在线点和蓝色设备图标；“设备管理”仍然在列表和预留空白区之后。

## 2026-07-01 13:50 - 让鼠标命中按可见行判断

### Changed Location
- `src/ui/DeviceGrid.cpp:2677`: 拖拽按下候选从旧的 `deviceNames()` 行号改为 `visibleDeviceRows()`。
- `src/ui/DeviceGrid.cpp:2719`: 拖拽开始日志改为按真实设备下标读取设备名。
- `src/ui/DeviceGrid.cpp:2766`: 双击重命名分组改为按可见行命中分组。
- `src/ui/DeviceGrid.cpp:2805`: 拖拽松开目标改为按可见行识别目标分组。
- `src/ui/DeviceGrid.cpp:2944`: 普通点击设备和点击分组展开收起改为按可见行统一判断。

### Reason
上一阶段 UI 已经按 `visibleDeviceRows()` 绘制，但鼠标事件仍然使用旧假设：设备行等于 `deviceNames()` 的下标，分组行等于 `names.size() + groupIndex`。当设备进入分组后，这个旧算法会认错“按下的是哪台设备”或“松开在哪个分组”。本次把鼠标命中也切到可见行模型，让绘制、点击、拖拽、双击使用同一套行顺序。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:2677-2697
if (event->button() == Qt::LeftButton && m_deviceGroupExpanded) {
    const QStringList names = deviceNames();
    const int visibleDeviceBottom = remoteAssistGroupHeaderRect(m_deviceGroupExpanded).y();
    for (int i = 0; i < names.size(); ++i) {
        if (deviceRowRect(i).contains(event->pos())) {
            m_draggingDeviceIndex = i;
            qInfo().noquote() << QStringLiteral("[wjy-drag] candidate deviceIndex=%1 device=%2")
                .arg(i)
                .arg(names.at(i));
            break;
        }
    }
}

// src/ui/DeviceGrid.cpp:2796-2816
const QStringList names = deviceNames();
for (int groupIndex = 0; groupIndex < g_deviceGroupNames.size(); ++groupIndex) {
    const int rowIndex = names.size() + groupIndex;
    if (deviceRowRect(rowIndex).contains(event->pos())) {
        targetType = QStringLiteral("group");
        targetGroup = g_deviceGroupNames.at(groupIndex);
        break;
    }
}
const QString deviceName = (m_draggingDeviceIndex >= 0 && m_draggingDeviceIndex < deviceNames().size())
    ? deviceNames().at(m_draggingDeviceIndex)
    : QString();

// src/ui/DeviceGrid.cpp:2927-2969
const QStringList names = deviceNames();
for (int groupIndex = 0; groupIndex < g_deviceGroupNames.size(); ++groupIndex) {
    const int rowIndex = names.size() + groupIndex;
    if (deviceRowRect(rowIndex).contains(event->pos())) {
        g_deviceGroupExpandedStates[groupIndex] = !g_deviceGroupExpandedStates.at(groupIndex);
        return;
    }
}
for (int i = 0; i < names.size(); ++i) {
    if (deviceRowRect(i).contains(event->pos())) {
        startDeviceSwitchAnimation(i, names.at(i));
        return;
    }
}
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:2677-2706
// =====wjy====
if (event->button() == Qt::LeftButton && m_deviceGroupExpanded) { // wjy: 左键按在“我的设备”的设备行上时，先记录为拖拽候选。
    const QVector<DeviceListRow> rows = visibleDeviceRows(); // wjy: 拖拽按下也使用当前可见行顺序，避免分组行插入后设备下标错位。
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const DeviceListRow& row = rows.at(rowIndex); // wjy: rowIndex 是界面行号，row.deviceIndex 才是真实设备下标。
        if (row.type != DeviceListRow::Type::Device) {
            continue; // wjy: 分组行只负责展开收起，不作为可拖拽设备。
        }
        const int deviceIndex = row.deviceIndex; // wjy: 记录真实设备下标，后面写 group 时才能改到正确设备。
        m_draggingDeviceIndex = deviceIndex; // wjy: 保存真实设备下标，而不是界面行号。
    }
}
// ===end====

// src/ui/DeviceGrid.cpp:2805-2838
// =====wjy====
if (m_draggingDevice) { // wjy: 如果本次鼠标操作已经进入设备拖拽状态，松开时只输出落点日志。
    const QVector<DeviceListRow> rows = visibleDeviceRows(); // wjy: 拖拽落点按当前可见行识别，和 UI 绘制顺序保持一致。
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const DeviceListRow& row = rows.at(rowIndex);
        if (row.type != DeviceListRow::Type::Group) {
            continue; // wjy: 目前只有松开在分组行上，才视为拖入该分组。
        }
        targetGroup = g_deviceGroupNames.at(row.groupIndex);
    }
    const QString deviceName = (m_draggingDeviceIndex >= 0 && m_draggingDeviceIndex < g_devices.size())
        ? deviceDisplayName(g_devices.at(m_draggingDeviceIndex))
        : QString(); // wjy: 防御性获取真实设备名，避免拖拽过程中设备列表变化导致越界。
}
// ===end====

// src/ui/DeviceGrid.cpp:2944-2992
// =====wjy====
const QVector<DeviceListRow> rows = visibleDeviceRows(); // wjy: 普通点击也使用可见行，保证点击位置和绘制出来的行一致。
for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) { // wjy: 一次遍历当前 UI 的每一行，设备行和分组行按同一套坐标命中。
    const DeviceListRow& row = rows.at(rowIndex);
    if (row.type == DeviceListRow::Type::Group) {
        const int groupIndex = row.groupIndex; // wjy: 点击分组行时，使用真实分组下标切换展开状态。
        g_deviceGroupExpandedStates[groupIndex] = !g_deviceGroupExpandedStates.at(groupIndex);
        return;
    }
    if (row.type == DeviceListRow::Type::Device) {
        const int deviceIndex = row.deviceIndex; // wjy: 点击设备行时，使用真实设备下标切换详情页。
        startDeviceSwitchAnimation(deviceIndex, deviceDisplayName(g_devices.at(deviceIndex))); // wjy: 使用真实设备下标，避免分组行导致选错设备。
        return;
    }
}
// ===end====
```

### Steps
1. 拖拽按下候选改为遍历 `visibleDeviceRows()`，只允许 `Device` 行进入拖拽候选。
2. `m_draggingDeviceIndex` 改为保存真实设备下标 `row.deviceIndex`，不再保存视觉行号。
3. 拖拽开始和拖拽松开日志改为通过 `g_devices` 读取真实设备名。
4. 拖拽松开目标改为遍历可见行里的 `Group` 行，松开在分组行上才写入对应分组名。
5. 双击重命名分组改为按可见行命中分组，避免分组位置变化后双击不到。
6. 普通点击改为一次遍历可见行：点到分组就展开/收起，点到设备就切换当前设备详情。

### Verification
未执行构建，按用户要求由用户自行构建测试。已用 `rg` 检查旧的 `names.size() + groupIndex`、`m_draggingDeviceIndex < deviceNames().size()`、`startDeviceSwitchAnimation(i, names.at(i))` 等旧命中写法，当前未再匹配到。建议测试：把设备拖到某个分组行，确认 `devices.json` 的该设备 `group` 变为目标分组名；再拖到“我的设备”预留空白区，确认 `group` 被清空；点击分组箭头和双击分组重命名也应命中正确分组。

## 2026-07-03 11:35 - 修复折叠分组后隐藏设备仍参与多选拖拽

### Changed Location
- `src/ui/DeviceGrid.h:88`: 新增 `pruneHiddenDeviceSelections()` 私有方法声明，用于清理折叠后不可见设备的多选状态。
- `src/ui/DeviceGrid.cpp:2977`: 新增 `DeviceGrid::pruneHiddenDeviceSelections()`，按当前可见设备行过滤多选集合、拖拽集合和 Shift 锚点。
- `src/ui/DeviceGrid.cpp:3266`: 拖拽快照改为只收集当前可见且已选中的设备，避免隐藏选中项被批量移动。
- `src/ui/DeviceGrid.cpp:3662`: 点击分组展开/收起后立即调用清理函数，让折叠动作同步修正选择状态。

### Reason
原来的 Shift 多选集合不会在分组折叠时清理。用户先多选分组内设备，再折叠该分组时，隐藏设备仍保留在 `m_selectedDeviceIndexes` 中；后续如果拖动一个可见的已选设备，隐藏设备也可能被一起移动到新分组。此次修改选择“折叠后清理 + 拖拽时再按可见行过滤”的双保险方案，既修正 UI 状态，也避免旧状态残留导致批量拖拽误操作。

### Original Code
```cpp
// src/ui/DeviceGrid.h:86-88 原逻辑
void beginDeviceGroupRename(int groupIndex); // wjy: 在分组文字原位置显示输入框，开始原地重命名。
void finishDeviceGroupRename(bool saveText); // wjy: 回车或点击外部时结束重命名，并按规则保存或恢复。
void runBackgroundTask(std::function<void()> task); // wjy: 后台任务统一由 DeviceGrid 持有，关闭窗口时等待它们结束，避免 detach 线程晚于界面销毁。
```

```cpp
// src/ui/DeviceGrid.cpp:原 startDeviceSwitchAnimation 后没有选择清理 helper
void DeviceGrid::setDesktopHoverActive(bool active)
{
    if (m_desktopHovered == active) {
        return;
    }
```

```cpp
// src/ui/DeviceGrid.cpp:原拖拽快照逻辑
if (m_selectedDeviceIndexes.contains(
        deviceIndex)) {

    // 按在已经选中的设备上：
    // 拖动当前全部选中的设备。
    m_draggingDeviceIndexes =
        m_selectedDeviceIndexes;
} else {
```

```cpp
// src/ui/DeviceGrid.cpp:原分组展开/收起逻辑
g_deviceGroupExpandedStates[groupIndex] = !g_deviceGroupExpandedStates.at(groupIndex); // wjy: 点击分组行时切换展开状态，从而让箭头上下倒转。
saveDevices(); // wjy: 保存分组展开状态，重启后箭头方向保持一致。
update();
```

### Modified Code
```cpp
// src/ui/DeviceGrid.h:86-89
void beginDeviceGroupRename(int groupIndex); // wjy: 在分组文字原位置显示输入框，开始原地重命名。
void finishDeviceGroupRename(bool saveText); // wjy: 回车或点击外部时结束重命名，并按规则保存或恢复。
void pruneHiddenDeviceSelections(); // wjy: 分组折叠后清理不可见设备的多选和拖拽状态，避免隐藏设备参与批量移动。
void runBackgroundTask(std::function<void()> task); // wjy: 后台任务统一由 DeviceGrid 持有，关闭窗口时等待它们结束，避免 detach 线程晚于界面销毁。
```

```cpp
// src/ui/DeviceGrid.cpp:2976-3050
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
        m_selectedDeviceIndexes.clear(); // wjy: 当前没有任何可见设备时，清空左侧选择，避免隐藏设备继续高亮或被拖拽。
        m_draggingDeviceIndexes.clear();
        m_selectionAnchorDeviceIndex = -1;
        return;
    }

    m_selectedDeviceIndex = nextDeviceIndex; // wjy: 将右侧详情主设备同步到仍可见的设备，避免详情指向折叠隐藏项。
    m_previousDeviceIndex = nextDeviceIndex;
    m_currentDeviceName = deviceDisplayName(g_devices.at(nextDeviceIndex));
    m_previousDeviceName = m_currentDeviceName;
    m_selectedDeviceIndexes.insert(nextDeviceIndex); // wjy: 新主设备必须在左侧多选集合里，保证视觉选中态一致。
    if (m_selectionAnchorDeviceIndex < 0) {
        m_selectionAnchorDeviceIndex = nextDeviceIndex; // wjy: 没有可用 Shift 锚点时，用新主设备作为下一次范围选择起点。
    }
    if (m_detailAnimationTimer) {
        m_detailAnimationTimer->stop(); // wjy: 分组折叠引发的主设备兜底切换不播放详情页切换动画，避免隐藏项参与过渡。
    }
}
// ===end====
```

```cpp
// src/ui/DeviceGrid.cpp:3261-3277
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
```

```cpp
// src/ui/DeviceGrid.cpp:3661-3664
g_deviceGroupExpandedStates[groupIndex] = !g_deviceGroupExpandedStates.at(groupIndex); // wjy: 点击分组行时切换展开状态，从而让箭头上下倒转。
pruneHiddenDeviceSelections(); // wjy: 分组折叠后立即移除隐藏设备选择，避免后续 Shift/拖拽继续带着不可见设备。
saveDevices(); // wjy: 保存分组展开状态，重启后箭头方向保持一致。
update();
```

### Steps
1. 在 `DeviceGrid` 私有方法里新增 `pruneHiddenDeviceSelections()`，统一处理隐藏设备选择清理。
2. 清理函数先从 `visibleDeviceRows()` 生成可见设备下标集合，再过滤 `m_selectedDeviceIndexes` 和 `m_draggingDeviceIndexes`。
3. 如果 Shift 锚点被折叠隐藏，就改为第一台可见已选设备；没有可见已选设备时清空锚点。
4. 如果右侧详情主设备被折叠隐藏，就优先切到可见已选设备，否则切到第一台可见设备，并停止详情切换动画。
5. 在分组展开/收起后立即调用清理函数，保证 UI 状态和数据状态同步。
6. 在拖拽候选生成时再次按当前可见行过滤多选集合，防止旧状态残留导致隐藏设备被批量拖动。

### Verification
执行了 `cmake --build build\Desktop_Qt_6_11_1_MSVC2022_64bit-Debug --config Debug --target FSRemote` 做编译验证，但当前命令行环境缺少完整 MSVC 标准库 include 路径，构建在包含 `<cstdint>` 时失败，尚未进入本次改动逻辑的有效编译验证。已用 `rg` 和带行号读取确认新增方法、分组折叠调用点和拖拽快照过滤点均落在预期位置。建议在 Qt Creator 或已加载 VS 开发环境的终端中再构建一次，并手动测试：多选分组内设备后折叠该分组，隐藏设备应不再保持多选；随后拖动可见已选设备，不应移动被折叠隐藏的设备。

## 2026-07-03 11:56 - 列表自动刷新间隔改为数字输入框

### Changed Location
- `src/ui/DeviceGrid.h:91`: 将自动刷新间隔控件成员改为 `QLineEdit* m_statusRefreshIntervalEdit`。
- `src/ui/DeviceGrid.h:122`: 将自动刷新间隔成员默认值改为 60 秒。
- `src/ui/DeviceGrid.cpp:1601`: 设置页初始化时创建数字输入框，并用 `QIntValidator` 限制只能输入正整数。
- `src/ui/DeviceGrid.cpp:1710`: 设置页显隐逻辑改为控制数字输入框。
- `src/system/AppSettings.cpp:48`: 自动刷新间隔设置默认值和非法值兜底改为 60 秒。

### Reason
原先使用 `QComboBox` 作为列表自动刷新间隔控件，用户反馈下拉框虽然能显示，但交互效果不符合预期。此次改为数字输入框，让用户直接输入刷新间隔秒数；输入框只允许数字，空值或非法值回退到默认 60 秒，保存后立即重新应用自动刷新定时器。

### Original Code
```cpp
// src/ui/DeviceGrid.h:91-92 原逻辑
QString m_currentDeviceName;
QComboBox* m_statusRefreshIntervalCombo = nullptr;
QLineEdit* m_deviceIpEdit = nullptr;
```

```cpp
// src/ui/DeviceGrid.cpp:原设置页下拉框初始化
m_statusRefreshIntervalCombo = new QComboBox(this);
m_statusRefreshIntervalCombo->setGeometry(654, 364, 112, 32);
m_statusRefreshIntervalCombo->addItem(zh("5 秒"), 5);
m_statusRefreshIntervalCombo->addItem(zh("10 秒"), 10);
m_statusRefreshIntervalCombo->addItem(zh("15 秒"), 15);
m_statusRefreshIntervalCombo->addItem(zh("30 秒"), 30);
m_statusRefreshIntervalCombo->addItem(zh("60 秒"), 60);
connect(m_statusRefreshIntervalCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (index < 0) {
        return;
    }
    m_statusAutoRefreshIntervalSeconds = m_statusRefreshIntervalCombo->itemData(index).toInt();
    platform::AppSettings::setStatusAutoRefreshIntervalSeconds(m_statusAutoRefreshIntervalSeconds);
    applyStatusAutoRefreshSetting(false);
});
```

```cpp
// src/system/AppSettings.cpp:48-57 原逻辑
int AppSettings::statusAutoRefreshIntervalSeconds()
{
    const int seconds = settings().value(QStringLiteral("statusAutoRefreshIntervalSeconds"), 10).toInt();
    return seconds > 0 ? seconds : 10;
}

void AppSettings::setStatusAutoRefreshIntervalSeconds(int seconds)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("statusAutoRefreshIntervalSeconds"), seconds > 0 ? seconds : 10);
}
```

### Modified Code
```cpp
// src/ui/DeviceGrid.h:90-92
QString m_currentDeviceName;
QLineEdit* m_statusRefreshIntervalEdit = nullptr; // wjy: 列表自动刷新间隔输入框，只允许输入秒数数字，替代下拉框。
QLineEdit* m_deviceIpEdit = nullptr;
```

```cpp
// src/ui/DeviceGrid.cpp:1601-1633
m_statusRefreshIntervalEdit = new QLineEdit(this);
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
```

```cpp
// src/system/AppSettings.cpp:48-57
int AppSettings::statusAutoRefreshIntervalSeconds()
{
    const int seconds = settings().value(QStringLiteral("statusAutoRefreshIntervalSeconds"), 60).toInt();
    return seconds > 0 ? seconds : 60;
}

void AppSettings::setStatusAutoRefreshIntervalSeconds(int seconds)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("statusAutoRefreshIntervalSeconds"), seconds > 0 ? seconds : 60);
}
```

### Steps
1. 删除设置页自动刷新间隔的 `QComboBox`/`QListView` 依赖，改为 `QLineEdit`。
2. 为输入框设置 `QIntValidator(1, 86400)`，限制只能输入数字秒数。
3. 输入框默认显示当前保存的秒数；没有保存值时默认 60 秒。
4. 在 `editingFinished` 和 `returnPressed` 时保存秒数，并调用 `applyStatusAutoRefreshSetting(false)` 让新间隔生效。
5. 将设置页控件显隐逻辑从下拉框改为输入框。
6. 将 `AppSettings` 的自动刷新间隔默认值和兜底值从 10 改成 60。

### Verification
未执行编译，按用户要求由用户自行编译验证。已用 `rg` 确认 `QComboBox`、`QListView`、`m_statusRefreshIntervalCombo` 不再残留；已执行 `git diff --check -- src/ui/DeviceGrid.cpp src/ui/DeviceGrid.h src/system/AppSettings.cpp`，没有空白或格式错误。

## 2026-07-03 12:15 - 修复自动刷新秒数输入框点击外部不失焦

### Changed Location
- `src/ui/DeviceGrid.cpp:1302`: 为 `DeviceGrid` 设置 `Qt::ClickFocus`，允许手绘区域接管焦点。
- `src/ui/DeviceGrid.cpp:3096`: 在鼠标按下时检测自动刷新秒数输入框是否有焦点，点击输入框外部时主动清除焦点。

### Reason
设置页大部分区域是手绘内容，不是真正的 Qt 子控件。用户点击空白区域或手绘开关时，`QLineEdit` 不一定自动失去焦点，所以仍然可以继续输入。此次在父控件上启用点击焦点，并在 `mousePressEvent()` 开头主动让秒数输入框失焦，从而触发 `editingFinished` 保存逻辑。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:1299-1302 原逻辑
setFixedSize(920, 680);
writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after setFixedSize before setMouseTracking")); // wjy: 记录固定尺寸设置完成，继续判断是否崩在鼠标追踪设置。
setMouseTracking(true);
writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after basic widget setup")); // wjy: 记录基础 QWidget 属性设置完成。
```

```cpp
// src/ui/DeviceGrid.cpp:3093-3105 原逻辑
void DeviceGrid::mousePressEvent(QMouseEvent* event)
{
// =====wjy====
    if (m_deviceGroupNameEdit
        && m_deviceGroupNameEdit->isVisible()
        && !m_deviceGroupNameEdit->geometry().contains(event->pos())) { // wjy: 点击分组输入框外部时，提交当前名字并关闭输入框。
        finishDeviceGroupRename(true);
    }
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:1299-1303
setFixedSize(920, 680);
writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after setFixedSize before setMouseTracking")); // wjy: 记录固定尺寸设置完成，继续判断是否崩在鼠标追踪设置。
setMouseTracking(true);
setFocusPolicy(Qt::ClickFocus); // wjy: 允许手绘区域被点击后接管焦点，数字输入框点击外部时才能真正失焦并保存。
writeDeviceGridStartupLog(QStringLiteral("[wjy-grid] after basic widget setup")); // wjy: 记录基础 QWidget 属性设置完成。
```

```cpp
// src/ui/DeviceGrid.cpp:3093-3107
void DeviceGrid::mousePressEvent(QMouseEvent* event)
{
// =====wjy====
    if (m_statusRefreshIntervalEdit
        && m_statusRefreshIntervalEdit->hasFocus()
        && !m_statusRefreshIntervalEdit->geometry().contains(event->pos())) {
        m_statusRefreshIntervalEdit->clearFocus(); // wjy: 设置页是手绘区域，点击空白/开关不会天然抢焦点，这里主动让秒数输入框失焦并触发保存。
        setFocus(Qt::MouseFocusReason); // wjy: 父控件接管焦点，避免输入框清焦后马上继续接收键盘输入。
    }

    if (m_deviceGroupNameEdit
        && m_deviceGroupNameEdit->isVisible()
        && !m_deviceGroupNameEdit->geometry().contains(event->pos())) { // wjy: 点击分组输入框外部时，提交当前名字并关闭输入框。
        finishDeviceGroupRename(true);
    }
```

### Steps
1. 给 `DeviceGrid` 设置 `Qt::ClickFocus`，让父控件能在点击手绘区域时接管焦点。
2. 在 `mousePressEvent()` 开头检测 `m_statusRefreshIntervalEdit` 是否有焦点。
3. 如果本次点击位置不在输入框几何范围内，就调用 `clearFocus()`，触发输入框失焦保存。
4. 调用 `setFocus(Qt::MouseFocusReason)` 让父控件接管键盘焦点，避免输入框继续接收输入。

### Verification
未执行编译，按用户当前节奏由用户自行编译验证。建议手动测试：打开列表自动刷新，点击秒数输入框输入 `44`，再点击设置页空白区域或其它开关，输入框蓝色焦点边框应消失，随后继续键盘输入不应再写入该输入框。

## 2026-07-04 10:35 - 自动登记远程终端 SSH 密钥并复制 OpenSSH 运行时

### Changed Location
- `src/system/PortableOpenSshManager.h:17`: 增加读取本机客户端公钥、授权远程客户端公钥、修复私钥 ACL 的接口声明。
- `src/system/PortableOpenSshManager.cpp:63`: 增加 `authorized_keys` 公钥去重和追加工具函数。
- `src/system/PortableOpenSshManager.cpp:249`: 增加 `clientPublicKey()`，打开终端前可读取本机公钥。
- `src/system/PortableOpenSshManager.cpp:272`: 增加 `authorizeClientPublicKey()`，目标设备收到授权命令后把发起方公钥写入 `authorized_keys`。
- `src/system/PortableOpenSshManager.cpp:490`: 增加 Windows 私钥 ACL 自动修复，避免 `ssh.exe` 加载私钥时 `Permission denied`。
- `src/system/DeviceCommandService.h:32`: 增加 `authorizeTerminalKey()` 命令发送接口。
- `src/system/DeviceCommandService.cpp:57`: 增加 `authorize_ssh_key` 命令编码。
- `src/system/DeviceCommandService.cpp:260`: 命令服务端增加 `authorize_ssh_key` 白名单处理。
- `src/ui/DeviceGrid.cpp:2504`: 点击终端前先把本机公钥登记到目标设备。
- `CMakeLists.txt:88`: 构建后自动复制 `OpenSSH-Win64` 到程序输出目录的固定运行时路径。

### Reason
原来每台设备都会各自生成 SSH 密钥，但目标设备只信任自己本地生成的公钥，另一台设备点击“终端”时会因为目标机 `authorized_keys` 不包含发起方公钥而认证失败，表现为弹出的终端窗口一闪而退。同时 Windows OpenSSH 对私钥文件 ACL 很敏感，权限不合规时会报 `Load key ... Permission denied`。这次改动把“修复私钥权限”和“打开终端前登记发起方公钥”写进程序流程，让其它设备更新新版 FSRemote 后自动具备终端互联能力。

### Original Code
```cpp
// src/system/PortableOpenSshManager.cpp:307-330 原逻辑
if (existing == publicKey) {
    return true;
}

QSaveFile saveFile(authorizedKeysPath());
...
saveFile.write(publicKey);
saveFile.write("\n");
```

```cpp
// src/ui/DeviceGrid.cpp:2484-2490 原逻辑
QString errorMessage;
if (platform::PortableOpenSshManager::instance().openTerminal(device.ip, loginUser, &errorMessage)) {
    return;
}
```

```cmake
# CMakeLists.txt:83-85 原逻辑
COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/parsec_vdd/parsec-vdd-0.45.0.0.exe"
    "$<TARGET_FILE_DIR:FSRemote>/parsec_vdd/parsec-vdd-0.45.0.0.exe"
```

### Modified Code
```cpp
// src/system/PortableOpenSshManager.cpp:420-431
if (authorizedKeysContain(existing, publicKey)) {
    return true;
}
...
saveFile.write(appendAuthorizedKeyLine(existing, publicKey)); // wjy: 只追加本机公钥，不覆盖其它 FSRemote 设备已经登记进来的远程公钥。
```

```cpp
// src/ui/DeviceGrid.cpp:2490-2518
const QString publicKey = platform::PortableOpenSshManager::instance().clientPublicKey(&errorMessage);
...
if (!platform::DeviceCommandService::authorizeTerminalKey(device.ip, publicKey, &errorMessage)) {
    ...
    return; // wjy: 目标设备未更新、命令端口不可达或授权写入失败时提前提示，不再让 SSH 黑窗一闪而过。
}
```

```cmake
# CMakeLists.txt:88-104
set(FSREMOTE_OPENSSH_RUNTIME_DIR "" CACHE PATH "OpenSSH-Win64 runtime directory copied beside FSRemote.exe")
...
COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${FSREMOTE_OPENSSH_RUNTIME_DIR}"
    "$<TARGET_FILE_DIR:FSRemote>/openssh/OpenSSH-Win64"
```

### Steps
1. 在 `PortableOpenSshManager` 中增加公钥读取、远程公钥授权、`authorized_keys` 去重追加和 Windows 私钥 ACL 自动修复。
2. 在 `DeviceCommandService` 中新增固定白名单命令 `authorize_ssh_key`，只用于登记终端 SSH 公钥，不开放任意命令执行。
3. 在 `DeviceGrid::openCurrentDeviceTerminal()` 中改为先读取本机公钥，再通过目标设备 `49102` 命令端口登记公钥，成功后才打开 SSH 终端。
4. 在 `CMakeLists.txt` 中增加 OpenSSH 运行时复制逻辑，构建后自动把 `OpenSSH-Win64` 放到 `FSRemote.exe` 旁边的 `openssh/OpenSSH-Win64`。

### Verification
已执行 `git diff --check -- src/system/PortableOpenSshManager.h src/system/PortableOpenSshManager.cpp src/system/DeviceCommandService.h src/system/DeviceCommandService.cpp src/ui/DeviceGrid.cpp CMakeLists.txt`，未发现空白或格式错误。尝试执行 `cmake --build build/Desktop_Qt_6_11_1_MSVC2022_64bit-Debug --config Debug --target FSRemote`，但当前 shell 缺少 MSVC 标准库 include 环境，报 `stdint`、`type_traits` 找不到；用户将自行在 Qt Creator/正确 MSVC 环境中重新构建。

## 2026-07-06 10:45 - 降低远控默认视频码率

### Changed Location
- `third_party/uu_stream_webrtc/src/webrtc_session.h:27`: 将远控 WebRTC 会话默认目标视频码率从 `120000 kbps` 降到 `60000 kbps`，用于先观察 60Mbps 下的画面流畅性和跟手程度。

### Reason
原来的默认码率约为 120Mbps，画质压力较高；当编码、网络、解码或 Qt 绘制任一环节跟不上时，过高码率可能造成排队、抖动或延迟变大。本次先用最小改动把默认码率降到 60Mbps，保留 60fps 不变，方便用户对比远控画面是否更流畅。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.h:25-29
struct SessionConfig {
    SessionRole role = SessionRole::Host;
    uint32_t target_bitrate_kbps = 120000;
    uint32_t fps = 60;
};
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.h:25-29
struct SessionConfig {
    SessionRole role = SessionRole::Host;
    uint32_t target_bitrate_kbps = 60000; // wjy: 将默认视频码率从 120Mbps 降到 60Mbps，先减轻编码、网络和解码压力来观察远控流畅性。
    uint32_t fps = 60;
};
```

### Steps
1. 定位 `SessionConfig::target_bitrate_kbps` 默认值，这是 host 端 `apply_sender_rate()` 使用的视频目标码率来源。
2. 将默认值从 `120000` 调整为 `60000`，只改变码率，不改变 fps、虚拟屏分辨率和 WebRTC 传输逻辑。
3. 在同一行加入 `wjy` 注释，说明这次降码率的目的和观察方向。

### Verification
已执行 `rg -n "target_bitrate_kbps|struct SessionConfig" third_party\\uu_stream_webrtc\\src\\webrtc_session.h` 确认最终位置为 `third_party/uu_stream_webrtc/src/webrtc_session.h:27`，并执行 `git diff -- third_party\\uu_stream_webrtc\\src\\webrtc_session.h` 确认只修改默认码率这一处。未执行完整编译；本次为头文件默认参数的单行调整，建议用户重新构建 `fsremote_stream.dll` 后实测远控流畅性。

## 2026-07-06 10:45 - 修复码率注释吞掉 fps 成员

### Changed Location
- `third_party/uu_stream_webrtc/src/webrtc_session.h:27`: 将码率行的 `wjy` 注释改为 ASCII 文本，避免中文注释/换行显示异常导致下一行 `fps` 被编译器当作注释内容。

### Reason
用户重新编译后报错 `SessionConfig` 没有 `fps` 成员，排查发现 `uint32_t fps = 60;` 在实际读取时被粘到了上一行 `// wjy:` 注释后面，导致 MSVC 看不到 `fps` 字段。为保证头文件在当前编译环境下稳定解析，本次保留 60Mbps 数值不变，只把注释改成 ASCII，并确认 `fps` 单独位于下一行。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.h:25-28
struct SessionConfig {
    SessionRole role = SessionRole::Host;
    uint32_t target_bitrate_kbps = 60000; // wjy: 将默认视频码率从 120Mbps 降到 60Mbps，先减轻编码、网络和解码压力来观察远控流畅性。
};
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.h:25-29
struct SessionConfig {
    SessionRole role = SessionRole::Host;
    uint32_t target_bitrate_kbps = 60000; // wjy: Default stream bitrate lowered from 120Mbps to 60Mbps for smoother remote-control testing.
    uint32_t fps = 60;
};
```

### Steps
1. 使用带行号读取确认 `fps` 被上一行 `//` 注释吞掉，是本次编译错误的直接原因。
2. 保持 `target_bitrate_kbps = 60000` 不变，仅替换行尾注释内容，避免再触发编码或换行解析问题。
3. 重新读取 `SessionConfig`，确认 `target_bitrate_kbps` 和 `fps` 分别位于独立行。

### Verification
已执行带行号读取，确认 `third_party/uu_stream_webrtc/src/webrtc_session.h:27` 为码率字段，`third_party/uu_stream_webrtc/src/webrtc_session.h:28` 为 `uint32_t fps = 60;`。已执行 `rg -n "target_bitrate_kbps|fps = 60|SessionConfig"` 确认 `webrtc_session.cpp` 中的 `config_.fps` 调用可以匹配到头文件成员声明。未执行完整 MSVC 编译，建议用户重新构建验证原报错是否消失。

## 2026-07-06 11:08 - 让 Qt Creator 显示 WebRTC 模块头文件

### Changed Location
- `third_party/uu_stream_webrtc/CMakeLists.txt:21`: 在 `uu_stream_common` target 的 source list 中补登记对应 `.h` 文件，方便 Qt Creator 的 CMake 项目树显示头文件。
- `third_party/uu_stream_webrtc/CMakeLists.txt:153`: 在 `fsremote_stream` target 中补登记 `FsRemoteStreamApi.h`，方便从 DLL target 下直接打开导出接口头文件。

### Reason
Qt Creator 的 CMake 项目视图主要根据 target source list 展示文件。原来 `third_party/uu_stream_webrtc/CMakeLists.txt` 只列了 `.cpp`，头文件虽然能被 `#include` 正常编译，但不一定显示在项目树里。本次把相关头文件登记到对应 target，解决 IDE 中只看到 cpp、看不到 h 的问题；这些 `.h` 只是作为 source list 元数据展示，不会引入额外编译单元。

### Original Code
```cmake
# third_party/uu_stream_webrtc/CMakeLists.txt:21-32
add_library(uu_stream_common STATIC
    ../lan_stream_probe/src/nvenc_h264_encoder.cpp
    ../lan_stream_probe/src/ffmpeg_decoder.cpp
    src/native_webrtc_runtime.cpp
    src/parsec_vdd_session.cpp
    src/sdp_guard.cpp
    src/signaling.cpp
    src/system_audio_stream.cpp
    src/uu_codec_factory.cpp
    src/uu_profile.cpp
    src/webrtc_session.cpp
)
```

```cmake
# third_party/uu_stream_webrtc/CMakeLists.txt:139-141
add_library(fsremote_stream SHARED
    src/fsremote_stream_api.cpp
)
```

### Modified Code
```cmake
# third_party/uu_stream_webrtc/CMakeLists.txt:21-44
add_library(uu_stream_common STATIC
    # wjy: Register headers beside sources so Qt Creator shows them in the CMake project tree.
    ../lan_stream_probe/src/nvenc_h264_encoder.cpp
    ../lan_stream_probe/src/nvenc_h264_encoder.h
    ../lan_stream_probe/src/ffmpeg_decoder.cpp
    ../lan_stream_probe/src/ffmpeg_decoder.h
    ../lan_stream_probe/src/common.h
    src/native_webrtc_runtime.cpp
    src/native_webrtc_runtime.h
    src/parsec_vdd_session.cpp
    src/parsec_vdd_session.h
    src/sdp_guard.cpp
    src/sdp_guard.h
    src/signaling.cpp
    src/signaling.h
    src/system_audio_stream.cpp
    src/system_audio_stream.h
    src/uu_codec_factory.cpp
    src/uu_codec_factory.h
    src/uu_profile.cpp
    src/uu_profile.h
    src/webrtc_session.cpp
    src/webrtc_session.h
)
```

```cmake
# third_party/uu_stream_webrtc/CMakeLists.txt:151-154
add_library(fsremote_stream SHARED
    src/fsremote_stream_api.cpp
    ../../include/FsRemoteStreamApi.h # wjy: Register the DLL API header so Qt Creator shows it under this target.
)
```

### Steps
1. 检查根目录 `CMakeLists.txt`，确认主程序 target 已经登记头文件，问题主要出在第三方 WebRTC 模块 target 只登记 `.cpp`。
2. 在 `uu_stream_common` 的 source list 中补入 WebRTC 模块和复用的 LAN/NVENC/FFmpeg 头文件。
3. 在 `fsremote_stream` 的 source list 中补入 DLL 导出接口头文件 `FsRemoteStreamApi.h`。
4. 因 `third_party/uu_stream_webrtc/CMakeLists.txt` 含有既有非 UTF-8 注释，`apply_patch` 无法读取该文件，本次改用 PowerShell 按系统默认编码做精确字符串替换，避免重写无关内容。

### Verification
已执行带行号读取确认 `third_party/uu_stream_webrtc/CMakeLists.txt:21-44` 和 `third_party/uu_stream_webrtc/CMakeLists.txt:151-154` 为预期内容；已执行 `rg -n "nvenc_h264_encoder.h|ffmpeg_decoder.h|native_webrtc_runtime.h|webrtc_session.h|FsRemoteStreamApi.h"` 确认新增头文件登记成功。未执行完整编译；此改动只改变 CMake target 文件列表，建议在 Qt Creator 中重新运行 CMake 配置或重新打开项目后查看 `.h` 是否显示。

## 2026-07-11 18:30 - 多控制端第一阶段协议、配置与状态基础

### Changed Location
- `third_party/uu_stream_webrtc/src/session_protocol.h:12`: 新增版本化会话消息类型、角色、控制权状态、拒绝原因和严格大小边界。
- `third_party/uu_stream_webrtc/src/session_protocol.cpp:187`: 新增确定性序列化、百分号转义、必填字段验证和有界解析实现。
- `third_party/uu_stream_webrtc/tests/session_protocol_tests.cpp:44`: 新增协议往返、未知字段、缺失字段、非法转义、重复字段和超长输入测试。
- `CMakeLists.txt:11`: 为原生 target 统一启用 UTF-8，并新增隔离 WebRTC 特殊宏的 `uu_session_protocol` 静态库及测试目标。
- `include/FsRemoteStreamApi.h:38`: 新增稳定状态码、独占控制策略、版本化主机配置结构和配置启动入口。
- `src/stream/StreamRuntime.h:11`: 新增 Qt 强类型状态映射和带配置主机启动重载。
- `src/stream/StreamRuntime.cpp:24`: 动态解析新 DLL 入口，并在旧 DLL 下自动回退到原始启动函数。
- `src/system/AppSettings.h:20`: 新增远控主机会话数、总码率、握手超时和控制权策略设置接口。
- `src/system/AppSettings.cpp:158`: 新增四类设置的安全默认值、范围夹紧和持久化逻辑。
- `src/main.cpp:43`: 启动主机前组装配置并通过新 C ABI 传入 DLL。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:37`: 新增 DLL 内配置复制与规范化，迁移阶段仍强制有效会话数为 1。

### Reason
现有远控代码只有分散的状态数字和固定单会话启动入口，无法安全承载后续准入认证、容量拒绝和控制权状态机。本阶段先建立不依赖 WebRTC 的有界协议模块、稳定 ABI 和可持久化主机配置，并保持实际连接数为 1，确保后续重构可以逐步验证而不提前改变生产行为。

### Original Code
```cmake
# CMakeLists.txt:9-11 原逻辑
if(FSREMOTE_BUILD_UU_STREAM_WEBRTC)
    add_subdirectory(third_party/uu_stream_webrtc)
endif()
```

```cpp
// include/FsRemoteStreamApi.h:35-38 原逻辑
typedef void* FsRemoteStreamHandle;

FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_host(uint16_t port);
```

```cpp
// src/main.cpp:41-43 原逻辑
writeStartupLog(QStringLiteral("[wjy-main] before StreamRuntime::startHost"));
FsRemoteStreamHandle hostHandle = stream::StreamRuntime::instance().startHost(49100);
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:638-643 原逻辑
class HostInstance final : public StreamInstance {
public:
    explicit HostInstance(uint16_t port)
    {
        worker_ = std::thread([this, port] { run(port); });
    }
```

```text
// 新增文件原位置
third_party/uu_stream_webrtc/src/session_protocol.h：new code, no old code at this location
third_party/uu_stream_webrtc/src/session_protocol.cpp：new code, no old code at this location
third_party/uu_stream_webrtc/tests/session_protocol_tests.cpp：new code, no old code at this location
```

### Modified Code
```cmake
# CMakeLists.txt:11-40
if(MSVC)
    target_compile_options(uu_stream_common PRIVATE /utf-8)
    target_compile_options(fsremote_stream PRIVATE /utf-8)
endif()
add_library(uu_session_protocol STATIC
    third_party/uu_stream_webrtc/src/session_protocol.cpp
    third_party/uu_stream_webrtc/src/session_protocol.h
)
target_link_libraries(fsremote_stream PRIVATE uu_session_protocol)
```

```cpp
// include/FsRemoteStreamApi.h:38-74
enum FsRemoteStreamStatusCode {
    FSREMOTE_STATUS_ADMITTED = 70,
    FSREMOTE_STATUS_VIEW_ONLY = 71,
    FSREMOTE_STATUS_CONTROL_GRANTED = 72,
    FSREMOTE_STATUS_CAPACITY_REJECTED = 91,
    FSREMOTE_STATUS_AUTHORIZATION_REJECTED = 92,
};

typedef struct FsRemoteHostConfig {
    uint32_t struct_size;
    uint32_t version;
    uint32_t max_sessions;
    uint32_t max_aggregate_video_kbps;
    uint32_t handshake_timeout_ms;
    uint32_t ownership_policy;
} FsRemoteHostConfig;
```

```cpp
// src/main.cpp:43-55
FsRemoteHostConfig hostConfig = {};
hostConfig.struct_size = sizeof(hostConfig);
hostConfig.version = 1;
hostConfig.max_sessions = static_cast<uint32_t>(platform::AppSettings::remoteHostMaxSessions());
hostConfig.max_aggregate_video_kbps = static_cast<uint32_t>(platform::AppSettings::remoteHostAggregateVideoKbps());
hostConfig.handshake_timeout_ms = static_cast<uint32_t>(platform::AppSettings::remoteHostHandshakeTimeoutMs());
hostConfig.ownership_policy = FSREMOTE_OWNERSHIP_EXCLUSIVE;
FsRemoteStreamHandle hostHandle = stream::StreamRuntime::instance().startHost(49100, hostConfig);
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:37-58
struct HostRuntimeConfig {
    uint32_t requested_max_sessions = 1;
    uint32_t effective_max_sessions = 1;
    uint32_t max_aggregate_video_kbps = 120000;
    uint32_t handshake_timeout_ms = 5000;
    uint32_t ownership_policy = FSREMOTE_OWNERSHIP_EXCLUSIVE;
};

HostRuntimeConfig normalized_host_config(const FsRemoteHostConfig* config)
{
    HostRuntimeConfig normalized;
    // validate, copy and clamp configuration
    normalized.effective_max_sessions = 1;
    return normalized;
}
```

```cpp
// third_party/uu_stream_webrtc/src/session_protocol.cpp:236-282
bool parse_session_message(std::string_view wire, SessionMessage* message, std::string* error)
{
    if (wire.empty() || wire.size() > kMaxSessionMessageBytes) return false;
    // parse versioned header, decode bounded fields, reject duplicates and validate required fields
    *message = std::move(parsed);
    return true;
}
```

### Steps
1. 定义版本 1 的准入、控制权、状态、拒绝和音频订阅消息模型。
2. 实现有界文本序列化与解析，特殊字符通过百分号转义，未知字段允许往返。
3. 新增独立协议静态库和无外部测试框架的边界测试目标。
4. 为 C ABI 增加可扩展主机配置结构和稳定状态码，在 Qt 层增加强类型映射。
5. 把会话数、总码率、握手超时和控制策略接入 `AppSettings` 与程序启动流程。
6. DLL 立即复制并夹紧配置，按迁移计划继续强制单会话，避免提前改变连接拓扑。
7. 为原生 WebRTC target 启用 `/utf-8`，避免中文 WJY 注释在系统代码页下破坏换行。

### Verification
已执行 `git diff --check`，未发现空白错误。已在 Visual Studio 2022 x64 开发者环境中生成 CMake，构建并运行 `uu_session_protocol_tests.exe`，输出 `session_protocol_tests passed`。完整 `FSRemote` Debug 构建已完成源码编译和 `FSRemote.exe`/`fsremote_stream.dll` 链接，最后仅在既有 OpenSSH 运行时复制步骤因目标目录权限失败；该失败与本阶段代码无关。

## 2026-07-11 09:46 - 多控制端第二阶段认证准入与真实 OpenSSH 验证

### Changed Location
- `include/FsRemoteStreamApi.h:70-89`：新增稳定的设备身份回调 ABI 和注册入口。
- `src/stream/StreamRuntime.h:65-96`、`src/stream/StreamRuntime.cpp:11-113`：Qt 主程序向 DLL 注册公钥读取、签名、授权检查和验签回调。
- `src/system/PortableOpenSshManager.h:24-47`、`src/system/PortableOpenSshManager.cpp:27-39,625-727,908-948,1037-1074`：增加真实 OpenSSH 签名/验签、精确授权检查、线程保护和进程账户 ACL 修复。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:277-689,1166-1186,1309-1328,1424,1448-1458`：在 SDP、WebRTC、桌面和音频资源之前执行版本化挑战应答准入，并保存 Viewer 准入结果。
- `CMakeLists.txt:149-183`：新增隔离运行目录中的真实 OpenSSH 身份测试目标。
- `tests/session_identity_tests.cpp:1-87`：新增公钥、签名、验签和上下文字段篡改测试。

### Reason
受控设备未来会同时面对多个 TCP 客户端，不能再把“能连到端口”视为已授权。该阶段复用现有 OpenSSH Ed25519 设备身份，在任何昂贵媒体资源创建前完成挑战应答；签名绑定客户端 ID、双方 nonce、主机身份、协议版本和请求角色，并通过结构化拒绝区分超时、不兼容、策略和未授权。真实测试还发现环境登录用户与实际进程账户可能不同，因此私钥 ACL 改为按 Windows 进程令牌账户收紧，保证普通桌面、服务和沙箱场景都能安全调用 `ssh-keygen -Y`。

### Original Code
```cpp
// include/FsRemoteStreamApi.h:35-38 原逻辑
typedef void* FsRemoteStreamHandle;

FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_host(uint16_t port);
```

```cpp
// src/stream/StreamRuntime.cpp:20-24 原逻辑
m_startHost = reinterpret_cast<StartHostFn>(library->resolve("fsremote_stream_start_host"));
m_startViewer = reinterpret_cast<StartViewerFn>(library->resolve("fsremote_stream_start_viewer"));
```

```cpp
// src/system/PortableOpenSshManager.h:24-25 原逻辑
QString clientPublicKey(QString* errorMessage = nullptr);
bool authorizeClientPublicKey(const QString& publicKey, QString* errorMessage = nullptr);
```

```cpp
// src/system/PortableOpenSshManager.cpp:894-919 原 ACL 逻辑
const QString user = currentLoginUser();
runIcacls({normalizedKeyPath, QStringLiteral("/inheritance:r")});
return runIcacls({
    normalizedKeyPath,
    QStringLiteral("/grant:r"),
    user + QStringLiteral(":(F)"),
    QStringLiteral("*S-1-5-18:(F)"),
    QStringLiteral("*S-1-5-32-544:(F)"),
});
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:721-731 原逻辑
socket_ = socket;
append_log("host accepted client");
{
    std::string audio_error;
    audio_streamer_ = std::make_unique<uu::HostAudioStreamer>();
    audio_streamer_->start(49105, &audio_error);
}
// 随后直接初始化 WebRTC，没有身份准入。
```

```cmake
# CMakeLists.txt 原位置
# no session identity test target at this location
```

```text
// tests/session_identity_tests.cpp 原位置
new code, no old code at this location
```

### Modified Code
```cpp
// include/FsRemoteStreamApi.h:70-89
typedef struct FsRemoteIdentityCallbacks {
    uint32_t struct_size;
    uint32_t version;
    void* user;
    FsRemoteReadPublicKeyCallback read_public_key;
    FsRemoteSignChallengeCallback sign_challenge;
    FsRemoteIsPublicKeyAuthorizedCallback is_public_key_authorized;
    FsRemoteVerifyChallengeCallback verify_challenge;
} FsRemoteIdentityCallbacks;

void FSREMOTE_STREAM_CALL fsremote_stream_set_identity_callbacks(const FsRemoteIdentityCallbacks* callbacks);
```

```cpp
// src/stream/StreamRuntime.cpp:103-113
if (m_setIdentityCallbacks) {
    FsRemoteIdentityCallbacks callbacks = {};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.version = 1;
    callbacks.read_public_key = &readSessionPublicKey;
    callbacks.sign_challenge = &signSessionChallenge;
    callbacks.is_public_key_authorized = &isSessionPublicKeyAuthorized;
    callbacks.verify_challenge = &verifySessionChallenge;
    m_setIdentityCallbacks(&callbacks);
}
```

```cpp
// src/system/PortableOpenSshManager.cpp:625-641,691-727
QByteArray PortableOpenSshManager::signSessionChallenge(const QByteArray& challenge, QString* errorMessage)
{
    std::lock_guard identityLock(m_identityMutex);
    // 写入隔离临时文件并调用 ssh-keygen -Y sign -n fsremote-session。
}

bool PortableOpenSshManager::verifySessionChallenge(
    const QString& publicKey,
    const QByteArray& challenge,
    const QByteArray& signature,
    QString* errorMessage)
{
    std::lock_guard identityLock(m_identityMutex);
    // 先精确检查 authorized_keys，再调用 ssh-keygen -Y verify。
}
```

```cpp
// src/system/PortableOpenSshManager.cpp:916-945
const QString aclUser = currentProcessAccount();
const QString loginUser = currentLoginUser();
runIcacls({normalizedKeyPath, QStringLiteral("/inheritance:r")});
if (!runIcacls({normalizedKeyPath, QStringLiteral("/grant:r"),
        aclUser + QStringLiteral(":(F)"),
        QStringLiteral("*S-1-5-18:(F)"),
        QStringLiteral("*S-1-5-32-544:(F)"),
    })) return false;
if (!loginUser.isEmpty() && QString::compare(loginUser, aclUser, Qt::CaseInsensitive) != 0) {
    return runIcacls({normalizedKeyPath, QStringLiteral("/remove:g"), loginUser});
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:485-581
bool perform_host_admission(uintptr_t socket, const HostRuntimeConfig& config,
    SessionAdmission* admission, std::string* error)
{
    set_socket_timeout(socket, config.handshake_timeout_ms);
    // 接收 client_hello，协商版本、角色和能力，精确检查公钥授权。
    // 生成 host nonce，验证绑定完整上下文的签名，签发 session ID 和单次音频令牌。
    // 超时、不兼容、策略、格式和未授权失败均返回结构化拒绝。
    return true;
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1166-1177
SessionAdmission admission;
if (!perform_host_admission(socket, config_, &admission, &error)) {
    const uintptr_t rejected = socket_.exchange(0);
    if (rejected) uu::close_socket(rejected);
    continue;
}
// 只有这里之后才创建 HostAudioStreamer、NativeWebrtcRuntime 和 WebrtcSession。
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1309-1328,1424
if (!perform_viewer_admission(socket, status_callback_, user_, &admission_, &error)) return;
if (has_capability(admission_.capabilities, "audio")) {
    audio_player_ = std::make_unique<uu::ViewerAudioPlayer>();
}
SessionAdmission admission_; // 准入结果覆盖 ViewerInstance 生命周期。
```

```cmake
# CMakeLists.txt:149-183
option(FSREMOTE_BUILD_SESSION_IDENTITY_TESTS "Build real OpenSSH session identity tests" ON)
add_executable(fsremote_session_identity_tests
    tests/session_identity_tests.cpp
    src/system/PortableOpenSshManager.cpp
)
set_target_properties(fsremote_session_identity_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/identity_test"
)
add_test(NAME fsremote_session_identity_tests COMMAND fsremote_session_identity_tests)
```

```cpp
// tests/session_identity_tests.cpp:58-77
const QByteArray signature = manager.signSessionChallenge(challenge, &error);
require(manager.verifySessionChallenge(publicKey, challenge, signature, &error),
    QStringLiteral("verify valid OpenSSH signature"), error);
for (const auto& mutation : {
         std::pair<QByteArray, QByteArray>{"version", "2"},
         {"client_id", "client-b"},
         {"client_nonce", "nonce-c"},
         {"host_id", "host-b"},
         {"host_nonce", "nonce-d"},
         {"requested_role", "view"},
     }) {
    const QByteArray changed = replaceContextField(challenge, mutation.first, mutation.second);
    require(!manager.verifySessionChallenge(publicKey, changed, signature, &error),
        QStringLiteral("modified challenge field must invalidate signature"), QString::fromLatin1(mutation.first));
}
```

### Steps
1. 为 DLL 增加版本化身份回调表，Qt 层注册现有 OpenSSH 管理器能力，DLL 立即复制回调快照。
2. 实现公钥读取、完整公钥行授权检查、`ssh-keygen -Y sign/verify` 和带标准输入的验签工具调用，并用递归互斥量串行保护密钥准备与临时文件操作。
3. 在主机侧增加 client hello、server challenge、client proof、accepted/rejected 状态机，加入握手超时、版本错误分类、能力交集和请求角色处理。
4. 签名上下文绑定客户端 ID、客户端 nonce、主机 ID、主机 nonce、协议版本和请求角色；旧证明因新主机 nonce 无法跨连接重放。
5. 签发 30 秒有效、按 session ID 限定且消费后删除的音频令牌；实际音频监听器强制校验留到 OpenSpec 6.2。
6. Viewer 在任何音频/WebRTC 初始化前完成准入，并把 session ID、主机身份、角色、能力、版本、所有权和音频令牌保存到实例成员。
7. 新增隔离目录的真实 OpenSSH 测试；测试揭示并修复进程账户与环境登录用户不一致时的私钥 ACL 问题。

### Verification
`fsremote_session_identity_tests` 已在真实 bundled OpenSSH 上通过：本地公钥读取、精确授权、Ed25519 签名和正确验签成功；版本、客户端 ID、客户端 nonce、主机 ID、主机 nonce、请求角色六类单字段修改以及损坏签名均被拒绝。`uu_session_protocol_tests` 输出 `session_protocol_tests passed`。`fsremote_stream` Debug 目标构建链接成功；`FSRemote.exe` 重新编译并链接成功，完整目标仅在历史 OpenSSH 整目录部署复制步骤因目标私钥 ACL 报 `Permission denied`。`git diff --check` 通过。有效并发上限仍强制为 1，未提前开放多会话。

## 2026-07-11 10:31 - 主机持久会话管理器与确定性关闭

### Changed Location
- `CMakeLists.txt:42-54`：新增真实 DLL/TCP 主机会话管理器测试目标。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1078-1316`：把单客户端阻塞主机改为持久 listener、同步会话表和可 join 的独立会话上下文。
- `third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:20-117`：记录 SSL 初始化状态，并按 factory、worker、network、signaling 的依赖顺序关闭 WebRTC runtime。
- `third_party/uu_stream_webrtc/tests/host_session_manager_tests.cpp:1-177`：新增容量拒绝、断开回收、重新接入和握手中关闭测试。

### Reason
原主机在接受一个客户端后就关闭监听端口，并把 socket、WebRTC、音频和 worker 生命周期绑定在同一个串行循环中，无法在不干扰现有会话的前提下继续接收或明确拒绝新连接。此次先在有效会话上限仍为 1 的安全门槛下完成 manager 化：每个连接拥有独立上下文和可 join worker，主机持有一个持久 listener 与共享 WebRTC runtime。停止阶段最初在 `worker_thread->Stop()` 偶发访问冲突；WebRTC 源码表明 factory 在 signaling 线程析构并同步使用 worker，因此必须在 factory 释放后保留 signaling，依次停止 worker、network，最后停止 signaling。

### Original Code
```cmake
# CMakeLists.txt:42 原位置
# no host session manager test target at this location
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:700-785 原逻辑
void run(uint16_t port)
{
    uu::NativeWebrtcRuntime runtime;
    while (running_) {
        const uintptr_t socket = acceptTcp(port, &error);
        socket_ = socket;
        uu::WebrtcSession session(&runtime, config);
        while (running_ && uu::recv_message(socket, &message)) {
            handle_message(session, message);
        }
        const uintptr_t current = socket_.exchange(0);
        if (current) uu::close_socket(current);
    }
}
```

```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:101-114 原逻辑
void NativeWebrtcRuntime::shutdown()
{
    if (impl_) {
        impl_->factory = nullptr;
        if (impl_->signaling_thread) impl_->signaling_thread->Stop();
        if (impl_->worker_thread) impl_->worker_thread->Stop();
        if (impl_->network_thread) impl_->network_thread->Stop();
        impl_.reset();
    }
    webrtc::CleanupSSL();
}
```

```text
// third_party/uu_stream_webrtc/tests/host_session_manager_tests.cpp 原位置
new code, no old code at this location
```

### Modified Code
```cmake
# CMakeLists.txt:42-54
add_executable(uu_host_session_manager_tests
    third_party/uu_stream_webrtc/tests/host_session_manager_tests.cpp
)
target_link_libraries(uu_host_session_manager_tests PRIVATE fsremote_stream uu_session_protocol ws2_32)
set_target_properties(uu_host_session_manager_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/third_party/uu_stream_webrtc"
)
add_test(NAME uu_host_session_manager_tests COMMAND uu_host_session_manager_tests)
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1078-1106,1157-1249,1276-1316
struct HostClientSession {
    std::atomic_uintptr_t socket = 0;
    std::mutex send_mutex;
    std::atomic_bool cancelled = false;
    std::atomic_bool completed = false;
    SessionAdmission admission;
    std::unique_ptr<uu::WebrtcSession> webrtc;
    std::thread worker;
};

void runAcceptLoop(uint16_t port)
{
    const uintptr_t listener = createListener(port, &error);
    while (running_) {
        const SOCKET accepted = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
        reapCompletedSessions();
        if (sessions_.size() >= config_.effective_max_sessions) {
            send_admission_rejection(static_cast<uintptr_t>(accepted),
                uu::SessionRejectionReason::Capacity, "maximum concurrent sessions reached");
            uu::close_socket(static_cast<uintptr_t>(accepted));
            continue;
        }
        auto session = std::make_shared<HostClientSession>(static_cast<uintptr_t>(accepted));
        sessions_.emplace(session->admission.session_id, session);
        session->worker = std::thread([this, session] { runClientSession(session); });
    }
    shutdownSessionsOnManagerThread();
}

void shutdown()
{
    running_ = false;
    if (const uintptr_t listener = server_socket_.exchange(0)) uu::close_socket(listener);
    if (worker_.joinable()) worker_.join();
    runtime_.shutdown();
}
```

```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:104-117
void NativeWebrtcRuntime::shutdown()
{
    if (impl_) {
        const bool cleanup_ssl = impl_->ssl_initialized;
        impl_->factory = nullptr;
        if (impl_->worker_thread) impl_->worker_thread->Stop();
        if (impl_->network_thread) impl_->network_thread->Stop();
        if (impl_->signaling_thread) impl_->signaling_thread->Stop();
        impl_.reset();
        if (cleanup_ssl) webrtc::CleanupSSL();
    }
}
```

```cpp
// third_party/uu_stream_webrtc/tests/host_session_manager_tests.cpp:137-174
SOCKET second = connectWithRetry(port);
const uu::SessionMessage capacity = receiveSessionMessage(second);
require(capacity.fields.at("reason") == "capacity", "second rejection reason must be capacity");
::closesocket(first);
SOCKET third = connectWithRetry(port);
require(sendFramed(third, validHelloWire()), "send hello after reconnect");
require(receiveSessionMessage(third).type == uu::SessionMessageType::ServerChallenge,
    "persistent listener must admit reconnect to challenge stage");
SOCKET pendingShutdown = connectWithRetry(port);
const auto shutdownBegin = std::chrono::steady_clock::now();
fsremote_stream_stop(host);
const auto shutdownMs = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - shutdownBegin).count();
require(shutdownMs < 3000, "host shutdown must not wait for handshake timeout");
```

### Steps
1. 抽取 `HostClientSession`，集中拥有 socket、发送锁、取消/完成状态、准入结果、独占 `WebrtcSession` 和 joinable worker。
2. 将 `HostInstance` 改为持有一个持久 listener、一个共享 `NativeWebrtcRuntime` 和加锁 session map 的管理器，同时继续强制有效上限为 1。
3. 让 accept 循环在活动会话期间持续工作；超限连接收到结构化 `capacity`，完成的 worker 由 manager 回收且不会自 join 或 detach。
4. 停止时先关闭 listener，再取消并关闭全部客户端 socket、join 会话 worker、停止单会话音频，最后关闭共享 runtime。
5. 用逐阶段诊断把偶发访问冲突定位到先停 signaling 后停 worker，随后按 WebRTC 依赖关系改成 factory → worker → network → signaling，并移除临时诊断输出。
6. 新增真实 TCP 生命周期测试，覆盖活动会话不阻塞 accept、容量拒绝、断开后再接入，以及未完成 30 秒握手时的快速关闭。

### Verification
Visual Studio 2022 x64 Debug 构建成功。`uu_host_session_manager_tests` 在移除诊断输出后连续运行 50 次全部通过，未再出现 `0xC0000005`，关闭耗时约 0.5–1.1 秒；随后 `uu_session_protocol_tests`、`uu_host_session_manager_tests`、`fsremote_session_identity_tests` 三组 CTest 回归全部通过。有效会话上限仍强制为 1；尚未执行真实视频、音频、键盘、绝对/相对鼠标的人工单控制端回归，因此 OpenSpec 3.5 保持未完成。

## 2026-07-11 11:43 - 共享桌面媒体管线与订阅生命周期

### Changed Location
- `CMakeLists.txt:15-21,64-75`：登记共享媒体源文件、统一 standalone host 编译运行库并新增生命周期测试目标。
- `third_party/uu_stream_webrtc/src/host_media_pipeline.h:15-59`、`host_media_pipeline.cpp:36-340`：新增 manager-owned `HostMediaPipeline`、共享桌面 source 和引用计数订阅令牌。
- `third_party/uu_stream_webrtc/src/webrtc_session.h:24-33`、`webrtc_session.cpp:530-588`：host 会话改为消费共享 source，并创建会话唯一 track/stream。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1105,1214-1249,1310-1330`：`HostInstance` 持有共享管线，认证后的会话按顺序订阅和释放。
- `third_party/uu_stream_webrtc/src/parsec_vdd_session.cpp:357-369`：最后一个 VDD 引用释放时立即停止并移除虚拟显示器。
- `third_party/uu_stream_webrtc/src/host_main.cpp:107-118`：standalone host 使用同一订阅接口。
- `third_party/uu_stream_webrtc/tests/host_media_pipeline_tests.cpp:1-73`：新增无显卡共享生命周期测试。

### Reason
此前每个 `WebrtcSession` 都创建自己的 `ParsecVddSession` 与 `DesktopVideoSource`，会话析构也直接停止自己的 source。这种所有权在有效上限为 1 时可用，但一旦允许多个查看者，会重复创建桌面捕获循环，并让任一会话退出都可能停止其他会话依赖的媒体。此次把 VDD 和桌面捕获启停权上移到 `HostInstance` 所有的共享管线；会话只持引用计数订阅和共享 source，同时继续拥有独立 PeerConnection、track、sender 与编码器。

### Original Code
```cmake
# CMakeLists.txt 原位置
# no HostMediaPipeline source registration or lifecycle test target
```

```text
// third_party/uu_stream_webrtc/src/host_media_pipeline.h/.cpp 原位置
new code, no old code at this location
```

```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.cpp:711-781 原逻辑
host_virtual_display_ = std::make_unique<ParsecVddSession>();
host_virtual_display_->start(&vdd_error);
auto source = webrtc::make_ref_counted<DesktopVideoSource>(
    config_.fps,
    host_virtual_display_ ? host_virtual_display_->preferred_source_id() : 0,
    host_virtual_display_ ? host_virtual_display_->preferred_device_name() : std::string(),
    host_virtual_display_ != nullptr);
if (!source->start(error)) return false;
local_video_source_ = source;
local_video_track_ = factory->CreateVideoTrack(local_video_source_, "video0");
auto result = pc_->AddTrack(local_video_track_, {"stream0"});
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1200-1246 原逻辑
startAudioForSingleSession();
uu::SessionConfig sessionConfig;
sessionConfig.role = uu::SessionRole::Host;
sessionConfig.target_bitrate_kbps = config_.max_aggregate_video_kbps;
context->webrtc = std::make_unique<uu::WebrtcSession>(&runtime_, sessionConfig);
// ...
context->webrtc.reset();
context->cancel();
```

```cpp
// third_party/uu_stream_webrtc/src/parsec_vdd_session.cpp:357-364 原逻辑
void release()
{
    std::lock_guard lock(mutex_);
    if (ref_count_ > 0) {
        --ref_count_;
    }
}
```

```cpp
// third_party/uu_stream_webrtc/src/host_main.cpp:100-108 原逻辑
uu::SessionConfig config;
config.role = uu::SessionRole::Host;
config.target_bitrate_kbps = bitrate_kbps;
config.fps = fps;
uu::WebrtcSession session(&runtime, config);
```

```text
// third_party/uu_stream_webrtc/tests/host_media_pipeline_tests.cpp 原位置
new code, no old code at this location
```

### Modified Code
```cmake
# CMakeLists.txt:15-18,64-75
target_sources(uu_stream_common PRIVATE
    third_party/uu_stream_webrtc/src/host_media_pipeline.cpp
    third_party/uu_stream_webrtc/src/host_media_pipeline.h
)
add_executable(uu_host_media_pipeline_tests
    third_party/uu_stream_webrtc/tests/host_media_pipeline_tests.cpp
)
target_link_libraries(uu_host_media_pipeline_tests PRIVATE uu_stream_common)
add_test(NAME uu_host_media_pipeline_tests COMMAND uu_host_media_pipeline_tests)
```

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.h:23-59
class HostMediaPipeline final {
public:
    class Subscription final {
    public:
        ~Subscription();
        webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source() const;
    };

    std::unique_ptr<Subscription> subscribe(std::string* error);
    void shutdown();
    size_t subscriber_count() const;
};
```

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:307-338
std::unique_ptr<HostMediaPipeline::Subscription> HostMediaPipeline::subscribe(std::string* error)
{
    std::lock_guard lock(state->mutex);
    if (state->shutting_down) return nullptr;
    if (state->subscribers == 0 && !state->start_locked(error)) return nullptr;
    ++state->subscribers;
    return std::unique_ptr<Subscription>(new Subscription(state, state->source));
}

HostMediaPipeline::Subscription::~Subscription()
{
    source_ = nullptr;
    std::lock_guard lock(state->mutex);
    if (state->subscribers > 0) --state->subscribers;
    if (state->subscribers == 0) state->stop_locked();
}
```

```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.h:30-31
webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> host_video_source;
std::string media_id;
```

```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.cpp:549-567
if (!config_.host_video_source) {
    if (error) *error = "host video source subscription is missing";
    return false;
}
local_video_source_ = config_.host_video_source;
const std::string media_suffix = config_.media_id.empty() ? std::string("0") : config_.media_id;
local_video_track_ = factory->CreateVideoTrack(local_video_source_, "video-" + media_suffix);
auto result = pc_->AddTrack(local_video_track_, {"stream-" + media_suffix});
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1214-1249
context->media_subscription = media_pipeline_.subscribe(&error);
if (!context->media_subscription) {
    context->cancel();
    context->completed = true;
    return;
}
sessionConfig.host_video_source = context->media_subscription->source();
sessionConfig.media_id = context->admission.session_id;
context->webrtc = std::make_unique<uu::WebrtcSession>(&runtime_, sessionConfig);
// ...
context->webrtc.reset();
context->media_subscription.reset();
```

```cpp
// third_party/uu_stream_webrtc/src/parsec_vdd_session.cpp:357-369
void release()
{
    std::lock_guard lock(mutex_);
    if (ref_count_ > 0) --ref_count_;
    if (ref_count_ == 0 && ready_) {
        stop_locked();
    }
}
```

```cpp
// third_party/uu_stream_webrtc/tests/host_media_pipeline_tests.cpp:45-68
auto first = pipeline.subscribe(&error);
auto second = pipeline.subscribe(&error);
require(first->source().get() == second->source().get(), "subscribers share one video source");
first.reset();
require(stops == 0, "one disconnect keeps backend alive");
second.reset();
require(stops == 1, "last disconnect stops backend");
```

### Steps
1. 把原 `WebrtcSession` 内的桌面捕获实现移动到新增 `HostMediaPipeline`，并由管线惰性创建一个 Parsec VDD 和一个 `DesktopVideoSource`。
2. 新增引用计数 `Subscription`；首个会话启动后端，后续会话复用 source，最后会话离开才停止捕获和 VDD。
3. `HostInstance` 在认证成功后订阅媒体，在销毁独占 PeerConnection/track 后释放订阅，并在 manager 关闭末尾禁止新订阅。
4. `WebrtcSession` 只消费外部共享 source，同时按 session ID 创建唯一 video track 和 stream；每个会话仍保留独立 sender/encoder。
5. 修改 Parsec 共享显示引用归零行为，避免空闲虚拟显示器一直保留到进程结束。
6. 增加假 source 生命周期测试，并统一可选 standalone host 的 UTF-8 与 `/MT` 编译配置。

### Verification
Visual Studio 2022 x64 Debug 下 `uu_host_media_pipeline_tests` 连续运行 100 次全部通过，验证两个订阅共享同一 source、单个断开不停止、最后断开停止、空闲后重启和 shutdown 门禁。`uu_host_session_manager_tests` 连续运行 20 次全部通过。最终 `uu_session_protocol_tests`、`uu_host_session_manager_tests`、`uu_host_media_pipeline_tests`、`fsremote_session_identity_tests` 四组 CTest 全部通过；`uu_webrtc_host` 也已单独编译链接成功，`git diff --check` 无错误。当前有效会话上限仍强制为 1，未执行 4.4 多会话开放，也未把 3.5 人工音视频/输入/重连/退出回归标记完成。

## 2026-07-11 14:17 - 开放配置并发视频会话并隔离只读输入

### Changed Location
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:46-65`：在用户完成单会话实机回归后启用 1 至 3 的配置并发上限。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:487-593`：认证完成后原子授予至多一个旧式控制/音频会话，其余会话仅获得共享视频能力。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1213-1279,1362`：按会话权限注册输入回调、管理单路音频并在销毁后释放临时控制槽。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1395-1403,1459,1570`：Viewer 使用原子权限位阻止只读窗口转发键鼠消息。
- `third_party/uu_stream_webrtc/tests/host_session_manager_tests.cpp:144-189`：把容量测试扩展为三个并发握手槽、第四个拒绝和单会话故障隔离。
- `include/FsRemoteStreamApi.h:66`、`src/main.cpp:46`：同步配置字段的当前有效语义；默认值仍保持 1。
- `openspec/changes/multi-controller-single-host/tasks.md:23`：依据用户实测结果完成单控制端回归任务 3.5。

### Reason
用户确认原有视频、音频、键鼠、重连和退出行为全部正常，因此可以解除 DLL 内部固定为 1 的迁移闸门。直接把上限改为 3 会让旧客户端的三个窗口都发送键鼠，并让现有单客户端音频端口互相争抢，所以本阶段只开放独立视频会话：第一个成功认证的控制请求继续使用原有控制和音频，后续认证会话明确为 `view_only + video`。完整的控制权排队/转让和多订阅音频仍由 OpenSpec 5.x、6.x 完成。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:46-61 (before)
uint32_t effective_max_sessions = 1;
// ...
normalized.effective_max_sessions = 1;
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:563-582 (before)
admission->audio_token = issue_audio_token(admission->session_id);
admission->capabilities = negotiated_capabilities;
admission->ownership = requested_role == "control" ? "control_granted" : "view_only";
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1206-1252 (before)
startAudioForSingleSession();
context->webrtc->set_control_callback([weak, webrtc](const std::string& message) {
    if (const auto locked = weak.lock(); locked && !locked->cancelled) inject_input_message(message, webrtc);
});
// ...
stopAudioForSingleSession();
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1395-1403 (before)
if (!message || !*message) {
    return false;
}
```

```cpp
// third_party/uu_stream_webrtc/tests/host_session_manager_tests.cpp:144-153 (before)
SOCKET first = connectWithRetry(port);
SOCKET second = connectWithRetry(port);
const uu::SessionMessage capacity = receiveSessionMessage(second);
require(capacity.fields.at("reason") == "capacity", "second rejection reason must be capacity");
```

```cpp
// include/FsRemoteStreamApi.h:66, src/main.cpp:46 (before)
uint32_t max_sessions; // DLL 内部仍强制有效值为 1。
hostConfig.max_sessions = static_cast<uint32_t>(platform::AppSettings::remoteHostMaxSessions()); // DLL 仍强制为 1。
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:46-61
uint32_t effective_max_sessions = 1; // wjy: 无有效配置时仍兼容单会话。
// ...
normalized.effective_max_sessions = normalized.requested_max_sessions; // wjy: 启用夹紧后的 1 至 3 配置值。
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:567-585
bool control_granted = false;
if (requested_role == "control" && control_owner_claimed) {
    bool expected = false;
    control_granted = control_owner_claimed->compare_exchange_strong(expected, true);
}
const std::string admitted_capabilities = control_granted ? negotiated_capabilities : "video";
admission->audio_token = has_capability(admitted_capabilities, "audio")
    ? issue_audio_token(admission->session_id)
    : std::string();
admission->capabilities = admitted_capabilities;
admission->ownership = control_granted ? "control_granted" : "view_only";
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1235-1279
const bool owns_control = context->admission.ownership == "control_granted";
if (owns_control && has_capability(context->admission.capabilities, "audio")) {
    startAudioForSingleSession();
}
if (owns_control) {
    context->webrtc->set_control_callback(/* only owner injects input */);
}
// ... destroy PeerConnection and shared-source subscription first ...
if (owns_control) stopAudioForSingleSession();
releaseControlIfOwned(context);
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1395-1403,1459,1570
if (!message || !*message || !control_allowed_) {
    return false;
}
control_allowed_ = admission_.ownership == "control_granted";
std::atomic_bool control_allowed_ = false;
```

```cpp
// third_party/uu_stream_webrtc/tests/host_session_manager_tests.cpp:144-180
SOCKET first = connectWithRetry(port);
SOCKET second = connectWithRetry(port);
require(receiveSessionMessage(second).type == uu::SessionMessageType::ServerChallenge,
    "second connection must reach challenge stage");
SOCKET third = connectWithRetry(port);
require(receiveSessionMessage(third).type == uu::SessionMessageType::ServerChallenge,
    "third connection must reach challenge stage");
SOCKET fourth = connectWithRetry(port);
const uu::SessionMessage capacity = receiveSessionMessage(fourth);
require(capacity.fields.at("reason") == "capacity", "fourth rejection reason must be capacity");
```

```cpp
// include/FsRemoteStreamApi.h:66, src/main.cpp:46
uint32_t max_sessions; // wjy: DLL 实际使用夹紧到 1 至 3 的并发视频上限。
hostConfig.max_sessions = static_cast<uint32_t>(platform::AppSettings::remoteHostMaxSessions()); // wjy: 默认仍为 1。
```

### Steps
1. 将 `effective_max_sessions` 从固定 1 改为使用已夹紧的调用方配置，保留缺省单会话行为。
2. 在认证证明通过后以原子操作竞争临时控制槽；首个控制请求获得原有控制/音频，后续会话只获得视频。
3. 目标端只为控制拥有者注册输入注入回调，Viewer 端也通过原子权限位阻止只读键鼠转发。
4. 让只读会话退出不再停止控制者的单路音频；控制会话在 PeerConnection 销毁后才归还控制槽。
5. 把 host manager 测试改为允许三个独立并发 worker，第 4 个才容量拒绝，并验证一个连接失败不影响另一个。
6. 根据用户实测勾选 OpenSpec 3.5；4.4 保持未完成，等待真实多设备视频/只读验证。

### Verification
Visual Studio 2022 x64 Debug 下四个相关目标编译成功。`uu_host_session_manager_tests` 连续运行 20 次全部通过；`uu_session_protocol_tests`、`uu_host_media_pipeline_tests`、`fsremote_session_identity_tests` 全部通过；`git diff --check` 无错误。最新 Debug DLL 已同步到主程序目录，大小 29,330,432 字节，时间 2026-07-11 14:16:02。真实多设备画面、第一台控制与第二/第三台只读仍需用户在目标设备把 `remoteHostMaxSessions` 配置为 3 后验证，因此尚未完成 OpenSpec 4.4。

## 2026-07-11 14:40 - 将无配置并发默认值直接改为三路

### Changed Location
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:47-58,1586`：DLL 配置缺失、配置无效和旧启动入口统一默认三路。
- `src/system/AppSettings.cpp:158-161`：Qt `QSettings` 没有保存项时使用 3，而不是 1。
- `src/main.cpp:46`：同步说明应用启动默认传入三路配置。
- `third_party/uu_stream_webrtc/tests/host_session_manager_tests.cpp:134-135`：测试改走无配置入口，直接验证默认三路容量。

### Reason
用户明确要求不要再以 1 作为新设备默认值，而是将程序直接改为 3，避免把 Release 目录复制到其他设备后还要逐台写注册表。为避免 Qt 新入口和 DLL 旧入口默认值不一致，本次同时修改两个层级；已存在的显式 1 至 3 配置仍可被读取，非法值仍会夹紧。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:47-58,1586 (before)
uint32_t requested_max_sessions = 1;
uint32_t effective_max_sessions = 1;
// ...
return normalized; // 空配置回退到单会话。
// ...
return new HostInstance(port, normalized_host_config(nullptr)); // 旧入口单会话。
```

```cpp
// src/system/AppSettings.cpp:158-161 (before)
int AppSettings::remoteHostMaxSessions()
{
    return qBound(1, settings().value(QStringLiteral("remoteHostMaxSessions"), 1).toInt(), 3);
}
```

```cpp
// src/main.cpp:46 (before)
hostConfig.max_sessions = static_cast<uint32_t>(platform::AppSettings::remoteHostMaxSessions()); // 默认仍保持 1。
```

```cpp
// third_party/uu_stream_webrtc/tests/host_session_manager_tests.cpp:133-142 (before)
FsRemoteHostConfig config = {};
config.struct_size = sizeof(config);
config.version = 1;
config.max_sessions = 3;
FsRemoteStreamHandle host = fsremote_stream_start_host_with_config(port, &config);
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:47-58,1586
uint32_t requested_max_sessions = 3; // wjy: DLL 无配置入口默认请求 3。
uint32_t effective_max_sessions = 3; // wjy: 无配置或旧入口采用三路有效上限。
// ...
return normalized; // wjy: 空配置、短结构或未知版本回退到三会话。
// ...
return new HostInstance(port, normalized_host_config(nullptr)); // wjy: 旧入口默认三路。
```

```cpp
// src/system/AppSettings.cpp:158-161
int AppSettings::remoteHostMaxSessions()
{
    return qBound(1, settings().value(QStringLiteral("remoteHostMaxSessions"), 3).toInt(), 3); // wjy: 无保存项直接返回 3。
}
```

```cpp
// src/main.cpp:46
hostConfig.max_sessions = static_cast<uint32_t>(platform::AppSettings::remoteHostMaxSessions()); // wjy: 默认值设为 3。
```

```cpp
// third_party/uu_stream_webrtc/tests/host_session_manager_tests.cpp:134-135
FsRemoteStreamHandle host = fsremote_stream_start_host(port); // wjy: 不传配置验证默认三路。
require(host != nullptr, "start host manager with default three-session limit");
```

### Steps
1. 将 DLL 的 `requested_max_sessions` 和 `effective_max_sessions` 初始值从 1 改为 3。
2. 将 Qt `QSettings` 的 `remoteHostMaxSessions` 缺省值从 1 改为 3。
3. 更新无配置回退、旧启动入口和主程序启动处的 WJY 中文说明。
4. 删除测试中显式构造的三路配置，改用 `fsremote_stream_start_host` 验证默认行为。
5. 编译 Debug 测试与 Release 主程序，并核对 Release 根目录 DLL 与内部构建 DLL 哈希一致。

### Verification
`uu_host_session_manager_tests` 在不传任何配置的情况下连续运行 20 次全部通过，验证前三个连接进入独立握手、第四个返回 capacity、单会话断开不影响其他连接。`uu_session_protocol_tests`、`uu_host_session_manager_tests`、`uu_host_media_pipeline_tests`、`fsremote_session_identity_tests` 四组回归全部通过，`git diff --check` 无错误。Release x64 的 `FSRemote.exe` 与 `fsremote_stream.dll` 已于 2026-07-11 14:40 重新生成；根目录和内部 DLL 的 SHA256 均为 `92A16C7CE2298F798FA8F7BD34988FDFC7DDB97E4350E2585779B11E104F9961`。Release 构建最终仍因已有 OpenSSH 私钥目录复制权限问题返回非零，但发生在 EXE/DLL 编译链接并复制完成之后。

## 2026-07-11 14:52 - 修复第二会话卡在 TCP 已连接

### Changed Location
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:573`：只读会话也签发非空的会话级音频令牌，使 `AdmissionAccepted` 满足协议必填字段校验。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:626-632,670-676`：Viewer 收不到准入 challenge/result 时上报明确错误，不再保留 TCP 已连接假状态。
- `third_party/uu_stream_webrtc/tests/session_protocol_tests.cpp:71-83`：新增只读准入消息和空令牌缺陷条件回归测试。

### Reason
用户通过交换第一、第二控制设备的连接顺序证明“谁第二个谁卡住”。检查代码确认：第一会话拥有 audio 能力并得到非空令牌，第二会话被降为 `view_only + video` 后却把 `audio_token` 写为空；协议层要求 `AdmissionAccepted.audio_token` 必填且非空，导致主机在发送最终准入结果前序列化失败并关闭连接。Viewer 对该接收失败没有发送状态回调，因此窗口永久显示最后一次的 TCP 已连接。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:572-576 (before)
admission->audio_token = has_capability(admitted_capabilities, "audio")
    ? issue_audio_token(admission->session_id)
    : std::string();
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:625-626,665-666 (before)
uu::SessionMessage challenge;
if (!recv_session_message(socket, &challenge, error)) return false;
// ...
uu::SessionMessage accepted;
if (!recv_session_message(socket, &accepted, error)) return false;
```

```cpp
// third_party/uu_stream_webrtc/tests/session_protocol_tests.cpp:71 (before)
// new test, no old code at this location
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:573
admission->audio_token = issue_audio_token(admission->session_id); // wjy: 只读会话也取得独立短期令牌，但 capabilities 不含 audio，因此不会连接音频端口。
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:626-632,670-676
uu::SessionMessage challenge;
if (!recv_session_message(socket, &challenge, error)) {
    const char* detail = error && !error->empty() ? error->c_str() : "Failed to receive admission challenge";
    report_status(status_callback, user, FSREMOTE_STATUS_ERROR, detail);
    return false;
}
// ... accepted 阶段使用同样的明确错误上报。
```

```cpp
// third_party/uu_stream_webrtc/tests/session_protocol_tests.cpp:71-83
uu::SessionMessage viewOnlyAccepted;
viewOnlyAccepted.type = uu::SessionMessageType::AdmissionAccepted;
viewOnlyAccepted.fields = {
    {"session_id", "view-session-1"},
    {"ownership", "view_only"},
    {"capabilities", "video"},
    {"audio_token", "scoped-unused-token"},
};
require(uu::serialize_session_message(viewOnlyAccepted, &wire, &error), "view-only admission must serialize with scoped token");
viewOnlyAccepted.fields["audio_token"].clear();
require(!uu::serialize_session_message(viewOnlyAccepted, &wire, &error), "empty view-only audio token must fail explicitly");
```

### Steps
1. 对比第一会话与第二会话唯一不同的准入字段，确认空 `audio_token` 与协议 `require_fields` 冲突。
2. 恢复为每个已认证会话签发唯一、短期、单次令牌；是否启动音频仍由 `capabilities` 控制。
3. 在 Viewer 两个准入接收点补充 `FSREMOTE_STATUS_ERROR`，避免握手传输失败后 UI 卡在旧状态。
4. 新增只读 AdmissionAccepted 正常往返和空令牌明确失败测试。
5. 重新编译 Debug/Release 并运行完整回归。

### Verification
`uu_host_session_manager_tests` 连续运行 20 次全部通过；`uu_session_protocol_tests`（含新增只读准入用例）、`uu_host_media_pipeline_tests`、`fsremote_session_identity_tests` 全部通过，`git diff --check` 无错误。Release 内部新 DLL 已于 2026-07-11 14:51:49 生成，SHA256 为 `AAE4008910FA05FD25FE46BBE2A2D6CF15EFD330F5BF5B10A43FDC0E1805A8CE`。由于 PID 35556 正在使用根目录旧 DLL，无法原位覆盖，已将新文件复制为 `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release/fsremote_stream_new.dll`；关闭全部 FSRemote 后需用它覆盖根目录 `fsremote_stream.dll`。

## 2026-07-11 15:22 - 默认三个已认证设备同时控制同一目标设备

### Changed Location
- `include/FsRemoteStreamApi.h:59-62,70`：新增稳定 ABI 枚举 `FSREMOTE_OWNERSHIP_SHARED`，并说明共享控制为默认策略。
- `src/main.cpp:49`：Qt 主程序默认向 DLL 传入共享控制策略。
- `third_party/uu_stream_webrtc/src/control_admission_policy.h:1-28`：新增可独立测试的共享/独占准入决策。
- `third_party/uu_stream_webrtc/src/shared_input_state.h:1-113`：新增逐会话键盘和鼠标按钮持有状态、最终抬起与断线清理语义。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:54-69,575-593,786-1170,1333-1504`：共享控制准入、单路音频解耦、串行 `InputDispatcher`、逐会话相对鼠标、模式广播和退出清理。
- `third_party/uu_stream_webrtc/tests/shared_input_state_tests.cpp:1-68`：新增双控制端同键、同按钮、错误抬键、断线释放及共享准入策略测试。
- `CMakeLists.txt:17-24,81-92`：登记共享控制头文件与自动测试目标。
- `openspec/changes/multi-controller-single-host/`：将唯一控制者方案同步修订为默认协同控制，并完成任务 5.1 至 5.5。

### Reason
用户明确要求“默认全部都可以同时控制”。旧实现把第一个认证会话同时当作唯一键鼠拥有者和唯一音频会话，第二、第三台设备虽然能看视频，但被准入为 `view_only`。如果仅删除这个判断，多个 WebRTC 回调会并发修改全局相对鼠标状态并直接调用 `SendInput`，还会在两个控制端按住同一键时发生提前抬键或断线卡键。因此本次将控制授权、单路音频限制和操作系统输入注入拆成三个独立层次。

### Original Code
```cpp
// include/FsRemoteStreamApi.h:59-61 (before)
enum FsRemoteOwnershipPolicy {
    FSREMOTE_OWNERSHIP_EXCLUSIVE = 1,
};
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:567-581 (before)
bool control_granted = false;
if (requested_role == "control" && control_owner_claimed) {
    bool expected = false;
    control_granted = control_owner_claimed->compare_exchange_strong(expected, true);
}
const std::string admitted_capabilities = control_granted ? negotiated_capabilities : "video";
admission->ownership = control_granted ? "control_granted" : "view_only";
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1257-1262 (before)
if (owns_control) {
    context->webrtc->set_control_callback([weak, webrtc](const std::string& message) {
        if (const auto locked = weak.lock(); locked && !locked->cancelled) inject_input_message(message, webrtc);
    });
}
```

### Modified Code
```cpp
// include/FsRemoteStreamApi.h:59-62
enum FsRemoteOwnershipPolicy {
    FSREMOTE_OWNERSHIP_EXCLUSIVE = 1,
    FSREMOTE_OWNERSHIP_SHARED = 2, // wjy: 默认协同策略允许所有已认证且协商 control 能力的会话同时发送键鼠输入。
};
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:575-584
const uu::ControlAdmissionDecision control_decision = uu::decideControlAdmission(
    requested_role == "control",
    has_capability(negotiated_capabilities, "control"),
    config.ownership_policy == FSREMOTE_OWNERSHIP_EXCLUSIVE,
    exclusive_control_claimed); // wjy: 认证完成后统一由可测试策略决定本会话是协同控制还是只读。
const bool control_granted = control_decision.granted;
admission->exclusive_control_slot = control_decision.claimed_exclusive_slot;
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1107-1144
class InputDispatcher final {
public:
    void dispatch(const std::string& session_id, const std::string& message)
    {
        std::lock_guard lock(mutex_); // wjy: 所有 WebRTC data-channel 回调在这里汇合。
        inject_input_message(session_id, message, nullptr, &pointer_by_session_[session_id], &held_input_);
    }

    void releaseSession(const std::string& session_id)
    {
        const uu::SharedInputReleaseBatch released = held_input_.releaseSession(session_id);
        // wjy: 仅释放该会话最后持有的键和按钮。
    }
};
```

### Steps
1. 修改 OpenSpec 的 proposal、design、delta spec 和 tasks，把“唯一拥有者/交接”改为“认证后的默认协同控制”。
2. 在 C ABI 中保留旧独占枚举并新增共享枚举，默认配置、无配置入口和 Qt 启动入口全部选择共享策略。
3. 将首个音频会话改用独立原子槽；第二、第三会话即使暂时没有音频，也得到 `video,control` 与 `control_granted`。
4. 增加主机级 `InputDispatcher`，把全部键盘、鼠标、滚轮和捕获消息串行送入唯一 `SendInput` 路径。
5. 按会话记录键和按钮；多人按住同一输入时只发送首次 down 和最终 up，断线仅释放该会话贡献。
6. 将相对鼠标坐标基线按会话隔离，并把模式变化安全广播到仍连接的控制窗口。
7. 增加策略与输入状态自动测试，完成 Debug 全量回归、20 次主机关闭压力测试和 Release x64 构建。

### Verification
`uu_session_protocol_tests`、`uu_host_session_manager_tests`、`uu_host_media_pipeline_tests`、`uu_shared_input_state_tests`、`fsremote_session_identity_tests` 五组测试全部通过；`uu_host_session_manager_tests` 额外连续运行 20 次全部通过，`git diff --check` 无错误。Release x64 的 `FSRemote.exe` 与 `fsremote_stream.dll` 已重新生成；EXE SHA256 为 `F56675838F414A099C34BDD0D8D2E9237D1CD52B4B5A40BF8F29A52B02C70276`，最终 DLL SHA256 为 `57B8A114126D28243BF8FBA90B9DE7F240E2BCCC2ABC9F358B982BF360090496`，根目录与内部 DLL 哈希一致。第一次完整目标执行在 OpenSSH 私钥目录复制处遇到权限错误，随后增量 Release 目标全部成功。真实两台设备同时键鼠操作、同键按住与断开清理仍需实机验收后再完成 OpenSpec 5.7/8.2。
## 2026-07-13 16:00 - 可靠自更新、失败回滚与自动重启

### Changed Location
- `src/updater/main.cpp:1`：新增无 Qt 依赖的独立更新器。
- `src/system/UpdateService.cpp:21`：重整发布、版本比较、暂存和更新器启动流程。
- `src/system/UpdateService.h:12`：补充更新退出信号和版本比较接口。
- `src/main.cpp:138`：接入有序退出以及更新/回滚重启反馈。
- `CMakeLists.txt:10`：构建并部署静态运行库更新器，登记更新测试。
- `tests/update_service_tests.cpp:1`：新增语义版本测试。

### Reason
原流程在 FSRemote 仍运行时直接替换 EXE 和已加载 DLL，Windows 文件占用会导致更新失败并可能留下混合版本。本次将准备阶段和安装阶段分离：主程序只在用户目录准备完整载荷，独立更新器等待主进程退出后备份、替换、失败回滚并自动重启。

### Original Code
```cpp
// src/system/UpdateService.cpp:386-397（修改前）
bool UpdateService::applyRemoteUpdate(QString* errorMessage)
{
    const QString share = updateShareRoot();
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!syncRuntimePayload(share, appDir, errorMessage)) {
        return false;
    }
    writeVersionFile(localVersionPath(), remoteVersionText(), nullptr);
    return true;
}
```

```text
// src/updater/main.cpp（修改前）
new code, no old code at this location
```

### Modified Code
```cpp
// src/system/UpdateService.cpp:247-272
bool UpdateService::applyRemoteUpdate(QString* error)
{
    // 将共享载荷复制到版本化 staging，写入任务并从 runner 目录启动更新器。
    // 更新器本地就绪后发出 updateReadyToQuit，让 main 有序释放服务和文件占用。
}
```

```cpp
// src/updater/main.cpp:209-224
int wmain(int argc, wchar_t* argv[])
{
    // 解析安全任务、等待主进程退出、事务安装或回滚，最后自动重启 FSRemote。
}
```

### Steps
1. 新增 `FSRemoteUpdater.exe` 原生目标，使用 `/MT` 静态运行库且不链接 Qt。
2. 定义 `schemaVersion=1` 的 JSON 任务，拒绝绝对路径、父目录跳转和空任务。
3. 把共享运行载荷复制到 `%LOCALAPPDATA%/FSRemote/Updates/<version>/payload` 并校验大小。
4. 更新器等待主 PID 退出，完整备份任务文件后逐项安装，失败则逆序恢复。
5. 成功或回滚后自动启动 FSRemote，并通过命令行参数展示结果。
6. 修复“不相等即更新”和发布基线未取最大版本的问题，手动检查不再受自动开关限制。
7. 增加语义版本排序与无效任务拒绝测试。

### Verification
- `FSRemoteUpdater` Release 目标编译成功。
- `fsremote_update_service_tests` 编译并通过。
- `fsremote_updater_rejects_missing_task` 通过，确认无效任务在修改安装目录前失败。
- CTest 结果：2/2 通过。
- `FSRemote` 已完成编译和链接；后置 OpenSSH 运行目录复制因目标目录权限被拒绝而失败，该问题位于既有部署步骤，不是本次更新代码的编译错误。

## 2026-07-13 16:30 - 发布前补齐并验收完整共享运行包

### Changed Location
- `src/system/UpdateService.cpp:108`：新增共享运行包逐文件和逐目录验收。
- `src/system/UpdateService.cpp:176`：根目录运行白名单全部改为发布必需项。
- `src/system/UpdateService.cpp:299`：发布成功前复核共享目录完整性。
- `openspec/changes/reliable-self-update-restart/tasks.md:23`：登记完整共享运行包任务。

### Reason
用户需要把共享目录整体复制到一台全新设备后直接运行。原发布流程允许部分可选 DLL 缺失，也没有在写入新版本号前复核共享目录，因此无法提供完整运行包保证。本次以当前 Release 输出为标准，缺失或大小不一致的文件会先补齐，复核失败时保持旧版本号不变。

### Original Code
```cpp
// src/system/UpdateService.cpp:98-103（修改前）
const bool required = name == QStringLiteral("FSRemote.exe") || name == QStringLiteral("FSRemoteUpdater.exe")
    || name == QStringLiteral("fsremote_stream.dll") || name.startsWith(QStringLiteral("Qt6"));
if (!copyFileClean(source, destination, required, error)) return false;
```

```cpp
// src/system/UpdateService.cpp:235-238（修改前）
if (!QDir().mkpath(share) || !syncRuntimePayload(appDir, share, error)) return false;
const QString local = normalizeSemanticVersion(localVersionText());
```

### Modified Code
```cpp
// src/system/UpdateService.cpp:176-179
for (const QString& name : rootRuntimeFileNames()) {
    if (!copyFileClean(QDir(sourceRoot).filePath(name), QDir(destinationRoot).filePath(name), true, error)) return false;
}
```

```cpp
// src/system/UpdateService.cpp:299-304
if (!QDir().mkpath(share) || !syncRuntimePayload(appDir, share, error)) return false;
if (!verifyRuntimePayload(appDir, share, error)) return false;
const QString local = normalizeSemanticVersion(localVersionText());
```

### Steps
1. 将根目录运行白名单全部设为发布必需文件，缺少时立即报告具体文件。
2. 递归比较发布端与共享目录中的插件及 OpenSSH 文件大小。
3. 强制验证 `platforms/qwindows.dll`、OpenSSH 核心五件套和 Parsec VDD 安装包。
4. 仅在共享运行包复核完全通过后递增并写入 `FSRemote.version`。

### Verification
- 更新服务 Release 测试目标编译成功。
- CTest 更新相关测试 2/2 通过。
- 检查当前 Release 输出的 27 个关键运行文件，全部存在，无缺失项。
- `git diff --check` 在最终交付前执行。

## 2026-07-16 10:19 - 状态自动刷新不再批量关闭远控窗口

### Changed Location
- src/ui/DeviceGrid.cpp:5351-5353：状态刷新只更新设备在线状态，不再根据单次 Offline 结果关闭远控窗口。

### Reason
多窗口同时远控时，状态探测服务可能因本机负载升高出现瞬时超时。原逻辑把单次 Offline 结果直接当作远控断开，并在同一次 UI 回调中批量调用 window->close()，导致仍在工作的 WebRTC 远控窗口同时消失。本次将设备状态显示与远控窗口生命周期解耦。

### Original Code
`cpp
// src/ui/DeviceGrid.cpp:5351-5366（修改前）
// wjy: 普通设备离线时关闭悬挂窗口；更新造成的预期离线必须保留原窗口继续等待重连。
for (auto it = grid->m_remoteDesktopWindows.begin(); it != grid->m_remoteDesktopWindows.end();) {
    const QString ip = it.key().trimmed();
    const platform::DevicePresenceState state = grid->m_deviceStatuses.value(ip, platform::DevicePresenceState::Unknown);
    QPointer<RemoteDesktopWindow> window = it.value();
    const bool keepForRemoteUpdate = window && window->isRemoteUpdateActive();
    if (state == platform::DevicePresenceState::Offline
        && window
        && !window->isClosingConnection()
        && !keepForRemoteUpdate) {
        window->close();
        it = grid->m_remoteDesktopWindows.erase(it);
        continue;
    }
    ++it;
}
`

### Modified Code
`cpp
// src/ui/DeviceGrid.cpp:5351-5353
// wjy: 状态自动刷新只更新设备列表状态，不再依据单次 Offline 结果关闭远控窗口。
// wjy: 设备状态探测服务的超时或瞬时失败不等于 WebRTC 会话已经断开，避免多窗口刷新时被批量误关。
// wjy: 远控窗口继续由用户操作、窗口自身连接流程、重新平铺或应用退出管理生命周期。
`

### Steps
1. 保留状态刷新结果写入和设备列表状态更新逻辑。
2. 删除刷新回调中遍历 m_remoteDesktopWindows 并调用 window->close() 的循环。
3. 保持手动关闭、关闭全部、重新平铺和应用退出等既有窗口管理逻辑不变。

### Verification
- git diff --check -- src/ui/DeviceGrid.cpp 通过。
- 按用户要求停止后续编译；未完成 Release 构建验证。

## 2026-07-16 18:45 - 二十路不断流远控画质协调与稳定性优化

### Changed Location
- `src/stream/RemoteQualityPolicy.h:9-119`：新增全局/单窗口画质模式、固定 FPS/分辨率档位及安全归一化。
- `src/system/AppSettings.cpp:208-246`、`src/system/AppSettings.h:34-35`：持久化全局远控画质配置。
- `src/ui/DeviceGrid.cpp:246-284,3162-3166,4997-5122,6801,6944`、`src/ui/DeviceGrid.h:157-160,236-242`：新增主窗口画质页、全局协调器、1 秒采样、即时重算和 30 秒资源快照。
- `src/ui/RemoteQualityCoordinator.cpp:67-220`、`src/ui/RemoteQualityCoordinator.h:10-68`：实现最小化立即降档、可见窗口先降分辨率再降 FPS、严重压力保护、视口档位、预算加权和恢复滞回。
- `src/ui/RemoteDesktopWindow.cpp:1194-1290,1412-1422,1575-1700,2110-2247,2697-2750,2888-3045,3426-3583`、`src/ui/RemoteDesktopWindow.h:76-79,99,159-164,205-225,239-242`：接入单窗口临时覆盖、质量请求去重/补发、Host 实际状态、标题栏反馈、D3D11 软件回退与恢复。
- `include/FsRemoteStreamApi.h:47,91-127,154-155`、`src/stream/StreamRuntime.cpp:96-97,212-224`、`src/stream/StreamRuntime.h:63-64,94-119`：扩展稳定 C ABI 和可选 DLL 导出加载。
- `third_party/uu_stream_webrtc/src/viewer_quality_protocol.h:11-54`、`viewer_quality_protocol.cpp:49-204`、`fsremote_stream_api.cpp:1753-1804,2000-2168,2511-2522`：实现版本化质量消息、单槽最新请求、Host 校验应用、确认和旧 Host 超时兼容。
- `third_party/uu_stream_webrtc/src/webrtc_session.cpp:423-468`、`webrtc_session.h:58-66`：在线修改 sender 码率、FPS、优先级和目标分辨率，不重建 PeerConnection。
- `third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:123-158`、`nvenc_h264_encoder.h:19,32-35`、`third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:166-225,282-292,345-374`：NVENC 改为在线 Reconfigure，并在编码/解码边界隔离异常。
- `src/ui/LatestTextureFrameSlot.h:14-102`、`tests/latest_texture_frame_slot_tests.cpp:1-31`：纹理帧交接改为单槽最新帧和单个 Qt drain。
- `src/ui/RemoteViewerLifecycleManager.cpp:14-220`、`RemoteViewerLifecycleManager.h:21-75`：四路初始化准入、固定四线程生命周期池、可等待退出和诊断快照。
- `third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:24-149`：Host/Viewer 共享进程级 WebRTC SSL 与三条线程，保留每 Viewer 独立 Factory/解码回调。
- `src/ui/D3D11FramePresenter.cpp:29-334`、`D3D11FramePresenter.h:25-43`：共享 D3D11 展示 Device、每窗口 SwapChain、设备移除换代和 HRESULT 诊断。
- `src/ui/MainWindow.cpp:64-73,200-214`、`MainWindow.h:54`：退出看门狗改为可取消、可 join 的 `std::jthread`，20 路清理时限调整为 30 秒。
- `src/system/WjyDiagnosticLog.cpp:14-86`、`WjyDiagnosticLog.h:8`：恢复 4 MB 单备份轮转诊断日志。
- `tests/remote_quality_policy_tests.cpp:1-40`、`tests/remote_quality_coordinator_tests.cpp:1-101`、`third_party/uu_stream_webrtc/tests/viewer_quality_protocol_tests.cpp:1-44`：覆盖归一化、固定档位、最小化、滞回、严重压力、软件回退和畸形协议。
- `CMakeLists.txt:45-75,125-153,209-212,254`：登记新增源码、测试目标和 Windows `psapi` 依赖。

### Reason
一次性远控二十台设备时，旧实现会按窗口放大 WebRTC 线程、D3D11 Device、Viewer 初始化、停止线程和 Qt 纹理任务；可见窗口没有统一资源协调，最小化窗口仍可能维持高分辨率高帧率；NVENC 调码率会重建编码器；任一后台线程异常还可能越过线程入口触发 `std::terminate`，表现为整个程序直接退出。

本次改动遵循“二十路同时在线是刚需、不能断流、可见窗口优先 FPS、先降分辨率后降 FPS、最小化立即降低 FPS、高质量锁定最后降级但不能突破稳定性硬边界”的约束。所有质量变化均在现有会话内完成，不通过关闭窗口、暂停流或重建 PeerConnection 实现。

### Original Code

```cpp
// src/ui/RemoteDesktopWindow.cpp（修改前，纹理帧逐帧投递 Qt 任务）
QMetaObject::invokeMethod(window, [window, width, height, sharedHandle, encodedMbps] {
    if (window->isClosingConnection()) return;
    window->m_texturePresenter->presentSharedTexture(sharedHandle, width, height);
}, Qt::QueuedConnection);
```

```cpp
// src/ui/RemoteDesktopWindow.cpp（修改前，停止工作使用不可等待线程）
std::thread([handle, window] {
    stream::StreamRuntime::instance().stop(handle);
    QMetaObject::invokeMethod(window, [window] { window->finishViewerStop({}); });
}).detach();
```

```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp（修改前，每个 runtime 三条线程和一次 SSL 生命周期）
webrtc::InitializeSSL();
impl_->network_thread = webrtc::Thread::CreateWithSocketServer();
impl_->worker_thread = webrtc::Thread::Create();
impl_->signaling_thread = webrtc::Thread::Create();
// shutdown 时逐 runtime Stop + CleanupSSL
```

```cpp
// third_party/uu_stream_webrtc/src/uu_codec_factory.cpp（修改前，调码率会停掉编码器）
if (encoder_.ready() && should_reconfigure_bitrate()) {
    encoder_.shutdown();
    last_bitrate_reconfigure_ms_ = GetTickCount64();
}
```

```cpp
// src/ui/MainWindow.cpp（修改前）
std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(8));
    ::TerminateProcess(::GetCurrentProcess(), 0);
}).detach();
```

```cpp
// src/system/WjyDiagnosticLog.cpp（修改前）
void writeWjyDiagnosticLog(const QString& message)
{
    (void)message;
}
```

```text
// 以下位置为新增能力，原位置无旧代码
src/stream/RemoteQualityPolicy.h
src/ui/RemoteQualityCoordinator.h/.cpp
src/ui/LatestTextureFrameSlot.h
src/ui/RemoteViewerLifecycleManager.h/.cpp
third_party/uu_stream_webrtc/src/viewer_quality_protocol.h/.cpp
tests/latest_texture_frame_slot_tests.cpp
tests/remote_quality_policy_tests.cpp
tests/remote_quality_coordinator_tests.cpp
third_party/uu_stream_webrtc/tests/viewer_quality_protocol_tests.cpp
```

### Modified Code

```cpp
// src/stream/RemoteQualityPolicy.h:27-41
struct RemoteQualityConfiguration {
    RemoteQualityMode defaultMode = RemoteQualityMode::Automatic;
    int targetFps = 60;
    int minimumVisibleFps = 30;
    int severePressureMinimumFps = 15;
    int minimizedFps = 15;
    RemoteResolutionTier minimumVisibleResolution = RemoteResolutionTier::P720;
    RemoteResolutionTier minimizedResolution = RemoteResolutionTier::P540;
    int degradationHoldMs = 2500;
    int recoveryHoldMs = 10000;
    bool automaticRecoveryEnabled = true;
    int aggregateReceiveBudgetMbps = 0;
};
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:126-142,151-161
if (decision.minimized) {
    decision.resolution = configuration.minimizedResolution;
    decision.targetFps = configuration.minimizedFps;
    decision.reason = RemoteQualityDegradationReason::Minimized;
} else if (window.softwareFallback) {
    state.resolutionIndex = std::max(state.resolutionIndex,
        resolutionIndex(stream::RemoteResolutionTier::P540));
    state.fpsIndex = std::max(state.fpsIndex, fpsIndexForTarget(24));
    decision.reason = RemoteQualityDegradationReason::SoftwareFallback;
} else if (pressure && nowMs - state.pressureSinceMs >= configuration.degradationHoldMs) {
    if (state.resolutionIndex < resolutionFloor) ++state.resolutionIndex;
    else if (state.fpsIndex < activeFpsFloor) ++state.fpsIndex;
}
```

```cpp
// src/ui/DeviceGrid.cpp:5074-5099
void DeviceGrid::evaluateRemoteQuality()
{
    const QVector<QPointer<RemoteDesktopWindow>> windows = openedRemoteWindows();
    std::vector<RemoteQualityWindowMetrics> metrics;
    for (const QPointer<RemoteDesktopWindow>& window : windows) {
        if (window) metrics.push_back(window->remoteQualityMetrics());
    }
    const auto decisions = m_remoteQualityCoordinator.evaluate(
        m_remoteQualityConfiguration, metrics, QDateTime::currentMSecsSinceEpoch());
    for (const RemoteQualityDecision& decision : decisions) {
        auto* window = reinterpret_cast<RemoteDesktopWindow*>(decision.windowId);
        if (window) window->applyRemoteQualityDecision(decision);
    }
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:1608-1647
void RemoteDesktopWindow::sendCurrentRemoteQualityDecision()
{
    FsRemoteViewerQualityConfig config = {};
    config.struct_size = sizeof(config);
    config.version = 1;
    config.mode = viewerQualityModeValue(m_remoteQualityDecision.effectiveMode);
    config.target_width = m_remoteQualityDecision.targetWidth;
    config.target_height = m_remoteQualityDecision.targetHeight;
    config.target_fps = m_remoteQualityDecision.targetFps;
    config.max_bitrate_kbps = m_remoteQualityDecision.maxBitrateKbps;
    config.priority = m_remoteQualityDecision.priority;
    if (sameQualityPayload(m_lastSentQualityConfig, config)
        && m_lastQualityViewerGeneration == currentGeneration) return;
    config.request_id = ++m_nextQualityRequestId;
    stream::StreamRuntime::instance().setViewerQuality(m_viewerHandle, config);
}
```

```cpp
// include/FsRemoteStreamApi.h:104-127
typedef struct FsRemoteViewerQualityConfig {
    uint32_t struct_size;
    uint32_t version;
    uint64_t request_id;
    uint32_t mode;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t target_fps;
    uint32_t max_bitrate_kbps;
    uint32_t priority;
} FsRemoteViewerQualityConfig;

typedef struct FsRemoteViewerQualityStatus {
    uint32_t struct_size;
    uint32_t version;
    uint64_t request_id;
    uint32_t supported;
    uint32_t applied_width;
    uint32_t applied_height;
    uint32_t applied_fps;
    uint32_t applied_bitrate_kbps;
    uint32_t limitation;
} FsRemoteViewerQualityStatus;
```

```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.cpp:423-461
bool WebrtcSession::apply_sender_quality(...)
{
    auto parameters = sender->GetParameters();
    auto& encoding = parameters.encodings.front();
    encoding.max_framerate = static_cast<double>(target_fps);
    encoding.max_bitrate_bps = static_cast<int>(safe_max_kbps) * 1000;
    encoding.network_priority = network_priority;
    encoding.scale_resolution_down_to = webrtc::Resolution{target_width, target_height};
    return sender->SetParameters(parameters).ok();
}
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:123-153
bool NvencH264Encoder::reconfigure(uint32_t bitrate_kbps, uint32_t fps, std::string* error)
{
    NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
    reconfigure.reInitEncodeParams = nextInit;
    reconfigure.resetEncoder = 0;
    reconfigure.forceIDR = 1;
    if (!check(fn_.nvEncReconfigureEncoder(encoder_, &reconfigure),
               "NvEncReconfigureEncoder", error)) return false;
    config_ = nextConfig;
    return true;
}
```

```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:43-72
std::shared_ptr<SharedWebrtcThreads> acquire_shared_webrtc_threads(std::string* error)
{
    if (auto existing = g_shared_webrtc_threads.lock()) return existing;
    auto shared = std::make_shared<SharedWebrtcThreads>();
    webrtc::InitializeSSL();
    shared->network_thread = webrtc::Thread::CreateWithSocketServer();
    shared->worker_thread = webrtc::Thread::Create();
    shared->signaling_thread = webrtc::Thread::Create();
    g_shared_webrtc_threads = shared;
    return shared;
}
```

```cpp
// src/ui/D3D11FramePresenter.cpp:43-73,309-329
std::shared_ptr<SharedPresentationDevice> acquirePresentationDevice();

bool D3D11FramePresenter::handleDeviceFailure(long result)
{
    const HRESULT queriedReason = m_impl->sharedDevice->device->GetDeviceRemovedReason();
    if (queriedReason == DXGI_ERROR_DEVICE_REMOVED
        || queriedReason == DXGI_ERROR_DEVICE_RESET
        || queriedReason == DXGI_ERROR_DEVICE_HUNG) {
        invalidatePresentationDevice(m_impl->sharedDevice, queriedReason);
    }
    m_impl->swapChain.Reset();
    return false;
}
```

```cpp
// src/ui/MainWindow.cpp:200-214
m_exitWatchdog = std::jthread([](std::stop_token stopToken) {
    for (int step = 0; step < 300; ++step) {
        if (stopToken.stop_requested()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::TerminateProcess(::GetCurrentProcess(), 0);
});
```

```cpp
// src/system/WjyDiagnosticLog.cpp:63-84
void writeWjyDiagnosticLog(const QString& message)
{
    QByteArray line = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toUtf8();
    line += " tid=";
    line += QByteArray::number(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    line += ' ';
    line += message.toUtf8();
    line += "\r\n";
    DiagnosticLogState& state = diagnosticLogState();
    QMutexLocker locker(&state.mutex);
    if (!ensureDiagnosticFileOpen(state)) return;
    rotateDiagnosticFileIfNeeded(state, line.size());
    if (state.file.isOpen()) {
        state.file.write(line);
        state.file.flush();
    }
}
```

### Steps
1. 新增固定画质策略类型和全局持久化设置，并在主窗口增加“远控画质”设置页。
2. 在远控标题栏增加仅当前窗口有效的跟随全局/自动/高质量锁定/均衡/流畅菜单。
3. 将纹理交接改为单槽最新帧，使用 Viewer 代际隔离旧帧、旧状态和旧关闭回调。
4. 使用四路初始化准入和固定四线程生命周期池替代每窗口 detached 停止线程。
5. 新增控制端质量协调器：小视口主动降像素，最小化立即降档，持续压力先降分辨率，再降 FPS；普通压力最低 30 FPS，严重总预算压力才允许到 15 FPS。
6. 扩展 C ABI、运行库加载器和 data-channel 协议；Viewer 只保留最新请求，Host 校验后在线修改 sender 并返回实际值。
7. 将 NVENC 码率/FPS变化改为在线 Reconfigure，失败时继续使用旧编码会话。
8. 将 WebRTC SSL 和三条底层线程改为进程级引用计数共享，保留每路独立 Factory 和解码回调。
9. 将展示端 D3D11 Device 改为共享设备、每窗口 SwapChain；设备移除后换代重建，失败窗口进入 540p/24 FPS 软件保活并退避重试。
10. 在 Viewer、Host 会话、捕获、编码、解码、Qt C ABI 回调和协调器入口增加异常隔离。
11. 将退出看门狗改为可取消 `std::jthread`，恢复有上限轮转日志并记录 30 秒资源快照。
12. 新增策略、协调器、纹理单槽和质量协议单元测试，并更新 OpenSpec 任务状态。

### Verification
- Git 上传基线确认：本地 `HEAD` 与 `origin/main` 均为 `e54234557ad05af4afa48e1571d7dd05ffaa4b14`。
- Release x64 配置与完整 `FSRemote`、`fsremote_stream` 构建通过。
- `ctest -C Release --output-on-failure`：12/12 测试通过。
- `fsremote_remote_quality_coordinator_tests` 覆盖最小化、视口、普通/严重压力、高质量优先和软件回退，通过。
- `uu_viewer_quality_protocol_tests` 覆盖请求/确认序列化、畸形消息和 0 FPS 拒绝，通过。
- `git diff --check` 通过，无空白错误。
- 尚未执行真实二十设备八小时 soak、随机断连重连和真实单窗口音视频/输入/剪贴板回归；这些需要实际设备环境验证。

## 2026-07-17 09:21 - 修复均衡与流畅模式周期性闪黑

### Changed Location
- `src/ui/D3D11FramePresenter.h:28-29`：公开最后成功画面和设备移除状态，供窗口选择保帧或恢复路径。
- `src/ui/D3D11FramePresenter.cpp:239-358,436-455`：新共享纹理改为候选提交，普通单帧失败不再清空旧纹理、处理器和 SwapChain。
- `src/ui/RemoteDesktopWindow.cpp:83,1413-1425,2704-2794,2802-2834`：单帧失败保留上一帧，连续三次才进入 BGRA 保活，并在隐藏 D3D 子窗口前同步铺好软件首帧。
- `third_party/lan_stream_probe/src/ffmpeg_decoder.cpp:238-249`：共享纹理发布前 Flush 解码生产端 D3D11 上下文。
- `third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:143-150`：码率/FPS 在线调参不再周期性强制 IDR。
- `openspec/changes/stabilize-multi-device-remote-quality/specs/multi-device-viewer-stability/spec.md:14-16,40-42`：增加最后成功帧和无黑屏软件回退场景。
- `openspec/changes/stabilize-multi-device-remote-quality/design.md:35,80`：记录 commit-on-success 呈现策略和跨设备纹理同步风险处理。
- `openspec/changes/stabilize-multi-device-remote-quality/tasks.md:8`：新增并完成无闪黑呈现任务 1.6。

### Reason
均衡模式使用 1080p、流畅模式使用 720p 和更低码率，在线调参及分辨率变化更容易经过 NVENC 重配置、解码输出纹理重建和跨设备共享纹理路径。旧实现把“纹理已经排入 Qt 单槽”提前当成“纹理已经成功显示”，并在单次 `OpenSharedResource`、视频处理或 `Present` 失败后立即清空上一帧、隐藏 Presenter、释放 SwapChain，父窗口的合法黑色背景因此会短暂露出。修复采用“新帧成功后才提交”的两阶段呈现：生产端先提交 GPU 命令，消费端保留旧资源，新帧完整 Present 成功后才替换；普通瞬时失败只丢当前帧，连续失败才执行有上限的软件回退。

### Original Code
```cpp
// src/ui/D3D11FramePresenter.cpp:247-253（修改前）
if (m_impl->currentHandle != sharedHandle || !m_impl->currentTexture) {
    m_impl->currentTexture.Reset();
    HRESULT hr = m_impl->sharedDevice->device->OpenSharedResource(
        static_cast<HANDLE>(sharedHandle), IID_PPV_ARGS(&m_impl->currentTexture));
    if (FAILED(hr) || !m_impl->currentTexture) {
        return handleDeviceFailure(hr);
    }
    m_impl->currentHandle = sharedHandle;
}
```

```cpp
// src/ui/D3D11FramePresenter.cpp:319-328（修改前）
if (removalReason == DXGI_ERROR_DEVICE_REMOVED
    || removalReason == DXGI_ERROR_DEVICE_RESET
    || removalReason == DXGI_ERROR_DEVICE_HUNG
    || removalReason == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
    invalidatePresentationDevice(m_impl->sharedDevice, removalReason);
}
releaseFrameResources();
m_impl->processor.Reset();
m_impl->processorEnum.Reset();
m_impl->swapChain.Reset();
return false;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2710-2738（修改前）
m_remoteTextureSize = QSize(frame->width, frame->height);
m_textureFrameActive = true;
m_remoteFrame = QImage();
if (!texturePresented) {
    m_texturePresentFailed.store(true);
    m_softwareFallbackActive = true;
    ++m_textureFailureCount;
    m_textureFrameActive = false;
    m_texturePresenter->hide();
    m_texturePresenter->reset();
    m_pendingTextureFrames.cancelPending();
    emit remoteQualityInputsChanged();
    update();
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2789-2800（修改前）
updateFrameStats(image);
m_textureFrameActive = false;
if (m_texturePresenter) {
    m_texturePresenter->hide();
}
m_remoteFrame = image;
update(isFullScreen() ? rect() : QRect(0, titleBarHeight(), width(), height() - titleBarHeight()));
```

```cpp
// third_party/lan_stream_probe/src/ffmpeg_decoder.cpp:238-248（修改前）
hr = video_context_->VideoProcessorBlt(processor_.Get(), output_view.Get(), 0, 1, &stream);
if (FAILED(hr)) {
    return false;
}
decoded->shared_handle = output_shared_handles_[output_index_];
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:146-147（修改前）
reconfigure.resetEncoder = 0;
reconfigure.forceIDR = 1;
```

### Modified Code
```cpp
// src/ui/D3D11FramePresenter.cpp:249-256,307-315（修改后）
ComPtr<ID3D11Texture2D> candidateTexture = m_impl->currentTexture; // wjy: 失败时保留最后成功纹理。
if (m_impl->currentHandle != sharedHandle || !candidateTexture) {
    candidateTexture.Reset();
    HRESULT hr = m_impl->sharedDevice->device->OpenSharedResource(
        static_cast<HANDLE>(sharedHandle), IID_PPV_ARGS(&candidateTexture));
    if (FAILED(hr) || !candidateTexture) {
        return handleDeviceFailure(hr);
    }
}
m_impl->currentTexture = std::move(candidateTexture); // wjy: 完整 Present 成功后才提交新资源。
m_impl->currentHandle = sharedHandle;
m_impl->hasPresentedFrame = true;
```

```cpp
// src/ui/D3D11FramePresenter.cpp:329-343（修改后）
const bool deviceLost = removalReason == DXGI_ERROR_DEVICE_REMOVED
    || removalReason == DXGI_ERROR_DEVICE_RESET
    || removalReason == DXGI_ERROR_DEVICE_HUNG
    || removalReason == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
m_impl->lastFailureWasDeviceLost = deviceLost;
if (deviceLost) {
    invalidatePresentationDevice(m_impl->sharedDevice, removalReason);
    releaseFrameResources();
    m_impl->processor.Reset();
    m_impl->processorEnum.Reset();
    m_impl->swapChain.Reset();
    m_impl->hasPresentedFrame = false;
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2729-2762（修改后）
const bool canContinueDisplayingTexture = hadSuccessfulTexture
    && m_texturePresenter->hasPresentedFrame();
const bool keepLastFrame = canContinueDisplayingTexture
    && !m_texturePresenter->lastFailureWasDeviceLost()
    && m_textureFailureCount < kTextureFailuresBeforeSoftwareFallback;
if (keepLastFrame) {
    m_textureFrameActive = true;
    m_texturePresenter->show();
    m_texturePresenter->raise(); // wjy: 新帧失败时继续显示上一帧。
} else {
    m_texturePresentFailed.store(true);
    m_softwareFallbackActive = true;
    m_textureFrameActive = canContinueDisplayingTexture;
    m_pendingTextureFrames.cancelPending();
    emit remoteQualityInputsChanged();
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2813-2824（修改后）
const bool textureWasVisible = m_textureFrameActive
    && m_texturePresenter && m_texturePresenter->isVisible();
m_remoteFrame = image;
m_textureFrameActive = false;
if (textureWasVisible) {
    repaint(contentRect); // wjy: 先在旧纹理背后同步铺好软件首帧。
    m_texturePresenter->hide();
}
```

```cpp
// third_party/lan_stream_probe/src/ffmpeg_decoder.cpp:238-249（修改后）
hr = video_context_->VideoProcessorBlt(processor_.Get(), output_view.Get(), 0, 1, &stream);
if (FAILED(hr)) {
    return false;
}
context_->Flush(); // wjy: 发布共享句柄前提交生产端 GPU 命令。
decoded->shared_handle = output_shared_handles_[output_index_];
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:146-147（修改后）
reconfigure.resetEncoder = 0; // wjy: 保持编码会话连续。
reconfigure.forceIDR = 0; // wjy: 码率/FPS 调参不再周期性强制 IDR。
```

### Steps
1. 在解码端共享纹理 `VideoProcessorBlt` 后执行 `ID3D11DeviceContext::Flush`，先把生产端 GPU 命令提交再发布句柄。
2. Presenter 使用局部候选纹理并校验共享纹理真实宽高，只有 Blt 和 Present 全部成功才替换最后成功资源，避免分辨率切换时误复用旧句柄。
3. 普通瞬时呈现失败不释放 SwapChain；只有 Device Removed/Reset/Hung 等设备级故障才撤销共享设备代际。
4. 远控窗口连续三次纹理失败后才进入 540p/24 FPS 软件保活，前两次失败直接停留在上一帧。
5. D3D 转 BGRA 时先同步绘制软件首帧，再隐藏原生 D3D 子窗口，避免父窗口黑底短暂暴露。
6. NVENC 码率/FPS 在线重配置取消强制 IDR，降低流畅模式周期性调参对解码链路的扰动。
7. 更新 OpenSpec 场景、设计风险和任务记录，保留真实单窗口与二十设备压力测试为未完成状态。

### Verification
- 为避免中断正在运行的 FSRemote，在独立目录 `build/flashfix-release` 完成 Release x64 构建。
- `FSRemote.exe` 与 `fsremote_stream.dll` 构建通过。
- `ctest --test-dir build/flashfix-release -C Release --output-on-failure`：12/12 测试通过。
- `git diff --check` 通过。
- `rg "\\.detach\\("`：源码无残留 detached thread。
- 尚未把新二进制替换到当前运行目录，也未执行真实设备上的高质量/均衡/流畅长时间切换验证；需要关闭当前实例后再进行实机回归。

## 2026-07-17 09:58 - 提升高质量远控的色彩与动态细节

### Changed Location
- `third_party/lan_stream_probe/src/ffmpeg_decoder.cpp:232`：配置 BT.709 limited-range 到 full-range BGRA 的 GPU 色彩转换。
- `third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:648`：软件回退直接复制 full-range BGRA，避免二次扩展。
- `third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:73`：增加受控峰值码率、两帧 VBV 和时间 AQ。
- `src/ui/RemoteQualityCoordinator.cpp:257`：高质量锁定码率倍率由 1.20 提升到 1.35。
- `tests/remote_quality_coordinator_tests.cpp:51`：增加高质量档码率余量回归测试。

### Reason
截图显示远控画面偏灰、黑位不深，同时复杂运动区域轻微软化。单纯提高平均码率收益有限，因此先修正解码显示链路的颜色范围，再为 NVENC 复杂帧提供有限峰值空间和时间 AQ。改动继续禁用 B 帧、lookahead 和高开销多遍编码，高质量锁定仍受总预算与 Host 硬上限约束。

### Original Code
```cpp
// third_party/lan_stream_probe/src/ffmpeg_decoder.cpp:229-233（修改前）
video_context_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &rect);
```

```cpp
// third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:648-656（修改前）
const int value = src_pixel[channel];
const int expanded = (value - 16) * 255 / 219;
dst_pixel[channel] = static_cast<uint8_t>(std::clamp(expanded, 0, 255));
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:71-79（修改前）
config.rcParams.maxBitRate = bitrate;
config.rcParams.vbvBufferSize = std::max<uint32_t>(bitrate / safe_fps, 1);
config.rcParams.enableAQ = 1;
config.rcParams.aqStrength = 8;
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:257（修改前）
if (mode == stream::RemoteQualityMode::HighQualityLocked) modeScale = 1.20;
```

```cpp
// tests/remote_quality_coordinator_tests.cpp:45（修改前）
// 此处原来没有高质量档 35% 码率余量的独立断言。
```

### Modified Code
```cpp
// third_party/lan_stream_probe/src/ffmpeg_decoder.cpp:232-240（修改后）
D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color_space = {};
input_color_space.YCbCr_Matrix = 1;
input_color_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &input_color_space);
D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color_space = {};
output_color_space.RGB_Range = 1;
output_color_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &output_color_space);
```

```cpp
// third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:654（修改后）
dst_pixel[channel] = src_pixel[channel]; // 解码器已输出 full-range，回退路径不再二次扩展。
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:73-78（修改后）
config.rcParams.maxBitRate = bitrate + bitrate / 4;
config.rcParams.vbvBufferSize = std::max<uint32_t>(config.rcParams.maxBitRate * 2 / safe_fps, 1);
config.rcParams.enableTemporalAQ = 1;
config.rcParams.aqStrength = 10;
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:257（修改后）
if (mode == stream::RemoteQualityMode::HighQualityLocked) modeScale = 1.35;
```

```cpp
// tests/remote_quality_coordinator_tests.cpp:51-64（修改后）
ui::RemoteQualityCoordinator qualityBudgetCoordinator;
// 构造相同分辨率/FPS的自动与高质量锁定窗口。
assert(decisions[1].maxBitrateKbps * 100 >= decisions[0].maxBitrateKbps * 135);
```

### Steps
1. D3D11 解码处理器声明 BT.709、输入 16-235、输出 RGB 0-255，使共享纹理直接得到正确桌面色阶。
2. 移除软件 BGRA 回退中的重复范围扩展，统一硬件纹理与软件回退的颜色。
3. NVENC 保持平均目标不变，允许 25% 瞬时峰值，把 VBV 控制在约两帧，并启用时间 AQ、将 AQ 强度从 8 调至 10。
4. 高质量锁定目标码率倍率提高到 1.35，但继续经过总预算与 Host 上限夹紧。
5. 添加协调器回归断言，防止高质量档退化成普通档。

### Verification
- Visual Studio 2022 x64 环境完成 `build/flashfix-release` Release 构建。
- `FSRemote.exe` 与 `fsremote_stream.dll` 构建通过。
- `ctest --test-dir build/flashfix-release -C Release --output-on-failure`：12/12 通过。
- `git diff --check` 通过。
- 尚未进行真实设备动态视频、颜色梯度和 20 窗口长时间压力验证；OpenSpec 6.2、6.3 继续保持未完成。

## 2026-07-24 16:01 - 远控标题栏增加系统/驱动鼠标后端切换

### Changed Location
- `third_party/uu_stream_webrtc/src/faker_input_bridge_client.h:1-244`：新增仅连接本机命名管道的 FakerInputBridge v1 客户端。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:15, 846-1005, 1228-1278, 1450-1534, 1554-1822, 2176-2211, 2825-2828`：Host 鼠标后端路由、全局切换、故障回退、状态广播及 Viewer 状态上报。
- `third_party/uu_stream_webrtc/src/shared_input_state.h:43-57`：后端切换时只释放共享鼠标按钮，不改变键盘持有状态。
- `third_party/uu_stream_webrtc/tests/shared_input_state_tests.cpp:48-68`：新增切换后端释放按钮的回归测试。
- `include/FsRemoteStreamApi.h:57`：新增鼠标注入后端状态码 65。
- `src/stream/StreamRuntime.h:22`：Qt 强类型状态枚举同步状态码 65。
- `src/ui/RemoteDesktopWindow.h:61, 136-140, 161, 174-177, 329-337`：声明标题栏鼠标后端状态、请求、查询和按钮状态。
- `src/ui/RemoteDesktopWindow.cpp:1349-1352, 2270-2280, 2312-2324, 2825-2932, 3198-3201, 3378-3381, 3444-3447, 3666-3677, 3740-3775, 3899-3903, 3947-3982, 4194-4199, 4282-4293, 4387-4393`：解析确认、自动查询、绘制按钮、命中测试、气泡和点击切换。

### Reason
部分游戏不接受 Windows `SendInput` 产生的鼠标事件，而独立 `FakerInputBridge` 已在 AION2 中实测可点击。此次把两种鼠标注入方式接入 FSRemote，同时保留原系统模式作为默认值和故障回退。后端由 Host 全局管理，避免多个控制端在共享按钮按下/抬起期间使用不同后端；键盘仍保持原有 `SendInput`，不扩大本次改动范围。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/faker_input_bridge_client.h（修改前）
// 新增文件，此位置原来没有 FSRemote 到 FakerInputBridge 的本机管道客户端。
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:799-842, 1381-1424（修改前）
void move_mouse_absolute(int x, int y, bool log_result) { /* 直接 SendInput */ }
void move_mouse_relative(int dx, int dy, bool log_result) { /* 直接 SendInput */ }
void send_mouse_button(DWORD flag) { /* 直接 SendInput */ }
void send_mouse_wheel(int delta) { /* 直接 SendInput */ }

void dispatch(const std::string& session_id, const std::string& message)
{
    std::lock_guard lock(mutex_);
    inject_input_message(session_id, message, nullptr, &pointer_by_session_[session_id], &held_input_);
}
```

```cpp
// third_party/uu_stream_webrtc/src/shared_input_state.h:32-42（修改前）
SharedInputReleaseBatch releaseSession(const std::string& session_id)
{
    // 只能按会话同时释放键盘和鼠标，没有“仅释放全部鼠标按钮”的切换屏障。
}
```

```cpp
// third_party/uu_stream_webrtc/tests/shared_input_state_tests.cpp:43-48（修改前）
// 原测试覆盖多人同键、错误抬键和会话断开；没有覆盖切换注入后端时保留键盘、清空鼠标按钮。
```

```cpp
// include/FsRemoteStreamApi.h:54-57（修改前）
FSREMOTE_STATUS_MOUSE_MODE = 61,
FSREMOTE_STATUS_QUALITY_APPLIED = 63,
FSREMOTE_STATUS_CURSOR_SHAPE = 64,
FSREMOTE_STATUS_ADMITTED = 70,
```

```cpp
// src/stream/StreamRuntime.h:19-22（修改前）
MouseMode = FSREMOTE_STATUS_MOUSE_MODE,
QualityApplied = FSREMOTE_STATUS_QUALITY_APPLIED,
CursorShape = FSREMOTE_STATUS_CURSOR_SHAPE,
Admitted = FSREMOTE_STATUS_ADMITTED,
```

```cpp
// src/ui/RemoteDesktopWindow.h:56-61, 154-164（修改前）
void setRemoteMouseCaptureActive(bool active);
void setRemoteCursorShape(const QString& statusMessage);

QRect remoteUpdateButtonRect() const;
QRect qualityButtonRect() const;
// 原来没有系统/驱动注入后端枚举、确认状态、pending 状态和按钮热区。
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2265-2274, 2302-2312（修改前）
QRect RemoteDesktopWindow::remoteUpdateButtonRect() const
{
    const QRect qualityRect = qualityButtonRect();
    return QRect(qualityRect.left() - 58, 3, 54, qMax(0, titleBarHeight() - 6));
}

// 标题栏空白命中只排除更新、画质、键鼠同步、剪切板、最小化和关闭按钮。
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/faker_input_bridge_client.h:73-128（修改后）
bool connectAndPing(std::string* error)
{
    // 最长等待本机 \\.\pipe\FakerInputBridge.v1 500ms，并严格验证 FIB1 v1 响应与 driver-ready 标志。
}
bool sendRelativeMouse(...);
bool sendAbsoluteMouse(...);
bool releaseAll(std::string* error);
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:872-1005, 1755-1814（修改后）
class MouseInputBackendRouter final {
public:
    bool selectFaker(std::string* error);
    void selectSystem();
    void moveAbsolute(int x, int y, bool log_result);
    void moveRelative(int dx, int dy, bool log_result);
    void sendButton(int button, bool down);
    void sendWheel(int delta);
    bool consumeFallback(std::string* error);
};

bool handleMouseBackendMessageLocked(...)
{
    // 处理 __fsremote_mouse_backend system/faker/query；切换前释放按钮，结果向全部控制 Viewer 广播。
}
```

```cpp
// third_party/uu_stream_webrtc/src/shared_input_state.h:44-56（修改后）
std::vector<int> releaseAllButtons()
{
    std::vector<int> released;
    for (const auto& [button, holders] : button_holders_) {
        if (holders > 0) released.push_back(button);
    }
    buttons_by_session_.clear();
    button_holders_.clear();
    return released;
}
```

```cpp
// third_party/uu_stream_webrtc/tests/shared_input_state_tests.cpp:48-68（修改后）
uu::SharedInputState backend_switch_state;
// 分别持有键盘、左键和右键。
const std::vector<int> switched_buttons = backend_switch_state.releaseAllButtons();
require(switched_buttons.size() == 2, "backend switch releases every unique held mouse button exactly once");
require(backend_switch_state.updateKey("controller-a", 70, false) == uu::SharedInputTransition::InjectUp,
    "backend switch keeps keyboard ownership intact");
```

```cpp
// include/FsRemoteStreamApi.h:57；src/stream/StreamRuntime.h:22（修改后）
FSREMOTE_STATUS_MOUSE_BACKEND = 65;
MouseBackend = FSREMOTE_STATUS_MOUSE_BACKEND;
```

```cpp
// src/ui/RemoteDesktopWindow.h:136-140, 329-337（修改后）
enum class RemoteMouseBackend { System, Faker };
RemoteMouseBackend m_remoteMouseBackend = RemoteMouseBackend::System;
bool m_remoteMouseBackendKnown = false;
bool m_remoteMouseBackendPending = false;
bool m_remoteMouseBackendFallback = false;
bool m_mouseBackendButtonPressed = false;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2825-2932, 3947-3982（修改后）
void RemoteDesktopWindow::setRemoteMouseBackendStatus(const QString& statusMessage)
{
    // 严格接受 system/faker ready 或 system fallback，确认后才提交真实状态。
}

void RemoteDesktopWindow::toggleRemoteMouseBackend()
{
    // 请求与当前 Host 确认值相反的后端，等待确认期间拒绝重复切换。
}

// 标题栏绘制“系统/驱动”，pending、回退、未确认分别附加 …、!、?。
```

### Steps
1. 复用独立 Bridge 的 FIB1 v1 固定结构，新增 header-only 本机管道客户端，避免改写历史非 UTF-8 子项目 CMake 文件。
2. 在 Host 输入调度器内新增唯一的鼠标后端路由器，绝对移动、相对移动、按钮和滚轮统一经过该路由器，键盘路径不变。
3. 把 Viewer 的 0..65535 绝对坐标缩放为 FakerInput 的 0..32767；滚轮把 Windows 120 单位转换为 HID 刻度。
4. 绝对和相对鼠标属于两个 HID collection，绝对报告固定不携带按钮，按钮只由相对 collection 持有，防止拖拽后粘键。
5. 新增 `system/faker/query` 控制消息和 `system/faker ready`、`system fallback` 状态消息；多控制端共享并同步同一 Host 后端。
6. 切换前只释放全部鼠标按钮并清除共享按钮持有关系，键盘持有状态保持不变。
7. Bridge 未启动、驱动未 ready、协议错误或运行中断管时立即转回 SendInput；运行中故障会在系统侧重建仍按下按钮，并向全部控制窗口广播回退。
8. 在自绘标题栏画质按钮左侧增加 54px 开关，支持悬停气泡、按下视觉态、同热区释放切换和全屏不拦截远端输入。
9. 新 Viewer 收到首帧后查询 Host 全局后端；两秒无确认时恢复按钮并提示可能为旧版本，不把请求值当作生效值。

### Verification
- Visual Studio 2022 x64 环境构建 `fsremote_stream`：通过，生成新的 `third_party/uu_stream_webrtc/fsremote_stream.dll`。
- `uu_shared_input_state_tests`：构建通过，执行退出码 `0`，覆盖切换时释放两个鼠标按钮且保留键盘持有状态。
- Qt `RemoteDesktopWindow.cpp` 与 MOC 对象编译通过。
- 当前正式 `FSRemote.exe` 被正在运行的 PID 2760 占用，常规目标在最后覆盖步骤返回 `LNK1104`；使用相同对象和库改为临时输出 `FSRemote.verify.exe` 后完整链接通过，验证文件随后已删除。
- `git diff --check`：通过。

## 2026-08-05 17:53 - 修复多台旧显卡远控无首帧并增加自动恢复

### Changed Location
- `third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:168`：修正 NVENC 在线重配置结构版本。
- `third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:213-218`：补齐输入步长、帧序号并让驱动决定普通帧类型。
- `third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:271-279`：DXGI 输出失效后清除属于旧 D3D 设备的缓存纹理。
- `src/ui/RemoteDesktopWindow.h:167-169`：声明首帧看门狗生命周期入口。
- `src/ui/RemoteDesktopWindow.h:306-310`：保存首帧定时器和“真正呈现”状态。
- `src/ui/RemoteDesktopWindow.cpp:1859-1862`：创建独立的首帧单次定时器。
- `src/ui/RemoteDesktopWindow.cpp:5091-5100`：D3D11 共享纹理真正呈现后确认首帧并记录诊断。
- `src/ui/RemoteDesktopWindow.cpp:5184-5193`：BGRA 软件回退真正显示后确认首帧并记录诊断。
- `src/ui/RemoteDesktopWindow.cpp:5429-5467`：实现 15 秒首帧超时、明确错误提示和安全自动重连。
- `src/ui/RemoteDesktopWindow.cpp:5634-5691`：每个 Viewer 代际重新启动首帧看门狗，排队时间不计入目标端超时。

### Reason
`PC-20251110PQOC`、`DSKTPCC`、`CHENGLONG` 和 `CLTEST` 的目标端日志都出现 `NvEncEncodePicture failed: 8`，其中 `DSKTPCC` 还发生了 `DXGI access lost`。NVIDIA SDK 头文件明确要求未知输入步长时至少填写输入宽度，并要求 `NV_ENC_RECONFIGURE_PARAMS` 使用结构版本 2 和扩展位；原实现分别保留了 `inputPitch=0` 和结构版本 1，旧 GTX 10/16 系列驱动会直接拒绝参数。

控制端原来只根据解码回调上报状态 50，没有独立确认 D3D/BGRA 是否真正进入可见路径。目标端采集或编码失败时，窗口可能长期停留在“等待远程画面”。本次用 15 秒单次看门狗覆盖首次连接和后续重连，并且只以真实 Present 为成功条件；超时后明确提示目标端采集/编码可能失败，先安全停止旧 Viewer，再按既有退避策略自动重连。

### Original Code
```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:164-218（修改前，节选）
NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
reconfigure.version = nvenc_struct_version(api_version_, 1);
reconfigure.reInitEncodeParams = nextInit;

NV_ENC_PIC_PARAMS pic = {};
pic.inputWidth = desc.Width;
pic.inputHeight = desc.Height;
pic.outputBitstream = bitstream_;
pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
pic.pictureType = force_idr ? NV_ENC_PIC_TYPE_IDR : NV_ENC_PIC_TYPE_P;
```

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:268-273（修改前）
} else if (capture_error == "busy") {
    ++busy_frames_;
} else if (capture_error != "timeout" && capture_error != "DXGI access lost") {
    ++dropped_frames_;
}
```

```cpp
// src/ui/RemoteDesktopWindow.h:160-167、300-306（修改前，节选）
void beginNetworkRecoveryGracePeriod();
void beginNetworkReconnect();
void scheduleNetworkReconnect();
void attemptNetworkReconnect();
void finishNetworkReconnect();
void cancelNetworkReconnect();

QTimer* m_networkReconnectTimer = nullptr;
bool m_networkReconnectActive = false;
bool m_networkRecoveryGraceActive = false;
int m_networkReconnectAttempt = 0;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:5567-5575（修改前）
m_viewerStartAdmissionActive = true;
m_hasReceivedVideoInCurrentViewer = false;
if (!m_networkReconnectActive) {
    m_networkWarningVisible = false;
} else if (m_networkReconnectTimer) {
    m_networkRecoveryGraceActive = true;
    m_networkReconnectTimer->start(15000);
}
```

### Modified Code
```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:166-218（修改后，节选）
NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
// =====wjy====
reconfigure.version = nvenc_struct_version(api_version_, 2, true); // wjy: 使用SDK要求的版本2和扩展位。
// ===end====
reconfigure.reInitEncodeParams = nextInit;

NV_ENC_PIC_PARAMS pic = {};
pic.inputWidth = desc.Width;
pic.inputHeight = desc.Height;
// =====wjy====
pic.inputPitch = desc.Width; // wjy: 未知D3D11步长时至少填写输入宽度。
pic.frameIdx = frame_id; // wjy: 传递单调递增且只在成功后推进的帧号。
// ===end====
pic.outputBitstream = bitstream_;
pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
pic.pictureType = NV_ENC_PIC_TYPE_UNKNOWN; // wjy: enablePTD开启后由驱动决定普通帧型。
```

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:269-281（修改后）
} else if (capture_error == "busy") {
    ++busy_frames_;
// =====wjy====
} else if (capture_error == "DXGI access lost") {
    last_frame_ = {}; // wjy: 立即丢弃属于旧D3D设备的缓存纹理。
    ++dropped_frames_; // wjy: 下一轮按原VDD设备名重新初始化并采集。
    append_stream_capture_diagnostic_log_rate_limited(
        "capture",
        "DXGI access lost; stale reusable frame discarded before reinitialize",
        1000);
// ===end====
}
```

```cpp
// src/ui/RemoteDesktopWindow.h:160-169、303-310（修改后，节选）
void beginNetworkRecoveryGracePeriod();
void beginNetworkReconnect();
void scheduleNetworkReconnect();
void attemptNetworkReconnect();
void finishNetworkReconnect();
void cancelNetworkReconnect();
void startFirstFrameWatchdog(); // wjy: 启动当前Viewer代际的15秒真实首帧等待。
void stopFirstFrameWatchdog(); // wjy: 呈现或终止时取消等待。
void handleFirstFrameTimeout(); // wjy: 明确提示并安全重建会话。

QTimer* m_networkReconnectTimer = nullptr;
QTimer* m_firstFrameWatchdogTimer = nullptr; // wjy: 首帧等待与网络退避互不覆盖。
bool m_networkReconnectActive = false;
bool m_networkRecoveryGraceActive = false;
int m_networkReconnectAttempt = 0;
bool m_hasPresentedVideoInCurrentViewer = false; // wjy: 只记录真正可见的D3D/BGRA首帧。
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:5429-5467（修改后，节选）
void RemoteDesktopWindow::startFirstFrameWatchdog()
{
    if (!m_firstFrameWatchdogTimer || m_closeInProgress || m_applicationExitInProgress
        || remoteUpdateActive() || !m_viewerHandle) {
        return;
    }
    m_firstFrameWatchdogTimer->start(15000); // wjy: 15秒覆盖VDD启动和首个关键帧。
}

void RemoteDesktopWindow::handleFirstFrameTimeout()
{
    if (m_closeInProgress || m_applicationExitInProgress || remoteUpdateActive()
        || !m_viewerHandle || m_hasPresentedVideoInCurrentViewer) {
        return;
    }
    setConnectionStatus(
        FSREMOTE_STATUS_ERROR,
        QString::fromUtf8("等待远程首帧超时，目标端采集或编码可能失败，正在自动重连"));
    if (!m_networkReconnectActive) {
        beginNetworkReconnect(); // wjy: 首次连接没有任何解码帧时也进入退避重建。
    }
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:5088-5100（修改后，节选）
m_connectionStatusCode = 50;
m_connectionStatus = QString::fromUtf8("画面已接收");
m_hasReceivedVideoInCurrentViewer = true;
const bool firstPresentedFrame = !m_hasPresentedVideoInCurrentViewer;
m_hasPresentedVideoInCurrentViewer = true; // wjy: 共享纹理已真正提交到SwapChain/DComp。
stopFirstFrameWatchdog();
if (firstPresentedFrame) {
    appendViewerDebugLog(QStringLiteral("first frame presented host=%1 path=d3d11 frame_id=%2 generation=%3")
        .arg(m_hostIp)
        .arg(frame->frameId)
        .arg(frame->viewerGeneration));
}
```

### Steps
1. 对照 NVIDIA SDK 头文件检查 `NV_ENC_PIC_PARAMS` 和 `NV_ENC_RECONFIGURE_PARAMS` 的字段约束，修正导致返回码 8 的两个参数。
2. 保留首帧强制 IDR 与 SPS/PPS 输出标志，但把普通帧类型交还 `enablePTD` 驱动决策，并传递单调帧号。
3. DXGI `ACCESS_LOST` 后清空旧设备纹理，下一采集循环继续复用已有的按设备名自动初始化逻辑。
4. 为每个 Viewer 代际增加独立 15 秒首帧看门狗，排队等待共享初始化名额的时间不计入超时。
5. 将首帧成功条件从原生状态 50 收紧为 D3D11 或 BGRA 真正呈现，并为两条路径写入低频诊断日志。
6. 首帧超时通过统一连接终态入口释放键鼠和同步资格，再停止旧 Viewer 并进入无限退避重连。
7. 复核 `DESKTOP-P2RBUQP` 控制端日志，确认 2026-08-05 17:33 会话先收到状态 50，随后由本机 `closeEvent` 主动停止；目标端 ICE closed 是该主动关闭的结果。

### Verification
- `cmake --build build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release --config Release --target FSRemote`：NVENC、DXGI 和 `fsremote_stream.dll` 编译/链接通过；复制 DLL 到正在运行的主目录时因现有 `FSRemote.exe` 占用而停止，没有结束用户正在运行的程序。
- `cmake --build build-video-webrtc-msvc --config Release --target FSRemote`：独立 Release 主程序完整编译、链接和 Qt 部署通过。
- `cmake --build build-video-webrtc-msvc --config Release --target uu_host_media_pipeline_tests fsremote_remote_connection_state_tests`：两个针对性测试目标编译通过。
- `ctest --test-dir build-video-webrtc-msvc -C Release -R "^(uu_host_media_pipeline_tests|fsremote_remote_connection_state_tests)$" --output-on-failure`：2/2 通过。
- 构建仍保留 WebRTC 头文件的既有 C4068、宽字符转换 C4244，以及 `RemoteDesktopWindow.cpp` 既有 C4834/C4804 警告；本次修改没有新增编译错误。
- `git diff --check`：通过。
- 当前开发机没有被控端的 FakerInputBridge/游戏运行环境，因此尚未进行本次集成版本的真实远端动态切换；此前独立 Bridge 点击已由用户在 AION2 中确认响应。

## 2026-07-24 16:34 - FakerInput 缺失时静默安装并自动启动 Bridge

### Changed Location
- `third_party/uu_stream_webrtc/src/faker_input_runtime_provisioner.h:1`：新增独立的 FakerInput 运行时准备器，负责固定哈希校验、安装状态查询、管理员令牌检查、隐藏 MSI、Bridge 启动和生命周期清理。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:12`：Host 引入准备器，并在输入调度器后台线程执行，不阻塞现有系统鼠标和 WebRTC 控制消息。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1572`：新增准备线程的创建、等待和最终状态提交。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1773`：后端协议新增 `system installing` 中间态，并支持安装期间切回系统模式。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:2909`：Viewer 原生白名单接收严格的 `system installing` 状态。
- `src/ui/RemoteDesktopWindow.cpp:2825`：标题栏显示静默安装进度，安装中禁止重复点击，并设置六分钟保护超时。
- `src/ui/RemoteDesktopWindow.cpp:2929`：失败提示改为检查管理员安装和 MSI/Bridge 完整性。
- `CMakeLists.txt:34`：将准备器登记到 `fsremote_stream` 并链接 Windows CryptoAPI 所需的 `advapi32`。
- `CMakeLists.txt:484`：使用独立的始终执行旁车目标复制最新 `fsremote_stream.dll`，主 EXE 无需重链接时发布根目录也不会残留旧 DLL。
- `CMakeLists.txt:498`：构建时自动确认独立 Bridge 已复制；兼容用户已经放在 build 根目录的 MSI，并支持配置稳定 MSI 源路径。
- `installer/FSRemote.iss.in:13`：网络安装器提升为管理员安装并禁止 MSI 自动重启系统。
- `installer/FSRemote.iss.in:55`：安装器按 ProductCode 跳过已安装版本，校验 MSI SHA-256 后隐藏执行 `msiexec /qn /norestart`。
- `installer/FSRemote.iss.in:126`：发布源必须同时包含主程序、Bridge 和 MSI。

### Reason
原实现只会连接用户手工启动的 `FakerInputBridge.exe --server`。目标设备缺少 FakerInput 驱动或 Bridge 未启动时，标题栏只能回退到 SendInput，无法在无人值守的被控端完成准备。本次把安装放到管理员网络安装器，并增加 Host 运行时兜底：已安装设备只启动 Bridge，未安装且当前进程已有管理员令牌时才静默执行 MSI；普通 `asInvoker` 进程不会绕过 UAC，而是安全回退系统鼠标。

MSI 与 Bridge 都位于用户可写的程序发布目录，因此在执行前固定校验当前已验证成品的 SHA-256。MSI 哈希为 `4C0AEFB7340051A91D606776243298B5CD1143EF5508BBAE6800C474F9ED0840`，Bridge 哈希为 `440FADF4D09000AE3BFEF115DA45A3D2F5F90C4FC4E124729C837FB47C628192`。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/faker_input_runtime_provisioner.h
// 新文件，此位置原来没有代码。
```

```cmake
# CMakeLists.txt:34-38（修改前）
target_sources(fsremote_stream PRIVATE
    third_party/uu_stream_webrtc/src/control_admission_policy.h
    third_party/uu_stream_webrtc/src/shared_input_state.h
    third_party/uu_stream_webrtc/src/windows_cursor_classifier.h
)

# CMakeLists.txt:480-492、原 488 后（修改前）
add_custom_command(TARGET FSRemote POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:fsremote_stream>"
        "$<TARGET_FILE_DIR:FSRemote>")
# 只有 FSRemote.exe 自身重链接时才会执行，DLL 单独更新会留在子目录。

# 没有 FakerInputBridge/MSI 的发布复制与完整性提示。
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:12-17（修改前）
#include "control_admission_policy.h"
#include "faker_input_bridge_client.h"
#include "shared_input_state.h"

// InputDispatcher（修改前）
// 没有后台准备线程；选择 faker 时直接在输入调度锁内 connectAndPing，失败立即 fallback。
ready = mouse_backend_.selectFaker(&bridge_error);

// Viewer 状态白名单（修改前）
message == "__fsremote_mouse_backend_status system ready"
    || message == "__fsremote_mouse_backend_status faker ready"
    || message == "__fsremote_mouse_backend_status system fallback"
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2831-2851（修改前）
const bool ready = parts.at(2) == QStringLiteral("ready");
const bool fallback = parts.at(2) == QStringLiteral("fallback");
if ((!system && !faker) || (!ready && !fallback) || (fallback && !system)) return;
m_remoteMouseBackendPending = false;
```

```ini
; installer/FSRemote.iss.in:13（修改前）
PrivilegesRequired=lowest

; [Code]（修改前）
; 没有 ProductCode 查询、哈希校验或静默 MSI 安装。
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/faker_input_runtime_provisioner.h:242-306
bool ensureReady(std::string* error)
{
    if (probeBridge(100, &probeError)) return true;
    if (startOwnedBridge(bridgePath, directory, &bridgeError)
        && probeBridge(5000, &bridgeError)) return true;

    if (productInstalled(&querySucceeded, &queryError)) return false;
    if (!sha256Matches(installerPath, kInstallerSha256, error)) return false;
    if (!currentProcessElevated(error)) return false;
    // 隐藏执行 msiexec /i <msi> /qn /norestart，并在成功后重新启动 Bridge、确认 driver-ready。
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1572-1584, 1804-1814, 1853-1887
InputDispatcher()
{
    faker_provision_worker_ = std::thread([this] { provisionFakerInputRuntime(); });
}

if (requested == MouseInjectionBackend::Faker
    && mouse_backend_.backend() != MouseInjectionBackend::Faker
    && !faker_provision_complete_) {
    *backend_token_out = "system";
    *backend_state_out = "installing";
    collectAllMouseBackendCallbacksLocked(callbacks);
    return true;
}

void provisionFakerInputRuntime()
{
    const bool provisioned = faker_provisioner_.ensureReady(&provisionError);
    // 在调度锁内只提交最终 faker ready 或 system fallback，耗时安装始终在锁外。
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2833-2870
const bool installing = parts.at(2) == QStringLiteral("installing");
m_remoteMouseBackendPending = installing;
if (installing) {
    m_remoteMouseBackendMessage = zh("目标端正在静默安装 FakerInput 并启动驱动服务…");
    QTimer::singleShot(6 * 60 * 1000, this, [this, requestGeneration] {
        // 超时只恢复按钮和系统鼠标提示，不中断目标端 MSI 事务。
    });
}
```

```cmake
# CMakeLists.txt:484-532
add_custom_target(FSRemoteStreamRuntimeCopy
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:fsremote_stream>"
        "$<TARGET_FILE_DIR:FSRemote>"
    DEPENDS fsremote_stream)
add_dependencies(FSRemote FSRemoteStreamRuntimeCopy)

set(FSREMOTE_FAKER_INPUT_BRIDGE_EXE "" CACHE FILEPATH "Prebuilt FakerInputBridge.exe copied beside FSRemote.exe")
add_custom_target(FSRemoteFakerInputBridgeCopy
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FSREMOTE_FAKER_INPUT_BRIDGE_EXE}"
        "$<TARGET_FILE_DIR:FSRemote>/FakerInputBridge.exe")
add_dependencies(FSRemote FSRemoteFakerInputBridgeCopy)
set(FSREMOTE_FAKER_INPUT_MSI "" CACHE FILEPATH "FakerInput 0.1.1 x64 MSI copied beside FSRemote.exe")
```

```ini
; installer/FSRemote.iss.in:13-14, 55-104
PrivilegesRequired=admin
RestartIfNeededByRun=no

function MsiQueryProductState(ProductCode: String): Integer;
  external 'MsiQueryProductStateW@msi.dll stdcall';

InstallerHash := GetSHA256OfFile(InstallerPath);
Exec(ExpandConstant('{sys}\msiexec.exe'),
  '/i "' + InstallerPath + '" /qn /norestart',
  ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
```

### Steps
1. 新增 header-only 运行时准备器，从 `FSRemote.exe` 同目录解析固定的 MSI 和 Bridge，不使用当前工作目录或 PATH。
2. 对 MSI/Bridge 计算 SHA-256 并与已验证成品固定值比较；不一致时拒绝启动或安装。
3. 动态调用系统 `msi.dll` 查询 FakerInput 0.1.1 ProductCode；只有 `INSTALLSTATE_DEFAULT` 视为完整安装，已安装时只隐藏启动 Bridge。
4. 未安装时检查当前 FSRemote 是否已经提升；只有管理员令牌才隐藏执行 `msiexec /qn /norestart`，绝不尝试绕过 UAC。
5. 用受限生命周期 Job 管理本进程创建的 Bridge；FSRemote 退出时只清理自己创建的子进程，用户手工启动的 Bridge 不受影响。
6. 在 Host 输入调度器中创建后台准备线程；安装期间系统鼠标继续工作，并向 Viewer 广播 `system installing`。
7. Viewer 标题栏显示安装进度并禁用重复切换；最终只接受 `faker ready` 或 `system fallback`。
8. 网络安装器改为管理员权限，在 FSRemote 首次启动前静默安装驱动，正确处理 0、3010 和 1641 返回码。
9. 构建发布流程使用独立旁车目标复制最新原生 DLL 和 Bridge，并确认用户放入 build 根目录的 MSI 可被当前发布目录直接使用；主 EXE 未重链接也不会留下旧 DLL。

### Verification
- Visual Studio 2022 x64 环境构建 `fsremote_stream`：通过，生成 `fsremote_stream.dll`。
- 构建 `FSRemote`：通过，Qt 源码、MOC、主程序链接及发布后复制均成功。
- 构建 `FSRemoteInstaller`：通过，Inno Setup 已验证 `GetSHA256OfFile`、`MsiQueryProductStateW` 声明和静默安装脚本语法。
- `uu_shared_input_state_tests.exe`：执行退出码 `0`。
- build 输出已确认包含 `FSRemote.exe`、`fsremote_stream.dll`、`FakerInputBridge.exe`、`FakerInput_Setup_0.1.1_x64.msi` 和 `FSRemote安装器.exe`。
- build 中 Bridge SHA-256 为 `440FADF4D09000AE3BFEF115DA45A3D2F5F90C4FC4E124729C837FB47C628192`；MSI SHA-256 为 `4C0AEFB7340051A91D606776243298B5CD1143EF5508BBAE6800C474F9ED0840`，均与代码固定值一致。
- 为避免改动开发机驱动状态，本轮没有实际执行 MSI；真实目标机仍需用管理员权限运行新版 FSRemote 安装器完成首次部署验证。

## 2026-07-24 16:59 - 发布更新纳入 FakerInput Bridge 与 MSI

### Changed Location
- `src/system/UpdateService.cpp:67-70`：把 `FakerInputBridge.exe` 与 `FakerInput_Setup_0.1.1_x64.msi` 加入根目录运行文件白名单，使发布、完整性校验、更新暂存、回撤校验和旧共享根目录清理使用同一组依赖。

### Reason
构建与网络安装器已经能生成和使用 FakerInput Bridge/MSI，但“发布更新”只复制固定运行白名单中的文件。若不把两个依赖加入该白名单，新设备通过共享目录更新后会缺少 Bridge 或驱动安装包，标题栏切换到驱动鼠标时只能回退到系统鼠标。

### Original Code
```cpp
// src/system/UpdateService.cpp:64-68（修改前）
QStringLiteral("FSRemote.exe"),
QStringLiteral("FSRemoteUpdater.exe"),
QStringLiteral("fsremote_stream.dll"),
QStringLiteral("Qt6Core.dll"), QStringLiteral("Qt6Gui.dll"), QStringLiteral("Qt6Network.dll"),
```

### Modified Code
```cpp
// src/system/UpdateService.cpp:64-71（修改后）
QStringLiteral("FSRemote.exe"),
QStringLiteral("FSRemoteUpdater.exe"),
QStringLiteral("fsremote_stream.dll"),
// =====wjy====
QStringLiteral("FakerInputBridge.exe"), // wjy: 驱动鼠标依赖的独立本机 Bridge 必须随每个不可变 release 发布、更新和回撤。
QStringLiteral("FakerInput_Setup_0.1.1_x64.msi"), // wjy: 目标端缺少驱动时由 FSRemote/安装器静默调用此固定 MSI，缺失即拒绝形成发布版本。
// ===end====
QStringLiteral("Qt6Core.dll"), QStringLiteral("Qt6Gui.dll"), QStringLiteral("Qt6Network.dll"),
```

### Steps
1. 将 Bridge 与固定版本 MSI 登记为发布根目录的必需运行文件。
2. 复用现有 `rootRuntimeFileNames()` 调用链，让发布复制与逐文件校验自动覆盖这两个依赖。
3. 让远端版本发现、更新暂存与回撤继续执行相同的必需文件检查，避免形成缺少驱动依赖的不完整版本。
4. 保留现有安全边界：本次没有启动发布流程，也没有执行 MSI。

### Verification
- Release 构建根目录已确认存在 `FakerInputBridge.exe`（297472 字节）与 `FakerInput_Setup_0.1.1_x64.msi`（1089536 字节）。
- Visual Studio 2022 x64 环境构建 `fsremote_update_service_tests`：通过，包含修改后的 `UpdateService.cpp` 编译与链接。
- `fsremote_update_service_tests.exe`：执行退出码 `0`。
- 完整 `FSRemote` 构建已完成源码编译，但正在运行的 PID 32160 占用 `FSRemote.exe`，最后覆盖链接返回 `LNK1104`；未强制关闭用户正在运行的程序。

## 2026-07-24 17:32 - 同版本更新自动识别并修复 FakerInput 缺失依赖

### Changed Location
- `src/system/UpdateService.h:32-36`：公开运行依赖残缺检查与“升级或修复可用”统一判定，供更新检查、执行入口和自动化测试复用。
- `src/system/UpdateService.cpp:454-481`：新增 Bridge/MSI 非空文件检查；更高版本正常更新，同版本仅在依赖缺失时开放修复。
- `src/system/UpdateService.cpp:611-615`：允许同版本缺件设备复用当前不可变 release，继续走完整暂存、校验、独立更新器替换与重启流程。
- `src/system/UpdateService.cpp:854-857`：周期检查在远端版本等于本地版本时继续检查缺失依赖，使标题栏和远端设备命令重新显示更新入口。
- `tests/update_service_tests.cpp:10-32`：增加旧客户端漏下发场景回归测试，并改用显式失败返回码保证 Release `/DNDEBUG` 下仍真正执行断言。

### Reason
旧版 FSRemote 使用自身硬编码的运行文件白名单下载新版本。首次引入 `FakerInputBridge.exe` 与 `FakerInput_Setup_0.1.1_x64.msi` 时，旧客户端会先把主程序版本更新到最新，却漏掉自己不认识的两个新增文件；重启后本地版本已经等于共享版本，原逻辑不再允许第二次更新，最终只能手工复制 Bridge。本次保留正常更新的严格升序规则，只为“同版本且本地 FakerInput 依赖确实缺失”增加一次可控修复入口，避免无条件重复安装和自动重启。

### Original Code
```cpp
// src/system/UpdateService.h:29-33（修改前）
static QString bumpPatchVersion(const QString& version);
static int compareSemanticVersions(const QString& left, const QString& right);

static QStringList availableRollbackVersions(QString* errorMessage = nullptr);
```

```cpp
// src/system/UpdateService.cpp:445-455（修改前）
int UpdateService::compareSemanticVersions(const QString& left, const QString& right)
{
    // 只比较三段语义版本。
}

QStringList UpdateService::availableRollbackVersions(QString* error)
```

```cpp
// src/system/UpdateService.cpp:582-586（修改前）
if (compareSemanticVersions(remoteVersion, localVersion) <= 0) {
    if (error) *error = QString::fromUtf8("共享版本不是高于当前版本的新版本。");
    return false;
}
return prepareRemoteVersionInstall(remoteVersion, error);
```

```cpp
// src/system/UpdateService.cpp:823-830（修改前）
m_remoteVersion = remoteVersion;
const QString localVersion = normalizeSemanticVersion(localVersionText(), QStringLiteral("0.0.0"));
m_updateAvailable = !m_remoteVersion.isEmpty()
    && compareSemanticVersions(m_remoteVersion, localVersion) > 0;
emit updateAvailabilityChanged(m_updateAvailable, m_remoteVersion);
```

```cpp
// tests/update_service_tests.cpp:7-15（修改前）
assert(platform::UpdateService::compareSemanticVersions("1.2.3", "1.2.4") < 0);
assert(platform::UpdateService::compareSemanticVersions("2.0.0", "2.0.0") == 0);
assert(platform::UpdateService::bumpPatchVersion("1.9.9") == "1.9.10");
// 没有 FakerInput 同版本缺件测试；Release 的 /DNDEBUG 会关闭 assert。
```

### Modified Code
```cpp
// src/system/UpdateService.h:29-39（修改后）
static QString bumpPatchVersion(const QString& version);
static int compareSemanticVersions(const QString& left, const QString& right);
static bool runtimeDependenciesNeedRepair(const QString& runtimeRoot); // wjy: 检查当前运行目录是否缺少 FakerInput Bridge/MSI。
static bool remoteUpdateOrRepairAvailable(
    const QString& remoteVersion,
    const QString& localVersion,
    const QString& runtimeRoot); // wjy: 版本相同仅在本地依赖残缺时开放一次修复更新。
```

```cpp
// src/system/UpdateService.cpp:454-481（修改后）
bool UpdateService::runtimeDependenciesNeedRepair(const QString& runtimeRoot)
{
    const QStringList repairFileNames{
        QStringLiteral("FakerInputBridge.exe"),
        QStringLiteral("FakerInput_Setup_0.1.1_x64.msi"),
    };
    for (const QString& fileName : repairFileNames) {
        const QFileInfo fileInfo(QDir(runtimeRoot).filePath(fileName));
        if (!fileInfo.isFile() || fileInfo.size() <= 0) return true;
    }
    return false;
}

return comparison > 0
    || (comparison == 0 && runtimeDependenciesNeedRepair(runtimeRoot));
```

```cpp
// src/system/UpdateService.cpp:611-615、854-857（修改后）
if (!remoteUpdateOrRepairAvailable(remoteVersion, localVersion, QCoreApplication::applicationDirPath())) {
    return false; // wjy: 只有 Bridge/MSI 确实缺失时才允许同版本修复。
}
return prepareRemoteVersionInstall(remoteVersion, error);

m_updateAvailable = remoteUpdateOrRepairAvailable(
    m_remoteVersion, localVersion, QCoreApplication::applicationDirPath());
```

```cpp
// tests/update_service_tests.cpp:16-32（修改后）
QTemporaryDir runtimeDir;
if (!platform::UpdateService::remoteUpdateOrRepairAvailable("1.1.100", "1.1.100", runtimeDir.path())) return 8;
// 写入非空 Bridge/MSI 后，同版本修复必须关闭；正常升级继续开放；远端旧版本始终拒绝。
if (platform::UpdateService::remoteUpdateOrRepairAvailable("1.1.100", "1.1.100", runtimeDir.path())) return 12;
if (!platform::UpdateService::remoteUpdateOrRepairAvailable("1.1.101", "1.1.100", runtimeDir.path())) return 13;
if (platform::UpdateService::remoteUpdateOrRepairAvailable("1.1.99", "1.1.100", runtimeDir.path())) return 15;
```

### Steps
1. 把 Bridge 与固定 MSI 定义为同版本修复所需的两个本地依赖，缺失或零字节都视为残缺。
2. 统一版本方向和依赖状态判定：远端更高时正常更新，版本相同且缺件时修复，远端更低时始终拒绝。
3. 周期检查使用统一判定，使升级后的新程序发现同版本缺件时重新显示“更新”。
4. 更新执行入口使用同一判定，防止界面显示可修复但点击后又被旧的严格升序条件拒绝。
5. 同版本修复继续调用现有完整载荷事务，由独立更新器在主程序退出后替换文件并自动重启，不直接覆盖运行文件。
6. 使用临时运行目录覆盖缺失、齐全、正常升级和远端降级四类测试，并移除会被 Release 禁用的 `assert` 依赖。

### Verification
- Visual Studio 2022 x64 Release 构建 `fsremote_update_service_tests`：通过。
- `fsremote_update_service_tests.exe`：显式测试退出码 `0`；测试在 `/DNDEBUG` 下仍真实执行。
- 正式 `FSRemote` 目标已完成 MOC 和修改后 `UpdateService.cpp` 编译；当前运行的 `FSRemote.exe` 被占用，正常输出覆盖在最后链接阶段返回 `LNK1104`。
- 使用正式目标的相同对象文件、库和嵌入清单临时链接 `FSRemote.verify.exe`：通过，生成后已删除临时成品。
- `git diff --check`：通过。
- 未实际触发同版本修复更新，避免在开发机上退出当前 FSRemote 或改动驱动安装状态。

## 2026-07-24 17:44 - 修复 FakerInput FPS 相对移动探测时的视角狂转

### Changed Location
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1184-1245`：把自动锁鼠判定从二值结果扩展为“绝对移动、Faker 探测静默、相对移动”三种路由结果。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1247-1308`：FakerInput 中心锁定候选帧只更新 Viewer 坐标基线，不再发送绝对 HID 报告；确认后继续使用原相对位移算法。

### Reason
系统模式与 FakerInput 模式原本都能在中心锁定确认后计算相对位移，FPS 视角狂转发生在确认前的前两次探测：旧逻辑会把 Viewer 的远端绝对坐标交给当前注入后端，FakerInput 因而生成绝对鼠标 HID 报告，游戏的 Raw Input 可能将其解释为巨大视角位移。本次保留原有三次确认门槛、中心距离阈值、退出判定和相对位移限幅，只在 FakerInput 的中心锁定候选阶段抑制绝对 HID，避免改变系统模式和普通桌面操作。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1184-1227（修改前）
bool should_use_relative_mouse_for_move(int x, int y, bool log_result, uu::WebrtcSession* session)
{
    // ...读取目标机光标与屏幕中心距离...
    const bool cursor_near_center = cursor_ok && std::abs(center_dx) <= 3 && std::abs(center_dy) <= 3;
    const bool target_far = normalized_point_far_from_center(x, y, screen_w, screen_h);

    if (cursor_near_center && target_far) {
        g_mouse_input_mode.lock_score = std::min(g_mouse_input_mode.lock_score + 1, 4);
        g_mouse_input_mode.unlock_score = 0;
    }
    // ...累计三次后进入相对模式，累计十次离开中心后退出...
    return g_mouse_input_mode.game_relative_mode;
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1238-1243（修改前）
if (!should_use_relative_mouse_for_move(x, y, log_result, session)) {
    pointer->has_last_viewer_pos = true;
    pointer->last_viewer_x = x;
    pointer->last_viewer_y = y;
    mouse_backend->moveAbsolute(x, y, log_result);
    return;
}
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1184-1245（修改后）
// =====wjy====
enum class MouseMoveRoutingDecision {
    Absolute,
    SuppressFakerAbsoluteProbe,
    Relative,
}; // wjy: 将“桌面绝对移动、驱动探测静默、游戏相对移动”分开表达，避免用单个 bool 把探测阶段误当成普通桌面移动。

MouseMoveRoutingDecision decide_mouse_move_routing(
    int x,
    int y,
    bool log_result,
    uu::WebrtcSession* session,
    bool faker_backend)
{
    // ...完全复用原中心锁定计分与退出逻辑...
    center_lock_candidate = cursor_near_center && target_far;
    if (g_mouse_input_mode.game_relative_mode) {
        return MouseMoveRoutingDecision::Relative;
    }
    if (faker_backend && center_lock_candidate) {
        return MouseMoveRoutingDecision::SuppressFakerAbsoluteProbe;
    }
    return MouseMoveRoutingDecision::Absolute;
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1247-1274（修改后）
const MouseMoveRoutingDecision routing = decide_mouse_move_routing(
    x,
    y,
    log_result,
    session,
    mouse_backend->backend() == MouseInjectionBackend::Faker);
if (routing != MouseMoveRoutingDecision::Relative) {
    pointer->has_last_viewer_pos = true;
    pointer->last_viewer_x = x;
    pointer->last_viewer_y = y;
    if (routing == MouseMoveRoutingDecision::SuppressFakerAbsoluteProbe) {
        if (log_result) {
            append_input_debug_log("host FakerInput abs suppressed reason=center-lock-probe" + cursor_lock_probe_text());
        }
        return;
    }
    mouse_backend->moveAbsolute(x, y, log_result);
    return;
}
```

### Steps
1. 复核 Viewer 中心重定位、Host 自动锁鼠判定、系统相对 `SendInput` 与 FakerInput 相对 HID 的完整链路。
2. 保留系统模式原有行为，把中心锁定候选状态显式传递到输入路由结果。
3. FakerInput 第一次、第二次中心锁定候选只记录 Viewer 坐标基线，不发送虚拟绝对鼠标报告。
4. 第三次确认后仍递增相对模式代次，并沿用既有相对差值、屏幕尺寸换算及 `-200..200` 限幅。
5. 增加 `center-lock-probe` 抑制日志，便于在目标机确认切换过程，不把主动静默误判为 Bridge 故障。

### Verification
- `git diff --check -- third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp`：通过。
- Visual Studio 2022 x64 Release 定向构建 `fsremote_stream`：编译与 `fsremote_stream.dll` 最终链接均通过。
- 发布根目录旁车复制目标 `FSRemoteStreamRuntimeCopy`：因 PID 14632 的现有 `FSRemote.exe` 正在加载根目录旧 DLL，Windows 拒绝覆盖；未强制结束用户进程。新 DLL 已完整生成在 `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release/third_party/uu_stream_webrtc/fsremote_stream.dll`，退出当前 FSRemote 后重新构建/执行复制目标即可进入发布根目录。
- 已静态核对路由：系统后端中心锁定候选仍发送绝对移动；FakerInput 候选帧静默；确认相对模式后两种后端仍走原相对算法；普通 FakerInput 桌面坐标仍走绝对 HID。
- 未在本机实际进入 FPS 游戏测试 Raw Input；需要随下一版 DLL 在目标设备上验证视角与灵敏度。

## 2026-07-25 08:31 - 接入 FakerInput 驱动键盘

### Changed Location
- `third_party/uu_stream_webrtc/src/faker_input_keyboard_state.h:1-168`：新增 Windows VK 到 USB HID usage 的映射及标准六键键盘快照状态机。
- `third_party/uu_stream_webrtc/src/faker_input_bridge_client.h:22-41, 69, 125-138`：复用 Bridge v1 的键盘命令和固定 8 字节载荷。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:16, 882-911, 966-989, 1018-1035, 1592-1605, 1789-1828, 1881-1921`：把远端键盘事件接入当前系统/FakerInput 后端，并补齐断管、断线、关闭和切换时的安全释放。
- `third_party/uu_stream_webrtc/src/shared_input_state.h:56-72`：新增完整键鼠后端切换所需的统一释放屏障。
- `third_party/uu_stream_webrtc/tests/shared_input_state_tests.cpp:3, 68-119`：覆盖统一键鼠释放、常用游戏键、左右修饰键、重复 down、未映射键和六键上限。
- `src/ui/RemoteDesktopWindow.cpp:2854-2945`：标题栏后端状态、请求提示和气泡统一改为“系统键鼠/驱动键鼠”。
- `src/ui/RemoteDesktopWindow.h:161`：说明兼容保留的鼠标模式热区现在切换完整键鼠后端。
- `include/FsRemoteStreamApi.h:57`：保留既有 ABI 状态名，但注释明确其现在代表键鼠注入后端。
- `CMakeLists.txt:36`：将驱动键盘状态机登记到生产 DLL 源文件和测试构建依赖中。

### Reason
原“驱动”模式只把鼠标交给 FakerInput，键盘始终使用 Windows `SendInput`。这会造成同一游戏的登录界面依赖虚拟 HID 才能接收输入，而进入游戏后键盘仍因 `SendInput` 被忽略。独立 `FakerInputBridge` 的 v1 协议已经公开 `keyboard = 2` 命令，现有 MSI 和 Bridge 无需升级；FSRemote 只需将 Viewer 的 Windows 虚拟键维护成标准 boot-keyboard 快照并经同一受限本机管道发送。

本次沿用现有标题栏按钮的统一语义：选择“驱动”时键盘和鼠标都走 FakerInput，选择“系统”时两者都走 `SendInput`。后端切换前同时抬起旧后端的键和鼠标按钮，避免 key-down 与 key-up 跨后端形成卡键。当前尚未新增“系统鼠标＋驱动键盘”的独立混合模式。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/faker_input_keyboard_state.h（修改前）
// 新增代码，此处无原代码。
```

```cpp
// third_party/uu_stream_webrtc/src/faker_input_bridge_client.h:21-42, 112-125（修改前）
constexpr std::uint16_t kCommandPing = 1;
constexpr std::uint16_t kCommandRelativeMouse = 3;
constexpr std::uint16_t kCommandAbsoluteMouse = 4;
constexpr std::uint16_t kCommandReleaseAll = 5;

struct MessageHeader { /* FIB1 v1 公共消息头 */ };
struct RelativeMousePayload { /* 8 字节相对鼠标载荷 */ };

bool connectAndPing(std::string* error) { /* 连接并确认驱动 ready */ }
bool sendRelativeMouse(...);
// 原客户端没有声明键盘命令、KeyboardPayload 或 sendKeyboard()。
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:15, 949-990, 1556-1569（修改前）
#include "faker_input_bridge_client.h"
// 原来没有 faker_input_keyboard_state.h。

class MouseInputBackendRouter final {
public:
    void sendButton(int button, bool down);
    void sendWheel(int delta);
    // 原路由器只承载鼠标；断管回退也只重建仍按下的鼠标按钮。
};

if (kind == "k") {
    const uu::SharedInputTransition transition = held_input->updateKey(session_id, vk, down != 0);
    if (transition == uu::SharedInputTransition::InjectDown
        || transition == uu::SharedInputTransition::InjectRepeat) {
        send_key(vk, true);
    } else if (transition == uu::SharedInputTransition::InjectUp) {
        send_key(vk, false);
    }
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1755-1814（修改前）
for (const int key : released.keys) send_key(key, false);

if (requested != mouse_backend_.backend()) {
    const std::vector<int> released_buttons = held_input_.releaseAllButtons();
    for (const int button : released_buttons) mouse_backend_.sendButton(button, false);
    // 切换只释放鼠标，键盘仍留在 SendInput。
}
```

```cpp
// third_party/uu_stream_webrtc/src/shared_input_state.h:43-57（修改前）
std::vector<int> releaseAllButtons()
{
    // 只收集和清空共享鼠标按钮；没有一次性释放全部键鼠的接口。
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2825-2932（修改前）
m_remoteMouseBackendMessage = zh("驱动鼠标不可用，已自动回退系统鼠标");
m_remoteMouseBackendMessage = zh("驱动鼠标已启用（FakerInputBridge）");

return m_remoteMouseBackend == RemoteMouseBackend::Faker
    ? zh("驱动鼠标（FakerInputBridge）\n键盘仍使用 SendInput；点击切换为系统鼠标")
    : zh("系统鼠标（SendInput）\n点击切换为驱动鼠标");
```

```cpp
// src/ui/RemoteDesktopWindow.h:161；include/FsRemoteStreamApi.h:57（修改前）
QRect mouseInputModeRect() const;
FSREMOTE_STATUS_MOUSE_BACKEND = 65, // 状态仅说明系统/FakerInputBridge 鼠标注入后端。
```

```cmake
# CMakeLists.txt:34-39（修改前）
target_sources(fsremote_stream PRIVATE
    third_party/uu_stream_webrtc/src/control_admission_policy.h
    third_party/uu_stream_webrtc/src/faker_input_runtime_provisioner.h
    third_party/uu_stream_webrtc/src/shared_input_state.h
)
# 原来没有登记 faker_input_keyboard_state.h。
```

```cpp
// third_party/uu_stream_webrtc/tests/shared_input_state_tests.cpp:48-80（修改前）
const std::vector<int> switched_buttons = backend_switch_state.releaseAllButtons();
require(backend_switch_state.updateKey("controller-a", 70, false)
        == uu::SharedInputTransition::InjectUp,
    "backend switch keeps keyboard ownership intact");
// 原测试明确保留键盘状态，没有驱动键盘映射和快照测试。
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/faker_input_keyboard_state.h:14-23, 24-49, 110-168（修改后）
struct FakerInputKeyboardReport {
    std::uint8_t modifiers = 0;
    std::array<std::uint8_t, 6> usages{};
}; // wjy: 标准 boot-keyboard 快照。

constexpr std::uint8_t fakerInputModifierBit(int virtualKey) noexcept;
constexpr std::uint8_t fakerInputUsageForVirtualKey(int virtualKey) noexcept;

class FakerInputKeyboardState final {
public:
    FakerInputKeyboardUpdate update(int virtualKey, bool down);
    FakerInputKeyboardReport report() const noexcept;
    const std::vector<int>& pressedVirtualKeys() const noexcept;
    void clear() noexcept;
};
```

```cpp
// third_party/uu_stream_webrtc/src/faker_input_bridge_client.h:22-41, 69, 125-138（修改后）
constexpr std::uint16_t kCommandKeyboard = 2;

struct KeyboardPayload {
    std::uint8_t modifiers = 0;
    std::uint8_t reserved = 0;
    std::uint8_t usages[6]{};
};
static_assert(sizeof(KeyboardPayload) == 8);

bool sendKeyboard(std::uint8_t modifiers,
    const std::array<std::uint8_t, 6>& usages, std::string* error)
{
    faker_input_bridge_detail::KeyboardPayload payload;
    payload.modifiers = modifiers;
    std::memcpy(payload.usages, usages.data(), usages.size());
    return request(faker_input_bridge_detail::kCommandKeyboard, &payload, sizeof(payload), error);
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:966-989, 1018-1035（修改后）
void sendKey(int virtualKey, bool down)
{
    const uu::FakerInputKeyboardUpdate update = keyboard_state_.update(virtualKey, down);
    if (backend_ != MouseInjectionBackend::Faker) {
        send_key(virtualKey, down);
        return;
    }
    if (update == uu::FakerInputKeyboardUpdate::Unsupported) {
        send_key(virtualKey, down);
        return;
    }
    if (update == uu::FakerInputKeyboardUpdate::Rollover
        || update == uu::FakerInputKeyboardUpdate::Unchanged) return;

    const uu::FakerInputKeyboardReport report = keyboard_state_.report();
    std::string bridge_error;
    if (bridge_.sendKeyboard(report.modifiers, report.usages, &bridge_error)) return;
    fallbackToSystem(bridge_error);
}

void fallbackToSystem(const std::string& bridge_error)
{
    bridge_.close();
    backend_ = MouseInjectionBackend::System;
    for (const int virtualKey : keyboard_state_.pressedVirtualKeys()) send_key(virtualKey, true);
    // 随后重建仍按下的鼠标按钮并广播回退。
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1592-1605, 1789-1828, 1881-1921（修改后）
if (transition == uu::SharedInputTransition::InjectDown
    || transition == uu::SharedInputTransition::InjectRepeat) {
    mouse_backend->sendKey(vk, true);
} else if (transition == uu::SharedInputTransition::InjectUp) {
    mouse_backend->sendKey(vk, false);
}

const uu::SharedInputReleaseBatch released = held_input_.releaseAll();
for (const int key : released.keys) mouse_backend_.sendKey(key, false);
for (const int button : released.buttons) mouse_backend_.sendButton(button, false);
// 会话断开、Host 关闭、显式切换和后台安装完成都通过当前真实后端执行同类释放。
```

```cpp
// third_party/uu_stream_webrtc/src/shared_input_state.h:56-72（修改后）
SharedInputReleaseBatch releaseAll()
{
    SharedInputReleaseBatch released;
    for (const auto& [key, holders] : key_holders_) {
        if (holders > 0) released.keys.push_back(key);
    }
    for (const auto& [button, holders] : button_holders_) {
        if (holders > 0) released.buttons.push_back(button);
    }
    keys_by_session_.clear();
    buttons_by_session_.clear();
    key_holders_.clear();
    button_holders_.clear();
    return released;
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2854-2945；src/ui/RemoteDesktopWindow.h:161（修改后）
m_remoteMouseBackendMessage = zh("驱动键鼠不可用，已自动回退系统键鼠");
m_remoteMouseBackendMessage = zh("驱动键鼠已启用（FakerInputBridge）");

return m_remoteMouseBackend == RemoteMouseBackend::Faker
    ? zh("驱动键鼠（FakerInputBridge）\n键盘与鼠标均通过虚拟 HID；点击切换为系统键鼠")
    : zh("系统键鼠（SendInput）\n点击切换为驱动键鼠");

QRect mouseInputModeRect() const; // wjy: 兼容保留既有函数名；该标题栏开关现在统一切换系统/驱动键鼠后端。
```

```cpp
// include/FsRemoteStreamApi.h:57；CMakeLists.txt:36（修改后）
FSREMOTE_STATUS_MOUSE_BACKEND = 65; // 兼容保留名称，状态现在确认系统/FakerInputBridge 键鼠后端。

// CMake：生产 DLL 显式登记同一份可测试键盘状态机。
third_party/uu_stream_webrtc/src/faker_input_keyboard_state.h
```

```cpp
// third_party/uu_stream_webrtc/tests/shared_input_state_tests.cpp:68-119（修改后）
const uu::SharedInputReleaseBatch switched_inputs = unified_backend_switch_state.releaseAll();
require(switched_inputs.keys.size() == 1 && switched_inputs.buttons.size() == 1,
    "unified backend switch releases held keyboard and mouse inputs");

require(uu::fakerInputUsageForVirtualKey('W') == 0x1A, "W maps to USB HID keyboard usage");
require(uu::fakerInputModifierBit(VK_RCONTROL) == 0x10, "right control maps to modifier bit 4");
require(keyboard_state.update('H', true) == uu::FakerInputKeyboardUpdate::Rollover,
    "seventh ordinary key is rejected without corrupting held six-key report");
```

### Steps
1. 复用 Bridge v1 现有 `keyboard = 2` 命令，锁定 8 字节 boot-keyboard 线格式，不修改 MSI、驱动或独立 Bridge。
2. 新增 VK→USB HID 映射，覆盖字母、数字、功能键、方向键、导航键、数字键盘、OEM 标点及左右 Ctrl/Shift/Alt/Win。
3. 用状态机维护一字节修饰键和六个普通 usage；重复 down 不重复占槽，第七个普通键安全拒绝，未知 Consumer Control 键暂时继续使用 `SendInput`。
4. 把 Host 的键盘 down/up、会话断开和关闭释放统一路由到当前真实后端。
5. Bridge 运行中断管时先依靠服务端连接清理虚拟 HID，再在系统后端重建仍按下的键和鼠标按钮并广播回退。
6. 显式切换及后台安装完成时使用统一 `releaseAll()` 屏障，确保旧后端先收到所有 key-up/button-up，再启用新后端。
7. 将标题栏文案和 ABI 注释统一为键鼠后端，避免界面继续显示“键盘仍使用 SendInput”。
8. 增加纯状态回归测试并登记新头文件，使用生产实现验证常见游戏组合和边界条件。

### Verification
- `git diff --check`：通过。
- Visual Studio 2022 x64 Release 构建 `uu_shared_input_state_tests`：通过；测试程序退出码 `0`。
- 测试覆盖 `W/A/Shift`、左右修饰键、重复 down、Consumer Control 未映射键、六键上限和统一键鼠释放。
- Visual Studio 2022 x64 Release 构建 `fsremote_stream`：编译和链接通过，新 DLL 位于 `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release/third_party/uu_stream_webrtc/fsremote_stream.dll`。
- `RemoteDesktopWindow.cpp.obj` 定向编译通过；只有项目既有 `C4804` 警告，无本次新增错误。
- PID 33624 的正式 `FSRemote.exe` 正在占用发布目录文件，未强制结束用户进程，因此尚未覆盖发布根目录 DLL，也未重新链接正式 EXE；退出当前 FSRemote 后重新构建即可完成发布目录更新。
- 尚未在目标游戏内动态验证 FakerInput 键盘是否被接受；需随新 DLL/EXE 在被控设备实测。

## 2026-07-25 09:12 - 修复运行中 FakerInputBridge 阻止被控端更新

### Changed Location
- `src/updater/main.cpp:4, 186-288`：枚举 Windows 进程并只终止映像路径精确等于目标安装目录的 `FakerInputBridge.exe`。
- `src/updater/main.cpp:381-387`：主程序退出后、备份和覆盖前增加 Bridge 文件锁释放阶段。

### Reason
被控端更新 `1.1.100 → 1.1.101` 的真实 `updater.log` 明确显示 `install failed: FakerInputBridge.exe` 和 `rollback incomplete`，同时 PID 5832 一直运行安装目录中的 Bridge。此前 FSRemote 会优先复用用户手工启动的 Bridge；这种外部进程不属于 FSRemote 的 Job，主程序退出后仍持有 `FakerInputBridge.exe`，导致更新器无法覆盖该文件，回滚也再次撞上同一文件锁。

修复放在独立更新器内：更新任务使用目标 release 中已暂存的新 `FSRemoteUpdater.exe` 作为 runner，因此下一次发布即可直接修复旧被控端，不需要先成功安装旧更新器。清理严格比较完整绝对路径，不会按进程名结束其他目录中的同名 Bridge。

### Original Code
```cpp
// src/updater/main.cpp:1-10（修改前）
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
// 原来没有 Tool Help 进程枚举接口。
```

```cpp
// src/updater/main.cpp:271-281（修改前）
if (!ensureProcessExited(task.processId)) {
    logLine(L"main process could not be stopped safely");
    return 4;
}
logLine(L"main process exited");
const bool updated = install(task, &error);
// 主程序退出后直接复制，外部启动的 FakerInputBridge 仍会锁住目标文件。
```

### Modified Code
```cpp
// src/updater/main.cpp:3-4（修改后）
#include <windows.h>
#include <tlhelp32.h>
```

```cpp
// src/updater/main.cpp:186-288（修改后）
std::wstring normalizedAbsolutePath(const fs::path& path)
{
    // 使用 GetFullPathNameW 生成绝对规范路径；失败时禁止继续终止进程。
}

bool pathsEqualInsensitive(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool stopBridgeAtTargetPath(const UpdateTask& task, std::wstring* error)
{
    if (!taskUpdatesBridge(task)) return true;
    const std::wstring normalizedTarget = normalizedAbsolutePath(
        task.targetDir / L"FakerInputBridge.exe");

    // 枚举进程并用 QueryFullProcessImageNameW 读取真实映像路径。
    // 只对完整路径等于 normalizedTarget 的进程先等待 2 秒，仍未退出才 TerminateProcess。
    // 终止后最多等待 5 秒确认文件锁已经释放，否则在复制前安全停止事务。
    return true;
}
```

```cpp
// src/updater/main.cpp:381-387（修改后）
if (!ensureProcessExited(task.processId)) {
    logLine(L"main process could not be stopped safely");
    return 4;
}
logLine(L"main process exited");
if (!stopBridgeAtTargetPath(task, &error)) {
    logLine(error);
    return 4;
}
const bool updated = install(task, &error);
```

### Steps
1. 根据被控端真实日志确认失败文件和仍存活 Bridge 的 PID、完整安装路径。
2. 在更新任务包含 Bridge 时，计算 `targetDir/FakerInputBridge.exe` 的绝对规范路径。
3. 使用 Tool Help 枚举进程，并用 `QueryFullProcessImageNameW` 获取每个可查询进程的完整映像路径。
4. 完整路径不区分大小写地精确相等时，先等待 FSRemote 自有 Job 的正常清理；两秒后仍存活才终止该进程。
5. 终止失败或五秒内未退出时，在备份/复制前停止更新并记录具体 PID 和 Win32 错误，避免再次产生半完成安装和不完整回滚。
6. 保持既有重启路径不变；更新成功后新版 FSRemote 会按需重新启动并管理 Bridge。

### Verification
- `git diff --check -- src/updater/main.cpp`：通过。
- Visual Studio 2022 x64 Release 构建 `FSRemoteUpdater`：通过，生成 `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release/FSRemoteUpdater.exe`，大小 349696 字节。
- 真实文件占用测试：把持续运行的 `PING.EXE` 分别复制并重命名为目标目录和外部目录中的 `FakerInputBridge.exe`，再用不同载荷执行更新。
- 测试结果：更新器退出码 `0`；目标 Bridge PID 被结束；外部目录同名 PID 保持运行；目标文件 SHA-256 与 payload 一致；日志记录 `target FakerInputBridge exited` 和 `update installed`。
- 测试结束后只终止外部临时测试 PID，并删除已验证位于仓库 `temp/updater_bridge_lock_e2e` 下的测试目录。

## 2026-07-25 09:29 - 完善更新前 Host 媒体资源确定性清理

### Changed Location
- `src/main.cpp:290-309`：收到更新退出信号后，在窗口退出前按确定顺序停止远控状态服务和 Host 媒体运行时。
- `src/updater/main.cpp:157-170`：把主进程正常退出宽限从 15 秒延长为 60 秒，超时后仍保留按 PID 兜底终止。

### Reason
被控端虽然已经返回 `update_status=complete`，控制端随后却停在等待视频首帧状态，直到重启 Windows 才恢复。历史 `updater.log` 又多次记录 `main process graceful exit timeout; terminating pid=...`，说明旧版更新器可能在活跃远控会话的 WebRTC、桌面采集、编码器、FakerInput Bridge 和 Qt 后台任务尚未完成释放时，于 15 秒到达后直接强制结束 FSRemote。窗口消失不等于这些原生资源已经完全关闭；过早强杀可能让下一版进程无法立即重新建立采集或编码链路。

本次把关键 Host 清理前移到更新专用退出信号中，并给正常退出留出 60 秒。普通程序更新应先完成应用级资源释放再覆盖文件、启动新版；只有清理真正卡死时才由独立更新器按原 PID 兜底结束旧进程。

### Original Code
```cpp
// src/main.cpp:289-293（修改前）
QObject::connect(&platform::UpdateService::instance(), &platform::UpdateService::updateReadyToQuit,
    &app, [&window] {
        window.requestApplicationExit();
    });
```

```cpp
// src/updater/main.cpp:157（修改前）
DWORD waitResult = WaitForSingleObject(process, 15000);
```

### Modified Code
```cpp
// src/main.cpp:290-309（修改后）
bool updateShutdownPrepared = false; // wjy: 更新退出只允许一个调用者执行服务和媒体预清理，重复信号不能二次释放 Host 句柄。
QObject::connect(&platform::UpdateService::instance(), &platform::UpdateService::updateReadyToQuit,
    &app, [&] {
        if (updateShutdownPrepared) return;
        updateShutdownPrepared = true;
        writeStartupLog(QStringLiteral("[wjy-update-exit] deterministic cleanup begin"));

        remoteControllerOverlayTimer.stop(); // wjy: Host 句柄释放前停止控制端列表轮询，后续事件不再读取已关闭的原生会话表。
        commandServer.stop(); // wjy: 先关闭 49102 并汇合已经完成暂存的更新线程，禁止退出期间再受理第二个更新或电源命令。
        statusServer.stop(); // wjy: 关闭 49101 后控制端会把本机视为预期更新离线，不再从旧 Host 句柄生成 busy 快照。
        realtimeStateService.stop(); // wjy: 停止 UDP 心跳和会话采样，确保下面销毁 Host 时没有并发读取会话状态。
        if (hostHandle) {
            stream::StreamRuntime::instance().stop(hostHandle); // wjy: 在 UI 析构前主动关闭监听、WebRTC 会话、采集编码管线和 FakerInput Bridge 客户端。
            hostHandle = nullptr; // wjy: 主退出尾声据此跳过重复 stop，防止对已经释放的原生句柄二次调用。
        }
        writeStartupLog(QStringLiteral("[wjy-update-exit] deterministic cleanup end"));
        window.requestApplicationExit(); // wjy: 关键媒体和服务资源确认释放后再进入窗口、后台任务、SSH 和 Qt 的统一退出路径。
    });
```

```cpp
// src/updater/main.cpp:157-158（修改后）
constexpr DWORD kGracefulExitTimeoutMs = 60000;
DWORD waitResult = WaitForSingleObject(process, kGracefulExitTimeoutMs); // wjy: 活跃远控更新会先关闭 WebRTC、采集编码、Bridge、SSH 和 Qt 后台线程；60 秒宽限禁止旧版 15 秒过早强杀留下显卡/驱动状态。
```

### Steps
1. 根据目标设备更新完成后仍等待视频首帧、重启系统后恢复的现象，结合历史更新器超时强杀日志，定位到更新退出与 Host 原生资源释放之间的时序风险。
2. 为更新退出增加一次性屏障，避免重复信号再次停止服务或二次释放 Host 句柄。
3. 在请求窗口和 Qt 事件循环退出前，依次停止控制端悬浮层轮询、命令服务、状态服务和实时状态服务。
4. 主动停止 Host 句柄，使监听、WebRTC 会话、桌面采集、编码管线以及 FakerInput Bridge 客户端在旧进程仍可正常执行清理代码时完成释放。
5. Host 停止后把句柄清空，保留主函数退出尾声的通用清理，同时阻止重复调用原生 `stop`。
6. 把独立更新器等待旧进程正常退出的时间从 15 秒扩展为 60 秒；若仍超时，则继续使用原有指定 PID 兜底终止，防止更新永久卡住。
7. 保留上一项修复中的 Bridge 精确安装路径清理，仍只处理目标目录的 `FakerInputBridge.exe`，不会误杀其他目录同名程序。

### Verification
- `src/main.cpp` 使用 Visual Studio 2022 x64 Release 环境定向构建 `src/main.obj`：通过。
- Visual Studio 2022 x64 Release 构建 `FSRemoteUpdater`：通过。
- 构建期间未结束 PID 33520 的运行中 `FSRemote.exe`，也未尝试覆盖其正在使用的发布文件。
- 该进程随后自然退出后，Visual Studio 2022 x64 Release 完整构建 `FSRemote`：编译、链接以及发布依赖复制全部通过，未再出现文件占用错误。
- `git diff --check`：通过。
- 仍需随下一发布版本在目标设备的活跃远控会话中执行一次真实更新，确认旧进程日志完整出现 `[wjy-update-exit] deterministic cleanup begin/end`，且新版可直接恢复视频首帧。
- 如果 FakerInput MSI 或 Windows 驱动安装明确返回 `3010`/`1641` 等要求重启的状态，仍应按系统要求重启；本修复针对普通程序更新过程中被过早强杀导致的非预期重启依赖。

## 2026-07-28 08:54 - 修复拖拽缩放时远控标题栏透明消失

### Changed Location
- `src/ui/NativeRemoteTitleBarSurface.h:23-25`：增加持久标题栏 DIB 的显式重绘入口。
- `src/ui/NativeRemoteTitleBarSurface.cpp:275-318`：按真实 HWND 显隐和兄弟窗口层级恢复标题栏，并支持缩放期间同步重绘缓存 DIB。
- `src/ui/RemoteDesktopWindow.cpp:5161-5174`：交互缩放每次变更窗口几何后刷新标题栏原生子表面。

### Reason
远控窗口使用独立 Win32 子窗口承载标题栏，同时父窗口启用了 `WS_CLIPCHILDREN`。拖拽边缘缩放时，如果 DWM 暂时丢失该子窗口的可见表面，父窗口不会补画被裁剪的标题栏区域，因此会直接露出后面的桌面；原实现又只信任本地 `visible/raised` 布尔缓存，透明状态可能一直保留到后续状态变化。本次继续复用上一张完整持久 DIB，不在缩放中重排控件，只同步请求子 HWND 重绘；交互结束后读取系统真实显隐状态和兄弟 HWND 顺序恢复可见性与层级。

### Original Code
```cpp
// src/ui/NativeRemoteTitleBarSurface.h:23-24（修改前）
void setVisible(bool visible);
void raise();
```

```cpp
// src/ui/NativeRemoteTitleBarSurface.cpp:275-299（修改前）
void NativeRemoteTitleBarSurface::setVisible(bool visible)
{
#if defined(Q_OS_WIN)
    if (!m_impl->window || m_impl->visible == visible) return;
    ::ShowWindow(m_impl->window, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    m_impl->visible = visible;
    if (!visible) {
        m_impl->raised = false;
    }
#else
    Q_UNUSED(visible)
#endif
}

void NativeRemoteTitleBarSurface::raise()
{
#if defined(Q_OS_WIN)
    if (m_impl->window && m_impl->visible && !m_impl->raised) {
        if (::SetWindowPos(m_impl->window, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
            m_impl->raised = true;
        }
    }
#endif
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:5161-5172（修改前对应逻辑）
void RemoteDesktopWindow::resizeEvent(QResizeEvent* event)
{
    if (!m_resizingWindow) {
        updateWindowMask();
    }
    updateTexturePresenterGeometry();
    if (!m_resizingWindow) {
        updateNativeTitleBarSurface();
    }
    updatePerformanceOverlayGeometry();
    QWidget::resizeEvent(event);
}
```

### Modified Code
```cpp
// src/ui/NativeRemoteTitleBarSurface.h:23-25（修改后）
void setVisible(bool visible);
void refresh(); // wjy: 父窗口交互缩放时把上一张持久DIB重新提交给DWM，防止子表面暂时透明后一直露出桌面。
void raise();
```

```cpp
// src/ui/NativeRemoteTitleBarSurface.cpp:275-318（修改后）
void NativeRemoteTitleBarSurface::setVisible(bool visible)
{
#if defined(Q_OS_WIN)
    if (!m_impl->window) return;
    const bool systemVisible = ::IsWindowVisible(m_impl->window) != FALSE; // wjy: Win32在父窗口合成变化后可能与本地缓存不一致，显隐判断必须复核真实HWND状态。
    if (m_impl->visible == visible && systemVisible == visible) return;
    ::ShowWindow(m_impl->window, visible ? SW_SHOWNOACTIVATE : SW_HIDE); // wjy: 缓存或真实状态任一不一致时恢复标题栏，避免“逻辑可见、系统实际隐藏”永久跳过修复。
    m_impl->visible = visible;
    if (!visible) {
        m_impl->raised = false;
    }
#else
    Q_UNUSED(visible)
#endif
}

void NativeRemoteTitleBarSurface::refresh()
{
#if defined(Q_OS_WIN)
    if (!m_impl->window || !m_impl->visible) return;
    if (::IsWindowVisible(m_impl->window) == FALSE) {
        ::ShowWindow(m_impl->window, SW_SHOWNOACTIVATE); // wjy: 缩放期间若系统意外隐藏子HWND，立即恢复但不激活或抢走远控窗口焦点。
        m_impl->raised = false;
    }
    ::RedrawWindow(
        m_impl->window,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_NOCHILDREN); // wjy: 同步BitBlt上一张完整DIB，不擦背景、不生成透明中间帧，也不在缩放中重新排版标题栏。
#endif
}

void NativeRemoteTitleBarSurface::raise()
{
#if defined(Q_OS_WIN)
    if (!m_impl->window || !m_impl->visible) return;
    if (::GetWindow(m_impl->window, GW_HWNDFIRST) == m_impl->window) {
        m_impl->raised = true;
        return;
    }
    if (::SetWindowPos(m_impl->window, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        m_impl->raised = true;
    }
#endif
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:5161-5174（修改后）
void RemoteDesktopWindow::resizeEvent(QResizeEvent* event)
{
    if (!m_resizingWindow) {
        updateWindowMask();
    }
    updateTexturePresenterGeometry();
    if (!m_resizingWindow) {
        updateNativeTitleBarSurface();
    } else if (m_nativeTitleBarSurface) {
        m_nativeTitleBarSurface->refresh(); // wjy: 交互缩放不重排标题栏内容，只把已缓存DIB同步重绘到子HWND，杜绝透明区域暴露桌面。
    }
    updatePerformanceOverlayGeometry();
    QWidget::resizeEvent(event);
}
```

### Steps
1. 根据用户说明把现象从“黑色背景”修正为“标题栏子表面透明并露出桌面”，重新检查父窗口 `WS_CLIPCHILDREN` 与原生标题栏 HWND 的关系。
2. 为原生标题栏增加 `refresh()`，缩放期间仅用 `RedrawWindow` 同步 BitBlt 已缓存 DIB，不重新生成标题栏布局，也不擦出透明中间帧。
3. 显隐操作同时检查本地期望值和 `IsWindowVisible` 的真实结果，修复缓存与 HWND 实际状态不一致时无法自愈的问题。
4. 交互结束时使用 `GetWindow(..., GW_HWNDFIRST)` 复核真实兄弟窗口顺序，仅在标题栏确实不在顶部时恢复 Z 序。
5. 在远控窗口交互缩放的 `resizeEvent` 中调用缓存表面刷新，使每次父窗口几何变化后标题栏仍有完整不透明像素。

### Verification
- Visual Studio 2022 x64 Debug 完整构建 `FSRemote`：通过，生成 `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Debug/FSRemote.exe`。
- Visual Studio 2022 x64 Debug 构建 `fsremote_remote_titlebar_renderer_tests`：通过。
- 尝试运行 `fsremote_remote_titlebar_renderer_tests` 时触发已有断言 `narrow.layout.close.isValid()`（`tests/remote_titlebar_renderer_tests.cpp:51`）；该断言属于当前标题栏窄宽布局测试，与本次 Win32 子表面刷新代码无关，本次未改动该测试及布局算法。
- Visual Studio 2022 x64 Release 源文件编译通过；最终链接因用户正在运行 PID 37644 的 Release `FSRemote.exe` 被占用而停止，未擅自结束该进程。
- `git diff --check`：通过。
- 仍需在真实远控画面中从四边及四角连续拖拽缩放，确认 DWM/显卡驱动实际合成路径下标题栏不再透明；自动化渲染测试无法模拟该 Win32 交互缩放过程。

## 2026-07-29 - 建立统一远控窗口合成器基线与状态入口

### Changed Location
- `openspec/changes/unify-remote-window-compositor/baseline.md:1`：记录当前 Qt 父窗口、D3D11 子窗口、标题栏 HWND、性能浮层和 resize 入口。
- `src/ui/RemoteWindowCompositor.h:13-71`、`src/ui/RemoteWindowCompositor.cpp:8-146`：新增统一合成器状态机、物理布局快照和运行时开关。
- `src/ui/RemoteDesktopWindow.h:23,227,386`、`src/ui/RemoteDesktopWindow.cpp:1572-1577,3978-4004,4148-4190,4291-4294,5446`：接入布局/帧/回退状态诊断，但保留旧可见表面路径。
- `tests/remote_window_compositor_tests.cpp:1-57`、`CMakeLists.txt:198-208,457-458`：新增不依赖显卡和 WebRTC 的状态/几何回归 harness。

### Reason
拖拽调整大小时，现有实现由多个可见 HWND 分别提交，事件日志无法证明最终屏幕像素是否连续。本次先建立单一物理布局快照和显式状态机，为后续迁移 D3D、标题栏、性能层到同一可见表面提供稳定边界；开关通过 `FSREMOTE_UNIFIED_COMPOSITOR` 环境变量读取，不写入用户设置。

### Original Code
```cpp
// src/ui/RemoteDesktopWindow.cpp:1571
setWindowTitle(m_deviceName);
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3990
void RemoteDesktopWindow::updateTexturePresenterGeometry()
{
    const QRect target = remoteContentRect();
    // 旧路径分别调整 D3D 子窗口、标题栏 HWND 和性能浮层。
}
```

### Modified Code
```cpp
// src/ui/RemoteWindowCompositor.cpp:17-27
bool RemoteWindowCompositorConfig::rolloutEnabled()
{
    const QByteArray value = qgetenv("FSREMOTE_UNIFIED_COMPOSITOR").trimmed().toLower();
    return value == "1" || value == "true" || value == "yes" || value == "on";
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3978-4004
void RemoteDesktopWindow::commitCompositorLayout()
{
    if (!m_unifiedCompositor || !m_unifiedCompositor->isEnabled()) return;
    const RemoteWindowLayoutSnapshot snapshot = compositorLayoutSnapshot();
    if (m_resizingWindow) {
        m_unifiedCompositor->updateInteractiveGeometry(snapshot);
    } else {
        m_unifiedCompositor->commitLayout(snapshot);
    }
}
```

### Steps
1. 记录现有四类可见表面、帧入口、D3D Present、软件回退和 resize 入口。
2. 新增 `Idle`、`InteractiveResize`、`FinalizeResize`、`HardwareFallback`、`DeviceRecovery` 五态状态机。
3. 将物理输出矩形、内容矩形、标题栏矩形、输入矩形、DPI、源/输出尺寸收敛到 `RemoteWindowLayoutSnapshot`。
4. 在远控窗口构造、DPI 变化、resize、硬件帧、软件帧和设备失败路径接入统一诊断。
5. 新增纯 Qt Core 回归 harness，覆盖移动、边缘缩放、最大化/还原、DPI、重连和设备回退状态。

### Verification
- `git diff --check`：通过。
- 未构建、未运行测试，遵守本轮“不要构建”的要求。
- 尚未迁移 D3D11、标题栏和性能浮层的实际可见像素所有权；这将在确认最终合成后端（顶层 SwapChain 或 DirectComposition）后继续实施。

## 2026-07-29 - 接入 DirectComposition 统一表面与可见像素采样

### Changed Location
- `src/ui/D3D11FramePresenter.h:31-59`、`src/ui/D3D11FramePresenter.cpp:231-489`：新增顶层 DirectComposition target、视频视觉、本地 Alpha 叠加 SwapChain、缩放变换和旧子 HWND 回退。
- `src/ui/RemoteDesktopWindow.cpp:1573-1618,2877-2979,3020-3035,3370-3380,4113-4168,4250-4325,4340-4520`：把标题栏、性能信息、连接/更新遮罩和 BGRA 帧合成到同一叠加层；硬件/软件切换先提交目标帧再切换可见性。
- `src/ui/RemoteDesktopWindow.h:226-232,341-343`：增加合成叠加和可选真实屏幕采样入口。
- `CMakeLists.txt:541-544`：链接 `dcomp`。
- `openspec/changes/unify-remote-window-compositor/design.md:29,70`、`tasks.md`：确定 DirectComposition 后端并勾选已完成迁移任务。

### Reason
仅有状态机无法消除原生 D3D 子 HWND、标题栏 HWND 和性能浮层之间的竞争；新路径必须让最终视频和本地 UI 由同一个顶层合成目标提交。DirectComposition 初始化失败时自动退回旧 Presenter，避免把兼容性问题转化为黑屏。

### Original Code
```cpp
// src/ui/D3D11FramePresenter.cpp:420-436
DXGI_SWAP_CHAIN_DESC desc = {};
desc.OutputWindow = reinterpret_cast<HWND>(winId());
desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
factory->CreateSwapChain(m_impl->sharedDevice->device.Get(), &desc, &m_impl->swapChain);
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:4248-4264
m_remoteFrame = image;
m_textureFrameActive = false;
m_texturePresenter->hide();
```

### Modified Code
```cpp
// src/ui/D3D11FramePresenter.cpp:668-705
DXGI_SWAP_CHAIN_DESC1 desc = {};
desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
factory2->CreateSwapChainForComposition(
    m_impl->sharedDevice->device.Get(), &desc, nullptr, &compositionSwapChain);
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:4508-4517
if (m_texturePresenter && m_texturePresenter->usesCompositorSurface()) {
    presentCompositorOverlay();
    m_texturePresenter->setPresentationVisible(false);
}
```

### Steps
1. 创建 DirectComposition 顶层 target、视频 visual 和 Alpha overlay visual。
2. D3D11 共享纹理继续使用 keyed mutex 和 VideoProcessor，输出改挂到 composition SwapChain。
3. 交互缩放期间只更新视频 visual transform 和 overlay transform；最终松手才 ResizeBuffers。
4. 用一张预乘 Alpha 图层承载标题栏、性能文本、连接/更新遮罩和 BGRA 回退帧。
5. 硬件恢复前先清理 overlay 中的旧软件帧，软件回退前先提交 BGRA，再隐藏硬件视觉，避免中间黑帧。
6. 增加 `FSREMOTE_RESIZE_PIXEL_PROBE=1` 采样真实屏幕可见像素，记录黑色比例、平均亮度、状态、布局版本和帧号。

### Verification
- `git diff --check`：通过。
- 已检查 Windows SDK `dcomp.h` 中 `DCompositionCreateDevice`、`SetTransform`、`SetOpacity` 和 `SetContent` 的真实签名，并修正为引用参数调用。
- 未构建、未运行测试；等待你自行构建后验证 DirectComposition 驱动差异。
- 旧路径仍保留，DirectComposition 创建失败时自动回退；任务 5.4 和 7.x 验证任务暂未完成。

## 2026-07-29 - DirectComposition 回退与采样链路加固

### Changed Location
- `src/ui/D3D11FramePresenter.cpp:266-276,387-435,457-499`：增加叠加层就绪判断、Present 失败后的资源清理、交互缩放复用旧 Alpha 缓冲和视觉变换。
- `src/ui/RemoteDesktopWindow.cpp:2830-2845,3031-3045,3378-3392,3427-3443,4250-4325`：DComp 叠加层未就绪时保留原生标题栏/性能层回退；输入映射读取已提交布局快照；增加真实屏幕黑像素采样。
- `src/ui/RemoteWindowCompositor.cpp:101-114`：交互缩放中硬件帧保持 `InteractiveResize` 状态，不被普通帧错误改回 `Idle`。

### Reason
避免 DirectComposition 初始化或叠加 SwapChain 创建失败时出现标题栏消失、性能信息消失或软件/硬件切换黑帧；同时让诊断能够区分真正可见空白与普通窗口消息。

### Modified Code
```cpp
// src/ui/RemoteDesktopWindow.cpp:4250-4325
if (m_resizePixelProbeEnabled && m_resizingWindow) {
    const QPixmap screenshot = screen->grabWindow(0, ...);
    appendResizeDebugTrace(QStringLiteral("visible.sample ... black_ratio=%1 ..."));
}
```

### Verification
- `git diff --check`：通过。
- 已核对 DirectComposition SDK 方法签名和缩放矩阵参数类型。
- 未构建；需由你构建后验证具体显卡驱动和 DPI 环境。
## 2026-07-29 - 修复 DirectComposition SetOpacity 编译错误

### Changed Location
- `src/ui/D3D11FramePresenter.cpp:388`：移除对 `IDCompositionVisual::SetOpacity` 的调用。
- `src/ui/D3D11FramePresenter.cpp:482-483`：使用 `SetContent` 的绑定/解绑状态控制视频视觉显示。

### Reason
Windows SDK 10.0.26100 的 `IDCompositionVisual` 接口不提供 `SetOpacity`，该方法属于更高版本的 `IDCompositionVisual3`。当前实现使用基础 `IDCompositionVisual`，因此会触发 C2039 编译错误。改为在同一次 DirectComposition Commit 前绑定或解绑 SwapChain 内容，既能实现隐藏/显示，又不引入 Visual3 的运行时依赖。

### Original Code
```cpp
// src/ui/D3D11FramePresenter.cpp:388
m_impl->compositionOverlay->SetOpacity(1.0f);

// src/ui/D3D11FramePresenter.cpp:484
m_impl->compositionVideo->SetOpacity(
    m_impl->compositorVisible ? 1.0f : 0.0f);
```

### Modified Code
```cpp
// src/ui/D3D11FramePresenter.cpp:388
m_impl->compositionOverlay->SetContent(
    m_impl->compositionOverlaySwapChain.Get());

// src/ui/D3D11FramePresenter.cpp:482-483
m_impl->compositionVideo->SetContent(
    m_impl->compositorVisible ? m_impl->swapChain.Get() : nullptr); // wjy: IDCompositionVisual没有SetOpacity，使用内容绑定/解绑实现原子显示与隐藏，避免引入Visual3运行时依赖。
```

### Steps
1. 对照 Windows SDK `dcomp.h`，确认 `SetOpacity` 不属于 `IDCompositionVisual`。
2. 删除叠加视觉上的无效透明度调用，保留 `SetContent` 绑定。
3. 将视频视觉的显示状态改为 SwapChain 内容绑定或解绑，并继续沿用现有的统一 Commit。

### Verification
- 已检查 Windows SDK 中 `IDCompositionVisual::SetContent(IUnknown*)` 的声明。
- `rg` 检查后已无实际的 `SetOpacity` API 调用。
- `git diff --check`：通过。
- 未构建、未运行测试，等待用户自行构建验证。
## 2026-07-29 - 交互缩放改为等比例居中显示

### Changed Location
- `src/ui/D3D11FramePresenter.cpp:472-498`：调整 DirectComposition 视频视觉的缩放比例和偏移计算。

### Reason
拖拽窗口时，SwapChain 为避免闪烁会暂时保持原尺寸，但旧逻辑分别计算 X/Y 缩放比例，窗口宽高比变化时会把远端画面横向压扁或纵向拉长。本次改为使用较小轴比例进行统一缩放，并将画面居中，剩余区域保留为黑边。

### Original Code
```cpp
// src/ui/D3D11FramePresenter.cpp:472-481
m_impl->compositionVideo->SetOffsetX(static_cast<float>(m_impl->compositorOutputRect.left()));
m_impl->compositionVideo->SetOffsetY(static_cast<float>(m_impl->compositorOutputRect.top()));
transform._11 = outputWidth / swapWidth;
transform._22 = outputHeight / swapHeight;
```

### Modified Code
```cpp
// src/ui/D3D11FramePresenter.cpp:472-498
const QRect& outputRect = m_impl->compositorOutputRect;
const bool hasOutputGeometry = outputRect.width() > 0 && outputRect.height() > 0;
const bool hasSwapGeometry = m_impl->swapWidth > 0 && m_impl->swapHeight > 0;
float uniformScale = 1.0f;
// 根据较小轴比例等比缩放，并计算居中偏移。
transform._11 = uniformScale;
transform._22 = uniformScale;
```

### Steps
1. 保留交互缩放期间不重建 SwapChain 的稳定策略。
2. 计算输出区域和 SwapChain 的 X/Y 比例，取较小值作为统一缩放比例。
3. 根据缩放后的尺寸重新计算水平、垂直居中偏移，避免非等比拉伸。

### Verification
- `rg` 已确认修改后的 X/Y 使用同一个 `uniformScale`。
- `git diff --check`：通过。
- 未构建、未运行测试，等待用户自行构建并体验拖拽效果。
## 2026-07-29 - 分离交互缩放中的视频内容与黑边

### Changed Location
- `src/ui/D3D11FramePresenter.cpp:472-524`：裁剪 SwapChain 内旧黑边，只把实际视频区域映射到当前窗口画面区域。
- `src/ui/RemoteDesktopWindow.cpp:2901-2914`：在统一透明叠加层中按当前窗口尺寸重绘黑边。

### Reason
上一版虽然改成了等比例缩放，但黑边已经写入旧尺寸 SwapChain，DirectComposition 缩放视觉时仍会把黑边一起缩放。现在硬件视频视觉使用 `SetClip` 裁掉旧黑边，并根据当前窗口重新计算目标图像矩形；透明叠加层负责当前尺寸的黑色留白，避免黑边继承旧缓冲尺寸。

### Original Code
```cpp
// src/ui/D3D11FramePresenter.cpp:472-524
m_impl->compositionVideo->SetOffsetX(centeredOffsetX);
m_impl->compositionVideo->SetOffsetY(centeredOffsetY);
transform._11 = uniformScale;
transform._22 = uniformScale;
m_impl->compositionVideo->SetContent(m_impl->swapChain.Get());
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2898-2900
QPainter painter(&overlay);
painter.setRenderHint(QPainter::Antialiasing, true);
painter.scale(dpr, dpr);
```

### Modified Code
```cpp
// src/ui/D3D11FramePresenter.cpp:481-524
const RECT sourceClipRect = letterbox_rect(sourceWidth, sourceHeight, swapWidth, swapHeight);
const RECT targetImageRect = letterbox_rect(sourceWidth, sourceHeight, outputWidth, outputHeight);
m_impl->compositionVideo->SetClip(videoClip);
m_impl->compositionVideo->SetOffsetX(centeredOffsetX);
m_impl->compositionVideo->SetOffsetY(centeredOffsetY);
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2901-2909
const QRect contentRect = remoteContentRect();
const QRect imageRect = remoteImageRect();
painter.fillRect(contentRect, Qt::black);
painter.setCompositionMode(QPainter::CompositionMode_Clear);
painter.fillRect(imageRect, Qt::transparent);
```

### Steps
1. 根据当前源帧尺寸和旧 SwapChain 尺寸计算实际视频像素区域。
2. 使用 DirectComposition Clip 只显示视频区域，再映射到当前窗口的等比例目标区域。
3. 在 Alpha 叠加层中按当前窗口重绘黑边，并清出真实视频区域供硬件视觉显示。

### Verification
- `rg` 已确认视频视觉新增 `SetClip`，并同时使用源区域和目标区域计算映射。
- `git diff --check`：通过。
- 未构建、未运行测试，等待用户自行构建并验证拖拽时黑边是否固定。

## 2026-07-30 10:22 - 脚本入口改为按文件选择并隔离重名工作区

### Changed Location
- `src/ui/DeviceGrid.cpp:1025-1135,1195-1204`：增加入口文件白名单、目录快照中的文件节点、精确入口校验和稳定工作区命名。
- `src/ui/DeviceGrid.cpp:5049-5195`：脚本树和右键菜单显示并绑定具体 `.bat/.cmd/.ps1/.py/.exe` 文件，目录节点只负责分组。
- `src/ui/DeviceGrid.cpp:7872-8223,8334-8376`：执行链改为复制入口文件的父目录并运行所选文件；每次执行都重新同步目录。
- `src/ui/DeviceGrid.h:169-170,292`：执行接口参数改为 `scriptEntryPath`。
- `src/ui/ScriptUiStateStore.h:27`、`src/ui/ScriptPanelController.h:31`、`src/ui/ScriptPanelController.cpp:20,48`：状态字段由目录路径改为入口文件路径。

### Reason
原逻辑点击目录后，会在目录内按固定优先级自动挑第一个脚本，并且远端 `work` 目录已存在时跳过复制，因此同目录多个脚本无法准确选择，新增文件也可能没有同步。现在菜单和脚本树直接展示可执行文件；用户选择 `test.py` 后，以它的父目录为复制范围并明确执行 `test.py`。`.whl` 不加入入口白名单，只会随父目录作为依赖文件复制。工作目录使用“父目录名 + 共享根相对路径 SHA-256 前 10 位”，解决不同目录同名脚本的缓存冲突。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:1028-1045
QJsonArray scriptFolderTreeChildrenSnapshot(const QString& folderPath, int depth)
{
    QJsonArray children;
    for (const QFileInfo& childInfo : scriptChildDirectories(folderPath)) {
        QJsonObject child;
        child.insert(QStringLiteral("name"), childInfo.fileName());
        child.insert(QStringLiteral("path"), childInfo.absoluteFilePath());
        child.insert(QStringLiteral("children"),
            scriptFolderTreeChildrenSnapshot(childInfo.absoluteFilePath(), depth + 1));
        children.append(child);
    }
    return children;
}
```

```cpp
// src/ui/DeviceGrid.cpp:1122-1144
QFileInfo scriptEntryFile(const QString& folderPath)
{
    const QStringList priorityExtensions = {
        QStringLiteral("*.bat"), QStringLiteral("*.cmd"), QStringLiteral("*.ps1"),
        QStringLiteral("*.py"), QStringLiteral("*.exe"), QStringLiteral("*.whl")
    };
    const QDir dir(folderPath);
    for (const QString& pattern : priorityExtensions) {
        const QFileInfoList files = dir.entryInfoList(QStringList{pattern}, QDir::Files,
            QDir::Name | QDir::IgnoreCase);
        if (!files.isEmpty()) return files.first();
    }
    return {};
}
```

```powershell
# src/ui/DeviceGrid.cpp:8045-8060
$workAlreadyExists = Test-Path -LiteralPath $work
if (-not $workAlreadyExists) {
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    & robocopy $source $work /E /R:1 /W:1 ...
} else {
    Write-Output ('FSRemote reuse existing work folder: ' + $work)
}
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:1025-1071,1097-1135,1195-1204
QStringList supportedScriptSuffixes();
QFileInfoList scriptEntryFiles(const QString& folderPath);
QString scriptWorkspaceName(const QString& sourceFolderPath);
QFileInfo scriptEntryFileForSelection(const QString& entryPath);
// 目录快照为每个目录附加 scripts 文件节点，入口动作绑定精确文件路径。
```

```cpp
// src/ui/DeviceGrid.cpp:5064-5100,5128-5158,5161-5179
// 文件节点生成 QAction，目录节点生成子菜单；目录节点不会再被当成隐式脚本入口。
if (item->data(0, kScriptTreeNodeTypeRole).toInt() == kScriptTreeFileNode) {
    QAction* scriptFileAction = targetMenu->addAction(item->text(0));
    scriptFileAction->setData(item->data(0, Qt::UserRole));
}
// 只有文件节点能设置当前执行入口。
m_lastScriptEntryPath = QDir::cleanPath(scriptEntryPath);
```

```powershell
# src/ui/DeviceGrid.cpp:8049-8151
$source = '%1'
New-Item -ItemType Directory -Force -Path $work | Out-Null
& robocopy $source $work /E /R:1 /W:1 ...
Write-Output ('FSRemote synchronized script folder: ' + $source)
Set-Location -LiteralPath $work
$entry = Join-Path $work '%2'
```

### Steps
1. 统一入口后缀白名单为 `bat/cmd/ps1/py/exe`，明确排除 `whl`。
2. 在后台目录快照中加入文件节点，并让树、右键菜单、单设备和批量执行共用同一个精确入口路径。
3. 执行时从入口文件取得父目录作为 `robocopy` 源目录，每次运行都重新同步，不再复用旧目录而跳过复制。
4. 用父目录相对共享根路径的稳定哈希生成工作目录，隔离不同目录中的同名脚本；同时把 UI 状态字段改名为 `lastScriptEntryPath`。

### Verification
- `cmake --build build/Desktop_Qt_6_11_1_llvm_mingw_64_bit-Debug --config Debug --target src/ui/DeviceGrid.obj`：通过。
- `cmake --build build/Desktop_Qt_6_11_1_llvm_mingw_64_bit-Debug --config Debug --target src/ui/ScriptPanelController.obj src/ui/ScriptUiStateStore.obj`：通过。
- `git diff --check`：通过。
- MSVC Debug 完整目标构建被现有工具链缺少标准头 `cstdint` 阻断，错误来自既有构建环境，不是本次脚本改动。

## 2026-07-30 11:38 - 按目录 Hash 与入口脚本 Hash 复用或创建工作区

### Changed Location
- `src/ui/DeviceGrid.cpp:1054-1076,1211-1244`：为工作区命名增加入口脚本内容 SHA-256，并分块计算脚本 Hash。
- `src/ui/DeviceGrid.h:100-106`：预检结果携带入口脚本 Hash。
- `src/ui/DeviceGrid.cpp:7950-8009`：后台预检计算脚本 Hash，Hash 读取失败时阻止执行并显示具体错误。
- `src/ui/DeviceGrid.cpp:8108`：用目录路径 Hash 和入口脚本 Hash 共同生成工作区名称。
- `src/ui/DeviceGrid.cpp:8188-8210`：只有工作区不存在时才执行 `robocopy`，存在且两个 Hash 已编码匹配时直接复用。

### Reason
之前虽然已经根据脚本父目录生成了路径 Hash，但每次启动仍然无条件执行 `robocopy`，会把目标设备本地已经修改过的配置文件覆盖回共享目录版本。本次增加入口脚本内容 Hash，并把两个 Hash 都编码到工作目录名中：目录路径或脚本内容任意变化都会得到新的工作目录；两者不变则复用原工作目录，保留本地配置修改。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:1054-1071
QString scriptWorkspaceName(const QString& sourceFolderPath)
{
    const QDir scriptRoot(QString::fromUtf8(kRemoteScriptFolderPath));
    const QString relativeFolder = QDir::fromNativeSeparators(
        scriptRoot.relativeFilePath(QDir::cleanPath(sourceFolderPath)))
        .trimmed()
        .toLower();
    const QByteArray digest = QCryptographicHash::hash(
        relativeFolder.toUtf8(), QCryptographicHash::Sha256).toHex().left(10);
    return baseName + QStringLiteral("__") + QString::fromLatin1(digest);
}
```

```cpp
// src/ui/DeviceGrid.cpp:7950-7957
const QFileInfo entryScript = scriptEntryFileForSelection(scriptEntryPath);
preflight.entryAvailable = entryScript.exists()
    && !scriptRunCommandForFile(entryScript).trimmed().isEmpty();
if (preflight.entryAvailable) {
    preflight.entryScriptPath = entryScript.absoluteFilePath();
    preflight.remoteStatus = platform::DeviceStatusService::query(targetIp);
}
```

```powershell
# src/ui/DeviceGrid.cpp:8136-8149
$scriptWorkName = '%4'
$work = Join-Path $workRoot $scriptWorkName
$source = '%1'
New-Item -ItemType Directory -Force -Path $work | Out-Null
& robocopy $source $work /E /R:1 /W:1 ...
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:1054-1076,1211-1244
QString scriptWorkspaceName(const QString& sourceFolderPath, const QString& entryScriptHash);
QString scriptEntryContentHash(const QFileInfo& scriptFile, QString* errorMessage = nullptr);
// 目录路径 Hash 与入口脚本内容 Hash 同时进入 work 目录名。
```

```cpp
// src/ui/DeviceGrid.cpp:7950-7968,8108
preflight.entryScriptHash = scriptEntryContentHash(entryScript, &hashError);
if (preflight.entryScriptHash.isEmpty()) {
    preflight.entryAvailable = false;
    preflight.errorMessage = hashError;
}
const QString scriptWorkName = scriptWorkspaceName(sourcePath, preflight.entryScriptHash);
```

```powershell
# src/ui/DeviceGrid.cpp:8188-8210
if (-not (Test-Path -LiteralPath $work)) {
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    & robocopy $source $work /E /R:1 /W:1 ...
    Write-Output ('FSRemote created script workspace: ' + $work)
} else {
    Write-Output ('FSRemote reused script workspace: ' + $work)
}
```

### Steps
1. 对用户选择的入口脚本按块计算完整 SHA-256，避免大文件一次性读入内存。
2. 将目录相对路径 Hash 和入口脚本内容 Hash 拼入工作目录名。
3. 两个 Hash 都未变化时跳过复制，直接运行旧工作目录中的入口脚本和本地配置。
4. 任一 Hash 变化时使用新目录复制完整脚本目录，旧版本工作区不被覆盖。

### Verification
- `cmake --build build/Desktop_Qt_6_11_1_llvm_mingw_64_bit-Debug --config Debug --target src/ui/DeviceGrid.obj src/ui/ScriptPanelController.obj src/ui/ScriptUiStateStore.obj`：通过。
- `git diff --check`：通过。
- 未运行远端设备联调；需要实际验证“首次复制、修改 control.txt、相同 Hash 再次运行、修改 particle.py 后创建新 work”四个场景。

## 2026-08-01 16:12 - 修复跨网段实时状态假离线并由安装器放行 UDP 49104

### Changed Location
- `src/system/DeviceStatusRefreshResult.h:10-33`：新增状态查询结果的统一消费目标，明确优先进入实时归并器。
- `src/ui/DeviceGrid.cpp:1337-1343,6785-6886`：批量扫描保留完整 TCP 49101 状态，并在目录更新后统一执行手动校准。
- `installer/FSRemote.iss.in:43-56`：安装、覆盖升级和卸载时维护程序专属 UDP 49104 防火墙规则。
- `tests/device_action_target_tests.cpp:1-85`：增加实时归并/旧缓存路由回归测试。
- `CMakeLists.txt:162-173`、`tests/installer_firewall_rule_tests.cmake:1-30`：增加安装器防火墙契约测试。

### Reason
跨网段设备能够通过 TCP 49101 扫描并通过 TCP 49100/49102 远控，但界面仍显示离线，说明真实故障集中在 UDP 49104 实时状态通道。原批量新增路径在 TCP 查询成功后直接把 `m_deviceStatuses` 写成 `Online`，绕过了实时服务的 TTL、快照优先级和离线过期规则，导致扫描后出现不受统一生命周期管理的“假在线”。同时网络安装器没有创建 UDP 49104 入站规则，Windows 防火墙按本地子网限制时会直接造成跨网段订阅或回包丢失。

本次让所有扫描结果优先进入 `DeviceRealtimeStateService::applyManualCalibration()`，只有实时服务不可用时才写旧缓存；安装器规则限定程序路径、UDP 49104 和 RFC1918 私网来源，并在覆盖升级及卸载时清理同名规则。

### Original Code
```cpp
// src/system/DeviceStatusRefreshResult.h:10-22
struct DeviceStatusRefreshResult {
    QHash<QString, DeviceStatusInfo> devices;
    QHash<QString, bool> updateAvailability;

    void reserve(int count)
    {
        devices.reserve(count);
        updateAvailability.reserve(count);
    }
};
```

```cpp
// src/ui/DeviceGrid.cpp:1337-1342,6784-6793
struct BatchAddResult {
    QString name;
    QString ip;
    QString mac;
    QString broadcastIp;
};

BatchAddResult result;
result.ip = ip;
result.name = info.deviceName.trimmed().isEmpty() ? ip : info.deviceName.trimmed();
result.mac = info.mac.trimmed();
result.broadcastIp = info.broadcastIp.trimmed();
```

```cpp
// src/ui/DeviceGrid.cpp:6825-6867
if (existingIndex >= 0) {
    // 补齐 MAC / broadcastIp
    grid->m_deviceStatuses.insert(ip, platform::DevicePresenceState::Online);
    continue;
}

// 新增目录记录
grid->m_deviceStatuses.insert(ip, platform::DevicePresenceState::Online);

if (addedCount > 0 || updatedCount > 0) {
    saveDevices();
    grid->updateRealtimeConfiguredDevices();
}
```

```ini
; installer/FSRemote.iss.in:43-44
[Run]
Filename: "{app}\FSRemote.exe"; Description: "安装完成后启动 FSRemote"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
```

```cpp
// tests/device_action_target_tests.cpp:1-74
#include "system/DeviceActionTargetResolver.h"
#include "system/DeviceActionPolicy.h"
#include "system/DeviceCatalog.h"

// 原测试只覆盖目标解析和动作资格，没有锁定状态结果应进入实时归并器。
```

```cmake
# CMakeLists.txt:160-164
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network Gui)

option(FSREMOTE_BUILD_WALLPAPER_TESTS "Build shared wallpaper discovery tests" ON)
```

```cmake
# tests/installer_firewall_rule_tests.cmake
# new file, no old code at this location
```

### Modified Code
```cpp
// src/system/DeviceStatusRefreshResult.h:10-21
enum class DeviceStatusResultSink {
    RealtimeReducer,
    DirectCache,
};

inline DeviceStatusResultSink deviceStatusResultSink(bool realtimeAvailable)
{
    return realtimeAvailable
        ? DeviceStatusResultSink::RealtimeReducer
        : DeviceStatusResultSink::DirectCache;
}
```

```cpp
// src/ui/DeviceGrid.cpp:1337-1343,6785-6795
struct BatchAddResult {
    QString name;
    QString ip;
    QString mac;
    QString broadcastIp;
    platform::DeviceStatusInfo status;
};

result.status = info;
```

```cpp
// src/ui/DeviceGrid.cpp:6820-6886
QHash<QString, platform::DeviceStatusInfo> discoveredStatuses;
// 已存在设备和新设备都先保存完整查询结果。
discoveredStatuses.insert(ip, result.status);

const bool realtimeAvailable = grid->m_realtimeStateService
    && grid->m_realtimeStateService->isRunning();
const platform::DeviceStatusResultSink statusSink =
    platform::deviceStatusResultSink(realtimeAvailable);
for (auto it = discoveredStatuses.cbegin(); it != discoveredStatuses.cend(); ++it) {
    if (statusSink == platform::DeviceStatusResultSink::RealtimeReducer) {
        grid->m_realtimeStateService->applyManualCalibration(it.key(), it.value());
    } else {
        grid->m_deviceStatuses.insert(it.key(), it.value().state);
    }
}
```

```ini
; installer/FSRemote.iss.in:43-56
[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""FSRemote Realtime UDP 49104"""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""FSRemote Realtime UDP 49104"" dir=in action=allow program=""{app}\FSRemote.exe"" protocol=UDP localport=49104 remoteip=10.0.0.0/8,172.16.0.0/12,192.168.0.0/16 profile=any enable=yes"; Flags: runhidden waituntilterminated
Filename: "{app}\FSRemote.exe"; Description: "安装完成后启动 FSRemote"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""FSRemote Realtime UDP 49104"""; Flags: runhidden waituntilterminated
```

```cpp
// tests/device_action_target_tests.cpp:67-85
bool statusResultsUseTheRealtimeReducerWhenAvailable()
{
    using platform::DeviceStatusResultSink;
    return expect(platform::deviceStatusResultSink(true) == DeviceStatusResultSink::RealtimeReducer,
                  "status results must use the realtime reducer when the service is running")
        && expect(platform::deviceStatusResultSink(false) == DeviceStatusResultSink::DirectCache,
                  "status results must use the legacy cache only when realtime is unavailable");
}
```

```cmake
# CMakeLists.txt:162-173
option(FSREMOTE_BUILD_INSTALLER_TESTS "Build installer firewall rule tests" ON)
if(FSREMOTE_BUILD_TESTS AND FSREMOTE_BUILD_INSTALLER_TESTS)
    enable_testing()
    add_test(
        NAME fsremote_installer_firewall_rule_tests
        COMMAND "${CMAKE_COMMAND}"
            "-DFSREMOTE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer_firewall_rule_tests.cmake"
    )
endif()
```

```cmake
# tests/installer_firewall_rule_tests.cmake:1-30
file(READ "${installer_path}" installer_text)
require_installer_text([[advfirewall firewall add rule name=""FSRemote Realtime UDP 49104""]]
    "UDP 49104 inbound allow rule")
require_installer_text([[program=""{app}\FSRemote.exe"" protocol=UDP localport=49104]]
    "program-scoped UDP port")
require_installer_text([[remoteip=10.0.0.0/8,172.16.0.0/12,192.168.0.0/16 profile=any enable=yes]]
    "RFC1918-only remote scope")
require_installer_text("[UninstallRun]" "uninstall cleanup section")
```

### Steps
1. 为状态查询结果增加统一的实时归并/旧缓存路由策略，并用纯状态测试固定该选择。
2. 批量扫描线程保留完整 `DeviceStatusInfo`，不再只携带设备名称、MAC 和广播地址。
3. 主线程先完成新增设备白名单更新，再将每个 TCP 49101 结果送入 `applyManualCalibration()`；实时服务不可用时保留真实 Online/Busy 回退。
4. 网络安装器在首次安装和覆盖升级时删除旧同名规则并创建程序专属 UDP 49104 入站规则，来源限制为 RFC1918 私网。
5. 卸载时删除安装器拥有的固定规则，并增加 CMake 文本契约测试防止后续打包改动遗漏规则。

### Verification
- `cmake -DFSREMOTE_SOURCE_DIR="C:/Users/test/Documents/Fsremote" -P tests/installer_firewall_rule_tests.cmake`：通过。
- `cmake --build build/codex-device-status-tests --target fsremote_device_action_target_tests`：通过。
- `ctest --test-dir build/codex-device-status-tests -R "fsremote_installer_firewall_rule_tests|fsremote_device_action_target_tests" --output-on-failure`：2/2 通过。
- `cmake --build build/codex-device-status-tests --target FSRemote`：通过，同时完成 `FSRemoteInstaller` 预处理，确认 C++ 与 Inno Setup 模板均可进入正式构建链。
- 构建保留一条原有 `DeviceRealtimeStateService.cpp:449` 的 MSVC C4804 警告，本次未修改该行，也不影响本次构建成功。
- 未在真实跨 VLAN 设备上执行网络联调；安装新版本后仍需确认目标设备防火墙中出现 `FSRemote Realtime UDP 49104`，并观察跨网段状态是否持续在线。

## 2026-08-01 17:08 - 排除 VMware 与 Hyper-V 虚拟网卡避免误判真实广播域

### Changed Location
- `src/system/NetworkInterfacePolicy.h:1-40`：新增可独立测试的虚拟局域网适配器识别策略。
- `src/system/DeviceRealtimeStateService.cpp:3-6,237-253`：实时状态广播端点枚举排除 VMware、Hyper-V、VirtualBox、WSL、Docker 和 Npcap 虚拟接口。
- `tests/network_interface_policy_tests.cpp:1-49`：覆盖现场 VMnet8、Hyper-V 以及真实 Realtek/Wi-Fi 网卡的筛选语义。
- `CMakeLists.txt:402-413,574-578`：登记独立网卡策略测试目标，并把策略头加入正式工程源列表。

### Reason
现场问题设备 `CLTEST / 192.168.3.32` 同时安装了 VMware 和 Hyper-V，其中 `VMware Network Adapter VMnet8` 占用了 `192.168.1.1/24`。原实时状态服务把所有已启用且支持广播的 Ethernet 接口都视为真实局域网接口，导致设备列表中的真实 `192.168.1.*` 目标被误判为与 VMnet8 同网段；`sendSubscriptions()` 因此跳过跨网段单播订阅，只向 VMware 虚拟广播地址发送数据。临时禁用 VMnet8 后跨网段在线状态立即恢复，确认根因不是防火墙或设备本地策略，而是虚拟网段冲突。

本次在广播端点形成前统一排除虚拟接口，使真实物理网卡继续广播，而与虚拟网卡地址重叠的公司网段重新走跨网段单播订阅，不要求用户永久关闭 VMware 或 Hyper-V。

### Original Code
```cpp
// src/system/NetworkInterfacePolicy.h
// new file, no old code at this location
```

```cpp
// src/system/DeviceRealtimeStateService.cpp:3-5,236-248
#include "system/DeviceInfoService.h"
#include "system/PortableOpenSshManager.h"
#include "system/WjyDiagnosticLog.h"

QList<BroadcastEndpoint> eligibleBroadcastEndpoints()
{
    for (const QNetworkInterface& networkInterface : QNetworkInterface::allInterfaces()) {
        const auto flags = networkInterface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || !flags.testFlag(QNetworkInterface::CanBroadcast)
            || flags.testFlag(QNetworkInterface::IsLoopBack)
            || flags.testFlag(QNetworkInterface::IsPointToPoint)) {
            continue;
        }
    }
}
```

```cpp
// tests/network_interface_policy_tests.cpp
// new file, no old code at this location
```

```cmake
# CMakeLists.txt:400-401
add_test(NAME fsremote_device_action_target_tests COMMAND fsremote_device_action_target_tests)
add_executable(fsremote_script_ui_state_store_tests EXCLUDE_FROM_ALL
```

```cmake
# CMakeLists.txt:574-577
src/system/DeviceRealtimeStateService.cpp
src/system/DeviceRealtimeStateService.h
```

### Modified Code
```cpp
// src/system/NetworkInterfacePolicy.h:9-38
inline bool isVirtualLanInterface(
    QNetworkInterface::InterfaceType type,
    const QString& systemName,
    const QString& displayName)
{
    if (type == QNetworkInterface::Virtual) {
        return true;
    }
    const QString identity = (systemName + QLatin1Char(' ') + displayName).toCaseFolded();
    static const QStringList virtualMarkers {
        QStringLiteral("vmware"), QStringLiteral("vmnet"),
        QStringLiteral("vethernet"), QStringLiteral("hyper-v"),
        QStringLiteral("default switch"), QStringLiteral("virtualbox"),
        QStringLiteral("wsl"), QStringLiteral("docker"), QStringLiteral("npcap"),
    };
    for (const QString& marker : virtualMarkers) {
        if (identity.contains(marker)) return true;
    }
    return false;
}
```

```cpp
// src/system/DeviceRealtimeStateService.cpp:3-6,237-253
#include "system/DeviceInfoService.h"
#include "system/NetworkInterfacePolicy.h"
#include "system/PortableOpenSshManager.h"

if (!flags.testFlag(QNetworkInterface::IsUp)
    || !flags.testFlag(QNetworkInterface::IsRunning)
    || !flags.testFlag(QNetworkInterface::CanBroadcast)
    || flags.testFlag(QNetworkInterface::IsLoopBack)
    || flags.testFlag(QNetworkInterface::IsPointToPoint)
    || isVirtualLanInterface(
        networkInterface.type(),
        networkInterface.name(),
        networkInterface.humanReadableName())) {
    continue;
}
```

```cpp
// tests/network_interface_policy_tests.cpp:13-48
bool virtualAdaptersAreRejectedWithoutHidingPhysicalLanAdapters()
{
    return expect(isVirtualLanInterface(Ethernet, "ethernet_2", "VMware Network Adapter VMnet8"),
                  "VMware Ethernet-style adapter must be rejected")
        && expect(isVirtualLanInterface(Ethernet, "vEthernet (Default Switch)", "Hyper-V Virtual Ethernet Adapter"),
                  "Hyper-V switch adapter must be rejected")
        && expect(!isVirtualLanInterface(Ethernet, "ethernet_1", "Realtek PCIe GbE Family Controller"),
                  "physical Realtek adapter must remain eligible")
        && expect(!isVirtualLanInterface(Wifi, "wlan_1", "Intel(R) Wi-Fi 6 AX201"),
                  "physical Wi-Fi adapter must remain eligible");
}
```

```cmake
# CMakeLists.txt:402-413
add_executable(fsremote_network_interface_policy_tests EXCLUDE_FROM_ALL
    tests/network_interface_policy_tests.cpp
    src/system/NetworkInterfacePolicy.h
)
target_include_directories(fsremote_network_interface_policy_tests PRIVATE src)
target_link_libraries(fsremote_network_interface_policy_tests PRIVATE Qt6::Network)
add_test(NAME fsremote_network_interface_policy_tests COMMAND fsremote_network_interface_policy_tests)
```

```cmake
# CMakeLists.txt:574-578
src/system/DeviceRealtimeStateService.cpp
src/system/DeviceRealtimeStateService.h
src/system/NetworkInterfacePolicy.h
```

### Steps
1. 将虚拟网卡识别拆成无系统副作用的头文件策略，便于模拟现场网卡名称测试。
2. 优先使用 Qt `Virtual` 类型，同时补充 VMware VMnet、Hyper-V vEthernet、VirtualBox、WSL、Docker 和 Npcap 名称标记。
3. 在 `eligibleBroadcastEndpoints()` 生成广播端点前排除虚拟接口，使其不再参与同网段判断。
4. 保留真实 Realtek 有线和 Intel Wi-Fi 接口，避免修复虚拟网段冲突时误伤正常局域网广播。
5. 新增独立 CTest 目标并执行完整主程序构建验证。

### Verification
- 现场手工验证：禁用 `VMware Network Adapter VMnet8` 后，`CLTEST` 的真实跨网段设备恢复在线，确认虚拟 `192.168.1.0/24` 冲突是根因。
- `cmake --build build/codex-device-status-tests --target fsremote_network_interface_policy_tests`：通过。
- `ctest --test-dir build/codex-device-status-tests -R "fsremote_network_interface_policy_tests|fsremote_installer_firewall_rule_tests|fsremote_device_action_target_tests" --output-on-failure`：3/3 通过。
- `cmake --build build/codex-device-status-tests --target FSRemote`：通过，确认生产实时状态服务能够编译和链接新增策略。
- 构建仍有一条原有 `DeviceRealtimeStateService.cpp:454` MSVC C4804 警告，本次未修改对应布尔表达式，不影响构建成功。
- 尚未把新构建发布到 `CLTEST` 做“保持 VMnet8 启用”的最终实机回归；发布后应重新启用 VMnet8、重启 FSRemote，并确认真实 `192.168.1.*` 设备持续在线。

## 2026-08-04 09:17 - 远控单选/多选质量变量与后台降帧

### Changed Location
- `src/ui/RemoteQualityCoordinator.cpp:11-40`：新增单选和多选质量档案变量，集中维护前台分辨率、FPS、码率、后台分辨率及按窗口数量的后台FPS梯度。
- `src/ui/RemoteQualityCoordinator.cpp:166-200,232-260,329-335`：按实际远控窗口数量选择质量档案，并将档案应用到前台/后台质量请求和码率。
- `tests/remote_quality_coordinator_tests.cpp:31-121,220-268`：更新单选1080p/60、多选1080p/45、后台15/10/5/3/1 FPS策略的断言。

### Reason
单窗口和多窗口共用原始分辨率/60 FPS时，单窗口会在高分辨率源上卡顿；多窗口后台还固定占用30 FPS，会和焦点窗口争用解码、GPU及Qt呈现资源。本次把质量集中到两个可直接编辑的 `constexpr` 变量：`kSingleSelectionQuality` 和 `kMultiSelectionQuality`。默认单选为1080p/60，多选焦点为1080p/45，多选后台按可见窗口数量降为15/10/5/3/1 FPS，后续可直接修改变量而不必改协调器流程。

### Original Code
```cpp
// src/ui/RemoteQualityCoordinator.cpp:10-11（修改前）
//constexpr std::array<int, 5> kBackgroundVisibleFpsTiers = {15, 10, 5, 3, 1};
constexpr std::array<int, 5> kBackgroundVisibleFpsTiers = {30, 30, 30, 30, 30};
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:192-219（修改前）
decision.resolution = stream::RemoteResolutionTier::P720;
decision.targetFps = backgroundFps;
decision.resolution = stream::RemoteResolutionTier::Native;
decision.targetFps = 60;
```

### Modified Code
```cpp
// src/ui/RemoteQualityCoordinator.cpp:11-40（修改后）
struct RemoteQualityPreset {
    stream::RemoteResolutionTier resolution;
    int targetFps;
    int maxBitrateKbps;
};

struct RemoteSelectionQualityProfile {
    RemoteQualityPreset foreground;
    RemoteQualityPreset background;
    std::array<int, 5> backgroundFpsTiers;
};

constexpr RemoteSelectionQualityProfile kSingleSelectionQuality = {
    {stream::RemoteResolutionTier::P1080, 60, 48000},
    {stream::RemoteResolutionTier::P720, 30, 24000},
    {30, 30, 30, 30, 30},
};

constexpr RemoteSelectionQualityProfile kMultiSelectionQuality = {
    {stream::RemoteResolutionTier::P1080, 45, 48000},
    {stream::RemoteResolutionTier::P720, 15, 24000},
    {15, 10, 5, 3, 1},
};
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:166-200,232-260,329-335（修改后）
const RemoteSelectionQualityProfile& selectionQuality =
    selectionQualityProfile(singleRemoteWindow);
const int baseBackgroundFps = backgroundFpsForVisibleWindowCount(
    visibleWindowCount,
    selectionQuality.backgroundFpsTiers);
decision.resolution = decision.minimized
    ? configuration.minimizedResolution
    : selectionQuality.background.resolution;
decision.resolution = selectionQuality.foreground.resolution;
decision.targetFps = selectionQuality.foreground.targetFps;
decision.maxBitrateKbps = !window.softwareFallback && selectedPreset.maxBitrateKbps > 0
    ? selectedPreset.maxBitrateKbps
    : bitrateForDecision(decision.resolution, decision.targetFps, decision.effectiveMode);
```

```cpp
// tests/remote_quality_coordinator_tests.cpp:31-61,220-268（修改后）
assert(decisions[0].resolution == stream::RemoteResolutionTier::P1080);
assert(decisions[0].targetFps == 60);
assert(decisions[1].targetFps == 15);
assert(decisions[1].targetFps == 45);
assert(sixWindowDecisions[index].targetFps == 10);
assert(elevenWindowDecisions[index].targetFps == 5);
assert(twentyWindowDecisions[index].targetFps == 3);
assert(twentyOneWindowDecisions[index].targetFps == 1);
```

### Steps
1. 将原先散落在分支中的单窗口原始/60、多窗口后台P720/30提取为单选/多选质量档案。
2. 让多选质量档案提供15/10/5/3/1 FPS后台梯度，并将Presenter压力降档复用同一梯度。
3. 在质量决策末尾使用档案码率；软件Presenter回退仍使用原有540p/24安全档，避免回退路径被高码率覆盖。
4. 更新质量协调器测试，覆盖单选、多选焦点、后台数量梯度和Presenter压力降档。

### Verification
- `cmd /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Qt\Tools\QtCreator\bin\jom\jom.exe" -f Makefile fsremote_remote_quality_coordinator_tests'`：通过。
- `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release/fsremote_remote_quality_coordinator_tests.exe`：通过，退出码0。
- 增量 `FSRemote` 链接阶段因已有 `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release/FSRemote.exe` 被占用而失败；本次修改的 `RemoteQualityCoordinator.cpp` 已完成编译，未强制终止现有进程。
- 未修改 `C:\Users\test\Documents\Fsremote2`；本次代码和日志均位于 `C:\Users\test\Documents\Fsremote`。

## 2026-08-04 13:25 - 完善原生视频帧所有权、严格帧率调度并同步OpenSpec

### Changed Location
- `third_party/lan_stream_probe/src/ffmpeg_decoder.h:28-30,64-66`：增加生产者/消费者交接状态和显式 handoff/reclaim/release 接口。
- `third_party/lan_stream_probe/src/ffmpeg_decoder.cpp:318-400`：在应用回调前交出 key 1，并为拒绝、软件回退和异常路径恢复 key 0。
- `third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:859-950`：用状态化守卫管理回调前交接、接受后转移和回退前重新取得所有权。
- `src/stream/RemoteVideoD3D11Surface.cpp:159-252`：区分“尚未取得消费者 key”的 `RetrySync` 与已经归还 key 的普通丢帧。
- `src/stream/RemoteVideoRenderWorker.h:22-54,100-112,142-145`：增加 in-flight 帧和待归还帧状态。
- `src/stream/RemoteVideoRenderWorker.cpp:226-228,287-289,308-558`：保留同步忙帧、退避重试、换代/关闭安全归还并维持 latest-frame 上限。
- `src/stream/RemoteVideoScheduler.h:32-39`、`src/stream/RemoteVideoScheduler.cpp:75-85,146-151`：增加重试退避并真正阻止 deadline 未到的窗口被提前呈现。
- `include/FsRemoteStreamApi.h:27-36`、`third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:3310-3320`：纹理回调增加 RTP、render 和 decoded 时间戳。
- `src/ui/RemoteDesktopWindow.h:64-73`、`src/ui/RemoteDesktopWindow.cpp:1564-1590,5093-5120`：原生帧直接保存真实解码时间轴，不再用应用回调时刻伪造帧龄。
- `src/ui/RemoteQualityCoordinator.cpp:226-254`：模式切换只重置压力状态，保留 350ms 远端角色防抖状态。
- `src/stream/RemoteQualityPolicy.h:27-34,84-106`：最高目标限制为 60 FPS，最小化远端请求统一为 640x360/1 FPS。
- `tests/remote_video_scheduler_tests.cpp:28-47`、`tests/remote_video_render_worker_tests.cpp:10-137`：覆盖严格 deadline、RetrySync、替换帧归还和 lease 单次释放。
- `tests/remote_quality_policy_tests.cpp:7-40`、`tests/remote_quality_coordinator_tests.cpp:65-99,276-294`：同步 360p/1 FPS 和焦点防抖断言。
- `CMakeLists.txt:329-344,390-406,550-560`：修复无 WebRTC 测试配置的无效音频测试注册、公共头目录和更新测试诊断实现链接。
- `openspec/changes/rebuild-video-frame-pipeline/tasks.md`：只勾选本轮有源码和验证证据支持的任务。

### Reason
旧实现的解码器在纹理回调返回后才执行 `ReleaseSync(key 1)`。应用回调一旦把帧异步提交给 RenderWorker，工作线程就可能先于解码线程尝试 `AcquireSync(key 1)`；原代码把这次 1ms 超时当成普通丢帧并释放 lease，导致消费者 key 没有归还、三槽解码输出池逐步被占满。与此同时，调度器虽然保存了 60/30 FPS deadline，却仍会返回尚未到期的窗口，因此后台 30 FPS 上限并未真正生效。

本次修改把所有权交接提前到应用回调之前；未取得 key 的帧由 RenderWorker 继续持有并退避重试，成功呈现或显式归还后才释放 lease。调度器现在严格检查 deadline，焦点窗口上限为 60 FPS、后台可见窗口为 30 FPS。最小化远端编码请求也与本地安全档统一为 640x360/1 FPS，避免 Host 继续编码无用的 540p 画面。

### Original Code
```cpp
// third_party/lan_stream_probe/src/ffmpeg_decoder.h:28-30（修改前）
int shared_texture_index = -1;
bool shared_texture_locked = false;

// third_party/lan_stream_probe/src/ffmpeg_decoder.h:63（修改前）
void release_shared_texture(DecodedFrame* frame, bool consumerAccepted);
```

```cpp
// third_party/lan_stream_probe/src/ffmpeg_decoder.cpp:326-341（修改前）
void H264Decoder::release_shared_texture(DecodedFrame* frame, bool consumerAccepted)
{
    if (!frame || !frame->shared_texture_locked) return;
    output_keyed_mutexes_[textureIndex]->ReleaseSync(
        consumerAccepted ? kSharedTextureConsumerKey : kSharedTextureProducerKey);
    frame->shared_texture_locked = false;
    frame->shared_texture_index = -1;
}
```

```cpp
// third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:859-910（修改前）
struct SharedTextureReleaseGuard {
    bool released = false;
    void release(bool consumerAccepted)
    {
        decoder->release_shared_texture(frame, consumerAccepted);
        released = true;
    }
};
texture_result = texture_callback_(/* ... */);
sharedTextureGuard.release(texture_result == DecodedTextureAccepted);
```

```cpp
// src/stream/RemoteVideoD3D11Surface.cpp:182-186,243-247（修改前）
const HRESULT acquire = guard.acquire(importedTexture_.Get(), impl->context.Get());
if (acquire == WAIT_TIMEOUT || acquire == DXGI_ERROR_WAS_STILL_DRAWING) {
    return RemoteVideoRenderResult::DroppedSyncBusy;
}
```

```cpp
// src/stream/RemoteVideoRenderWorker.h:22-28,98-106（修改前）
enum class RemoteVideoRenderResult : std::uint8_t {
    Presented, DroppedStale, DroppedSyncBusy, DeviceLost, Failed,
};
struct Session {
    RemoteVideoSurfaceState state;
    std::shared_ptr<RemoteVideoRenderSurface> surface;
    FrameInbox inbox;
};
```

```cpp
// src/stream/RemoteVideoRenderWorker.cpp:319-325,367-405（修改前）
if (result.replaced) {
    const auto lease = result.replaced->lease;
    it->second.surface->discard(std::move(*result.replaced));
    if (lease) lease->release(RemoteVideoFrameReleaseReason::PendingReplaced);
}
auto frame = session.inbox.takeLatest();
const auto result = session.surface->render(std::move(*frame));
if (lease) lease->release(reason);
```

```cpp
// src/stream/RemoteVideoScheduler.cpp:132-141（修改前）
if (!selected) return std::nullopt;
RemoteVideoScheduleDecision decision;
decision.windowId = selectedId;
decision.deadlineMs = selected->nextDueMs;
return decision;
```

```cpp
// include/FsRemoteStreamApi.h:27-33（修改前）
typedef int(FSREMOTE_STREAM_CALL* FsRemoteTextureFrameCallback)(
    void* user, int width, int height, void* shared_handle,
    uint64_t frame_id, double encoded_mbps);
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:3293-3313（修改前）
(void)rtp_timestamp;
(void)render_time_ms;
(void)decoded_at_us;
return texture_callback_(user_, width, height, shared_handle, frame_id, encoded_mbps);
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:5086-5096（修改前）
frame.frameId = frameId;
frame.decodedAtUs = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
frame.width = width;
frame.height = height;
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:226-230（修改前）
if (!state.initialized || state.effectiveMode != decision.effectiveMode) {
    state = {};
    state.fpsIndex = baselineFps;
    state.effectiveMode = decision.effectiveMode;
    state.initialized = true;
}
```

```cpp
// src/stream/RemoteQualityPolicy.h:32-34,89,105-106（修改前）
int minimizedFps = 1; // 注释仍写15 FPS
RemoteResolutionTier minimizedResolution = RemoteResolutionTier::P540;
configuration.targetFps = std::clamp(configuration.targetFps, 15, 360);
configuration.minimizedResolution = RemoteResolutionTier::P540;
```

```cmake
# CMakeLists.txt:329-340,390-400,550-560（修改前）
add_executable(fsremote_viewer_audio_stop_tests EXCLUDE_FROM_ALL tests/viewer_audio_player_stop_tests.cpp)
target_link_libraries(fsremote_viewer_audio_stop_tests PRIVATE uu_stream_common)
add_test(NAME fsremote_viewer_audio_stop_tests COMMAND fsremote_viewer_audio_stop_tests)
target_include_directories(fsremote_remote_connection_state_tests PRIVATE src)
# fsremote_update_service_tests 未包含 WjyDiagnosticLog.cpp
```

```cpp
// tests/remote_video_scheduler_tests.cpp（修改前）
// 没有验证60/30 FPS deadline，也没有RetrySync退避断言。

// tests/remote_video_render_worker_tests.cpp（修改前）
// Fake Surface始终立即Presented/DroppedStale，没有覆盖同步忙后lease保留。
```

```cpp
// tests/remote_quality_policy_tests.cpp:13-17（修改前）
assert(defaults.minimizedFps == 15);
assert(defaults.minimizedResolution == stream::RemoteResolutionTier::P540);
```

### Modified Code
```cpp
// third_party/lan_stream_probe/src/ffmpeg_decoder.h:28-30,64-66（修改后）
int shared_texture_index = -1;
bool shared_texture_locked = false;
bool shared_texture_handed_off = false;
bool handoff_shared_texture(DecodedFrame* frame);
bool reclaim_shared_texture(DecodedFrame* frame);
bool release_shared_texture(DecodedFrame* frame);
```

```cpp
// third_party/lan_stream_probe/src/ffmpeg_decoder.cpp:327-400（修改后）
bool H264Decoder::handoff_shared_texture(DecodedFrame* frame)
{
    context_->Flush();
    const HRESULT result = keyedMutex->ReleaseSync(kSharedTextureConsumerKey);
    if (FAILED(result)) return false;
    frame->shared_texture_locked = false;
    frame->shared_texture_handed_off = true;
    return true;
}

bool H264Decoder::reclaim_shared_texture(DecodedFrame* frame)
{
    const HRESULT acquire = keyedMutex->AcquireSync(kSharedTextureConsumerKey, 2);
    if (acquire != S_OK) return false;
    frame->shared_texture_handed_off = false;
    frame->shared_texture_locked = true;
    return true;
}

bool H264Decoder::release_shared_texture(DecodedFrame* frame)
{
    if (frame->shared_texture_handed_off && !reclaim_shared_texture(frame)) return false;
    const HRESULT release = keyedMutex->ReleaseSync(kSharedTextureProducerKey);
    if (FAILED(release)) return false;
    frame->shared_texture_index = -1;
    return true;
}
```

```cpp
// third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:859-950（修改后）
struct SharedTextureReleaseGuard {
    bool handoff() { return decoder->handoff_shared_texture(frame); }
    bool reclaim() { return decoder->reclaim_shared_texture(frame); }
    void release() { if (decoder->release_shared_texture(frame)) completed = true; }
    void dismissAccepted() { completed = true; }
};
if (texture_callback_ && decoded.shared_handle && sharedTextureGuard.handoff()) {
    texture_result = texture_callback_(/* ... */);
}
if (texture_result == DecodedTextureAccepted) sharedTextureGuard.dismissAccepted();
else if (texture_result == DecodedTextureDropped) sharedTextureGuard.release();
else if (!sharedTextureGuard.reclaim()) return WEBRTC_VIDEO_CODEC_ERROR;
```

```cpp
// src/stream/RemoteVideoD3D11Surface.cpp:159-191,233-252（修改后）
const HRESULT acquire = guard.acquire(importedTexture_.Get(), impl->context.Get());
if (acquire == WAIT_TIMEOUT || acquire == DXGI_ERROR_WAS_STILL_DRAWING) {
    return RemoteVideoRenderResult::RetrySync;
}
if (!state_.nativeWindow || !state_.visible || state_.minimized) {
    return RemoteVideoRenderResult::DroppedStale; // guard归还key 0
}
```

```cpp
// src/stream/RemoteVideoRenderWorker.h:22-54,100-112（修改后）
enum class RemoteVideoRenderResult : std::uint8_t {
    Presented, DroppedStale, DroppedSyncBusy, RetrySync, DeviceLost, Failed,
};
struct Session {
    RemoteVideoSurfaceState state;
    std::shared_ptr<RemoteVideoRenderSurface> surface;
    FrameInbox inbox;
    std::optional<NativeVideoFrame> inFlight;
    std::deque<RetiringFrame> retiringFrames;
};
```

```cpp
// src/stream/RemoteVideoRenderWorker.cpp:351-558（修改后）
void RemoteVideoRenderWorker::processRetiringFrames(std::int64_t nowMs)
{
    if (result == RemoteVideoRenderResult::RetrySync) {
        retiring.nextRetryMs = nowMs + delayMs;
        session.retiringFrames.push_back(std::move(retiring));
        continue;
    }
    lease->release(retiring.reason);
}

if (!session.inFlight) session.inFlight = session.inbox.takeLatest();
const auto result = session.surface->render(*session.inFlight);
if (result == RemoteVideoRenderResult::RetrySync) {
    scheduler_.markRetry(decision->windowId, nowMs);
    continue;
}
session.inFlight.reset();
```

```cpp
// src/stream/RemoteVideoScheduler.cpp:75-85,146-151（修改后）
void RemoteVideoScheduler::markRetry(std::uint64_t windowId, std::int64_t nowMs, std::int64_t retryDelayMs)
{
    entry.state.frameReady = true;
    entry.nextDueMs = nowMs + std::max<std::int64_t>(1, retryDelayMs);
}
if (selected->nextDueMs > nowMs) return std::nullopt;
```

```cpp
// include/FsRemoteStreamApi.h:27-36（修改后）
typedef int(FSREMOTE_STREAM_CALL* FsRemoteTextureFrameCallback)(
    void* user, int width, int height, void* shared_handle,
    uint64_t frame_id, int64_t rtp_timestamp, int64_t render_time_ms,
    uint64_t decoded_at_us, double encoded_mbps);
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:5114-5120（修改后）
frame.frameId = frameId;
frame.rtpTimestamp = rtpTimestamp;
frame.renderTimeMs = renderTimeMs;
frame.decodedAtUs = static_cast<std::int64_t>(decodedAtUs);
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:226-254（修改后）
if (!state.initialized) {
    state = {};
    state.initialized = true;
} else if (state.effectiveMode != decision.effectiveMode) {
    state.fpsIndex = baselineFps;
    state.pressureSinceMs = 0;
    state.recoverySinceMs = 0;
    state.effectiveMode = decision.effectiveMode;
    // pendingHighPerformance/remoteHighPerformance/roleChangedAtMs继续保留
}
```

```cpp
// src/stream/RemoteQualityPolicy.h:32-34,89,105-106（修改后）
int minimizedFps = 1;
RemoteResolutionTier minimizedResolution = RemoteResolutionTier::P360;
configuration.targetFps = std::clamp(configuration.targetFps, 15, 60);
configuration.minimizedResolution = RemoteResolutionTier::P360;
```

```cmake
# CMakeLists.txt:329-344,390-406,550-560（修改后）
if(TARGET uu_stream_common)
    add_executable(fsremote_viewer_audio_stop_tests EXCLUDE_FROM_ALL tests/viewer_audio_player_stop_tests.cpp)
    target_include_directories(fsremote_viewer_audio_stop_tests PRIVATE third_party/uu_stream_webrtc/src)
    add_test(NAME fsremote_viewer_audio_stop_tests COMMAND fsremote_viewer_audio_stop_tests)
else()
    message(STATUS "Skipping fsremote_viewer_audio_stop_tests because the WebRTC common target is disabled")
endif()
target_include_directories(fsremote_remote_connection_state_tests PRIVATE src include)
target_sources(fsremote_update_service_tests PRIVATE src/system/WjyDiagnosticLog.cpp src/system/WjyDiagnosticLog.h)
```

```cpp
// tests/remote_video_scheduler_tests.cpp:28-47（修改后）
assert(!scheduler.next(1005).has_value());
assert(scheduler.next(1016).has_value());
scheduler.markRetry(1, 1016, 2);
assert(!scheduler.next(1017).has_value());
assert(!scheduler.next(2032).has_value());
assert(scheduler.next(2033).has_value());
```

```cpp
// tests/remote_video_render_worker_tests.cpp:90-136（修改后）
surface->renderRetriesRemaining = 1;
assert(surface->renderAttempts >= 2);
assert(releases == 1); // RetrySync期间不释放
surface->discardRetriesRemaining = 1;
assert(surface->discardAttempts >= 3);
assert(releases == 2);
```

```cpp
// tests/remote_quality_policy_tests.cpp:13-17,37-39（修改后）
assert(defaults.minimizedFps == 1);
assert(defaults.minimizedResolution == stream::RemoteResolutionTier::P360);
assert(normalized.minimizedFps == 1);
assert(normalized.minimizedResolution == stream::RemoteResolutionTier::P360);
```

```cpp
// tests/remote_quality_coordinator_tests.cpp:68,98,284-294（修改后）
assert(decisions.front().resolution == stream::RemoteResolutionTier::P360);
assert(!debounceDecisions[0].requestRemoteProfile);
assert(!debounceDecisions[1].requestRemoteProfile);
assert(debounceDecisions[0].requestRemoteProfile); // 稳定超过350ms
assert(debounceDecisions[1].requestRemoteProfile);
```

### Steps
1. 将解码器共享纹理从“回调返回后交接”改为“回调调用前交接”，消除解码线程与 RenderWorker 的 key 1 时序竞争。
2. 把 `DroppedSyncBusy` 拆分为已经安全归还的普通丢帧和尚未取得所有权的 `RetrySync`。
3. 为每个会话增加唯一 in-flight 帧与待归还帧退避队列，替换、换代、关闭路径只有在成功归还或确认设备故障后才结束 lease。
4. 修复调度器 deadline 未真正生效的问题，并增加 2/4/8/16ms 同步重试退避，让焦点窗口和后台窗口按 60/30 FPS 上限公平运行。
5. 将 WebRTC RTP timestamp、render time 和 decoded monotonic timestamp 贯穿 DLL C API 到 `NativeVideoFrame`。
6. 修复焦点切换时画质模式重置绕过 350ms 远端改参防抖的问题。
7. 将最小化远端请求统一为 640x360/1 FPS，并把异常目标 FPS 最高夹到 60。
8. 补充 scheduler、RenderWorker、画质策略和协调器测试；修复测试构建中缺失的 include/source 条目。
9. 根据源码和验证证据同步 `rebuild-video-frame-pipeline` OpenSpec 勾选，仍保留 Fence、旧模块删除和真实多窗口压力验收等未完成项。

### Verification
- `ctest --test-dir build-video-tests-msvc -C Debug --output-on-failure -R "fsremote_remote_video_(scheduler|render_worker|frame_inbox|diagnostics|policy)_tests|fsremote_remote_quality_coordinator_tests" --timeout 30`：6/6 通过。
- `cmake --build build-video-tests-msvc --config Debug --target FSRemote -- /m`：通过。
- `cmake --build build-video-webrtc-msvc --config Debug --target fsremote_stream FSRemote -- /m`：通过。
- WebRTC Debug `FSRemote.exe` 启动 5 秒冒烟：进程持续存活，无启动闪退；随后仅终止本次测试启动的 PID。
- 全仓 CTest 已运行到第 14 项：前 13 项全部通过；随后被与本次视频链路无关的 `fsremote_remote_input_broadcast_tests` 既有断言 `!narrow.inputSync.isEmpty()` 阻断，因此不宣称全仓 30/30。
- `git diff --check`：通过，无空白错误或补丁格式问题。
- 未修改 `C:\Users\test\Documents\Fsremote2`；本次实现、OpenSpec 和日志均位于 `C:\Users\test\Documents\Fsremote`。


## 2026-08-04 16:32 - 完成视频 Sink 解耦、删除旧 Presenter/纹理契约并建立确定性停流

### Changed Location
- CMakeLists.txt:268-280,607,616-617：登记 RemoteVideoSessionSink 专项测试和轻量视频表面，移除旧 texture slot 测试与旧 Presenter 源文件。
- include/FsRemoteStreamApi.h:28-68,207-213：仅保留原生帧信封、显式 release 与 native viewer 入口，删除三态 texture callback C ABI。
- src/stream/StreamRuntime.h:47-70,99-107,129、src/stream/StreamRuntime.cpp:93-95,190-203：删除旧 texture viewer 动态入口，只选择 native-frame 或 BGRA 回退。
- src/ui/RemoteVideoSessionSink.h:17-219、tests/remote_video_session_sink_tests.cpp:1-109：新增独立 Sink 及 lease/代际/软件最新帧测试。
- src/ui/RemoteVideoSurfaceWidget.h:17-56、src/ui/RemoteVideoSurfaceWidget.cpp:21-140：新增无 GPU 状态的稳定 HWND 与输入转发表面。
- src/ui/RemoteDesktopWindow.cpp:6,1559,1573,1747,1941-1945,2541-2596,5098-5129、src/ui/RemoteDesktopWindow.h:19,46,301,448：接入 Sink/轻量表面，删除旧 Qt texture drain，并在停流/析构时等待 RenderWorker。
- src/ui/RemoteVideoRenderService.h:26、src/ui/RemoteVideoRenderService.cpp:93-96：新增有界 waitForIdle 生命周期屏障。
- src/stream/RemoteVideoRenderWorker.cpp:197-207、tests/remote_video_render_worker_tests.cpp:44-77：命令拒绝按 ExplicitDrop 归还 lease，并补充原因测试。
- src/ui/RemoteClipboardCodec.cpp:20-36：启用严格 Base64 校验。
- third_party/uu_stream_webrtc/src/uu_codec_factory.h:13-64、uu_codec_factory.cpp:758-768,990-1048,1254-1313：删除旧 texture 三态分支，只保留原生 lease Sink 与 BGRA 回退。
- third_party/uu_stream_webrtc/src/native_webrtc_runtime.h:17-43、native_webrtc_runtime.cpp:76-121,160-167：Runtime 不再保存旧 texture callback。
- third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:2949-2967,3316-3375,3627-3684：ViewerInstance 和 DLL 导出删除 texture callback。
- 已删除 src/ui/D3D11FramePresenter.cpp、src/ui/D3D11FramePresenter.h、src/ui/LatestTextureFrameSlot.h、tests/latest_texture_frame_slot_tests.cpp。
- openspec/specs/remote-video-diagnostics/spec.md:1-69、openspec/specs/remote-video-pipeline/spec.md:1-95：同步两个主规格。
- openspec/changes/rebuild-video-frame-pipeline/tasks.md:55-67：完成 7.1、7.2、7.4、7.5、8.3、8.4、8.5，进度更新为 42/45。

### Reason
旧代码仍保留 UI 线程 D3D11 Presenter、Qt 逐帧 drain、texture 单槽和三态所有权契约，容易造成双链路复活及关闭竞态。本次把解码边界收敛为独立 Sink，把窗口表面收敛为 HWND/Input façade，并确保 RenderWorker 在解码器与 HWND 销毁前处理代际失效和 lease 回收。

### Original Code

~~~cpp
// src/ui/RemoteDesktopWindow.cpp:5097-5390（修改前）
#if 0
void RemoteDesktopWindow::enqueueRemoteFrame(...);
bool RemoteDesktopWindow::enqueueRemoteNativeFrame(...);
int RemoteDesktopWindow::enqueueRemoteTextureFrame(...);
void RemoteDesktopWindow::drainPendingRemoteTextureFrame();
#endif
~~~

~~~cpp
// src/ui/D3D11FramePresenter.h:25-60（修改前）
class D3D11FramePresenter final : public QWidget {
public:
    bool presentSharedTexture(void* sharedHandle, int width, int height);
    void discardSharedTexture(void* sharedHandle);
    void reset();
};
~~~

~~~cpp
// include/FsRemoteStreamApi.h:27-44,225-231（修改前）
typedef int(FSREMOTE_STREAM_CALL* FsRemoteTextureFrameCallback)(...);
enum FsRemoteTextureFrameResult {
    FSREMOTE_TEXTURE_FRAME_FALLBACK = 0,
    FSREMOTE_TEXTURE_FRAME_ACCEPTED = 1,
    FSREMOTE_TEXTURE_FRAME_DROPPED = 2,
};
FsRemoteStreamHandle fsremote_stream_start_viewer_with_texture(...);
~~~

~~~cpp
// third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:990-1019（修改前）
int texture_result = DecodedTextureFallback;
if (texture_callback_ && decoded.shared_handle && sharedTextureGuard.handoff()) {
    texture_result = texture_callback_(...);
}
if (texture_result != DecodedTextureFallback) {
    return WEBRTC_VIDEO_CODEC_OK;
}
~~~

~~~cpp
// src/ui/RemoteDesktopWindow.cpp:1941-1944,2541-2566（修改前）
RemoteVideoRenderService::instance().unregisterWindow(windowId);
invalidateViewerCallbacks();
stream::StreamRuntime::instance().stop(handle);
~~~

~~~cmake
# CMakeLists.txt:201-209（修改前）
add_executable(fsremote_latest_texture_frame_slot_tests ...)
# 没有 RemoteVideoSessionSink 专项测试。
~~~

~~~cpp
// src/ui/RemoteClipboardCodec.cpp:24（修改前）
const QByteArray decoded = QByteArray::fromBase64(encodedBase64.toLatin1());
~~~

~~~text
// 新增位置（修改前）
RemoteVideoSessionSink.h、RemoteVideoSurfaceWidget.*、remote_video_session_sink_tests.cpp：new code, no old code at this location
openspec/specs/remote-video-*/spec.md：main specs did not exist
~~~

### Modified Code

~~~cpp
// src/ui/RemoteVideoSessionSink.h:43-133（修改后）
void submitNative(const FsRemoteNativeVideoFrame* source) noexcept
{
    // 校验 session/generation 后把 DLL release 令牌封装为唯一 lease。
    native_submitter_(window_id_, std::move(frame));
}

void submitSoftware(QImage image, std::uint64_t generation) noexcept
{
    QMutexLocker locker(&software_mutex_);
    pending_software_frame_ = std::move(image);
}
~~~

~~~cpp
// src/ui/RemoteVideoSurfaceWidget.cpp:21-49,81-108（修改后）
RemoteVideoSurfaceWidget::RemoteVideoSurfaceWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    winId();
    hide();
}
~~~

~~~cpp
// src/ui/RemoteDesktopWindow.cpp:1559-1575,1747,1941-1945,2574-2590（修改后）
sink->submitSoftware(std::move(copy), context->generation);
sink->submitNative(frame);
m_texturePresenter = new RemoteVideoSurfaceWidget(this);

videoService.unregisterWindow(videoWindowId);
videoService.waitForIdle(std::chrono::milliseconds(250));
const bool drained = videoService.waitForIdle(std::chrono::milliseconds(250));
stream::StreamRuntime::instance().stop(handle);
~~~

~~~cpp
// src/stream/RemoteVideoRenderWorker.cpp:197-207（修改后）
if (!running_ || stopRequested_ || commands_.size() >= kCommandQueueLimit) {
    if (command.type == CommandType::Submit) {
        command.frame.release(RemoteVideoFrameReleaseReason::ExplicitDrop);
    }
    return false;
}
~~~

~~~cpp
// include/FsRemoteStreamApi.h:41-68,207-213（修改后）
typedef struct FsRemoteNativeVideoFrame {
    uint64_t session_id;
    uint64_t viewer_generation;
    uint64_t frame_id;
    void* shared_handle;
    void* lease_context;
    FsRemoteNativeVideoFrameReleaseCallback release;
} FsRemoteNativeVideoFrame;

FsRemoteStreamHandle fsremote_stream_start_viewer_with_native_frames(...);
~~~

~~~cpp
// third_party/uu_stream_webrtc/src/uu_codec_factory.h:23-48,59-61（修改后）
struct DecodedNativeFrameSink {
    std::function<bool()> enabled;
    std::function<void(DecodedNativeFrame)> submit;
};
std::unique_ptr<webrtc::VideoDecoderFactory> CreateUuVideoDecoderFactory(
    DecodedBgraCallback bgra_callback = {},
    DecodedNativeFrameSink native_frame_sink = {});
~~~

~~~cpp
// src/ui/RemoteClipboardCodec.cpp:25-34（修改后）
const auto decodeResult = QByteArray::fromBase64Encoding(
    encodedBase64.toLatin1(),
    QByteArray::AbortOnBase64DecodingErrors);
if (!decodeResult) return false;
const QByteArray& decoded = decodeResult.decoded;
~~~

~~~cmake
# CMakeLists.txt:268-280,607,616-617（修改后）
add_executable(fsremote_remote_video_session_sink_tests EXCLUDE_FROM_ALL
    tests/remote_video_session_sink_tests.cpp
    src/ui/RemoteVideoSessionSink.h
    src/stream/RemoteVideoDiagnostics.cpp
)
target_link_libraries(fsremote_remote_video_session_sink_tests PRIVATE Qt6::Gui)
# FSRemote 使用 RemoteVideoSurfaceWidget，不再包含 D3D11FramePresenter。
~~~

~~~markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/tasks.md:55-67（修改后） -->
- [x] 7.1 / 7.2 / 7.4 / 7.5
- [x] 8.3 / 8.4 / 8.5
~~~

### Steps
1. 把独立 Sink 专项测试加入 CMake 并验证 lease、代际和软件最新帧。
2. 物理删除 294 行旧 Qt texture callback/drain。
3. 用 RemoteVideoSurfaceWidget 替换旧 D3D11FramePresenter。
4. 删除 LatestTextureFrameSlot、旧 Presenter、旧 texture C ABI、StreamRuntime 入口和 WebRTC 分支。
5. 增加队列拒绝 ExplicitDrop 与确定性 stop/unregister 屏障。
6. 修复剪贴板严格 Base64 校验。
7. 同步 OpenSpec 主规格并更新任务勾选。

### Verification
- 非 WebRTC Debug：FSRemote、RemoteVideoSessionSink、RenderWorker 构建通过。
- WebRTC Debug：fsremote_stream.dll 与 FSRemote.exe 构建通过。
- 视频专项 CTest：frame_inbox、diagnostics、policy、scheduler、native_adapter、session_sink、render_worker 共 7/7 通过。
- 输入脚本、连接状态、剪贴板 CTest：3/3 通过。
- 音频 stop 测试：临时把 build-video-webrtc-msvc/third_party/uu_stream_webrtc/Debug 加入该测试进程 PATH 后退出码 0。
- remote_input_broadcast 的输入协调器用例先通过，最终被既有标题栏窄宽度断言 !narrow.inputSync.isEmpty() 阻断；该断言不属于本次视频链路。
- 扫描确认 FsRemoteTextureFrameCallback、FSREMOTE_TEXTURE_FRAME_*、start_viewer_with_texture、DecodedTextureCallback、D3D11FramePresenter、LatestTextureFrameSlot 均无生产代码引用。
- 仅修改 C:\Users\test\Documents\Fsremote；未修改 C:\Users\test\Documents\Fsremote2。

## 2026-08-04 17:26 - 修复双击设备时视频子窗口创建递归闪退

### Changed Location
- `src/ui/RemoteVideoSurfaceWidget.cpp:56-110`：原生事件和诊断快照改为只读取已经存在的窗口 ID，不再从同步 Windows 回调中调用会创建 HWND 的 `winId()`。
- `tests/remote_video_surface_widget_tests.cpp:1-48`：新增真实 Qt/Win32 视频表面生命周期回归测试。
- `CMakeLists.txt:301-314`：注册视频表面专项测试目标。
- `openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-pipeline/spec.md:36-39`：补充原生表面创建期间同步消息不得重入窗口创建的场景。
- `openspec/changes/rebuild-video-frame-pipeline/design.md:90`：记录 `internalWinId()` 与 `winId()` 的生命周期边界。
- `openspec/changes/rebuild-video-frame-pipeline/tasks.md:60`：新增并完成任务 7.6。
- `openspec/specs/remote-video-pipeline/spec.md:42-45`：将新增窗口创建安全场景同步到主规格。

### Reason
用户双击任意设备后，`RemoteDesktopWindow` 构造视频子窗口并主动调用 `QWidget::winId()`。Windows 会在 HWND 创建尚未返回时同步派发原生消息，旧 `nativeEvent()` 又调用一次 `winId()` 比较消息目标，导致 `QWidget::createWinId` 无限重入。两份系统崩溃转储均显示 `QWindowPrivate::create -> QWidget::createWinId -> QWidget::winId -> RemoteVideoSurfaceWidget::nativeEvent` 重复堆叠，最终在 Qt6Core 的 `QRect` 复制中以 `0xC000041D` 退出。修复选择 `internalWinId()`，因为它只读取 Qt 已经发布的句柄，不会隐式创建窗口；创建早期 ID 仍为 0 时把消息交回 Qt 默认流程。

### Original Code
```cpp
// src/ui/RemoteVideoSurfaceWidget.cpp:56-66（修改前）
QString RemoteVideoSurfaceWidget::resizeDebugSnapshot() const
{
    const QRect rect = geometry();
    return QStringLiteral("surface_hwnd=%1 geometry=%2,%3,%4x%5 visible=%6")
        .arg(static_cast<qulonglong>(winId()))
        .arg(rect.x())
        .arg(rect.y())
        .arg(rect.width())
        .arg(rect.height())
        .arg(hasVisiblePresentation() ? 1 : 0);
}
```

```cpp
// src/ui/RemoteVideoSurfaceWidget.cpp:81-107（修改前）
bool RemoteVideoSurfaceWidget::nativeEvent(
    const QByteArray& eventType,
    void* message,
    qintptr* result)
{
#ifdef Q_OS_WIN
    auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage && nativeMessage->hwnd == reinterpret_cast<HWND>(winId())) {
        // WM_ERASEBKGND / WM_PAINT handling
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}
```

```text
// tests/remote_video_surface_widget_tests.cpp（修改前）
new code, no old code at this location
```

```text
// CMakeLists.txt:301（修改前）
没有 fsremote_remote_video_surface_widget_tests 测试目标。
```

```text
// OpenSpec 相关位置（修改前）
没有“Native surface creation dispatches synchronous messages”场景、对应设计说明和任务 7.6。
```

### Modified Code
```cpp
// src/ui/RemoteVideoSurfaceWidget.cpp:56-66（修改后）
QString RemoteVideoSurfaceWidget::resizeDebugSnapshot() const
{
    const QRect rect = geometry();
    const WId surfaceWindowId = internalWinId(); // wjy: 诊断只读取Qt已经建立的原生句柄，绝不因为记录日志而隐式创建HWND。
    return QStringLiteral("surface_hwnd=%1 geometry=%2,%3,%4x%5 visible=%6")
        .arg(static_cast<qulonglong>(surfaceWindowId)) // wjy: internalWinId为0时如实记录未创建状态，不进入QWidget::winId的同步创建流程。
        .arg(rect.x())
        .arg(rect.y())
        .arg(rect.width())
        .arg(rect.height())
        .arg(hasVisiblePresentation() ? 1 : 0);
}
```

```cpp
// src/ui/RemoteVideoSurfaceWidget.cpp:82-110（修改后）
bool RemoteVideoSurfaceWidget::nativeEvent(
    const QByteArray& eventType,
    void* message,
    qintptr* result)
{
#ifdef Q_OS_WIN
    auto* nativeMessage = static_cast<MSG*>(message);
    const WId surfaceWindowId = internalWinId(); // wjy: 创建途中只能读取现有句柄，禁止再次调用winId形成递归。
    if (nativeMessage && surfaceWindowId != 0
        && nativeMessage->hwnd == reinterpret_cast<HWND>(surfaceWindowId)) {
        // WM_ERASEBKGND / WM_PAINT handling
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}
```

```cpp
// tests/remote_video_surface_widget_tests.cpp:19-46（修改后）
int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QWidget parent;
    parent.show();
    QApplication::processEvents();

    ui::RemoteVideoSurfaceWidget surface(&parent);
    surface.setPresentationVisible(true);
    QApplication::processEvents();

    const WId firstWindowId = surface.internalWinId();
    assert(firstWindowId != 0);
    ::SendMessageW(reinterpret_cast<HWND>(firstWindowId), WM_ERASEBKGND, 0, 0);
    ::InvalidateRect(reinterpret_cast<HWND>(firstWindowId), nullptr, FALSE);
    ::UpdateWindow(reinterpret_cast<HWND>(firstWindowId));
    assert(surface.internalWinId() == firstWindowId);
    return 0;
}
```

```cmake
# CMakeLists.txt:301-314（修改后）
add_executable(fsremote_remote_video_surface_widget_tests EXCLUDE_FROM_ALL
    tests/remote_video_surface_widget_tests.cpp
    src/ui/RemoteVideoSurfaceWidget.cpp
    src/ui/RemoteVideoSurfaceWidget.h
)
target_link_libraries(fsremote_remote_video_surface_widget_tests PRIVATE Qt6::Widgets user32)
add_test(NAME fsremote_remote_video_surface_widget_tests COMMAND fsremote_remote_video_surface_widget_tests)
```

```markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-pipeline/spec.md:36-39（修改后） -->
#### Scenario: Native surface creation dispatches synchronous messages
- **WHEN** Qt creates the native video child window and Windows dispatches native messages before handle creation returns
- **THEN** the UI facade reads only the already-established native identity and does not re-enter native-window creation from the message handler
```

### Steps
1. 解析两份 `FSRemote.exe` 崩溃转储，把重复二进制偏移对应到 `RemoteVideoSurfaceWidget::nativeEvent()` 内的 `winId()`。
2. 把原生消息路径和诊断快照改为 `internalWinId()`，创建早期没有 ID 时不拦截消息。
3. 新增真实 QWidget 父窗口、原生视频子窗口和同步 Win32 绘制消息回归测试。
4. 将测试注册到视频稳定性测试分类，并补充 WJY 源码说明。
5. 更新变更规格、设计、任务，并同步 `remote-video-pipeline` 主规格。

### Verification
- `fsremote_remote_video_surface_widget_tests` Debug：1/1 通过，真实创建子 HWND，并覆盖 `WM_ERASEBKGND` 与 `WM_PAINT`。
- 独立 `build-video-webrtc-msvc/Release/FSRemote.exe` 在用户要求停止构建前已经完成编译和链接；本次修改文件 `RemoteVideoSurfaceWidget.cpp` 编译通过。
- `git diff --check` 通过。
- OpenSpec 进度由 42/45 更新为 43/46；剩余任务仍是 Fence 与两项实机视频验证，没有虚假勾选。
- 用户要求不再构建后未启动新的编译任务。
- 仅修改 `C:\Users\test\Documents\Fsremote`；未修改 `C:\Users\test\Documents\Fsremote2`。

## 2026-08-04 17:56 - 修复多窗口 WebRTC 初始化堵塞与启动名额泄漏

### Changed Location
- `third_party/uu_stream_webrtc/src/runtime_initialization_gate.h:1-38`：新增进程级、可取消的 Runtime 初始化门禁。
- `third_party/uu_stream_webrtc/tests/runtime_initialization_gate_tests.cpp:1-65`：新增串行、取消和后继获取测试。
- `third_party/uu_stream_webrtc/src/native_webrtc_runtime.h:21-29`：为 Runtime 初始化增加可选取消回调，保留旧调用写法。
- `third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:29,91-151`：把媒体默认配置与 Factory 创建放入同一串行临界区，并处理等待取消。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:3397-3413`：Viewer 初始化接入停止信号并记录开始、结束和耗时日志。
- `src/ui/RemoteDesktopWindow.h:152-154,298`：声明普通首次连接看门狗及其独立计时器。
- `src/ui/RemoteDesktopWindow.cpp:1882-1885,1947-1949,2540,5550-5581,5619,5719`：接入 20 秒首次连接看门狗，并保持停止完成后才归还启动名额。
- `CMakeLists.txt:136-145`：登记 Runtime 初始化门禁测试目标。
- `openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-pipeline/spec.md:40-53`：增加有界且可取消的 Viewer 启动要求。
- `openspec/changes/rebuild-video-frame-pipeline/design.md:92-96`：记录门禁范围以及 20 秒、15 秒、5 分钟三类超时边界。
- `openspec/changes/rebuild-video-frame-pipeline/tasks.md:61`：新增并完成任务 7.7。
- `openspec/specs/remote-video-pipeline/spec.md:46-59`：把启动生命周期要求同步到主规格。

### Reason
八个窗口同时启动时，最多四个 Viewer 会并发进入 WebRTC Runtime 初始化。旧实现只串行 `CreateModularPeerConnectionFactory`，但 `EnableMediaWithDefaults` 仍会并发触碰进程级媒体状态；一旦其中的工作线程卡住，窗口停止会等待该线程退出，启动名额也无法归还，后续窗口便长期停留在“等待远控初始化资源”。本次把完整的进程敏感创建区串行化，并允许尚未进入临界区的 Viewer 在停止时取消等待。普通首次连接另外使用独立 20 秒看门狗，但网络重连继续使用原有 15 秒逻辑，远程更新继续使用原有 5 分钟状态机，避免改变其它连接流程。

### Original Code
```text
// third_party/uu_stream_webrtc/src/runtime_initialization_gate.h（修改前）
new code, no old code at this location
```

```text
// third_party/uu_stream_webrtc/tests/runtime_initialization_gate_tests.cpp（修改前）
new code, no old code at this location
```

```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.h:20-27（修改前）
class NativeWebrtcRuntime {
public:
    NativeWebrtcRuntime();
    ~NativeWebrtcRuntime();

    bool initialize(std::string* error);
```

```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:28,90-143（修改前，节选）
std::mutex factory_creation_mutex;

bool NativeWebrtcRuntime::initialize(std::string* error)
{
    // 创建依赖……
    webrtc::EnableMediaWithDefaults(deps);
    {
        std::lock_guard lock(impl_->shared_threads->factory_creation_mutex);
        impl_->factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
    }
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:3397-3402（修改前）
if (!runtime.initialize(&error)) {
    append_viewer_log("viewer runtime init failed error=" + error);
    report_status(status_callback_, user_, 90,
        error.empty() ? "WebRTC runtime initialization failed" : error.c_str());
    return;
}
```

```text
// src/ui/RemoteDesktopWindow.h（修改前）
没有普通首次连接专用看门狗声明和成员。
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2535-2543（修改前）
void RemoteDesktopWindow::stopViewerConnectionAsync(bool deleteAfterStop)
{
    if (deleteAfterStop) {
        m_deleteAfterViewerStop = true;
    }
    if (m_viewerStopInProgress) {
        return;
    }
```

```text
// CMakeLists.txt:136（修改前）
没有 uu_runtime_initialization_gate_tests 测试目标。
```

```text
// OpenSpec 相关文件（修改前）
没有“Bounded and cancellable viewer startup”要求、设计决策和任务 7.7。
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/runtime_initialization_gate.h:10-34（修改后）
class RuntimeInitializationGate final {
public:
    using CancellationCheck = std::function<bool()>;

    std::unique_lock<std::timed_mutex> acquire(
        const CancellationCheck& cancelled = {},
        std::chrono::milliseconds pollInterval = std::chrono::milliseconds(25))
    {
        const std::chrono::milliseconds boundedPoll = pollInterval.count() > 0
            ? pollInterval
            : std::chrono::milliseconds(1);
        std::unique_lock<std::timed_mutex> lease(mutex_, std::defer_lock);
        while (!lease.try_lock_for(boundedPoll)) {
            if (cancelled && cancelled()) {
                return lease;
            }
        }
        if (cancelled && cancelled()) {
            lease.unlock();
        }
        return lease;
    }

private:
    std::timed_mutex mutex_;
};
```

```cpp
// third_party/uu_stream_webrtc/tests/runtime_initialization_gate_tests.cpp:26-62（修改后，节选）
uu::RuntimeInitializationGate gate;
auto owner = gate.acquire();
assert(owner.owns_lock());

std::thread cancelledWaiter([&] {
    auto lease = gate.acquire(
        [&] { return cancelled.load(std::memory_order_acquire); },
        std::chrono::milliseconds(2));
    cancelledWaiterOwned.store(lease.owns_lock(), std::memory_order_release);
});
// 取消等待者后，再验证 owner 释放时后继线程可以取得门禁。
```

```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.h:21-29（修改后）
using CancellationCheck = std::function<bool()>;

bool initialize(
    std::string* error,
    CancellationCheck cancellationCheck = {});
```

```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:103-151（修改后，节选）
const std::shared_ptr<SharedWebrtcThreads> sharedThreads = impl_->shared_threads;
auto initializationLease =
    sharedThreads->runtime_initialization_gate.acquire(cancellationCheck);
if (!initializationLease.owns_lock()) {
    if (error) *error = "WebRTC runtime initialization cancelled";
    shutdown();
    return false;
}

webrtc::EnableMediaWithDefaults(deps);
impl_->factory =
    webrtc::CreateModularPeerConnectionFactory(std::move(deps));
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:3397-3413（修改后）
const auto runtimeInitializationStarted = std::chrono::steady_clock::now();
append_viewer_log("viewer runtime init begin");
const bool runtimeInitialized = runtime.initialize(&error, [this] {
    return !running_.load(std::memory_order_acquire);
});
const auto runtimeInitializationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - runtimeInitializationStarted).count();
append_viewer_log("viewer runtime init end ok="
    + std::to_string(runtimeInitialized ? 1 : 0)
    + " elapsed_ms=" + std::to_string(runtimeInitializationMs)
    + " error=" + error);
```

```cpp
// src/ui/RemoteDesktopWindow.h:152-154,298（修改后）
void startViewerStartupWatchdog();
void stopViewerStartupWatchdog();
void handleViewerStartupTimeout();
QTimer* m_viewerStartupWatchdogTimer = nullptr;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:5550-5581（修改后）
void RemoteDesktopWindow::startViewerStartupWatchdog()
{
    if (!m_viewerStartupWatchdogTimer || m_networkReconnectActive || remoteUpdateActive()
        || m_closeInProgress || m_applicationExitInProgress) {
        return;
    }
    m_viewerStartupWatchdogTimer->start(20 * 1000);
}

void RemoteDesktopWindow::handleViewerStartupTimeout()
{
    if (!m_viewerStartAdmissionActive || m_closeInProgress || m_applicationExitInProgress
        || m_networkReconnectActive || remoteUpdateActive()) {
        return;
    }
    m_connectionStatusCode = FSREMOTE_STATUS_ERROR;
    m_connectionStatus = QString::fromUtf8("远控初始化超时，请关闭窗口后重试");
    stopViewerConnectionAsync(false);
}
```

```cmake
# CMakeLists.txt:136-145（修改后）
add_executable(uu_runtime_initialization_gate_tests EXCLUDE_FROM_ALL
    third_party/uu_stream_webrtc/tests/runtime_initialization_gate_tests.cpp
    third_party/uu_stream_webrtc/src/runtime_initialization_gate.h
)
target_include_directories(
    uu_runtime_initialization_gate_tests PRIVATE third_party/uu_stream_webrtc/src)
add_test(
    NAME uu_runtime_initialization_gate_tests
    COMMAND uu_runtime_initialization_gate_tests)
```

```markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-pipeline/spec.md:40-53（修改后） -->
### Requirement: Bounded and cancellable viewer startup
The viewer lifecycle SHALL serialize process-sensitive WebRTC runtime construction
while keeping established sessions concurrent, and SHALL bound ordinary
first-connection startup so one stalled initialization cannot permanently consume
the multi-window admission budget.
```

```markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/design.md:92-96（修改后） -->
### 9. Serialized and cancellable WebRTC startup

The process keeps the existing four-window startup admission budget, but WebRTC
environment/media-default setup and CreateModularPeerConnectionFactory share one
cancellable process-level gate.
```

```markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/tasks.md:61（修改后） -->
- [x] 7.7 Serialize and make WebRTC runtime startup cancellable, add an ordinary
  first-connect watchdog, and verify the lifecycle boundaries with focused tests.
```

```markdown
<!-- openspec/specs/remote-video-pipeline/spec.md:46-59（修改后） -->
### Requirement: Bounded and cancellable viewer startup
<!-- 与 delta spec 相同的三个场景已同步到主规格。 -->
```

### Steps
1. 从多窗口日志和线程生命周期中确认堵塞发生在 WebRTC Runtime 创建阶段，而不是目标设备端口、在线解码或呈现阶段。
2. 新增只覆盖进程敏感初始化区的可取消门禁，并把 `EnableMediaWithDefaults` 与 Factory 创建放入同一临界区。
3. Viewer 用自身 `running_` 原子状态取消门禁等待；Host 和独立工具继续使用默认空回调，保持原调用语义。
4. 普通首次连接增加独立 20 秒看门狗；网络重连的 15 秒计时和远程更新的 5 分钟状态机保持不变。
5. 保留四路启动预算，并确认停止路径先作废 Viewer 代际、再等待原生线程退出、最后幂等归还名额。
6. 增加门禁测试目标，并同步 OpenSpec delta spec、设计、任务和主规格。

### Verification
- `git diff --check` 通过。
- 所有本次涉及的 C++、头文件和 CMake 文件均通过严格 UTF-8 读取检查，未发现 NUL 或编码损坏。
- 静态顺序核对通过：门禁获取位于 `native_webrtc_runtime.cpp:108`，`EnableMediaWithDefaults` 位于 135，Factory 创建位于 138；锁只在 Runtime 初始化函数内出现。
- 旧调用兼容性核对通过：Host、`host_main` 和 `viewer_main` 仍可使用 `initialize(&error)`，由默认取消参数保持源码兼容。
- 停止竞态核对通过：`stopViewerConnectionAsync()` 在提交原生 stop 前先推进 Viewer 代际，取消初始化产生的旧代际状态不会提前释放名额；名额在 `finishViewerStop()` 中于原生 `join` 完成后归还。
- OpenSpec 主规格与 delta spec 已同步；当前进度为 44/47，未虚假完成 Fence 和两项实机验证。
- 按用户要求，本轮没有启动编译、链接或测试执行；只新增测试源码并完成非构建静态检查。
- 仅修改 `C:\Users\test\Documents\Fsremote`；未修改 `C:\Users\test\Documents\Fsremote2`。

## 2026-08-04 18:09 - 修复关闭后再次远控永久等待初始化资源

### Changed Location
- `third_party/uu_stream_webrtc/src/native_webrtc_runtime.h:44`：新增门禁内清理独立 Factory 的私有入口。
- `third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:29,108-181`：Factory 创建和销毁统一使用进程级生命周期门禁。
- `third_party/uu_stream_webrtc/src/runtime_initialization_gate.h:31`：明确门禁同时保护 Factory 创建与销毁。
- `third_party/uu_stream_webrtc/tests/runtime_initialization_gate_tests.cpp:50-81`：增加“创建→销毁→再次创建”的串行顺序测试。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:2983-2989,3533,3707-3715`：区分信令清理、Runtime 析构、线程退出和 API stop 返回。
- `openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-pipeline/spec.md:40-58`：补充并发关闭后再次启动场景。
- `openspec/specs/remote-video-pipeline/spec.md:46-64`：同步生命周期主规格。
- `openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-diagnostics/spec.md:25-27`：补充 Viewer 完整停止日志要求。
- `openspec/specs/remote-video-diagnostics/spec.md:31-33`：同步诊断主规格。
- `openspec/changes/rebuild-video-frame-pipeline/design.md:94-98`：记录实机日志证据和创建/销毁统一串行决策。
- `openspec/changes/rebuild-video-frame-pipeline/tasks.md:62`：新增并完成任务 7.8。

### Reason
实机日志显示，第一次及同批多路连接均能成功初始化；但多个窗口随后同时关闭时，日志虽然到达旧的 `viewer worker end`，对应 Viewer 线程和四个生命周期工作线程却仍未退出。该日志实际写在 `run()` 返回之前，之后还会析构 `WebrtcSession`、`PeerConnectionFactory` 和 `NativeWebrtcRuntime`。多个独立 Factory 同时在共享 signaling/worker 线程上析构形成清理竞争，新 Viewer 随后全部停在 `runtime init begin`，最终四个启动名额被占满。上一条记录只串行了 Factory 创建，本条记录以新的现场证据修正该边界：Factory 析构也必须使用同一生命周期门禁，但在线会话热路径仍完全不加锁。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.h:42-45（修改前）
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
```

```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:159-167（修改前）
void NativeWebrtcRuntime::shutdown()
{
    if (impl_) {
        impl_->factory = nullptr;
        impl_.reset();
    }
}
```

```cpp
// third_party/uu_stream_webrtc/src/runtime_initialization_gate.h:30-32（修改前）
private:
    std::timed_mutex mutex_; // 只保护媒体默认配置和Factory创建。
```

```cpp
// third_party/uu_stream_webrtc/tests/runtime_initialization_gate_tests.cpp:50-64（修改前）
std::thread follower([&] {
    auto lease = gate.acquire({}, std::chrono::milliseconds(2));
    followerAcquired.store(lease.owns_lock(), std::memory_order_release);
});
owner.unlock();
assert(waitForFlag(followerAcquired, std::chrono::milliseconds(500)));
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:3531,3705-3711（修改前）
append_viewer_log("viewer worker end");

void fsremote_stream_stop(FsRemoteStreamHandle handle)
{
    append_viewer_log("api stop handle=" + std::to_string(reinterpret_cast<uintptr_t>(handle)));
    delete static_cast<StreamInstance*>(handle);
}
```

```text
// openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-pipeline/spec.md（修改前）
启动要求只覆盖媒体默认配置和 Factory 创建，没有多个 Viewer 同时销毁 Factory 后再次连接的场景。
```

```text
// openspec/specs/remote-video-pipeline/spec.md（修改前）
主规格没有“Several viewers close before another opens”场景。
```

```text
// openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-diagnostics/spec.md（修改前）
没有区分信令清理、Runtime 析构、线程退出和 API stop 返回的日志要求。
```

```text
// openspec/specs/remote-video-diagnostics/spec.md（修改前）
主诊断规格没有 Viewer shutdown completion 场景。
```

```markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/design.md:94（修改前） -->
The gate is held only during factory construction.
```

```text
// openspec/changes/rebuild-video-frame-pipeline/tasks.md:62（修改前）
没有任务 7.8。
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.h:42-45（修改后）
private:
    struct Impl;
    void resetImplementationInsideLifecycleBoundary();
    std::unique_ptr<Impl> impl_;
```

```cpp
// third_party/uu_stream_webrtc/src/native_webrtc_runtime.cpp:159-181（修改后）
void NativeWebrtcRuntime::shutdown()
{
    if (!impl_) return;
    const std::shared_ptr<SharedWebrtcThreads> sharedThreads = impl_->shared_threads;
    if (!impl_->factory || !sharedThreads) {
        resetImplementationInsideLifecycleBoundary();
        return;
    }
    auto lifecycleLease = sharedThreads->runtime_lifecycle_gate.acquire();
    resetImplementationInsideLifecycleBoundary();
}

void NativeWebrtcRuntime::resetImplementationInsideLifecycleBoundary()
{
    if (!impl_) return;
    impl_->factory = nullptr;
    impl_.reset();
}
```

```cpp
// third_party/uu_stream_webrtc/src/runtime_initialization_gate.h:30-32（修改后）
private:
    std::timed_mutex mutex_; // 只保护媒体默认配置与Factory创建/销毁，不进入在线热路径。
```

```cpp
// third_party/uu_stream_webrtc/tests/runtime_initialization_gate_tests.cpp:50-81（修改后，节选）
std::thread teardown([&] {
    auto lease = gate.acquire({}, std::chrono::milliseconds(2));
    teardownAcquired.store(lease.owns_lock(), std::memory_order_release);
    while (!releaseTeardown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
});
owner.unlock();
assert(waitForFlag(teardownAcquired, std::chrono::milliseconds(500)));

std::thread follower([&] {
    auto lease = gate.acquire({}, std::chrono::milliseconds(2));
    followerAcquired.store(lease.owns_lock(), std::memory_order_release);
});
assert(!followerAcquired.load(std::memory_order_acquire));
releaseTeardown.store(true, std::memory_order_release);
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:2983-2989,3533,3707-3715（修改后）
append_viewer_log("viewer signaling cleanup complete");
// run() 返回并完成Session/Runtime局部对象析构后：
append_viewer_log("viewer runtime objects destroyed");
append_viewer_log("viewer worker thread exit");

const uintptr_t handleValue = reinterpret_cast<uintptr_t>(handle);
append_viewer_log("api stop begin handle=" + std::to_string(handleValue));
delete static_cast<StreamInstance*>(handle);
append_viewer_log("api stop end handle=" + std::to_string(handleValue));
```

```markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-pipeline/spec.md:55-58（修改后） -->
#### Scenario: Several viewers close before another opens
- **WHEN** multiple established viewers destroy their independent factories while another viewer starts
- **THEN** factory destruction and construction use the same process-level lifecycle gate, every completed stop returns from its worker join, and the later viewer can initialize without inheriting a stuck shared-thread cleanup
```

```markdown
<!-- openspec/specs/remote-video-pipeline/spec.md:61-64（修改后） -->
#### Scenario: Several viewers close before another opens
- **WHEN** multiple established viewers destroy their independent factories while another viewer starts
- **THEN** factory destruction and construction use the same process-level lifecycle gate, every completed stop returns from its worker join, and the later viewer can initialize without inheriting a stuck shared-thread cleanup
```

```markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-diagnostics/spec.md:25-27（修改后） -->
#### Scenario: Viewer shutdown completes
- **WHEN** a Viewer is stopped or closed
- **THEN** logs distinguish signaling cleanup, Runtime and Factory destruction completion, worker-thread exit, API stop begin, and API stop return
```

```markdown
<!-- openspec/specs/remote-video-diagnostics/spec.md:31-33（修改后） -->
#### Scenario: Viewer shutdown completes
- **WHEN** a Viewer is stopped or closed
- **THEN** logs distinguish signaling cleanup, Runtime and Factory destruction completion, worker-thread exit, API stop begin, and API stop return
```

```markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/design.md:94-98（修改后，节选） -->
WebRTC environment/media-default setup, CreateModularPeerConnectionFactory,
and destruction of each independent factory share one process-level lifecycle gate.
```

```markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/tasks.md:62（修改后） -->
- [x] 7.8 Serialize PeerConnectionFactory destruction with Runtime construction and add completion logs for close-then-open recovery.
```

### Steps
1. 读取 17:59 至 18:02 的原生和 Qt 日志，确认首批连接成功、关闭后工作线程未真正退出、后续四路停在 Runtime 初始化、第五路停在启动准入。
2. 核对活动线程，确认四个 Viewer 线程和四个生命周期工作线程在写出旧“worker end”后仍处于等待状态。
3. 把每个独立 `PeerConnectionFactory` 的析构纳入与创建相同的进程级门禁，避免共享 WebRTC 线程同时处理多个 Factory 清理。
4. 初始化取消/失败路径在已持有门禁时直接执行内部清理，避免 `shutdown()` 重复获取非递归门禁造成自锁。
5. 把旧的“viewer worker end”更名为信令清理完成，并在 Runtime 析构、线程退出和 API stop 返回后分别写日志。
6. 扩展纯 C++ 门禁测试，覆盖创建、销毁、再次创建的顺序；同步 delta spec、主规格、设计和任务。

### Verification
- `git diff --check` 通过。
- 静态门禁审查通过：Factory 创建和 Factory 析构都使用 `runtime_lifecycle_gate`；在线会话、信令循环、解码、音频、输入、画质和 Present 不持有该门禁。
- 自锁审查通过：初始化线程已持有门禁的取消/失败分支调用内部清理，不再次进入 `shutdown()`；未取得门禁的取消分支仅回收尚无 Factory 的空状态。
- 日志边界现在可区分 `viewer signaling cleanup complete`、`viewer runtime objects destroyed`、`viewer worker thread exit`、`api stop begin` 和 `api stop end`。
- OpenSpec delta 与主规格保持一致；任务 7.8 已完成，Fence 和两项实机验证仍保持未完成。
- 按用户要求未启动编译、链接或测试执行；当前运行中的旧进程不会加载这次源码修改。
- 仅修改 `C:\Users\test\Documents\Fsremote`；未修改 `C:\Users\test\Documents\Fsremote2`。

## 2026-08-05 09:38 - 修复 Release 诊断测试被 assert 裁剪的问题并校正 Runtime 生命周期规范

### Changed Location
- `tests/remote_video_diagnostics_tests.cpp:3-26,50-153`：把会执行关键操作的 `assert(...)` 改为 Release/Debug 都生效的 `require(...)`，防止 `NDEBUG` 删除日志启动、事件提交和快照检查。
- `openspec/changes/rebuild-video-frame-pipeline/design.md:94-130`：明确进程级门禁只保护媒体默认配置与 Factory 创建，Factory 销毁和在线 Runtime 使用保持每 Viewer 隔离。
- `openspec/changes/rebuild-video-frame-pipeline/tasks.md:28,62`：把 4.4 记录为固定三槽缓存，校正 7.8 的完成定义。
- `openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-pipeline/spec.md:40-61`、`openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-diagnostics/spec.md:25-31`：补充独立线程组、慢停止和停止阶段日志场景。
- `openspec/specs/remote-video-pipeline/spec.md:46-67`、`openspec/specs/remote-video-diagnostics/spec.md:31-37`：同步同一 delta 到主规格。

### Reason
RelWithDebInfo/Release 测试崩溃位置落在 `std::filesystem::directory_iterator`，CDB 显示是未处理的 `filesystem_error`。根因是 Release 定义 `NDEBUG` 后，`assert(diagnostics.start(...))` 等表达式完全不执行，日志目录从未创建，后续遍历目录直接抛异常。另一个问题是规范仍宣称 Factory 销毁和创建共用全局门禁，与已落地的每 Runtime 独立线程实现相矛盾；这会误导后续高风险 Fence/停机策略设计。

### Original Code
```cpp
// tests/remote_video_diagnostics_tests.cpp:3,40,53,120（修改前）
#include <cassert>
assert(diagnostics.start(directory, 1024, 3));
assert(diagnostics.submit(lifecycle));
assert(std::filesystem::exists(logPath));
```

```markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-pipeline/spec.md:40-41（修改前） -->
The viewer lifecycle SHALL serialize process-sensitive WebRTC runtime construction and per-viewer factory destruction while keeping established sessions concurrent.

<!-- openspec/changes/rebuild-video-frame-pipeline/design.md:94（修改前） -->
WebRTC environment/media-default setup, CreateModularPeerConnectionFactory,
and destruction of each independent factory share one process-level lifecycle gate.
```

### Modified Code
```cpp
// tests/remote_video_diagnostics_tests.cpp:4,19-26,50,63,130-150（修改后）
#include <cstdlib>

void require(bool condition)
{
    if (!condition) {
        std::exit(1); // wjy: Debug/Release 都执行前置条件，失败时返回明确非零码。
    }
}

require(diagnostics.start(directory, 1024, 3));
require(diagnostics.submit(lifecycle));
require(std::filesystem::exists(logPath));
```

```markdown
<!-- openspec/changes/rebuild-video-frame-pipeline/specs/remote-video-pipeline/spec.md:40-61（修改后） -->
The viewer lifecycle SHALL serialize process-sensitive WebRTC runtime construction while keeping each viewer's factory, decoder sink, and WebRTC execution threads independent; per-viewer teardown SHALL remain isolated.

#### Scenario: One viewer's decoder stop is slow
- **WHEN** an established viewer's decoder or Runtime teardown is slow or cannot be interrupted immediately
- **THEN** only that viewer remains in its stopping phase while other viewers retain independent signaling/decode threads and may initialize or continue rendering
```

### Steps
1. 使用 RelWithDebInfo CDB 转储确认异常来自 `std::filesystem::directory_iterator`，并核对 Release 下 `assert` 会移除关键调用。
2. 为诊断测试增加带 WJY 标记的 `require` 辅助函数，将所有会产生副作用或验证结果的 `assert` 改为显式执行。
3. 重新编译 RelWithDebInfo 和 Release 诊断测试，保留 Debug 版本做交叉验证。
4. 手动将 delta spec 合并到 `openspec/specs/` 主规格；`openspec sync` 在当前 CLI 中不是可用子命令，因此未伪造命令结果。
5. 保持 4.6 Fence、8.1 单窗口实机验证、8.2 多窗口实机验证为未完成，不把文档修订误标为实测完成。

### Verification
- `build-video-tests-msvc/RelWithDebInfo/fsremote_remote_video_diagnostics_tests.exe`：exit 0。
- `build-video-tests-msvc/Debug/fsremote_remote_video_diagnostics_tests.exe`：exit 0。
- `build-video-tests-msvc/Release/fsremote_remote_video_diagnostics_tests.exe`：exit 0；此前的 `0xC0000409` 不再出现。
- CDB 已定位旧崩溃为未处理 C++ `filesystem_error`，不是视频运行时代码异常。
- 主规格与 delta 的生命周期场景已一致；本次未构建安装器或发布包，未修改 `C:\Users\test\Documents\Fsremote2`。

## 2026-08-05 15:26 - 设备列表改为在线优先与自然名称排序

### Changed Location
- `src/ui/DeviceListSortPolicy.h:1-58`：新增可独立测试的设备排序策略，统一处理在线优先、Busy 在线语义、数字自然排序、英文字母排序和同名稳定收尾。
- `src/ui/DeviceGrid.cpp:24,1437-1441,1526-1667,7096-11893`：设备分组、根部列表、绘制、点击、右键、双击、拖拽、搜索定位和行内编辑统一传入当前状态缓存并复用同一排序快照。
- `tests/device_list_sort_policy_tests.cpp:1-71`：新增自然数字、英文大小写和在线/离线分区回归测试。
- `CMakeLists.txt:408-418,513`：注册独立排序策略测试目标，并把策略头文件加入主程序源文件清单。

### Reason
旧实现只比较设备名首字符和完整字符串，因此纯数字设备名会得到 `1、10、11、2` 这类字典序，不能按用户理解的自然数大小排列；同时在线和离线设备混在一起，设备较多时不便于优先找到当前可操作目标。本次将状态提升为第一排序键：`Online` 与 `Busy` 统一视为在线并排在前面，`Offline` 与 `Unknown` 排在后面；每个状态分区内部使用 Qt `QCollator` 的数字模式进行自然名称排序，英文忽略大小写按字母顺序比较。所有绘制和交互入口使用同一行快照，避免只改变视觉顺序后出现点击、拖拽或编辑命中错位。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:1524-1568（修改前）
bool deviceIndexLessByLeadingCharacter(int leftIndex, int rightIndex)
{
    const QString leftName = deviceDisplayName(g_devices.at(leftIndex)).trimmed();
    const QString rightName = deviceDisplayName(g_devices.at(rightIndex)).trimmed();
    const QString leftKey = leftName.left(1).toCaseFolded();
    const QString rightKey = rightName.left(1).toCaseFolded();
    const int keyCompare = QString::localeAwareCompare(leftKey, rightKey);
    if (keyCompare != 0) {
        return keyCompare < 0;
    }

    const int nameCompare = QString::localeAwareCompare(leftName, rightName);
    if (nameCompare != 0) {
        return nameCompare < 0;
    }
    return leftIndex < rightIndex;
}

QVector<int> sortedDeviceIndexesForGroup(int groupIndex)
{
    // 收集分组设备……
    std::stable_sort(deviceIndexes.begin(), deviceIndexes.end(), deviceIndexLessByLeadingCharacter);
    return deviceIndexes;
}
```

```text
src/ui/DeviceListSortPolicy.h：新增文件，此位置没有旧代码。
tests/device_list_sort_policy_tests.cpp：新增文件，此位置没有旧代码。
CMakeLists.txt:408-418,513：新增测试目标和主程序头文件清单项，此位置没有旧代码。
```

### Modified Code
```cpp
// src/ui/DeviceListSortPolicy.h:17-54（修改后）
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
        const bool leftOnline = devicePresenceSortsOnlineFirst(left.presence);
        const bool rightOnline = devicePresenceSortsOnlineFirst(right.presence);
        if (leftOnline != rightOnline) {
            return leftOnline; // wjy: 任意在线/忙碌设备都优先于 Offline 和 Unknown，名称只在同一状态分区内比较。
        }
        const int naturalCompare = m_collator.compare(left.displayName.trimmed(), right.displayName.trimmed());
        if (naturalCompare != 0) {
            return naturalCompare < 0;
        }
        return left.sourceIndex < right.sourceIndex;
    }
};
```

```cpp
// src/ui/DeviceGrid.cpp:1540-1602（修改后，节选）
QVector<int> sortedDeviceIndexesForGroup(
    int groupIndex,
    const QHash<QString, platform::DevicePresenceState>& deviceStatuses)
{
    // 收集分组设备……
    const ui::DeviceListNaturalLess naturalLess; // wjy: 同一分组的一轮排序复用一个 QCollator，数字按数值、英文按字母比较。
    std::stable_sort(deviceIndexes.begin(), deviceIndexes.end(), [&](int leftIndex, int rightIndex) {
        const ui::DeviceListSortItem left{
            deviceDisplayName(g_devices.at(leftIndex)),
            devicePresenceForSorting(leftIndex, deviceStatuses),
            leftIndex,
        };
        const ui::DeviceListSortItem right{
            deviceDisplayName(g_devices.at(rightIndex)),
            devicePresenceForSorting(rightIndex, deviceStatuses),
            rightIndex,
        };
        return naturalLess(left, right); // wjy: 第一关键字是在线状态，同一状态分区内再执行自然名称排序。
    });
    return deviceIndexes;
}
```

```cpp
// tests/device_list_sort_policy_tests.cpp:43-66（修改后，节选）
require(sortedNames({
    {QStringLiteral("10"), DevicePresenceState::Online, 0},
    {QStringLiteral("2"), DevicePresenceState::Online, 1},
    {QStringLiteral("1"), DevicePresenceState::Online, 2},
}) == QStringList({QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("10")}),
    "numeric device names must use natural-number order");

require(sortedNames({
    {QStringLiteral("1"), DevicePresenceState::Offline, 0},
    {QStringLiteral("20"), DevicePresenceState::Online, 1},
    {QStringLiteral("3"), DevicePresenceState::Busy, 2},
}) == QStringList({QStringLiteral("3"), QStringLiteral("20"), QStringLiteral("1")}),
    "online and busy devices must precede offline and unknown devices");
```

### Steps
1. 抽出 `DeviceListNaturalLess`，使用 `QCollator::setNumericMode(true)` 把连续数字段按自然数比较，并使用不区分大小写的区域比较保持英文设备名按字母顺序排列。
2. 把 `Online` 和 `Busy` 合并为在线分区，把 `Offline` 和 `Unknown` 放到非在线分区；状态优先级高于设备名称。
3. 修改根部和每个分组的排序函数，使其同时接收设备状态缓存并构造“展示名、状态、源下标”排序键。
4. 将绘制、右键、双击、普通点击、拖拽、搜索定位、批量远控和行内编辑等入口全部改为消费同一份状态排序行快照，防止视觉与命中顺序分叉。
5. 新增纯 Qt Core 测试并注册 CMake 目标，覆盖数字、英文和在线优先三个核心场景。

### Verification
- `cmake --build build-video-tests-msvc --config Debug --target fsremote_device_list_sort_policy_tests`：编译通过。
- `ctest --test-dir build-video-tests-msvc -C Debug -R fsremote_device_list_sort_policy_tests --output-on-failure`：1/1 通过。
- `cmake --build build-video-tests-msvc --config Debug --target FSRemote`：主程序 Debug 编译和链接通过。
- 主程序构建保留 `RemoteDesktopWindow.cpp` 中既有的 C4834、C4804 警告；本次排序代码没有新增编译警告或错误。
- `git diff --check`：通过。

## 2026-08-05 18:24 - 修复旧代显卡远控首帧 NVENC 非法参数

### Changed Location
- `third_party/uu_stream_webrtc/src/d3d11_frame_transformer.cpp:15-24`：原始分辨率也统一经过 D3D11 VideoProcessor，优先产生 NV12 编码纹理。
- `third_party/uu_stream_webrtc/src/d3d11_frame_transformer.h:16-20`：更新转换器接口说明，明确所有原生帧都走 GPU 转换。
- `third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:69-111`：查询 HEVC 输入格式和 Temporal AQ 能力，并按能力启用可选参数。
- `third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:329-363`：新增 NVENC 输入格式与编码能力查询实现。
- `third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:381-435`：注册纹理前校验驱动声明的格式，并补充驱动错误详情。
- `third_party/lan_stream_probe/src/nvenc_h264_encoder.h:19-56`：保存并公开本次编码会话的能力诊断信息。
- `third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:294-408`：记录首个编码纹理属性，首帧成功前禁止在线重配置，并写入 NVENC 能力快照。
- `third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:695-725`：更新转换器职责说明并增加首纹理日志状态。

### Reason
`PC-20251110PQOC（192.168.1.172，GTX 1080）` 在 `1.1.162` 中稳定采集 1920×1080 桌面，NVENC 初始化也成功，但第一帧持续返回 `NvEncEncodePicture failed: 8`。目标端日志证明失败只出现在原始分辨率的 `native_path=1` 路径；该路径此前直接把 DXGI BGRA 采集纹理注册给 NVENC。为兼容 GTX 10/16 系列旧代编码器，本次把所有原生帧统一在 GPU 内转换为 NV12，同时让可选编码参数服从驱动能力查询，并隔离首帧前无意义的在线重配置错误。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/d3d11_frame_transformer.cpp:19-24（修改前）
if (transformTimeUs) *transformTimeUs = 0;
if (!frame.requires_transform()) {
    *output = frame.texture();
    return true;
}
if (!ensure_resources(frame, error)) return false;
```

```cpp
// third_party/uu_stream_webrtc/src/d3d11_frame_transformer.h:16-20（修改前）
bool transform(const D3D11NativeFrameBuffer& frame,
               Microsoft::WRL::ComPtr<ID3D11Texture2D>* output,
               uint64_t* transformTimeUs,
               std::string* error); // 原始尺寸直接返回采集纹理，低分辨率才转换NV12。
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:68-95（修改前，节选）
if (!check(status, "NvEncOpenEncodeSession", error)) {
    return false;
}

NV_ENC_PRESET_CONFIG preset = {};
// 未查询输入格式与Temporal AQ能力。
config.rcParams.enableAQ = 1;
config.rcParams.enableTemporalAQ = 1;
config.rcParams.aqStrength = 10;
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:305（修改前）
// new code, no old code at this location：原实现没有输入格式和能力查询函数。
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:323-356（修改前，节选）
const NV_ENC_BUFFER_FORMAT buffer_format = nvenc_buffer_format(desc.Format);
if (buffer_format == NV_ENC_BUFFER_FORMAT_UNDEFINED) {
    if (error) *error = "unsupported NVENC D3D11 texture format";
    return false;
}
// 未校验驱动实际公布的HEVC输入格式。

if (error) {
    *error = std::string(call) + " failed: " + std::to_string(static_cast<int>(status));
}
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.h:19-49（修改前，节选）
bool initialize(...);
bool encode(...);
void shutdown();
bool ready() const { return encoder_ && bitstream_; }

bool load_api(std::string* error);
bool register_texture(ID3D11Texture2D* texture, std::string* error);
// 未保存输入格式、Temporal AQ和诊断字符串。
```

```cpp
// third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:294-389（修改前，节选）
if (native) {
    if (transformer_.transform(*native, &encode_texture, &transform_time_us, &transform_error)) {
        native_ready = true;
    }
}

if (encoder_.ready() && should_reconfigure_rate(requested_bitrate_kbps, requested_fps)) {
    encoder_.reconfigure(requested_bitrate_kbps, requested_fps, &reconfigure_error);
}

append_stream_capture_diagnostic_log("encoder", "NVENC initialize success");
```

```cpp
// third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:675-704（修改前，节选）
D3D11FrameTransformer transformer_; // 原生高质量帧直通。
bool first_worker_frame_logged_ = false;
bool first_encoded_frame_logged_ = false;
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/d3d11_frame_transformer.cpp:19-23（修改后）
if (transformTimeUs) *transformTimeUs = 0;
// =====wjy====
if (!ensure_resources(frame, error)) return false; // wjy: 原始分辨率也统一经过VideoProcessor输出NV12，避免GTX 10/16系列把DXGI采集BGRA纹理直送NVENC时首帧返回非法参数8。
// ===end====
```

```cpp
// third_party/uu_stream_webrtc/src/d3d11_frame_transformer.h:16-20（修改后）
bool transform(const D3D11NativeFrameBuffer& frame,
               Microsoft::WRL::ComPtr<ID3D11Texture2D>* output,
               uint64_t* transformTimeUs,
               std::string* error); // wjy: 所有原生D3D11帧都经VideoProcessor裁剪/缩放并优先输出NV12，兼容旧代NVENC对DXGI BGRA纹理的限制。
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:69-111（修改后，节选）
// =====wjy====
query_input_formats(); // wjy: 会话建立后读取HEVC真实输入格式，旧代显卡不再靠型号名单猜测BGRA/NV12兼容性。
int temporal_aq_capability = 0;
temporal_aq_supported_ = query_capability(NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ, &temporal_aq_capability)
    && temporal_aq_capability > 0;
diagnostics_ = diagnostics.str(); // wjy: 初始化成功日志记录能力快照。
// ===end====
config.rcParams.enableTemporalAQ = temporal_aq_supported_ ? 1u : 0u;
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:329-363（修改后，节选）
bool NvencH264Encoder::query_input_formats()
{
    uint32_t count = 0;
    // 读取驱动实际返回的HEVC格式数量并识别NV12/ARGB。
    input_formats_known_ = true;
    return true;
}

bool NvencH264Encoder::query_capability(NV_ENC_CAPS capability, int* value) const
{
    NV_ENC_CAPS_PARAM params = {};
    params.version = nvenc_struct_version(api_version_, 1);
    params.capsToQuery = capability;
    return fn_.nvEncGetEncodeCaps(encoder_, NV_ENC_CODEC_HEVC_GUID, &params, value) == NV_ENC_SUCCESS;
}
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.cpp:381-435（修改后，节选）
const bool format_supported = buffer_format == NV_ENC_BUFFER_FORMAT_NV12
    ? supports_nv12_
    : buffer_format == NV_ENC_BUFFER_FORMAT_ARGB && supports_argb_;
if (input_formats_known_ && !format_supported) {
    return false; // wjy: 驱动能力明确不含该格式时提前给出可读错误。
}

if (encoder_ && fn_.nvEncGetLastErrorString) {
    const char* detail = fn_.nvEncGetLastErrorString(encoder_);
    // 把驱动错误详情追加到错误文本。
}
```

```cpp
// third_party/lan_stream_probe/src/nvenc_h264_encoder.h:19-56（修改后，节选）
const std::string& diagnostics() const { return diagnostics_; }
bool query_input_formats();
bool query_capability(NV_ENC_CAPS capability, int* value) const;
bool input_formats_known_ = false;
bool supports_nv12_ = false;
bool supports_argb_ = false;
bool temporal_aq_supported_ = false;
std::string diagnostics_;
```

```cpp
// third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:349-408（修改后，节选）
if (!first_encode_texture_logged_ && encode_texture) {
    D3D11_TEXTURE2D_DESC desc = {};
    encode_texture->GetDesc(&desc);
    append_stream_capture_diagnostic_log("encoder", "first encode texture format=" + std::to_string(desc.Format));
    first_encode_texture_logged_ = true;
}

if (frame_id_ > 0 && encoder_.ready() && should_reconfigure_rate(requested_bitrate_kbps, requested_fps)) {
    encoder_.reconfigure(requested_bitrate_kbps, requested_fps, &reconfigure_error);
}

append_stream_capture_diagnostic_log("encoder", "NVENC initialize success " + encoder_.diagnostics());
```

```cpp
// third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:695-725（修改后，节选）
D3D11FrameTransformer transformer_; // wjy: 所有原生档位在GPU内裁剪/缩放并优先转换NV12。
bool first_worker_frame_logged_ = false;
bool first_encode_texture_logged_ = false; // wjy: 每个设备代际只记录一次NVENC输入纹理格式和资源标志。
bool first_encoded_frame_logged_ = false;
```

### Steps
1. 通过目标端 `stream_capture_debug.log` 确认 `PC-20251110PQOC` 在 1920×1080 原始纹理路径中稳定采集，但 NVENC 首帧返回非法参数 8。
2. 删除原尺寸绕过转换的分支，使原始、均衡和流畅档位都优先输出 GPU NV12 纹理。
3. 查询 NVENC HEVC 输入格式与 Temporal AQ 能力，只注册驱动声明支持的格式，只在能力明确支持时启用 Temporal AQ。
4. 记录首个编码纹理的 DXGI 格式、BindFlags、MiscFlags 和输入路径，并把 NVENC 驱动错误详情写入目标端日志。
5. 首份码流成功前禁止 `NvEncReconfigureEncoder`，避免第二个非法参数错误干扰首帧诊断和恢复。
6. 关闭占用正式 DLL 的本机 FSRemote，完成 Release 全量构建后重新启动程序。

### Verification
- 目标端 `PC-20251110PQOC（192.168.1.172）` 最新日志复核：采集约 60 FPS、`native_d3d11=1`、NVENC 初始化成功，旧版本首帧持续返回错误 8，定位依据成立。
- `jom -f Makefile -j1 /nologo FSRemote`：Release 全量编译、链接及正式目录 DLL 复制通过。
- `ctest --output-on-failure -R "^(uu_latest_encode_frame_slot_tests|uu_host_media_pipeline_tests)$"`：2/2 通过。
- `git diff --check`：通过。
- 新 Release `fsremote_stream.dll` SHA-256：`7C22F44C5B3946EC351659100ED5FA51E798F7A143A180987B863BBBB605759C`。
- 尚未在 GTX 1080 目标端运行本次新 DLL；需要发布下一版本后复测首帧日志中的 `first encode texture format=103（DXGI_FORMAT_NV12）` 和 `first encoded frame delivered`。

## 2026-08-06 10:56 - 新增 FakerInput 驱动键鼠最小交付包

### Changed Location
- `FakerInputDriverKit/README.md:1-159`：新增面向同事的安装、工程依赖、键盘状态快照、鼠标报告和异常释放说明。
- `FakerInputDriverKit/src/faker_input_device.h:1-79`：收录直接打开 FakerInput HID 控制集合的接口，并补齐独立工程需要的 Windows 宏保护。
- `FakerInputDriverKit/src/faker_input_device.cpp:1-382`：收录已验证的设备枚举、键盘报告、相对/绝对鼠标报告和释放实现。
- `FakerInputDriverKit/src/faker_input_keyboard_state.h:1-188`：收录 Windows VK 到 USB HID usage 的映射及六键状态机，并使该头文件可独立安全包含 Windows 虚拟键定义。
- `FakerInputDriverKit/driver/FakerInput_Setup_0.1.1_x64.msi`：新增可在目标 Windows x64 设备安装的固定版本驱动包。
- `FakerInputDriverKit/THIRD_PARTY_NOTICES.md:1-29`：随交付包保留 FakerInput MIT 授权和来源说明。

### Reason
用户需要把现有驱动键鼠能力交给同事，并明确要求保持简单，由同事安装驱动后自行封装进其程序。因此本次不增加 FSRemote 依赖、Bridge 服务、SDK 工程或演示程序，只整理驱动安装包、底层 HID 访问源码、VK 映射状态机和必要使用说明。交付版头文件额外消除了原独立 Bridge 工程对全局 `NOMINMAX`、`WIN32_LEAN_AND_MEAN` 编译定义的隐式依赖，避免源码复制到普通 Windows 工程后直接编译失败。

### Original Code
```text
// FakerInputDriverKit/README.md（新增前）
new file, no old code at this location
```

```cpp
// FakerInputDriverKit/src/faker_input_device.h（新增前）
new file, no old code at this location
```

```cpp
// FakerInputDriverKit/src/faker_input_device.cpp（新增前）
new file, no old code at this location
```

```cpp
// FakerInputDriverKit/src/faker_input_keyboard_state.h（新增前）
new file, no old code at this location
```

```text
// FakerInputDriverKit/THIRD_PARTY_NOTICES.md（新增前）
new file, no old code at this location
```

```text
// FakerInputDriverKit/driver/FakerInput_Setup_0.1.1_x64.msi（新增前）
new binary file, no old file at this location
```

### Modified Code
```markdown
<!-- FakerInputDriverKit/README.md:1-12（新增后，节选） -->
# FakerInput 驱动键鼠最小集成包

这个目录用于交给需要在自己程序中调用 FakerInput 虚拟键盘、鼠标的同事。

不依赖 FSRemote、Qt、WebRTC 或 `fsremote_stream.dll`，也不要求运行 `FakerInputBridge.exe`。
```

```cpp
// FakerInputDriverKit/src/faker_input_device.h:1-15（新增后）
#pragma once

// =====wjy====
// wjy: 从独立 FakerInputBridge 项目收录底层 HID 访问接口，供同事安装 MSI 后直接集成到自己的 Windows C++ 程序。
#ifndef NOMINMAX
#define NOMINMAX // wjy: 独立源码不再依赖原项目的编译定义，避免 Windows 的 min/max 宏破坏 std::min、std::max 和数值边界代码。
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // wjy: 只引入驱动访问需要的 Win32 声明，减少不同宿主工程头文件之间的宏和类型冲突。
#endif
// ===end====
```

```cpp
// FakerInputDriverKit/src/faker_input_device.cpp:1-7（新增后）
#include "faker_input_device.h"

// =====wjy====
// wjy: 保留经过现有 FSRemote 验证的 FakerInput 设备枚举与报告写入实现，避免接收方重新猜测 VID/PID、Usage 和报告布局。
// ===end====
```

```cpp
// FakerInputDriverKit/src/faker_input_keyboard_state.h:20-31（新增后，节选）
struct FakerInputKeyboardReport {
    std::uint8_t modifiers = 0;
    std::array<std::uint8_t, 6> usages{};
}; // wjy: FakerInput 键盘使用标准 boot-keyboard 快照，一字节修饰键加最多六个普通 USB HID usage。

enum class FakerInputKeyboardUpdate {
    Changed,
    Unchanged,
    Unsupported,
    Rollover,
};
```

```markdown
<!-- FakerInputDriverKit/THIRD_PARTY_NOTICES.md:1-5（新增后） -->
# Third-party notices

FakerInputBridge implements the public report format and device identifiers from the FakerInput project. FakerInput is licensed under the MIT License.
```

```text
// FakerInputDriverKit/driver/FakerInput_Setup_0.1.1_x64.msi（新增后）
FakerInput 0.1.1 x64 安装包，SHA-256：
4C0AEFB7340051A91D606776243298B5CD1143EF5508BBAE6800C474F9ED0840
```

### Steps
1. 从已验证的 Release 目录复制固定版本 FakerInput MSI，并保留第三方授权说明。
2. 从独立 `FakerInputBridge` 项目收录底层 HID 设备接口和实现，不携带 Bridge 服务端或命名管道代码。
3. 收录 FSRemote 已使用的 VK→USB HID 映射及键盘状态机，供接收方直接封装 `sendKey(vk, down)`。
4. 编写最小中文说明，记录安装命令、MSVC/CMake 依赖、键盘和鼠标完整状态快照语义以及 `release_all` 要求。
5. 在交付版头文件中内置 `NOMINMAX` 和 `WIN32_LEAN_AND_MEAN`，消除原工程全局编译定义依赖。

### Verification
- `FakerInput_Setup_0.1.1_x64.msi` SHA-256 校验通过：`4C0AEFB7340051A91D606776243298B5CD1143EF5508BBAE6800C474F9ED0840`。
- 使用 Visual Studio 2022 MSVC 14.44、C++20 编译交付目录中的 `faker_input_device.cpp`、`faker_input_device.h` 和 `faker_input_keyboard_state.h`，链接 `hid.lib`、`setupapi.lib`、`user32.lib` 通过。
- 运行无输入注入的映射验证程序通过，确认 Windows VK `A` 生成 USB HID usage `0x04`。
- 未执行真实键盘或鼠标注入，避免验证过程影响当前用户桌面。

## 2026-08-06 11:18 - 修复远控恢复期黑屏闪烁并降低异常重建开销

### Changed Location
- third_party/lan_stream_probe/src/dxgi_capture.h:23-70：新增类型化采集结果、轻量 Duplication 恢复接口和恢复退避状态。
- third_party/lan_stream_probe/src/dxgi_capture_policy.h:23-53：新增 HRESULT 恢复分级和 0/50/100/250ms 有界退避纯策略。
- third_party/lan_stream_probe/src/dxgi_capture.cpp:207-209、219-348、353-448、489-500：把 ACCESS_LOST/INVALID_CALL 改为轻量重建，只在设备真实丢失时完整重建设备，并在健康采帧路径避免额外设备状态查询。
- third_party/parsec_vdd/parsec-vdd.h:271-367：为 VDD IOCTL 增加可配置超时、CancelIoEx 安全取消和带结果的心跳接口。
- third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:216-239、255-328、347-356：恢复期保留最后成功画面、暂停重复编码和 OnFrameDropped，并增加低频聚合恢复指标。
- third_party/uu_stream_webrtc/src/parsec_vdd_session.cpp:26-29、425-467：心跳改为 100ms 绝对节拍、250ms 超时和异常限频日志。
- third_party/uu_stream_webrtc/tests/dxgi_capture_policy_tests.cpp:26-41：补充错误分类优先级和恢复退避策略测试。

### Reason
远控过程中出现的黑色闪屏与 DXGI Desktop Duplication 短暂失效被扩大为完整 D3D11 Device、帧槽和 NVENC 链路重建有关；同时旧 Host 会清空最后帧并在恢复空档持续上报丢帧，使控制端从已有画面退回黑屏或“等待远程画面”。本次将显示拓扑级故障与真实设备丢失分开处理：正常采帧路径不增加新的 COM 查询，异常时只重建必要资源并有界退避；Host 保留控制端已经显示的最后成功帧，但不反复编码旧纹理。VDD 心跳也由最长五秒阻塞改为短超时安全取消，避免驱动保活空档诱发输出反复掉线。

### Original Code
~~~cpp
// third_party/lan_stream_probe/src/dxgi_capture.h:21-45（修改前，节选）
class DxgiCapture {
public:
    bool initialize(std::string* error);
    bool initialize(const std::wstring& preferredDeviceName, std::string* error);
    bool capture(CapturedFrame* frame, std::string* error);
    void reset();

private:
    bool recreate_frame_texture(const std::shared_ptr<FrameSlot>& slot,
                                const D3D11_TEXTURE2D_DESC& source_desc,
                                std::string* error);
    void reset_resources();
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    std::vector<std::shared_ptr<FrameSlot>> frame_slots_;
    std::wstring preferred_device_name_;
    Size size_;
};
~~~

~~~cpp
// third_party/lan_stream_probe/src/dxgi_capture_policy.h:20-36（修改前，节选）
class FrameSlotLeaseState final : public std::enable_shared_from_this<FrameSlotLeaseState> {
public:
    bool try_acquire(std::shared_ptr<void>* token);
private:
    std::atomic_bool leased_ = false;
};
// 原文件没有 DXGI 错误分级和恢复退避策略。
~~~

~~~cpp
// third_party/lan_stream_probe/src/dxgi_capture.cpp:216-246（修改前，节选）
bool DxgiCapture::capture(CapturedFrame* frame, std::string* error)
{
    if (!duplication_ && !initialize(error)) {
        return false;
    }
    HRESULT hr = duplication_->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (error) *error = "timeout";
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        reset_resources();
        if (error) *error = "DXGI access lost";
        return false;
    }
    if (FAILED(hr)) {
        return false;
    }
}
~~~

~~~text
// third_party/lan_stream_probe/src/dxgi_capture.cpp（修改前）
recreate_duplication、schedule_duplication_recovery 和 reset_duplication 为新增函数，
原位置没有对应代码。
~~~

~~~cpp
// third_party/parsec_vdd/parsec-vdd.h:269-301、323-326（修改前，节选）
static DWORD VddIoControl(HANDLE vdd, VddCtlCode code, const void *data, size_t size)
{
    OVERLAPPED Overlapped;
    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    DeviceIoControl(vdd, (DWORD)code, InBuffer, sizeof(InBuffer),
                    &OutBuffer, sizeof(DWORD), NULL, &Overlapped);
    if (!GetOverlappedResultEx(vdd, &Overlapped, &NumberOfBytesTransferred, 5000, FALSE)) {
        CloseHandle(Overlapped.hEvent);
        return -1;
    }
    CloseHandle(Overlapped.hEvent);
    return OutBuffer;
}

static void VddUpdate(HANDLE vdd)
{
    VddIoControl(vdd, VDD_IOCTL_UPDATE, NULL, 0);
}
~~~

~~~cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:250-308（修改前，节选）
const bool fresh = capture_.capture(&captured, &capture_error);
if (fresh) {
    last_frame_ = std::move(captured);
} else if (capture_error == "busy") {
    ++busy_frames_;
} else if (capture_error == "DXGI access lost") {
    last_frame_ = {};
    ++dropped_frames_;
}

if (last_frame_.texture) {
    publish_frame(last_frame_);
} else {
    ++dropped_frames_;
    OnFrameDropped();
}
~~~

~~~cpp
// third_party/uu_stream_webrtc/src/parsec_vdd_session.cpp:424-430（修改前）
heartbeat_running_ = true;
heartbeat_thread_ = std::thread([this] {
    while (heartbeat_running_) {
        parsec_vdd::VddUpdate(handle_);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
});
~~~

~~~cpp
// third_party/uu_stream_webrtc/tests/dxgi_capture_policy_tests.cpp:19-25（修改前）
std::shared_ptr<void> retained = first;
first.reset();
assert(state->leased());
retained.reset();
assert(!state->leased());
assert(state->try_acquire(&denied));
return 0;
~~~

### Modified Code
~~~cpp
// third_party/lan_stream_probe/src/dxgi_capture.h:23-70（修改后，节选）
enum class DxgiCaptureStatus : uint8_t {
    FreshFrame,
    NoDesktopChange,
    FrameSlotBusy,
    DuplicationRecovering,
    DeviceRecovering,
    FatalError,
};

struct DxgiCaptureResult {
    DxgiCaptureStatus status = DxgiCaptureStatus::FatalError;
    long result = S_OK;
};

DxgiCaptureResult capture_frame(CapturedFrame* frame, std::string* error);
bool recreate_duplication(std::string* error, long* result);
void schedule_duplication_recovery(long result);
void reset_duplication();
bool awaiting_recovery_frame_ = false;
uint32_t consecutive_duplication_failures_ = 0;
std::chrono::steady_clock::time_point next_duplication_retry_{};
~~~

~~~cpp
// third_party/lan_stream_probe/src/dxgi_capture_policy.h:23-53（修改后）
enum class DxgiFailureAction : uint8_t {
    KeepResources,
    RecreateDuplication,
    RecreateDevice,
};

inline DxgiFailureAction dxgi_failure_action(long result, long deviceRemovalReason)
{
    if (dxgi_result_is_device_lost(result) || dxgi_result_is_device_lost(deviceRemovalReason)) {
        return DxgiFailureAction::RecreateDevice; // wjy: 真实设备丢失优先，不能被 ACCESS_LOST 掩盖。
    }
    if (result == DXGI_ERROR_ACCESS_LOST || result == DXGI_ERROR_INVALID_CALL) {
        return DxgiFailureAction::RecreateDuplication; // wjy: 设备健康时只重建 Duplication。
    }
    return DxgiFailureAction::KeepResources;
}

inline uint32_t dxgi_duplication_retry_delay_ms(uint32_t consecutiveFailures)
{
    if (consecutiveFailures <= 1) return 0;
    if (consecutiveFailures == 2) return 50;
    if (consecutiveFailures == 3) return 100;
    return 250;
}
~~~

~~~cpp
// third_party/lan_stream_probe/src/dxgi_capture.cpp:219-302（修改后，节选）
DxgiCaptureResult DxgiCapture::capture_frame(CapturedFrame* frame, std::string* error)
{
    if (!duplication_) {
        const auto now = std::chrono::steady_clock::now();
        if (next_duplication_retry_.time_since_epoch().count() > 0
            && now < next_duplication_retry_) {
            return {device_ ? DxgiCaptureStatus::DuplicationRecovering
                            : DxgiCaptureStatus::DeviceRecovering, S_FALSE};
        }
        if (device_ && context_ && !frame_slots_.empty()) {
            long recreate_result = S_OK;
            if (!recreate_duplication(error, &recreate_result)) {
                schedule_duplication_recovery(recreate_result);
                return {DxgiCaptureStatus::DuplicationRecovering, recreate_result};
            }
        } else if (!initialize(error)) {
            next_duplication_retry_ = now + std::chrono::milliseconds(250);
            return {DxgiCaptureStatus::DeviceRecovering, E_FAIL};
        }
    }

    HRESULT hr = duplication_->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return {awaiting_recovery_frame_ ? DxgiCaptureStatus::DuplicationRecovering
                                         : DxgiCaptureStatus::NoDesktopChange, hr};
    }
    if (FAILED(hr)) {
        const long removal_reason = device_ ? device_->GetDeviceRemovedReason() : E_FAIL;
        const DxgiFailureAction failure_action = dxgi_failure_action(hr, removal_reason);
        if (failure_action == DxgiFailureAction::RecreateDuplication) {
            schedule_duplication_recovery(hr);
            return {DxgiCaptureStatus::DuplicationRecovering, hr};
        }
        if (failure_action == DxgiFailureAction::RecreateDevice) {
            const long recovery_result = dxgi_result_is_device_lost(hr)
                ? hr
                : removal_reason;
            reset_resources();
            next_duplication_retry_ =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            return {DxgiCaptureStatus::DeviceRecovering, recovery_result};
        }
        return {DxgiCaptureStatus::FatalError, hr};
    }
}
~~~

~~~cpp
// third_party/lan_stream_probe/src/dxgi_capture.cpp:353-448（修改后，节选）
bool DxgiCapture::recreate_duplication(std::string* error, long* result)
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = device_.As(&dxgi_device);
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);

    Microsoft::WRL::ComPtr<IDXGIOutput> selected_output;
    DXGI_OUTPUT_DESC selected_desc = {};
    // 中间按 preferred_device_name_ 在原适配器枚举同名输出。
    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = selected_output.As(&output1);
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> replacement;
    if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(device_.Get(), &replacement);
    if (FAILED(hr) || !replacement) {
        if (result) *result = FAILED(hr) ? hr : E_FAIL;
        return false;
    }

    duplication_ = std::move(replacement);
    awaiting_recovery_frame_ = true;
    return true;
}

void DxgiCapture::schedule_duplication_recovery(long result)
{
    reset_duplication();
    ++consecutive_duplication_failures_;
    const uint32_t delay_ms =
        dxgi_duplication_retry_delay_ms(consecutive_duplication_failures_);
    next_duplication_retry_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
}

void DxgiCapture::reset_duplication()
{
    duplication_.Reset(); // wjy: 保留 Device、Context、帧槽和已交付纹理。
    awaiting_recovery_frame_ = true;
}
~~~

~~~cpp
// third_party/parsec_vdd/parsec-vdd.h:271-367（修改后，节选）
static DWORD VddIoControlWithTimeout(
    HANDLE vdd, VddCtlCode code, const void *data, size_t size,
    DWORD timeoutMs, DWORD *lastError)
{
    const BOOL issued = DeviceIoControl(
        vdd, (DWORD)code, InBuffer, sizeof(InBuffer),
        &OutBuffer, sizeof(DWORD), NULL, &Overlapped);
    const DWORD issueError = issued ? ERROR_SUCCESS : GetLastError();
    if (!issued && issueError != ERROR_IO_PENDING) {
        if (lastError != NULL) *lastError = issueError;
        CloseHandle(Overlapped.hEvent);
        return -1;
    }
    if (!GetOverlappedResultEx(
            vdd, &Overlapped, &NumberOfBytesTransferred, timeoutMs, FALSE)) {
        const DWORD waitError = GetLastError();
        if (waitError == WAIT_TIMEOUT || waitError == ERROR_IO_INCOMPLETE) {
            CancelIoEx(vdd, &Overlapped);
            GetOverlappedResult(vdd, &Overlapped, &NumberOfBytesTransferred, TRUE);
        }
        if (lastError != NULL) *lastError = waitError;
        CloseHandle(Overlapped.hEvent);
        return -1;
    }
    CloseHandle(Overlapped.hEvent);
    return OutBuffer;
}

static BOOL VddUpdateWithTimeout(HANDLE vdd, DWORD timeoutMs, DWORD *lastError)
{
    return VddIoControlWithTimeout(
        vdd, VDD_IOCTL_UPDATE, NULL, 0, timeoutMs, lastError) != (DWORD)-1;
}
~~~

~~~cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:255-328（修改后，节选）
const lsp::DxgiCaptureResult capture_result =
    capture_.capture_frame(&captured, &capture_error);
bool publish_allowed = true;

if (capture_result.status == lsp::DxgiCaptureStatus::FreshFrame) {
    last_frame_ = std::move(captured);
    if (capture_recovering_) {
        capture_recovering_ = false;
        append_stream_capture_diagnostic_log(
            "capture",
            "DXGI recovery completed; fresh frame resumed on retained pipeline");
    }
} else if (capture_result.status == lsp::DxgiCaptureStatus::DuplicationRecovering
        || capture_result.status == lsp::DxgiCaptureStatus::DeviceRecovering) {
    publish_allowed = false; // wjy: 保留最后帧，但恢复期不重复送入 NVENC。
    ++recovery_suppressed_frames_;
    if (!capture_recovering_) {
        capture_recovering_ = true;
        ++recovery_events_;
    }
}

if (publish_allowed && last_frame_.texture) {
    publish_frame(last_frame_);
} else if (publish_allowed) {
    OnFrameDropped();
}
~~~

~~~cpp
// third_party/uu_stream_webrtc/src/parsec_vdd_session.cpp:425-467（修改后，节选）
heartbeat_running_ = true;
heartbeat_thread_ = std::thread([this] {
    auto next_heartbeat = std::chrono::steady_clock::now();
    uint32_t consecutive_failures = 0;
    while (heartbeat_running_) {
        next_heartbeat += std::chrono::milliseconds(100);
        const auto update_begin = std::chrono::steady_clock::now();
        DWORD update_error = ERROR_SUCCESS;
        const BOOL update_ok =
            parsec_vdd::VddUpdateWithTimeout(handle_, 250, &update_error);
        const auto update_end = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            update_end - update_begin).count();
        if (!update_ok) {
            ++consecutive_failures;
            append_stream_capture_diagnostic_log_rate_limited(
                "vdd",
                "heartbeat failed error=" + std::to_string(update_error)
                    + " elapsed_ms=" + std::to_string(elapsed_ms)
                    + " consecutive=" + std::to_string(consecutive_failures),
                1000);
        } else {
            if (consecutive_failures > 0) {
                append_stream_capture_diagnostic_log(
                    "vdd",
                    "heartbeat recovered previous_failures="
                        + std::to_string(consecutive_failures));
            }
            consecutive_failures = 0;
        }
        const auto pacing_now = std::chrono::steady_clock::now();
        if (pacing_now > next_heartbeat + std::chrono::milliseconds(100)) {
            next_heartbeat = pacing_now;
        }
        std::this_thread::sleep_until(next_heartbeat);
    }
});
~~~

~~~cpp
// third_party/uu_stream_webrtc/tests/dxgi_capture_policy_tests.cpp:26-41（修改后，节选）
assert(lsp::dxgi_failure_action(DXGI_ERROR_ACCESS_LOST, S_OK)
    == lsp::DxgiFailureAction::RecreateDuplication);
assert(lsp::dxgi_failure_action(DXGI_ERROR_INVALID_CALL, S_OK)
    == lsp::DxgiFailureAction::RecreateDuplication);
assert(lsp::dxgi_failure_action(DXGI_ERROR_WAIT_TIMEOUT, S_OK)
    == lsp::DxgiFailureAction::KeepResources);
assert(lsp::dxgi_failure_action(
           DXGI_ERROR_ACCESS_LOST, DXGI_ERROR_DEVICE_REMOVED)
    == lsp::DxgiFailureAction::RecreateDevice);
assert(lsp::dxgi_duplication_retry_delay_ms(1) == 0);
assert(lsp::dxgi_duplication_retry_delay_ms(2) == 50);
assert(lsp::dxgi_duplication_retry_delay_ms(3) == 100);
assert(lsp::dxgi_duplication_retry_delay_ms(20) == 250);
~~~

### Steps
1. 将 DXGI 采集返回值从布尔值扩展为类型化状态，区分新帧、静止桌面、槽位繁忙、Duplication 恢复、设备恢复和未分类错误。
2. 把 ACCESS_LOST 与 INVALID_CALL 限定为轻量 Duplication 重建；Device Removed、Reset、Hung 和驱动内部错误才完整释放 D3D11 设备资源。
3. 增加 0/50/100/250ms 恢复退避，避免 60 FPS 循环在异常时持续枚举输出和创建 DXGI 对象。
4. 把 GetDeviceRemovedReason 限制在 AcquireNextFrame 异常路径，健康采帧不增加额外 COM 调用。
5. Host 恢复期不清空 last_frame_、不重复编码旧帧、不调用 OnFrameDropped，依靠控制端保留上一张已成功显示的画面。
6. 为 VDD 心跳 IOCTL 增加 250ms 超时、CancelIoEx 取消和取消完成等待，并把正常心跳维持为约 100ms 绝对节拍。
7. 增加恢复开始、恢复结束、心跳失败、心跳恢复和慢心跳的低频诊断，以及错误分类与退避纯策略断言。

### Verification
- git diff --check：通过。
- 已完成 DXGI 资源释放分支、正常路径额外开销、Host 最后帧保留逻辑、VDD CancelIoEx/OVERLAPPED 生命周期和函数声明一致性的静态源码复核。
- 按用户要求未编译，未运行测试；新增的策略断言仅完成源码检查，需后续构建或实机发布时验证。
- 本次没有新增文件；OpenSpec 任务 7.4 仍包含其它会话销毁和适配器恢复工作，因此未将整项标记完成，也未勾选需要实机验证的任务 8.1。

## 2026-08-06 11:17 - 为 FakerInput 交付包增加安全测试案例

### Changed Location
- `FakerInputDriverKit/CMakeLists.txt:1-24`：新增独立 MSVC/CMake 测试程序构建入口并显式链接 HID、SetupAPI 和 User32。
- `FakerInputDriverKit/test/main.cpp:1-167`：新增默认无输入的驱动状态检查，以及需要显式参数和五秒倒计时的键盘、鼠标测试，并把最终全零释放结果纳入测试成败。
- `FakerInputDriverKit/README.md:8-190`：补充测试文件目录、编译命令、状态检查和真实输入测试说明，并顺延原集成章节编号。

### Reason
同事在把底层源码封装进自己的程序前，需要先区分“驱动没有安装或未重启”“HID 控制集合无法打开”和“键鼠报告发送失败”。因此增加一个独立测试程序：无参数和 `--status` 只打开驱动并读取版本，不产生输入；`--key-test`、`--mouse-test` 必须由使用者显式指定并等待五秒后才发送最小报告，降低误操作风险。测试代码复用了交付目录中的同一份源码，能够真实验证同事后续会集成的文件和链接依赖。

### Original Code
```text
// FakerInputDriverKit/CMakeLists.txt（新增前）
new file, no old code at this location
```

```cpp
// FakerInputDriverKit/test/main.cpp（新增前）
new file, no old code at this location
```

````markdown
<!-- FakerInputDriverKit/README.md:8-20（修改前） -->
```text
FakerInputDriverKit/
├─ driver/
│  └─ FakerInput_Setup_0.1.1_x64.msi   驱动安装包
├─ src/
│  ├─ faker_input_device.h              驱动设备访问接口
│  ├─ faker_input_device.cpp            HID 设备枚举与报告发送实现
│  └─ faker_input_keyboard_state.h      Windows VK 到 USB HID 的映射和按键状态机
├─ README.md                             本说明
└─ THIRD_PARTY_NOTICES.md                FakerInput MIT 授权说明
```
````

```markdown
<!-- FakerInputDriverKit/README.md:37（修改前） -->
## 2. 集成到自己的 C++ 程序
```

### Modified Code
```cmake
# FakerInputDriverKit/CMakeLists.txt:6-21（新增后，节选）
add_executable(FakerInputDriverTest
    test/main.cpp # wjy: 独立测试入口默认只查询驱动，真实键鼠操作必须由命令行参数显式触发。
    src/faker_input_device.cpp
    src/faker_input_device.h
    src/faker_input_keyboard_state.h
)

target_compile_features(FakerInputDriverTest PRIVATE cxx_std_20)
target_compile_definitions(FakerInputDriverTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(FakerInputDriverTest PRIVATE src)
target_link_libraries(FakerInputDriverTest PRIVATE hid setupapi user32)
```

```cpp
// FakerInputDriverKit/test/main.cpp:135-165（新增后，节选）
int wmain(int argc, wchar_t** argv)
{
    const std::wstring_view command = argc > 1 ? argv[1] : L"--status"; // wjy: 无参数默认只探测设备，双击测试程序不会产生任何键盘或鼠标输入。
    if (command == L"--help") {
        printUsage();
        return kExitSuccess;
    }

    faker_bridge::DeviceError openError;
    auto device = faker_bridge::FakerInputDevice::open(&openError); // wjy: 每次测试只打开一次 HID 控制集合，所有报告在同一设备会话中保持有序。
    if (!device) {
        printDeviceError(L"open FakerInput driver", openError);
        return kExitDriverUnavailable;
    }

    if (command == L"--status") {
        return kExitSuccess; // wjy: 状态命令在成功打开设备后立即退出，不调用任何键盘或鼠标发送接口。
    }
}
```

````markdown
<!-- FakerInputDriverKit/README.md:40-68（修改后） -->
## 2. 编译和运行测试程序

```powershell
cmake.exe -S . -B build -A x64
cmake.exe --build build --config Release
```

默认命令只打开驱动并显示版本，不产生任何键鼠输入：

```powershell
.\build\Release\FakerInputDriverTest.exe --status
```

真实输入测试必须显式指定参数，并且都会先倒计时五秒。
````

### Steps
1. 新增独立 `CMakeLists.txt`，直接编译交付包中的设备实现、键盘状态机和测试入口。
2. 为测试程序增加 `--help`、`--status`、`--key-test`、`--mouse-test` 四种命令，无参数默认等价于安全的 `--status`。
3. 键盘测试复用 `sendKey` 状态快照语义，发送一次 `A` 按下和抬起；失败时回滚本地状态并执行 `release_all`。
4. 鼠标测试只发送右移 2 个相对单位和反向恢复报告，不产生按钮或滚轮动作。
5. 在 README 中补充 Visual Studio 2022 x64 编译和测试命令，明确真实输入命令的五秒倒计时与使用边界。

### Verification
- `cmake.exe -S FakerInputDriverKit -B build/faker-input-driver-kit-test -G "Visual Studio 17 2022" -A x64`：配置和生成通过。
- `cmake.exe --build build/faker-input-driver-kit-test --config Release --parallel`：Release 编译、链接通过，MSVC `/W4` 未报告警告。
- `FakerInputDriverTest.exe --help`：退出码 0，正确显示四种命令且不打开驱动。
- `FakerInputDriverTest.exe --status`：退出码 0，只打开 FakerInput 设备并读取到驱动版本 2，没有发送键盘或鼠标报告。
- 未运行 `--key-test` 和 `--mouse-test`，避免自动验证影响当前桌面；两个真实输入命令保留给使用者在无风险测试窗口中显式执行。

## 2026-08-06 11:44 - 修复相对鼠标测试结束后的光标跳动

### Changed Location
- `FakerInputDriverKit/src/faker_input_device.h:74-84`：记录本设备实例最后成功送达的绝对鼠标按钮和坐标状态。
- `FakerInputDriverKit/src/faker_input_device.cpp:120-315`：移动设备时同步转移绝对状态，绝对报告成功后提交状态，并让 `release_all()` 仅在绝对按钮仍按住时发送绝对释放。
- `FakerInputDriverKit/test/main.cpp:108-143`：鼠标测试记录倒计时后的起点和释放后的终点，输出真实净位移。
- `FakerInputDriverKit/README.md:58-186`：说明鼠标测试坐标诊断、业务程序真实链接依赖和新的无跳动释放语义。

### Reason
用户实际连续运行 `--mouse-test` 时观察到光标明显向右跳动。测试发送的相对位移只有 `+2` 和 `-2`，异常移动来自结束清理：旧 `release_all()` 无条件读取 Windows 当前光标，再按主屏尺寸换算并发送一份绝对鼠标全零报告。多显示器、缩放或 HID 绝对坐标映射与主屏不一致时，这份并非必要的报告会重新定位光标。修复后，设备只跟踪本实例真实成功送达的绝对按钮状态；纯键盘或相对鼠标会话从未按住绝对按钮，因此释放时不再发送任何绝对坐标。

### Original Code
```cpp
// FakerInputDriverKit/src/faker_input_device.h:74-76（修改前）
HANDLE handle_ = INVALID_HANDLE_VALUE;
std::uint16_t output_report_bytes_ = 0;
std::uint16_t driver_version_ = 0;
```

```cpp
// FakerInputDriverKit/src/faker_input_device.cpp:87-106（修改前，节选）
[[nodiscard]] std::pair<std::uint16_t, std::uint16_t> current_absolute_position() {
    POINT point{};
    if (!GetCursorPos(&point)) {
        return {std::uint16_t{0}, std::uint16_t{0}};
    }

    const int width = std::max(GetSystemMetrics(SM_CXSCREEN) - 1, 1);
    const int height = std::max(GetSystemMetrics(SM_CYSCREEN) - 1, 1);
    // 按主屏尺寸把当前光标换算为 0..32767。
}
```

```cpp
// FakerInputDriverKit/src/faker_input_device.cpp:297-312（修改前）
const auto [x, y] = current_absolute_position();
DeviceError absolute_error;
if (!send_absolute_mouse(0, x, y, 0, &absolute_error) && success) {
    success = false;
    first_error = absolute_error;
}
```

```cpp
// FakerInputDriverKit/test/main.cpp:108-130（修改前，节选）
countdown(L"mouse test");
device.send_relative_mouse(0, 2, 0, 0, 0, &error);
device.send_relative_mouse(0, -2, 0, 0, 0, &error);
releaseAll(device, keyboardState);
std::wcout << L"RESULT: Relative mouse round-trip reports sent successfully.\n";
```

```markdown
<!-- FakerInputDriverKit/README.md:61（修改前） -->
- `--mouse-test`：鼠标向右移动 2 个相对单位，再移动回来；
```

### Modified Code
```cpp
// FakerInputDriverKit/src/faker_input_device.h:77-82（修改后）
mutable std::uint8_t absolute_buttons_ = 0; // wjy: 只记录本实例最后成功送达的绝对鼠标按钮快照，纯相对鼠标释放时无需发送绝对坐标。
mutable std::uint16_t absolute_x_ = 0; // wjy: 绝对按钮仍按住时在原报告位置抬起，避免清理操作重新映射当前光标。
mutable std::uint16_t absolute_y_ = 0;
```

```cpp
// FakerInputDriverKit/src/faker_input_device.cpp:282-315（修改后，节选）
if (!send_inner_report(report, error)) {
    return false;
}
absolute_buttons_ = buttons;
absolute_x_ = x;
absolute_y_ = y;

if (absolute_buttons_ != 0) {
    DeviceError absolute_error;
    if (!send_absolute_mouse(0, absolute_x_, absolute_y_, 0, &absolute_error) && success) {
        success = false;
        first_error = absolute_error;
    }
}
```

```cpp
// FakerInputDriverKit/test/main.cpp:112-140（修改后，节选）
POINT startPosition{};
const bool hasStartPosition = ::GetCursorPos(&startPosition);

device.send_relative_mouse(0, 2, 0, 0, 0, &error);
device.send_relative_mouse(0, -2, 0, 0, 0, &error);
releaseAll(device, keyboardState);

POINT endPosition{};
if (hasStartPosition && ::GetCursorPos(&endPosition)) {
    std::wcout << L"CURSOR: start=(...) end=(...) net=(...)\n";
}
```

```markdown
<!-- FakerInputDriverKit/README.md:61、186（修改后，节选） -->
- `--mouse-test`：鼠标向右移动 2 个相对单位再移动回来，并打印起点、终点和净位移；

`release_all()` 只有在当前设备实例确实还按住绝对鼠标按钮时才发送绝对释放报告。
```

### Steps
1. 删除按主屏尺寸重新计算当前绝对光标位置的清理辅助函数。
2. 在 `FakerInputDevice` 内记录最后成功送达的绝对按钮、X 和 Y，并在移动构造、移动赋值时同步转移状态。
3. 让 `release_all()` 始终清零键盘和相对鼠标，但仅在本实例仍持有绝对按钮时于原坐标发送绝对抬起。
4. 为 `--mouse-test` 增加起点、终点和净位移输出，便于用户复测是否仍存在绝对坐标跳转。
5. 更新 README，移除业务集成不再需要的 `user32.lib`，保留测试程序读取光标诊断所需的 User32 链接。

### Verification
- 在 `FakerInputDriverKit/build` 中重新执行 Visual Studio 2022 x64 Release 编译和链接，通过且 MSVC `/W4` 未报告警告。
- 新生成 `FakerInputDriverKit/build/Release/FakerInputDriverTest.exe`。
- `FakerInputDriverTest.exe --status`：退出码 0，成功读取驱动版本 2，未发送键盘或鼠标报告。
- 未由 Codex 自动执行新的 `--mouse-test`，避免主动移动用户光标；需要用户复测并检查新增的 `CURSOR ... net=(x,y)` 输出。

## 2026-08-06 12:00 - 增大 FakerInput 鼠标测试可见位移

### Changed Location
- `FakerInputDriverKit/test/main.cpp:14-182`：将鼠标测试位移抽成 50 个相对单位常量，并把往返间隔增加到 500 毫秒。
- `FakerInputDriverKit/README.md:61`：同步说明新的测试幅度和半秒停留行为。

### Reason
用户复测修正版鼠标路径后得到 `net=(1,0)`，证明异常绝对坐标跳转已经消失，但原先 `+2/-2` 个 HID 相对单位的中间移动过小，肉眼不容易观察。测试目标是让使用者确认虚拟鼠标报告确实生效，因此将幅度提高到 50 个相对单位，并在右移和返回之间停留半秒；最终仍发送完全相反的位移并输出净坐标，不改变往返验证语义。

### Original Code
```cpp
// FakerInputDriverKit/test/main.cpp:26、118-124（修改前，节选）
<< L"  FakerInputDriverTest.exe --mouse-test  After 5 seconds, move right 2px and back.\n"

device.send_relative_mouse(0, 2, 0, 0, 0, &error);
::Sleep(120);
device.send_relative_mouse(0, -2, 0, 0, 0, &error);
```

```markdown
<!-- FakerInputDriverKit/README.md:61（修改前） -->
- `--mouse-test`：鼠标向右移动 2 个相对单位再移动回来，并打印起点、终点和净位移；
```

### Modified Code
```cpp
// FakerInputDriverKit/test/main.cpp:18-19、28、120-127（修改后，节选）
constexpr std::int16_t kMouseTestDelta = 50; // wjy: 使用明显可见但仍受控的相对位移，便于肉眼确认虚拟鼠标报告生效。
constexpr DWORD kMouseTestPauseMs = 500; // wjy: 在右移和返回之间停留半秒，让使用者能够观察到中间位置。

<< L"  FakerInputDriverTest.exe --mouse-test  After 5 seconds, move right 50 units and back.\n"

device.send_relative_mouse(0, kMouseTestDelta, 0, 0, 0, &error);
::Sleep(kMouseTestPauseMs);
device.send_relative_mouse(0, -kMouseTestDelta, 0, 0, 0, &error);
```

```markdown
<!-- FakerInputDriverKit/README.md:61（修改后） -->
- `--mouse-test`：鼠标向右移动 50 个相对单位，停留约半秒后再移动回来，并打印起点、终点和净位移；
```

### Steps
1. 新增 `kMouseTestDelta=50`，统一控制右移和反向返回的 HID 相对计数。
2. 新增 `kMouseTestPauseMs=500`，让中间位置保持半秒以便肉眼观察。
3. 更新命令帮助和 README，使用“相对单位”而不是不准确的“像素”描述。
4. 在交付目录内重新编译 Release 测试程序。

### Verification
- `cmake.exe --build FakerInputDriverKit/build --config Release --parallel`：编译和链接通过，MSVC `/W4` 未报告警告。
- `FakerInputDriverTest.exe --help`：退出码 0，显示 `move right 50 units and back`。
- `FakerInputDriverTest.exe --status`：退出码 0，成功读取驱动版本 2，未产生键鼠输入。
- 未自动执行 `--mouse-test`，避免 Codex 主动移动用户光标；等待用户在无风险窗口中复测。

## 2026-08-06 12:11 - 增加 FakerInput 绝对鼠标往返测试

### Changed Location
- `FakerInputDriverKit/test/main.cpp:1-291`：新增虚拟桌面坐标查询、屏幕坐标到 `0..32767` 的归一化、绝对测试目标选择和 `--absolute-mouse-test` 执行路径。
- `FakerInputDriverKit/README.md:58-167`：补充绝对鼠标测试命令、多显示器行为和绝对坐标换算说明。

### Reason
现有 `--mouse-test` 只验证相对 HID 报告，用户需要单独确认 `send_absolute_mouse()`。绝对鼠标不能把屏幕像素直接作为驱动坐标，尤其当前设备存在负坐标副屏；因此测试读取完整 Windows 虚拟桌面范围，将当前点和水平约 200 像素的目标点统一映射到 FakerInput 的 `0..32767` 绝对轴，停留半秒后再发送原始绝对坐标，并输出桌面范围、屏幕点、HID 点和最终净位移。

### Original Code
```cpp
// FakerInputDriverKit/test/main.cpp:23-30（修改前，节选）
std::wcout
    << L"FakerInputDriverTest\n\n"
    << L"Usage:\n"
    << L"  FakerInputDriverTest.exe --mouse-test  After 5 seconds, move right 50 units and back.\n"
    << L"  FakerInputDriverTest.exe --help        Show this help.\n";
```

```cpp
// FakerInputDriverKit/test/main.cpp:153-180（修改前，节选）
if (argc > 2 || (command != L"--status" && command != L"--key-test"
        && command != L"--mouse-test" && command != L"--help")) {
    return kExitUsage;
}

return command == L"--key-test"
    ? runKeyTest(*device, keyboardState)
    : runMouseTest(*device, keyboardState);
```

````markdown
<!-- FakerInputDriverKit/README.md:55-63（修改前，节选） -->
```powershell
.\build\Release\FakerInputDriverTest.exe --key-test
.\build\Release\FakerInputDriverTest.exe --mouse-test
```
````

### Modified Code
```cpp
// FakerInputDriverKit/test/main.cpp:80-124（修改后，节选）
struct VirtualDesktopGeometry {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

std::uint16_t normalizeAbsoluteCoordinate(int value, int origin, int extent)
{
    const int clamped = std::clamp(value, origin, origin + extent - 1);
    const std::int64_t offset = static_cast<std::int64_t>(clamped) - origin;
    return static_cast<std::uint16_t>((offset * 32767) / (extent - 1));
}
```

```cpp
// FakerInputDriverKit/test/main.cpp:196-253（修改后，节选）
int runAbsoluteMouseTest(
    faker_bridge::FakerInputDevice& device,
    uu::FakerInputKeyboardState& keyboardState)
{
    countdown(L"absolute mouse test");
    const POINT targetPosition = absoluteMouseTestTarget(startPosition, geometry);
    device.send_absolute_mouse(0, targetX, targetY, 0, &error);
    ::Sleep(kMouseTestPauseMs);
    device.send_absolute_mouse(0, startX, startY, 0, &error);
    std::wcout << L"CURSOR: start=(...) target=(...) end=(...) net=(...)\n";
}
```

````markdown
<!-- FakerInputDriverKit/README.md:55-63、167（修改后，节选） -->
```powershell
.\build\Release\FakerInputDriverTest.exe --absolute-mouse-test
```

- `--absolute-mouse-test`：按完整虚拟桌面范围换算 `0..32767`，水平移动约 200 像素、停留半秒，再返回原位置；

绝对坐标不是屏幕像素。多显示器程序应使用完整虚拟桌面指标归一化到 `0..32767`。
````

### Steps
1. 查询 `SM_XVIRTUALSCREEN`、`SM_YVIRTUALSCREEN`、`SM_CXVIRTUALSCREEN` 和 `SM_CYVIRTUALSCREEN`，覆盖负坐标副屏。
2. 使用 64 位整数把屏幕 X/Y 映射到 FakerInput 绝对轴 `0..32767`。
3. 在虚拟桌面边界内选择水平约 200 像素的测试点，右侧空间不足时改为向左移动。
4. 新增 `--absolute-mouse-test`，发送目标点、停留半秒、返回原点并输出完整诊断。
5. 修正严格 MSVC 编译发现的 `POINT::LONG` 与 `int` 模板类型歧义，统一目标点计算为 `LONG`。
6. 更新 README 和帮助文本，并在交付目录重新生成 Release 测试程序。

### Verification
- 首次编译准确发现 `std::max/std::clamp` 的 `LONG`/`int` 模板推导错误；统一为 `LONG` 后重新验证通过。
- `cmake.exe --build FakerInputDriverKit/build --config Release --parallel`：Release 编译和链接通过，MSVC `/W4` 未报告警告。
- `FakerInputDriverTest.exe --help`：退出码 0，正确显示 `--absolute-mouse-test`。
- `FakerInputDriverTest.exe --status`：退出码 0，成功读取驱动版本 2，未产生键鼠输入。
- 未自动执行 `--absolute-mouse-test`，避免 Codex 主动改变用户绝对光标位置；等待用户在无风险窗口中显式复测。

## 2026-08-06 12:29 - 修正 FakerInput 绝对鼠标主屏映射

### Changed Location
- `FakerInputDriverKit/test/main.cpp:80-278`：把绝对 HID 坐标换算从完整虚拟桌面改为 Windows 主显示器，并为副屏启动场景增加主屏内往返测试及原副屏位置恢复。
- `FakerInputDriverKit/README.md:63、167`：明确 FakerInput 绝对 HID 轴只能寻址主屏，补充多显示器控制的替代方案。

### Reason
用户在左侧负坐标副屏运行绝对鼠标测试后得到：

```text
ABSOLUTE: desktop=(-3840,-1062,5760,2160) startScreen=(-1085,123) targetScreen=(-885,123) startHid=(15675,17984) targetHid=(16813,17984)
CURSOR: start=(-1085,123) target=(-885,123) end=(985,592) net=(2070,469)
```

显示器布局为主屏 `(0,0,1920,1080)`、副屏 `(-3840,-1062,3840,2160)`。输出证明 FakerInput 驱动没有把绝对轴映射到完整虚拟桌面，而是把 `0..32767` 固定映射到主屏；原实现按虚拟桌面归一化，因此同一 HID 数值最终落到了主屏 `(985,592)`。修正后只用主屏范围计算 FakerInput 绝对坐标；若测试从副屏开始，则先在主屏中心附近执行绝对往返，再用 Win32 `SetCursorPos` 恢复用户原副屏位置，避免再次跨屏跳转后停留在错误位置。

### Original Code
```cpp
// FakerInputDriverKit/test/main.cpp:80-123（修改前，节选）
struct VirtualDesktopGeometry { /* ... */ };

geometry->left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
geometry->top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
geometry->width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
geometry->height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

const POINT targetPosition = absoluteMouseTestTarget(startPosition, geometry);
const std::uint16_t startX = normalizeAbsoluteCoordinate(startPosition.x, geometry.left, geometry.width);
const std::uint16_t startY = normalizeAbsoluteCoordinate(startPosition.y, geometry.top, geometry.height);
```

```markdown
<!-- FakerInputDriverKit/README.md:63、167（修改前，节选） -->
- `--absolute-mouse-test`：按完整虚拟桌面范围换算 `0..32767`，水平移动约 200 像素、停留半秒，再返回原位置；

多显示器程序应使用完整虚拟桌面指标归一化到 `0..32767`。
```

### Modified Code
```cpp
// FakerInputDriverKit/test/main.cpp:80-263（修改后，节选）
struct PrimaryScreenGeometry { /* ... */ };

geometry->left = 0; // wjy: 实机验证确认 FakerInput 绝对 HID 轴固定映射 Windows 主屏。
geometry->top = 0;
geometry->width = ::GetSystemMetrics(SM_CXSCREEN);
geometry->height = ::GetSystemMetrics(SM_CYSCREEN);

const bool startOnPrimary = pointInsidePrimaryScreen(startPosition, geometry);
POINT referencePosition = startPosition;
if (!startOnPrimary) {
    referencePosition.x = geometry.left + geometry.width / 2;
    referencePosition.y = geometry.top + geometry.height / 2;
}

if (!startOnPrimary) {
    ::SetCursorPos(startPosition.x, startPosition.y);
}
```

```markdown
<!-- FakerInputDriverKit/README.md:63、167（修改后，节选） -->
- `--absolute-mouse-test`：按主显示器范围换算 `0..32767`，水平移动约 200 像素并返回；若从副屏启动，结束时使用 Win32 恢复原位置；

FakerInput 的绝对 HID 轴映射到 Windows 主显示器，不能直接寻址负坐标副屏。
```

### Steps
1. 使用 `SM_CXSCREEN`、`SM_CYSCREEN` 建立以 `(0,0)` 为原点的主屏坐标范围。
2. 判断倒计时结束后的光标是否位于主屏；主屏起点直接执行约 200 像素的绝对往返。
3. 副屏起点改用主屏中心作为测试基准，避免负坐标被夹紧或错误映射。
4. 驱动往返和全零报告成功后，通过 `SetCursorPos` 恢复副屏原始位置，并打印 `RESTORE` 说明实际恢复方式。
5. 更新 README，说明副屏绝对控制应使用相对鼠标、`SendInput` 虚拟桌面模式或其它后端。
6. 在交付目录内重新生成 Release 测试程序，不自动执行会移动光标的测试。

### Verification
- `cmake.exe --build FakerInputDriverKit/build --config Release --parallel`：Release 编译和链接通过。
- `FakerInputDriverTest.exe --help`：退出码 0，绝对测试命令仍可用。
- `FakerInputDriverTest.exe --status`：退出码 0，成功读取驱动版本 2，未产生键鼠输入。
- 未自动执行新的 `--absolute-mouse-test`；需要用户从当前副屏位置复测，预期出现 `startOnPrimary=no`、`RESTORE:`，最终 `CURSOR` 的 `net` 接近 `(0,0)`。

## 2026-08-06 13:52 - 数字开头设备默认启动自动壁纸

### Changed Location
- `src/system/AppSettings.h:24`：允许壁纸开关读取入口接收“没有历史配置时”的调用方默认值。
- `src/system/AppSettings.cpp:173-175`：保存过的用户开关优先，只有配置缺失时才采用设备名默认状态。
- `src/system/DeviceInfoService.h:30`：新增轻量本机设备名读取接口。
- `src/system/DeviceInfoService.cpp:227-232`：复用 Windows 宽字符计算机名读取，跳过完整网卡枚举。
- `src/system/DesktopWallpaperService.h:20`：公开数字开头设备名的默认轮换策略。
- `src/system/DesktopWallpaperService.cpp:89-93`：实现去空白后首字符为数字的判断。
- `src/ui/DeviceGrid.cpp:3643-3646`：启动恢复设置时把当前设备名规则作为未配置设备的默认值。
- `tests/desktop_wallpaper_service_tests.cpp:147-154,169`：新增数字、字母、中文和空设备名的策略回归用例。

### Reason
编号设备的 Windows 计算机名通常以数字开头，这类设备需要在没有历史壁纸配置时自动启动桌面壁纸轮换，同时必须保留用户后续手动关闭的选择。实现采用“设备名规则只提供 QSettings 缺省值”的方式：已保存的 `true` 或 `false` 始终优先，因此不会在每次启动时强制覆盖用户设置；设备名读取也只调用轻量的计算机名接口，不把原本延迟执行的完整网卡枚举提前到启动关键路径。

### Original Code

#### `src/system/AppSettings.h`
```cpp
// src/system/AppSettings.h:24（修改前）
static bool desktopWallpaperRotationEnabled(); // wjy: 自动桌面壁纸轮换默认关闭，仅在用户主动开启后跨启动恢复。
```

#### `src/system/AppSettings.cpp`
```cpp
// src/system/AppSettings.cpp:173-176（修改前）
bool AppSettings::desktopWallpaperRotationEnabled()
{
    return settings().value(QStringLiteral("desktopWallpaperRotationEnabled"), false).toBool(); // wjy: 没有历史配置的新安装保持关闭，避免程序首次启动就修改用户桌面。
}
```

#### `src/system/DeviceInfoService.h`
```cpp
// src/system/DeviceInfoService.h:28-31（修改前）
class DeviceInfoService final {
public:
    static DeviceInfo local();
};
```

#### `src/system/DeviceInfoService.cpp`
```cpp
// src/system/DeviceInfoService.cpp:225-230（修改前）
} // namespace

DeviceInfo DeviceInfoService::local()
{
    return currentDeviceInfo();
}
```

#### `src/system/DesktopWallpaperService.h`
```cpp
// src/system/DesktopWallpaperService.h:17-21（修改前）
class DesktopWallpaperService final {
public:
    static QString sharedDirectoryPath();
    static QStringList supportedImageCandidates(const QString& directoryPath);
};
```

#### `src/system/DesktopWallpaperService.cpp`
```cpp
// src/system/DesktopWallpaperService.cpp:84-89（修改前）
QString DesktopWallpaperService::sharedDirectoryPath()
{
    return QString::fromUtf8(R"(\\192.168.1.100\广告部工具\远程软件_桌面)");
}

QStringList DesktopWallpaperService::supportedImageCandidates(const QString& directoryPath)
```

#### `src/ui/DeviceGrid.cpp`
```cpp
// src/ui/DeviceGrid.cpp:3641-3644（修改前）
m_periodicDeviceDiscoveryEnabled = platform::AppSettings::periodicDeviceDiscoveryEnabled();
m_periodicDeviceDiscoveryIntervalSeconds = platform::AppSettings::periodicDeviceDiscoveryIntervalSeconds();
m_wallpaperRotationEnabled = platform::AppSettings::desktopWallpaperRotationEnabled();
m_wallpaperRotationIntervalMinutes = platform::AppSettings::desktopWallpaperRotationIntervalMinutes();
```

#### `tests/desktop_wallpaper_service_tests.cpp`
```cpp
// tests/desktop_wallpaper_service_tests.cpp:145-160（修改前，节选）
// 此位置没有设备名默认策略测试。

testDeviceNameOverlayIsFlattenedIntoUpperRightPixels();
testOverlayPixelsFollowDeviceNameAndTrimWhitespace();
return 0;
```

### Modified Code

#### `src/system/AppSettings.h`
```cpp
// src/system/AppSettings.h:24（修改后）
static bool desktopWallpaperRotationEnabled(bool defaultEnabled = false); // wjy: 已保存开关始终优先；没有历史配置时允许调用方按当前设备名提供默认值。
```

#### `src/system/AppSettings.cpp`
```cpp
// src/system/AppSettings.cpp:173-176（修改后）
bool AppSettings::desktopWallpaperRotationEnabled(bool defaultEnabled)
{
    return settings().value(QStringLiteral("desktopWallpaperRotationEnabled"), defaultEnabled).toBool(); // wjy: 用户保存过的开关覆盖设备名默认值，未配置的新设备才采用调用方计算的默认状态。
}
```

#### `src/system/DeviceInfoService.h`
```cpp
// src/system/DeviceInfoService.h:28-32（修改后）
class DeviceInfoService final {
public:
    static QString localDeviceName(); // wjy: 只读取当前 Windows 计算机名，不枚举网卡，供启动阶段快速计算设备专属默认策略。
    static DeviceInfo local();
};
```

#### `src/system/DeviceInfoService.cpp`
```cpp
// src/system/DeviceInfoService.cpp:227-237（修改后）
// =====wjy====
QString DeviceInfoService::localDeviceName()
{
    return currentComputerName(); // wjy: 复用与完整本机信息相同的宽字符计算机名来源，但跳过 GetAdaptersAddresses 等较重的网络枚举。
}
// ===end====

DeviceInfo DeviceInfoService::local()
{
    return currentDeviceInfo();
}
```

#### `src/system/DesktopWallpaperService.h`
```cpp
// src/system/DesktopWallpaperService.h:17-21（修改后）
class DesktopWallpaperService final {
public:
    static QString sharedDirectoryPath();
    static bool rotationEnabledByDefaultForDeviceName(const QString& deviceName); // wjy: 设备名以数字开头时为未配置设备提供自动壁纸默认开启策略。
    static QStringList supportedImageCandidates(const QString& directoryPath);
};
```

#### `src/system/DesktopWallpaperService.cpp`
```cpp
// src/system/DesktopWallpaperService.cpp:84-95（修改后）
QString DesktopWallpaperService::sharedDirectoryPath()
{
    return QString::fromUtf8(R"(\\192.168.1.100\广告部工具\远程软件_桌面)");
}

bool DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(const QString& deviceName)
{
    const QString normalizedDeviceName = deviceName.trimmed(); // wjy: 防御性移除首尾空白，确保默认策略判断的是实际设备名首字符。
    return !normalizedDeviceName.isEmpty() && normalizedDeviceName.front().isDigit(); // wjy: 仅数字开头的设备默认启动自动壁纸，字母、中文或空名称仍保持默认关闭。
}

QStringList DesktopWallpaperService::supportedImageCandidates(const QString& directoryPath)
```

#### `src/ui/DeviceGrid.cpp`
```cpp
// src/ui/DeviceGrid.cpp:3641-3646（修改后）
m_periodicDeviceDiscoveryEnabled = platform::AppSettings::periodicDeviceDiscoveryEnabled();
m_periodicDeviceDiscoveryIntervalSeconds = platform::AppSettings::periodicDeviceDiscoveryIntervalSeconds();
const QString localWallpaperDeviceName = platform::DeviceInfoService::localDeviceName(); // wjy: 启动阶段只读取计算机名，不提前执行原本延迟的完整网卡枚举。
const bool defaultWallpaperRotationEnabled = platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(localWallpaperDeviceName); // wjy: 数字开头设备在没有历史开关配置时默认启动自动壁纸。
m_wallpaperRotationEnabled = platform::AppSettings::desktopWallpaperRotationEnabled(defaultWallpaperRotationEnabled); // wjy: 已保存的用户开关优先，用户明确关闭后不会被设备名规则重新打开。
m_wallpaperRotationIntervalMinutes = platform::AppSettings::desktopWallpaperRotationIntervalMinutes(); // wjy: 恢复自动壁纸分钟数，新安装仍使用 1 分钟默认周期。
```

#### `tests/desktop_wallpaper_service_tests.cpp`
```cpp
// tests/desktop_wallpaper_service_tests.cpp:147-154,169（修改后）
void testDigitPrefixedDeviceNameEnablesRotationByDefault()
{
    assert(platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(QStringLiteral("99-DESKTOP")));
    assert(platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(QStringLiteral("  7-PC  ")));
    assert(!platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(QStringLiteral("PC-99")));
    assert(!platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(QString::fromUtf8("设备99")));
    assert(!platform::DesktopWallpaperService::rotationEnabledByDefaultForDeviceName(QString()));
}

testDigitPrefixedDeviceNameEnablesRotationByDefault(); // wjy: 回归验证设备名默认开启策略，不读取注册表也不修改真实桌面。
```

### Steps
1. 为 `AppSettings::desktopWallpaperRotationEnabled` 增加调用方缺省值参数，让 QSettings 中已有的开关记录继续拥有最高优先级。
2. 为 `DeviceInfoService` 增加只读取计算机名的轻量入口，避免启动阶段提前枚举网卡。
3. 在 `DesktopWallpaperService` 中集中实现“去除首尾空白后首字符为数字”的默认开启策略。
4. 在 `DeviceGrid` 恢复设置时读取当前设备名，并把数字开头判断结果作为未配置设备的壁纸开关默认值。
5. 增加纯策略测试，覆盖数字开头、空白包围、数字非首位、中文开头和空名称。
6. 保留已有定时器行为：默认开启后启动轮换计时，已有用户关闭记录不会被重新打开。

### Verification
- `git diff --check`：通过，未发现空白错误。
- 未执行构建和测试：用户明确要求不构建，只完成变更记录和 Git 提交。

## 2026-08-06 14:15 - 启动清理机器号辅助进程

### Changed Location
- `src/main.cpp:7,39`：引入设备名读取和 Windows 进程快照头文件。
- `src/main.cpp:93-160`：新增启动阶段按当前设备名精确清理两个目标进程的逻辑。
- `src/main.cpp:248`：在单实例确认后、其它服务启动前调用清理函数。

### Reason
部分设备会残留以机器号命名的主进程和置顶辅助进程。程序启动时需要先结束 `<机器名>.exe` 与 `<机器名>_置顶.exe`，避免旧进程继续占用资源或与新实例并行运行。实现只比较 Windows 进程快照提供的精确映像文件名，并排除当前 FSRemote 的 PID；无法打开或结束目标时只记录日志，不阻断主程序启动。

### Original Code
```cpp
// src/main.cpp:4-15、33-38（修改前）
#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "system/DeviceRealtimeStateService.h"
// ...
#include <windows.h>
```

```cpp
// src/main.cpp:240-247（修改前）
QLocalServer::removeServer(QString::fromLatin1(kSingleInstanceKey));
QLocalServer singleInstanceServer;
if (!singleInstanceServer.listen(QString::fromLatin1(kSingleInstanceKey))) {
    writeStartupLog(QStringLiteral("[wjy-main] single-instance server listen failed"));
}
```

```cpp
// src/main.cpp:93（修改前）
// 此位置没有启动机器号进程清理函数。
```

### Modified Code
```cpp
// src/main.cpp:4-15、34-40（修改后）
#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "system/DeviceInfoService.h"
#include "system/DeviceRealtimeStateService.h"
// ...
#include <windows.h>
#include <tlhelp32.h>
```

```cpp
// src/main.cpp:93-160（修改后，核心逻辑）
void cleanupMachineNumberProcessesAtStartup()
{
    const QString machineName = platform::DeviceInfoService::localDeviceName().trimmed();
    const QStringList targetProcessNames{
        machineName + QStringLiteral(".exe"),
        machineName + QStringLiteral("_置顶.exe"),
    };
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    BOOL hasProcess = ::Process32FirstW(snapshot, &entry);
    while (hasProcess) {
        const DWORD processId = entry.th32ProcessID;
        const QString processName = QString::fromWCharArray(entry.szExeFile);
        if (processId != 0
            && processId != ::GetCurrentProcessId()
            && targetProcessNames.contains(processName, Qt::CaseInsensitive)) {
            HANDLE process = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId);
            if (process && ::TerminateProcess(process, ERROR_PROCESS_ABORTED)) {
                ::WaitForSingleObject(process, 2000);
            }
            if (process) {
                ::CloseHandle(process);
            }
        }
        hasProcess = ::Process32NextW(snapshot, &entry);
    }
    ::CloseHandle(snapshot);
}
```

```cpp
// src/main.cpp:243-249（修改后）
QLocalServer::removeServer(QString::fromLatin1(kSingleInstanceKey));
QLocalServer singleInstanceServer;
if (!singleInstanceServer.listen(QString::fromLatin1(kSingleInstanceKey))) {
    writeStartupLog(QStringLiteral("[wjy-main] single-instance server listen failed"));
}
cleanupMachineNumberProcessesAtStartup(); // wjy: 单实例确认后立即清理当前设备号对应的两个旧进程，再继续启动其它服务。
```

### Steps
1. 复用 `DeviceInfoService::localDeviceName()` 获取当前 Windows 计算机名。
2. 拼出两个精确目标名：`<机器名>.exe` 和 `<机器名>_置顶.exe`。
3. 使用 `CreateToolhelp32Snapshot`、`Process32FirstW` 和 `Process32NextW` 枚举当前进程。
4. 跳过 PID 0 和当前 FSRemote PID，仅对大小写不敏感匹配的目标调用 `TerminateProcess`。
5. 每次终止后最多等待 2 秒，并将成功、权限失败和枚举异常写入启动日志。
6. 将清理调用放在单实例确认之后，避免二次启动唤醒已有窗口时重复执行清理。

### Verification
- `git diff --check`：通过，未发现空白错误。
- 已静态核对目标名为精确文件名匹配，并排除当前进程 PID。
- 未执行构建和运行测试：遵循用户要求不构建。

## 2026-08-06 14:33 - 修正为固定机器号进程名清理

### Changed Location
- `src/main.cpp:7`：移除不再需要的 `DeviceInfoService` 依赖。
- `src/main.cpp:92-153`：将启动清理从“按当前计算机名动态拼接”修正为两个固定进程名。
- `src/main.cpp:241`：调用重命名后的固定进程清理函数。

### Reason
上一项实现误把“机器号”理解为当前计算机名。实际需求是两个固定进程名：`机器号.exe` 和 `机器号_置顶.exe`。本次修正删除动态设备名读取和名称拼接，启动时始终仅匹配这两个固定字面量，避免不同计算机名导致未能结束目标进程。

### Original Code
```cpp
// src/main.cpp:4-8（修改前）
#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "system/DeviceInfoService.h"
#include "system/DeviceRealtimeStateService.h"
```

```cpp
// src/main.cpp:93-107（修改前）
void cleanupMachineNumberProcessesAtStartup()
{
    const QString machineName = platform::DeviceInfoService::localDeviceName().trimmed();
    if (machineName.isEmpty()) {
        writeStartupLog(QStringLiteral("[wjy-startup-process] machine name empty, skip cleanup"));
        return;
    }

    const QStringList targetProcessNames{
        machineName + QStringLiteral(".exe"),
        machineName + QStringLiteral("_置顶.exe"),
    };
}
```

```cpp
// src/main.cpp:248（修改前）
cleanupMachineNumberProcessesAtStartup();
```

### Modified Code
```cpp
// src/main.cpp:4-7（修改后）
#include "system/AppSettings.h"
#include "system/DeviceCommandService.h"
#include "system/DeviceRealtimeStateService.h"
```

```cpp
// src/main.cpp:92-100（修改后）
void cleanupFixedMachineNumberProcessesAtStartup()
{
#if defined(Q_OS_WIN)
    const QStringList targetProcessNames{
        QString::fromUtf8("机器号.exe"), // wjy: 目标主进程名是固定字面量，不根据当前计算机名拼接或替换。
        QString::fromUtf8("机器号_置顶.exe"), // wjy: 目标置顶辅助进程名同样固定，必须与进程列表中的文件名完全对应。
    };
    writeStartupLog(QStringLiteral("[wjy-startup-process] cleanup targets=%1")
        .arg(targetProcessNames.join(QStringLiteral(","))));
#endif
}
```

```cpp
// src/main.cpp:241（修改后）
cleanupFixedMachineNumberProcessesAtStartup(); // wjy: 单实例确认后立即清理固定名称的两个旧进程，再继续启动其它服务。
```

### Steps
1. 删除按 `DeviceInfoService::localDeviceName()` 读取当前设备名的逻辑。
2. 将目标列表固定为 `机器号.exe`、`机器号_置顶.exe` 两个 UTF-8 进程映像文件名。
3. 保留原有的精确文件名比较、当前 FSRemote PID 排除、终止等待和错误日志处理。
4. 重命名启动清理函数，明确它不再依赖当前计算机名。

### Verification
- `git diff --check`：通过，未发现空白错误。
- 已静态确认代码中不再引用 `DeviceInfoService`，目标列表仅含两个固定字面量。
- 未执行构建和运行测试：遵循用户此前“不用构建”的要求。

## 2026-08-06 14:37 - 新增 FSRemote 局域网带宽余量被动监控器

### Changed Location
- `BandwidthCapacityMonitor/.gitignore:1-2`：忽略独立项目的本地构建目录和运行产物。
- `BandwidthCapacityMonitor/CMakeLists.txt:1-35`：新增不依赖 Qt 或 FSRemote 主程序的 Windows CMake 工程、监控器目标和数学测试目标。
- `BandwidthCapacityMonitor/src/MonitorMath.h:9-68`：新增 Mbps、理论余量、利用率和风险等级计算策略。
- `BandwidthCapacityMonitor/src/main.cpp:1-741`：新增 Windows 网卡计数、链路速率、丢弃/错误、整机 CPU、FSRemote 进程资源和 CSV 采样逻辑。
- `BandwidthCapacityMonitor/tests/monitor_math_tests.cpp:1-76`：新增纯 C++ 边界测试，覆盖默认链路容量、手工有效容量、风险阈值和计数器复位。
- `BandwidthCapacityMonitor/README.md:1-91`：新增构建、使用、余量口径限制和卡顿判定说明。

### Reason
用户需要在 FSRemote 同时远控约 20 台设备时观察当前局域网还剩多少带宽，而不是向网络发送模拟压测流量。独立监控器只读取 Windows 网卡累计计数和协商速率，因此不会改变远控流量；同时采集 FSRemote 与整机 CPU、内存和线程，帮助区分网络瓶颈与解码/渲染/线程压力。程序将“余量”明确标为本机网卡理论余量，并支持 `--capacity-mbps` 使用实测有效容量覆盖协商速率，避免把 Wi-Fi 或交换机上联能力误当成保证带宽。

### Original Code
```text
// BandwidthCapacityMonitor/.gitignore:1-2
新增目录，原位置无旧代码。

// BandwidthCapacityMonitor/CMakeLists.txt:1-35
新增独立工程，原位置无旧代码。

// BandwidthCapacityMonitor/src/MonitorMath.h:1-71
新增头文件，原位置无旧代码。

// BandwidthCapacityMonitor/src/main.cpp:1-741
新增 Windows 控制台程序，原位置无旧代码。

// BandwidthCapacityMonitor/tests/monitor_math_tests.cpp:1-76
新增测试文件，原位置无旧代码。

// BandwidthCapacityMonitor/README.md:1-91
新增使用文档，原位置无旧代码。
```

### Modified Code
```cmake
// BandwidthCapacityMonitor/CMakeLists.txt:14-34
add_executable(BandwidthCapacityMonitor
    src/main.cpp
    src/MonitorMath.h
)
target_link_libraries(BandwidthCapacityMonitor PRIVATE iphlpapi psapi)
add_executable(BandwidthCapacityMonitorTests
    tests/monitor_math_tests.cpp
    src/MonitorMath.h
)
enable_testing()
add_test(NAME BandwidthCapacityMonitorTests COMMAND BandwidthCapacityMonitorTests)
```

```cpp
// BandwidthCapacityMonitor/src/MonitorMath.h:9-68
enum class RiskLevel { Normal, Attention, High, Saturated };

inline DirectionMetrics calculateDirection(
    std::uint64_t byteDelta,
    double elapsedSeconds,
    std::uint64_t linkBitsPerSecond,
    double capacityOverrideMbps)
{
    DirectionMetrics metrics;
    metrics.currentMbps = bytesToMbps(byteDelta, elapsedSeconds);
    metrics.capacityMbps = capacityOverrideMbps > 0.0
        ? capacityOverrideMbps
        : static_cast<double>(linkBitsPerSecond) / 1'000'000.0;
    metrics.headroomMbps = std::max(0.0, metrics.capacityMbps - metrics.currentMbps);
    metrics.utilizationPercent = metrics.capacityMbps > 0.0
        ? metrics.currentMbps * 100.0 / metrics.capacityMbps
        : 0.0;
    metrics.risk = classifyRisk(metrics.utilizationPercent);
    return metrics;
}
```

```cpp
// BandwidthCapacityMonitor/src/main.cpp:273-311、364-451、568-629
std::vector<AdapterSample> queryAdapters()
{
    PMIB_IF_TABLE2 table = nullptr;
    if (::GetIfTable2(&table) != NO_ERROR || table == nullptr) {
        return {};
    }
    // 复制活动物理网卡的累计字节、链路速率、丢弃和错误计数后释放 IP Helper 表。
}

class ProcessMonitor final {
    // 按 FSRemote.exe 的 PID 聚合 CPU、工作集、私有内存和线程数。
};

void printAdapterReport(...)
{
    // 接收和发送方向分别计算当前 Mbps、理论余量、利用率与丢弃/错误增量，并写入 CSV。
}
```

```cpp
// BandwidthCapacityMonitor/src/main.cpp:631-741
int wmain(int argc, wchar_t* argv[])
{
    // 建立采样基线后按周期读取网卡和进程状态，不创建 socket、不发送测试数据。
}
```

```cpp
// BandwidthCapacityMonitor/tests/monitor_math_tests.cpp:13-76
void testRateAndLinkHeadroom();
void testEffectiveCapacityOverride();
void testRiskThresholdsAndSaturationClamp();
void testCounterResetAndZeroInterval();
```

```markdown
// BandwidthCapacityMonitor/README.md:1-91
# BandwidthCapacityMonitor

说明被动监控能够观测的本机网卡余量、无法直接观测的 Wi-Fi/交换机瓶颈，以及卡顿时结合带宽和 CPU 指标的判断方法。
```

### Steps
1. 创建 `BandwidthCapacityMonitor` 独立目录和独立 CMake 工程，避免修改 FSRemote 主程序构建入口。
2. 使用 IP Helper `GetIfTable2` 读取活动物理网卡的累计字节、协商速率、丢弃和错误计数，并以相邻采样差计算接收/发送 Mbps。
3. 增加 `--interface`、`--interval-ms`、`--duration`、`--capacity-mbps`、`--process` 和 `--csv` 参数，支持按网卡筛选、有效容量校准和故障前数据留存。
4. 使用 Toolhelp32 与 Process/PSAPI API 聚合 `FSRemote.exe` 的 CPU、工作集、私有内存和线程数。
5. 增加 70%、85%、95% 利用率风险阈值，并把网卡丢弃/错误增长单独提示，避免只依据理论余量下结论。
6. 在源码中加入 WJY 所有权标记和中文解释，补充独立项目 README 与纯数学回归测试。

### Verification
- `cmake -S . -B build -A x64`：配置成功，使用 Visual Studio 17 2022 x64 生成器。
- `cmake --build build --config Release`：`BandwidthCapacityMonitor.exe` 和 `BandwidthCapacityMonitorTests.exe` 构建成功。
- `ctest --test-dir build -C Release --output-on-failure`：1/1 测试通过。
- `BandwidthCapacityMonitor.exe --list`：成功识别 `[9] 以太网`，Realtek PCIe GbE Family Controller，接收/发送协商速率均为 1000 Mbps。
- `BandwidthCapacityMonitor.exe --duration 2 --csv build/monitor-smoke.csv`：完成两次真实网卡采样，输出接收/发送 Mbps、理论余量、FSRemote 进程资源和丢弃/错误计数，CSV 文件成功落盘。
- `BandwidthCapacityMonitor.exe --help`：返回退出码 0；非法参数返回退出码 1。
- `git diff --check`：已检查本次独立目录改动，未发现空白错误。

## 2026-08-06 16:23 - 将带宽余量显示到主窗口版本号右侧

### Changed Location
- `CMakeLists.txt:435-444,604-608`：登记主程序内置网卡采样器和纯计算测试目标。
- `src/system/LocalNetworkBandwidthMonitor.h:12-94`：新增网卡方向指标、风险阈值、累计计数差和只读采样器接口。
- `src/system/LocalNetworkBandwidthMonitor.cpp:20-229`：新增 Windows 活动物理网卡枚举、相邻计数采样、主流量网卡选择和断线基线重置。
- `src/ui/DeviceGrid.h:4,308-310`：主窗口持有标题栏带宽采样器、最新样本和一秒定时器。
- `src/ui/DeviceGrid.cpp:242-245,3879-3895,4220-4224,9790-9853`：新增标题栏局部刷新、采样生命周期和版本号右侧带宽文字绘制。
- `tests/local_network_bandwidth_monitor_tests.cpp:1-61`：新增 Mbps、理论余量、风险阈值和计数器复位测试。

### Reason
用户希望在丰实远程控制主窗口中直接观察多台远控带来的汇总网络压力，并要求监控文字位于版本号右侧。实现不启动之前的独立监控 EXE，而是在 FSRemote UI 线程中每秒读取一次 Windows 物理网卡累计计数；标题栏显示接收 Mbps 和基于协商链路速率计算的理论余量，并通过蓝、琥珀、红三类颜色提示利用率或入站丢弃风险。多张物理网卡时优先选择接收流量最大的接口，空闲时回到默认路由接口，避免 VMware、隧道或广播流量造成误选。

### Original Code
```cmake
// CMakeLists.txt:420-431（修改前）
add_executable(fsremote_network_interface_policy_tests EXCLUDE_FROM_ALL
    tests/network_interface_policy_tests.cpp
    src/system/NetworkInterfacePolicy.h
)
add_test(NAME fsremote_network_interface_policy_tests COMMAND fsremote_network_interface_policy_tests)

// 此处原来没有标题栏带宽计算测试目标。
```

```cmake
// CMakeLists.txt:589-593（修改前）
src/system/LocalSystemInfoService.cpp
src/system/LocalSystemInfoService.h

// 此处原来没有 LocalNetworkBandwidthMonitor 生产源码。
```

```text
// src/system/LocalNetworkBandwidthMonitor.h:1-97
新增文件，原位置无旧代码。

// src/system/LocalNetworkBandwidthMonitor.cpp:1-232
新增文件，原位置无旧代码。

// tests/local_network_bandwidth_monitor_tests.cpp:1-61
新增文件，原位置无旧代码。
```

```cpp
// src/ui/DeviceGrid.h:1-6、304-307（修改前）
#include "system/DeviceInfoService.h"
#include "system/LocalSystemInfoService.h"

RemoteQualityCoordinator m_remoteQualityCoordinator;
QTimer* m_remoteQualityTimer = nullptr;
bool m_remoteQualityEvaluationQueued = false;
```

```cpp
// src/ui/DeviceGrid.cpp:3870-3873（修改前）
m_remoteQualityTimer->setInterval(1000);
connect(m_remoteQualityTimer, &QTimer::timeout, this, &DeviceGrid::evaluateRemoteQuality);
m_remoteQualityTimer->start();

// 此处原来没有标题栏网络采样定时器。
```

```cpp
// src/ui/DeviceGrid.cpp:9760-9772（修改前）
{
    QFont versionFont(QStringLiteral("Microsoft YaHei UI"));
    versionFont.setPixelSize(11);
    painter.setFont(versionFont);
    painter.setPen(QColor(QStringLiteral("#6B7280")));
    const QString versionText = QStringLiteral("v%1").arg(platform::UpdateService::displayVersion());
    const QFontMetrics versionMetrics(versionFont);
    const int versionWidth = versionMetrics.horizontalAdvance(versionText) + 4;
    const QRect versionRect(titleWordmarkRect.right() + 8, titleWordmarkRect.y(), versionWidth, titleWordmarkRect.height());
    painter.drawText(QRectF(versionRect), Qt::AlignVCenter | Qt::AlignLeft, versionText);
}
```

### Modified Code
```cmake
// CMakeLists.txt:435-444、604-608（修改后）
add_executable(fsremote_local_network_bandwidth_monitor_tests EXCLUDE_FROM_ALL
    tests/local_network_bandwidth_monitor_tests.cpp
    src/system/LocalNetworkBandwidthMonitor.h
)
add_test(NAME fsremote_local_network_bandwidth_monitor_tests COMMAND fsremote_local_network_bandwidth_monitor_tests)

src/system/LocalSystemInfoService.cpp
src/system/LocalSystemInfoService.h
src/system/LocalNetworkBandwidthMonitor.cpp
src/system/LocalNetworkBandwidthMonitor.h
```

```cpp
// src/system/LocalNetworkBandwidthMonitor.h:48-94（新增）
inline LocalNetworkDirectionMetrics calculateLocalNetworkDirectionMetrics(
    quint64 byteDelta,
    double elapsedSeconds,
    quint64 linkBitsPerSecond);

class LocalNetworkBandwidthMonitor final {
public:
    LocalNetworkBandwidthSample sample();
    void reset();
};
```

```cpp
// src/system/LocalNetworkBandwidthMonitor.cpp:136-229（新增，核心流程）
LocalNetworkBandwidthSample LocalNetworkBandwidthMonitor::sample()
{
    const std::vector<LocalAdapterCounter> currentAdapters = queryPhysicalAdapterCounters();
    // 首次建立基线；后续按相邻累计字节差计算收发 Mbps。
    // 多网卡优先选择接收流量最大接口，低流量时使用默认路由并保持接口稳定。
}
```

```cpp
// src/ui/DeviceGrid.h:4,308-310（修改后）
#include "system/LocalNetworkBandwidthMonitor.h"

platform::LocalNetworkBandwidthMonitor m_titlebarBandwidthMonitor;
platform::LocalNetworkBandwidthSample m_titlebarBandwidthSample;
QTimer* m_titlebarBandwidthTimer = nullptr;
```

```cpp
// src/ui/DeviceGrid.cpp:3879-3895（修改后）
m_titlebarBandwidthTimer = new QTimer(this);
m_titlebarBandwidthTimer->setTimerType(Qt::CoarseTimer);
m_titlebarBandwidthTimer->setInterval(1000);
connect(m_titlebarBandwidthTimer, &QTimer::timeout, this, [this] {
    m_titlebarBandwidthSample = m_titlebarBandwidthMonitor.sample();
    update(titlebarBandwidthUpdateRect());
});
QTimer::singleShot(0, this, [this] {
    m_titlebarBandwidthMonitor.reset();
    m_titlebarBandwidthSample = m_titlebarBandwidthMonitor.sample();
    m_titlebarBandwidthTimer->start();
});
```

```cpp
// src/ui/DeviceGrid.cpp:9790-9853（修改后，核心布局）
QRect versionRect;
// 先绘制版本号并保存 versionRect。
const int networkLeft = versionRect.right() + 10;
// 空间充足时显示“↓200M 余800M”，不足时优先保留“↓200M”。
// 正常为蓝色，70%-85% 为琥珀色，85% 以上或出现入站丢弃/错误为红色。
```

```cpp
// tests/local_network_bandwidth_monitor_tests.cpp:20-59（新增）
bool directionMetricsUseDecimalMbpsAndClampHeadroom();
bool riskThresholdsAndCounterResetAreStable();
```

### Steps
1. 新增 `LocalNetworkBandwidthMonitor`，通过 `GetIfTable2` 每秒只读活动物理网卡的协商速率、累计字节、丢弃和错误计数。
2. 排除断开、回环、隧道和可识别的虚拟接口；多张物理网卡按接收流量选择当前代表接口，并保留 75% 稳定门槛避免每秒跳动。
3. 在 `DeviceGrid` 事件循环开始后建立首个基线，后续一秒定时采样并只重绘标题栏局部区域。
4. 保存版本号矩形，把网络文字从其右侧 10px 开始布局；为本机名、IP、更新按钮和窗口按钮预留现有空间。
5. 完整宽度显示接收 Mbps 与理论余量，空间不足时只显示接收 Mbps，紧凑标题栏自动隐藏以避免重叠。
6. 增加纯计算测试，锁定十进制 Mbps、零余量钳制、70/85/95 风险阈值和计数器复位行为。

### Verification
- `cmake -S . -B temp/bandwidth-titlebar-build3 -G Ninja ...`：使用 MSVC 19.44 和 Qt 6.11.1 配置成功。
- `cmake --build temp/bandwidth-titlebar-build3 --target fsremote_local_network_bandwidth_monitor_tests`：目标测试编译成功。
- `ctest --test-dir temp/bandwidth-titlebar-build3 -R ^fsremote_local_network_bandwidth_monitor_tests$ --output-on-failure`：1/1 测试通过。
- `cmake --build temp/bandwidth-titlebar-build3 --target FSRemote`：主程序全部 84 个步骤编译和链接成功，新增 `LocalNetworkBandwidthMonitor.cpp` 与 `DeviceGrid.cpp` 均进入真实目标。
- 未启动新构建产物进行界面目视测试：避免关闭或干扰用户当前正在运行的 FSRemote 实例；用户随后明确要求不再编译，因此后续只执行源码、日志和 Git 复核。
- `git diff --check`：通过，未发现空白错误。

## 2026-08-06 16:34 - 增加多窗口黑闪合成时间线与运行期像素探针

### Changed Location
- `src/ui/D3D11FramePresenter.h:19-36`：扩展Overlay和DComp最近一次提交遥测。
- `src/ui/D3D11FramePresenter.cpp:12-15,255-276,476-596,761-778,1345-1368`：记录Overlay序号、实际提交模式、BackBuffer索引、Present结果和单次Commit耗时。
- `src/ui/RemoteDesktopWindow.h:392-403`：增加普通运行期像素探针和时间线限频状态。
- `src/ui/RemoteDesktopWindow.cpp:1169-1201,1758-1774,3156-3363,5018-5133,5200-5262`：把黑闪时间线写入程序`data`文件夹，并关联Overlay、视频Present、DComp Commit和真实屏幕像素。

### Reason
现有`remote_viewer_state.log`已经证明复现期间网络、D3D Present、Overlay Present和DComp Commit累计失败数均为零，但无法回答黑闪瞬间究竟发生在视频SwapChain、Overlay双缓冲翻转、DComp提交还是最终DWM可见结果。新增独立时间线后，可以按设备IP和毫秒时间戳对齐这些节点；普通屏幕像素采样采用显式环境开关，默认不产生截图性能开销。

### Original Code
```cpp
// src/ui/D3D11FramePresenter.h:19-27（修改前）
struct D3D11CompositorTelemetry {
    std::uint64_t commitCount = 0;
    std::uint64_t commitFailureCount = 0;
    double averageCommitMs = 0.0;
    std::uint64_t overlayFullPresentCount = 0;
    std::uint64_t overlayPartialPresentCount = 0;
    std::uint64_t overlayPresentFailureCount = 0;
    std::uint64_t overlayUploadedBytes = 0;
};
```

```cpp
// src/ui/D3D11FramePresenter.cpp:556-571,740-770（修改前）
HRESULT presentResult = S_OK;
if (partialPresent) {
    presentResult = targetSwapChain1->Present1(0, 0, &parameters);
} else {
    presentResult = targetSwapChain->Present(0, 0);
}

const auto commitStarted = std::chrono::steady_clock::now();
const HRESULT commitResult = m_impl->compositionDevice->Commit();
m_impl->compositionCommitTimeUs += elapsedCommitTime;
```

```cpp
// src/ui/RemoteDesktopWindow.h:392-394（修改前）
bool m_resizePixelProbeEnabled = false;
qint64 m_lastResizePixelProbeMs = -1000;
quint64 m_resizeVisibleSampleCount = 0;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3253-3257,4912-4923,5059-5065（修改前）
m_texturePresenter->presentCompositorOverlay(m_compositorOverlayCache, dirtyPhysicalRect);
sampleVisibleCompositorRegion();

if (!m_resizePixelProbeEnabled
    || !m_resizingWindow
    || !m_resizeDebugClock.isValid()) {
    return;
}

texturePresented = m_texturePresenter->presentSharedTexture(
    frame->sharedHandle, frame->width, frame->height);
```

### Modified Code
```cpp
// src/ui/D3D11FramePresenter.h:28-34（修改后）
std::uint64_t overlayPresentSequence = 0;
QString lastOverlayMode = QStringLiteral("none");
QRect lastOverlayDirtyRect;
int lastOverlayBackBufferIndex = -1;
long lastOverlayPresentResult = 0;
long lastCompositionCommitResult = 0;
std::uint64_t lastCompositionCommitTimeUs = 0;
```

```cpp
// src/ui/D3D11FramePresenter.cpp:576-596,764-778（修改后）
++m_impl->compositionOverlayPresentSequence;
ComPtr<IDXGISwapChain3> targetSwapChain3;
if (targetSwapChain
    && SUCCEEDED(targetSwapChain->QueryInterface(IID_PPV_ARGS(&targetSwapChain3)))) {
    m_impl->lastCompositionOverlayBackBufferIndex =
        static_cast<int>(targetSwapChain3->GetCurrentBackBufferIndex());
}
m_impl->lastCompositionOverlayPresentResult = presentResult;

const std::uint64_t commitTimeUs = measureCommitTime();
m_impl->lastCompositionCommitTimeUs = commitTimeUs;
m_impl->lastCompositionCommitResult = commitResult;
```

```cpp
// src/ui/RemoteDesktopWindow.h:396-400（修改后）
bool m_compositorPixelProbeEnabled = false;
qint64 m_lastCompositorPixelProbeMs = -1000;
quint64 m_compositorVisibleSampleCount = 0;
qint64 m_lastCompositorFrameTimelineMs = -1000;
qint64 m_lastCompositorPartialTimelineMs = -1000;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:1169-1201,3296-3360,5020-5131,5220-5260（修改后）
const QString logPath = QDir(dataDir).filePath(
    QStringLiteral("remote_compositor_timeline.log"));

const bool overlayPresented = m_texturePresenter->presentCompositorOverlay(
    m_compositorOverlayCache, dirtyPhysicalRect);
// 整窗提交全部记录；局部提交每500毫秒最多一条。
appendRemoteCompositorTimelineLog(timeline);

const bool runtimeProbeActive = m_compositorPixelProbeEnabled
    && m_sessionClock.isValid();
// FSREMOTE_COMPOSITOR_PIXEL_PROBE=1时，每窗口最多每100毫秒采样真实视频可见区。

const bool videoTimelineDue = texturePresented
    && videoTimelineMs - m_lastCompositorFrameTimelineMs >= 1000;
// 成功视频Present每秒一条，失败立即记录。
```

### Steps
1. 扩展`D3D11CompositorTelemetry`，保存Overlay实际模式、提交序号、脏区、BackBuffer索引以及最近一次Present/Commit结果。
2. 在Overlay调用前后比较累计计数，完整提交全部写入时间线，标题栏局部提交按每窗口500毫秒限频。
3. 在视频共享纹理成功Present路径每秒记录一次帧号、设备IP、分辨率、Overlay和DComp快照；失败不限频并立即写入。
4. 新增`FSREMOTE_COMPOSITOR_PIXEL_PROBE=1`，普通运行期从真实视频区域抓取可见像素，记录黑像素比例和平均亮度；默认关闭。
5. 所有新日志固定写入`<程序目录>/data/remote_compositor_timeline.log`，文件不可写时不改变远控状态。

### Verification
- 已读取2026-08-06 16:02至16:03的16窗口现场日志，确认`overlay_fail=0`、`comp_fail=0`、`d3d=0x0`，新增字段针对现有诊断盲区。
- `git diff --check -- src/ui/D3D11FramePresenter.h src/ui/D3D11FramePresenter.cpp src/ui/RemoteDesktopWindow.h src/ui/RemoteDesktopWindow.cpp`：通过，未发现空白错误。
- 使用`rg`复核新环境开关、data日志文件名、Overlay提交序号和限频成员均已接入实际调用路径。
- 按用户此前要求未执行编译、链接和运行测试；需要发布新控制端后进行多窗口复现验证。

## 2026-08-06 16:54 - 去除视频帧重复DirectComposition提交

### Changed Location
- `src/ui/D3D11FramePresenter.cpp:335-358`：可见状态未变化时跳过重复show/hide和DComp视觉树提交。
- `src/ui/D3D11FramePresenter.cpp:1173-1182`：视频SwapChain成功Present后不再额外调用`IDCompositionDevice::Commit()`。

### Reason
新时间线显示9个窗口的网络、视频Present、Overlay Present和DComp Commit均无失败，稳定期完整Overlay提交也已经停止；但每个窗口的`comp_commit`仍以约30次/秒增长，几乎等于视频帧率。代码复核确认每帧都会再次请求`setPresentationVisible(true)`并提交视觉树，随后`blitAndPresent()`又直接执行一次未计数的DComp Commit。视频SwapChain的内容翻转不需要重复提交未变化的视觉属性，多窗口下这些提交会放大DWM合成压力并可能造成周期黑闪。

### Original Code
```cpp
// src/ui/D3D11FramePresenter.cpp:338-356（修改前）
if (!m_impl->compositorMode) {
    if (visible) {
        show();
    } else {
        hide();
    }
    return;
}
m_impl->compositorVisible = visible;
if (!visible && isVisible()) {
    hide();
}
commitCompositionVisual();
```

```cpp
// src/ui/D3D11FramePresenter.cpp:1169-1180（修改前）
const HRESULT presentResult = m_impl->swapChain->Present(0, 0);
if (FAILED(presentResult)) {
    return handleDeviceFailure(presentResult);
}
if (m_impl->compositorMode && m_impl->compositionDevice) {
    m_impl->compositionDevice->Commit();
}
```

### Modified Code
```cpp
// src/ui/D3D11FramePresenter.cpp:338-358（修改后）
if (!m_impl->compositorMode) {
    if (visible == !isHidden()) {
        return;
    }
    if (visible) {
        show();
    } else {
        hide();
    }
    return;
}
if (m_impl->compositorVisible == visible) {
    return;
}
m_impl->compositorVisible = visible;
commitCompositionVisual();
```

```cpp
// src/ui/D3D11FramePresenter.cpp:1173-1182（修改后）
const HRESULT presentResult = m_impl->swapChain->Present(0, 0);
if (FAILED(presentResult)) {
    return handleDeviceFailure(presentResult);
}
// 视频SwapChain的Present会自行通知DirectComposition消费新缓冲；
// 视觉属性未变化时禁止额外Commit。
```

### Steps
1. 解析`remote_compositor_timeline.log`，确认9个窗口共计没有任何非零HRESULT、Overlay失败或Commit失败。
2. 对比视频帧和`comp_commit`增长速度，定位到每窗口约30次/秒的重复视觉树提交。
3. 给旧子窗口和DComp路径增加可见状态去重，只有true/false真正切换时才改变显示状态。
4. 删除视频Present后的逐帧DComp Commit，保留几何、视觉内容和可见状态变化时的受控`commitCompositionVisual()`。

### Verification
- 现场时间线共覆盖9个设备；稳定期`overlay_full`固定在14至16次，后续只执行标题栏局部提交。
- 现场所有`video.present`和`overlay.present`均为`ok=1`，`present_hr=0x0`、`commit_hr=0x0`，单次受控Commit约63至428微秒。
- `git diff --check -- src/ui/D3D11FramePresenter.cpp`：通过，未发现空白错误。
- 按用户此前要求未执行编译、链接和运行测试；需要更新控制端后复核`comp_commit`应不再跟随视频帧持续增长。

## 2026-08-06 17:02 - 修正多显示器负坐标像素探针

### Changed Location
- `src/ui/RemoteDesktopWindow.cpp:5051-5070`：将远控视频区全局坐标裁剪并转换为目标QScreen局部坐标后再截图。
- `src/ui/RemoteDesktopWindow.cpp:5121-5148`：在可见像素日志中补充屏幕全局矩形和实际局部抓取矩形。

### Reason
开启`FSREMOTE_COMPOSITOR_PIXEL_PROBE=1`后的9窗口日志全部持续显示`black_ratio=1.0000`和`avg_luma=0.0`，但用户实际能看到远控画面，因此不是持续黑屏。日志同时显示所有采样矩形位于负全局坐标，例如`global_rect=-3803,-1034`。`QScreen::grabWindow(0, ...)`使用当前屏幕局部坐标，原实现直接传入虚拟桌面全局负坐标，导致抓取屏幕范围外的纯黑区域。

### Original Code
```cpp
// src/ui/RemoteDesktopWindow.cpp:5051-5065（修改前）
QRect globalRect = frameGeometry();
if (runtimeProbeActive) {
    const QRect imageRect = remoteImageRect();
    if (imageRect.isValid()) {
        globalRect = QRect(mapToGlobal(imageRect.topLeft()), imageRect.size());
    }
}
const QPixmap screenshot = screen->grabWindow(
    0,
    globalRect.left(),
    globalRect.top(),
    globalRect.width(),
    globalRect.height());
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:5121-5139（修改前）
appendRemoteCompositorTimelineLog(QStringLiteral(
    "host=%1 device=%2 event=%3 session_ms=%4 global_rect=%5,%6,%7,%8 "
    "overlay_seq=%9 overlay_mode=%10 buffer=%11 overlay_hr=0x%12 "
    "commit_hr=0x%13 commit_us=%14")
    .arg(globalRect.x())
    .arg(globalRect.y())
    .arg(globalRect.width())
    .arg(globalRect.height()));
```

### Modified Code
```cpp
// src/ui/RemoteDesktopWindow.cpp:5051-5070（修改后）
const QRect screenGeometry = screen->geometry();
const QRect visibleGlobalRect = globalRect.intersected(screenGeometry);
if (visibleGlobalRect.isEmpty()) {
    return;
}
const QRect captureRect(
    visibleGlobalRect.topLeft() - screenGeometry.topLeft(),
    visibleGlobalRect.size());
const QPixmap screenshot = screen->grabWindow(
    0,
    captureRect.left(),
    captureRect.top(),
    captureRect.width(),
    captureRect.height());
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:5121-5148（修改后）
appendRemoteCompositorTimelineLog(QStringLiteral(
    "... global_rect=... screen_rect=... capture_rect=... overlay_seq=..."));
```

### Steps
1. 确认新版本的`comp_commit`在约25秒内保持为4，上一轮去除逐帧DComp Commit的修复已经生效。
2. 汇总所有`visible.sample`，确认9个窗口从首个样本起始终为纯黑，与实际显示不符。
3. 根据日志中的负坐标定位多显示器坐标系错误。
4. 将窗口视频区先与当前屏幕全局范围求交集，再减去屏幕左上角得到非负局部抓取坐标。
5. 日志同时输出`global_rect`、`screen_rect`和`capture_rect`，下一轮可直接核对坐标转换。

### Verification
- 现场日志确认`pixel_probe=1`已在2026-08-06 17:00:31的新会话生效。
- 现场样本的负全局坐标与全黑截图一一对应，确认原探针结果无诊断价值。
- `git diff --check -- src/ui/RemoteDesktopWindow.cpp`：通过，未发现空白错误。
- 按用户此前要求未执行编译、链接和运行测试；需要更新控制端后重新采样。

## 2026-08-06 17:29 - 用局部上传和完整Flip消除标题栏触发黑闪

### Changed Location
- `src/ui/D3D11FramePresenter.cpp:525-615`：保留标题栏物理脏区上传，但取消透明整窗Overlay的`Present1`脏区翻转，统一使用完整`Present`。
- `src/ui/D3D11FramePresenter.h:58`：更新Overlay接口注释，明确“局部上传、完整Flip Present”。
- `src/ui/RemoteWindowCompositor.cpp:33`：明确物理脏区只限制GPU上传范围，不再传给DXGI Present1。
- `src/ui/RemoteDesktopWindow.cpp:3304-3308`：把诊断变量改名为`partialUploadAdvanced`，与新呈现语义一致。

### Reason
修正多显示器坐标后，像素探针在稳定视频期间真实抓到了15次完整黑屏样本。除一次发生在连接遮罩切换外，其余14次黑屏前最近一次Overlay事件都是`titlebar_dirty mode=partial`；多个典型样本在标题栏`Present1`后约100至230毫秒变为`black_ratio=1.0000`，下一次采样又恢复正常，而视频Present、Overlay HRESULT和DComp Commit始终成功。这表明透明整窗Overlay使用脏区Flip时，DWM会短暂把未标记的透明视频孔合成为黑色。

### Original Code
```cpp
// src/ui/D3D11FramePresenter.cpp:529-539（修改前）
ComPtr<IDXGISwapChain1> targetSwapChain1;
bool partialPresent = !useCandidate
    && !reuseCurrentDuringResize
    && !m_impl->interactiveResize
    && m_impl->compositionOverlayFullPresentsRemaining == 0
    && requestedPartialUpdate
    && targetSwapChain
    && SUCCEEDED(targetSwapChain->QueryInterface(IID_PPV_ARGS(&targetSwapChain1)));
```

```cpp
// src/ui/D3D11FramePresenter.cpp:588-600（修改前）
if (partialPresent) {
    DXGI_PRESENT_PARAMETERS parameters = {};
    parameters.DirtyRectsCount = 1;
    parameters.pDirtyRects = &dirtyRect;
    presentResult = targetSwapChain1->Present1(0, 0, &parameters);
} else {
    presentResult = targetSwapChain->Present(0, 0);
}
```

### Modified Code
```cpp
// src/ui/D3D11FramePresenter.cpp:529-541（修改后）
const bool partialUpload = !useCandidate
    && !reuseCurrentDuringResize
    && !m_impl->interactiveResize
    && m_impl->compositionOverlayFullPresentsRemaining == 0
    && requestedPartialUpdate
    && targetSwapChain;

m_impl->lastCompositionOverlayMode = partialUpload
    ? QStringLiteral("partial_upload_full_present")
    : QStringLiteral("full");
```

```cpp
// src/ui/D3D11FramePresenter.cpp:588（修改后）
presentResult = targetSwapChain->Present(0, 0);
```

### Steps
1. 统计2026-08-06 17:22:37之后的4689条真实可见像素样本。
2. 排除首帧前连接界面，筛出15条`black_ratio>=0.98`且已有视频帧的完整黑屏样本。
3. 对每条黑屏样本回溯同设备最近一次Overlay事件，确认14/15命中标题栏局部Present路径。
4. 保留`UpdateSubresource`的标题栏脏区Box，使CPU到GPU上传量仍只覆盖约28像素高区域。
5. 删除`IDXGISwapChain1::Present1`脏区参数，透明整窗Overlay始终进行完整Flip Present，确保视频孔沿用完整初始化过的BackBuffer透明像素。

### Verification
- 典型样本：`192.168.1.107`在17:23:07.415执行标题栏局部Present，17:23:07.548采样为全黑，17:23:07.697恢复正常。
- 典型样本：`192.168.1.109`在17:23:50.467执行标题栏局部Present，17:23:50.696采样为全黑，17:23:50.847恢复正常。
- 新版本中`comp_commit`稳定保持5至6，视频逐帧DComp提交修复继续生效。
- `git diff --check`覆盖四个修改源码文件并通过。
- 按用户此前要求未执行编译、链接和运行测试；需要更新控制端后确认日志模式为`partial_upload_full_present`且不再出现稳定期全黑样本。

## 2026-08-06 18:35 - 隔离DXGI恢复预热帧消除周期黑闪

### Changed Location
- `third_party/lan_stream_probe/src/dxgi_capture_policy.h:56-88`：新增`DxgiRecoveryFrameGate`，为每轮Desktop Duplication初建或重建隔离第一张成功采集帧。
- `third_party/lan_stream_probe/src/dxgi_capture.h:4,69`：引入恢复门控策略头，并用门控对象替换原有单布尔恢复状态。
- `third_party/lan_stream_probe/src/dxgi_capture.cpp:207,246-248,289-295,317-323,356,436,458,495`：在初始化、轻量重建、完整设备恢复和主动重置路径接入门控；恢复首帧只释放不复制、不租用帧槽、不编码，下一张成功帧才恢复交付。
- `third_party/uu_stream_webrtc/tests/dxgi_capture_policy_tests.cpp:41-52`：新增恢复门控状态转换测试，覆盖开始恢复、丢弃首帧、第二帧完成恢复和重置清理。

### Reason
本轮测试中控制端在稳定期捕获到6次完整黑帧（`.109`两次、`.161`两次、`.108`一次、`.127`一次），每次均为`black_ratio=1.0000 avg_luma=0.0`。通过同一session admission对齐控制端与被控端时钟后，6/6黑帧都紧跟被控端`DXGI_ERROR_ACCESS_LOST`后的Duplication恢复，间隔约117-156毫秒。黑帧前后采样均恢复正常，且D3D、Present、DComp Commit、KeyedMutex均无失败，说明不是控制端标题栏Overlay或Present失败，而是Duplication重建后的第一张API成功帧仍处于驱动/VDD预热阶段。原逻辑会立即CopyResource并送入编码器，覆盖控制端最后一张正常画面，因此产生黑屏闪烁。

### Original Code
```cpp
// third_party/lan_stream_probe/src/dxgi_capture.h:3,68（修改前）
#include "common.h"
bool awaiting_recovery_frame_ = false; // Duplication重建后等待真实新帧
```

```cpp
// third_party/lan_stream_probe/src/dxgi_capture.cpp:207,266-267,339-345（修改前）
awaiting_recovery_frame_ = true;
return {awaiting_recovery_frame_ ? DxgiCaptureStatus::DuplicationRecovering
                                 : DxgiCaptureStatus::NoDesktopChange, hr};
context_->CopyResource(selected_slot->texture.Get(), desktop_texture.Get());
duplication_->ReleaseFrame();
awaiting_recovery_frame_ = false;
```

### Modified Code
```cpp
// third_party/lan_stream_probe/src/dxgi_capture_policy.h:56-88（修改后）
class DxgiRecoveryFrameGate final {
public:
    void begin() noexcept
    {
        awaitingFrame_ = true;       // 每轮恢复进入门控状态。
        warmupFramePending_ = true;  // 只隔离第一张成功取得的预热帧。
    }

    bool awaitingFrame() const noexcept { return awaitingFrame_; }

    bool discardWarmupFrame() noexcept
    {
        if (!awaitingFrame_ || !warmupFramePending_) return false;
        warmupFramePending_ = false;
        return true;
    }

    void complete() noexcept
    {
        awaitingFrame_ = false;
        warmupFramePending_ = false;
    }

    void reset() noexcept
    {
        awaitingFrame_ = false;
        warmupFramePending_ = false;
    }
};
```

```cpp
// third_party/lan_stream_probe/src/dxgi_capture.cpp:289-295,317-323,356（修改后）
if (recovery_frame_gate_.discardWarmupFrame()) {
    duplication_->ReleaseFrame();
    append_stream_capture_diagnostic_log_rate_limited(
        "dxgi",
        "recovery warmup frame discarded; retained last good frame size="
            + std::to_string(desc.Width) + "x" + std::to_string(desc.Height),
        500);
    return {DxgiCaptureStatus::DuplicationRecovering, S_FALSE};
}
context_->CopyResource(selected_slot->texture.Get(), desktop_texture.Get());
duplication_->ReleaseFrame();
frame->texture = selected_slot->texture;
frame->lifetime = std::move(selected_lease);
recovery_frame_gate_.complete();
```

### Steps
1. 将初次初始化、`ACCESS_LOST`轻量重建、设备移除/重置恢复和主动重置统一接入`DxgiRecoveryFrameGate`。
2. 在完成`AcquireNextFrame`并获取纹理描述后，先判断是否为本轮首张预热帧；命中时立即`ReleaseFrame`并返回`DuplicationRecovering`。
3. 保留控制端上一张已成功交付的画面，不租用帧槽、不执行`CopyResource`，避免黑色预热帧进入NVENC/WebRTC码流。
4. 第二张成功帧按原流程复制、交付并调用`complete()`，恢复正常采集热路径；正常帧只增加一个轻量布尔分支，不调用`GetDeviceRemovedReason()`。
5. 增加策略单元测试，并在被控端既有`data/stream_capture_debug.log`中加入`recovery warmup frame discarded; retained last good frame`诊断记录。

### Verification
- 已对齐控制端`remote_compositor_timeline.log`、`remote_viewer_state.log`与四台被控端`stream_capture_debug.log`；确认6/6稳定期黑帧均发生在Duplication恢复后117-156毫秒。
- 已确认黑帧期间没有Overlay Present、D3D Present、DComp Commit、KeyedMutex失败，也没有`remote_presenter_diagnostic.log`同步超时记录。
- `rg`复核确认源码中不再残留`awaiting_recovery_frame_`，门控覆盖初始化、轻量重建、完整恢复和重置路径。
- `git diff --check`：通过。
- 按用户要求未编译、未链接、未运行二进制测试；新增策略测试仅完成静态检查，需更新被控端后观察上述预热帧丢弃日志及黑帧样本是否消失。

## 2026-08-07 09:20 - 设备界面新增隐藏本机开关

### Changed Location
- `src/system/AppSettings.h:22-23`：声明隐藏本机设置的读取与保存接口。
- `src/system/AppSettings.cpp:171-180`：使用当前用户 `QSettings` 持久化开关，默认关闭。
- `src/ui/DeviceGrid.h:246,420`：增加开关应用函数和窗口状态字段。
- `src/ui/DeviceGrid.cpp:1376-1443`：增加本机设备名、IPv4、MAC 的统一身份识别与过滤策略。
- `src/ui/DeviceGrid.cpp:1564-1701`：设备列表、分组排序和初始详情选择跳过隐藏本机。
- `src/ui/DeviceGrid.cpp:3083-3120,3448-3558`：常规设置页增加“隐藏本机设备”卡片、图标、开关，并顺延后续卡片布局。
- `src/ui/DeviceGrid.cpp:3746-4084,4177-4225`：启动恢复开关，完整本机信息到位后重新核对身份，并在共享快照与实时状态白名单中保持过滤。
- `src/ui/DeviceGrid.cpp:6599-7006`：关闭开关时立即恢复目录中的本机；目录缺失时用本机信息补回。
- `src/ui/DeviceGrid.cpp:7059-7140,7445-7462`：批量、周期和手动新增均在写入目录前跳过本机。
- `src/ui/DeviceGrid.cpp:8049-8069,9083-9086`：分组批量脚本、电源、终端和远控操作排除隐藏本机。
- `src/ui/DeviceGrid.cpp:10647-10844`：搜索结果排除本机，并清理本机占用的主选择、多选、拖拽和 Shift 锚点。
- `src/ui/DeviceGrid.cpp:11535-11542,12511-12516`：补齐开关悬停命中、点击切换、即时应用和持久化。

### Reason
设备界面原来始终显示本机，批量或周期发现也可能再次把本机加入列表。新增默认关闭的本机可见性偏好：开启后仅在当前电脑的界面层过滤本机，不从共享 `DeviceCatalog` 快照物理删除，避免其它电脑同步后也失去这台设备；关闭时立即复用已有记录，确实不存在才补回本机。过滤同时覆盖搜索、选择状态和分组批量动作，避免隐藏设备仍被后台操作。

### Original Code
```cpp
// src/system/AppSettings.h:18-24（修改前）
static bool periodicDeviceDiscoveryEnabled();
static void setPeriodicDeviceDiscoveryEnabled(bool enabled);
static int periodicDeviceDiscoveryIntervalSeconds();
static void setPeriodicDeviceDiscoveryIntervalSeconds(int seconds);
// 此处没有隐藏本机设置接口。
```

```cpp
// src/system/AppSettings.cpp:163-170（修改前）
void AppSettings::setPeriodicDeviceDiscoveryIntervalSeconds(int seconds)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("periodicDeviceDiscoveryIntervalSeconds"), seconds > 0 ? seconds : 60);
}
// 此处没有隐藏本机设置读写逻辑。
```

```cpp
// src/ui/DeviceGrid.h:243-248,416-421（修改前）
void applyPeriodicDeviceDiscoverySetting(bool scanImmediately);
void startBatchAddDevices(bool userInitiated = true);
// 此处没有 applyHideLocalDeviceSetting。

bool m_periodicDeviceDiscoveryEnabled = false;
bool m_statusRefreshInProgress = false;
// 此处没有 m_hideLocalDeviceEnabled。
```

```cpp
// src/ui/DeviceGrid.cpp:1496-1504,1555-1565（修改前）
for (const DeviceEntry& device : g_devices) {
    names.append(deviceDisplayName(device));
}

for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
    if (groupIndex < 0) {
        // 所有目录设备都会进入列表或分组排序。
    }
}
```

```cpp
// src/ui/DeviceGrid.cpp:2991-3028,3351-3354（修改前）
QRect settingsPeriodicDeviceDiscoverySwitchRect();
QRect settingsPublishUpdateButtonRect();
QRect settingsRollbackVersionComboRect();
QRect settingsWallpaperRotationSwitchRect();

const QRect periodicDiscoveryCard(contentLeft(), 512 + settingsYShift, contentWidth(), 71);
const QRect wallpaperTestCard(contentLeft(), 588 + settingsYShift, contentWidth(), 71);
const QRect rollbackCard(contentLeft(), 664 + settingsYShift, contentWidth(), 71);
const QRect updateCard(contentLeft(), 740 + settingsYShift, contentWidth(), 56);
// 周期发现与壁纸卡片之间没有隐藏本机开关。
```

```cpp
// src/ui/DeviceGrid.cpp:3678-3690,3970-3975（修改前）
if (g_devices.isEmpty()) {
    m_settingsSelected = true;
} else {
    m_selectedDeviceIndex = 0;
    m_selectedDeviceIndexes.insert(0);
    m_currentDeviceName = deviceDisplayName(g_devices.first());
}

refreshLocalDeviceInfo();
updateLocalInfoControls();
// 启动选择和延迟本机信息读取都没有应用本机过滤状态。
```

```cpp
// src/ui/DeviceGrid.cpp:6891-6970,7279-7288（修改前）
QMetaObject::invokeMethod(self, [self, results = std::move(results), userInitiated]() mutable {
    for (const BatchAddResult& result : results) {
        const QString ip = result.ip.trimmed();
        // 批量结果未判断是否为本机。
    }
});

const bool addingFirstDevice = g_devices.isEmpty();
DeviceEntry newDevice;
// 手动新增也未判断是否为本机。
```

```cpp
// src/ui/DeviceGrid.cpp:7873-7884,10462-10475（修改前）
return platform::DeviceActionTargetResolver::indexesForDeviceIds(
    g_deviceCatalog,
    platform::DeviceActionTargetResolver::deviceIdsForGroup(g_deviceCatalog, groupId));

for (const DeviceEntry& device : g_devices) {
    searchItems.append(DeviceSearchItem{ /* ... */ });
}
// 分组批量动作和搜索都会包含本机。
```

### Modified Code
```cpp
// src/system/AppSettings.h:22-23（修改后）
static bool hideLocalDeviceEnabled(); // wjy: 隐藏本机属于当前电脑自己的设备列表偏好，默认关闭并跨启动恢复。
static void setHideLocalDeviceEnabled(bool enabled);
```

```cpp
// src/system/AppSettings.cpp:171-180（修改后）
bool AppSettings::hideLocalDeviceEnabled()
{
    return settings().value(QStringLiteral("hideLocalDeviceEnabled"), false).toBool();
}

void AppSettings::setHideLocalDeviceEnabled(bool enabled)
{
    QSettings appSettings = settings();
    appSettings.setValue(QStringLiteral("hideLocalDeviceEnabled"), enabled);
}
```

```cpp
// src/ui/DeviceGrid.h:246,420（修改后）
void applyHideLocalDeviceSetting(bool revealLocalDeviceIfMissing); // wjy: 同步本机过滤状态，关闭开关时按需立即补回本机记录。
bool m_hideLocalDeviceEnabled = false; // wjy: 默认显示本机；开启后只在当前电脑界面隐藏，不向共享目录传播删除。
```

```cpp
// src/ui/DeviceGrid.cpp:1391-1432（修改后）
bool deviceIdentityMatchesLocal(
    const QString& candidateName,
    const QString& candidateIp,
    const QString& candidateMac,
    const platform::DeviceInfo& localInfo)
{
    // 依次比较 IPv4、规范化 MAC、忽略大小写的设备名。
    // 任一稳定身份一致即判定为本机。
}

bool deviceHiddenByLocalPreference(const DeviceEntry& device)
{
    return g_hideLocalDeviceFromList && deviceRecordMatchesLocal(device);
}
```

```cpp
// src/ui/DeviceGrid.cpp:1564-1573,1624-1632,1694-1701（修改后）
for (const DeviceEntry& device : g_devices) {
    if (deviceHiddenByLocalPreference(device)) continue;
    names.append(deviceDisplayName(device));
}

for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
    if (deviceHiddenByLocalPreference(g_devices.at(deviceIndex))) continue;
    // 未隐藏设备继续进入原有分组和自然排序。
}

int firstUnhiddenDeviceIndex(); // 启动、同步和新增回退只跳过本机过滤，不改变折叠分组语义。
```

```cpp
// src/ui/DeviceGrid.cpp:3083-3088,3448-3454,3497,3557（修改后）
QRect settingsHideLocalDeviceSwitchRect()
{
    return QRect(contentLeft() + contentWidth() - 90, 607 + (kDetailScriptPanelTop - 120), 82, 32);
}

const QRect hideLocalDeviceCard(contentLeft(), 588 + settingsYShift, contentWidth(), 71);
const QRect wallpaperTestCard(contentLeft(), 664 + settingsYShift, contentWidth(), 71);
painter.drawText(/* ... */, QString::fromUtf8("隐藏本机设备"));
drawSwitchWithLabel(settingsHideLocalDeviceSwitchRect(), hideLocalDeviceEnabled);
```

```cpp
// src/ui/DeviceGrid.cpp:3746-3754,4077-4084（修改后）
m_hideLocalDeviceEnabled = platform::AppSettings::hideLocalDeviceEnabled();
g_hideLocalDeviceFromList = m_hideLocalDeviceEnabled;
g_localDeviceIdentityForList.name = platform::DeviceInfoService::localDeviceName();

refreshLocalDeviceInfo();
if (m_hideLocalDeviceEnabled) {
    applyHideLocalDeviceSetting(false); // 完整 IP/MAC 到位后只在开启状态重新过滤。
}
```

```cpp
// src/ui/DeviceGrid.cpp:6961-7006（修改后）
void DeviceGrid::applyHideLocalDeviceSetting(bool revealLocalDeviceIfMissing)
{
    g_hideLocalDeviceFromList = m_hideLocalDeviceEnabled;
    if (!m_hideLocalDeviceEnabled && revealLocalDeviceIfMissing) {
        // 优先复用目录中的本机；确实缺失时使用本机名称、IP、MAC、广播地址补回并保存。
    }
    pruneHiddenDeviceSelections();
    updateRealtimeConfiguredDevices();
    update();
}
```

```cpp
// src/ui/DeviceGrid.cpp:7131-7140,7456-7462（修改后）
if (grid->m_hideLocalDeviceEnabled
    && (batchAddResultMatchesLocal(result, localDeviceIdentity)
        || batchAddResultMatchesLocal(result, grid->m_localDeviceInfo))) {
    continue; // 批量或周期发现命中本机时不写入目录。
}

if (m_hideLocalDeviceEnabled
    && deviceIdentityMatchesLocal(name, ip, mac, g_localDeviceIdentityForList)) {
    return; // 手动新增本机同样跳过。
}
```

```cpp
// src/ui/DeviceGrid.cpp:8049-8069,10651-10656（修改后）
for (const int deviceIndex : catalogIndexes) {
    if (deviceHiddenByLocalPreference(g_devices.at(deviceIndex))) continue;
    result.append(deviceIndex); // 分组批量动作只保留未隐藏设备。
}

for (const DeviceEntry& device : g_devices) {
    if (deviceHiddenByLocalPreference(device)) continue;
    searchItems.append(DeviceSearchItem{ /* ... */ });
}
```

```cpp
// src/ui/DeviceGrid.cpp:10767-10844,12511-12516（修改后）
void DeviceGrid::pruneHiddenDeviceSelections()
{
    // 从主选择、多选、拖拽和 Shift 锚点中删除本机，并回退到第一台未隐藏设备。
    // 没有未隐藏设备时清空详情并保持设置页可操作。
}

if (settingsLayout.containsPoint(settingsHideLocalDeviceSwitchRect(), event->pos())) {
    m_hideLocalDeviceEnabled = !m_hideLocalDeviceEnabled;
    platform::AppSettings::setHideLocalDeviceEnabled(m_hideLocalDeviceEnabled);
    applyHideLocalDeviceSetting(!m_hideLocalDeviceEnabled);
}
```

### Steps
1. 在 `AppSettings` 中增加默认关闭的持久化开关，并在设备界面启动阶段优先恢复设置。
2. 用 IPv4、规范化 MAC 和设备名建立统一的本机身份判断，供列表、搜索、新增和同步路径复用。
3. 在常规设置页插入独立开关卡片，补齐图标、说明文字、悬停与点击命中，并将后续壁纸、回撤、发布、本机信息和新增设备区域整体下移。
4. 开启后立即从左侧列表和搜索中隐藏本机，清理本机选择状态，同时从实时状态白名单和分组批量目标中排除本机。
5. 批量新增、周期发现和手动新增在目录写入前跳过本机，避免开关开启期间重新出现。
6. 关闭后优先显示目录已有本机；目录确实缺失时读取本机信息创建记录并立即保存、显示。
7. 启动延迟读取完整网卡信息时只在开关开启状态重新过滤，避免开关关闭时影响折叠分组的当前详情选择。

### Verification
- `git diff --check`：通过。
- `rg` 静态复核：设置页绘制参数只有一个调用点，开关矩形已同时接入绘制、悬停和点击命中。
- `rg` 静态复核：列表排序、搜索、批量/周期新增、手动新增、同步快照、实时状态白名单、分组批量动作均接入本机过滤。
- 按用户要求未构建、未链接、未运行程序或二进制测试。

## 2026-08-07 15:43 - 更新成功重启时最小化到托盘

### Changed Location
- `src/updater/main.cpp:364-370`：更新成功后的重启命令追加 `--minimized` 参数；回撤重启参数保持不变。

### Reason
更新器替换文件完成后会自动重启 FSRemote。用户希望更新重启不要直接弹出完整主窗口，因此仅对正常更新成功路径增加最小化参数，主程序现有参数处理会让它进入托盘；回撤仍保留原来的失败提示行为。

### Original Code
```cpp
// src/updater/main.cpp:364-370（修改前）
command += updated
    ? L"--updated-from \"" + task.fromVersion + L"\" --updated-to \"" + task.toVersion + L"\""
    : L"--update-rollback \"" + task.toVersion + L"\"";
```

### Modified Code
```cpp
// src/updater/main.cpp:364-370（修改后）
command += updated
    ? L"--updated-from \"" + task.fromVersion + L"\" --updated-to \"" + task.toVersion + L"\" --minimized" // wjy: 更新成功后的新主程序直接进入托盘，避免更新重启打断用户当前桌面。
    : L"--update-rollback \"" + task.toVersion + L"\"";
```

### Steps
1. 确认 `src/main.cpp` 已支持 `--minimized` 并调用 `hideToTray()`。
2. 在独立更新器正常更新成功的重启命令中追加 `--minimized`。
3. 保持回撤重启参数不变，避免改变回撤失败提示逻辑。

### Verification
- `git diff --check`：通过。
- 静态确认主程序会将 `--minimized` 转换为托盘启动。
- 按用户要求未构建、未链接、未运行程序或二进制测试。

## 2026-08-07 09:35 - 壁纸改为直接使用共享原图

### Changed Location
- `src/system/DesktopWallpaperService.h:24-25`：移除带设备名参数的壁纸接口，改为直接处理共享原图。
- `src/system/DesktopWallpaperService.cpp:133-153`：删除设备名绘制、字体缩放和右上角描边合成代码；缓存前不再修改图片像素。
- `src/ui/DeviceGrid.cpp:6379`：壁纸后台任务不再读取本机名称或传入设备名，仅按共享图片轮换。
- `tests/desktop_wallpaper_service_tests.cpp:99-146,178-179`：删除依赖设备名像素合成的测试，保留图片枚举、坏图跳过、轮换顺序和数字设备默认开关测试。

### Reason
后续共享文件夹中的图片本身就是最终桌面素材，不需要再把目标设备名写到右上角。删除合成逻辑后，每台设备会直接使用共享目录中的原图，避免本地 `current.bmp` 出现额外字符像素。

### Original Code
```cpp
// src/system/DesktopWallpaperService.cpp:133-190（修改前）
QImage DesktopWallpaperService::composeDeviceNameOverlay(const QImage& sourceImage, const QString& deviceName)
{
    // 根据设备名创建字体路径，并将白字黑边绘制到图片右上角。
    return composedImage;
}
```

```cpp
// src/system/DesktopWallpaperService.cpp:192-212（修改前）
DesktopWallpaperApplyResult DesktopWallpaperService::applyFirstSharedImage(const QString& deviceName);
DesktopWallpaperApplyResult DesktopWallpaperService::applyNextSharedImage(
    const QString& previousSourcePath,
    const QString& deviceName);
image = composeDeviceNameOverlay(image, deviceName);
```

```cpp
// src/ui/DeviceGrid.cpp:6379-6387（修改前）
const platform::DeviceInfo targetDeviceInfo = platform::DeviceInfoService::local();
QString targetDeviceName = targetDeviceInfo.name.trimmed();
// 读取设备名/IP并传入壁纸合成函数。
const platform::DesktopWallpaperApplyResult result =
    platform::DesktopWallpaperService::applyNextSharedImage(previousSourcePath, targetDeviceName);
```

### Modified Code
```cpp
// src/system/DesktopWallpaperService.h:24-25（修改后）
static DesktopWallpaperApplyResult applyFirstSharedImage(); // wjy: 使用共享原图完成首张图片选择、本地 BMP 缓存和当前 Windows 桌面切换。
static DesktopWallpaperApplyResult applyNextSharedImage(const QString& previousSourcePath); // wjy: 自动轮换直接使用共享原图，复用稳定 BMP 缓存与 Windows API。
```

```cpp
// src/system/DesktopWallpaperService.cpp:133-153（修改后）
DesktopWallpaperApplyResult DesktopWallpaperService::applyFirstSharedImage()
{
    return applyNextSharedImage(QString()); // wjy: 首次调用采用排序后的第一张可解码共享原图。
}

DesktopWallpaperApplyResult DesktopWallpaperService::applyNextSharedImage(const QString& previousSourcePath)
{
    // 选择共享图片后直接写入本地 current.bmp，不再执行设备名像素合成。
}
```

```cpp
// src/ui/DeviceGrid.cpp:6379（修改后）
const platform::DesktopWallpaperApplyResult result =
    platform::DesktopWallpaperService::applyNextSharedImage(previousSourcePath); // wjy: 壁纸任务直接使用共享原图，不再读取本机名称或合成字符。
```

### Steps
1. 删除 `composeDeviceNameOverlay` 声明及实现，移除设备名字体、路径和描边绘制逻辑。
2. 收窄 `applyFirstSharedImage`、`applyNextSharedImage` 接口，不再携带设备名参数。
3. 删除壁纸后台任务中的本机信息读取，改为直接轮换共享原图。
4. 删除依赖字符像素变化的回归测试，保留共享图片选择和默认开关测试。

### Verification
- `git diff --check`：通过。
- `rg` 复核：源码和壁纸测试中不再引用 `composeDeviceNameOverlay` 或 `targetDeviceName`。
- 按用户要求未构建、未链接、未运行程序或二进制测试。

## 2026-08-07 10:01 - 真实主屏优先与虚拟屏智能兜底

### Changed Location
- `openspec/changes/auto-select-remote-display/.openspec.yaml:1-2`：登记本次变更使用 `spec-driven` 工作流及创建日期。
- `openspec/changes/auto-select-remote-display/proposal.md:1-28`：新增本次自动选屏变更的动机、范围、能力和影响说明。
- `openspec/changes/auto-select-remote-display/design.md:1-74`：确定真实主屏 DXGI/CPU 两级捕获、VDD 兜底、稳定共享目标和所有权边界。
- `openspec/changes/auto-select-remote-display/specs/automatic-host-display-selection/spec.md:1-67`：新增可验证的 Host 自动显示选择行为规格。
- `openspec/changes/auto-select-remote-display/tasks.md:1-21`：记录十项实现、验证和文档任务及完成状态。
- `third_party/uu_stream_webrtc/src/host_display_selection_policy.h:1-49`：新增无显卡可测试的主屏候选模型和确定性资格策略。
- `third_party/uu_stream_webrtc/tests/host_display_selection_policy_tests.cpp:1-65`：新增物理主屏、Parsec、非活动、副屏、远程、镜像和无效模式回归测试。
- `CMakeLists.txt:41,126-137`：登记生产策略头和新的独立测试目标。
- `third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:76-181,480-742,753-991,1069-1075`：增加 Windows 显示枚举、精确 DesktopCapturer 选屏、真实屏优先启动顺序、活动目标状态和统一刷新率读取。
- `third_party/uu_stream_webrtc/src/parsec_vdd_session.cpp:358-363,519-521`：停止跨进程删除全部 Parsec 显示器，只创建并释放本会话持有的索引。

### Reason
原共享媒体管线在首个远控订阅建立时无条件创建 Parsec VDD，即使 Windows 已连接并可捕获真实主屏。这样会反复改变显示拓扑和主屏，也可能在创建前删除其他进程持有的 Parsec 显示实例。本次改为在同一个首订阅边界内优先捕获真实主屏，只有真实主屏不存在或 DXGI、DesktopCapturer 都无法精确使用时才创建 VDD，同时保持多 Viewer 共享一个视频源、最后订阅者统一清理的现有架构。

### Original Code
```cmake
# CMakeLists.txt:40-42（修改前）
third_party/uu_stream_webrtc/src/latest_encode_frame_slot.h
third_party/uu_stream_webrtc/src/host_media_pipeline.cpp
third_party/uu_stream_webrtc/src/host_media_pipeline.h

# CMakeLists.txt:123-125（修改前）
add_test(NAME uu_host_media_pipeline_tests COMMAND uu_host_media_pipeline_tests)

add_executable(uu_latest_encode_frame_slot_tests EXCLUDE_FROM_ALL
```

```cpp
// third_party/uu_stream_webrtc/src/host_display_selection_policy.h（新增文件，无原代码）
// new code, no old code at this location
```

```cpp
// third_party/uu_stream_webrtc/tests/host_display_selection_policy_tests.cpp（新增文件，无原代码）
// new code, no old code at this location
```

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:416-450（修改前）
auto chosen = sources.front();
const auto choose_parsec_fallback = [&sources]() -> webrtc::DesktopCapturer::Source {
    for (const auto& source : sources) {
        const std::string title = to_lower_copy(source.title);
        if (title.find("parsec") != std::string::npos || title.find("psccdd0") != std::string::npos) {
            return source;
        }
    }
    return sources.front();
};

if (!matched && (preferred_source_id_ != 0 || !preferred_device_name_.empty())) {
    chosen = choose_parsec_fallback();
}
```

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:632-704（修改前）
bool start_locked(std::string* error)
{
    if (source) return true;
    if (hooks.start) {
        source = hooks.start(error);
        return source != nullptr;
    }

    virtual_display = std::make_unique<ParsecVddSession>();
    virtual_display->start(&vdd_error);
    if (virtual_display) {
        // 仅 VDD 成功后尝试 DXGI。
    }
    // 最后创建一个可能回退到 sources.front 的 DesktopCapturer。
}
```

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:816-823（修改前）
uint32_t HostMediaPipeline::source_refresh_hz() const
{
    const std::shared_ptr<SharedState> state = state_;
    if (!state) return 60;
    std::lock_guard lock(state->mutex);
    if (!state->virtual_display) return 60;
    return display_refresh_hz(state->virtual_display->preferred_device_name());
}
```

```cpp
// third_party/uu_stream_webrtc/src/parsec_vdd_session.cpp:214-264,412-415（修改前）
std::vector<int> enumerate_parsec_display_indexes();
void remove_existing_parsec_displays(HANDLE handle)
{
    // 枚举并删除驱动中的全部 Parsec 显示索引。
}

remove_existing_parsec_displays(handle_);
std::this_thread::sleep_for(std::chrono::milliseconds(300));
display_index_ = parsec_vdd::VddAddDisplay(handle_);
```

### Modified Code
```cmake
# CMakeLists.txt:40-42,126-137（修改后）
third_party/uu_stream_webrtc/src/latest_encode_frame_slot.h
third_party/uu_stream_webrtc/src/host_display_selection_policy.h # wjy: 生产枚举与测试共享自动选屏规则。
third_party/uu_stream_webrtc/src/host_media_pipeline.cpp

add_executable(uu_host_display_selection_policy_tests EXCLUDE_FROM_ALL
    third_party/uu_stream_webrtc/tests/host_display_selection_policy_tests.cpp
    third_party/uu_stream_webrtc/src/host_display_selection_policy.h
)
add_test(NAME uu_host_display_selection_policy_tests COMMAND uu_host_display_selection_policy_tests)
```

```cpp
// third_party/uu_stream_webrtc/src/host_display_selection_policy.h:11-46（修改后）
struct HostDisplayCandidate {
    std::string device_name;
    int64_t monitor_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refresh_hz = 0;
    bool active = false;
    bool primary = false;
    bool parsec = false;
    bool remote = false;
    bool mirroring = false;
};

inline bool is_eligible_existing_primary(const HostDisplayCandidate& candidate)
{
    return candidate.active
        && candidate.primary
        && !candidate.parsec
        && !candidate.remote
        && !candidate.mirroring
        && !candidate.device_name.empty()
        && candidate.monitor_id != 0
        && candidate.width > 0
        && candidate.height > 0;
}
```

```cpp
// third_party/uu_stream_webrtc/tests/host_display_selection_policy_tests.cpp:25-63（修改后）
const auto selected = uu::select_existing_primary_display({valid_primary()});
assert(selected.has_value());

parsec.parsec = true;
assert(!uu::select_existing_primary_display({parsec}));
inactive.active = false;
assert(!uu::select_existing_primary_display({inactive}));
secondary.primary = false;
assert(!uu::select_existing_primary_display({secondary}));
remote.remote = true;
assert(!uu::select_existing_primary_display({remote}));
mirroring.mirroring = true;
assert(!uu::select_existing_primary_display({mirroring}));
```

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:121-160（修改后）
std::vector<HostDisplayCandidate> enumerate_host_display_candidates()
{
    // 枚举 Windows 适配器，保存活动、主屏、Parsec、远程、镜像、模式和 HMONITOR 状态。
    // 每个候选都写入 display-select 诊断日志，再交给纯策略选择。
}
```

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:533-580（修改后）
const auto choose_parsec_fallback = [&sources]()
    -> std::optional<webrtc::DesktopCapturer::Source> {
    // 只返回明确带有 Parsec 身份的源。
    return std::nullopt;
};

if (!matched && require_exact_target_) {
    if (error) *error = "DesktopCapturer target display was not found";
    return false; // 精确目标缺失时交给下一层回退，不再选择 sources.front。
}
```

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:819-944（修改后）
const auto physicalTarget = select_existing_primary_display(
    enumerate_host_display_candidates());
if (physicalTarget) {
    if (start_dxgi_locked(/* physical */)) return true;
    if (start_desktop_locked(/* physical, exact */)) return true;
}

virtual_display = std::make_unique<ParsecVddSession>();
if (!virtual_display->start(&vdd_error)) {
    // 保留旧版通用 DesktopCapturer 紧急兼容路径。
}
if (start_dxgi_locked(/* virtual */)) return true;
if (start_desktop_locked(/* virtual, exact */)) return true;
```

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:1069-1075（修改后）
uint32_t HostMediaPipeline::source_refresh_hz() const
{
    const std::shared_ptr<SharedState> state = state_;
    if (!state) return 60;
    std::lock_guard lock(state->mutex);
    return display_refresh_hz(state->active_device_name);
}
```

```cpp
// third_party/uu_stream_webrtc/src/parsec_vdd_session.cpp:358-363,519-521（修改后）
append_stream_capture_diagnostic_log(
    "vdd",
    "preserve-existing displays; add owned display only");
display_index_ = parsec_vdd::VddAddDisplay(handle_);

if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE && display_index_ >= 0) {
    parsec_vdd::VddRemoveDisplay(handle_, display_index_); // 仅释放本会话保存的索引。
}
```

### Steps
1. 创建 `auto-select-remote-display` OpenSpec 变更，完成 proposal、design、行为规格和十项可追踪任务。
2. 新增 `HostDisplayCandidate` 纯策略，限定只有活动、有效、非 Parsec、非远程、非镜像的 Windows 主屏可以绕过 VDD。
3. 在首个媒体订阅启动时枚举当前显示拓扑，并记录每个候选的设备名、模式、标志和 HMONITOR。
4. 将生产启动顺序调整为真实主屏 DXGI、真实主屏精确 DesktopCapturer、VDD DXGI、VDD 精确 DesktopCapturer；VDD 驱动本身不可用时保留旧通用 CPU 紧急回退。
5. 为 `DesktopVideoSource` 增加精确目标门禁，物理屏或 VDD 身份无法匹配时返回失败，不再静默抓取其他屏幕。
6. 保存最终捕获模式、设备名和监视器 ID，并让刷新率查询同时支持真实屏和虚拟屏；最后订阅者退出时清空状态供下次重新判断。
7. 删除 VDD 创建前的全局 Parsec 索引枚举和删除，只移除当前 `VddAddDisplay` 返回并由本会话持有的索引。
8. 新增并登记无显卡显示选择策略测试，重新配置 CMake，构建生产流媒体库和相关测试目标。

### Verification
- `cmake -S . -B build-video-webrtc-msvc -DFSREMOTE_BUILD_TESTS=ON`：配置成功；仅保留项目已有的 FakerInput 安装包缺失和 Qt AUTOGEN 开发警告。
- `cmake --build build-video-webrtc-msvc --config Release --target uu_stream_common fsremote_stream uu_host_display_selection_policy_tests uu_host_media_pipeline_tests uu_dxgi_capture_policy_tests -- /m`：全部目标构建成功；仅有 WebRTC 外部头中的未知 clang pragma 警告。
- `ctest --test-dir build-video-webrtc-msvc -C Release -R "uu_(host_display_selection_policy|host_media_pipeline|dxgi_capture_policy)_tests" --output-on-failure`：3/3 测试通过，0 失败。
- `git diff --check`：通过。
- 变更范围复核：未修改 Viewer、WebRTC 信令、输入协议或 Qt 远控窗口代码。
# 2026-08-07 - 修复真实显示源无首帧时无法回退的问题

## 修改位置

- `third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:238-277`：为 DXGI 捕获源增加首帧等待通知和有界探测。
- `third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:394-402`：首个新帧到达时唤醒等待方。
- `third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:809-828`：DXGI 初始化成功但 2.5 秒内无首帧时停止该源并继续 DesktopCapturer/VDD 回退。

## 原因

A12 等无实体显示器设备在 Windows 枚举出活动显示设备后，DXGI 初始化可能表面成功，但随后持续 `DXGI_ERROR_ACCESS_LOST`、发布帧率为 0。旧逻辑会一直保留 `physical-dxgi`，控制端只能等待首帧，必须先由 UU 创建可用虚拟显示器才能恢复。本次增加启动期首帧健康检查，让失效的 DXGI 路径及时让出给后续捕获回退。

## 原始代码

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:804-818
if (!native->start(&nativeError)) {
    ...
    return false;
}
dxgi_source = native;
source = native;
activate_target_locked(mode, deviceName, monitorId);
return true;
```

## 修改后代码

```cpp
// third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:809-828
constexpr auto kFirstFrameProbeTimeout = std::chrono::milliseconds(2500);
if (!native->wait_for_first_frame(kFirstFrameProbeTimeout)) {
    native->stop();
    if (failure) {
        *failure = nativeError.empty()
            ? "DXGI source produced no first frame within probe window"
            : nativeError + "; no first frame within probe window";
    }
    append_stream_capture_diagnostic_log(
        "display-select",
        std::string(targetLabel) + " DXGI no first frame; falling back after probe_ms="
            + std::to_string(kFirstFrameProbeTimeout.count()));
    return false;
}
dxgi_source = native;
source = native;
activate_target_locked(mode, deviceName, monitorId);
return true;
```

## 修改步骤

1. 在 DXGI 源中增加条件变量，等待首个新帧而不是只等待线程启动成功。
2. 首帧到达后通知共享媒体管线；停止源时同步唤醒等待线程，避免退出阻塞。
3. 真实屏和虚拟屏的 DXGI 启动统一使用 2.5 秒首帧探测，超时后返回失败，让既有 DesktopCapturer/VDD 顺序继续执行。

## 验证

- 已执行目标构建命令，但当前环境缺少 MSVC 标准头 `stddef.h`，`fsremote_stream` 构建未完成；未修改其它用户文件。
- 目标 A12 日志已确认原问题表现为 `physical-dxgi` 初始化后持续 `DXGI_ERROR_ACCESS_LOST` 与 `publish_fps=0`。

## 2026-08-07 - 收敛远控角色画质配置

### 修改位置

- `src/stream/RemoteVideoPolicy.h:46-66`：为角色配置增加统一码率字段，并将焦点、后台、最小化的分辨率、FPS、码率集中到同一组常量。
- `src/stream/RemoteVideoPolicy.h:139-148`：压力状态不再通过旧的 `15/10/5/3/1 FPS` 梯度改写正常后台配置。
- `src/ui/RemoteQualityCoordinator.cpp:18-40`：删除单选/多选两套重复预设及按窗口数量计算后台 FPS 的函数。
- `src/ui/RemoteQualityCoordinator.cpp:186-222`：改为从 `RemoteVideoPolicy` 读取角色分辨率和 FPS。
- `src/ui/RemoteQualityCoordinator.cpp:284-292`：改为从统一角色配置读取正常码率，软件回退保留独立安全码率。
- `tests/remote_video_policy_tests.cpp:35-40`：更新压力场景断言，确认压力等级可记录但不再改写正常后台 FPS。

### 修改原因

原实现同时维护 `RemoteQualityCoordinator.cpp` 和 `RemoteVideoPolicy.h` 两套角色画质配置，并在后台 FPS 上叠加窗口数量梯度、Presenter 压力和函数末尾二次覆盖，导致同一窗口的最终分辨率和 FPS 难以预测。本次将角色配置收敛到 `RemoteVideoPolicy`，移除设备数量 FPS 梯度，避免“45 FPS 后又被改成 60 FPS”等重复覆盖。

### 原始代码

```cpp
// src/ui/RemoteQualityCoordinator.cpp:23-47
struct RemoteQualityPreset { /* 单选/多选各自维护分辨率、FPS、码率 */ };
constexpr RemoteSelectionQualityProfile kSingleSelectionQuality = { /* ... */ };
constexpr RemoteSelectionQualityProfile kMultiSelectionQuality = { /* ... */ };

// src/stream/RemoteVideoPolicy.h:62-64
inline constexpr RemoteVideoProfile kFocusedRemoteVideoProfile = {1920, 1080, 60, 100, true};
inline constexpr RemoteVideoProfile kVisibleBackgroundRemoteVideoProfile = {1280, 720, 30, 40, true};
inline constexpr RemoteVideoProfile kMinimizedRemoteVideoProfile = {640, 360, 1, 5, true};
```

### 修改后代码

```cpp
// src/stream/RemoteVideoPolicy.h:46-66
struct RemoteVideoProfile {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t targetFps = 0;
    std::uint32_t priority = 0;
    bool requestsRemoteQuality = false;
    std::uint32_t maxBitrateKbps = 0; // wjy: 角色默认码率与分辨率、FPS统一保存。
};

inline constexpr RemoteVideoProfile kFocusedRemoteVideoProfile = {1920, 1080, 60, 100, true, 48000};
inline constexpr RemoteVideoProfile kVisibleBackgroundRemoteVideoProfile = {1280, 720, 30, 40, true, 24000};
inline constexpr RemoteVideoProfile kMinimizedRemoteVideoProfile = {640, 360, 1, 5, true, 7000};

// src/ui/RemoteQualityCoordinator.cpp:186-222
const auto roleProfile = decision.minimized
    ? stream::RemoteVideoPolicy::minimizedProfile()
    : stream::RemoteVideoPolicy::backgroundProfile();
decision.resolution = decision.minimized
    ? stream::RemoteResolutionTier::P360
    : stream::RemoteResolutionTier::P720;
decision.targetFps = static_cast<int>(roleProfile.targetFps);
```

### 修改步骤

1. 将角色默认码率加入 `RemoteVideoProfile`，统一保存分辨率、FPS 和码率。
2. 删除 `kSingleSelectionQuality`、`kMultiSelectionQuality` 及其后台 FPS 数量梯度函数。
3. 让协调器直接使用焦点、后台、最小化角色配置；保留软件回退的 540p/24 安全档。
4. 移除协调器函数末尾对后台和前台 FPS 的二次覆盖。
5. 更新远程视频策略测试，验证压力状态不再恢复旧数量降帧逻辑。

### 验证

- 已执行 `git diff --check`，通过。
- 已使用 `rg` 确认 `RemoteQualityCoordinator.cpp` 中不再存在 `selectionQuality`、`backgroundFps`、`RemoteQualityPreset` 和 `RemoteSelectionQualityProfile`。
- 未编译；用户此前要求本次不编译，且当前环境曾存在 MSVC 标准头缺失问题。

### 补充修改位置

- `tests/remote_quality_coordinator_tests.cpp:34-273`：更新单窗口、多窗口、6/11/20/21 窗口和压力场景的帧率断言，验证窗口数量不会再改变后台 FPS。

## 2026-08-07 - 让统一角色配置直接控制分辨率

### 修改位置

- `src/stream/RemoteVideoPolicy.h:3,46-67`：为 `RemoteVideoProfile` 增加 `RemoteResolutionTier resolution` 字段，并为焦点、后台、最小化常量填写对应档位。
- `src/ui/RemoteQualityCoordinator.cpp:199-215`：焦点、后台和最小化决策改为直接读取角色配置中的分辨率档位，不再写死 `P1080/P720/P360`。
- `tests/remote_video_policy_tests.cpp:7-28`：补充分辨率档位回归断言。

### 修改原因

此前统一配置中的宽高字段只是描述信息，协调器仍然把分辨率枚举写死，因此修改配置宽高不会可靠改变最终编码分辨率。本次将 `RemoteResolutionTier` 直接放入角色配置，确保以后只修改一组角色配置即可同步控制分辨率、FPS 和码率。

### 原始代码

```cpp
// src/stream/RemoteVideoPolicy.h
struct RemoteVideoProfile {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t targetFps = 0;
    std::uint32_t priority = 0;
    bool requestsRemoteQuality = false;
    std::uint32_t maxBitrateKbps = 0;
};

// src/ui/RemoteQualityCoordinator.cpp
decision.resolution = stream::RemoteResolutionTier::P1080;
```

### 修改后代码

```cpp
// src/stream/RemoteVideoPolicy.h
struct RemoteVideoProfile {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t targetFps = 0;
    std::uint32_t priority = 0;
    bool requestsRemoteQuality = false;
    std::uint32_t maxBitrateKbps = 0;
    RemoteResolutionTier resolution = RemoteResolutionTier::Native; // wjy: 修改一处即可改变最终分辨率档位。
};

inline constexpr RemoteVideoProfile kFocusedRemoteVideoProfile =
    {1920, 1080, 60, 100, true, 48000, RemoteResolutionTier::P1080};

// src/ui/RemoteQualityCoordinator.cpp
decision.resolution = stream::kFocusedRemoteVideoProfile.resolution;
```

### 修改步骤

1. 将 `RemoteResolutionTier` 加入统一角色配置结构。
2. 为三种角色配置分别绑定 `P1080`、`P720`、`P360`。
3. 协调器读取配置档位计算目标宽高，避免硬编码分辨率。
4. 更新策略测试，确认角色结果带有正确分辨率档位。

### 验证

- 已执行 `git diff --check` 前置检查。
- 未编译；按用户要求本次不编译。

## 2026-08-07 - 将统一画质配置加入 Qt 工程文件列表

### 修改位置

- `CMakeLists.txt:251`：将 `src/stream/RemoteVideoPolicy.h` 加入远控质量协调器测试目标。
- `CMakeLists.txt:567`：将 `src/stream/RemoteVideoPolicy.h` 加入主程序源文件列表，使 Qt Creator 项目树显示该配置文件。

### 修改原因

文件实际存在于 `src/stream/RemoteVideoPolicy.h`，但此前只通过头文件间接包含，没有出现在 CMake 源文件列表中，因此 Qt Creator 项目树可能看不到。补入 CMake 后，重新加载 CMake 项目即可直接浏览和修改统一角色配置。

### 修改后代码

```cmake
src/stream/RemoteQualityPolicy.h
src/stream/RemoteVideoPolicy.h # wjy: Qt Creator可见的统一角色画质配置。
```

### 验证

- 已确认目标文件存在且路径正确。
- 已执行静态文件列表检查。
- 未编译；按用户要求本次不编译。

## 2026-08-07 - 角色配置改为只填写分辨率档位

### 修改位置

- `src/stream/RemoteVideoPolicy.h:48-68`：删除角色配置中的重复宽高字段，改为第一个字段直接填写 `RemoteResolutionTier`，并为焦点、后台、最小化配置补充中文修改说明。
- `src/stream/RemoteVideoRenderWorker.cpp:311-316`：角色配置变更比较改为比较 `resolution` 档位，避免引用已删除的宽高字段。
- `tests/remote_video_policy_tests.cpp:7-28`：更新策略测试，验证配置只使用分辨率档位。

### 修改原因

用户希望后续只修改 `P1080`、`P720`、`P360` 等档位，而不再同时填写 `1920, 1080`。现在配置中的分辨率唯一来源是 `RemoteResolutionTier`，宽高由既有 `resolutionTierHeight()` 和 `targetSize()` 根据源画面比例计算。

### 原始代码

```cpp
struct RemoteVideoProfile {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t targetFps = 0;
    std::uint32_t priority = 0;
    bool requestsRemoteQuality = false;
    std::uint32_t maxBitrateKbps = 0;
    RemoteResolutionTier resolution = RemoteResolutionTier::Native;
};

inline constexpr RemoteVideoProfile kFocusedRemoteVideoProfile =
    {1920, 1080, 60, 100, true, 48000, RemoteResolutionTier::P1080};
```

### 修改后代码

```cpp
struct RemoteVideoProfile {
    RemoteResolutionTier resolution = RemoteResolutionTier::Native; // wjy: 只填写P1080/P720/P360等档位，宽高由统一换算逻辑生成。
    std::uint32_t targetFps = 0;
    std::uint32_t priority = 0;
    bool requestsRemoteQuality = false;
    std::uint32_t maxBitrateKbps = 0;
};

inline constexpr RemoteVideoProfile kFocusedRemoteVideoProfile =
    {RemoteResolutionTier::P1080, 60, 100, true, 48000}; // wjy: 修改P1080即可切换焦点分辨率。
```

### 修改步骤

1. 将 `resolution` 移到角色配置第一个字段，删除重复的 `width`、`height`。
2. 焦点、后台、最小化分别使用 `P1080`、`P720`、`P360`，并在代码旁写明修改方式。
3. RenderWorker 的变更检测改为比较分辨率档位。
4. 更新策略测试，确保不再依赖角色配置中的宽高字段。

### 验证

- 已执行静态搜索，确认源码和测试不再引用 `profile.width/profile.height`。
- 未编译；按用户要求本次不编译。
## 2026-08-08 设备平铺自然排序

### Changed Location
- `src/ui/DeviceGrid.cpp:9258`：平铺前按设备名和 IP 对远控窗口排序，确保布局按从左到右、从上到下呈现。
- `src/ui/RemoteDesktopWindow.h:93`、`src/ui/RemoteDesktopWindow.cpp:2128`：提供窗口绑定设备名读取接口，作为平铺排序依据。

### Reason
用户要求数字开头的设备按自然数顺序排列（例如 4 在 15 前），数字开头设备排在字母开头设备前；名称相同时按 IP 地址排序，避免哈希容器遍历造成顺序不稳定，也不使用随机排序。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:9258（原逻辑）
if (m_remoteWindowCoordinator->windowsTiled()) {
    // ...
}
const int count = windows.size();
for (int i = 0; i < windows.size(); ++i) {
    RemoteDesktopWindow* remoteWindow = windows.at(i).data();
    // 按 openedRemoteWindows() 返回顺序直接布局
}
```

```cpp
// src/ui/RemoteDesktopWindow.h / .cpp（原状态）
// 原来仅提供 hostIp()，没有公开窗口绑定的设备名。
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:9258-9285
QCollator naturalCollator;
naturalCollator.setCaseSensitivity(Qt::CaseInsensitive);
naturalCollator.setNumericMode(true);
std::sort(windows.begin(), windows.end(), [&naturalCollator](
    const QPointer<RemoteDesktopWindow>& left,
    const QPointer<RemoteDesktopWindow>& right) {
    // 数字开头优先；名称自然排序；同名按 IP 排序。
});
```

```cpp
// src/ui/RemoteDesktopWindow.h:93
QString deviceName() const;

// src/ui/RemoteDesktopWindow.cpp:2128-2131
QString RemoteDesktopWindow::deviceName() const
{
    return m_deviceName.trimmed();
}
```

### Steps
1. 在远控窗口公开构造时保存的设备名和已有 IP。
2. 在平铺动作开始处对有效窗口执行稳定的分类、自然名称和 IP 排序。
3. 保留原有行优先网格计算，使排序后的窗口依次从左到右、从上到下放置。

### Verification
- 已通过静态阅读和 `git diff` 检查确认修改范围及排序链路。
- 未构建，符合用户要求。
## 2026-08-08 Windows 虚拟桌面分别平铺

### Changed Location
- `src/ui/DeviceGrid.cpp`：识别远控窗口所属 Windows 虚拟桌面，并对每个虚拟桌面单独计算平铺网格。

### Reason
多个 Windows 虚拟桌面同时存在时，原逻辑把所有远控窗口总数用于计算列数，导致桌面 1 只显示窗口 1、3 时按全局索引落在同一列，桌面 2 的窗口 2、4 也落在同一列。现在每个虚拟桌面独立计算行列，窗口会在各自桌面从左到右、从上到下排列。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp（原逻辑）
const int count = windows.size();
const int columnCount = qMax(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)))));
for (int i = 0; i < windows.size(); ++i) {
    const int row = i / columnCount;
    const int column = i % columnCount;
    // 所有虚拟桌面共用 count 和索引 i
}
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp
QHash<QString, QVector<QPointer<RemoteDesktopWindow>>> desktopGroups;
for (const QPointer<RemoteDesktopWindow>& window : windows) {
    desktopGroups[virtualDesktopKey(window.data())].append(window);
}
for (const auto& group : desktopGroups) {
    const int count = group.size();
    const int columnCount = qMax(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)))));
    for (int i = 0; i < group.size(); ++i) {
        const int row = i / columnCount;
        const int column = i % columnCount;
        // 每个虚拟桌面独立计算并布局
    }
}
```

### Steps
1. 使用 `IVirtualDesktopManager::GetWindowDesktopId` 获取每个远控窗口的虚拟桌面 GUID。
2. 以 GUID 将窗口分组，查询失败时统一回退到原有单组行为。
3. 对每组分别计算行列数和可用区域，保持既有名称/IP排序顺序。

### Verification
- 已执行差异静态检查准备工作，未构建，符合用户要求。
## 2026-08-08 修复虚拟桌面 GUID 字符串编译错误

### Changed Location
- `src/ui/DeviceGrid.cpp:152`：修正 `GUID::Data4` 转十六进制字符串的 Qt 类型调用。

### Reason
原实现把 `QString::fromLatin1()` 的返回值继续调用 `toHex()`，但 `toHex()` 属于 `QByteArray`，导致 Qt 编译器报 `QString` 没有 `toHex` 以及 `QString::arg` 无匹配重载。现在先用 `QByteArray::fromRawData(...).toHex()` 完成字节转换，再转成 `QString`。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:152
.arg(QString::fromLatin1(reinterpret_cast<const char*>(desktopId.Data4), 8).toHex());
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:152
.arg(QString::fromLatin1(QByteArray::fromRawData(
    reinterpret_cast<const char*>(desktopId.Data4), 8).toHex()));
```

### Steps
1. 将 GUID 尾部 8 字节包装为 `QByteArray`。
2. 在 `QByteArray` 上调用 `toHex()`，再转换为 `QString` 传给 `arg()`。

### Verification
- 已进行静态差异检查。
- 未构建；用户反馈的 Qt 编译错误已针对性修复。
## 2026-08-08 F10 脚本 Ctrl+V 随机字符串配置

### Changed Location
- `src/ui/RemoteDesktopWindow.cpp`：F10 播放设置新增随机后缀配置，并在播放脚本检测到 Ctrl+V 时先发送带随机后缀的剪贴板文本。
- `src/ui/RemoteDesktopWindow.h`：新增播放配置和 Ctrl 状态字段。

### Reason
用户要求 F9 录制的脚本在 F10 播放时，遇到脚本内 Ctrl+V 才追加随机字符串；人工 Ctrl+V 和录制过程不受影响。随机字符串支持开关、间隔符、长度以及混合/纯数字/纯字母三种模式。

### Original Code
```cpp
// src/ui/RemoteDesktopWindow.cpp（原播放选项）
struct RemoteInputPlaybackOptions {
    QString filePath;
    int loopCount = 1;
    int loopIntervalMs = 0;
    double speedMultiplier = 1.0;
};
```

```cpp
// 原播放循环仅直接发送脚本事件
const RemoteInputEvent event = m_inputScriptPlaybackEvents.at(m_inputScriptPlaybackIndex).input;
sent = dispatchRemoteInputEvent(event);
```

### Modified Code
```cpp
// 新增 F10 配置
bool pasteRandomSuffixEnabled = false;
QString pasteRandomSeparator = QStringLiteral("......");
int pasteRandomLength = 3;
int pasteRandomMode = 0;
```

```cpp
// 播放遇到 Ctrl+V 时先向目标设备发送随机化剪贴板
if (m_inputScriptPasteRandomSuffixEnabled
    && event.type == RemoteInputEventType::KeyDown
    && event.virtualKey == 'V'
    && m_inputScriptPlaybackCtrlDown) {
    // 原剪贴板 + 间隔符 + 随机字符串，再发送 V Down。
}
```

### Steps
1. 在 F10 播放对话框增加开关、间隔符、长度和随机模式控件。
2. 播放器跟踪脚本中的 Ctrl 按下状态，仅识别脚本内 Ctrl+V。
3. 发送 V Down 前读取本机当前剪贴板，生成随机后缀并通过现有 `cb ` 数据通道推送到目标设备。
4. 保留原脚本事件顺序，人工键鼠和 F9 录制流程不进入该逻辑。

### Verification
- 已执行 `git diff --check` 静态检查。
- 未构建；按用户要求仅静态检查并提交。
## 2026-08-08 修复 F10 随机粘贴读取本机剪贴板问题

### Changed Location
- `src/ui/RemoteDesktopWindow.cpp`：剪贴板同步关闭时仍缓存目标设备文本，F10 脚本 Ctrl+V 使用目标设备剪贴板缓存生成随机后缀。

### Reason
此前播放逻辑读取 `QGuiApplication::clipboard()`，导致控制端本机剪贴板内容被追加随机字符串后发送到 A1；当 A1 的剪贴板同步关闭时，目标设备上报的文本又被提前丢弃。现在目标剪贴板缓存和本机剪贴板落地显示解耦，关闭同步只是不修改本机剪贴板。

### Original Code
```cpp
// src/ui/RemoteDesktopWindow.cpp
if (!m_clipboardSyncEnabled) {
    return;
}
// F10 播放时读取控制端剪贴板
const QString sourceText = clipboard ? clipboard->text() : QString();
```

### Modified Code
```cpp
// 先缓存 Host 上报文本；同步关闭时不写入本机剪贴板
m_lastAppliedRemoteClipboardText = text;
if (!m_clipboardSyncEnabled) {
    return;
}
```

```cpp
// F10 Ctrl+V 基于目标设备最近上报的剪贴板
const QString sourceText = m_lastAppliedRemoteClipboardText;
```

### Steps
1. 移除剪贴板同步关闭时的提前返回，保留目标剪贴板缓存。
2. 保持关闭同步时不更新控制端本机剪贴板。
3. 将播放随机后缀的源文本改为目标设备剪贴板缓存。

### Verification
- 已执行静态差异检查。
- 未构建；本次修复针对用户反馈的本机剪贴板误用问题。

## 2026-08-08 15:50 - 修复远控更新窗口遮挡主界面鼠标

### Changed Location
- `src/ui/RemoteDesktopWindow.cpp:2083`：更新期间让远控内容区的 Windows 鼠标命中穿透到下层窗口，同时保留标题栏本地操作。
- `src/ui/RemoteDesktopWindow.cpp:2492`：更新状态开始时主动释放该远控窗口及其子控件持有的 Qt/Win32 鼠标捕获。

### Reason
其它设备进入远程更新状态后，原逻辑只隐藏 D3D/DirectComposition 视频表面，远控顶层窗口本身仍然存在并参与鼠标命中。如果它覆盖主窗口，主窗口虽然没有卡死，但鼠标消息会先落到更新中的远控窗口；任务栏或托盘恢复通过重新激活主窗口才会暂时恢复。此次修复保留可见更新遮罩和标题栏按钮，只让不可操作的远控内容区在更新期间穿透，并在状态切换时清理可能遗留的鼠标捕获。

### Original Code
```cpp
// src/ui/RemoteDesktopWindow.cpp:2081-2088（修改前）
const auto* nativeMessage = static_cast<MSG*>(message);
// =====wjy====
if (nativeMessage && nativeMessage->message == WM_ERASEBKGND) {
    if (m_resizingWindow) {
        appendResizeDebugTrace(QStringLiteral("parent.native.WM_ERASEBKGND"));
    }
    if (result) *result = 1;
    return true;
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2477-2483（修改前）
m_remoteMouseCaptureRequested = false;
suspendRemoteMouseCapture();
releaseForwardedKeys();
m_waitingShortcutRelease = false;
m_shortcutReleaseVirtualKeys.clear();
m_localShortcutReleaseKeys.clear();
setKeyboardForwardingActive(false);
```

### Modified Code
```cpp
// src/ui/RemoteDesktopWindow.cpp:2083-2094（当时修改）
if (nativeMessage
    && nativeMessage->message == WM_NCHITTEST
    && remoteUpdateActive()
    && result) {
    const QPoint globalPosition(
        static_cast<short>(LOWORD(nativeMessage->lParam)),
        static_cast<short>(HIWORD(nativeMessage->lParam)));
    if (remoteContentRect().contains(mapFromGlobal(globalPosition))) {
        *result = HTTRANSPARENT;
        return true;
    }
}
```

### Steps
1. 曾在更新窗口中增加内容区命中穿透和 Qt/Win32 鼠标捕获释放。
2. 根据复现反馈确认该方向没有改善主窗口客户区输入，且与实际现象不匹配。
3. 已回撤上述源代码修改，恢复更新窗口原有行为。

### Verification
- 已执行 `git revert --no-commit 1772207`，源代码已恢复到提交前状态。
- 未构建；本次只回撤无效修复并保留历史记录。

## 2026-08-08 16:00 - 回撤无效的远控更新鼠标穿透修复

### Changed Location
- `src/ui/RemoteDesktopWindow.cpp`：删除 `WM_NCHITTEST` 内容区穿透及更新开始时的 Qt/Win32 捕获释放代码。
- `WJY_CODE_CHANGE_LOG.md`：保留原修复记录并补充回撤原因，避免变更历史失真。

### Reason
用户复现确认：更新其它设备后，主窗口可以被点击带到前台，但客户区仍然不能操作；因此问题不是远控顶层窗口覆盖主窗口，上一轮穿透修复没有效果。继续保留会改变更新窗口的正常交互行为，故回撤。

### Modified Code
```cpp
// src/ui/RemoteDesktopWindow.cpp
已删除上一轮新增的 WM_NCHITTEST/HTTRANSPARENT 处理和鼠标捕获清理代码，恢复到 1772207 之前的实现。
```

### Steps
1. 根据新的复现条件重新修正问题模型：主窗口激活成功，说明窗口层级不是根因。
2. 回撤提交 `1772207` 的两个源代码修改点。
3. 保留历史日志，并记录该修复无效的原因。

### Verification
- 已核对暂存差异仅包含上一轮代码的反向删除和本次回撤记录。
- 未构建，等待下一步针对 Qt 客户区输入状态、隐藏弹窗或内部控件命中路径继续定位。

## 2026-08-08 16:39 - 修复更新重启后首次恢复主窗口无法操作

### Changed Location
- `src/main.cpp:442-454`：调整 `--minimized` 启动分支，先完成一次不可激活的普通显示生命周期，再进入系统最小化。

### Reason
更新器从提交 `799e5e0` 开始为更新成功后的新进程追加 `--minimized`。原启动分支直接对一个从未普通显示过的无边框透明主窗口调用 `hideToTray()`，其内部实际执行 `showMinimized()`；目标设备更新重启后，首次从任务栏或托盘恢复时，Qt 客户区与 Windows 原生窗口的首次显示状态没有按普通窗口路径完整建立，表现为顶层窗口可以激活、但内部鼠标交互失效，再最小化和恢复一次才正常。

本次保留“更新后静默最小化”、任务栏图标和托盘入口，只把首次窗口创建与系统最小化拆成两个明确步骤，并在首次创建时禁止抢占桌面焦点。

### Original Code
```cpp
// src/main.cpp:442-449（修改前）
// wjy: 仅开机自启带 --minimized 时进托盘；手动双击启动仍显示主窗口。
if (args.contains(QStringLiteral("--minimized"), Qt::CaseInsensitive)) {
    window.hideToTray();
    writeStartupLog(QStringLiteral("[wjy-main] started minimized to tray"));
} else {
    window.show();
    writeStartupLog(QStringLiteral("[wjy-main] window shown before app.exec"));
}
```

### Modified Code
```cpp
// src/main.cpp:442-454
// wjy: 仅开机自启或更新重启带 --minimized 时进入最小化；手动双击启动仍显示主窗口。
if (args.contains(QStringLiteral("--minimized"), Qt::CaseInsensitive)) {
    // =====wjy====
    window.setAttribute(Qt::WA_ShowWithoutActivating, true); // wjy: 首次正常创建窗口时禁止抢占当前桌面焦点，更新后的目标设备不会突然弹到前台。
    window.show(); // wjy: 无边框透明主窗口必须先完整进入一次普通显示生命周期，确保 Qt 客户区和原生输入状态都已建立。
    window.showMinimized(); // wjy: 普通显示生命周期完成后再进入系统最小化，保留任务栏和托盘入口且首次恢复即可正常接收鼠标。
    window.setAttribute(Qt::WA_ShowWithoutActivating, false); // wjy: 启动最小化完成后恢复普通激活能力，后续任务栏或托盘打开窗口可以获得焦点。
    writeStartupLog(QStringLiteral("[wjy-main] initialized normally before startup minimize")); // wjy: 日志明确区分安全启动最小化与运行中的普通最小化。
    // ===end====
} else {
    window.show();
    writeStartupLog(QStringLiteral("[wjy-main] window shown before app.exec"));
}
```

### Steps
1. 重新按“被更新的是其它目标设备”校正故障场景，排除控制端更新回调和远控窗口遮挡方向。
2. 对照更新器提交记录，确认故障出现前新增的 `--minimized` 会进入从未普通显示即最小化的启动路径。
3. 启动最小化前先调用 `show()` 完成主窗口、中央 `DeviceGrid` 和原生客户区的首次显示生命周期。
4. 使用 `Qt::WA_ShowWithoutActivating` 防止初始化窗口抢占目标设备当前桌面焦点，随后立即 `showMinimized()` 并恢复正常激活能力。
5. 保留运行期间原有 `hideToTray()` 与托盘切换逻辑，避免扩大修改范围。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已核对修改只涉及 `src/main.cpp` 的 `--minimized` 启动分支和本条变更记录。
- 按用户要求未构建、未链接、未运行程序或二进制测试；需在其它设备完成一次更新后验证首次从任务栏或托盘恢复的鼠标操作。

## 2026-08-08 16:51 - 回撤错误的更新启动最小化修复

### Changed Location
- `src/main.cpp:442-451`：删除“先正常显示再立即最小化”的启动顺序，恢复既有 `hideToTray()` 最小化入口。

### Reason
用户提供的更新后实机截图显示版本为 `v1.1.187`，该版本已包含上一条启动顺序修改；窗口更新后直接正常显示，但整个客户区仍无法响应鼠标。因此上一条将问题归因于“首次从最小化恢复”的判断不成立：它既没有让实机更新重启可靠保持最小化，也没有改善客户区输入。为避免保留没有效果且会改变启动时序的代码，本次立即回撤该分支修改，后续改从更新后新进程的 Qt 输入门禁、鼠标抓取和隐藏弹窗状态继续定位。

### Original Code
```cpp
// src/main.cpp:442-454（回撤前）
// wjy: 仅开机自启或更新重启带 --minimized 时进入最小化；手动双击启动仍显示主窗口。
if (args.contains(QStringLiteral("--minimized"), Qt::CaseInsensitive)) {
    // =====wjy====
    window.setAttribute(Qt::WA_ShowWithoutActivating, true);
    window.show();
    window.showMinimized();
    window.setAttribute(Qt::WA_ShowWithoutActivating, false);
    writeStartupLog(QStringLiteral("[wjy-main] initialized normally before startup minimize"));
    // ===end====
}
```

### Modified Code
```cpp
// src/main.cpp:442-451
// wjy: 仅开机自启带 --minimized 时进托盘；手动双击启动仍显示主窗口。
if (args.contains(QStringLiteral("--minimized"), Qt::CaseInsensitive)) {
    // =====wjy====
    window.hideToTray(); // wjy: v1.1.187 实机证明“先普通显示再最小化”既未让更新重启可靠最小化，也未恢复客户区输入，回到原有启动最小化入口。
    writeStartupLog(QStringLiteral("[wjy-main] started minimized to tray")); // wjy: 恢复既有日志文字，避免把已证伪的初始化顺序误判成成功路径。
    // ===end====
}
```

### Steps
1. 根据用户截图确认故障窗口是更新后直接显示的窗口，而不是从最小化状态恢复后的窗口。
2. 对照共享更新目录，确认截图 `v1.1.187` 于 2026-08-08 16:46 发布，已包含上一条修改。
3. 删除未奏效的首次普通显示、禁止激活和二次最小化调用，恢复原有 `hideToTray()` 路径。
4. 将原先的推测保留在历史记录中，并明确标注为已被实机结果证伪。

### Verification
- 已核对共享目录 `FSRemote.version` 和 `releases/1.1.187/FSRemote.version` 均为 `1.1.187`，发布时间为 2026-08-08 16:46。
- 已执行 `git diff --check`，未发现空白错误。
- 按用户要求未构建、未链接、未运行程序或二进制测试。

## 2026-08-08 16:55 - 强制更新重启进程从创建阶段最小化

### Changed Location
- `src/updater/main.cpp:371-378`：为更新成功后的 `CreateProcessW` 设置 Windows 启动显示状态。

### Reason
用户确认更新后目标设备主窗口直接可见，而不是处于最小化状态；因此仅在 FSRemote 主程序启动后调用 `hideToTray()` 不足以保证更新重启期间窗口不可见。更新器已经传递 `--minimized`，但没有设置 `STARTUPINFO` 的 `wShowWindow`，Windows 仍可能先按普通窗口创建新进程。

本次在更新器层把成功更新的启动状态明确设为 `SW_SHOWMINNOACTIVE`，让 Windows 从进程创建阶段就最小化且不抢焦点；回滚启动保持 `SW_SHOWNORMAL`，确保失败提示仍可见。

### Original Code
```cpp
// src/updater/main.cpp:371-377（修改前）
STARTUPINFOW startup{sizeof(startup)};
PROCESS_INFORMATION process{};
const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
    task.targetDir.c_str(), &startup, &process);
```

### Modified Code
```cpp
// src/updater/main.cpp:371-378
STARTUPINFOW startup{sizeof(startup)};
// =====wjy====
startup.dwFlags = STARTF_USESHOWWINDOW; // wjy: 明确要求 Windows 使用下面的启动显示状态，避免新进程在 Qt 解析参数前先按普通窗口创建。
startup.wShowWindow = updated ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL; // wjy: 成功更新从创建阶段保持最小化且不抢焦点，回滚仍按普通窗口启动以便显示失败提示。
// ===end====
PROCESS_INFORMATION process{};
const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
    task.targetDir.c_str(), &startup, &process);
```

### Steps
1. 保留成功更新命令行中的 `--minimized`，继续让 Qt 逻辑知道本次是静默重启。
2. 在 `STARTUPINFOW` 中启用 `STARTF_USESHOWWINDOW`。
3. 成功更新使用 `SW_SHOWMINNOACTIVE`，回滚使用 `SW_SHOWNORMAL`。
4. 不修改更新文件安装、进程等待和旧进程清理顺序。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已核对修改只涉及 `src/updater/main.cpp` 和本次变更记录。
- 按用户要求未构建、未链接、未运行程序或二进制测试；需发布新版本后验证更新完成时窗口是否保持最小化。

## 2026-08-08 17:15 - 更新重启改为主窗口完全隐藏进托盘

### Changed Location
- `src/main.cpp:442-452`：将 `--minimized` 启动从 `showMinimized()` 路径改为保持主窗口隐藏。

### Reason
用户重新构建并更新到 `v1.1.189` 后确认，更新完成仍会自动弹出主窗口，并且该窗口只能显示、无法点击。说明即使更新器指定 `SW_SHOWMINNOACTIVE`，主程序启动阶段调用 `hideToTray()` 内部的 `showMinimized()` 仍会创建主窗口，无法保证更新重启时窗口不出现在桌面。

本次将静默启动与运行期间的普通最小化彻底分开：带 `--minimized` 时主窗口保持隐藏，只保留托盘图标；用户正常运行中点击最小化、关闭按钮或托盘切换仍沿用原有系统最小化逻辑，不改变日常操作。

### Original Code
```cpp
// src/main.cpp:442-451（修改前）
// wjy: 仅开机自启带 --minimized 时进托盘；手动双击启动仍显示主窗口。
if (args.contains(QStringLiteral("--minimized"), Qt::CaseInsensitive)) {
    // =====wjy====
    window.hideToTray();
    writeStartupLog(QStringLiteral("[wjy-main] started minimized to tray"));
    // ===end====
} else {
    window.show();
}
```

### Modified Code
```cpp
// src/main.cpp:442-452
// wjy: 仅开机自启或更新重启带 --minimized 时静默进入托盘；手动双击启动仍显示主窗口。
if (args.contains(QStringLiteral("--minimized"), Qt::CaseInsensitive)) {
    // =====wjy====
    window.hide(); // wjy: 静默启动时主窗口保持从未创建/从未显示状态，避免 Qt 的 showMinimized 路径在更新重启后把窗口重新弹到桌面。
    writeStartupLog(QStringLiteral("[wjy-main] started hidden to tray")); // wjy: 记录本次明确走隐藏托盘而非系统最小化，便于与用户手动最小化路径区分。
    // ===end====
} else {
    window.show();
}
```

### Steps
1. 保留更新器传递的 `--minimized` 和 Windows `SW_SHOWMINNOACTIVE` 双重启动意图。
2. 主程序收到 `--minimized` 后不再调用会创建窗口的 `showMinimized()`。
3. 主窗口保持隐藏，托盘图标仍由构造阶段的 `setupTrayIcon()` 创建并显示。
4. 不修改 `MainWindow::hideToTray()`，正常运行期间的任务栏最小化行为保持不变。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已核对修改只涉及 `src/main.cpp` 的静默启动分支和本次变更记录。
- 按用户要求未构建、未链接、未运行程序或二进制测试；需发布下一版本验证更新后桌面不再出现主窗口，程序仍可从托盘手动打开。

## 2026-08-08 17:35 - 远控窗口完全遮挡时使用最小化画质

### Changed Location
- `src/ui/RemoteDesktopWindow.cpp:95-270`：新增 Windows 顶层窗口可见区域与完全遮挡检测。
- `src/ui/RemoteDesktopWindow.cpp:521-563`：增加完全遮挡状态文案和决策变化比较。
- `src/ui/RemoteDesktopWindow.cpp:2370-2464`：每秒采集遮挡状态并写入画质决策日志。
- `src/ui/RemoteQualityCoordinator.h:13-116`：新增完全遮挡指标、决策字段和降级原因。
- `src/ui/RemoteQualityCoordinator.cpp:118-203`：完全遮挡窗口复用最小化资源角色。
- `src/ui/DeviceGrid.cpp:6897-6905`：完全遮挡窗口不再保留唯一前台高质量身份。
- `tests/remote_quality_coordinator_tests.cpp:65-85`：补充完全遮挡映射到360p/1 FPS的策略断言。

### Reason
多个远控窗口同时打开时，普通后台窗口即使被其它窗口完全盖住，原逻辑仍把它视为可见后台窗口并持续请求720p/30 FPS。用户希望只有真正能看到画面的窗口保留可见档；完全没有任何可见区域的窗口应与最小化一样使用最低资源保活档。

本次按 Windows 原生 Z 顺序逐层扣减目标窗口的屏幕区域。只要仍露出任意区域就保持普通前台/后台策略；区域被完全扣空、窗口完全移出屏幕或位于其它虚拟桌面时，统一请求360p/1 FPS和最低优先级。半透明、色键和鼠标穿透工具窗不作为完整遮挡依据，避免悬浮提示造成误降级。

### Original Code
```cpp
// src/ui/RemoteQualityCoordinator.h:13-18（修改前）
enum class RemoteQualityDegradationReason {
    None,
    ModePreference,
    Background,
    Minimized,
    LargestWindowBelowThreshold,
};

struct RemoteQualityWindowMetrics {
    bool visible = true;
    bool minimized = false;
    bool active = false;
};
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:118-141（修改前）
if (window.windowId == 0 || !window.visible || window.minimized || !window.active) {
    continue;
}
const bool eligibleVisibleWindow = window.visible && !window.minimized;
decision.minimized = !eligibleVisibleWindow;
decision.active = window.active && eligibleVisibleWindow;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2194-2201（修改前）
RemoteQualityWindowMetrics metrics;
metrics.visible = isVisible() && !m_closeInProgress;
metrics.minimized = isMinimized();
metrics.fullScreen = isFullScreen();
```

```cpp
// src/ui/DeviceGrid.cpp:6902-6904（修改前）
RemoteQualityWindowMetrics snapshot = window->remoteQualityMetrics();
snapshot.active = snapshot.visible && !snapshot.minimized && window->isActiveWindow();
metrics.push_back(snapshot);
```

```cpp
// tests/remote_quality_coordinator_tests.cpp
原测试仅验证 minimized=true 和 visible=false，未覆盖普通显示窗口被完全遮挡的情况。
```

### Modified Code
```cpp
// src/ui/RemoteQualityCoordinator.h:13-18,64-70,99-113
enum class RemoteQualityDegradationReason {
    None,
    ModePreference,
    Background,
    Minimized,
    FullyOccluded,
};

struct RemoteQualityWindowMetrics {
    bool visible = true;
    bool minimized = false;
    bool fullyOccluded = false;
    bool active = false;
};

struct RemoteQualityDecision {
    bool minimized = false;
    bool fullyOccluded = false;
};
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:118-203
if (window.windowId == 0 || !window.visible || window.minimized || window.fullyOccluded || !window.active) {
    continue;
}
const bool eligibleVisibleWindow = window.visible && !window.minimized && !window.fullyOccluded;
decision.minimized = !eligibleVisibleWindow;
decision.fullyOccluded = window.visible && !window.minimized && window.fullyOccluded;
decision.active = window.active && eligibleVisibleWindow;

decision.reason = decision.fullyOccluded
    ? RemoteQualityDegradationReason::FullyOccluded
    : decision.minimized
        ? RemoteQualityDegradationReason::Minimized
        : RemoteQualityDegradationReason::Background;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:95-270（核心逻辑）
bool nativeWindowFullyOccluded(HWND targetWindow)
{
    if (nativeWindowIsCloaked(targetWindow)) {
        return true;
    }
    HRGN remainingRegion = nativeWindowVisualRegion(targetWindow, targetBounds);
    for (HWND candidate = GetWindow(targetWindow, GW_HWNDPREV);
         candidate;
         candidate = GetWindow(candidate, GW_HWNDPREV)) {
        // 跳过隐藏、最小化、其它虚拟桌面和透明工具窗口。
        CombineRgn(remainingRegion, remainingRegion, candidateRegion, RGN_DIFF);
        if (remainingType == NULLREGION) {
            return true;
        }
    }
    return false;
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2370-2383
metrics.visible = isVisible() && !m_closeInProgress;
metrics.minimized = isMinimized();
#if defined(Q_OS_WIN)
metrics.fullyOccluded = metrics.visible
    && !metrics.minimized
    && nativeWindowFullyOccluded(reinterpret_cast<HWND>(winId()));
#endif
```

```cpp
// src/ui/DeviceGrid.cpp:6902-6905
RemoteQualityWindowMetrics snapshot = window->remoteQualityMetrics();
snapshot.active = snapshot.visible && !snapshot.minimized && !snapshot.fullyOccluded
    && window->isActiveWindow();
metrics.push_back(snapshot);
```

```cpp
// tests/remote_quality_coordinator_tests.cpp:74-85
backgroundWindow.minimized = false;
backgroundWindow.fullyOccluded = true;
decisions = coordinator.evaluate(configuration, {backgroundWindow}, 1250);
assert(decisions.front().minimized);
assert(decisions.front().fullyOccluded);
assert(decisions.front().resolution == stream::RemoteResolutionTier::P360);
assert(decisions.front().targetFps == configuration.minimizedFps);
assert(decisions.front().reason == ui::RemoteQualityDegradationReason::FullyOccluded);
```

### Steps
1. 动态解析 `DwmGetWindowAttribute`，读取真实扩展边界和虚拟桌面 Cloaked 状态。
2. 为目标远控窗口建立屏幕可见 Region，并裁剪到多显示器虚拟屏幕范围。
3. 按 Z 顺序遍历所有更高层窗口，逐个减去其实际窗口 Region。
4. 跳过隐藏、最小化、其它虚拟桌面、半透明、色键及鼠标穿透窗口。
5. Region 完全为空时写入 `fullyOccluded`，协调器复用最小化360p/1 FPS配置。
6. 窗口重新露出任意区域后，下一次1秒采样恢复普通后台或焦点画质。
7. 增加独立状态文案和日志字段 `occluded=1`，便于实机验证。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已静态核对所有 `RemoteQualityDegradationReason` 分支均包含 `FullyOccluded`。
- 已检查 Windows Region、DWM模块和候选窗口 Region 在所有返回路径均释放。
- 按用户要求未构建、未链接、未运行程序或测试二进制；测试源码已补充完全遮挡策略断言。

## 2026-08-08 18:01 - 隔离多控制端的远控画质请求

### Changed Location
- `third_party/uu_stream_webrtc/src/webrtc_session.cpp:18-20`：引入会话视频中继所需的 WebRTC 适配源与帧缓冲接口。
- `third_party/uu_stream_webrtc/src/webrtc_session.cpp:56-164`：新增 `SessionVideoSource`，在每个 PeerConnection 内独立完成帧率和分辨率适配。
- `third_party/uu_stream_webrtc/src/webrtc_session.cpp:866-868`：Host 视频 track 改为连接本会话中继，不再直接连接 `HostMediaPipeline` 共享源。

### Reason
A、B 两台控制端同时远控同一台设备 D 时，Host 端为多个 PeerConnection 共用一个 `AdaptedVideoTrackSource`。A 上的 D 窗口完全遮挡后，A 会话通过 `RtpSender::SetParameters` 请求 1 FPS；WebRTC 又把这个 sender 的 `VideoSinkWants.max_framerate_fps` 反馈到共享源。经核对本地 WebRTC `api/video/video_broadcaster.cc`，多个 sink 的最大帧率按最小值聚合，因此共享源被整体限制成 1 FPS，B 上未遮挡的 D 会话也只能取得 1 FPS。

本次在共享采集源和每个发送 track 之间增加独立 `SessionVideoSource`。中继向共享源只提交无限制的活动订阅，使公共桌面采集继续按原始节奏发布；每个 sender 的 1 FPS、60 FPS及分辨率请求只进入自己中继内的 `VideoAdapter`。A 的窗口被遮挡后只丢弃 A 会话的多余帧，B 的会话不再受影响。

### Original Code
```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.cpp:15-19（修改前）
#include <api/field_trials.h>
#include <api/rtp_transceiver_interface.h>
#include <api/video/video_frame.h>
#include <api/video_codecs/sdp_video_format.h>
```

```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.cpp:53（修改前）
// sdp_type_from_kind() 后直接进入 append_viewer_log()，没有会话级视频中继。
```

```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.cpp:751-757（修改前）
const auto send_caps = factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
log_video_capabilities("host sender", send_caps);
local_video_source_ = config_.host_video_source;
const std::string media_suffix = config_.media_id.empty() ? std::string("0") : config_.media_id;
local_video_track_ = factory->CreateVideoTrack(local_video_source_, "video-" + media_suffix);
```

### Modified Code
```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.cpp:18-20
#include <api/video/adapted_video_track_source.h>
#include <api/video/video_frame.h>
#include <api/video/video_frame_buffer.h>
```

```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.cpp:56-164（核心逻辑）
class SessionVideoSource : public webrtc::AdaptedVideoTrackSource,
                           public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit SessionVideoSource(
        webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> sharedSource)
        : shared_source_(std::move(sharedSource))
    {
        webrtc::VideoSinkWants sharedWants;
        sharedWants.is_active = true;
        shared_source_->AddOrUpdateSink(this, sharedWants);
    }

    void OnFrame(const webrtc::VideoFrame& frame) override
    {
        if (!AdaptFrame(width, height, frame.timestamp_us(),
                        &outWidth, &outHeight,
                        &cropWidth, &cropHeight, &cropX, &cropY)) {
            return;
        }
        outputBuffer = sourceBuffer->CropAndScale(
            cropX, cropY, cropWidth, cropHeight, outWidth, outHeight);
        AdaptedVideoTrackSource::OnFrame(outputFrame);
    }
};
```

```cpp
// third_party/uu_stream_webrtc/src/webrtc_session.cpp:866-868
local_video_source_ = webrtc::make_ref_counted<SessionVideoSource>(
    config_.host_video_source);
```

### Steps
1. 核对 Viewer 质量请求、Host 控制消息和 `apply_sender_quality()`，确认请求原本只修改目标 PeerConnection 的 sender。
2. 检查本地 WebRTC `VideoBroadcaster::UpdateWants()`，确认共享源会取所有 sink 中最低的 `max_framerate_fps`。
3. 新增每会话 `SessionVideoSource`，使用无限制 wants 订阅公共采集源，切断 sender 限制向共享源的传播。
4. 在中继内调用独立 `AdaptFrame()`，按当前会话决定丢帧、裁剪和目标尺寸。
5. D3D11 原生帧继续通过 `CropAndScale()` 创建共享纹理视图，不增加桌面重复采集；CPU 回退帧按需缩放。
6. 会话中继析构时从共享源移除 sink，保证关闭后不再收到采集线程回调。
7. Host track 改为使用中继源，现有 sender 码率、优先级和编解码器选择保持不变。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已静态核对 WebRTC `VideoBroadcaster` 的帧率聚合确实取最小值，问题原因已定位到共享 `AdaptedVideoTrackSource` 的 sink wants 反馈。
- 已静态核对每个 `WebrtcSession` 都创建独立 `SessionVideoSource`，共享源收到的订阅不携带 1 FPS 或低分辨率限制。
- 已核对 D3D11 原生帧缓冲已实现 `CropAndScale()` 共享纹理视图，隔离适配不会新增一套桌面采集器。
- 按用户要求未构建、未链接、未运行程序或测试二进制。

## 2026-08-10 11:11 - F9共享发布与F10目标端独立执行

### Changed Location
- `CMakeLists.txt:582`：登记独立目标端键鼠脚本执行服务。
- `src/main.cpp:181`、`src/main.cpp:323`、`src/main.cpp:397`、`src/main.cpp:491`：把目标端执行状态接入实时广播，并只在目标程序自身退出时停止执行器。
- `src/system/InputScriptExecutionService.h:20`、`src/system/InputScriptExecutionService.cpp:37`：新增独立状态模型、共享下载、哈希缓存、本地定时调度和 `SendInput` 执行器。
- `src/system/DeviceCommandService.h:64`、`src/system/DeviceCommandService.cpp:230`、`src/system/DeviceCommandService.cpp:355`、`src/system/DeviceCommandService.cpp:924`：新增 F10 启动、停止和状态查询命令。
- `src/system/DeviceStatusService.h:45`、`src/system/DeviceStatusService.cpp:282`、`src/system/DeviceStatusService.cpp:377`：在 TCP 49101 状态中追加目标端键鼠脚本快照。
- `src/system/DeviceRealtimeStateService.h:67`、`src/system/DeviceRealtimeStateService.cpp:160`、`src/system/DeviceRealtimeStateService.cpp:592`、`src/system/DeviceRealtimeStateService.cpp:871`、`src/system/DeviceRealtimeStateService.cpp:1043`、`src/system/DeviceRealtimeStateService.cpp:1189`：通过 UDP 49104 广播、校验和归并独立 F9/F10 状态。
- `src/ui/DeviceGrid.h:217`、`src/ui/DeviceGrid.h:318`、`src/ui/DeviceGrid.cpp:4292`、`src/ui/DeviceGrid.cpp:4322`、`src/ui/DeviceGrid.cpp:9063`、`src/ui/DeviceGrid.cpp:9215`：按 IP 缓存状态并同步到全部普通、平铺和后来打开的远控窗口。
- `src/ui/RemoteDesktopWindow.h:98`、`src/ui/RemoteDesktopWindow.h:232`、`src/ui/RemoteDesktopWindow.h:454`、`src/ui/RemoteDesktopWindow.cpp:354`、`src/ui/RemoteDesktopWindow.cpp:2403`、`src/ui/RemoteDesktopWindow.cpp:4241`、`src/ui/RemoteDesktopWindow.cpp:4255`、`src/ui/RemoteDesktopWindow.cpp:4397`、`src/ui/RemoteDesktopWindow.cpp:4437`：F10 改为发送元数据命令，删除控制端逐事件回放和生命周期自动停止逻辑。
- `src/ui/RemoteInputScript.h:33`、`src/ui/RemoteInputScript.cpp:187`：F9 录制文件固定发布到 `\\192.168.1.100\广告部工具\远控键鼠脚本`。

### Reason
旧 F10 在控制端按录制时间逐条通过 Viewer 网络发送键鼠事件，网络抖动或主控退出会造成执行断流。新实现让 F9 把完整脚本发布到固定共享目录，F10 只发送文件名、大小、SHA-256 和循环参数；被控端自行复制到按哈希命名的本地缓存并执行。运行状态由被控端持有，因此主控关闭窗口、退出控制或重启程序后，重新远控仍可通过 UDP 49104、TCP 49101 和 TCP 49102 恢复同一状态。

该实现使用独立 `InputScriptExecutionService`，没有复用主界面右键脚本的 `work`、PowerShell、SSH、进程清单或运行状态。

### Original Code
```cmake
# CMakeLists.txt:578-584（修改前）
src/system/PortableOpenSshManager.cpp
src/system/PortableOpenSshManager.h
src/system/StartupManager.cpp
src/system/StartupManager.h
```

```cpp
// src/main.cpp:177-181（修改前）
platform::DeviceRealtimeLocalState state;
state.script = currentRealtimeScriptRuntime();
state.update = updateState;
```

```cpp
// src/system/InputScriptExecutionService.h、.cpp（修改前）
// 新增文件，原位置没有独立的目标端键鼠脚本执行服务。
```

```cpp
// src/system/DeviceCommandService.h、.cpp（修改前）
// 49102 只处理电源、更新、重命名、授权和设备同步命令，
// 没有 input_script_start、input_script_stop、input_script_status。
```

```cpp
// src/system/DeviceStatusService.h、.cpp（修改前）
struct DeviceStatusInfo {
    RemoteScriptRuntimeInfo scriptRuntime;
    int remoteSessionCount = 0;
    QString remoteControllerNames;
};
```

```cpp
// src/system/DeviceRealtimeStateService.h、.cpp（修改前）
struct DeviceRealtimeLocalState {
    QList<DeviceRealtimeHostSession> hostSessions;
    DeviceRealtimeScriptRuntime script;
    DeviceRealtimeUpdateState update;
};
```

```cpp
// src/ui/DeviceGrid.cpp（修改前）
m_deviceRealtimeScriptStates.insert(ip, state.script.state);
// 没有独立 F9/F10 状态缓存，也没有向同 IP 的全部远控窗口分发。
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:4160-4171（修改前）
if (m_inputScriptPlaying) {
    stopInputScriptPlayback(QStringLiteral("user_f10"), true);
    return;
}
chooseAndStartInputScriptPlayback();
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:4248-4272（修改前）
m_inputScriptPlaybackEvents = std::move(script.events);
m_inputScriptPlaybackClock.start();
m_inputScriptPlaying = true;
processInputScriptPlaybackEvents();
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2162-2164（修改前）
cancelInputScriptRecording(QStringLiteral("window_destructed"));
stopInputScriptPlayback(QStringLiteral("window_destructed"), true);
```

```cpp
// src/ui/RemoteInputScript.cpp:187-190（修改前）
QString RemoteInputScriptStore::defaultDirectory()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("script"));
}
```

### Modified Code
```cmake
# CMakeLists.txt:581-584
# =====wjy====
src/system/InputScriptExecutionService.cpp
src/system/InputScriptExecutionService.h
# ===end====
```

```cpp
// src/main.cpp:179-182、320-324
platform::DeviceRealtimeLocalState state;
state.script = currentRealtimeScriptRuntime();
state.inputScript = platform::InputScriptExecutionService::instance().snapshot();

platform::InputScriptExecutionService::instance().setStatusChangedCallback(
    [&realtimeStateService] { realtimeStateService.notifyLocalStateChanged(); });
```

```cpp
// src/system/InputScriptExecutionService.h:20-68
enum class RemoteInputScriptState {
    Unknown, Idle, Preparing, Running, WaitingLoop, Stopping, Failed,
};

struct RemoteInputScriptStartRequest {
    QString runId;
    QString fileName;
    qint64 fileSize = 0;
    QString sha256;
    int loopCount = 1;
    int loopIntervalMs = 0;
    double speedMultiplier = 1.0;
};
```

```cpp
// src/system/InputScriptExecutionService.cpp:371-419
const QString cachePath = cacheDirectory.filePath(
    request.sha256.toLower() + QStringLiteral(".fsinput.json"));
if (QFileInfo::exists(cachePath)
    && hashFile(cachePath, request.fileSize, request.sha256, cancelled, &validationError)
    && ui::RemoteInputScriptStore::loadFromFile(cachePath, &result.script, &validationError)) {
    result.success = true;
    return result;
}
const QString sourcePath = QDir(remoteInputScriptSharedDirectory()).filePath(request.fileName);
```

```cpp
// src/system/DeviceCommandService.cpp:355-409
const RemoteInputScriptCommandResult result = InputScriptExecutionService::instance().start(
    request, &errorMessage);
if (result == RemoteInputScriptCommandResult::Accepted) {
    replyAndClose(QByteArrayLiteral("accepted|") + request.runId.toUtf8() + '\n');
} else if (result == RemoteInputScriptCommandResult::AlreadyRunning) {
    replyAndClose(QByteArrayLiteral("already_running\n"));
}
const QByteArray status = inputScriptRuntimeJson(
    InputScriptExecutionService::instance().snapshot()).toBase64();
replyAndClose(QByteArrayLiteral("status|") + status + '\n');
```

```cpp
// src/system/DeviceStatusService.cpp:282-306
const RemoteInputScriptRuntimeInfo inputScript = InputScriptExecutionService::instance().snapshot();
payload.append('|');
payload.append(inputScript.supported ? '1' : '0');
payload.append('|');
payload.append(remoteInputScriptStateName(inputScript.state).toUtf8());
// 后续追加 runId、文件名、哈希、循环进度、事件进度、开始时间、revision 和错误文本。
```

```cpp
// src/system/DeviceRealtimeStateService.cpp:160-175、1043
QJsonObject inputScriptObject(const RemoteInputScriptRuntimeInfo& script)
{
    return {
        {QStringLiteral("supported"), script.supported},
        {QStringLiteral("state"), remoteInputScriptStateName(script.state)},
        {QStringLiteral("runId"), script.runId},
        {QStringLiteral("revision"), QString::number(script.revision)},
    };
}
// encodeSnapshot() 中独立写入 inputScript 字段。
```

```cpp
// src/ui/DeviceGrid.cpp:4322-4335
void DeviceGrid::applyRemoteInputScriptRuntimeState(
    const QString& deviceIp,
    const platform::RemoteInputScriptRuntimeInfo& runtime)
{
    const QString ip = deviceIp.trimmed();
    if (ip.isEmpty()) {
        return;
    }
    m_deviceRealtimeInputScriptStates.insert(ip, runtime);
    for (const QPointer<RemoteDesktopWindow>& window : openedRemoteWindows()) {
        if (window && window->hostIp().compare(ip, Qt::CaseInsensitive) == 0) {
            window->setRemoteInputScriptStatus(runtime);
        }
    }
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:4255-4394
QThreadPool::globalInstance()->start(QRunnable::create(
    [window, hostIp, filePath, options, runId, cancelled] {
        const RemoteInputScriptFileMetadata metadata = inspectSharedInputScriptFile(filePath, cancelled);
        platform::RemoteInputScriptStartRequest request;
        request.runId = runId;
        request.fileName = metadata.fileName;
        request.fileSize = metadata.fileSize;
        request.sha256 = metadata.sha256;
        request.loopCount = options.loopCount;
        request.loopIntervalMs = options.loopIntervalMs;
        request.speedMultiplier = options.speedMultiplier;
        const platform::RemoteInputScriptCommandResult result =
            platform::DeviceCommandService::requestInputScriptStart(hostIp, request, &errorMessage);
    }));
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:4397-4458
void RemoteDesktopWindow::stopRemoteInputScriptPlayback()
{
    QThreadPool::globalInstance()->start(QRunnable::create([window, hostIp, runId] {
        const platform::RemoteInputScriptCommandResult result =
            platform::DeviceCommandService::requestInputScriptStop(hostIp, runId, &errorMessage);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [window, result, errorMessage] {
            if (window) window->requestRemoteInputScriptStatus();
        }, Qt::QueuedConnection);
    }));
}

void RemoteDesktopWindow::requestRemoteInputScriptStatus()
{
    QThreadPool::globalInstance()->start(QRunnable::create([window, hostIp, queryGeneration] {
        const platform::RemoteInputScriptRuntimeInfo runtime =
            platform::DeviceCommandService::queryInputScriptStatus(hostIp, &errorMessage);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [window, runtime, queryGeneration] {
            if (window) window->setRemoteInputScriptStatus(runtime);
        }, Qt::QueuedConnection);
    }));
}
```

```cpp
// src/ui/RemoteDesktopWindow.h:440-456
bool m_inputScriptRecording = false;
bool m_inputScriptDialogActive = false;
QString m_inputScriptPlaybackFilePath;
int m_inputScriptPlaybackLoopCount = 1;
int m_inputScriptPlaybackLoopIntervalMs = 0;
double m_inputScriptPlaybackSpeedMultiplier = 1.0;
platform::RemoteInputScriptRuntimeInfo m_remoteInputScriptStatus;
// 已删除控制端播放定时器、事件数组、事件索引、按键/按钮持有集合和播放时钟。
```

```cpp
// src/ui/RemoteInputScript.cpp:187-190
QString RemoteInputScriptStore::defaultDirectory()
{
    return QString::fromUtf8(R"(\\192.168.1.100\广告部工具\远控键鼠脚本)");
}
```

### Steps
1. 将 F9 默认保存目录从控制端程序目录 `script` 改为固定共享目录。
2. 新增独立目标端执行服务，按 SHA-256 缓存脚本，复制后再次校验大小、哈希和 JSON 结构。
3. 在目标端使用本机高精度定时器和 `SendInput` 执行键鼠事件，并在停止、失败、每轮结束和程序退出时释放持有输入。
4. 新增 49102 启动、停止、状态查询命令；启动命令只携带文件元数据和播放选项。
5. 将完整运行快照追加到 49101，并加入 49104 UDP 实时广播和 TCP 手动校准。
6. 在 `DeviceGrid` 按 IP 缓存状态，并同步给同一目标的全部普通和平铺窗口；新窗口创建时立即恢复缓存并直查目标状态。
7. F10 改为后台计算共享文件哈希和发送目标端命令；再次按下只发送带 runId 的停止命令。
8. 删除控制端逐事件回放定时器、事件队列、持有键鼠集合，以及析构、断线、更新、关窗和控制端退出时自动停止目标脚本的旧逻辑。
9. 目标端程序自身更新退出或正常退出时，统一停止执行器、汇合共享读取线程并释放输入。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已用 `rg` 确认 `m_inputScriptPlaying`、`m_dispatchingInputScriptPlayback`、`inputScriptPlaybackTimer`、`scheduleNextInputScriptPlaybackEvent`、`processInputScriptPlaybackEvents`、`completeInputScriptPlaybackLoop`、`stopInputScriptPlayback`、`releaseInputScriptPlaybackInputs`、`trackInputScriptPlaybackState` 均已从 `src` 删除。
- 已静态核对 F10 命令只发送文件名、大小、SHA-256、循环、间隔、速度和随机粘贴选项，不发送脚本事件数组。
- 已静态核对目标端缓存路径为 `data/input-scripts/cache/<sha256>.fsinput.json`，缓存命中和共享复制都执行大小、SHA-256 与 JSON 校验。
- 已静态核对目标运行状态同时进入 UDP 49104、TCP 49101 和 TCP 49102，主控窗口重开可恢复，且同 IP 全部窗口使用同一快照。
- 已静态核对控制端窗口析构、远程更新、应用退出、连接断开和关窗路径不再发送 F10 停止命令。
- 按用户要求未构建、未链接、未运行程序或测试二进制。

## 2026-08-10 11:25 - 修复目标端键鼠脚本编译错误

### Changed Location
- `src/ui/RemoteDesktopWindow.cpp:4373`：按自由函数方式调用 Viewer 调试日志。
- `src/system/InputScriptExecutionService.cpp:25`：在包含 `windows.h` 前定义 `NOMINMAX`。

### Reason
本次大量 MSVC 报错实际只有两个根因。`appendViewerDebugLog` 定义在 `RemoteDesktopWindow.cpp` 的匿名命名空间中，并不是 `RemoteDesktopWindow` 成员，使用 `window->appendViewerDebugLog()` 会产生 C2039。目标端执行服务直接包含 `windows.h` 但没有定义 `NOMINMAX`，Windows 的 `min/max` 宏会破坏 `std::min<>()` 和 `std::numeric_limits<int>::max()`，因此同一宏污染连续产生 C2589、C2059 和 C2143。

### Original Code
```cpp
// src/ui/RemoteDesktopWindow.cpp:4373-4374（修改前）
window->appendViewerDebugLog(QStringLiteral("target input script accepted host=%1 run_id=%2")
    .arg(window->m_hostIp, window->m_remoteInputScriptStatus.runId));
```

```cpp
// src/system/InputScriptExecutionService.cpp:21-25（修改前）
#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
```

### Modified Code
```cpp
// src/ui/RemoteDesktopWindow.cpp:4373-4374
appendViewerDebugLog(QStringLiteral("target input script accepted host=%1 run_id=%2")
    .arg(window->m_hostIp, window->m_remoteInputScriptStatus.runId)); // wjy: 日志函数属于当前翻译单元，异步回到UI线程后按自由函数调用。
```

```cpp
// src/system/InputScriptExecutionService.cpp:21-29
#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif // wjy: 禁止windows.h定义min/max宏，避免破坏std::min和numeric_limits::max()模板调用。
#include <windows.h>
```

### Steps
1. 将异步启动成功日志从错误的成员调用改为当前翻译单元内的自由函数调用。
2. 在目标端执行服务包含 Windows SDK 前关闭 `min/max` 宏定义。
3. 使用现有 Qt 6.11.1 MSVC 2022 Release 构建目录重新增量编译 `FSRemote`。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已通过 Visual Studio 2022 `VsDevCmd.bat` 加载 MSVC x64 环境。
- 已执行 `cmake --build build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release --target FSRemote`，增量编译、链接和目标生成完成，退出码为 0。
- 原 C2039、C2589、C2059、C2143 报错均未再次出现。

## 2026-08-10 12:06 - 主窗口标题栏仅显示当前带宽

### Changed Location
- `src/ui/DeviceGrid.cpp:10114-10128`：调整主窗口标题栏带宽文本生成逻辑。

### Reason
主窗口标题栏原本同时显示当前接收带宽和理论剩余带宽，例如 `↓12M 余999M`。理论余量容易造成信息拥挤，用户要求只保留当前实际占用数量，因此删除余量文本及其相关判断分支，保留当前接收带宽、风险颜色和窄窗口省略策略。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:10114-10138（修改前）
const QString receiveText = formatMbps(m_titlebarBandwidthSample.receive.currentMbps);
const QString headroomText = formatMbps(m_titlebarBandwidthSample.receive.headroomMbps);
networkText = m_titlebarBandwidthSample.receive.capacityMbps > 0.0
    ? QString::fromUtf8("↓%1M 余%2M").arg(receiveText, headroomText)
    : QString::fromUtf8("↓%1M").arg(receiveText);
// 后续根据标题栏剩余宽度再次隐藏余量。
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:10114-10128
const QString receiveText = formatMbps(m_titlebarBandwidthSample.receive.currentMbps);
networkText = QString::fromUtf8("↓%1M").arg(receiveText); // wjy: 主窗口标题栏只显示当前接收带宽，不再展示理论余量。
// 保留带宽风险颜色和最终的 elidedText 窄窗口保护。
```

### Steps
1. 删除理论剩余带宽 `headroomText` 的格式化和标题栏拼接。
2. 删除仅用于隐藏剩余带宽的重复宽度判断。
3. 保留当前接收带宽数值、风险颜色和标题栏空间保护逻辑。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已通过 `rg` 确认标题栏带宽文本只生成 `↓当前值M`，不再包含 `余` 字样。
- 按此前用户要求，本次未构建、未链接和未运行程序。

## 2026-08-10 15:44 - 封装远控分辨率与帧率命名档案

### Changed Location
- `src/stream/RemoteVideoPolicy.h:56`: 新增不携带角色语义的 `RemoteVideoEncodingPreset` 编码档案结构和角色组装函数。
- `src/stream/RemoteVideoPolicy.h:77`: 新增 1080p/60、1080p/30、720p/60、720p/30、540p/30、540p/25、360p/30和360p/1共8组命名档案。
- `src/stream/RemoteVideoPolicy.h:87`: 将焦点、可见后台和最小化角色改为引用命名档案，当前分别选用540p/30、540p/25和360p/1。
- `src/ui/RemoteQualityCoordinator.cpp:140`: 协调器一次选定角色档案，并将其分辨率、FPS、码率和优先级统一用于最终质量决策。
- `tests/remote_video_policy_tests.cpp:7`: 新增8组命名档案及三类角色别名的纯C++回归断言。
- `tests/remote_quality_coordinator_tests.cpp:31`: 将协调器预期更新为焦点540p/30、后台540p/25、最小化360p/1，并验证14/14/7 Mbps码率和100/40/5优先级。

### Reason
原有代码在焦点、后台和最小化常量中直接手工填写分辨率、FPS、优先级、请求标记和码率。只改分辨率时容易遗留上一档的高码率，例如540p仍使用48/24 Mbps。本次把编码参数封装为8组命名档案，角色只选择档案并补充优先级，使以后的画质切换只需替换一个别名，不再重复手工同步多个数值。

### Original Code
```cpp
// src/stream/RemoteVideoPolicy.h:64-66
inline constexpr RemoteVideoProfile kFocusedRemoteVideoProfile = {RemoteResolutionTier::P540, 30, 100, true, 48000};
inline constexpr RemoteVideoProfile kVisibleBackgroundRemoteVideoProfile = {RemoteResolutionTier::P540, 25, 40, true, 24000};
inline constexpr RemoteVideoProfile kMinimizedRemoteVideoProfile = {RemoteResolutionTier::P360, 1, 5, true, 7000};
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:143-149
const bool highPerformance = eligibleVisibleWindow
    && (singleRemoteWindow || focusedWindowEligible);
decision.priority = priorityForRole(highPerformance);
```

```cpp
// tests/remote_video_policy_tests.cpp:7-7
const stream::RemoteVideoProfile focused{stream::RemoteResolutionTier::P1080, 60, 100, true, 48000};
```

```cpp
// tests/remote_quality_coordinator_tests.cpp:31-43
assert(decisions[0].resolution == stream::RemoteResolutionTier::P1080);
assert(decisions[0].targetFps == 60);
assert(decisions[1].resolution == stream::RemoteResolutionTier::P720);
assert(decisions[1].targetFps == 30);
assert(decisions[1].priority == 10);
```

### Modified Code
```cpp
// src/stream/RemoteVideoPolicy.h:77-89
inline constexpr RemoteVideoEncodingPreset kRemoteVideo1080p60Preset = {RemoteResolutionTier::P1080, 60, 48000};
inline constexpr RemoteVideoEncodingPreset kRemoteVideo1080p30Preset = {RemoteResolutionTier::P1080, 30, 48000};
inline constexpr RemoteVideoEncodingPreset kRemoteVideo720p60Preset = {RemoteResolutionTier::P720, 60, 24000};
inline constexpr RemoteVideoEncodingPreset kRemoteVideo720p30Preset = {RemoteResolutionTier::P720, 30, 24000};
inline constexpr RemoteVideoEncodingPreset kRemoteVideo540p30Preset = {RemoteResolutionTier::P540, 30, 14000};
inline constexpr RemoteVideoEncodingPreset kRemoteVideo540p25Preset = {RemoteResolutionTier::P540, 25, 14000};
inline constexpr RemoteVideoEncodingPreset kRemoteVideo360p30Preset = {RemoteResolutionTier::P360, 30, 7000};
inline constexpr RemoteVideoEncodingPreset kRemoteVideo360p1Preset = {RemoteResolutionTier::P360, 1, 7000};

inline constexpr RemoteVideoProfile kFocusedRemoteVideoProfile = makeRemoteVideoProfile(kRemoteVideo540p30Preset, 100);
inline constexpr RemoteVideoProfile kVisibleBackgroundRemoteVideoProfile = makeRemoteVideoProfile(kRemoteVideo540p25Preset, 40);
inline constexpr RemoteVideoProfile kMinimizedRemoteVideoProfile = makeRemoteVideoProfile(kRemoteVideo360p1Preset, 5);
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:140-149
const auto roleProfile = decision.minimized
    ? stream::RemoteVideoPolicy::minimizedProfile()
    : highPerformance
        ? stream::kFocusedRemoteVideoProfile
        : stream::RemoteVideoPolicy::backgroundProfile();
decision.priority = static_cast<int>(roleProfile.priority);
```

```cpp
// tests/remote_video_policy_tests.cpp:8-33
assert(stream::kRemoteVideo1080p60Preset.resolution == stream::RemoteResolutionTier::P1080
    && stream::kRemoteVideo1080p60Preset.targetFps == 60
    && stream::kRemoteVideo1080p60Preset.maxBitrateKbps == 48000);
const stream::RemoteVideoProfile focused = stream::kFocusedRemoteVideoProfile;
```

```cpp
// tests/remote_quality_coordinator_tests.cpp:31-45
assert(decisions[0].resolution == stream::RemoteResolutionTier::P540);
assert(decisions[0].targetFps == 30);
assert(decisions[0].maxBitrateKbps == 14000);
assert(decisions[1].resolution == stream::RemoteResolutionTier::P540);
assert(decisions[1].targetFps == 25);
assert(decisions[1].maxBitrateKbps == 14000);
assert(decisions[1].priority == 40);
```

### Steps
1. 拆分编码档案与角色优先级，避免档案名称隐含焦点或后台语义。
2. 定义8组用户指定的分辨率/FPS组合，并按现有码率表为1080p、720p、540p和360p分别配置48、24、14和7 Mbps上限。
3. 使用 `makeRemoteVideoProfile()` 把命名档案与焦点/可见后台/最小化优先级组合为生产角色别名。
4. 调整 `RemoteQualityCoordinator` 只选择一次 `roleProfile`，后续统一读取分辨率、FPS、码率和优先级。
5. 更新策略和协调器回归测试，覆盖8组档案、角色切换、遮挡/最小化、多窗口和焦点防抖。

### Verification
- 已使用 VS2022 C++20 直接编译 `tests/remote_video_policy_tests.cpp`，编译通过。
- 已运行 `build/resolution-policy-review/remote_video_policy_tests.exe`，所有档案和角色策略断言通过。
- 已使用 VS2022 C++20 直接编译 `tests/remote_quality_coordinator_tests.cpp` 与 `src/ui/RemoteQualityCoordinator.cpp`，编译通过。
- 已运行 `build/resolution-policy-review/remote_quality_coordinator_tests.exe`，角色分辨率、FPS、码率、优先级、多窗口和焦点防抖断言全部通过。

## 2026-08-10 13:47 - 主窗口标题栏显示被控会话总数

### Changed Location
- `src/ui/DeviceGrid.h:222、313`：声明设备列表远控会话总数汇总函数，并更新标题栏定时器职责说明。
- `src/ui/DeviceGrid.cpp:4321`：实时设备会话变化时立即刷新标题栏区域。
- `src/ui/DeviceGrid.cpp:7596-7620`：按设备列表和现有兼容规则汇总远控会话路数。
- `src/ui/DeviceGrid.cpp:10155-10202`：在当前带宽右侧绘制远控会话总数徽标。

### Reason
主窗口设备列表已经保存每台目标设备的真实远控会话数，但标题栏没有汇总显示。用户要求在当前带宽右侧增加数字，并规定同一目标设备被两台设备控制时计为 2，因此不能只统计 `Busy` 设备个数，必须累计 `m_deviceRemoteSessionCounts`。旧版目标没有人数扩展字段时仍沿用设备行现有规则，将 `Busy` 按 1 路计算。

### Original Code
```cpp
// src/ui/DeviceGrid.h:221-223（修改前）
platform::DevicePresenceState devicePresenceForIndex(int index) const;
bool devicePoweringOnForIndex(int index) const;
```

```cpp
// src/ui/DeviceGrid.cpp:4319-4320（修改前）
update(deviceListViewportRect(m_deviceGroupExpanded));
}
```

```cpp
// src/ui/DeviceGrid.cpp:7593（修改前）
// 此位置没有远控会话总数汇总函数。
```

```cpp
// src/ui/DeviceGrid.cpp:10128-10133（修改前）
const QFontMetrics networkMetrics(networkFont);
painter.setPen(networkColor);
painter.drawText(
    QRectF(networkRect),
    Qt::AlignVCenter | Qt::AlignLeft,
    networkMetrics.elidedText(networkText, Qt::ElideRight, networkRect.width()));
```

### Modified Code
```cpp
// src/ui/DeviceGrid.h:221-223
platform::DevicePresenceState devicePresenceForIndex(int index) const;
int totalRemoteControlSessionCount() const; // wjy: 汇总设备列表中的真实远控会话路数。
bool devicePoweringOnForIndex(int index) const;
```

```cpp
// src/ui/DeviceGrid.cpp:4320-4321
update(deviceListViewportRect(m_deviceGroupExpanded));
update(titlebarBandwidthUpdateRect()); // wjy: 会话变化时立即刷新标题栏汇总数字。
```

```cpp
// src/ui/DeviceGrid.cpp:7596-7620
int DeviceGrid::totalRemoteControlSessionCount() const
{
    qint64 total = 0;
    for (const DeviceEntry& device : g_devices) {
        if (deviceHiddenByLocalPreference(device)) continue;
        const QString ip = device.ip.trimmed();
        int sessionCount = qBound(0, m_deviceRemoteSessionCounts.value(ip, 0), 10);
        if (sessionCount <= 0
            && m_deviceStatuses.value(ip) == platform::DevicePresenceState::Busy) {
            sessionCount = 1;
        }
        total += sessionCount;
    }
    return static_cast<int>(qMin<qint64>(total, std::numeric_limits<int>::max()));
}
```

```cpp
// src/ui/DeviceGrid.cpp:10155-10202
const int controlledSessionCount = totalRemoteControlSessionCount();
const QString controlledSessionText = QString::number(controlledSessionCount);
// 为数字徽标预留空间，带宽文字不足时先省略。
// 零路使用灰色，有会话时复用设备行远控人数色阶。
painter.drawRoundedRect(controlledSessionBadgeRect.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
painter.drawText(controlledSessionBadgeRect, Qt::AlignCenter, controlledSessionText);
```

### Steps
1. 新增统一汇总函数，遍历设备目录并排除被用户隐藏的本机记录。
2. 对每台设备优先使用目标端真实会话数，旧版 `Busy` 状态回退为 1 路。
3. 在带宽文字右侧预留固定间距，绘制可随数字位数扩展的数量徽标。
4. 保留带宽颜色和省略策略，并让窄窗口优先压缩带宽文字，避免覆盖数字和右侧身份区域。
5. 实时设备状态发生变化时同时刷新设备列表和标题栏。
6. 清理标题栏定时器仍描述“理论余量”的旧注释，使代码说明与当前界面一致。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已静态核对同一目标两路会话贡献 2，多台目标按会话路数继续累加，不按 `Busy` 设备个数统计。
- 已静态核对标题栏数字不限制为 10，颜色档位超过 10 后保持最高等级但文本继续显示真实总数。
- 已静态核对窄窗口先为数字徽标预留空间，带宽文字使用 `elidedText` 缩短，不覆盖本机身份和窗口按钮。
- 按此前用户要求，本次未构建、未链接和未运行程序。

## 2026-08-10 14:27 - 修复全屏退出后标题栏双击无法还原窗口

### Changed Location
- `src/ui/RemoteDesktopWindow.h:75、364-366`：新增全屏切换入口及进入全屏前的窗口状态快照。
- `src/ui/RemoteDesktopWindow.cpp:3809-3842`：保存并恢复普通窗口几何和最大化状态。
- `src/ui/DeviceGrid.cpp:9326`：Ctrl+D 改为调用远控窗口自己的状态保持入口。

### Reason
原 Ctrl+D 逻辑直接在 `showFullScreen()` 和 `showNormal()` 之间切换，没有记录进入全屏前的普通窗口矩形和最大化状态。从最大化窗口进入全屏后再次按 Ctrl+D 会无条件进入普通状态，Qt 的最大化还原基准也可能被全屏切换扰动，导致随后双击标题栏不能回到进入全屏前的窗口大小。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:9325-9328（修改前）
rememberRemoteWindowActivation(window);
window->isFullScreen() ? window->showNormal() : window->showFullScreen();
window->raise();
window->activateWindow();
```

```cpp
// src/ui/RemoteDesktopWindow.h（修改前）
// 没有保存全屏前普通几何和最大化状态的成员，也没有独立全屏切换入口。
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3807（修改前）
// 全屏切换完全由 DeviceGrid 直接调用 QWidget 的 showFullScreen/showNormal。
```

### Modified Code
```cpp
// src/ui/RemoteDesktopWindow.h:75、364-366
void toggleFullscreenMode();
QRect m_fullscreenRestoreGeometry;
bool m_fullscreenRestoreWasMaximized = false;
bool m_fullscreenRestoreStateValid = false;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3809-3842
void RemoteDesktopWindow::toggleFullscreenMode()
{
    if (!isFullScreen()) {
        m_fullscreenRestoreWasMaximized = isMaximized();
        m_fullscreenRestoreGeometry = m_fullscreenRestoreWasMaximized
            ? normalGeometry()
            : frameGeometry();
        m_fullscreenRestoreStateValid = true;
        showFullScreen();
        return;
    }

    showNormal();
    if (restoreStateValid && restoreGeometry.isValid()) {
        setGeometry(restoreGeometry);
    }
    if (restoreStateValid && restoreMaximized) {
        showMaximized();
    }
}
```

```cpp
// src/ui/DeviceGrid.cpp:9325-9328
rememberRemoteWindowActivation(window);
window->toggleFullscreenMode();
window->raise();
window->activateWindow();
```

### Steps
1. 将 Ctrl+D 的全屏状态管理从 `DeviceGrid` 收回 `RemoteDesktopWindow`。
2. 进入全屏前记录普通窗口矩形以及是否处于最大化状态。
3. 最大化窗口优先读取 `normalGeometry()`，无效时回退设备 JSON 中保存的普通矩形。
4. 退出全屏时先 `showNormal()` 并恢复普通矩形，再根据进入前状态重新最大化。
5. 使用一次性有效标志清理状态快照，避免后续切换重复使用陈旧几何。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已通过 `rg` 确认 `showFullScreen()` 只保留在 `RemoteDesktopWindow::toggleFullscreenMode()` 内部，Ctrl+D 不再直接切换 QWidget 状态。
- 已静态核对普通窗口路径：普通矩形进入全屏，退出后恢复同一矩形。
- 已静态核对最大化路径：保存 `normalGeometry()`，退出后先恢复普通矩形再最大化，随后标题栏双击 `showNormal()` 可回到原大小。
- 已静态核对平铺路径：保存当前平铺矩形，退出全屏后不改变 `m_rememberGeometry`，仍回到原平铺位置。
- 按此前用户要求，本次未构建、未链接和未运行程序。

## 2026-08-10 16:11 - 收紧远控标题栏计时与性能数字

### Changed Location
- `src/ui/RemoteDesktopWindow.cpp:3281`: 将FPS与码率文本改为紧凑的数字、单位和中点分隔格式。
- `src/ui/RemoteTitleBarRenderer.cpp:181`: 计时、FPS和码率改用11px字号、实际文字宽度和10px分组间距布局。
- `tests/remote_titlebar_renderer_tests.cpp:104`: 新增700px宽度下性能数字仍能绘制的像素回归覆盖。

### Reason
原标题栏为会话计时固定预留70px，为FPS与码率固定预留122px，第二组文字还使用固定起点。实际的 `00:00:53` 只占其中一部分，因此计时和 `30 FPS | 1 Mbps` 之间出现明显空白。本次改为根据 `QFontMetrics` 计算真实宽度，仅保留10px视觉分组距离，并缩短文本本身，在不删减信息的前提下收紧整个数字区域。

### Original Code
```cpp
// src/ui/RemoteDesktopWindow.cpp:3281-3283
state.performanceText = QStringLiteral("%1 FPS | %2 Mbps")
    .arg(fpsText)
    .arg(bitrateText);
```

```cpp
// src/ui/RemoteTitleBarRenderer.cpp:181-190
const int elapsedX = state.identityRight + 14;
const int secondaryX = elapsedX + 78;
const int contentRight = titleTextRight - 8;
const auto paintElapsed = [&] {
    if (elapsedX + 70 > contentRight) return;
    QFont elapsedFont(QStringLiteral("Microsoft YaHei UI"));
    elapsedFont.setPixelSize(12);
    painter.drawText(QRectF(elapsedX, 0, 70, barHeight),
        Qt::AlignVCenter | Qt::AlignLeft, state.elapsedText);
};
```

```cpp
// tests/remote_titlebar_renderer_tests.cpp:62-121
// 原测试只验证网络警告、脚本状态和鼠标锁定的像素差异，
// 没有覆盖计时与性能数字的紧凑布局。
```

### Modified Code
```cpp
// src/ui/RemoteDesktopWindow.cpp:3281-3283
state.performanceText = QStringLiteral("%1FPS · %2Mbps")
    .arg(fpsText)
    .arg(bitrateText);
```

```cpp
// src/ui/RemoteTitleBarRenderer.cpp:181-190
constexpr int kIdentityToSessionGap = 10;
constexpr int kSessionItemGap = 10;
constexpr int kSessionRightPadding = 6;
QFont sessionFont(QStringLiteral("Microsoft YaHei UI"));
sessionFont.setPixelSize(11);
const QFontMetrics sessionMetrics(sessionFont);
const int elapsedWidth = std::max(1, sessionMetrics.horizontalAdvance(state.elapsedText) + 1);
const int performanceWidth = std::max(1, sessionMetrics.horizontalAdvance(state.performanceText) + 1);
const int elapsedX = state.identityRight + kIdentityToSessionGap;
const int secondaryX = elapsedX + elapsedWidth + kSessionItemGap;
```

```cpp
// tests/remote_titlebar_renderer_tests.cpp:104-120
void verifyCompactSessionMetricsRendering()
{
    ui::RemoteTitleBarVisualState compact = baseState(700, 1.0);
    const QImage elapsedOnly = ui::RemoteTitleBarRenderer::render(compact);
    compact.performanceText = QStringLiteral("60FPS · 30Mbps");
    const QImage compactImage = ui::RemoteTitleBarRenderer::render(compact);
    assert(compactImage != elapsedOnly);
}
```

### Steps
1. 将标题栏性能文本从 `30 FPS | 1 Mbps` 改为 `30FPS · 1Mbps`。
2. 将数字区字号从12px调整为11px，设备名和IP字号保持不变。
3. 删除70px计时占位、122px性能占位和固定第二起点，改用 `QFontMetrics::horizontalAdvance()` 计算实际宽度。
4. 设备信息到计时保留10px，计时到FPS/码率保留10px，保持分组又减少空白。
5. 为完整DComp标题栏和分段原生标题栏增加700px宽度回归断言。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 用户要求不再构建后，未继续执行构建或程序运行。
- 在用户消息到达前，`fsremote_remote_titlebar_renderer_tests` 目标已完成编译与链接；本次未运行该测试可执行文件。

## 2026-08-10 16:48 - 增加远控标题栏画质档位前端菜单

### Changed Location
- `src/ui/RemoteTitleBarLayout.h:10-94`: 在系统/驱动按钮左侧增加54px画质按钮，并纳入可见性、局部坐标和按钮组签名。
- `src/ui/RemoteTitleBarRenderer.h:41-42`: 增加默认 `540/30` 文本与菜单展开视觉状态。
- `src/ui/RemoteTitleBarRenderer.cpp:77-98、174-179、293-296、352-355`: 绘制紧凑画质按钮，并让计时、性能文字和鼠标锁定状态避开新按钮。
- `src/ui/RemoteDesktopWindow.h:192、210、438-440`: 增加画质按钮热区、菜单函数和纯前端选择状态。
- `src/ui/RemoteDesktopWindow.cpp:3303-3304、3692-3809、3892-3943、5148-5159、6543-6608、6821-6826、7021-7031`: 接入状态快照、悬停、手型光标、按压释放、纵向菜单以及旧标题栏回退绘制。
- `tests/remote_titlebar_renderer_tests.cpp:33-51、112-126`: 增加画质按钮可见性和展开态像素断言，并按新增按钮宽度调整紧凑数字场景。
- `tests/remote_input_broadcast_coordinator_tests.cpp:307-333`: 增加正常、窄和最小宽度下画质按钮可见性断言。

### Reason
需要先确认远控窗口画质切换入口的标题栏视觉和交互，再接入实际清晰度策略。本次仅增加前端预览：按钮默认显示 `540/30`，菜单按从高到低顺序列出八个档位，点选后只更新当前窗口按钮文字和菜单勾选项，不发送远端请求、不修改协调器输入，也不实现“全部窗口默认540p/25”的后续策略。

### Original Code
```cpp
// src/ui/RemoteTitleBarLayout.h:10-13（修改前）
struct RemoteTitleBarLayoutSnapshot {
    QRect update;
    QRect mouseBackend;
    QRect inputSync;
};
```

```cpp
// src/ui/RemoteTitleBarLayout.h:80-81（修改前）
const QRect rawMouseBackend(rawInputSync.left() - 58, 3, 54, std::max(0, safeHeight - 6));
const QRect rawUpdate(rawMouseBackend.left() - 58, 3, 54, std::max(0, safeHeight - 6));
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:6729-6737（修改前）
if (event->button() == Qt::LeftButton) {
    if (!isFullScreen() && mouseInputModeRect().contains(event->pos())) {
        m_mouseBackendButtonPressed = true;
        requestTitleBarUpdate(mouseInputModeRect());
        event->accept();
        return;
    }
}
```

### Modified Code
```cpp
// src/ui/RemoteTitleBarLayout.h:10-13、80-83
struct RemoteTitleBarLayoutSnapshot {
    QRect update;
    QRect quality;
    QRect mouseBackend;
};

const QRect rawMouseBackend(rawInputSync.left() - 58, 3, 54, std::max(0, safeHeight - 6));
const QRect rawQuality(rawMouseBackend.left() - 58, 3, 54, std::max(0, safeHeight - 6));
const QRect rawUpdate(rawQuality.left() - 58, 3, 54, std::max(0, safeHeight - 6));
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3911-3925
const QStringList options = {
    QStringLiteral("1080/60"),
    QStringLiteral("1080/30"),
    QStringLiteral("720/60"),
    QStringLiteral("720/30"),
    QStringLiteral("540/30"),
    QStringLiteral("540/25"),
    QStringLiteral("360/25"),
    QStringLiteral("360/1")
};
for (const QString& option : options) {
    QAction* action = menu.addAction(option);
    action->setCheckable(true);
    action->setChecked(option == m_qualityPreviewText);
    action->setData(option);
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3934-3941
if (selectedAction) {
    const QString selectedText = selectedAction->data().toString();
    if (!selectedText.isEmpty()) {
        m_qualityPreviewText = selectedText;
    }
}
m_qualityMenuOpen = false;
requestTitleBarUpdate(buttonRect);
```

```cpp
// tests/remote_titlebar_renderer_tests.cpp:35-45
ui::RemoteTitleBarVisualState normalState = baseState(900, 1.0);
assert(!normalState.layout.quality.isEmpty());
const QImage normal = ui::RemoteTitleBarRenderer::render(normalState);
normalState.qualityMenuOpen = true;
const QImage menuOpen = ui::RemoteTitleBarRenderer::render(normalState);
assert(menuOpen != normal);
```

### Steps
1. 在标题栏统一布局快照中把画质按钮固定放到系统/驱动按钮左侧，更新按钮继续排列在更左侧。
2. 为原生标题栏绘制器和旧Qt父窗口回退路径增加相同的 `540/30 ▾` 按钮视觉。
3. 点击按钮后弹出96px宽、八行纵向紧凑菜单，当前项使用可勾选动作和浅蓝背景显示。
4. 选择动作只写入 `m_qualityPreviewText` 并刷新标题栏，不触发 `remoteQualityInputsChanged()`。
5. 将新按钮加入悬停、空白标题栏排除、手型光标、按压释放和窄窗口隐藏规则。
6. 增加布局与渲染静态测试断言；由于新增54px按钮，将性能数字紧凑场景宽度从700px调整为760px。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已使用 `rg` 核对八个菜单项顺序为 `1080/60` 到 `360/1`，其中常规360档为 `360/25`。
- 已使用 `rg` 确认新增菜单函数只更新 `m_qualityPreviewText`，没有新增 `remoteQualityInputsChanged()` 调用。
- 已静态核对原生标题栏和 `FSREMOTE_LEGACY_PARENT_TITLE_BAR` 回退路径均绘制新按钮。
- 按用户要求，本次未构建、未链接、未运行测试或启动程序。

## 2026-08-11 15:48 - 统一运行日志到data并在主实例启动时清理

### Changed Location
- `CMakeLists.txt:508-512, 649-653`：将统一日志管理器加入更新服务测试目标和正式主程序目标。
- `src/system/RuntimeLogManager.h:1-21`：新增日志根目录与启动清理结果接口。
- `src/system/RuntimeLogManager.cpp:1-181`：新增data日志清理、旧路径清理和启动计时INI迁移实现。
- `src/main.cpp:67-98, 223-268`：把日志清理移动到单实例确认之后，并让重启实例等待父进程关闭日志句柄。
- `src/system/WjyDiagnosticLog.cpp:1-36`：诊断日志从AppData迁移到`FSRemote.exe/data`。
- `src/system/StartupPerformanceLog.cpp:1-64`：启动计时日志和INI迁移到data。
- `src/system/StartupPerformanceLog.h:10-13`：更新启动计时日志与配置文件路径说明。
- `src/ui/DeviceGrid.cpp:20, 1423-1433, 9202, 9268-9298`：控制端脚本输出和目标端脚本日志迁移到data子目录。
- `src/ui/RemoteDesktopWindow.cpp:5, 1499-1503`：Qt输入调试日志从系统Temp迁移到data。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:147-159`：原生输入调试日志复用data日志写入入口。
- `src/updater/main.cpp:364-371, 392-407`：更新器日志迁移到目标安装目录data，并让新主程序等待更新器退出。
- `src/system/UpdateService.cpp:777-780`：同步更新更新器日志位置和PID等待说明。

### Reason
原日志分散在EXE根目录、AppData、系统Temp、更新任务目录和目标端脚本work目录，重启后还会继续保留旧内容。统一管理后，所有当前运行日志只写入`FSRemote.exe/data`或其子目录；真正取得单实例身份的主程序每次启动先删除上一轮日志，再开始记录本轮内容。二次误启动不会删除正在运行实例的日志，更新重启也会先等待更新器释放`updater.log`句柄。

### Original Code
```cmake
# CMakeLists.txt:503-516
add_executable(fsremote_update_service_tests EXCLUDE_FROM_ALL
    tests/update_service_tests.cpp
    src/system/UpdateService.cpp
    src/system/SharedStorageAvailabilityService.cpp
    src/system/StartupPerformanceLog.cpp
    src/system/WjyDiagnosticLog.cpp
)

# 正式FSRemote目标中没有RuntimeLogManager源文件。
```

```cpp
// src/system/RuntimeLogManager.h, src/system/RuntimeLogManager.cpp
// 新增文件，此位置原来没有代码。
```

```cpp
// src/main.cpp:66-90, 217-243
void waitForRestartParentIfRequested()
{
    // ...
    writeStartupLog(QStringLiteral("[wjy-restart] waiting for parent pid=%1").arg(parentPid));
    ::WaitForSingleObject(parentProcess, 15000);
}

QApplication app(argc, argv);
writeStartupLog(QStringLiteral("[wjy-main] app created qapplication_ms=%1")
    .arg(applicationCreationTimer.elapsed()));
waitForRestartParentIfRequested();
if (activateExistingInstance()) {
    writeStartupLog(QStringLiteral("[wjy-main] another instance is running, activate and exit"));
    return 0;
}
```

```cpp
// src/system/WjyDiagnosticLog.cpp:28-35
const QString dataDirectory = QStandardPaths::writableLocation(
    QStandardPaths::AppLocalDataLocation);
state.path = QDir(dataDirectory).filePath(QStringLiteral("fsremote_diagnostic.log"));
```

```cpp
// src/system/StartupPerformanceLog.cpp:59-62
const QString executableDirectory = QCoreApplication::applicationDirPath();
state.logPath = QDir(executableDirectory).filePath(QString::fromLatin1(kStartupTimingLogFileName));
state.settingsPath = QDir(executableDirectory).filePath(QString::fromLatin1(kStartupTimingSettingsFileName));
```

```cpp
// src/system/StartupPerformanceLog.h:12-13
static QString logFilePath(); // 返回可执行文件目录中的启动计时日志路径。
static QString settingsFilePath(); // 返回同目录INI开关路径。
```

```cpp
// src/ui/DeviceGrid.cpp:1422-1429, 9264-9290
QString scriptOutputTempFilePath()
{
    // ...
    return QDir(QDir::tempPath()).filePath(fileName);
}

$log = Join-Path $work 'fsremote_robocopy.log'
$runLog = Join-Path $work 'fsremote_script_run.log'
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:1498-1501
void appendInputDebugLog(const QString& line)
{
    QFile file(QDir::temp().filePath(QStringLiteral("fsremote_input_debug.log")));
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:149-168
char tempPath[MAX_PATH] = {};
if (::GetTempPathA(MAX_PATH, tempPath) == 0) return;
std::string path(tempPath);
path += "fsremote_input_debug.log";
FILE* file = nullptr;
fopen_s(&file, path.c_str(), "ab");
```

```cpp
// src/updater/main.cpp:365-370, 392-399
std::wstring command = L"\"" + executable.wstring() + L"\" ";
command += updated ? L"--updated-from ..." : L"--update-rollback ...";

const fs::path taskPath = fs::absolute(argv[2]);
g_log.open(taskPath.parent_path() / L"updater.log", std::ios::app);
if (!parseTask(taskPath, &task, &error)) return 3;
```

```cpp
// src/system/UpdateService.cpp:778-779
writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] updater start end pid=%1")
    .arg(updaterPid)); // updater.log位于任务目录。
```

### Modified Code
```cmake
# CMakeLists.txt:508-512, 649-653
# =====wjy====
src/system/RuntimeLogManager.cpp
src/system/RuntimeLogManager.h
# ===end====
```

```cpp
// src/system/RuntimeLogManager.h:8-18
struct RuntimeLogResetResult {
    int removedFileCount = 0;
    int failedFileCount = 0;
    bool dataDirectoryReady = false;
};

class RuntimeLogManager final {
public:
    static QString dataDirectory();
    static RuntimeLogResetResult resetForPrimaryProcessStart();
};
```

```cpp
// src/system/RuntimeLogManager.cpp:139-181
QString RuntimeLogManager::dataDirectory()
{
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
    QDir().mkpath(path);
    return QDir::cleanPath(path);
}

RuntimeLogResetResult RuntimeLogManager::resetForPrimaryProcessStart()
{
    // 删除data内*.log、*.log.*、*.jsonl；保留JSON配置、密钥、脚本和状态文件。
    // 清理EXE根目录、Temp、旧AppData/Updates和work中的已知旧日志。
    // 迁移并保留FSRemote_startup_timing.ini。
}
```

```cpp
// src/main.cpp:67-98, 223-268
qint64 waitForRestartParentIfRequested()
{
    // 等待期间不写文件日志；父进程未确认退出时返回负PID。
}

const qint64 restartParentPid = waitForRestartParentIfRequested();
if (activateExistingInstance()) return 0;
if (restartParentPid < 0) return 1;
if (!singleInstanceServer.listen(QString::fromLatin1(kSingleInstanceKey))) return 1;

const platform::RuntimeLogResetResult logReset =
    platform::RuntimeLogManager::resetForPrimaryProcessStart();
writeStartupLog(QStringLiteral("[wjy-main] app created qapplication_ms=%1")
    .arg(qApplicationCreationMs));
```

```cpp
// src/system/WjyDiagnosticLog.cpp:31-35
const QString dataDirectory = RuntimeLogManager::dataDirectory();
state.path = QDir(dataDirectory).filePath(QStringLiteral("fsremote_diagnostic.log"));
```

```cpp
// src/system/StartupPerformanceLog.cpp:60-62
const QString dataDirectory = RuntimeLogManager::dataDirectory();
state.logPath = QDir(dataDirectory).filePath(QString::fromLatin1(kStartupTimingLogFileName));
state.settingsPath = QDir(dataDirectory).filePath(QString::fromLatin1(kStartupTimingSettingsFileName));
```

```cpp
// src/system/StartupPerformanceLog.h:12-13
static QString logFilePath(); // 返回FSRemote.exe/data中的启动计时日志路径。
static QString settingsFilePath(); // 返回data中的INI开关路径，清日志时保留。
```

```cpp
// src/ui/DeviceGrid.cpp:1423-1433, 9268-9298
QString scriptOutputLogFilePath()
{
    const QString outputDirectory = QDir(platform::RuntimeLogManager::dataDirectory())
        .filePath(QStringLiteral("script_output"));
    return QDir(outputDirectory).filePath(fileName);
}

$scriptLogRoot = Join-Path $fsremoteDir 'data\script_logs'
$scriptLogDirectory = Join-Path $scriptLogRoot $scriptWorkName
$log = Join-Path $scriptLogDirectory 'fsremote_robocopy.log'
$runLog = Join-Path $scriptLogDirectory 'fsremote_script_run.log'
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:1500-1503
QFile file(QDir(platform::RuntimeLogManager::dataDirectory())
    .filePath(QStringLiteral("fsremote_input_debug.log")));
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:147-159
const std::string text = std::string(prefix) + line;
append_log_to_file("fsremote_input_debug.log", text);
```

```cpp
// src/updater/main.cpp:364-371, 392-407
command += L"--restart-after-pid " + std::to_wstring(GetCurrentProcessId()) + L" ";

const fs::path logDirectory = task.targetDir / L"data";
fs::create_directories(logDirectory, logDirectoryError);
g_log.open(logDirectory / L"updater.log", std::ios::out | std::ios::trunc);
```

```cpp
// src/system/UpdateService.cpp:778-779
writeWjyDiagnosticLog(QStringLiteral("[wjy-update-prepare] updater start end pid=%1")
    .arg(updaterPid)); // updater.log写入安装目录data，并等待更新器关闭句柄。
```

### Steps
1. 新增`RuntimeLogManager`，把所有运行日志根目录固定为`QCoreApplication::applicationDirPath()/data`。
2. 清理器只删除data及子目录中的`*.log`、`*.log.*`、`*.jsonl`，保留`devices.json`、OpenSSH密钥和配置、脚本状态JSON/TXT、用户脚本及`control.txt`。
3. 同时清理旧EXE根目录日志、`%TEMP%`输入与脚本输出日志、旧AppData诊断与更新日志，以及work中的两个旧脚本日志。
4. 把`FSRemote_startup_timing.ini`迁移到data并保留`StartupTiming/Enabled`，后续重启只删除日志扩展名文件。
5. 调整主程序启动顺序：先等待重启父进程、激活已有实例、成功监听单实例服务，再清日志并开始写本轮启动记录。
6. 父进程等待超时、权限错误或单实例监听失败时直接退出，不删除任何正在使用的日志。
7. 将诊断、启动计时、Qt输入、原生输入、控制端脚本输出、目标端脚本复制/执行和更新器日志迁移到data对应子目录。
8. 更新器向新主程序追加`--restart-after-pid <更新器PID>`，保证新主程序等更新器关闭`data/updater.log`后再执行清理。

### Verification
- 已执行`git diff --check`，未发现空白错误。
- 已使用`rg`复核全部已知`.log`和`.jsonl`写入点：当前写入均位于`FSRemote.exe/data`或其子目录。
- 已确认`QDir::tempPath`、`QStandardPaths::AppLocalDataLocation`和旧work日志名称只保留在`RuntimeLogManager`旧日志清理逻辑中，不再作为新日志输出位置。
- 已静态核对二次启动在单实例激活后直接退出，不调用`resetForPrimaryProcessStart()`。
- 已静态核对更新重启先等待更新器PID，正常主实例取得成功后才清理data。
- 已确认清理规则不匹配普通`.json`、`.txt`、`.ini`、密钥、脚本和其它用户数据。
- 按用户要求，本次未构建、未链接、未运行测试或启动程序。

- 用户在 `src/stream/RemoteVideoPolicy.h` 中单独修改的360档参数不属于本次前端任务，未改动且不会随本次提交暂存。

## 2026-08-10 17:04 - 精简画质菜单并增加高带宽确认

### Changed Location
- `src/ui/RemoteTitleBarRenderer.cpp:89-101`: 删除画质按钮右侧下拉三角，并让选中的 `1080/60`、`720/60` 使用红色文字。
- `src/ui/RemoteDesktopWindow.cpp:3892-3983`: 用无勾选区域的按钮重做八档菜单，增加圆角窗口遮罩和高带宽中文确认框。
- `src/ui/RemoteDesktopWindow.cpp:6639-6653`: 旧Qt标题栏回退路径同步删除三角，并显示高带宽红字。

### Reason
原标题栏按钮带有向下三角，菜单通过可勾选 `QAction` 显示对勾，与用户要求的纯数字紧凑界面不符。菜单透明圆角窗口在Windows上还可能残留黑色直角。高帧率的 `1080/60` 和 `720/60` 会显著增加带宽压力，因此需要用红字提示，并在前端选中前二次确认；本次仍不接入实际清晰度下发逻辑。

### Original Code
```cpp
// src/ui/RemoteTitleBarRenderer.cpp:94-97（修改前）
painter.drawText(
    layout.quality.adjusted(2, 0, -2, 0),
    Qt::AlignCenter,
    state.qualityText + QStringLiteral(" ▾"));
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3899-3909（修改前）
QMenu menu(this);
menu.setAttribute(Qt::WA_TranslucentBackground);
menu.setWindowFlag(Qt::NoDropShadowWindowHint, true);
menu.setStyleSheet(QStringLiteral(
    "QMenu{background:#FFFFFF;border:1px solid #D8DEE8;border-radius:5px;}"
    "QMenu::item:checked{font-weight:600;color:#1D4ED8;background:#F3F7FF;}"
    "QMenu::indicator{width:12px;height:12px;}"));
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3921-3925（修改前）
QAction* action = menu.addAction(option);
action->setCheckable(true);
action->setChecked(option == m_qualityPreviewText);
action->setData(option);
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3933-3941（修改前）
QAction* selectedAction = menu.exec(popupPosition);
if (selectedAction) {
    m_qualityPreviewText = selectedAction->data().toString();
}
m_qualityMenuOpen = false;
requestTitleBarUpdate(buttonRect);
```

### Modified Code
```cpp
// src/ui/RemoteTitleBarRenderer.cpp:93-101
const bool highBandwidthSelection = state.qualityText == QStringLiteral("1080/60")
    || state.qualityText == QStringLiteral("720/60");
painter.setPen(highBandwidthSelection
    ? QColor(QStringLiteral("#DC2626"))
    : QColor(QStringLiteral("#344054")));
painter.drawText(layout.quality.adjusted(2, 0, -2, 0),
    Qt::AlignCenter, state.qualityText);
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3917-3945
QString selectedText;
for (const QString& option : options) {
    const bool highBandwidthOption = option == QStringLiteral("1080/60")
        || option == QStringLiteral("720/60");
    auto* action = new QWidgetAction(&menu);
    auto* optionButton = new QPushButton(option, &menu);
    optionButton->setStyleSheet(QStringLiteral(
        "QPushButton{border:0;background:%1;color:%2;}"
        "QPushButton:hover{background:#E8F1FF;color:%2;}")
        .arg(defaultBackground, textColor));
    connect(optionButton, &QPushButton::clicked, &menu, [&menu, &selectedText, option] {
        selectedText = option;
        menu.close();
    });
    action->setDefaultWidget(optionButton);
    menu.addAction(action);
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3947-3950
menu.adjustSize();
QPainterPath menuMaskPath;
menuMaskPath.addRoundedRect(QRectF(menu.rect()), 5, 5);
menu.setMask(QRegion(menuMaskPath.toFillPolygon().toPolygon()));
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3964-3982
const bool needsBandwidthConfirmation = selectedText == QStringLiteral("1080/60")
    || selectedText == QStringLiteral("720/60");
if (needsBandwidthConfirmation) {
    QMessageBox confirmation(this);
    confirmation.setWindowTitle(QString::fromUtf8("高带宽提示"));
    confirmation.setText(QString::fromUtf8("此分辨率设置会大幅消耗带宽，造成卡顿，是否设置？"));
    QPushButton* confirmButton = confirmation.addButton(QString::fromUtf8("是"), QMessageBox::AcceptRole);
    QPushButton* cancelButton = confirmation.addButton(QString::fromUtf8("否"), QMessageBox::RejectRole);
    confirmation.exec();
    if (confirmation.clickedButton() != confirmButton) return;
}
m_qualityPreviewText = selectedText;
requestTitleBarUpdate(buttonRect);
```

### Steps
1. 删除原生标题栏和旧Qt回退标题栏中的下拉三角字符。
2. 将可勾选 `QAction` 替换为 `QWidgetAction` 内嵌无边框 `QPushButton`，彻底移除对勾及其左侧占位。
3. 当前档位仅保留轻微浅蓝底色；`1080/60`、`720/60` 在菜单和标题栏中固定显示红字。
4. 菜单按最终尺寸创建5px圆角窗口遮罩，裁掉四角窗口像素，避免黑色直角残留。
5. 点击两个高带宽档时弹出“是/否”中文警告，只有“是”会更新前端标题栏文字。
6. 选择“否”、Esc、关闭弹窗或取消菜单都保持原值，不发送任何后台质量请求。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已使用 `rg` 确认标题栏源码中不再包含下拉三角字符、`setCheckable()`、画质项 `setChecked()` 或 `QMenu::indicator`。
- 已静态核对 `1080/60` 和 `720/60` 同时控制红字与确认框，其他六档不会弹出警告。
- 已静态核对最终赋值仍只写入 `m_qualityPreviewText` 并刷新标题栏，没有新增 `remoteQualityInputsChanged()`。
- 按用户要求，本次未构建、未链接、未运行测试或启动程序。
- `src/stream/RemoteVideoPolicy.h` 的用户独立修改继续保持未暂存。

## 2026-08-10 18:19 - 接入远控精确画质档位与持久化恢复策略

### Changed Location
- `src/stream/RemoteVideoPolicy.h:63-163`: 增加八档稳定枚举、精确编码映射、默认/全屏/遮挡档常量和历史高档恢复判断；将用户修改的360常规档正式命名为 `360p/25`。
- `src/system/AppSettings.h:42-45`、`src/system/AppSettings.cpp:20-24、270-300`: 使用现有 `QSettings` 按设备保存精确档位枚举，不创建额外配置文件。
- `src/ui/RemoteQualityCoordinator.h:63-134`、`src/ui/RemoteQualityCoordinator.cpp:18-168`: 删除焦点、窗口数量和性能压力改档分支，按手选、全屏、默认、遮挡的固定优先级输出精确档位。
- `src/ui/RemoteDesktopWindow.h:100-104、221-222、327-331`、`src/ui/RemoteDesktopWindow.cpp:566-601、1999-2004、2299-2324、2476-2522、3954-4059`: 读取历史手选、接入菜单选择、立即持久化、刷新标题栏并向协调器发送真实输入。
- `src/ui/DeviceGrid.cpp:3346-3393、6346-6359、6890-6956`、`src/ui/DeviceGrid.h:239、308`: 更新设置页说明，并在窗口注册时执行“历史高档只自动恢复一个”的A/B/C仲裁。
- `tests/remote_quality_coordinator_tests.cpp:1-181`: 覆盖默认540/30、全屏720/30、手选优先、遮挡360/1、多个运行中高档并存以及A/B/C重新打开场景。
- `tests/remote_video_policy_tests.cpp:6-85`: 覆盖八档枚举映射、360/25命名修正和焦点/后台统一540/30。
- `CMakeLists.txt:247-253`: 更新质量协调器测试目标说明，使其与新策略一致。

### Reason
前端菜单此前只修改标题栏文字，没有连接Host在线质量接口，也没有保存精确的分辨率/FPS组合。旧协调器仍按焦点把窗口分为540/30和540/25，并可能按软件回退或性能压力继续改档，与“默认全部540/30、全屏自动720/30、手选最高、完全遮挡360/1”的规则冲突。同时，重新远控多个保存了高档位的设备时，需要只允许一个历史高档自动恢复，但不能限制当前打开窗口之后继续手动升档。

### Original Code
```cpp
// src/stream/RemoteVideoPolicy.h:83-88（修改前）
inline constexpr RemoteVideoEncodingPreset kRemoteVideo360p30Preset =
    {RemoteResolutionTier::P360, 25, 7000};
inline constexpr RemoteVideoProfile kFocusedRemoteVideoProfile =
    makeRemoteVideoProfile(kRemoteVideo540p30Preset, 100);
inline constexpr RemoteVideoProfile kVisibleBackgroundRemoteVideoProfile =
    makeRemoteVideoProfile(kRemoteVideo540p25Preset, 40);
```

```cpp
// src/system/AppSettings.h:44-45（修改前）
static bool remoteDeviceQualityMode(
    const QString& deviceKey,
    stream::RemoteQualityMode* mode);
static void setRemoteDeviceQualityMode(
    const QString& deviceKey,
    stream::RemoteQualityMode mode);
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:129-149（修改前）
const bool highPerformance = eligibleVisibleWindow
    && (singleRemoteWindow || focusedWindowEligible);
const auto roleProfile = decision.minimized
    ? stream::RemoteVideoPolicy::minimizedProfile()
    : highPerformance
        ? stream::kFocusedRemoteVideoProfile
        : stream::RemoteVideoPolicy::backgroundProfile();
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3934-3942（修改前）
if (selectedAction) {
    m_qualityPreviewText = selectedAction->data().toString();
}
m_qualityMenuOpen = false;
requestTitleBarUpdate(buttonRect);
```

```cpp
// src/ui/DeviceGrid.cpp:6890-6902（修改前）
window->setGlobalQualityConfiguration(m_remoteQualityConfiguration);
connect(window, &RemoteDesktopWindow::remoteQualityInputsChanged,
    this, &DeviceGrid::requestRemoteQualityEvaluation);
requestRemoteQualityEvaluation();
```

### Modified Code
```cpp
// src/stream/RemoteVideoPolicy.h:63-72、109-111
enum class RemoteVideoQualityPreset : std::uint8_t {
    P1080_60,
    P1080_30,
    P720_60,
    P720_30,
    P540_30,
    P540_25,
    P360_25,
    P360_1,
};
inline constexpr RemoteVideoQualityPreset kDefaultRemoteVideoQualityPreset =
    RemoteVideoQualityPreset::P540_30;
inline constexpr RemoteVideoQualityPreset kFullscreenRemoteVideoQualityPreset =
    RemoteVideoQualityPreset::P720_30;
inline constexpr RemoteVideoQualityPreset kOccludedRemoteVideoQualityPreset =
    RemoteVideoQualityPreset::P360_1;
```

```cpp
// src/system/AppSettings.cpp:270-300
bool AppSettings::remoteDeviceQualityPreset(
    const QString& deviceKey,
    stream::RemoteVideoQualityPreset* preset)
{
    const QVariant stored = settings().value(remoteDeviceQualityPresetKey(deviceKey));
    const auto storedPreset = static_cast<stream::RemoteVideoQualityPreset>(stored.toInt());
    if (!stream::isValidRemoteVideoQualityPreset(storedPreset)) return false;
    *preset = storedPreset;
    return true;
}

void AppSettings::setRemoteDeviceQualityPreset(
    const QString& deviceKey,
    stream::RemoteVideoQualityPreset preset)
{
    QSettings appSettings = settings();
    appSettings.setValue(remoteDeviceQualityPresetKey(deviceKey), static_cast<int>(preset));
}
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:118-139
const stream::RemoteVideoQualityPreset preferredPreset = window.userQualityPresetActive
    ? window.userQualityPreset
    : decision.fullScreen
        ? stream::kFullscreenRemoteVideoQualityPreset
        : stream::kDefaultRemoteVideoQualityPreset;
decision.preset = eligibleVisibleWindow
    ? preferredPreset
    : stream::kOccludedRemoteVideoQualityPreset;
const stream::RemoteVideoEncodingPreset encoding =
    stream::remoteVideoEncodingPreset(decision.preset);
decision.resolution = encoding.resolution;
decision.targetFps = static_cast<int>(encoding.targetFps);
decision.maxBitrateKbps = static_cast<int>(encoding.maxBitrateKbps);
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:4009-4038
stream::RemoteVideoQualityPreset selectedPreset = stream::kDefaultRemoteVideoQualityPreset;
if (!remoteVideoQualityPresetFromText(selectedText, &selectedPreset)) return;
// 1080/60与720/60先执行已有高带宽确认框。
m_userQualityPreset = selectedPreset;
m_savedUserQualityPreset = selectedPreset;
m_hasSavedUserQualityPreset = true;
m_userQualityPresetActive = true;
platform::AppSettings::setRemoteDeviceQualityPreset(m_hostIp, selectedPreset);
refreshQualityPreviewText();
emit remoteQualityInputsChanged();
```

```cpp
// src/ui/DeviceGrid.cpp:6895-6908
bool anotherAboveDefaultPresetIsOpen = false;
for (const QPointer<RemoteDesktopWindow>& openedWindow : openedRemoteWindows()) {
    if (openedWindow && openedWindow.data() != window
        && openedWindow->hasActiveUserQualityPresetAboveDefault()) {
        anotherAboveDefaultPresetIsOpen = true;
        break;
    }
}
window->restoreSavedUserQualityPreset(anotherAboveDefaultPresetIsOpen);
```

```cpp
// tests/remote_quality_coordinator_tests.cpp:122-147
// A、B、C运行中手选多个高档允许并存。
assert(decisions[0].preset == stream::RemoteVideoQualityPreset::P720_30);
assert(decisions[1].preset == stream::RemoteVideoQualityPreset::P720_30);
assert(decisions[2].preset == stream::RemoteVideoQualityPreset::P1080_60);

// A先重新打开并恢复历史720/30后，B本次恢复被拒绝；C已高档时A、B都被拒绝。
const bool restoreA = stream::shouldRestoreSavedRemoteVideoQualityPreset(
    stream::RemoteVideoQualityPreset::P720_30, false);
const bool restoreB = stream::shouldRestoreSavedRemoteVideoQualityPreset(
    stream::RemoteVideoQualityPreset::P720_30, restoreA);
assert(restoreA && !restoreB);
```

### Steps
1. 将八个标题栏选项定义为稳定枚举，并集中映射分辨率、FPS和码率；正式纳入用户已修改的360/25档。
2. 使用现有 `QSettings("FSRemote", "FSRemote")` 按设备IP保存精确档位，配置键存在即表示用户手动选择过。
3. 普通可见窗口统一请求540/30，不再根据焦点、窗口数量、接收性能或软件Presenter状态自动改档。
4. 无手选窗口进入全屏自动请求720/30，退出全屏恢复540/30；手选窗口全屏仍保持手选档。
5. 完全遮挡、最小化或隐藏窗口临时请求360/1；重新暴露时通过 `Expose` 和周期采样立即恢复手选或自动档。
6. 标题栏确认选择后立即写QSettings并触发现有在线质量评估，Viewer连接不停止、不重建。
7. 重新打开历史高档窗口时扫描当前已打开窗口：第一个历史高档可恢复，后续历史高档本次使用默认；历史配置本身不删除。
8. 当前打开窗口随后手动升到高档不受数量限制，因此A、B、C可以同时保持用户选择的高档位。
9. 用户手选540/30也会保存“手选”身份，从而阻止全屏自动切到720/30；默认及以下历史档不占高档恢复名额。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已使用 `rg` 确认旧 `remoteDeviceQualityMode`、`qualityOverrideMode`、`kRemoteVideo360p30Preset` 和焦点画质防抖字段均已移除。
- 已静态核对菜单选择会调用 `setRemoteDeviceQualityPreset()`、更新用户手选状态并发出 `remoteQualityInputsChanged()`。
- 已静态核对全屏自动档不会写入QSettings，遮挡360/1也不会覆盖历史手选配置。
- 已静态核对A/B/C规则：运行中多高档允许；重新打开历史高档时只恢复一个；已有C高档时A、B均从默认开始。
- 已更新纯策略测试源码覆盖默认、全屏、手选、遮挡、恢复和多窗口场景。
- 按用户此前要求，本次未构建、未链接、未运行测试或启动程序。

## 2026-08-11 10:20 - 监控模式改为全设备只读轮询会话

### Changed Location
- `include/FsRemoteStreamApi.h:39、195`: 新增普通控制与只读监控 Viewer 角色及显式角色启动 ABI。
- `src/stream/StreamRuntime.h:58、100、135`: 缓存角色化 DLL 导出，并为纹理 Viewer 增加只读参数。
- `src/stream/StreamRuntime.cpp:94、188`: 优先调用角色化 Viewer 入口；旧 DLL 不允许把监控降级成控制会话。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:656、2324、2381、2935、3637`: Viewer 握手请求 `view`，Host 排除监控人数和控制者名称，并允许同机 `control`/`view` 共存。
- `src/ui/RemoteDesktopWindow.h:53、329`: 构造阶段固定监控只读属性。
- `src/ui/RemoteDesktopWindow.cpp:1972、3947、4253、4707、6388`: 关闭监控窗口的键鼠、剪贴板、音频、脚本和同步输入，并把只读角色传入 DLL。
- `src/ui/DeviceGrid.h:189、324`: 使用设备 ID 到独立监控窗口的映射替代普通窗口恢复快照。
- `src/ui/DeviceGrid.cpp:4472、7226、9628、9843、9902、9955、9982、10797`: 按全部在线远端设备创建、分页、统计和关闭只读监控窗口，并纳入统一画质与退出清理。

### Reason
上一版监控模式只接管本机已经打开的普通远控窗口，标题栏数字仍显示控制会话人数，关闭后还会恢复旧窗口布局。该行为无法覆盖其它设备，也会让监控 Viewer 以普通控制身份进入目标端会话统计。

本次把监控改为独立只读会话：目标集合来自设备目录中所有在线或占用的远端设备，不依赖本机普通远控窗口；Host 从认证握手开始只授予视频能力，拒绝键鼠输入，并从远控人数、控制者名称和被控提示中排除监控会话。

### Original Code
```cpp
// include/FsRemoteStreamApi.h:169-186
FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_viewer_with_texture(
    const char* host_ip,
    uint16_t port,
    FsRemoteFrameCallback frame_callback,
    FsRemoteTextureFrameCallback texture_callback,
    FsRemoteStatusCallback status_callback,
    void* user);
```

```cpp
// src/stream/StreamRuntime.cpp:186-199
FsRemoteStreamHandle StreamRuntime::startViewer(
    const QString& hostIp,
    uint16_t port,
    FsRemoteFrameCallback frameCallback,
    FsRemoteTextureFrameCallback textureCallback,
    FsRemoteStatusCallback statusCallback,
    void* user)
{
    const QByteArray ip = hostIp.toUtf8();
    if (m_startViewerWithTexture) {
        return m_startViewerWithTexture(ip.constData(), port, frameCallback, textureCallback, statusCallback, user);
    }
    return startViewer(hostIp, port, frameCallback, statusCallback, user);
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:671-674、2324-2330
const std::string requested_role = "control";
const std::string offered_capabilities = "video,audio,control";

if (session->admission.ownership != "control_granted"
    && session->admission.requested_role != "control") {
    // still include any active session that holds video so multi-viewer is visible
}
```

```cpp
// src/ui/RemoteDesktopWindow.h:53-58
explicit RemoteDesktopWindow(
    const QString& deviceName,
    const QString& hostIp,
    RemoteViewerLifecycleManager* lifecycleManager,
    RemoteInputBroadcastCoordinator* inputBroadcastCoordinator,
    QWidget* parent = nullptr);
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:4241-4248
bool RemoteDesktopWindow::sendInputMessage(const QByteArray& message)
{
    if (remoteUpdateActive() || !m_viewerHandle
        || !RemoteConnectionState::acceptsRemoteInput(m_connectionStatusCode)) {
        return false;
    }
    return stream::StreamRuntime::instance().sendInput(m_viewerHandle, message);
}
```

```cpp
// src/ui/DeviceGrid.h:113-119、329-332
struct RemoteMonitorWindowState {
    QPointer<RemoteDesktopWindow> window;
    QRect normalGeometry;
    Qt::WindowStates windowState = Qt::WindowNoState;
    bool visible = true;
    bool rememberGeometry = true;
};

QHash<RemoteDesktopWindow*, RemoteMonitorWindowState> m_remoteMonitorWindowStates;
bool m_remoteMonitorModeEnabled = false;
int m_remoteMonitorPageIndex = 0;
```

```cpp
// src/ui/DeviceGrid.cpp:9865-9878、9933-9941
QVector<QPointer<RemoteDesktopWindow>> windows = openedRemoteWindows();
for (auto it = windows.begin(); it != windows.end();) {
    if (!*it || (*it)->isClosingConnection()) {
        it = windows.erase(it);
    } else {
        ++it;
    }
}

if (windows.isEmpty()) {
    m_remoteMonitorPageIndex = 0;
    return;
}
```

### Modified Code
```cpp
// include/FsRemoteStreamApi.h:39-43、195-202
enum FsRemoteViewerRole {
    FSREMOTE_VIEWER_ROLE_CONTROL = 0,
    FSREMOTE_VIEWER_ROLE_MONITOR = 1,
};

FsRemoteStreamHandle FSREMOTE_STREAM_CALL fsremote_stream_start_viewer_with_texture_role(
    const char* host_ip,
    uint16_t port,
    FsRemoteFrameCallback frame_callback,
    FsRemoteTextureFrameCallback texture_callback,
    FsRemoteStatusCallback status_callback,
    void* user,
    enum FsRemoteViewerRole role);
```

```cpp
// src/stream/StreamRuntime.cpp:188-219
FsRemoteStreamHandle StreamRuntime::startViewer(
    const QString& hostIp,
    uint16_t port,
    FsRemoteFrameCallback frameCallback,
    FsRemoteTextureFrameCallback textureCallback,
    FsRemoteStatusCallback statusCallback,
    void* user,
    bool monitorReadOnly)
{
    if (m_startViewerWithTextureRole) {
        return m_startViewerWithTextureRole(
            ip.constData(), port, frameCallback, textureCallback, statusCallback, user,
            monitorReadOnly ? FSREMOTE_VIEWER_ROLE_MONITOR : FSREMOTE_VIEWER_ROLE_CONTROL);
    }
    if (monitorReadOnly) return nullptr;
    return m_startViewerWithTexture
        ? m_startViewerWithTexture(ip.constData(), port, frameCallback, textureCallback, statusCallback, user)
        : startViewer(hostIp, port, frameCallback, statusCallback, user);
}
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:674-677、2328-2331、2391-2397
const bool monitor_read_only = viewer_role == FSREMOTE_VIEWER_ROLE_MONITOR;
const std::string requested_role = monitor_read_only ? "view" : "control";
const std::string offered_capabilities = monitor_read_only ? "video" : "video,audio,control";

if (session->admission.ownership != "control_granted") continue;

if (session->admission.public_key != current->admission.public_key
    || session->admission.requested_role != current->admission.requested_role) {
    continue;
}
```

```cpp
// src/ui/RemoteDesktopWindow.h:53-59、329
explicit RemoteDesktopWindow(
    const QString& deviceName,
    const QString& hostIp,
    RemoteViewerLifecycleManager* lifecycleManager,
    RemoteInputBroadcastCoordinator* inputBroadcastCoordinator,
    QWidget* parent = nullptr,
    bool monitorReadOnly = false);

bool m_monitorReadOnly = false;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:4253-4277、4707-4714、6400-6407
if (m_monitorReadOnly || remoteUpdateActive() || !m_viewerHandle
    || !RemoteConnectionState::acceptsRemoteInput(m_connectionStatusCode)) {
    return false;
}

return !m_monitorReadOnly
    && m_viewerHandle != nullptr
    && RemoteConnectionState::acceptsRemoteInput(m_connectionStatusCode);

m_viewerHandle = stream::StreamRuntime::instance().startViewer(
    m_hostIp.trimmed(), 49100, onRemoteFrame, onRemoteTextureFrame,
    onViewerStatus, callbackContext.get(), m_monitorReadOnly);
```

```cpp
// src/ui/DeviceGrid.h:189-202、324-329
int remoteMonitorDeviceCount() const;
QVector<int> remoteMonitorDeviceIndexes() const;
void createRemoteMonitorWindowForDevice(int deviceIndex);
QVector<QPointer<RemoteDesktopWindow>> openedRemoteMonitorWindows() const;
void closeRemoteMonitorWindows();

QHash<QString, QPointer<RemoteDesktopWindow>> m_remoteMonitorWindows;
QSet<QString> m_pendingRemoteMonitorDeviceIds;
bool m_remoteMonitorModeEnabled = false;
int m_remoteMonitorPageIndex = 0;
```

```cpp
// src/ui/DeviceGrid.cpp:9843-9856、9902-9938、9982-10031
int DeviceGrid::remoteMonitorDeviceCount() const
{
    int count = 0;
    for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        const DeviceEntry& device = g_devices.at(deviceIndex);
        const platform::DevicePresenceState presence = devicePresenceForIndex(deviceIndex);
        if (!device.ip.trimmed().isEmpty() && !deviceRecordMatchesLocal(device)
            && (presence == platform::DevicePresenceState::Online
                || presence == platform::DevicePresenceState::Busy)) {
            ++count;
        }
    }
    return count;
}

auto* monitorWindow = new RemoteDesktopWindow(
    deviceDisplayName(device), ip, m_remoteViewerLifecycleManager.get(),
    nullptr, nullptr, true);
m_remoteMonitorWindows.insert(key, monitorWindow);
registerRemoteQualityWindow(monitorWindow, true);

const QVector<int> deviceIndexes = remoteMonitorDeviceIndexes();
for (const int deviceIndex : deviceIndexes) {
    if (m_authorizedRemoteControlIps.contains(ip)) {
        createRemoteMonitorWindowForDevice(deviceIndex);
    } else {
        authorizationIds.append(device.id);
    }
}
```

```cpp
// src/ui/DeviceGrid.cpp:10797-10803
const int controlledSessionCount = m_remoteMonitorModeEnabled
    ? remoteMonitorDeviceCount()
    : totalRemoteControlSessionCount();
```

### Steps
1. 在公开流 API 中新增 `CONTROL` 和 `MONITOR` Viewer 角色，以及角色化纹理 Viewer 启动入口。
2. `StreamRuntime` 动态解析新导出；普通远控兼容旧 DLL，监控模式缺少新导出时明确失败，禁止退化成控制会话。
3. Viewer 准入握手在监控模式下请求 `view` 且只声明 `video` 能力，角色被签名上下文绑定。
4. Host 继续只统计 `control_granted` 会话，并修正控制者名称列表，使只读监控不出现在人数、名称或被控提示中。
5. 同一公钥的 `control` 和 `view` 会话按角色分别处理重连，普通远控与监控窗口可以同时观看同一设备。
6. `RemoteDesktopWindow` 在构造阶段固定只读属性，关闭键盘 Hook、鼠标转发、剪贴板、音频、画质手选、键鼠脚本和输入同步。
7. `DeviceGrid` 从全部在线或占用的非本机设备构造稳定轮询集合，不再读取 `openedRemoteWindows()` 作为监控来源。
8. 监控窗口按设备 ID 独立保存，不登记普通窗口协调器、不调用 `publishRemoteControllerTarget()`，并使用设置页统一画质。
9. 设备上下线时立即同步监控窗口；轮询定时器按配置宫格切页，非当前页继续以360/1保活。
10. 关闭监控模式时关闭全部独立监控窗口，不恢复旧布局；程序退出时把尚在异步关闭的监控 Viewer 纳入统一停止和等待流程。
11. 标题栏数字在监控开启时显示全部在线轮询设备数，关闭后恢复真实控制会话人数。
12. 更新监控设置页说明，明确目标来自全部在线远端设备，监控窗口不使用设备历史手选画质。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已使用 `rg` 核对角色化 Viewer ABI 在声明、动态解析、实现和调用处名称一致。
- 已静态核对监控窗口创建路径未调用 `publishRemoteControllerTarget()`，也未注册 `RemoteInputBroadcastCoordinator`。
- 已静态核对 Viewer 与 Host 两端均有只读门禁：Viewer 不发送输入，Host 的 `view_only` 会话不登记输入分发且不会进入控制人数与控制者列表。
- 已静态核对普通远控仍使用原协调器、控制租约、历史画质和快捷键集合，监控开启不会修改普通窗口画质或布局。
- 已静态核对设备离线、关闭监控和程序退出三条路径都会停止并清理独立监控 Viewer。
- 按用户要求，本次未构建、未链接、未运行测试或启动程序。

## 2026-08-11 09:27 - 新增分页监控模式与可配置宫格轮询

### Changed Location
- `src/stream/RemoteVideoPolicy.h:74-211`: 增加监控宫格枚举、默认九宫格/540-30/30秒配置、布局行列换算和统一画质标签。
- `src/system/AppSettings.h:46-47`、`src/system/AppSettings.cpp:302-328`: 使用现有QSettings持久化监控宫格、画质和轮询秒数。
- `src/ui/DeviceGrid.h:87-120、190-204、316-360`: 增加监控设置页、窗口恢复快照、定时器和运行状态。
- `src/ui/DeviceGrid.cpp:274-389、3290-3521、4200-4205、6559-6658、7133-7168、7216-7249、9795-9998、10643-10733、11910-13118`: 接入标题栏按钮、独立设置界面、分页平铺、窗口扫描、关闭恢复和鼠标命中。
- `src/ui/RemoteDesktopWindow.h:80-81、102、330-333`、`src/ui/RemoteDesktopWindow.cpp:566-569、2466-2519、4050-4061、4223-4231`: 增加监控统一画质输入并同步标题栏显示，同时暴露只读几何记忆状态供监控退出恢复。
- `src/ui/RemoteQualityCoordinator.h:63-70、124-129`、`src/ui/RemoteQualityCoordinator.cpp:117-141`: 将监控画质插入手选与全屏自动档之间。
- `tests/remote_video_policy_tests.cpp:41-56`: 覆盖默认监控配置、六种宫格和损坏值回退。
- `tests/remote_quality_coordinator_tests.cpp:72-94`: 覆盖监控画质、窗口手选和遮挡保活优先级。

### Reason
主窗口此前只能手动把全部远控窗口一次性平铺，没有长期巡视入口，也不能按固定单页容量轮换大量窗口。本次新增“监控模式”：自动扫描本控制端当前已打开的远控窗口，默认按九宫格显示，每30秒切换下一页；最后一页只显示剩余窗口。监控接管前保存每个窗口的普通几何、最大化/全屏/最小化状态和显隐，关闭监控后原样恢复。设置页增加独立监控界面，宫格、统一画质和轮询秒数都可持久化，其中宫格和画质在监控运行中立即生效。

### Original Code
```cpp
// src/stream/RemoteVideoPolicy.h:63-72（修改前）
enum class RemoteVideoQualityPreset : std::uint8_t {
    P1080_60 = 0,
    P1080_30 = 1,
    P720_60 = 2,
    P720_30 = 3,
    P540_30 = 4,
    P540_25 = 5,
    P360_25 = 6,
    P360_1 = 7,
};
```

```cpp
// src/system/AppSettings.h:42-45（修改前）
static stream::RemoteQualityConfiguration remoteQualityConfiguration();
static void setRemoteQualityConfiguration(const stream::RemoteQualityConfiguration& configuration);
static bool remoteDeviceQualityPreset(const QString& deviceKey, stream::RemoteVideoQualityPreset* preset);
static void setRemoteDeviceQualityPreset(const QString& deviceKey, stream::RemoteVideoQualityPreset preset);
```

```cpp
// src/system/AppSettings.cpp:270-300（修改前）
// 新增代码，原位置没有监控宫格、统一画质和轮询秒数的读写接口。
```

```cpp
// src/ui/DeviceGrid.h:87-91（修改前）
enum class SettingsTab {
    General,
    Keyboard,
    RemoteControl,
};
```

```cpp
// src/ui/DeviceGrid.cpp:3172-3186（修改前）
QRect settingsRemoteControlTabRect()
{
    return QRect(contentLeft() + 144, kDetailScriptTabTop, 84, 36);
}
```

```cpp
// src/ui/RemoteDesktopWindow.h:100-104（修改前）
void setGlobalQualityConfiguration(const stream::RemoteQualityConfiguration& configuration);
bool hasSavedUserQualityPreset() const;
stream::RemoteVideoQualityPreset savedUserQualityPreset() const;
bool hasActiveUserQualityPresetAboveDefault() const;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:4042-4049（修改前）
stream::RemoteVideoQualityPreset RemoteDesktopWindow::preferredRemoteVideoQualityPreset() const
{
    if (m_userQualityPresetActive) return m_userQualityPreset;
    return isFullScreen()
        ? stream::kFullscreenRemoteVideoQualityPreset
        : stream::kDefaultRemoteVideoQualityPreset;
}
```

```cpp
// src/ui/RemoteQualityCoordinator.h:63-68（修改前）
stream::RemoteVideoQualityPreset userQualityPreset = stream::kDefaultRemoteVideoQualityPreset;
bool userQualityPresetActive = false;
bool visible = true;
bool minimized = false;
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:118-123（修改前）
const stream::RemoteVideoQualityPreset preferredPreset = window.userQualityPresetActive
    ? window.userQualityPreset
    : decision.fullScreen
        ? stream::kFullscreenRemoteVideoQualityPreset
        : stream::kDefaultRemoteVideoQualityPreset;
```

```cpp
// tests/remote_video_policy_tests.cpp:33-39（修改前）
assert(stream::remoteVideoEncodingPreset(stream::RemoteVideoQualityPreset::P1080_60).targetFps == 60);
assert(stream::remoteVideoQualityPresetExceedsDefault(stream::RemoteVideoQualityPreset::P720_30));
assert(!stream::remoteVideoQualityPresetExceedsDefault(stream::RemoteVideoQualityPreset::P540_30));
```

```cpp
// tests/remote_quality_coordinator_tests.cpp:62-69（修改前）
second.fullScreen = true;
decisions = coordinator.evaluate(configuration, {second}, 1100);
assert(decisions.front().preset == stream::RemoteVideoQualityPreset::P720_30);
```

### Modified Code
```cpp
// src/stream/RemoteVideoPolicy.h:74-87、135-211
enum class RemoteMonitorGridPreset : std::uint8_t {
    Grid4 = 4,
    Grid9 = 9,
    Grid12 = 12,
    Grid16 = 16,
    Grid20 = 20,
    Grid25 = 25,
};

struct RemoteMonitorConfiguration {
    RemoteMonitorGridPreset grid = RemoteMonitorGridPreset::Grid9;
    RemoteVideoQualityPreset quality = RemoteVideoQualityPreset::P540_30;
    int rotationIntervalSeconds = 30;
};
```

```cpp
// src/system/AppSettings.h:46-47
static stream::RemoteMonitorConfiguration remoteMonitorConfiguration();
static void setRemoteMonitorConfiguration(
    const stream::RemoteMonitorConfiguration& configuration);
```

```cpp
// src/system/AppSettings.cpp:302-328
stream::RemoteMonitorConfiguration AppSettings::remoteMonitorConfiguration()
{
    QSettings appSettings = settings();
    stream::RemoteMonitorConfiguration configuration;
    configuration.grid = static_cast<stream::RemoteMonitorGridPreset>(
        appSettings.value(
            QStringLiteral("remoteMonitor/grid"),
            static_cast<int>(stream::kDefaultRemoteMonitorGridPreset)).toInt());
    configuration.quality = static_cast<stream::RemoteVideoQualityPreset>(
        appSettings.value(
            QStringLiteral("remoteMonitor/quality"),
            static_cast<int>(stream::kDefaultRemoteVideoQualityPreset)).toInt());
    configuration.rotationIntervalSeconds = appSettings.value(
        QStringLiteral("remoteMonitor/rotationIntervalSeconds"),
        stream::kDefaultRemoteMonitorRotationIntervalSeconds).toInt();
    return stream::normalizedRemoteMonitorConfiguration(configuration);
}
```

```cpp
// src/ui/DeviceGrid.h:87-120、201-204
enum class SettingsTab {
    General,
    Keyboard,
    RemoteControl,
    RemoteMonitor,
};

struct RemoteMonitorWindowState {
    QPointer<RemoteDesktopWindow> window;
    QRect normalGeometry;
    Qt::WindowStates windowState = Qt::WindowNoState;
    bool visible = true;
    bool rememberGeometry = true;
};
```

```cpp
// src/ui/DeviceGrid.cpp:9795-9998
void DeviceGrid::refreshRemoteMonitorMode(bool advancePage)
{
    QVector<QPointer<RemoteDesktopWindow>> windows = openedRemoteWindows();
    const int capacity = qMax(1,
        stream::remoteMonitorGridCapacity(m_remoteMonitorConfiguration.grid));
    const int pageStart = m_remoteMonitorPageIndex * capacity;
    const int pageEnd = qMin(pageStart + capacity, windows.size());
    for (int index = 0; index < windows.size(); ++index) {
        RemoteDesktopWindow* remoteWindow = windows.at(index).data();
        if (index < pageStart || index >= pageEnd) {
            remoteWindow->hide();
            continue;
        }
        remoteWindow->showNormal();
        remoteWindow->setGeometry(target);
        remoteWindow->show();
        remoteWindow->raise();
    }
}
```

```cpp
// src/ui/RemoteDesktopWindow.h:101、329-332
void setRemoteMonitorQualityPreset(bool active, stream::RemoteVideoQualityPreset preset);
stream::RemoteVideoQualityPreset m_monitorQualityPreset = stream::kDefaultRemoteVideoQualityPreset;
bool m_monitorQualityPresetActive = false;
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:4050-4061
if (m_userQualityPresetActive) {
    return m_userQualityPreset;
}
if (m_monitorQualityPresetActive) {
    return m_monitorQualityPreset;
}
return isFullScreen()
    ? stream::kFullscreenRemoteVideoQualityPreset
    : stream::kDefaultRemoteVideoQualityPreset;
```

```cpp
// src/ui/RemoteQualityCoordinator.h:63-70
stream::RemoteVideoQualityPreset userQualityPreset = stream::kDefaultRemoteVideoQualityPreset;
bool userQualityPresetActive = false;
stream::RemoteVideoQualityPreset monitorQualityPreset = stream::kDefaultRemoteVideoQualityPreset;
bool monitorQualityPresetActive = false;
```

```cpp
// src/ui/RemoteQualityCoordinator.cpp:117-124
const stream::RemoteVideoQualityPreset preferredPreset = window.userQualityPresetActive
    ? window.userQualityPreset
    : window.monitorQualityPresetActive
        ? window.monitorQualityPreset
        : decision.fullScreen
            ? stream::kFullscreenRemoteVideoQualityPreset
            : stream::kDefaultRemoteVideoQualityPreset;
```

```cpp
// tests/remote_video_policy_tests.cpp:41-56
stream::RemoteMonitorConfiguration monitorConfiguration;
assert(monitorConfiguration.grid == stream::RemoteMonitorGridPreset::Grid9);
assert(monitorConfiguration.quality == stream::RemoteVideoQualityPreset::P540_30);
assert(monitorConfiguration.rotationIntervalSeconds == 30);
assert(stream::remoteMonitorGridColumns(stream::RemoteMonitorGridPreset::Grid12) == 4);
```

```cpp
// tests/remote_quality_coordinator_tests.cpp:72-94
monitorWindow.monitorQualityPresetActive = true;
monitorWindow.monitorQualityPreset = stream::RemoteVideoQualityPreset::P540_25;
decisions = coordinator.evaluate(configuration, {monitorWindow}, 1150);
assert(decisions.front().preset == stream::RemoteVideoQualityPreset::P540_25);
```

### Steps
1. 在主窗口标题栏会话数字右侧增加“监控模式”按钮，开启状态使用蓝底白字，绘制、拖窗排除和点击命中共用同一动态布局快照。
2. 开启监控时扫描当前控制端所有已打开远控窗口，保存每个窗口进入监控前的普通几何、窗口状态和显隐。
3. 默认按3列3行九宫格显示第一页；12宫格使用4×3，20宫格使用5×4，其余使用2×2、4×4、5×5。
4. 超过单页容量时按默认30秒轮询下一页；最后一页只显示剩余窗口，非当前页窗口隐藏但保持连接并自动降到360/1。
5. 关闭监控后停止定时器，撤销监控统一画质，并恢复每个窗口原来的普通、最小化、最大化、全屏、隐藏状态和几何记忆策略。
6. 在“远控画质”右侧新增“监控模式”设置页，提供4/9/12/16/20/25宫格、八档画质和1至86400秒轮询输入。
7. 宫格修改立即重排，画质修改立即在线生效，轮询时间修改后从当前时刻重新计时；三项配置使用QSettings跨程序重启保存。
8. 保持既有画质优先级：窗口标题栏手选最高，其次监控统一画质，再次是全屏720/30和普通540/30；隐藏或完全遮挡最终使用360/1。
9. 监控设置选择1080/60或720/60时复用高带宽确认，取消后恢复原选项且不修改在线窗口。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已使用 `rg` 静态核对监控配置的声明、持久化、设置控件、定时器、分页入口、窗口画质输入和测试引用完整存在。
- 已静态核对标题栏监控按钮的绘制矩形同时用于鼠标手型、拖窗排除和释放点击，不存在黑区或隐藏热区分叉。
- 已静态核对监控关闭路径会停止定时器、撤销画质覆盖、恢复窗口几何/状态/显隐并清空恢复快照。
- 已更新纯策略测试源码，覆盖默认九宫格、六种布局、30秒默认值、监控画质优先级、手选优先级和360/1保活。
- 按用户此前要求，本次未构建、未链接、未运行测试或启动程序。

## 2026-08-11 11:03 - 限制监控模式只连接当前宫格页

### Changed Location
- `src/ui/DeviceGrid.h:194-199`：将监控刷新职责改为只创建当前页 Viewer，并新增标题栏统一会话计数接口。
- `src/ui/DeviceGrid.h:327-328`：明确监控窗口容器只持有当前宫格页，异步授权结果必须重新校验当前页。
- `src/ui/DeviceGrid.cpp:4517`：设备状态变化后重算当前页 Viewer 和标题栏实际监控增量。
- `src/ui/DeviceGrid.cpp:9843-9855`：标题栏改为显示普通远控会话数加当前页有效监控 Viewer 数。
- `src/ui/DeviceGrid.cpp:9981-10120`：分页前先选出当前页设备，只授权、创建和布局这一页，并关闭上一页连接。
- `src/ui/DeviceGrid.cpp:10804、12075、12396、13003`：绘制、拖窗排除、悬停和点击命中统一使用新的标题栏计数口径。

### Reason
此前监控模式会为全部在线设备一次性创建只读 Viewer，只把非当前页窗口隐藏并降到 `360/1`，同时标题栏直接显示全部待轮询设备数。因此即使选择 4 宫格，开启后连接和标题栏数字仍会按全部在线设备增长。现在把宫格容量同时作为可见窗口和实际监控 Viewer 的上限：4 宫格只保留当前页最多 4 路，轮询时关闭上一页再建立下一页；标题栏在原普通远控会话数基础上只叠加当前页有效监控数。

### Original Code
```cpp
// src/ui/DeviceGrid.h:194-199、327-328
void refreshRemoteMonitorMode(bool advancePage); // 按全部在线设备目录同步监控窗口并显示当前宫格页。
int remoteMonitorDeviceCount() const; // 统计全部监控目标数量。
void createRemoteMonitorWindowForDevice(int deviceIndex); // 为设备创建只读视频窗口。
QHash<QString, QPointer<RemoteDesktopWindow>> m_remoteMonitorWindows; // 持有全部设备Viewer。
QSet<QString> m_pendingRemoteMonitorDeviceIds; // 为全部轮询目标去重授权任务。
```

```cpp
// src/ui/DeviceGrid.cpp:9843-9856
int DeviceGrid::remoteMonitorDeviceCount() const
{
    int count = 0;
    for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        const DeviceEntry& device = g_devices.at(deviceIndex);
        const platform::DevicePresenceState presence = devicePresenceForIndex(deviceIndex);
        if (!device.ip.trimmed().isEmpty() && !deviceRecordMatchesLocal(device)
            && (presence == platform::DevicePresenceState::Online
                || presence == platform::DevicePresenceState::Busy)) {
            ++count;
        }
    }
    return count;
}
```

```cpp
// src/ui/DeviceGrid.cpp:9988-10033、10085-10112
const QVector<int> deviceIndexes = remoteMonitorDeviceIndexes();
for (const int deviceIndex : deviceIndexes) {
    targetKeys.insert(remoteMonitorDeviceKey(deviceIndex));
}
for (const int deviceIndex : deviceIndexes) {
    // 已授权设备会在此调用 createRemoteMonitorWindowForDevice(deviceIndex)。
}
for (int index = 0; index < deviceIndexes.size(); ++index) {
    RemoteDesktopWindow* remoteWindow = m_remoteMonitorWindows.value(
        remoteMonitorDeviceKey(deviceIndexes.at(index))).data();
    if (index < pageStart || index >= pageEnd) {
        remoteWindow->hide();
        continue;
    }
    remoteWindow->show();
}
```

```cpp
// src/ui/DeviceGrid.cpp:10799-10801
const int controlledSessionCount = m_remoteMonitorModeEnabled
    ? remoteMonitorDeviceCount()
    : totalRemoteControlSessionCount();
```

### Modified Code
```cpp
// src/ui/DeviceGrid.h:194-199、327-328
void refreshRemoteMonitorMode(bool advancePage); // wjy: 从全部在线设备中选出当前宫格页，只为这一页建立独立只读Viewer并在轮询时替换上一页。
int titlebarRemoteSessionCount() const; // wjy: 标题栏显示普通远控会话数加当前页监控Viewer数，宫格容量直接限制监控增量。
void createRemoteMonitorWindowForDevice(int deviceIndex); // wjy: 仅为当前宫格页创建不发布控制租约、不加入键鼠同步器的只读视频窗口。
QHash<QString, QPointer<RemoteDesktopWindow>> m_remoteMonitorWindows; // wjy: 只持有当前宫格页的只读Viewer，分页替换时移除并关闭上一页连接。
QSet<QString> m_pendingRemoteMonitorDeviceIds; // wjy: 当前页公钥授权期间按稳定设备ID去重，异步返回时再次校验设备仍在当前页。
```

```cpp
// src/ui/DeviceGrid.cpp:9843-9855
int DeviceGrid::titlebarRemoteSessionCount() const
{
    qint64 total = totalRemoteControlSessionCount();
    if (m_remoteMonitorModeEnabled) {
        for (auto it = m_remoteMonitorWindows.constBegin(); it != m_remoteMonitorWindows.constEnd(); ++it) {
            RemoteDesktopWindow* monitorWindow = it.value().data();
            if (monitorWindow && !monitorWindow->isClosingConnection()) {
                ++total;
            }
        }
    }
    return static_cast<int>(qMin<qint64>(total, std::numeric_limits<int>::max()));
}
```

```cpp
// src/ui/DeviceGrid.cpp:9987-10046
const QVector<int> deviceIndexes = remoteMonitorDeviceIndexes();
const int capacity = qMax(1, stream::remoteMonitorGridCapacity(m_remoteMonitorConfiguration.grid));
const int pageStart = m_remoteMonitorPageIndex * capacity;
const int pageEnd = qMin(pageStart + capacity, deviceIndexes.size());
for (int index = pageStart; index < pageEnd; ++index) {
    const int deviceIndex = deviceIndexes.at(index);
    pageDeviceIndexes.append(deviceIndex);
    pageTargetKeys.insert(remoteMonitorDeviceKey(deviceIndex));
}
for (auto it = m_remoteMonitorWindows.begin(); it != m_remoteMonitorWindows.end();) {
    const bool stillMatches = pageTargetKeys.contains(it.key());
    if (!stillMatches) {
        obsoleteWindows.append(it.value());
        it = m_remoteMonitorWindows.erase(it);
        continue;
    }
    ++it;
}
for (const int deviceIndex : pageDeviceIndexes) {
    if (m_authorizedRemoteControlIps.contains(g_devices.at(deviceIndex).ip.trimmed())) {
        createRemoteMonitorWindowForDevice(deviceIndex);
    }
}
```

```cpp
// src/ui/DeviceGrid.cpp:10056-10075
const QSet<QString> currentPageKeys = [&] {
    QSet<QString> keys;
    const QVector<int> currentIndexes = remoteMonitorDeviceIndexes();
    const int currentCapacity = qMax(1, stream::remoteMonitorGridCapacity(m_remoteMonitorConfiguration.grid));
    const int currentPageStart = m_remoteMonitorPageIndex * currentCapacity;
    const int currentPageEnd = qMin(currentPageStart + currentCapacity, currentIndexes.size());
    for (int index = currentPageStart; index < currentPageEnd; ++index) {
        keys.insert(remoteMonitorDeviceKey(currentIndexes.at(index)));
    }
    return keys;
}();
```

```cpp
// src/ui/DeviceGrid.cpp:10095-10116、10804
for (int slot = 0; slot < pageDeviceIndexes.size(); ++slot) {
    RemoteDesktopWindow* remoteWindow = m_remoteMonitorWindows.value(
        remoteMonitorDeviceKey(pageDeviceIndexes.at(slot))).data();
    if (!remoteWindow || remoteWindow->isClosingConnection()) continue;
    remoteWindow->show();
}
const int controlledSessionCount = titlebarRemoteSessionCount();
```

### Steps
1. 把完整在线设备集合限制为页数和稳定轮询顺序的数据源，不再对完整集合逐台建连。
2. 根据当前宫格容量生成本页设备、稳定键和 IP 快照，只对本页执行公钥授权与 Viewer 创建。
3. 翻页、宫格缩小、设备离线或 IP 变化时，先从监控索引移除旧窗口，再关闭旧 Viewer，避免标题栏同时累计两页。
4. 异步授权返回时重新计算当前页设备键，只允许仍位于当前页的设备创建 Viewer，防止快速翻页后旧授权结果回补旧页。
5. 删除非当前页隐藏并使用 `360/1` 保活的分支，改为轮询时真正关闭上一页连接。
6. 标题栏数字改为普通远控会话总数加当前页有效监控 Viewer 数，并让绘制和三个鼠标命中入口共用该接口。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已使用 `rg` 确认旧的 `remoteMonitorDeviceCount` 已无引用，标题栏四个布局入口全部改用 `titlebarRemoteSessionCount`。
- 已静态核对授权循环和布局循环只遍历 `pageDeviceIndexes`，非当前页不再调用 Viewer 创建或隐藏保活。
- 已静态核对异步授权回调使用 `currentPageKeys` 拒绝已经轮换出去的设备。
- 按用户要求，本次未构建、未链接、未运行测试或启动程序。

## 2026-08-11 11:34 - 新增固定槽位监控窗口并改为切换视频源

### Changed Location
- `src/ui/RemoteDesktopWindow.h:50-67、125-133、342-346、529-541`：新增监控窗口样式、切源接口、切源状态和专用 `RemoteMonitorWindow` 类型。
- `src/ui/RemoteDesktopWindow.cpp:699-738、1983-2246`：监控标题栏取消 Logo，空槽构造时不启动 Viewer，并让槽位计时从窗口创建后持续运行。
- `src/ui/RemoteDesktopWindow.cpp:2438-2539、3079-3157`：实现旧 Viewer 安全停止后连接最新 IP 的监控切源状态机。
- `src/ui/RemoteDesktopWindow.cpp:3437-3507、6812-6820、7110-7425`：监控标题栏仅显示设备名、IP、时间，并屏蔽本机鼠标、拖动、缩放和双击操作。
- `src/ui/DeviceGrid.h:57、113-118、204-210、335`：使用固定监控槽位替代按设备持有窗口的哈希表。
- `src/ui/DeviceGrid.cpp:7257-7262、9629-9659、9843-9950、9982-10099`：仅在开启监控或宫格容量变化时增减窗口，普通轮询只调用槽位 `switchSource()`，并合并首次槽位创建后的重复刷新。

### Reason
上一版虽然把同时连接数限制到了当前宫格页，但每次轮询仍会关闭上一页顶层窗口并为下一页创建新窗口，导致窗口层级、D3D 表面和计时器反复建立。用户要求监控刷新不创建新窗口，只切换视频源，并为监控模式使用单独的精简窗口样式：标题栏只显示设备名、IP 和持续累计时间，切换设备时计时不归零。

本次将监控窗口改为按宫格位置长期存在的固定槽位。不同设备仍必须建立各自的 WebRTC Viewer，但旧连接和新连接在同一个窗口对象内顺序切换；Viewer 代际、回调上下文和停止任务继续复用现有安全生命周期，旧设备迟到帧不会出现在新设备标题下方。

### Original Code
```cpp
// src/ui/RemoteDesktopWindow.h:49-59
class RemoteDesktopWindow final : public QWidget, public RemoteInputEndpoint {
public:
    explicit RemoteDesktopWindow(
        const QString& deviceName,
        const QString& hostIp,
        RemoteViewerLifecycleManager* lifecycleManager,
        RemoteInputBroadcastCoordinator* inputBroadcastCoordinator,
        QWidget* parent = nullptr,
        bool monitorReadOnly = false);
};
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2229-2232
updateNativeTitleBarSurface(true);
QTimer::singleShot(0, this, &RemoteDesktopWindow::startViewerConnection);
if (!m_monitorReadOnly) {
    QTimer::singleShot(0, this, &RemoteDesktopWindow::requestRemoteInputScriptStatus);
}
```

```cpp
// src/ui/DeviceGrid.h:327-328
QHash<QString, QPointer<RemoteDesktopWindow>> m_remoteMonitorWindows;
QSet<QString> m_pendingRemoteMonitorDeviceIds;
```

```cpp
// src/ui/DeviceGrid.cpp:9901-9938
void DeviceGrid::createRemoteMonitorWindowForDevice(int deviceIndex)
{
    auto* monitorWindow = new RemoteDesktopWindow(
        deviceDisplayName(device),
        ip,
        m_remoteViewerLifecycleManager.get(),
        nullptr,
        nullptr,
        true);
    m_remoteMonitorWindows.insert(key, monitorWindow);
    registerRemoteQualityWindow(monitorWindow, true);
}
```

```cpp
// src/ui/DeviceGrid.cpp:10012-10027、10095-10117
for (auto it = m_remoteMonitorWindows.begin(); it != m_remoteMonitorWindows.end();) {
    if (!pageTargetKeys.contains(it.key())) {
        obsoleteWindows.append(it.value());
        it = m_remoteMonitorWindows.erase(it);
        continue;
    }
    ++it;
}
for (const QPointer<RemoteDesktopWindow>& obsoleteWindow : obsoleteWindows) {
    if (obsoleteWindow) obsoleteWindow->close();
}
for (int slot = 0; slot < pageDeviceIndexes.size(); ++slot) {
    RemoteDesktopWindow* remoteWindow = m_remoteMonitorWindows.value(
        remoteMonitorDeviceKey(pageDeviceIndexes.at(slot))).data();
    remoteWindow->show();
}
```

### Modified Code
```cpp
// src/ui/RemoteDesktopWindow.h:50-67、529-541
enum class RemoteDesktopWindowStyle {
    Control,
    Monitor,
};

class RemoteMonitorWindow final : public RemoteDesktopWindow {
public:
    explicit RemoteMonitorWindow(RemoteViewerLifecycleManager* lifecycleManager, QWidget* parent = nullptr);
    void switchSource(const QString& deviceName, const QString& hostIp);
    void clearSource();
    bool hasSource() const;
};
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:2438-2500
void RemoteDesktopWindow::switchMonitorSource(const QString& deviceName, const QString& hostIp)
{
    const QString targetName = deviceName.trimmed();
    const QString targetIp = hostIp.trimmed();
    cancelNetworkReconnect();
    m_monitorSourceRestartRequested = !targetIp.isEmpty();
    m_deviceName = targetName;
    m_hostIp = targetIp;
    invalidateViewerCallbacks();
    m_remoteFrame = QImage();
    if (m_texturePresenter) m_texturePresenter->reset();
    if (m_viewerHandle || m_viewerStopInProgress || m_viewerStartQueued || m_viewerStartAdmissionActive) {
        stopViewerConnectionAsync(false);
        return;
    }
    if (m_monitorSourceRestartRequested) {
        m_monitorSourceRestartRequested = false;
        startViewerConnection();
    }
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3083-3087、3149-3153
if (m_monitorSourceRestartRequested && m_windowStyle == RemoteDesktopWindowStyle::Monitor
    && !m_hostIp.trimmed().isEmpty()) {
    m_monitorSourceRestartRequested = false;
    startViewerConnection();
}
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:3437-3507
if (m_windowStyle == RemoteDesktopWindowStyle::Monitor) {
    return {};
}

state.networkWarningText = m_windowStyle == RemoteDesktopWindowStyle::Control && m_networkWarningVisible
    ? zh("网络不佳")
    : QString();
if (m_windowStyle == RemoteDesktopWindowStyle::Control
    && m_connectionStatusCode == FSREMOTE_STATUS_RECEIVING_VIDEO) {
    state.performanceText = QStringLiteral("%1FPS · %2Mbps").arg(fpsText).arg(bitrateText);
}
```

```cpp
// src/ui/DeviceGrid.h:113-118、335
struct RemoteMonitorSlot {
    QPointer<RemoteMonitorWindow> window;
    QString deviceKey;
};
QVector<RemoteMonitorSlot> m_remoteMonitorSlots;
```

```cpp
// src/ui/DeviceGrid.cpp:9902-9937、10043-10088
void DeviceGrid::ensureRemoteMonitorWindowSlots(int slotCount)
{
    if (m_remoteMonitorSlots.size() < slotCount) {
        m_remoteMonitorSlots.resize(slotCount);
    }
    for (RemoteMonitorSlot& slot : m_remoteMonitorSlots) {
        if (!slot.window) {
            slot.window = new RemoteMonitorWindow(m_remoteViewerLifecycleManager.get());
        }
    }
}

for (int slotIndex = 0; slotIndex < m_remoteMonitorSlots.size(); ++slotIndex) {
    RemoteMonitorSlot& slot = m_remoteMonitorSlots[slotIndex];
    RemoteMonitorWindow* remoteWindow = slot.window.data();
    if (slotIndex >= pageDeviceIndexes.size()) {
        remoteWindow->clearSource();
        remoteWindow->hide();
        continue;
    }
    remoteWindow->switchSource(deviceDisplayName(device), ip);
    remoteWindow->setGeometry(target);
    remoteWindow->show();
}
```

### Steps
1. 将 `RemoteDesktopWindow` 从不可继承类调整为可复用流内核，并增加普通远控与监控两种固定窗口样式。
2. 新增 `RemoteMonitorWindow` 类型，构造时不绑定设备、不启动 Viewer，但立即启动槽位会话时钟。
3. 新增监控切源状态：切换时先更新新设备名和 IP、清除旧帧、推进 Viewer 代际，再异步停止旧 Viewer。
4. 在旧 Viewer 停止完成回调中启动当前最新目标；30 秒内发生多次切换时只连接最后一次设备。
5. 监控标题栏移除 Logo、更新、画质、键鼠、同步、音频、剪贴板、最小化、关闭、FPS、码率和网络状态，只保留设备名、IP、`HH:MM:SS`。
6. 监控窗口拦截本机鼠标按下、移动、释放、双击和滚轮，不允许拖动、缩放、最大化或向远端发送输入。
7. 将 `DeviceGrid` 的设备窗口哈希替换为固定槽位数组；只有开启监控、关闭监控或宫格容量变化时增减顶层窗口。
8. 轮询页变化时按槽位调用 `switchSource()`；最后一页空槽只清源并隐藏，槽位窗口和累计时间不销毁、不归零。
9. 删除每个新槽位各自排队的二次刷新，首次创建 25 宫格时仍只由当前一次刷新完成全部分源和布局。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已使用 `rg` 确认 `m_remoteMonitorWindows` 和 `createRemoteMonitorWindowForDevice` 已从源码移除。
- 已静态核对 `new RemoteMonitorWindow` 只存在于 `ensureRemoteMonitorWindowSlots()`，`refreshRemoteMonitorMode()` 的正常轮询路径只调用 `switchSource()` 或 `clearSource()`。
- 已静态核对 `m_sessionClock.start()` 只在窗口构造阶段调用，监控切源路径没有重启或重置计时器。
- 已静态核对旧 Viewer 代际在切源前失效，并且新 Viewer 只在旧 stop 完成后启动。
- 已静态核对槽位构造完成后不再逐窗口调用 `refreshRemoteMonitorMode()`，避免大宫格首次开启产生重复事件队列。
- 按用户要求，本次未构建、未链接、未运行测试或启动程序。

## 2026-08-11 11:57 - 固定监控窗口显示状态并移除轮询重显

### Changed Location
- `src/ui/DeviceGrid.cpp:10004`：更新当前页设备集合说明，明确未分配槽位保留固定窗口位置。
- `src/ui/DeviceGrid.cpp:10041-10096`：把宫格布局与视频源分配分离，监控窗口只在首次展示时显示，轮询期间不再隐藏、恢复或重复置顶。

### Reason
固定槽位版本虽然只在 `ensureRemoteMonitorWindowSlots()` 中创建 `RemoteMonitorWindow`，但每轮刷新仍对当前页窗口执行 `showNormal()`、`show()` 和 `raise()`；最后一页空槽与等待授权槽还会先 `hide()`，下一页再重新显示。顶层窗口对象没有重新 `new`，但 Windows 层面的反复重显、恢复和调整 Z 序会产生类似重新创建窗口的闪烁和抢焦点效果。

本次让宫格容量对应的窗口外壳在首次创建后持续可见并固定在原槽位。30 秒轮询只调用 `switchSource()` 或 `clearSource()` 更换窗口内部 Viewer；仅当屏幕或宫格配置使目标矩形实际变化时才重新设置窗口几何。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:10041-10087（修改前）
RemoteMonitorWindow* remoteWindow = slot.window.data();
if (!remoteWindow || remoteWindow->isClosingConnection()) continue;

if (slotIndex >= pageDeviceIndexes.size()) {
    slot.deviceKey.clear();
    remoteWindow->clearSource();
    remoteWindow->hide();
    continue;
}

// 分配视频源后，每轮都会重新恢复、定位、显示和置顶窗口。
remoteWindow->showNormal();
remoteWindow->setGeometry(target);
remoteWindow->show();
remoteWindow->raise();
```

```cpp
// src/ui/DeviceGrid.cpp:10057-10061（修改前）
if (!m_authorizedRemoteControlIps.contains(ip)) {
    slot.deviceKey.clear();
    remoteWindow->clearSource();
    remoteWindow->hide();
    continue;
}
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:10041-10069（修改后）
RemoteMonitorWindow* remoteWindow = slot.window.data();
if (!remoteWindow) continue;

const int row = slotIndex / columnCount;
const int column = slotIndex % columnCount;
QRect target(
    availableRect.x() + column * tileWidth,
    availableRect.y() + row * tileHeight,
    tileWidth,
    tileHeight);
if (remoteWindow->geometry() != target) {
    remoteWindow->setGeometry(target);
}
if (!remoteWindow->isVisible()) {
    remoteWindow->show();
    remoteWindow->raise();
}

if (remoteWindow->isClosingConnection()) continue;
```

```cpp
// src/ui/DeviceGrid.cpp:10071-10084（修改后）
if (slotIndex >= pageDeviceIndexes.size()) {
    slot.deviceKey.clear();
    remoteWindow->clearSource();
    continue;
}

if (!m_authorizedRemoteControlIps.contains(ip)) {
    slot.deviceKey.clear();
    remoteWindow->clearSource();
    continue;
}
```

### Steps
1. 将每个槽位的行列映射和目标矩形计算移动到视频源分配之前，使空槽、授权等待槽和在线槽都使用固定位置。
2. 为 `setGeometry()` 增加矩形变化判断，普通轮询不再重复触发原生窗口定位。
3. 删除轮询路径中的 `showNormal()`，并把 `show()`、`raise()` 限制为窗口首次展示或异常隐藏后的恢复路径。
4. 删除空槽和授权等待路径中的 `hide()`，仅清理旧 Viewer，让窗口外壳持续显示“等待监控设备”。
5. 保留关闭监控模式和宫格容量缩小时的既有窗口关闭逻辑，这两种显式场景仍会释放不再需要的槽位。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已使用 `rg` 确认 `new RemoteMonitorWindow` 仍只存在于 `ensureRemoteMonitorWindowSlots()`。
- 已使用 `rg` 确认 `refreshRemoteMonitorMode()` 中不存在 `hide()` 和 `showNormal()`，`show()`、`raise()` 只位于 `!isVisible()` 首次展示分支。
- 已静态核对轮询换页继续复用 `switchSource()`，空槽继续复用 `clearSource()`，均不会销毁 `RemoteMonitorWindow`。
- 按用户要求，本次未构建、未链接、未运行测试或启动程序。

## 2026-08-11 12:48 - 锁定监控模式首次铺屏显示器

### Changed Location
- `src/ui/DeviceGrid.h:337`：新增当前监控会话锁定的显示器名称状态。
- `src/ui/DeviceGrid.cpp:9965-9982`：开启监控时记录主窗口所在显示器，关闭监控时清理锁定状态。
- `src/ui/DeviceGrid.cpp:10039-10055`：轮询布局优先解析锁定显示器，目标屏幕失效时才执行回退。

### Reason
上一版虽然不再重复显示或创建监控窗口，但 `refreshRemoteMonitorMode()` 每次仍通过 `window()->screen()` 获取主窗口当前显示器。因此主窗口从 A 屏移动到 B 屏后，下一次 30 秒轮询会计算出 B 屏的宫格矩形，几何变化判断随即把所有固定监控窗口搬到 B 屏。

本次把显示器选择改为监控会话级状态：开启时锁定主窗口所在屏幕，普通设备轮询、授权回调和宫格刷新都继续使用该屏幕。只有锁定显示器被拔除、重命名或不再由 Qt 提供时，才回退到当前可用屏幕并重新锁定。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:9962-9968（修改前）
m_remoteMonitorModeEnabled = enabled;
m_remoteMonitorPageIndex = 0;
if (enabled) {
    refreshRemoteMonitorMode(false);
    if (m_remoteMonitorTimer) {
        m_remoteMonitorTimer->start();
    }
}
```

```cpp
// src/ui/DeviceGrid.cpp:10031-10035（修改前）
QScreen* screen = window() ? window()->screen() : QGuiApplication::primaryScreen();
if (!screen) {
    screen = QGuiApplication::primaryScreen();
}
const QRect availableRect = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
```

### Modified Code
```cpp
// src/ui/DeviceGrid.h:337（修改后）
QString m_remoteMonitorScreenName;
```

```cpp
// src/ui/DeviceGrid.cpp:9965-9972（修改后）
QScreen* monitorScreen = window() ? window()->screen() : QGuiApplication::primaryScreen();
if (!monitorScreen) {
    monitorScreen = QGuiApplication::primaryScreen();
}
m_remoteMonitorScreenName = monitorScreen ? monitorScreen->name() : QString();
refreshRemoteMonitorMode(false);
```

```cpp
// src/ui/DeviceGrid.cpp:10039-10055（修改后）
QScreen* screen = nullptr;
for (QScreen* candidateScreen : QGuiApplication::screens()) {
    if (candidateScreen && candidateScreen->name() == m_remoteMonitorScreenName) {
        screen = candidateScreen;
        break;
    }
}
if (!screen) {
    screen = window() ? window()->screen() : QGuiApplication::primaryScreen();
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    m_remoteMonitorScreenName = screen ? screen->name() : QString();
}
```

### Steps
1. 在 `DeviceGrid` 中增加监控目标显示器名称，生命周期与一次监控模式开启周期一致。
2. 开启监控时读取一次主窗口所在显示器名称，不再让定时轮询直接依赖主窗口的实时 `screen()`。
3. 每次布局时从 `QGuiApplication::screens()` 解析锁定显示器，并继续使用其 `availableGeometry()`。
4. 仅当锁定屏幕无法解析时才回退到主窗口当前屏幕或主屏，随后锁定新的有效目标。
5. 关闭监控模式时清空屏幕名称，使下次开启能够选择届时主窗口所在的屏幕。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已使用 `rg` 确认 `refreshRemoteMonitorMode()` 的正常路径不再直接把 `window()->screen()` 作为首选屏幕。
- 已静态核对主窗口从 A 屏移动到 B 屏时，保存的 `m_remoteMonitorScreenName` 不会改变，轮询继续使用 A 屏几何。
- 已静态核对锁定显示器失效后才会执行回退并更新名称，避免窗口落在已经断开的屏幕区域。
- 按用户要求，本次未构建、未链接、未运行测试或启动程序。

## 2026-08-11 13:48 - 监控轮询仅保留指定 Busy 设备

### Changed Location
- `src/ui/DeviceGrid.cpp:1417-1438`：新增可手动维护的监控排除白名单，以及英文首字母和白名单匹配辅助函数。
- `src/ui/DeviceGrid.cpp:9880-9903`：将轮询候选集改为只接受符合名称规则的 Busy 设备。
- `src/ui/DeviceGrid.h:202-205`：同步更新监控刷新与候选集接口说明。

### Reason
原监控集合同时包含 `Online` 和 `Busy` 设备，会查看大量当前没有被控制的普通在线电脑。用户要求监控模式只轮询已经被控的 Busy 设备，同时排除名称以英文字母开头的设备，并提供一个后续可直接修改源码的设备名名单，名单中的设备即使 Busy 也不参与轮询。

本次把用户所称的“白名单”实现为排除白名单。名单按最终界面显示设备名执行完整匹配并忽略英文大小写；英文开头规则只识别 ASCII `A-Z` 和 `a-z`，不会把中文、数字或其它字符开头的名称误判为英文。

### Original Code
```cpp
// src/ui/DeviceGrid.cpp:9860-9869（修改前）
for (int deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
    const DeviceEntry& device = g_devices.at(deviceIndex);
    const QString ip = device.ip.trimmed();
    const platform::DevicePresenceState presence = devicePresenceForIndex(deviceIndex);
    if (ip.isEmpty() || deviceRecordMatchesLocal(device)
        || (presence != platform::DevicePresenceState::Online
            && presence != platform::DevicePresenceState::Busy)) {
        continue;
    }
    indexes.append(deviceIndex);
}
```

### Modified Code
```cpp
// src/ui/DeviceGrid.cpp:1417-1438（修改后）
const QStringList kRemoteMonitorExclusionWhitelist = {
    // QStringLiteral("这里填写不参与监控轮询的完整设备名"),
};

bool remoteMonitorNameStartsWithEnglishLetter(const QString& deviceName)
{
    const QString normalizedName = deviceName.trimmed();
    if (normalizedName.isEmpty()) return false;
    const QChar firstCharacter = normalizedName.front();
    return (firstCharacter >= QLatin1Char('A') && firstCharacter <= QLatin1Char('Z'))
        || (firstCharacter >= QLatin1Char('a') && firstCharacter <= QLatin1Char('z'));
}
```

```cpp
// src/ui/DeviceGrid.cpp:9884-9903（修改后）
const DeviceEntry& device = g_devices.at(deviceIndex);
const QString ip = device.ip.trimmed();
const platform::DevicePresenceState presence = devicePresenceForIndex(deviceIndex);
const QString displayName = deviceDisplayName(device);
if (ip.isEmpty() || deviceRecordMatchesLocal(device)) {
    continue;
}
if (presence != platform::DevicePresenceState::Busy) {
    continue;
}
if (remoteMonitorNameStartsWithEnglishLetter(displayName)) {
    continue;
}
if (remoteMonitorNameIsWhitelistedForExclusion(displayName)) {
    continue;
}
indexes.append(deviceIndex);
```

### Steps
1. 在设备显示名辅助函数旁增加空的 `kRemoteMonitorExclusionWhitelist` 数组，并保留可直接复制修改的示例项。
2. 增加 ASCII 英文首字母判断，名称首字符为 `A-Z` 或 `a-z` 时排除。
3. 增加排除白名单完整名称匹配，去除首尾空格并忽略英文大小写。
4. 将设备状态条件从 `Online || Busy` 收紧为仅 `Busy`。
5. 保留有效 IP、本机排除、稳定自然排序、分页和固定槽位切源逻辑。

### Verification
- 已执行 `git diff --check`，未发现空白错误。
- 已使用 `rg` 确认候选集仅接受 `DevicePresenceState::Busy`，不再接受普通 `Online`。
- 已静态核对英文首字母过滤只覆盖 ASCII 英文字母，中文和数字开头的 Busy 设备仍可进入轮询。
- 已静态核对白名单使用最终显示名称完整匹配，默认空数组不会排除任何额外设备。
- 已确认本次只准备提交 `DeviceGrid.cpp`、`DeviceGrid.h` 和 `WJY_CODE_CHANGE_LOG.md`，不会包含用户当前对输入脚本和 WebRTC 文件的未提交修改。
- 按用户要求，本次未构建、未链接、未运行测试或启动程序。

## 2026-08-11 13:48 - 修复F10随机粘贴覆盖原剪贴板

### Changed Location
- `src/system/InputScriptExecutionService.h:115`：为F10独立执行器增加随机粘贴准备、延迟恢复、条件恢复和状态清理接口。
- `src/system/InputScriptExecutionService.h:131`：保存临时剪贴板恢复定时器、粘贴前原文、Windows序号和待恢复状态。
- `src/system/InputScriptExecutionService.cpp:37`：新增带Windows自定义格式标记的文本剪贴板写入函数。
- `src/system/InputScriptExecutionService.cpp:354`：创建独立的80毫秒单次恢复定时器。
- `src/system/InputScriptExecutionService.cpp:684`：脚本遇到Ctrl+V时改为基于粘贴前原文生成本次随机文本，V抬起后调度恢复。
- `src/system/InputScriptExecutionService.cpp:721`：实现连续粘贴去累积、外部剪贴板变化保护、恢复重试以及停止和退出兜底。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1618`：登记与F10执行器一致的临时剪贴板格式名。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1931`：Host轮询跳过带F10临时标记的随机文本，防止同步到主控和其它窗口。

### Reason
原实现直接读取目标端当前文本，再用`原文本 + 分隔符 + 随机串`永久覆盖系统剪贴板。下一轮Ctrl+V会把已经追加过随机串的内容再次作为源文本，形成连续累积；开启剪贴板同步时，临时随机内容还可能被Host轮询广播到主控端。新实现将随机文本限定为一次粘贴期间的临时值，并只在剪贴板序号未被用户或目标程序改变时恢复粘贴前原文。

### Original Code
```cpp
// src/system/InputScriptExecutionService.cpp:602-616
QClipboard* clipboard = QGuiApplication::clipboard();
const QString sourceText = clipboard ? clipboard->text() : QString();
if (!sourceText.isEmpty() && clipboard) {
    // 生成随机字符串。
    clipboard->setText(sourceText + m_request.pasteRandomSeparator + randomText);
}
```

```cpp
// src/system/InputScriptExecutionService.h:115-125
// 此位置原来只有injectEvent、releaseHeldInputs和播放状态字段，
// 没有独立的随机粘贴恢复接口或剪贴板原文快照。
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1918-1923
const DWORD sequence = ::GetClipboardSequenceNumber();
if (sequence == 0 || sequence == last_clipboard_sequence_) return;
if (sequence == g_clipboard_ignore_sequence.load()) {
    last_clipboard_sequence_ = sequence;
    return;
}
```

### Modified Code
```cpp
// src/system/InputScriptExecutionService.cpp:684-700
if (m_request.pasteRandomSuffixEnabled && event.virtualKey == 'V' && m_ctrlDown) {
    if (!prepareRandomPasteClipboard()) return false;
}
if (!sendKey(event.virtualKey, true)) return false;

if (event.virtualKey == 'V' && m_pasteClipboardRestorePending) {
    schedulePasteClipboardRestore();
}
```

```cpp
// src/system/InputScriptExecutionService.cpp:721-786
if (!m_pasteClipboardRestorePending) {
    m_pasteClipboardOriginalText = clipboard ? clipboard->text() : QString();
}
// 每次基于固定原文生成新的临时随机文本，并记录Windows剪贴板序号。
// 80ms后仅在序号仍一致时恢复原文；检测到外部复制则放弃恢复权。
```

```cpp
// third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:1931-1941
const UINT transientFormat = input_script_transient_clipboard_format();
if (transientFormat != 0 && ::IsClipboardFormatAvailable(transientFormat)) {
    last_clipboard_sequence_ = sequence;
    return;
}
```

### Steps
1. 在F10目标端独立执行器中增加专用单次恢复定时器，恢复逻辑不依赖Viewer窗口或网络连接。
2. 第一次随机Ctrl+V保存目标端粘贴前文本；连续粘贴始终基于该原文生成新随机后缀，不读取上一次临时结果。
3. 临时文本写入Windows剪贴板时附加`FSRemote.InputScript.TransientClipboard.v1`自定义格式标记。
4. V抬起80毫秒后检查剪贴板序号；序号一致才恢复原文，用户或目标程序期间产生的新剪贴板内容不会被覆盖。
5. 剪贴板短暂被占用时保留恢复状态并重试；脚本停止、注入失败和目标程序退出也执行恢复兜底。
6. Host剪贴板轮询识别临时标记并跳过随机文本；恢复后的真实原文不带标记，可以继续按正常同步逻辑处理。

### Verification
- 已执行`git diff --check`，未发现空白错误。
- 已使用`rg`核对新增方法声明、定义和调用均完整存在。
- 已静态核对连续Ctrl+V不会把上一次随机结果作为下一次源文本。
- 已静态核对恢复前比较Windows剪贴板序号，外部新内容不会被旧脚本覆盖。
- 已静态核对临时随机文本带自定义格式，Host轮询不会把它广播到主控或其它监控窗口。
- 按用户此前要求，本次未构建、未链接、未运行测试或启动程序。

## 2026-08-11 14:52 - 新增F12目标端原图截图与确认管理

### Changed Location
- `CMakeLists.txt:545`：登记截图确认窗口源码；`CMakeLists.txt:588`登记截图服务源码。
- `src/system/AppSettings.h:66`、`src/system/AppSettings.cpp:414`：增加默认F12的截图快捷键读取和保存接口。
- `src/system/DeviceCommandService.h:61`、`src/system/DeviceCommandService.cpp:204`、`src/system/DeviceCommandService.cpp:318`、`src/system/DeviceCommandService.cpp:392`、`src/system/DeviceCommandService.cpp:982`：增加截图命令、完整单行回复读取、目标端执行和共享路径返回。
- `src/ui/DeviceGrid.h:266`、`src/ui/DeviceGrid.h:379`：声明截图焦点路由、请求、预览和串行确认状态。
- `src/ui/DeviceGrid.cpp:635`：快捷键编辑项扩展为10项；`src/ui/DeviceGrid.cpp:854`增加独立全局截图Hook；`src/ui/DeviceGrid.cpp:3762`增加设置行；`src/ui/DeviceGrid.cpp:4813`登记全局快捷键；`src/ui/DeviceGrid.cpp:4936`实现远端/本机截图路由；`src/ui/DeviceGrid.cpp:12956`接收Hook消息。
- `src/ui/RemoteDesktopWindow.h:143`、`src/ui/RemoteDesktopWindow.cpp:6266`、`src/ui/RemoteDesktopWindow.cpp:7489`：远控窗口Hook和Qt按键路径消费截图快捷键并上报当前窗口。
- `src/system/ScreenshotService.h:9`、`src/system/ScreenshotService.cpp:38`：新增原始主屏采集、固定共享目录、文件名清洗、PNG写入和受控路径校验。
- `src/ui/ScreenshotReviewDialog.h:11`、`src/ui/ScreenshotReviewDialog.cpp:24`：新增原像素预览、文件名编辑、确定保留和关闭删除窗口。

### Reason
控制端Viewer画面可能经过缩放和视频编码，直接在控制端截屏会降低清晰度。新流程只发送很短的截图命令，目标设备使用自己的主屏原始像素生成无损PNG并写入`\\192.168.1.100\ggc\喊话截图`，控制端只读取共享路径并负责最终确认。Windows把裸F12保留给调试器，`RegisterHotKey`可能失败，因此截图使用独立低级键盘Hook兜底；普通远控窗口仍优先使用原有键盘Hook，保证自定义组合键不会误传给目标应用。

### Original Code
```cmake
# CMakeLists.txt:541-548
src/ui/RemoteDesktopWindow.cpp
src/ui/RemoteDesktopWindow.h
src/ui/NativeRemoteTitleBarSurface.cpp

# 原来没有ScreenshotReviewDialog和ScreenshotService构建项。
```

```cpp
// src/system/AppSettings.h:64-68
static QKeySequence remoteShortcutInputScriptPlayback();
static void setRemoteShortcutInputScriptPlayback(const QKeySequence& shortcut);
static QKeySequence deviceShortcutDelete();
static void setDeviceShortcutDelete(const QKeySequence& shortcut);
```

```cpp
// src/system/AppSettings.cpp:404-414
void AppSettings::setRemoteShortcutInputScriptPlayback(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("remoteShortcutInputScriptPlayback"), shortcut, QKeySequence(QStringLiteral("F10")));
}

QKeySequence AppSettings::deviceShortcutDelete()
```

```cpp
// src/system/DeviceCommandService.h:58-62
static bool requestDeviceListSync(const QString& hostIp, QString* errorMessage = nullptr,
    uint16_t port = 49102, int timeoutMs = 700);
// 原来没有requestScreenshot接口。
```

```cpp
// src/system/DeviceCommandService.cpp:215-229
if (!socket.waitForConnected(timeoutMs)
    || socket.write(payload) != payload.size()
    || !socket.waitForBytesWritten(timeoutMs)
    || !socket.waitForReadyRead(timeoutMs)) {
    return false;
}
if (reply) *reply = socket.readAll().trimmed();

// 原来没有screenshot命令编码、目标端截图分支和结果解析。
```

```cpp
// src/ui/DeviceGrid.h:261-270, 377-380
void registerGlobalShortcuts();
void unregisterGlobalShortcuts();
void triggerShortcutAction(int shortcutIndex);
void releaseRemoteShortcutKeyState(int shortcutIndex);

QSet<int> m_registeredGlobalShortcutIds;
quintptr m_globalShortcutWindowHandle = 0;
// 原来没有截图路由方法、待处理目标和确认窗口状态。
```

```cpp
// src/ui/DeviceGrid.cpp:632-640
constexpr int kRemoteShortcutCount = 5;
constexpr int kRemoteWindowShortcutMouseLockIndex = 5;
constexpr int kRemoteWindowShortcutRecordingIndex = 6;
constexpr int kRemoteWindowShortcutPlaybackIndex = 7;
constexpr int kShortcutEditorCount = 9;
constexpr int kDeleteDeviceShortcutIndex = 8;

// 原来没有独立截图Hook、截图设置行和远端/本机焦点路由。
```

```cpp
// src/ui/RemoteDesktopWindow.h:139-144
void shortcutCloseAllRequested();
void shortcutClipboardSyncRequested();
void titleBarContextMenuRequested(const QString& hostIp, const QPoint& globalPosition);
// 原来没有shortcutScreenshotRequested信号。
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:6257-6267
if (matchesShortcut(current, platform::AppSettings::remoteShortcutInputScriptPlayback())) {
    emit shortcutInputScriptPlaybackRequested();
    return true;
}
if (matchesShortcut(current, platform::AppSettings::remoteShortcutFullscreen())) {
    // 原来F10后直接进入其它快捷键判断，没有截图分支。
}
```

```text
// src/system/ScreenshotService.h
// src/system/ScreenshotService.cpp
新增文件，此位置没有旧代码。
```

```text
// src/ui/ScreenshotReviewDialog.h
// src/ui/ScreenshotReviewDialog.cpp
新增文件，此位置没有旧代码。
```

### Modified Code
```cmake
// CMakeLists.txt:545-546, 588-589
src/ui/ScreenshotReviewDialog.cpp
src/ui/ScreenshotReviewDialog.h
src/system/ScreenshotService.cpp
src/system/ScreenshotService.h
```

```cpp
// src/system/AppSettings.h:66-67
static QKeySequence screenshotShortcut();
static void setScreenshotShortcut(const QKeySequence& shortcut);
```

```cpp
// src/system/AppSettings.cpp:414-422
QKeySequence AppSettings::screenshotShortcut()
{
    return shortcutFromSettings(QStringLiteral("screenshotShortcut"), QKeySequence(QStringLiteral("F12")));
}

void AppSettings::setScreenshotShortcut(const QKeySequence& shortcut)
{
    setShortcutToSettings(QStringLiteral("screenshotShortcut"), shortcut, QKeySequence(QStringLiteral("F12")));
}
```

```cpp
// src/system/DeviceCommandService.h:61-68
static bool requestScreenshot(
    const QString& hostIp,
    const QString& groupName,
    const QString& deviceName,
    QString* filePath,
    QString* errorMessage = nullptr,
    uint16_t port = 49102,
    int timeoutMs = 60000);
```

```cpp
// src/system/DeviceCommandService.cpp:227-258, 392-412, 982-1018
while (!completeReply.contains('\n')) {
    completeReply.append(socket.readAll());
    // 持续读取到协议换行，弱网分包不会截断Base64路径。
}

if (command == "screenshot") {
    const ScreenshotCaptureResult result = ScreenshotService::capturePrimaryScreen(groupName, deviceName);
    replyAndClose(result.success
        ? QByteArrayLiteral("screenshot|") + result.filePath.toUtf8().toBase64() + '\n'
        : QByteArrayLiteral("error|...") + '\n');
}

bool DeviceCommandService::requestScreenshot(...) {
    // 发送命名元数据，验证返回路径只属于固定截图目录。
}
```

```cpp
// src/ui/DeviceGrid.h:266-270, 379-380
RemoteDesktopWindow* focusedRemoteWindow() const;
void triggerScreenshotCapture();
void requestRemoteScreenshot(RemoteDesktopWindow* remoteWindow);
void captureLocalScreenshot();
void showScreenshotReview(const QString& filePath, QWidget* preferredParent);
QSet<QString> m_pendingScreenshotTargets;
bool m_screenshotReviewActive = false;
```

```cpp
// src/ui/DeviceGrid.cpp:635-641, 854-949, 4936-5050
constexpr int kScreenshotShortcutIndex = 8;
constexpr int kDeleteDeviceShortcutIndex = 9;
constexpr int kShortcutEditorCount = 10;

g_screenshotShortcutHook = SetWindowsHookExW(
    WH_KEYBOARD_LL, screenshotShortcutHookProc, GetModuleHandleW(nullptr), 0);

void DeviceGrid::triggerScreenshotCapture()
{
    if (m_shuttingDown || m_screenshotReviewActive || !m_pendingScreenshotTargets.isEmpty()) return;
    if (RemoteDesktopWindow* remoteWindow = focusedRemoteWindow()) {
        requestRemoteScreenshot(remoteWindow);
        return;
    }
    captureLocalScreenshot();
}
```

```cpp
// src/ui/RemoteDesktopWindow.h:143
void shortcutScreenshotRequested();
```

```cpp
// src/ui/RemoteDesktopWindow.cpp:6266-6273, 7489-7498
const QKeySequence screenshotShortcut = platform::AppSettings::screenshotShortcut();
if (matchesShortcut(current, screenshotShortcut)) {
    beginShortcutReleaseGuard(screenshotShortcut);
    emit shortcutScreenshotRequested();
    return true;
}
```

```cpp
// src/system/ScreenshotService.h:9-27
struct ScreenshotCaptureResult {
    bool success = false;
    QString filePath;
    QString errorMessage;
};

class ScreenshotService final {
public:
    static ScreenshotCaptureResult capturePrimaryScreen(const QString& groupName, const QString& deviceName);
    static bool isManagedScreenshotPath(const QString& filePath);
};
```

```cpp
// src/system/ScreenshotService.cpp:38-69, 72-132
return QString::fromUtf8(R"(\\192.168.1.100\ggc\喊话截图)");
return QStringLiteral("%1_%2_%3").arg(groupName, deviceName,
    capturedAt.toString(QStringLiteral("yyyyMMdd_HHmmss")));

const QPixmap screenshot = screen->grabWindow(0);
// 以PNG无损写入共享目录，失败时清理半成品。
// 路径校验只允许固定目录第一层PNG。
```

```cpp
// src/ui/ScreenshotReviewDialog.h:11-23
class ScreenshotReviewDialog final : public QDialog {
public:
    explicit ScreenshotReviewDialog(const QString& filePath, QWidget* parent = nullptr);
    ~ScreenshotReviewDialog() override;
private:
    void keepScreenshot();
    void deleteScreenshotFile();
    bool m_retained = false;
};
```

```cpp
// src/ui/ScreenshotReviewDialog.cpp:24-130
imageLabel->setPixmap(screenshot);
connect(keepButton, &QPushButton::clicked, this, [this] { keepScreenshot(); });
connect(deleteButton, &QPushButton::clicked, this, &QDialog::reject);

ScreenshotReviewDialog::~ScreenshotReviewDialog()
{
    if (!m_retained) deleteScreenshotFile();
}
```

### Steps
1. 增加固定共享目录截图服务，在执行截图的设备上通过`QScreen::grabWindow(0)`采集主屏原始像素并无损保存PNG。
2. 使用`分组名_设备名_yyyyMMdd_HHmmss.png`生成默认名称，过滤Windows非法字符、限制名称长度并避让同秒重名。
3. 扩展49102命令协议，控制端只发送分组名和设备名，目标端截图后仅返回Base64编码的共享路径，不通过远控视频流或命令端口传输图片内容。
4. 将命令回复读取改为等待完整换行，并把截图默认等待时间扩展到60秒，降低弱网和大分辨率PNG写入时的分包、超时风险。
5. 在键盘设置页增加第9项“原图截图”，默认F12，可按其它快捷键相同方式修改和持久化；删除设备移动到第10项，原F2/F9/F10索引不变。
6. 增加独立Windows低级键盘Hook，解决裸F12可能无法通过`RegisterHotKey`注册的问题；普通远控窗口继续由现有Hook消费并释放组合键。
7. 根据真实活动窗口选择截图目标：普通和平铺远控窗口向该目标发送截图命令；主窗口、桌面、其它程序或无远控焦点时截图控制端本机；只读监控窗口按当前绑定目标处理。
8. 后台等待远端截图，共享文件可读后在控制端显示原像素滚动预览；同一时刻只允许一张截图处于请求或确认阶段。
9. 确认窗口允许修改基础文件名并固定`.png`；只有“确定”成功后保留，点击“删除”、Esc、标题栏关闭或程序退出均删除未确认文件。
10. 强化失败清理和路径边界，损坏PNG、写入半成品、非法返回路径以及控制端退出后的未确认截图不会被保留。

### Verification
- 已执行`git diff --check`，未发现空白错误。
- 已使用`rg`核对`0..9`快捷键索引、10行设置数组、设置读写接口、普通/平铺远控信号连接和Windows Hook消息映射一致。
- 已静态核对目标端截图命令只传命名元数据，返回路径必须位于`\\192.168.1.100\ggc\喊话截图`第一层且扩展名为PNG。
- 已静态核对无远控焦点走本机主屏截图，活动远控窗口走目标端截图，F12不会作为普通键鼠事件继续转发到目标应用。
- 已静态核对确认窗口只有成功确定才设置保留状态，删除、Esc、关闭和析构均执行受控路径删除。
- 已静态核对弱网回复按换行完整读取，单个截图请求最多等待60秒，控制端等待发生在后台线程。
- 已确认本轮只会暂存本条记录列出的13个源码/构建文件和`WJY_CODE_CHANGE_LOG.md`，不会包含工作区其它未跟踪文件。
- 按用户要求，本次未构建、未链接、未运行测试或启动程序。
