#include "native_webrtc_runtime.h"
#include "signaling.h"
#include "uu_codec_factory.h"
#include "webrtc_session.h"

#include <api/video/video_frame.h>
#include <api/video/video_frame_buffer.h>
#include <libyuv/convert_from.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <tchar.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using Microsoft::WRL::ComPtr;

struct SharedFrame {
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    uint64_t frames = 0;
};

std::atomic_bool g_running = true;
std::mutex g_mutex;
std::shared_ptr<const SharedFrame> g_frame;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<IDXGISwapChain> g_swap_chain;
ComPtr<ID3D11RenderTargetView> g_rtv;

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
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    g_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &g_rtv);
}

void cleanup_render_target()
{
    g_rtv.Reset();
}

bool create_device(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL feature_level = {};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                             D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device,
                                             &feature_level, &g_context))) {
        return false;
    }
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(g_context.As(&mt))) mt->SetMultithreadProtected(TRUE);
    create_render_target();
    return true;
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED && g_swap_chain) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
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

bool handle_message(uu::WebrtcSession& session, const std::string& msg)
{
    const auto split = msg.find('\n');
    if (split == std::string::npos) return true;
    const std::string kind = msg.substr(0, split);
    const std::string body = msg.substr(split + 1);
    std::string error;
    if (kind == "offer" || kind == "answer" || kind == "pranswer") {
        std::cout << "recv signaling kind=" << kind << " size=" << body.size() << "\n";
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

void store_bgra_frame(int width, int height, const uint8_t* bgra, size_t size)
{
    if (width <= 0 || height <= 0 || !bgra || size == 0) return;
    auto frame = std::make_shared<SharedFrame>();
    frame->width = width;
    frame->height = height;
    frame->bgra.assign(bgra, bgra + size);
    std::lock_guard lock(g_mutex);
    frame->frames = g_frame ? g_frame->frames + 1 : 1;
    g_frame = frame;
    if (frame->frames == 1 || frame->frames % 120 == 0) {
        std::cout << "viewer display queued frames=" << frame->frames
                  << " size=" << frame->width << "x" << frame->height << "\n";
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    std::cout << "uu_webrtc_viewer build " << __DATE__ << " " << __TIME__ << "\n";
    std::cout << "transport: native WebRTC PeerConnection; codec factory: custom H265 D3D11 decode + WebRTC fallback\n";
    std::cout << "usage: uu_webrtc_viewer.exe <host_ip> <signal_port=49100>\n";
    if (argc < 2) return 1;

    const std::string host_ip = argv[1];
    const uint16_t signal_port = static_cast<uint16_t>(argc > 2 ? parse_u32(argv[2], 49100) : 49100);

    WNDCLASSEX wc = {sizeof(WNDCLASSEX), CS_CLASSDC, wnd_proc, 0L, 0L, GetModuleHandle(nullptr),
                     nullptr, nullptr, nullptr, nullptr, _T("UuWebrtcViewer"), nullptr};
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, _T("UU WebRTC Viewer Probe"), WS_OVERLAPPEDWINDOW,
                             100, 100, 1600, 900, nullptr, nullptr, wc.hInstance, nullptr);
    if (!create_device(hwnd)) return 1;
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());
    ImGui::StyleColorsDark();

    std::string error;
    const uintptr_t socket = uu::connect_tcp(host_ip, signal_port, &error);
    if (!socket) {
        std::cerr << error << "\n";
        return 2;
    }
    std::cout << "signaling connected to " << host_ip << ":" << signal_port << "\n";

    uu::NativeWebrtcRuntime runtime;
    if (!runtime.initialize(&error)) {
        std::cerr << "WebRTC init failed: " << error << "\n";
        return 3;
    }

    uu::SessionConfig config;
    config.role = uu::SessionRole::Viewer;
    uu::WebrtcSession session(&runtime, config);
    uu::SetUuDecodedBgraHook([](int width, int height, const uint8_t* bgra, size_t size) {
        store_bgra_frame(width, height, bgra, size);
    });
    session.set_signal_callback([&](const std::string& kind, const std::string& body) {
        std::cout << "send signaling kind=" << kind << " size=" << body.size() << "\n";
        uu::send_message(socket, kind + "\n" + body);
    });
    session.set_frame_callback([](const webrtc::VideoFrame& frame) {
        static uint64_t sink_frames = 0;
        ++sink_frames;
        if (sink_frames == 1 || sink_frames % 120 == 0) {
            std::cout << "viewer WebRTC track sink frames=" << sink_frames
                      << " size=" << frame.width() << "x" << frame.height() << "\n";
        }
    });
    if (!session.initialize(&error)) {
        std::cerr << "session init failed: " << error << "\n";
        return 4;
    }

    std::thread recv_thread([&] {
        std::string msg;
        while (g_running && uu::recv_message(socket, &msg)) {
            handle_message(session, msg);
        }
        g_running = false;
    });

    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    int tex_w = 0;
    int tex_h = 0;
    uint64_t tex_frame = 0;
    uint64_t last_frame = 0;
    double fps = 0.0;
    auto stat_time = std::chrono::steady_clock::now();

    while (g_running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }

        std::shared_ptr<const SharedFrame> local;
        {
            std::lock_guard lock(g_mutex);
            local = g_frame;
        }

        if (local && !local->bgra.empty() && tex_frame != local->frames) {
            if (tex_w != local->width || tex_h != local->height || !texture) {
                texture.Reset();
                srv.Reset();
                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = static_cast<UINT>(local->width);
                desc.Height = static_cast<UINT>(local->height);
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA data = {};
                data.pSysMem = local->bgra.data();
                data.SysMemPitch = local->width * 4;
                if (SUCCEEDED(g_device->CreateTexture2D(&desc, &data, &texture))) {
                    g_device->CreateShaderResourceView(texture.Get(), nullptr, &srv);
                    tex_w = local->width;
                    tex_h = local->height;
                }
            } else {
                g_context->UpdateSubresource(texture.Get(), 0, nullptr, local->bgra.data(), local->width * 4, 0);
            }
            tex_frame = local->frames;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - stat_time >= std::chrono::seconds(1)) {
            const double sec = std::chrono::duration<double>(now - stat_time).count();
            const uint64_t frames = local ? local->frames : last_frame;
            fps = double(frames - last_frame) / sec;
            last_frame = frames;
            stat_time = now;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos);
        ImGui::SetNextWindowSize(ImGui::GetMainViewport()->WorkSize);
        ImGui::Begin("viewer", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("native webrtc viewer  host=%s:%u  frames=%llu  fps=%.1f  size=%dx%d",
                    host_ip.c_str(), signal_port, static_cast<unsigned long long>(local ? local->frames : 0),
                    fps, local ? local->width : 0, local ? local->height : 0);
        ImGui::Separator();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (srv && tex_w > 0 && tex_h > 0) {
            const float scale = std::min(avail.x / float(tex_w), avail.y / float(tex_h));
            const ImVec2 image_size(float(tex_w) * scale, float(tex_h) * scale);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - image_size.x) * 0.5f));
            ImGui::Image(reinterpret_cast<ImTextureID>(srv.Get()), image_size);
        } else {
            ImGui::Text("waiting for WebRTC decoded frames...");
        }
        ImGui::End();
        ImGui::Render();

        const float clear[4] = {0.02f, 0.02f, 0.025f, 1.0f};
        g_context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
        g_context->ClearRenderTargetView(g_rtv.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap_chain->Present(1, 0);
    }

    uu::close_socket(socket);
    uu::SetUuDecodedFrameHook({});
    uu::SetUuDecodedBgraHook({});
    if (recv_thread.joinable()) recv_thread.join();
    return 0;
}
