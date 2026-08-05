#include "ui/RemoteInputBroadcastCoordinator.h"
#include "ui/RemoteTitleBarLayout.h"

#include <QObject>

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

// =====wjy====
class FakeEndpoint final : public QObject, public ui::RemoteInputEndpoint {
public:
    bool synchronizedInputEligible() const override
    {
        return eligible;
    }

    QSize synchronizedInputFrameSize() const override
    {
        return frameSize;
    }

    bool sendSynchronizedInputEvent(const ui::RemoteInputEvent& event) override
    {
        if (!eligible || failSends) {
            return false; // wjy: 测试替身可以模拟断线或单目标发送失败，验证扇出隔离和不可达释放兜底。
        }
        events.push_back(event);
        messages.push_back(ui::serializeRemoteInputEvent(event, frameSize));
        return true;
    }

    void synchronizedInputRoleChanged(ui::RemoteInputSyncRole nextRole) override
    {
        role = nextRole;
        ++roleChangeCount;
    }

    void clear()
    {
        events.clear();
        messages.clear();
    }

    bool contains(const QByteArray& message) const
    {
        for (const QByteArray& candidate : messages) {
            if (candidate == message) return true;
        }
        return false;
    }

    bool eligible = true;
    bool failSends = false;
    QSize frameSize = QSize(1920, 1080);
    ui::RemoteInputSyncRole role = ui::RemoteInputSyncRole::Off;
    int roleChangeCount = 0;
    std::vector<ui::RemoteInputEvent> events;
    std::vector<QByteArray> messages;
};

ui::RemoteInputEvent keyEvent(int virtualKey, bool down)
{
    ui::RemoteInputEvent event;
    event.type = down ? ui::RemoteInputEventType::KeyDown : ui::RemoteInputEventType::KeyUp;
    event.virtualKey = virtualKey;
    return event;
}

ui::RemoteInputEvent buttonEvent(int button, bool down, int x, int y)
{
    ui::RemoteInputEvent event;
    event.type = down ? ui::RemoteInputEventType::ButtonDown : ui::RemoteInputEventType::ButtonUp;
    event.button = button;
    event.normalizedX = x;
    event.normalizedY = y;
    return event;
}

ui::RemoteInputEvent moveEvent(int x, int y, int buttons = 0)
{
    ui::RemoteInputEvent event;
    event.type = ui::RemoteInputEventType::AbsoluteMove;
    event.normalizedX = x;
    event.normalizedY = y;
    event.buttons = buttons;
    return event;
}
// ===end====

void testRolesRoutingAndGeneration()
{
    FakeEndpoint first;
    FakeEndpoint second;
    FakeEndpoint third;
    ui::RemoteInputBroadcastCoordinator coordinator;
    coordinator.registerEndpoint(&first, &first);
    coordinator.registerEndpoint(&second, &second);
    coordinator.registerEndpoint(&third, &third);
    assert(coordinator.endpointCount() == 3);
    assert(first.role == ui::RemoteInputSyncRole::Off);

    assert(coordinator.toggleMaster(&first));
    assert(coordinator.master() == &first);
    assert(first.role == ui::RemoteInputSyncRole::Master);
    assert(second.role == ui::RemoteInputSyncRole::Follower);
    assert(third.role == ui::RemoteInputSyncRole::Follower);

    const std::uint64_t firstGeneration = coordinator.capturedGenerationFor(&first);
    assert(firstGeneration != 0);
    assert(coordinator.routeInput(&first, keyEvent(65, true), firstGeneration));
    assert(first.contains("k 65 1"));
    assert(second.contains("k 65 1"));
    assert(third.contains("k 65 1"));

    first.clear();
    second.clear();
    third.clear();
    assert(coordinator.routeInput(&second, keyEvent(66, true), 0));
    assert(first.messages.empty());
    assert(second.contains("k 66 1"));
    assert(third.messages.empty()); // wjy: 跟随窗口输入只到自身，不因当前组已开启而反向成为广播源。

    const std::uint64_t staleGeneration = coordinator.generation();
    assert(coordinator.toggleMaster(&second));
    assert(coordinator.master() == &first);
    assert(second.role == ui::RemoteInputSyncRole::Excluded); // wjy: 跟随端第一次点击只关闭本设备同步，不直接抢占当前主控。
    first.clear();
    second.clear();
    third.clear();
    coordinator.routeInput(&first, keyEvent(69, true), firstGeneration);
    assert(first.contains("k 69 1"));
    assert(second.messages.empty());
    assert(third.contains("k 69 1")); // wjy: 本机关闭端不再接收广播，其余同步成员继续正常跟随。

    assert(coordinator.toggleMaster(&second));
    assert(coordinator.master() == &second);
    assert(second.role == ui::RemoteInputSyncRole::Master); // wjy: 第二次点击才经过全组释放屏障切换为唯一主控。
    first.clear();
    second.clear();
    third.clear();
    assert(coordinator.routeInput(&first, keyEvent(67, true), staleGeneration));
    assert(first.contains("k 67 1"));
    assert(second.messages.empty());
    assert(third.messages.empty()); // wjy: 旧主控迟到输入保留其单窗口语义，但不能越过代际进入新同步组。
}

void testFailureIsolationAndDynamicMembership()
{
    FakeEndpoint master;
    FakeEndpoint failedFollower;
    FakeEndpoint healthyFollower;
    ui::RemoteInputBroadcastCoordinator coordinator;
    coordinator.registerEndpoint(&master, &master);
    coordinator.registerEndpoint(&failedFollower, &failedFollower);
    coordinator.registerEndpoint(&healthyFollower, &healthyFollower);
    assert(coordinator.toggleMaster(&master));

    failedFollower.failSends = true;
    const std::uint64_t generation = coordinator.capturedGenerationFor(&master);
    assert(coordinator.routeInput(&master, moveEvent(1000, 2000), generation));
    assert(master.contains("m 1000 2000 0"));
    assert(failedFollower.messages.empty());
    assert(healthyFollower.contains("m 1000 2000 0"));

    FakeEndpoint lateFollower;
    coordinator.registerEndpoint(&lateFollower, &lateFollower);
    assert(lateFollower.role == ui::RemoteInputSyncRole::Follower);
    assert(lateFollower.messages.empty());
    coordinator.routeInput(&master, keyEvent(68, true), generation);
    assert(lateFollower.contains("k 68 1")); // wjy: 中途登记只接收后续事件，没有任何历史事件重放。

    coordinator.unregisterEndpoint(&failedFollower);
    coordinator.unregisterEndpoint(&healthyFollower);
    coordinator.unregisterEndpoint(&lateFollower);
    assert(coordinator.master() == &master);
    assert(master.role == ui::RemoteInputSyncRole::Master); // wjy: 仅剩主控时保持开启，之后新增窗口可以自动成为跟随端。
}

void testReleaseBarriersAndEligibilityLoss()
{
    FakeEndpoint first;
    FakeEndpoint second;
    FakeEndpoint third;
    ui::RemoteInputBroadcastCoordinator coordinator;
    coordinator.registerEndpoint(&first, &first);
    coordinator.registerEndpoint(&second, &second);
    coordinator.registerEndpoint(&third, &third);
    assert(coordinator.toggleMaster(&first));
    const std::uint64_t generation = coordinator.capturedGenerationFor(&first);
    coordinator.routeInput(&first, keyEvent(70, true), generation);
    coordinator.routeInput(&first, buttonEvent(1, true, 1234, 5678), generation);

    assert(coordinator.toggleMaster(&second));
    assert(second.role == ui::RemoteInputSyncRole::Excluded);
    assert(coordinator.toggleMaster(&second));
    for (FakeEndpoint* endpoint : {&first, &second, &third}) {
        assert(endpoint->contains("k 70 0"));
        assert(endpoint->contains("u 1 1234 5678"));
        assert(endpoint->contains("c 0"));
    }

    first.clear();
    second.clear();
    third.clear();
    const std::uint64_t secondGeneration = coordinator.capturedGenerationFor(&second);
    coordinator.routeInput(&second, keyEvent(71, true), secondGeneration);
    third.eligible = false;
    coordinator.notifyEligibilityChanged(&third);
    coordinator.routeInput(&second, keyEvent(72, true), secondGeneration);
    assert(!third.contains("k 72 1")); // wjy: 不可达跟随端被排除，释放失败不会阻断主控和健康目标。

    second.eligible = false;
    coordinator.notifyEligibilityChanged(&second);
    assert(coordinator.master() == nullptr);
    assert(first.role == ui::RemoteInputSyncRole::Off);
    assert(third.role == ui::RemoteInputSyncRole::Off); // wjy: 主控失效关闭整组，不自动提升任一跟随窗口。

    second.eligible = true;
    assert(coordinator.toggleMaster(&second));
    assert(first.role == ui::RemoteInputSyncRole::Follower);
    assert(third.role == ui::RemoteInputSyncRole::Follower); // wjy: 新一轮同步已清空上一轮的本机关闭状态。
}

void testExpiredQObjectGuard()
{
    ui::RemoteInputBroadcastCoordinator coordinator;
    auto* expiredMaster = new FakeEndpoint();
    coordinator.registerEndpoint(expiredMaster, expiredMaster);
    assert(coordinator.toggleMaster(expiredMaster));
    delete expiredMaster;

    FakeEndpoint survivor;
    coordinator.registerEndpoint(&survivor, &survivor); // wjy: 下一次协调器操作清理空 QPointer，过程不得解引用已销毁接口。
    assert(coordinator.master() == nullptr);
    assert(survivor.role == ui::RemoteInputSyncRole::Off);
}

void testSerializationAndMixedResolutions()
{
    ui::RemoteInputEvent absolute = moveEvent(-10, 70000, 5);
    assert(ui::serializeRemoteInputEvent(absolute, QSize(1920, 1080)) == "m 0 65535 5");

    ui::RemoteInputEvent relative;
    relative.type = ui::RemoteInputEventType::RelativeMove;
    relative.relativeX = 0.01;
    relative.relativeY = -0.01;
    relative.fallbackDeltaX = 7;
    relative.fallbackDeltaY = -8;
    relative.buttons = 1;
    assert(ui::serializeRemoteInputEvent(relative, QSize(1920, 1080)) == "r 19 -11 1");
    assert(ui::serializeRemoteInputEvent(relative, QSize(3840, 2160)) == "r 38 -22 1");
    assert(ui::serializeRemoteInputEvent(relative, QSize()) == "r 7 -8 1");

    relative.relativeX = 1.0;
    relative.relativeY = -1.0;
    assert(ui::serializeRemoteInputEvent(relative, QSize(7680, 4320)) == "r 200 -200 1"); // wjy: 混合分辨率按比例换算，同时保持协议的单事件安全边界。

    ui::RemoteInputEvent wheel;
    wheel.type = ui::RemoteInputEventType::Wheel;
    wheel.wheelDelta = -120;
    wheel.normalizedX = 30000;
    wheel.normalizedY = 40000;
    assert(ui::serializeRemoteInputEvent(wheel, QSize(1280, 720)) == "w -120 30000 40000");

    int normalizedX = -1;
    int normalizedY = -1;
    const QRect letterboxedImage(100, 50, 801, 451);
    assert(!ui::normalizedRemoteInputPoint(letterboxedImage, QPoint(99, 100), &normalizedX, &normalizedY));
    assert(ui::normalizedRemoteInputPoint(letterboxedImage, letterboxedImage.topLeft(), &normalizedX, &normalizedY));
    assert(normalizedX == 0 && normalizedY == 0);
    assert(ui::normalizedRemoteInputPoint(letterboxedImage, letterboxedImage.bottomRight(), &normalizedX, &normalizedY));
    assert(normalizedX == 65535 && normalizedY == 65535); // wjy: 黑边外拒绝，画面两个端点精确覆盖完整归一化范围。

    const QRect clipboardRect(840, 0, 28, 28);
    const QRect syncRect = ui::inputSyncButtonRectForClipboard(clipboardRect, 28);
    assert(syncRect == QRect(790, 0, 46, 28));
    assert(!syncRect.intersects(clipboardRect));
    assert(clipboardRect.left() - syncRect.right() - 1 == 4); // wjy: QRect 右边界为闭区间，扣除端点后验证两个按钮之间恰有 4px 空隙。
}

void testTwentyWindowMouseFanout()
{
    std::vector<std::unique_ptr<FakeEndpoint>> endpoints;
    endpoints.reserve(20);
    for (int index = 0; index < 20; ++index) {
        endpoints.push_back(std::make_unique<FakeEndpoint>());
    }
    ui::RemoteInputBroadcastCoordinator coordinator;
    for (const auto& endpoint : endpoints) {
        coordinator.registerEndpoint(endpoint.get(), endpoint.get());
    }
    assert(coordinator.toggleMaster(endpoints.front().get()));
    const std::uint64_t generation = coordinator.capturedGenerationFor(endpoints.front().get());
    for (int index = 0; index < 1000; ++index) {
        coordinator.routeInput(endpoints.front().get(), moveEvent(index, index * 2), generation);
    }
    for (const auto& endpoint : endpoints) {
        assert(endpoint->messages.size() == 1000); // wjy: 20 路高频移动保持每目标每事件一次，没有遗漏、重复或额外队列。
    }
}

void testRemoteTitleBarLayoutVisibility()
{
    const ui::RemoteTitleBarLayoutSnapshot normal = ui::remoteTitleBarLayoutSnapshot(
        920, 28, 250, true, 6);
    assert(!normal.update.isEmpty());
    assert(!normal.mouseBackend.isEmpty());
    assert(!normal.inputSync.isEmpty());
    assert(!normal.clipboard.isEmpty());
    assert(!normal.minimize.isEmpty());
    assert(!normal.close.isEmpty()); // wjy: 正常宽度下设备信息与整条按钮链互不覆盖，全部保持可见可点击。

    const ui::RemoteTitleBarLayoutSnapshot narrow = ui::remoteTitleBarLayoutSnapshot(
        260, 28, 80, true, 6);
    assert(narrow.update.isEmpty());
    assert(narrow.mouseBackend.isEmpty());
    assert(!narrow.inputSync.isEmpty());
    assert(!narrow.clipboard.isEmpty());
    assert(!narrow.minimize.isEmpty());
    assert(!narrow.close.isEmpty()); // wjy: 窄窗口按从左到右的覆盖结果隐藏低优先级控件，不留下部分可见热区。

    const ui::RemoteTitleBarLayoutSnapshot minimum = ui::remoteTitleBarLayoutSnapshot(
        100, 28, 99, true, 6);
    assert(minimum.update.isEmpty());
    assert(minimum.mouseBackend.isEmpty());
    assert(minimum.inputSync.isEmpty());
    assert(minimum.clipboard.isEmpty());
    assert(minimum.minimize.isEmpty());
    assert(minimum.close.isEmpty()); // wjy: 设备信息完全覆盖标题栏时所有按钮视觉和命中同时关闭。
}

} // namespace

int main()
{
    testRolesRoutingAndGeneration();
    testFailureIsolationAndDynamicMembership();
    testReleaseBarriersAndEligibilityLoss();
    testExpiredQObjectGuard();
    testSerializationAndMixedResolutions();
    testTwentyWindowMouseFanout();
    testRemoteTitleBarLayoutVisibility();
    return 0;
}
