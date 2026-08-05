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
    void set_decoded_texture_callback(std::function<int(
        int width,
        int height,
        void* shared_handle,
        uint64_t frame_id,
        int64_t rtp_timestamp,
        int64_t render_time_ms,
        uint64_t decoded_at_us,
        double encoded_mbps)> callback); // wjy: 保留三态结果并把真实RTP、render和解码时间贯穿到应用层。

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uu
