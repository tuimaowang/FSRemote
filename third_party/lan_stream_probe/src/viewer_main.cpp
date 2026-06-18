#include "ffmpeg_decoder.h"
#include "protocol.h"
#include "udp_socket.h"

#include <d3d11.h>
#include <d3d11_4.h>
#include <tchar.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using Microsoft::WRL::ComPtr;

struct SharedFrame {
    std::shared_ptr<lsp::DecodedFrame> frame;
    bool has_frame = false;
    uint32_t frame_id = 0;
    uint64_t decoded_frames = 0;
    uint64_t received_packets = 0;
    uint64_t received_bytes = 0;
    uint64_t incomplete_frames = 0;
    uint64_t dropped_complete_frames = 0;
    uint64_t bad_packets = 0;
    uint64_t feedback_sent = 0;
    uint64_t pli_sent = 0;
    uint64_t nack_sent = 0;
    uint64_t fec_recovered = 0;
    std::string last_error;
};

struct EncodedFrame {
    uint32_t frame_id = 0;
    std::shared_ptr<std::vector<uint8_t>> data;
};

struct FrameAssembly {
    uint32_t frame_id = 0;
    uint16_t fragment_count = 0;
    uint32_t frame_size = 0;
    lsp::Size size{};
    std::vector<uint8_t> data;
    std::vector<uint8_t> received;
    std::vector<uint16_t> payload_sizes;
    std::unordered_map<uint16_t, std::vector<uint8_t>> fec_groups;
    std::unordered_map<uint16_t, uint16_t> fec_sizes;
    uint16_t received_count = 0;
    bool nack_sent = false;
    bool pli_sent = false;
    bool initialized = false;
    bool keyframe = false;
    std::chrono::steady_clock::time_point first_seen{};
    std::chrono::steady_clock::time_point last_nack{};

    void reset(const lsp::PacketHeader& header)
    {
        initialized = true;
        nack_sent = false;
        pli_sent = false;
        keyframe = (header.flags & lsp::kFlagKeyFrame) != 0;
        first_seen = std::chrono::steady_clock::now();
        last_nack = {};
        frame_id = header.frame_id;
        fragment_count = header.fragment_count;
        frame_size = header.frame_size;
        size = {header.width, header.height};
        data.assign(frame_size, 0);
        received.assign(fragment_count, 0);
        payload_sizes.assign(fragment_count, 0);
        fec_groups.clear();
        fec_sizes.clear();
        received_count = 0;
    }

    bool add(const lsp::PacketView& packet)
    {
        const auto& h = packet.header;
        if (h.fragment_index >= fragment_count || h.fragment_count != fragment_count || h.frame_size != frame_size) {
            return false;
        }
        if (h.flags & lsp::kFlagFec) {
            fec_groups[h.fragment_index].assign(packet.payload, packet.payload + h.payload_size);
            fec_sizes[h.fragment_index] = h.payload_size;
            return complete();
        }
        if (!received[h.fragment_index]) {
            const size_t offset = size_t(h.fragment_index) * lsp::kMaxPayload;
            if (offset + h.payload_size > data.size()) {
                return false;
            }
            std::memcpy(data.data() + offset, packet.payload, h.payload_size);
            payload_sizes[h.fragment_index] = h.payload_size;
            received[h.fragment_index] = 1;
            ++received_count;
        }
        return received_count == fragment_count;
    }

    uint16_t expected_payload_size(uint16_t fragment_index) const
    {
        const size_t offset = size_t(fragment_index) * lsp::kMaxPayload;
        if (offset >= frame_size) {
            return 0;
        }
        return static_cast<uint16_t>(std::min<size_t>(lsp::kMaxPayload, frame_size - offset));
    }

    uint32_t recover_fec()
    {
        uint32_t recovered = 0;
        for (const auto& [group_start, parity] : fec_groups) {
            if (parity.empty()) {
                continue;
            }
            const uint16_t group_end = std::min<uint16_t>(fragment_count, group_start + lsp::kFecGroupSize);
            int missing = -1;
            int missing_count = 0;
            for (uint16_t i = group_start; i < group_end; ++i) {
                if (!received[i]) {
                    missing = i;
                    ++missing_count;
                }
            }
            if (missing_count != 1) {
                continue;
            }

            const uint16_t restored_size = expected_payload_size(static_cast<uint16_t>(missing));
            if (!restored_size) {
                continue;
            }
            std::vector<uint8_t> restored = parity;
            restored.resize(std::max<size_t>(restored.size(), restored_size), 0);
            for (uint16_t i = group_start; i < group_end; ++i) {
                if (i == missing || !received[i]) {
                    continue;
                }
                const size_t offset = size_t(i) * lsp::kMaxPayload;
                const uint16_t size = payload_sizes[i] ? payload_sizes[i] : expected_payload_size(i);
                for (uint16_t j = 0; j < size && j < restored.size(); ++j) {
                    restored[j] ^= data[offset + j];
                }
            }

            const size_t missing_offset = size_t(missing) * lsp::kMaxPayload;
            if (missing_offset + restored_size > data.size()) {
                continue;
            }
            std::memcpy(data.data() + missing_offset, restored.data(), restored_size);
            payload_sizes[missing] = restored_size;
            received[missing] = 1;
            ++received_count;
            ++recovered;
        }
        return recovered;
    }

    bool complete() const
    {
        return initialized && received_count == fragment_count;
    }

    std::vector<uint16_t> missing_indices(uint16_t limit) const
    {
        std::vector<uint16_t> missing;
        for (uint16_t i = 0; i < fragment_count && missing.size() < limit; ++i) {
            if (!received[i]) {
                missing.push_back(i);
            }
        }
        return missing;
    }

    std::vector<uint8_t> assemble() const
    {
        return data;
    }
};

std::mutex g_mutex;
SharedFrame g_shared;
std::atomic_bool g_running = true;
std::atomic_bool g_decode_chain_broken = false;
std::mutex g_encoded_mutex;
std::condition_variable g_encoded_cv;
std::deque<EncodedFrame> g_encoded_queue;

ComPtr<ID3D11Device> g_pd3dDevice;
ComPtr<ID3D11DeviceContext> g_pd3dDeviceContext;
ComPtr<IDXGISwapChain> g_pSwapChain;
ComPtr<ID3D11RenderTargetView> g_mainRenderTargetView;

uint32_t parse_u32(const char* text, uint32_t fallback)
{
    try {
        return static_cast<uint32_t>(std::stoul(text));
    } catch (...) {
        return fallback;
    }
}

void create_render_target()
{
    ComPtr<ID3D11Texture2D> back_buffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    g_pd3dDevice->CreateRenderTargetView(back_buffer.Get(), nullptr, &g_mainRenderTargetView);
}

void cleanup_render_target()
{
    g_mainRenderTargetView.Reset();
}

bool create_device_d3d(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT create_device_flags = 0;
    D3D_FEATURE_LEVEL feature_level;
    const D3D_FEATURE_LEVEL feature_level_array[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, create_device_flags, feature_level_array, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &feature_level, &g_pd3dDeviceContext);
    if (FAILED(hr)) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(g_pd3dDeviceContext.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }
    create_render_target();
    return true;
}

void cleanup_device_d3d()
{
    cleanup_render_target();
    g_pSwapChain.Reset();
    g_pd3dDeviceContext.Reset();
    g_pd3dDevice.Reset();
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return true;
    }
    switch (msg) {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED && g_pSwapChain) {
            cleanup_render_target();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
            create_render_target();
        }
        return 0;
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}

void receive_thread(uint16_t port)
{
    lsp::WsaRuntime wsa;
    if (!wsa.ok()) {
        std::lock_guard lock(g_mutex);
        g_shared.last_error = "WSAStartup failed";
        return;
    }

    std::string error;
    lsp::UdpSocket socket;
    if (!socket.open(port, &error)) {
        std::lock_guard lock(g_mutex);
        g_shared.last_error = error;
        return;
    }
    socket.set_recv_buffer(256 * 1024 * 1024, nullptr);
    socket.set_nonblocking(true, nullptr);
    {
        std::lock_guard lock(g_mutex);
        g_shared.last_error = "udp rcvbuf=" + std::to_string(socket.recv_buffer_size());
    }

    std::unordered_map<uint32_t, FrameAssembly> assemblies;
    uint32_t highest_frame_id = 0;
    uint32_t submitted_frame_id = 0;
    bool waiting_for_keyframe = true;
    auto last_keyframe_request = std::chrono::steady_clock::time_point{};
    std::vector<uint8_t> buffer(65536);
    uint64_t local_packets = 0;
    uint64_t local_bytes = 0;
    uint32_t empty_polls = 0;
    auto flush_packet_stats = [&] {
        if (!local_packets) {
            return;
        }
        std::lock_guard lock(g_mutex);
        g_shared.received_packets += local_packets;
        g_shared.received_bytes += local_bytes;
        local_packets = 0;
        local_bytes = 0;
    };
    auto request_keyframe = [&](const sockaddr_in& to, uint32_t frame_id,
                                std::chrono::steady_clock::time_point now, bool force) {
        if (!force && last_keyframe_request.time_since_epoch().count() != 0 &&
            now - last_keyframe_request < std::chrono::milliseconds(100)) {
            return;
        }
        socket.send_to(to, lsp::make_keyframe_feedback(frame_id));
        waiting_for_keyframe = true;
        last_keyframe_request = now;
        std::lock_guard lock(g_mutex);
        ++g_shared.feedback_sent;
        ++g_shared.pli_sent;
    };

    while (g_running) {
        sockaddr_in from = {};
        const int received = socket.recv(buffer.data(), static_cast<int>(buffer.size()), &from);
        if (received <= 0) {
            if (++empty_polls < 64) {
                SwitchToThread();
            } else {
                flush_packet_stats();
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                empty_polls = 0;
            }
            continue;
        }
        empty_polls = 0;

        lsp::PacketView packet;
        ++local_packets;
        local_bytes += static_cast<uint64_t>(received);
        if (local_packets >= 1024) {
            flush_packet_stats();
        }

        if (!lsp::parse_packet(buffer.data(), static_cast<size_t>(received), &packet) ||
            packet.header.fragment_count == 0 || packet.header.fragment_count > 4096) {
            request_keyframe(from, 0, std::chrono::steady_clock::now(), false);
            std::lock_guard lock(g_mutex);
            ++g_shared.bad_packets;
            continue;
        }

        if (g_decode_chain_broken.exchange(false)) {
            request_keyframe(from, packet.header.frame_id, std::chrono::steady_clock::now(), true);
        }

        const auto packet_time = std::chrono::steady_clock::now();
        if (packet.header.frame_id > highest_frame_id) {
            highest_frame_id = packet.header.frame_id;
            for (auto& [id, pending] : assemblies) {
                const bool should_nack = id < highest_frame_id && pending.initialized && !pending.complete() &&
                    (pending.last_nack.time_since_epoch().count() == 0 ||
                     packet_time - pending.last_nack >= std::chrono::milliseconds(15));
                if (should_nack) {
                    const auto missing = pending.missing_indices(128);
                    for (uint16_t fragment_index : missing) {
                        socket.send_to(from, lsp::make_nack_feedback(id, fragment_index));
                    }
                    pending.nack_sent = true;
                    pending.last_nack = packet_time;
                    std::lock_guard lock(g_mutex);
                    ++g_shared.incomplete_frames;
                    g_shared.nack_sent += missing.size();
                    g_shared.feedback_sent += missing.size();
                }
            }
        }

        if (packet.header.frame_id + 8 < highest_frame_id || packet.header.frame_id <= submitted_frame_id) {
            continue;
        }

        auto& assembly = assemblies[packet.header.frame_id];
        if (!assembly.initialized) {
            assembly.reset(packet.header);
        }

        assembly.add(packet);
        const uint32_t fec_recovered = assembly.recover_fec();
        if (fec_recovered) {
            std::lock_guard lock(g_mutex);
            g_shared.fec_recovered += fec_recovered;
        }

        auto submit_frame = [&](uint32_t id, FrameAssembly& ready) {
            auto encoded = std::make_shared<std::vector<uint8_t>>(ready.assemble());
            {
                std::unique_lock encoded_lock(g_encoded_mutex);
                constexpr size_t kMaxDecodeQueue = 8;
                if (g_encoded_queue.size() >= kMaxDecodeQueue) {
                    g_encoded_queue.clear();
                    g_decode_chain_broken = true;
                    std::lock_guard stats_lock(g_mutex);
                    ++g_shared.dropped_complete_frames;
                }
                g_encoded_queue.push_back({id, std::move(encoded)});
                submitted_frame_id = id;
            }
            g_encoded_cv.notify_one();
        };

        if (waiting_for_keyframe) {
            uint32_t best_keyframe = 0;
            for (const auto& [id, ready] : assemblies) {
                if (ready.complete() && ready.keyframe && id > best_keyframe) {
                    best_keyframe = id;
                }
            }
            if (best_keyframe == 0) {
                request_keyframe(from, highest_frame_id, packet_time, false);
            } else {
                submit_frame(best_keyframe, assemblies[best_keyframe]);
                waiting_for_keyframe = false;
            }
        } else {
            for (;;) {
                const uint32_t next_id = submitted_frame_id + 1;
                auto it = assemblies.find(next_id);
                if (it == assemblies.end() || !it->second.complete()) {
                    const bool next_stalled = it != assemblies.end() && it->second.initialized &&
                                              packet_time - it->second.first_seen >= std::chrono::milliseconds(120);
                    if (next_stalled || highest_frame_id > next_id + 8) {
                        request_keyframe(from, next_id, packet_time, false);
                    }
                    break;
                }
                submit_frame(next_id, it->second);
            }
        }

        for (auto it = assemblies.begin(); it != assemblies.end();) {
            if (it->first + 8 < highest_frame_id || it->first <= submitted_frame_id) {
                it = assemblies.erase(it);
            } else {
                ++it;
            }
        }
        if (true) {
            continue;
        }
    }
    flush_packet_stats();
}

void decode_thread()
{
    lsp::H264Decoder decoder;
    std::string init_error;
    if (!decoder.initialize_d3d11(g_pd3dDevice.Get(), g_pd3dDeviceContext.Get(), &init_error)) {
        std::lock_guard lock(g_mutex);
        g_shared.last_error = init_error;
        return;
    }

    while (g_running) {
        std::shared_ptr<std::vector<uint8_t>> encoded;
        uint32_t frame_id = 0;
        {
            std::unique_lock lock(g_encoded_mutex);
            g_encoded_cv.wait_for(lock, std::chrono::milliseconds(20), [&] {
                return !g_running || !g_encoded_queue.empty();
            });
            if (!g_running) {
                break;
            }
            if (g_encoded_queue.empty()) {
                continue;
            }
            EncodedFrame next = std::move(g_encoded_queue.front());
            g_encoded_queue.pop_front();
            encoded = std::move(next.data);
            frame_id = next.frame_id;
        }

        std::string error;
        lsp::DecodedFrame decoded;
        if (decoder.decode(*encoded, &decoded, &error)) {
            auto frame = std::make_shared<lsp::DecodedFrame>(std::move(decoded));
            std::lock_guard lock(g_mutex);
            g_shared.frame = std::move(frame);
            g_shared.has_frame = true;
            g_shared.frame_id = frame_id;
            ++g_shared.decoded_frames;
            g_shared.last_error.clear();
        } else if (!error.empty()) {
            {
                std::lock_guard encoded_lock(g_encoded_mutex);
                g_encoded_queue.clear();
            }
            g_decode_chain_broken = true;
            std::lock_guard lock(g_mutex);
            g_shared.last_error = error;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const uint16_t port = static_cast<uint16_t>(argc > 1 ? parse_u32(argv[1], 49000) : 49000);
    std::thread receiver(receive_thread, port);

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wnd_proc, 0L, 0L, GetModuleHandle(nullptr),
                      nullptr, nullptr, nullptr, nullptr, L"LanStreamProbeViewer", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"LAN Stream Probe Viewer", WS_OVERLAPPEDWINDOW,
                              100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    if (!create_device_d3d(hwnd)) {
        cleanup_device_d3d();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        g_running = false;
        g_encoded_cv.notify_all();
        if (receiver.joinable()) receiver.join();
        return 1;
    }
    std::thread decoder(decode_thread);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice.Get(), g_pd3dDeviceContext.Get());

    ComPtr<ID3D11ShaderResourceView> frame_srv;
    lsp::Size texture_size{};
    uint32_t texture_frame_id = 0;
    SharedFrame local;
    uint64_t last_decoded = 0;
    uint64_t last_packets = 0;
    uint64_t last_bytes = 0;
    double fps = 0.0;
    double mbps = 0.0;
    auto stats_time = std::chrono::steady_clock::now();

    while (g_running) {
        const auto frame_start = std::chrono::steady_clock::now();
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                g_running = false;
            }
        }
        if (!g_running) {
            break;
        }

        {
            std::lock_guard lock(g_mutex);
            local = g_shared;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - stats_time >= std::chrono::seconds(1)) {
            const double seconds = std::chrono::duration<double>(now - stats_time).count();
            fps = double(local.decoded_frames - last_decoded) / seconds;
            mbps = double(local.received_bytes - last_bytes) * 8.0 / seconds / 1000000.0;
            last_decoded = local.decoded_frames;
            last_packets = local.received_packets;
            last_bytes = local.received_bytes;
            stats_time = now;
        }

        if (local.has_frame && local.frame && texture_frame_id != local.frame_id) {
            if (local.frame->srv) {
                frame_srv = local.frame->srv;
                texture_size = local.frame->size;
                texture_frame_id = local.frame_id;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("viewer", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::Text("listen: 0.0.0.0:%u   status: %s   frame: %u   fps: %.1f   recv: %.1f Mbps   packets: %llu   incomplete: %llu   dropped: %llu   bad: %llu   fec: %llu   nack_sent: %llu   pli_sent: %llu",
                    port, local.has_frame ? "receiving" : "waiting", local.frame_id, fps, mbps,
                    static_cast<unsigned long long>(local.received_packets),
                    static_cast<unsigned long long>(local.incomplete_frames),
                    static_cast<unsigned long long>(local.dropped_complete_frames),
                    static_cast<unsigned long long>(local.bad_packets),
                    static_cast<unsigned long long>(local.fec_recovered),
                    static_cast<unsigned long long>(local.nack_sent),
                    static_cast<unsigned long long>(local.pli_sent));
        if (!local.last_error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", local.last_error.c_str());
        }
        ImGui::Separator();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (frame_srv && texture_size.width > 0 && texture_size.height > 0) {
            const float scale = std::min(avail.x / float(texture_size.width), avail.y / float(texture_size.height));
            const ImVec2 image_size(float(texture_size.width) * scale, float(texture_size.height) * scale);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - image_size.x) * 0.5f));
            ImGui::Image(reinterpret_cast<ImTextureID>(frame_srv.Get()), image_size);
        } else {
            ImGui::Text("waiting for complete keyframe...");
        }

        ImGui::End();
        ImGui::Render();

        const float clear_color[4] = {0.02f, 0.02f, 0.025f, 1.0f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, g_mainRenderTargetView.GetAddressOf(), nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView.Get(), clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(0, 0);

        const auto elapsed = std::chrono::steady_clock::now() - frame_start;
        const auto target = std::chrono::microseconds(8333);
        if (elapsed < target) {
            std::this_thread::sleep_for(target - elapsed);
        }
    }

    g_running = false;
    g_encoded_cv.notify_all();
    if (receiver.joinable()) {
        receiver.join();
    }
    if (decoder.joinable()) {
        decoder.join();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device_d3d();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
