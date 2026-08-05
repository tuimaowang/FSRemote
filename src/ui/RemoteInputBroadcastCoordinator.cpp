#include "ui/RemoteInputBroadcastCoordinator.h"

#include <QObject>

#include <algorithm>
#include <cmath>

namespace ui {
namespace {

// =====wjy====
int normalizedCoordinate(int value)
{
    return std::clamp(value, 0, 65535); // wjy: 所有绝对输入在序列化前统一夹紧，防止窗口边缘取整越过 Host 接受范围。
}

int relativePixels(double fraction, int fallback, int dimension)
{
    const int pixels = dimension > 0
        ? static_cast<int>(std::lround(fraction * static_cast<double>(dimension)))
        : fallback; // wjy: 有目标分辨率时按比例还原本机像素；首帧尺寸未知时保持源窗口已经计算出的安全回退量。
    return std::clamp(pixels, -200, 200); // wjy: 与 Host 当前相对鼠标保护范围一致，单个异常移动不能制造大幅跳变。
}
// ===end====

} // namespace

// =====wjy====
QByteArray serializeRemoteInputEvent(const RemoteInputEvent& event, const QSize& remoteFrameSize)
{
    const int x = normalizedCoordinate(event.normalizedX);
    const int y = normalizedCoordinate(event.normalizedY);
    switch (event.type) {
    case RemoteInputEventType::AbsoluteMove:
        return QByteArray("m ") + QByteArray::number(x) + ' '
            + QByteArray::number(y) + ' ' + QByteArray::number(event.buttons); // wjy: 绝对移动沿用归一化坐标和当前按钮掩码。
    case RemoteInputEventType::RelativeMove: {
        const int dx = relativePixels(event.relativeX, event.fallbackDeltaX, remoteFrameSize.width());
        const int dy = relativePixels(event.relativeY, event.fallbackDeltaY, remoteFrameSize.height());
        return QByteArray("r ") + QByteArray::number(dx) + ' '
            + QByteArray::number(dy) + ' ' + QByteArray::number(event.buttons); // wjy: 每个目标按自己的远端宽高换算同一相对移动比例。
    }
    case RemoteInputEventType::ButtonDown:
        return QByteArray("d ") + QByteArray::number(event.button) + ' '
            + QByteArray::number(x) + ' ' + QByteArray::number(y);
    case RemoteInputEventType::ButtonUp:
        return QByteArray("u ") + QByteArray::number(event.button) + ' '
            + QByteArray::number(x) + ' ' + QByteArray::number(y);
    case RemoteInputEventType::Wheel:
        return QByteArray("w ") + QByteArray::number(event.wheelDelta) + ' '
            + QByteArray::number(x) + ' ' + QByteArray::number(y);
    case RemoteInputEventType::KeyDown:
        return QByteArray("k ") + QByteArray::number(event.virtualKey) + " 1";
    case RemoteInputEventType::KeyUp:
        return QByteArray("k ") + QByteArray::number(event.virtualKey) + " 0";
    case RemoteInputEventType::CaptureRelease:
        return QByteArray("c 0"); // wjy: 主控切换或退出时同步退出所有目标的相对鼠标捕获状态。
    }
    return {};
}

bool normalizedRemoteInputPoint(const QRect& imageRect, const QPoint& position, int* x, int* y)
{
    if (!imageRect.contains(position) || imageRect.width() <= 1 || imageRect.height() <= 1) {
        return false; // wjy: 标题栏、边框和等比缩放黑边不属于远端画面，不能产生同步鼠标事件。
    }
    if (x) {
        const qint64 numerator = qint64(position.x() - imageRect.left()) * 65535;
        *x = normalizedCoordinate(static_cast<int>(numerator / (imageRect.width() - 1))); // wjy: 64 位乘法避免超宽窗口坐标换算溢出。
    }
    if (y) {
        const qint64 numerator = qint64(position.y() - imageRect.top()) * 65535;
        *y = normalizedCoordinate(static_cast<int>(numerator / (imageRect.height() - 1)));
    }
    return true;
}

QRect inputSyncButtonRectForClipboard(const QRect& clipboardRect, int titleBarHeight)
{
    return QRect(clipboardRect.left() - 50, 0, 46, std::max(0, titleBarHeight)); // wjy: 46px 胶囊可直接显示同步角色文字，并与剪切板保持 4px 间隔。
}

RemoteInputBroadcastCoordinator::~RemoteInputBroadcastCoordinator()
{
    pruneExpired();
    releaseAll(); // wjy: DeviceGrid 兜底析构时对仍可达会话释放同步持有状态，不把按键留在目标端。
}

void RemoteInputBroadcastCoordinator::registerEndpoint(QObject* lifetimeGuard, RemoteInputEndpoint* endpoint)
{
    if (!lifetimeGuard || !endpoint) {
        return;
    }
    pruneExpired();
    if (Entry* existing = findEntry(endpoint)) {
        existing->guard = lifetimeGuard; // wjy: 重复登记只刷新生命周期守卫，不产生重复扇出目标。
        endpoint->synchronizedInputRoleChanged(roleFor(endpoint));
        return;
    }
    m_entries.push_back({QPointer<QObject>(lifetimeGuard), endpoint});
    m_excludedEndpoints.erase(endpoint); // wjy: 新生命周期默认参与当前同步组，清除同地址旧对象可能遗留的本机关闭标记。
    endpoint->synchronizedInputRoleChanged(roleFor(endpoint)); // wjy: 同步已开启时新窗口立即显示跟随态，但不会补发历史输入。
}

void RemoteInputBroadcastCoordinator::unregisterEndpoint(RemoteInputEndpoint* endpoint)
{
    if (!endpoint) {
        return;
    }
    pruneExpired();
    const auto iterator = std::find_if(m_entries.begin(), m_entries.end(), [endpoint](const Entry& entry) {
        return entry.endpoint == endpoint;
    });
    if (iterator == m_entries.end()) {
        m_heldByEndpoint.erase(endpoint);
        m_excludedEndpoints.erase(endpoint);
        return;
    }

    if (m_master == endpoint) {
        releaseAll(); // wjy: 主控销毁前先释放整组，再清除主控指针，剩余窗口不会被隐式选为新主控。
        m_master = nullptr;
        m_excludedEndpoints.clear(); // wjy: 主控退出即结束本轮同步，所有幸存窗口恢复默认参与状态。
        ++m_generation;
    } else {
        releaseEndpoint(endpoint); // wjy: 跟随窗口退出只释放自身状态，其余窗口继续同步。
    }
    m_heldByEndpoint.erase(endpoint);
    m_excludedEndpoints.erase(endpoint);
    m_entries.erase(iterator);
    notifyRoles();
}

bool RemoteInputBroadcastCoordinator::toggleMaster(RemoteInputEndpoint* endpoint)
{
    pruneExpired();
    const Entry* entry = findEntry(endpoint);
    if (!entry || entry->guard.isNull() || !endpoint->synchronizedInputEligible()) {
        return false; // wjy: 尚未连接或正在更新的窗口保持关闭态，不能成为无法产生可靠输入的主控。
    }
    if (!m_master) {
        m_excludedEndpoints.clear(); // wjy: 从全局关闭状态开启新同步组时不继承上一轮的本机关闭选择。
        m_master = endpoint;
        ++m_generation;
        notifyRoles();
        return true;
    }
    if (m_master == endpoint) {
        disableSynchronization();
        return true;
    }

    if (!m_excludedEndpoints.contains(endpoint)) {
        releaseEndpoint(endpoint); // wjy: 跟随端第一次点击先释放该设备可能持有的同步按键和鼠标状态。
        m_excludedEndpoints.insert(endpoint);
        notifyRoles();
        return true;
    }

    releaseAll(); // wjy: 发布新主控前形成释放屏障，旧主控按下的键和鼠标按钮不会跨代际泄漏。
    m_master = endpoint;
    m_excludedEndpoints.erase(endpoint); // wjy: 本机关闭端第二次点击成为主控，自身重新加入同步参与集合。
    ++m_generation;
    notifyRoles();
    return true;
}

void RemoteInputBroadcastCoordinator::disableSynchronization()
{
    pruneExpired();
    if (!m_master && m_heldByEndpoint.empty() && m_excludedEndpoints.empty()) {
        return;
    }
    releaseAll();
    m_master = nullptr;
    m_excludedEndpoints.clear(); // wjy: 主控关闭整组时清空全部单设备排除状态，下一次开启从一致状态开始。
    ++m_generation; // wjy: 即使旧输入已经排队，代际推进也会阻止它进入之后重新开启的同步组。
    notifyRoles();
}

void RemoteInputBroadcastCoordinator::notifyEligibilityChanged(RemoteInputEndpoint* endpoint)
{
    pruneExpired();
    if (!findEntry(endpoint) || endpoint->synchronizedInputEligible()) {
        return;
    }
    if (m_master == endpoint) {
        disableSynchronization(); // wjy: 主控断开、关闭或更新时整组关闭，不猜测用户希望哪个跟随窗口接管。
        return;
    }
    releaseEndpoint(endpoint); // wjy: 跟随端失效只尽力清理该目标，主控与其它跟随端不受影响。
}

RemoteInputSyncRole RemoteInputBroadcastCoordinator::roleFor(const RemoteInputEndpoint* endpoint) const
{
    if (!endpoint || !findEntry(endpoint) || !m_master) {
        return RemoteInputSyncRole::Off;
    }
    if (endpoint == m_master) {
        return RemoteInputSyncRole::Master;
    }
    return m_excludedEndpoints.contains(const_cast<RemoteInputEndpoint*>(endpoint))
        ? RemoteInputSyncRole::Excluded
        : RemoteInputSyncRole::Follower; // wjy: 非主控窗口明确区分正在跟随和仅本机关闭两种状态。
}

std::uint64_t RemoteInputBroadcastCoordinator::capturedGenerationFor(const RemoteInputEndpoint* endpoint) const
{
    return endpoint && endpoint == m_master ? m_generation : 0; // wjy: 只有主控输入携带广播代际，跟随窗口的直接操作永远保持本地单路。
}

bool RemoteInputBroadcastCoordinator::routeInput(
    RemoteInputEndpoint* origin,
    const RemoteInputEvent& event,
    std::uint64_t capturedGeneration)
{
    if (!origin) {
        return false;
    }
    pruneExpired();
    const bool originSent = origin->sendSynchronizedInputEvent(event); // wjy: 先且只向来源窗口发送一次，保持关闭同步时的原有单窗口行为。
    const bool broadcast = m_master == origin
        && capturedGeneration != 0
        && capturedGeneration == m_generation;
    if (!broadcast) {
        return originSent; // wjy: 跟随来源或旧代际事件不得进入当前同步组，但来源自身仍按原路径处理。
    }
    if (originSent) {
        trackSuccessfulDelivery(origin, event);
    }

    const std::vector<Entry> snapshot = m_entries; // wjy: 对稳定快照扇出，发送过程中的 Qt 生命周期变化不会使 vector 迭代器失效。
    for (const Entry& entry : snapshot) {
        RemoteInputEndpoint* target = entry.endpoint;
        if (!target || target == origin || entry.guard.isNull()
            || m_excludedEndpoints.contains(target) || !target->synchronizedInputEligible()) {
            continue;
        } // wjy: 本机关闭设备仍可直接操作自身，但不再接收主控扇出的后续事件。
        if (target->sendSynchronizedInputEvent(event)) {
            trackSuccessfulDelivery(target, event); // wjy: 单个发送失败不抛出也不中断循环，其余目标仍各自尝试一次。
        }
    }
    return originSent;
}

RemoteInputEndpoint* RemoteInputBroadcastCoordinator::master() const
{
    return m_master;
}

std::uint64_t RemoteInputBroadcastCoordinator::generation() const
{
    return m_generation;
}

std::size_t RemoteInputBroadcastCoordinator::endpointCount() const
{
    return m_entries.size();
}

RemoteInputBroadcastCoordinator::Entry* RemoteInputBroadcastCoordinator::findEntry(RemoteInputEndpoint* endpoint)
{
    const auto iterator = std::find_if(m_entries.begin(), m_entries.end(), [endpoint](const Entry& entry) {
        return entry.endpoint == endpoint;
    });
    return iterator == m_entries.end() ? nullptr : &*iterator;
}

const RemoteInputBroadcastCoordinator::Entry* RemoteInputBroadcastCoordinator::findEntry(
    const RemoteInputEndpoint* endpoint) const
{
    const auto iterator = std::find_if(m_entries.cbegin(), m_entries.cend(), [endpoint](const Entry& entry) {
        return entry.endpoint == endpoint;
    });
    return iterator == m_entries.cend() ? nullptr : &*iterator;
}

void RemoteInputBroadcastCoordinator::pruneExpired()
{
    bool masterExpired = false;
    for (auto iterator = m_entries.begin(); iterator != m_entries.end();) {
        if (!iterator->guard.isNull()) {
            ++iterator;
            continue;
        }
        masterExpired = masterExpired || iterator->endpoint == m_master;
        m_heldByEndpoint.erase(iterator->endpoint);
        m_excludedEndpoints.erase(iterator->endpoint);
        iterator = m_entries.erase(iterator); // wjy: QObject 已销毁时只按指针值清理容器，绝不解引用悬空接口。
    }
    if (masterExpired) {
        m_master = nullptr;
        m_excludedEndpoints.clear(); // wjy: 主控异常销毁关闭整组时同步清除幸存设备的本机关闭状态。
        ++m_generation;
        releaseAll();
        notifyRoles(); // wjy: 未显式注销的异常销毁同样关闭整组并刷新剩余窗口状态。
    }
}

void RemoteInputBroadcastCoordinator::notifyRoles()
{
    const std::vector<Entry> snapshot = m_entries;
    for (const Entry& entry : snapshot) {
        if (entry.endpoint && !entry.guard.isNull()) {
            entry.endpoint->synchronizedInputRoleChanged(roleFor(entry.endpoint)); // wjy: 所有标题栏在同一 UI 调用栈内看到同一个主控快照。
        }
    }
}

void RemoteInputBroadcastCoordinator::trackSuccessfulDelivery(
    RemoteInputEndpoint* endpoint,
    const RemoteInputEvent& event)
{
    HeldState& state = m_heldByEndpoint[endpoint];
    switch (event.type) {
    case RemoteInputEventType::AbsoluteMove:
    case RemoteInputEventType::ButtonDown:
    case RemoteInputEventType::ButtonUp:
    case RemoteInputEventType::Wheel:
        state.normalizedX = normalizedCoordinate(event.normalizedX);
        state.normalizedY = normalizedCoordinate(event.normalizedY); // wjy: 保存每个目标最后位置，强制抬起鼠标时继续使用有效远端坐标。
        break;
    default:
        break;
    }
    if (event.type == RemoteInputEventType::KeyDown && event.virtualKey > 0) {
        state.keys.insert(event.virtualKey);
    } else if (event.type == RemoteInputEventType::KeyUp) {
        state.keys.erase(event.virtualKey);
    } else if (event.type == RemoteInputEventType::ButtonDown && event.button > 0) {
        state.buttons.insert(event.button);
    } else if (event.type == RemoteInputEventType::ButtonUp) {
        state.buttons.erase(event.button); // wjy: 自动重复 down 由集合去重，切换主控时每个逻辑键或按钮只补一次 up。
    }
}

void RemoteInputBroadcastCoordinator::releaseEndpoint(RemoteInputEndpoint* endpoint)
{
    const auto iterator = m_heldByEndpoint.find(endpoint);
    if (!endpoint || iterator == m_heldByEndpoint.end()) {
        return;
    }
    HeldState state = iterator->second;
    m_heldByEndpoint.erase(iterator); // wjy: 先移除本地持有记录，释放发送即使失败也不会在重入路径重复循环。

    for (int key : state.keys) {
        RemoteInputEvent release;
        release.type = RemoteInputEventType::KeyUp;
        release.virtualKey = key;
        endpoint->sendSynchronizedInputEvent(release); // wjy: 可达目标收到每个同步按键的配对抬起，失败由会话断开清理兜底。
    }
    for (int button : state.buttons) {
        RemoteInputEvent release;
        release.type = RemoteInputEventType::ButtonUp;
        release.button = button;
        release.normalizedX = state.normalizedX;
        release.normalizedY = state.normalizedY;
        endpoint->sendSynchronizedInputEvent(release);
    }
    RemoteInputEvent captureRelease;
    captureRelease.type = RemoteInputEventType::CaptureRelease;
    endpoint->sendSynchronizedInputEvent(captureRelease); // wjy: 无论是否有按钮按下都退出该会话可能存在的相对鼠标捕获。
}

void RemoteInputBroadcastCoordinator::releaseAll()
{
    std::vector<RemoteInputEndpoint*> endpoints;
    endpoints.reserve(m_heldByEndpoint.size());
    for (const auto& item : m_heldByEndpoint) {
        endpoints.push_back(item.first); // wjy: releaseEndpoint 会删除 map 项，先复制键避免迭代器失效。
    }
    for (RemoteInputEndpoint* endpoint : endpoints) {
        const Entry* entry = findEntry(endpoint);
        if (entry && !entry->guard.isNull()) {
            releaseEndpoint(endpoint);
        } else {
            m_heldByEndpoint.erase(endpoint);
        }
    }
}
// ===end====

} // namespace ui
