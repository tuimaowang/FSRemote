#include "ui/RemoteWindowCompositor.h"

#include <cassert>

using ui::RemoteWindowCompositor;
using ui::RemoteWindowCompositorState;
using ui::RemoteWindowLayoutSnapshot;

namespace {

RemoteWindowLayoutSnapshot layout(int width, int height, qreal dpr = 1.0)
{
    RemoteWindowLayoutSnapshot snapshot;
    snapshot.physicalOutputRect = QRect(0, 0, width, height);
    snapshot.titleBarRect = QRect(0, 0, width, 28);
    snapshot.contentRect = QRect(0, 28, width, height - 28);
    snapshot.inputRect = snapshot.contentRect;
    snapshot.outputSize = QSize(width, height - 28);
    snapshot.devicePixelRatio = dpr;
    return snapshot;
}

void runResizeScenarios()
{
    RemoteWindowCompositor compositor(true);
    compositor.commitLayout(layout(1280, 748)); // 普通/移动后的稳定布局。
    assert(compositor.state() == RemoteWindowCompositorState::Idle);
    assert(compositor.layout().revision == 1);
    compositor.commitLayout(layout(1920, 1080)); // 最大化后的稳定输出几何。
    compositor.commitLayout(layout(1280, 748)); // 还原后必须回到同一提交入口，不能残留最大化布局。

    compositor.beginInteractiveResize(layout(1281, 749)); // 边缘慢速拖拽第一步。
    compositor.updateInteractiveGeometry(layout(1600, 900)); // 快速连续几何变化只更新快照。
    assert(compositor.state() == RemoteWindowCompositorState::InteractiveResize);
    assert(compositor.layout().sourceSize.isEmpty()); // 尚未收到新视频帧时不得伪造源尺寸。

    compositor.finalizeResize(layout(1600, 900, 1.25)); // 松手、DPI 变化和最终提交合并为一次收尾。
    assert(compositor.state() == RemoteWindowCompositorState::Idle);
    assert(compositor.layout().devicePixelRatio == 1.25);
    assert(compositor.layoutCommitCount() == 6);

    RemoteWindowLayoutSnapshot stale = layout(800, 600);
    stale.revision = compositor.layout().revision;
    compositor.commitLayout(stale);
    assert(compositor.layout().physicalOutputRect.size() == QSize(1600, 900));
    RemoteWindowLayoutSnapshot invalid;
    compositor.commitLayout(invalid);
    assert(compositor.rejectedLayoutCount() == 2); // 迟到版本和无效几何都保留上一份完整快照。

    compositor.markHardwareFrame(41, QSize(1920, 1080));
    assert(compositor.lastFrameId() == 41);
    assert(compositor.presentedFrameCount() == 1);

    compositor.enterHardwareFallback(); // 设备丢失前先进入软件保活。
    assert(compositor.state() == RemoteWindowCompositorState::HardwareFallback);
    compositor.markSoftwareFrame(42, QSize(1920, 1080));
    compositor.enterDeviceRecovery(); // 重建设备期间保留最后有效软件帧。
    assert(compositor.state() == RemoteWindowCompositorState::DeviceRecovery);
    assert(compositor.fallbackCount() == 2);
    compositor.commitLayout(layout(1280, 748)); // 重连完成后重新提交当前窗口几何。
    compositor.markHardwareFrame(43, QSize(1920, 1080));
    assert(compositor.state() == RemoteWindowCompositorState::Idle);
}

} // namespace

int main()
{
    runResizeScenarios();
    return 0;
}
