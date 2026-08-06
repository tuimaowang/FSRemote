#pragma once

#include <QRect>
#include <QString>
#include <QSize>

#include <cstdint>

namespace ui {

// =====wjy====
// wjy: 统一合成器的状态只描述“最终可见像素由谁提交”，不改变 WebRTC 或编码协议。
enum class RemoteWindowCompositorState : std::uint8_t {
    Idle,
    InteractiveResize,
    FinalizeResize,
    HardwareFallback,
    DeviceRecovery,
};

// wjy: 所有输出、标题栏、输入和性能层都从同一份物理像素快照派生，避免父窗口与子 HWND 各算各的。
struct RemoteWindowLayoutSnapshot {
    QRect physicalOutputRect;
    QRect contentRect;
    QRect titleBarRect;
    QRect inputRect;
    QSize sourceSize;
    QSize outputSize;
    qreal devicePixelRatio = 1.0;
    std::uint64_t revision = 0;

    bool isValid() const;
};

QRect remoteCompositorPhysicalDirtyRect(
    const QRect& logicalRect,
    qreal devicePixelRatio,
    const QSize& physicalSurfaceSize); // wjy: 标题栏局部更新统一向外取整并裁剪到Overlay物理边界，避免DPI缩放留下未更新像素缝。

// 默认使用统一保留式合成路径；FSREMOTE_UNIFIED_COMPOSITOR=0保留一个发布周期的现场回退能力。
class RemoteWindowCompositorConfig final {
public:
    static bool rolloutEnabled();
    static QString activePathId();
};

// wjy: 该对象先作为无窗口状态机接入现有 RemoteDesktopWindow，后续再把 D3D/软件表面迁移到同一可见表面。
class RemoteWindowCompositor final {
public:
    explicit RemoteWindowCompositor(bool enabled = RemoteWindowCompositorConfig::rolloutEnabled());

    bool isEnabled() const;
    RemoteWindowCompositorState state() const;
    const RemoteWindowLayoutSnapshot& layout() const;
    std::uint64_t lastFrameId() const;
    std::uint64_t presentedFrameCount() const;
    std::uint64_t fallbackCount() const;
    std::uint64_t layoutCommitCount() const;
    std::uint64_t rejectedLayoutCount() const;

    void commitLayout(RemoteWindowLayoutSnapshot snapshot);
    void beginInteractiveResize(const RemoteWindowLayoutSnapshot& snapshot);
    void updateInteractiveGeometry(const RemoteWindowLayoutSnapshot& snapshot);
    void finalizeResize(const RemoteWindowLayoutSnapshot& snapshot);

    void markHardwareFrame(std::uint64_t frameId, const QSize& sourceSize);
    void markSoftwareFrame(std::uint64_t frameId, const QSize& sourceSize);
    void enterHardwareFallback();
    void enterDeviceRecovery();

private:
    bool commitLayoutInternal(RemoteWindowLayoutSnapshot snapshot);

    bool m_enabled = false;
    RemoteWindowCompositorState m_state = RemoteWindowCompositorState::Idle;
    RemoteWindowLayoutSnapshot m_layout;
    std::uint64_t m_lastFrameId = 0;
    std::uint64_t m_presentedFrameCount = 0;
    std::uint64_t m_fallbackCount = 0;
    std::uint64_t m_layoutCommitCount = 0;
    std::uint64_t m_rejectedLayoutCount = 0;
};

} // namespace ui
