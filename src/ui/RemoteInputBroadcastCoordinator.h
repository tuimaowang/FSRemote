#pragma once

#include <QByteArray>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSize>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QObject;

namespace ui {

// =====wjy====
enum class RemoteInputEventType {
    AbsoluteMove,
    RelativeMove,
    ButtonDown,
    ButtonUp,
    Wheel,
    KeyDown,
    KeyUp,
    CaptureRelease,
}; // wjy: 用语义事件隔离 Qt 输入采集与既有文本协议，协调器只复制键鼠含义而不会误广播剪贴板或画质消息。

struct RemoteInputEvent {
    RemoteInputEventType type = RemoteInputEventType::AbsoluteMove;
    int normalizedX = 32768;
    int normalizedY = 32768;
    double relativeX = 0.0;
    double relativeY = 0.0;
    int fallbackDeltaX = 0;
    int fallbackDeltaY = 0;
    int buttons = 0;
    int button = 0;
    int wheelDelta = 0;
    int virtualKey = 0;
}; // wjy: 绝对坐标固定为 0..65535，相对位移同时保存比例与回退像素，供不同分辨率目标各自序列化。

enum class RemoteInputSyncRole {
    Off,
    Master,
    Follower,
    Excluded,
}; // wjy: 同步开启后允许单个被控窗口进入“本机关闭”状态，第二次点击再安全切换为唯一主控。

QByteArray serializeRemoteInputEvent(const RemoteInputEvent& event, const QSize& remoteFrameSize); // wjy: 复用现有 m/r/d/u/w/k/c 文本协议，不增加 Host 端命令。
bool normalizedRemoteInputPoint(const QRect& imageRect, const QPoint& position, int* x, int* y); // wjy: 软件帧与纹理帧共用可单测的黑边排除和 0..65535 坐标换算。
QRect inputSyncButtonRectForClipboard(const QRect& clipboardRect, int titleBarHeight); // wjy: 标题栏布局计算独立测试按钮间距和热区大小。

class RemoteInputEndpoint {
public:
    virtual ~RemoteInputEndpoint() = default;
    virtual bool synchronizedInputEligible() const = 0;
    virtual QSize synchronizedInputFrameSize() const = 0;
    virtual bool sendSynchronizedInputEvent(const RemoteInputEvent& event) = 0;
    virtual void synchronizedInputRoleChanged(RemoteInputSyncRole role) = 0;
}; // wjy: 窄接口让生产窗口和无网络测试替身共用同一套主控、扇出与安全释放逻辑。

class RemoteInputBroadcastCoordinator final {
public:
    RemoteInputBroadcastCoordinator() = default;
    ~RemoteInputBroadcastCoordinator();

    void registerEndpoint(QObject* lifetimeGuard, RemoteInputEndpoint* endpoint);
    void unregisterEndpoint(RemoteInputEndpoint* endpoint);
    bool toggleMaster(RemoteInputEndpoint* endpoint);
    void disableSynchronization();
    void notifyEligibilityChanged(RemoteInputEndpoint* endpoint);

    RemoteInputSyncRole roleFor(const RemoteInputEndpoint* endpoint) const;
    std::uint64_t capturedGenerationFor(const RemoteInputEndpoint* endpoint) const;
    bool routeInput(RemoteInputEndpoint* origin, const RemoteInputEvent& event, std::uint64_t capturedGeneration);

    RemoteInputEndpoint* master() const;
    std::uint64_t generation() const;
    std::size_t endpointCount() const;

private:
    struct Entry {
        QPointer<QObject> guard;
        RemoteInputEndpoint* endpoint = nullptr;
    };

    struct HeldState {
        std::unordered_set<int> keys;
        std::unordered_set<int> buttons;
        int normalizedX = 32768;
        int normalizedY = 32768;
    };

    Entry* findEntry(RemoteInputEndpoint* endpoint);
    const Entry* findEntry(const RemoteInputEndpoint* endpoint) const;
    void pruneExpired();
    void notifyRoles();
    void trackSuccessfulDelivery(RemoteInputEndpoint* endpoint, const RemoteInputEvent& event);
    void releaseEndpoint(RemoteInputEndpoint* endpoint);
    void releaseAll();

    std::vector<Entry> m_entries;
    std::unordered_map<RemoteInputEndpoint*, HeldState> m_heldByEndpoint;
    std::unordered_set<RemoteInputEndpoint*> m_excludedEndpoints; // wjy: 当前同步组内主动关闭本设备同步的端点集合，关闭整组时统一清空。
    RemoteInputEndpoint* m_master = nullptr;
    std::uint64_t m_generation = 0;
};
// ===end====

} // namespace ui
