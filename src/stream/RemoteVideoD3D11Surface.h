#pragma once

#include "stream/RemoteVideoRenderWorker.h"

#include <cstdint>
#include <memory>

namespace stream {

// =====wjy====
class RemoteVideoD3D11Surface;

// 一个适配器对象只由对应的 RenderWorker 线程调用，独占 D3D11 immediate context。
class RemoteVideoD3D11Adapter final : public std::enable_shared_from_this<RemoteVideoD3D11Adapter> {
public:
    explicit RemoteVideoD3D11Adapter(std::uint32_t adapterIndex = 0);
    ~RemoteVideoD3D11Adapter();

    RemoteVideoD3D11Adapter(const RemoteVideoD3D11Adapter&) = delete;
    RemoteVideoD3D11Adapter& operator=(const RemoteVideoD3D11Adapter&) = delete;

    std::shared_ptr<RemoteVideoRenderSurface> createSurface();
    std::uint32_t adapterIndex() const noexcept;
    long lastDeviceError() const noexcept;
    void resetDevice() noexcept;

private:
    friend class RemoteVideoD3D11Surface;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// ===end====

} // namespace stream
