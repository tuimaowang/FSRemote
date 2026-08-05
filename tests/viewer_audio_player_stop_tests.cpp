#include "system_audio_stream.h"

#include <cassert>
#include <chrono>
#include <string>
#include <thread>

int main()
{
    // =====wjy====
    uu::ViewerAudioPlayer player;
    std::string error;
    assert(player.start("203.0.113.1", 49105, &error)); // wjy: TEST-NET-3地址不会依赖局域网真实设备，用于稳定覆盖“仍在连接”状态。
    std::this_thread::sleep_for(std::chrono::milliseconds(25)); // wjy: 给音频线程进入非阻塞connect/select循环，确保测试不是只停止尚未启动的线程。

    const auto stopStarted = std::chrono::steady_clock::now();
    player.stop(); // wjy: stop必须关闭连接中socket并唤醒select，不能等待Windows默认TCP连接超时。
    const auto stopElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stopStarted).count();
    assert(stopElapsedMs < 500); // wjy: 允许调度抖动和50ms重试间隔，但任何接近秒级的焦点切换阻塞都会让测试失败。
    // ===end====
    return 0;
}
