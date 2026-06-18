#include "dxgi_capture.h"
#include "nvenc_h264_encoder.h"
#include "protocol.h"
#include "udp_socket.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <mmsystem.h>
#include <ws2tcpip.h>

namespace {

std::atomic_bool g_running = true;

void on_signal(int)
{
    g_running = false;
}

uint64_t now_us()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

uint32_t parse_u32(const char* text, uint32_t fallback)
{
    try {
        return static_cast<uint32_t>(std::stoul(text));
    } catch (...) {
        return fallback;
    }
}

class TimerResolution {
public:
    TimerResolution() { timeBeginPeriod(1); }
    ~TimerResolution() { timeEndPeriod(1); }
};

void wait_until_precise(std::chrono::steady_clock::time_point target);

template <typename SendFn>
void send_packets_paced(size_t packet_count, std::chrono::steady_clock::time_point deadline, SendFn&& send_one)
{
    if (packet_count == 0) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    if (deadline <= start || packet_count < 8) {
        for (size_t i = 0; i < packet_count; ++i) {
            send_one(i);
        }
        return;
    }

    constexpr size_t batch_size = 4;
    const auto budget = deadline - start;
    for (size_t i = 0; i < packet_count; ++i) {
        send_one(i);
        if ((i + 1) % batch_size == 0 && i + 1 < packet_count) {
            const auto target = start + budget * (i + 1) / packet_count;
            wait_until_precise(target);
        }
    }
}

void wait_until_precise(std::chrono::steady_clock::time_point target)
{
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= target) {
            return;
        }
        const auto remaining = target - now;
        if (remaining > std::chrono::milliseconds(2)) {
            std::this_thread::sleep_until(target - std::chrono::milliseconds(1));
        } else if (remaining > std::chrono::microseconds(300)) {
            SwitchToThread();
        } else {
            YieldProcessor();
        }
    }
}

struct CachedFramePackets {
    uint32_t frame_id = 0;
    std::vector<std::vector<uint8_t>> packets;
};

CachedFramePackets make_retransmit_cache(uint32_t frame_id, const std::vector<std::vector<uint8_t>>& packets)
{
    CachedFramePackets cache;
    cache.frame_id = frame_id;
    for (const auto& packet : packets) {
        lsp::PacketView view = {};
        if (!lsp::parse_packet(packet.data(), packet.size(), &view)) {
            continue;
        }
        if (view.header.flags & lsp::kFlagFec) {
            continue;
        }
        if (cache.packets.empty()) {
            cache.packets.resize(view.header.fragment_count);
        }
        if (view.header.fragment_index < cache.packets.size()) {
            cache.packets[view.header.fragment_index] = packet;
        }
    }
    return cache;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cout << "usage: lan_stream_host.exe <viewer_ip> [port=49000] [bitrate_kbps=120000] [fps=60]\n";
        return 1;
    }

    const std::string viewer_ip = argv[1];
    const uint16_t viewer_port = static_cast<uint16_t>(argc > 2 ? parse_u32(argv[2], 49000) : 49000);
    const uint32_t bitrate_kbps = argc > 3 ? parse_u32(argv[3], 120000) : 120000;
    const uint32_t fps = argc > 4 ? parse_u32(argv[4], 60) : 60;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    TimerResolution timer_resolution;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    lsp::WsaRuntime wsa;
    if (!wsa.ok()) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    std::string error;
    lsp::UdpSocket socket;
    if (!socket.open(0, &error)) {
        std::cerr << error << "\n";
        return 1;
    }
    socket.set_send_buffer(256 * 1024 * 1024, nullptr);
    socket.set_recv_buffer(4 * 1024 * 1024, nullptr);
    socket.set_nonblocking(true, nullptr);

    sockaddr_in viewer_addr = {};
    viewer_addr.sin_family = AF_INET;
    viewer_addr.sin_port = htons(viewer_port);
    if (::inet_pton(AF_INET, viewer_ip.c_str(), &viewer_addr.sin_addr) != 1) {
        std::cerr << "invalid viewer ip: " << viewer_ip << "\n";
        return 1;
    }

    lsp::DxgiCapture capture;
    if (!capture.initialize(&error)) {
        std::cerr << "DXGI initialize failed: " << error << "\n";
        return 1;
    }

    lsp::NvencH264Encoder encoder;
    lsp::Size encoder_size{};
    uint32_t frame_id = 1;
    uint64_t frames = 0;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t encode_errors = 0;
    uint64_t capture_timeouts = 0;
    uint64_t reused_frames = 0;
    uint64_t send_errors = 0;
    uint64_t keyframe_requests = 0;
    uint64_t rtx_requests = 0;
    uint64_t rtx_sent = 0;
    bool pending_keyframe_request = false;
    std::deque<CachedFramePackets> packet_cache;
    constexpr size_t kPacketCacheFrames = 90;
    auto stat_start = std::chrono::steady_clock::now();
    auto idle_stat_start = std::chrono::steady_clock::now();
    auto next_frame = std::chrono::steady_clock::now();
    const auto frame_interval = std::chrono::microseconds(1000000 / std::max<uint32_t>(fps, 1));
    lsp::CapturedFrame last_frame;

    std::cout << "target=" << viewer_ip << ":" << viewer_port
              << " bitrate=" << bitrate_kbps << "kbps fps=" << fps
              << " sndbuf=" << socket.send_buffer_size()
              << " rcvbuf=" << socket.recv_buffer_size() << "\n";

    while (g_running) {
        next_frame += frame_interval;
        const auto loop_start = std::chrono::steady_clock::now();
        if (next_frame < loop_start - frame_interval) {
            next_frame = loop_start + frame_interval;
        }

        lsp::CapturedFrame frame;
        error.clear();
        if (!capture.capture(&frame, &error)) {
            if (error == "timeout") {
                ++capture_timeouts;
                if (last_frame.texture) {
                    frame = last_frame;
                    ++reused_frames;
                } else {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - idle_stat_start >= std::chrono::seconds(1)) {
                        std::cout << "waiting for first DXGI frame"
                                  << " cap_timeout=" << capture_timeouts << "\n";
                        idle_stat_start = now;
                        capture_timeouts = 0;
                    }
                    wait_until_precise(next_frame);
                    continue;
                }
            } else {
                std::cerr << "capture failed: " << error << "\n";
                encoder.shutdown();
                encoder_size = {};
                last_frame = {};
                capture.reset();
                if (!capture.initialize(&error)) {
                    std::cerr << "capture reinit failed: " << error << "\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                wait_until_precise(next_frame);
                continue;
            }
        } else {
            last_frame = frame;
        }
        idle_stat_start = std::chrono::steady_clock::now();

        if (!encoder.ready() || encoder_size.width != frame.size.width || encoder_size.height != frame.size.height) {
            encoder.shutdown();
            encoder_size = frame.size;
            if (!encoder.initialize(capture.device(), frame.size, bitrate_kbps, fps, &error)) {
                std::cerr << "NVENC initialize failed: " << error << "\n";
                return 1;
            }
            std::cout << "capture=" << frame.size.width << "x" << frame.size.height << "\n";
        }

        std::vector<uint8_t> encoded;
        bool keyframe = false;
        uint8_t feedback_buffer[256] = {};
        sockaddr_in feedback_from = {};
        for (;;) {
            const int received = socket.recv(feedback_buffer, sizeof(feedback_buffer), &feedback_from);
            if (received <= 0) {
                break;
            }
            lsp::FeedbackHeader feedback = {};
            if (lsp::parse_feedback(feedback_buffer, static_cast<size_t>(received), &feedback) &&
                feedback.type == lsp::kFeedbackRequestKeyFrame) {
                ++keyframe_requests;
                pending_keyframe_request = true;
            } else if (lsp::parse_feedback(feedback_buffer, static_cast<size_t>(received), &feedback) &&
                       feedback.type == lsp::kFeedbackNackFragment) {
                ++rtx_requests;
                bool resent = false;
                for (const auto& cached : packet_cache) {
                    if (cached.frame_id == feedback.frame_id &&
                        feedback.fragment_index < cached.packets.size() &&
                        !cached.packets[feedback.fragment_index].empty()) {
                        if (socket.send_to(viewer_addr, cached.packets[feedback.fragment_index])) {
                            ++rtx_sent;
                        }
                        resent = true;
                        break;
                    }
                }
                if (!resent) {
                    pending_keyframe_request = true;
                }
            }
        }

        const bool force_keyframe = frame_id == 1 || (frame_id % (std::max<uint32_t>(fps, 1) * 5)) == 0 ||
                                    pending_keyframe_request;
        if (!encoder.encode(frame.texture.Get(), frame_id, force_keyframe, &encoded, &keyframe, &error)) {
            ++encode_errors;
            std::cerr << "encode failed: " << error << "\n";
            encoder.shutdown();
            encoder_size = {};
            wait_until_precise(next_frame);
            continue;
        }
        if (!encoded.empty()) {
            if (keyframe) {
                pending_keyframe_request = false;
            }
            auto fragments = lsp::fragment_frame(encoded, frame_id, now_us(), frame.size, keyframe);
            packet_cache.push_back(make_retransmit_cache(frame_id, fragments));
            while (packet_cache.size() > kPacketCacheFrames) {
                packet_cache.pop_front();
            }
            const auto send_deadline = next_frame - std::chrono::microseconds(500);
            send_packets_paced(fragments.size(), send_deadline, [&](size_t packet_index) {
                const auto& packet = fragments[packet_index];
                if (!socket.send_to(viewer_addr, packet)) {
                    ++send_errors;
                } else {
                    ++packets;
                    bytes += packet.size();
                }
            });
            ++frames;
            ++frame_id;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - stat_start >= std::chrono::seconds(1)) {
            const double seconds = std::chrono::duration<double>(now - stat_start).count();
            const double mbps = (double(bytes) * 8.0) / seconds / 1000000.0;
            std::cout << "fps=" << int(double(frames) / seconds)
                      << " mbps=" << int(mbps)
                      << " packets=" << packets
                      << " cap_timeout=" << capture_timeouts
                      << " reused=" << reused_frames
                      << " pli=" << keyframe_requests
                      << " nack=" << rtx_requests
                      << " rtx=" << rtx_sent
                      << " enc_err=" << encode_errors
                      << " send_err=" << send_errors << "\n";
            stat_start = now;
            frames = packets = bytes = capture_timeouts = reused_frames = keyframe_requests = rtx_requests = rtx_sent = encode_errors = send_errors = 0;
        }

        wait_until_precise(next_frame);
    }

    std::cout << "stopped\n";
    return 0;
}
