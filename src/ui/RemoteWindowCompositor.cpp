#include "ui/RemoteWindowCompositor.h"

#include <utility>

namespace ui {

bool RemoteWindowLayoutSnapshot::isValid() const
{
    return physicalOutputRect.isValid()
        && contentRect.isValid()
        && inputRect.isValid()
        && devicePixelRatio > 0.0;
}

bool RemoteWindowCompositorConfig::rolloutEnabled()
{
    return true;
}

QString RemoteWindowCompositorConfig::activePathId()
{
    return QStringLiteral("unified-retained-surface");
}

RemoteWindowCompositor::RemoteWindowCompositor(bool enabled)
    : m_enabled(enabled)
{
}

bool RemoteWindowCompositor::isEnabled() const
{
    return m_enabled;
}

RemoteWindowCompositorState RemoteWindowCompositor::state() const
{
    return m_state;
}

const RemoteWindowLayoutSnapshot& RemoteWindowCompositor::layout() const
{
    return m_layout;
}

std::uint64_t RemoteWindowCompositor::lastFrameId() const
{
    return m_lastFrameId;
}

std::uint64_t RemoteWindowCompositor::presentedFrameCount() const
{
    return m_presentedFrameCount;
}

std::uint64_t RemoteWindowCompositor::fallbackCount() const
{
    return m_fallbackCount;
}

std::uint64_t RemoteWindowCompositor::layoutCommitCount() const
{
    return m_layoutCommitCount;
}

std::uint64_t RemoteWindowCompositor::rejectedLayoutCount() const
{
    return m_rejectedLayoutCount;
}

void RemoteWindowCompositor::commitLayout(RemoteWindowLayoutSnapshot snapshot)
{
    if (!m_enabled) {
        return;
    }
    commitLayoutInternal(std::move(snapshot));
}

void RemoteWindowCompositor::beginInteractiveResize(const RemoteWindowLayoutSnapshot& snapshot)
{
    if (!m_enabled) {
        return;
    }
    m_state = RemoteWindowCompositorState::InteractiveResize;
    commitLayoutInternal(snapshot);
}

void RemoteWindowCompositor::updateInteractiveGeometry(const RemoteWindowLayoutSnapshot& snapshot)
{
    if (!m_enabled || m_state != RemoteWindowCompositorState::InteractiveResize) {
        return;
    }
    // wjy: 拖拽中只提交几何快照，保留上一张源帧和所有本地层，不触发破坏性资源重建。
    commitLayoutInternal(snapshot);
}

void RemoteWindowCompositor::finalizeResize(const RemoteWindowLayoutSnapshot& snapshot)
{
    if (!m_enabled) {
        return;
    }
    m_state = RemoteWindowCompositorState::FinalizeResize;
    commitLayoutInternal(snapshot);
    m_state = RemoteWindowCompositorState::Idle;
}

void RemoteWindowCompositor::markHardwareFrame(std::uint64_t frameId, const QSize& sourceSize)
{
    if (!m_enabled) {
        return;
    }
    m_lastFrameId = frameId;
    ++m_presentedFrameCount;
    if (m_state != RemoteWindowCompositorState::InteractiveResize
        && m_state != RemoteWindowCompositorState::FinalizeResize) {
        m_state = RemoteWindowCompositorState::Idle; // wjy: 普通帧才回到Idle，交互缩放中的新帧必须保留InteractiveResize诊断语义。
    }
    m_layout.sourceSize = sourceSize;
}

void RemoteWindowCompositor::markSoftwareFrame(std::uint64_t frameId, const QSize& sourceSize)
{
    if (!m_enabled) {
        return;
    }
    m_lastFrameId = frameId;
    ++m_presentedFrameCount;
    m_state = RemoteWindowCompositorState::HardwareFallback;
    m_layout.sourceSize = sourceSize;
}

void RemoteWindowCompositor::enterHardwareFallback()
{
    if (!m_enabled) {
        return;
    }
    m_state = RemoteWindowCompositorState::HardwareFallback;
    ++m_fallbackCount;
}

void RemoteWindowCompositor::enterDeviceRecovery()
{
    if (!m_enabled) {
        return;
    }
    m_state = RemoteWindowCompositorState::DeviceRecovery;
    ++m_fallbackCount;
}

bool RemoteWindowCompositor::commitLayoutInternal(RemoteWindowLayoutSnapshot snapshot)
{
    if (!snapshot.isValid()) {
        ++m_rejectedLayoutCount;
        return false;
    }
    if (snapshot.revision == 0) {
        snapshot.revision = m_layout.revision + 1;
    } else if (snapshot.revision <= m_layout.revision) {
        ++m_rejectedLayoutCount;
        return false;
    }
    m_layout = std::move(snapshot);
    ++m_layoutCommitCount;
    return true;
}

} // namespace ui
