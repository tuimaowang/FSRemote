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

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uu
