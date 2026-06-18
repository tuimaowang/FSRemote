#pragma once

#include <stdint.h>

#include <memory>
#include <string>

namespace uu {

class HostAudioStreamer final {
public:
    HostAudioStreamer();
    ~HostAudioStreamer();

    HostAudioStreamer(const HostAudioStreamer&) = delete;
    HostAudioStreamer& operator=(const HostAudioStreamer&) = delete;

    bool start(uint16_t port, std::string* error);
    void stop();
    void resetClient();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class ViewerAudioPlayer final {
public:
    ViewerAudioPlayer();
    ~ViewerAudioPlayer();

    ViewerAudioPlayer(const ViewerAudioPlayer&) = delete;
    ViewerAudioPlayer& operator=(const ViewerAudioPlayer&) = delete;

    bool start(const std::string& host_ip, uint16_t port, std::string* error);
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uu
