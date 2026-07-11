#include "host_media_pipeline.h"
#include "native_webrtc_runtime.h"
#include "signaling.h"
#include "webrtc_session.h"

#include <csignal>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <windows.h>
#include <mmsystem.h>

namespace {

std::atomic_bool g_running = true;

void on_signal(int)
{
    g_running = false;
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

bool handle_message(uu::WebrtcSession& session, const std::string& msg)
{
    const auto split = msg.find('\n');
    if (split == std::string::npos) return true;
    const std::string kind = msg.substr(0, split);
    const std::string body = msg.substr(split + 1);
    std::string error;
    if (kind == "answer" || kind == "offer" || kind == "pranswer") {
        if (!session.accept_remote_description(kind, body, &error)) {
            std::cerr << "remote description failed: " << error << "\n";
        }
    } else if (kind == "candidate") {
        const auto split2 = body.find('\n');
        const auto split3 = body.find('\n', split2 == std::string::npos ? 0 : split2 + 1);
        if (split2 != std::string::npos && split3 != std::string::npos) {
            const std::string mid = body.substr(0, split2);
            const int mline = std::stoi(body.substr(split2 + 1, split3 - split2 - 1));
            const std::string candidate = body.substr(split3 + 1);
            if (!session.add_remote_candidate(mid, mline, candidate, &error)) {
                std::cerr << "remote candidate failed: " << error << "\n";
            }
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    std::cout << "uu_webrtc_host build " << __DATE__ << " " << __TIME__ << "\n";
    std::cout << "transport: native WebRTC PeerConnection; codec factory: custom H265 NVENC + WebRTC fallback\n";
    std::cout << "usage: uu_webrtc_host.exe <signal_port=49100> <bitrate_kbps=120000> <fps=60>\n";

    const uint16_t signal_port = static_cast<uint16_t>(argc > 1 ? parse_u32(argv[1], 49100) : 49100);
    const uint32_t bitrate_kbps = argc > 2 ? parse_u32(argv[2], 120000) : 120000;
    const uint32_t fps = argc > 3 ? parse_u32(argv[3], 60) : 60;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    TimerResolution timer;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    std::string error;
    std::cout << "waiting signaling tcp 0.0.0.0:" << signal_port << "\n";
    const uintptr_t socket = uu::accept_tcp(signal_port, &error);
    if (!socket) {
        std::cerr << error << "\n";
        return 1;
    }
    std::cout << "signaling accepted\n";

    uu::NativeWebrtcRuntime runtime;
    if (!runtime.initialize(&error)) {
        std::cerr << "WebRTC init failed: " << error << "\n";
        uu::close_socket(socket);
        return 2;
    }

    uu::SessionConfig config;
    config.role = uu::SessionRole::Host;
    config.target_bitrate_kbps = bitrate_kbps;
    config.fps = fps;

    // =====wjy====
    uu::HostMediaPipeline media_pipeline(fps); // wjy: standalone host 也遵守 manager-owned capture，不让 WebrtcSession 隐式创建桌面源。
    auto media_subscription = media_pipeline.subscribe(&error);
    if (!media_subscription) {
        std::cerr << "host media init failed: " << error << "\n";
        uu::close_socket(socket);
        return 3;
    }
    config.host_video_source = media_subscription->source();
    config.media_id = "standalone";
    // ===end====

    uu::WebrtcSession session(&runtime, config);
    session.set_signal_callback([&](const std::string& kind, const std::string& body) {
        std::cout << "send signaling kind=" << kind << " size=" << body.size() << "\n";
        uu::send_message(socket, kind + "\n" + body);
    });
    if (!session.initialize(&error)) {
        std::cerr << "session init failed: " << error << "\n";
        uu::close_socket(socket);
        return 4;
    }
    if (!session.start_offer(&error)) {
        std::cerr << "offer failed: " << error << "\n";
        uu::close_socket(socket);
        return 5;
    }

    std::thread recv_thread([&] {
        std::string msg;
        while (g_running && uu::recv_message(socket, &msg)) {
            handle_message(session, msg);
        }
        g_running = false;
    });

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    uu::close_socket(socket);
    if (recv_thread.joinable()) recv_thread.join();
    return 0;
}
