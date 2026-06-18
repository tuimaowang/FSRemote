#pragma once

#include <api/scoped_refptr.h>

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

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uu
