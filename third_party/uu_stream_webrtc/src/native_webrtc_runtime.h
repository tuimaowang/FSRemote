#pragma once

#include <api/scoped_refptr.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace webrtc {
class PeerConnectionFactoryInterface;
}

namespace uu {

class NativeWebrtcRuntime {
public:
    NativeWebrtcRuntime();
    ~NativeWebrtcRuntime();

    NativeWebrtcRuntime(const NativeWebrtcRuntime&) = delete;
    NativeWebrtcRuntime& operator=(const NativeWebrtcRuntime&) = delete;

    bool initialize(std::string* error);
    void shutdown();
    void set_decoded_bgra_callback(std::function<void(int width, int height, const uint8_t* bgra, size_t size, double encoded_mbps)> callback);
    void set_decoded_texture_callback(std::function<int(int width, int height, void* shared_handle, uint64_t frame_id, double encoded_mbps)> callback); // wjy: 保留C ABI整数结果，避免把受控丢帧误判为需要BGRA回读。

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uu
