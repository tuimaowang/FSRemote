#include "system_audio_stream.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include "signaling.h"

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace uu {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint16_t kDefaultAudioPort = 49105;
constexpr int kTargetSampleRate = 48000;
constexpr int kTargetChannels = 2;
constexpr int kTargetBytesPerSample = 2;
constexpr int kTargetFrameBytes = kTargetChannels * kTargetBytesPerSample;
constexpr REFERENCE_TIME kSharedBufferDuration = 1000000;

bool ensure_wsa()
{
    static bool initialized = [] {
        WSADATA data = {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

std::string format_hresult(const char* what, HRESULT hr)
{
    return std::string(what) + " failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
}

class ComScope {
public:
    ComScope()
    {
        initialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    }

    ~ComScope()
    {
        if (initialized_) {
            CoUninitialize();
        }
    }

    bool ok() const
    {
        return initialized_;
    }

private:
    bool initialized_ = false;
};

AVSampleFormat sample_format_from_wave(const WAVEFORMATEX* format)
{
    if (!format) {
        return AV_SAMPLE_FMT_NONE;
    }

    WORD tag = format->wFormatTag;
    WORD bits = format->wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
            bits = extensible->Samples.wValidBitsPerSample ? extensible->Samples.wValidBitsPerSample : extensible->Format.wBitsPerSample;
        } else if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            tag = WAVE_FORMAT_PCM;
            bits = extensible->Samples.wValidBitsPerSample ? extensible->Samples.wValidBitsPerSample : extensible->Format.wBitsPerSample;
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32) {
        return AV_SAMPLE_FMT_FLT;
    }
    if (tag == WAVE_FORMAT_PCM && bits == 16) {
        return AV_SAMPLE_FMT_S16;
    }
    if (tag == WAVE_FORMAT_PCM && bits == 32) {
        return AV_SAMPLE_FMT_S32;
    }
    return AV_SAMPLE_FMT_NONE;
}

bool create_default_device(EDataFlow flow, ComPtr<IMMDevice>* device, std::string* error)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        if (error) *error = format_hresult("CoCreateInstance(MMDeviceEnumerator)", hr);
        return false;
    }

    hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, device->ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        if (error) *error = format_hresult("GetDefaultAudioEndpoint", hr);
        return false;
    }
    return true;
}

class LoopbackCapture {
public:
    ~LoopbackCapture()
    {
        stop();
    }

    bool start(std::string* error)
    {
        if (!com_.ok()) {
            if (error) *error = "CoInitializeEx failed";
            return false;
        }

        if (!create_default_device(eRender, &device_, error)) {
            return false;
        }

        HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            if (error) *error = format_hresult("Activate(IAudioClient)", hr);
            return false;
        }

        hr = client_->GetMixFormat(&mix_format_);
        if (FAILED(hr) || !mix_format_) {
            if (error) *error = format_hresult("GetMixFormat", hr);
            return false;
        }

        input_sample_format_ = sample_format_from_wave(mix_format_);
        if (input_sample_format_ == AV_SAMPLE_FMT_NONE) {
            if (error) *error = "Unsupported loopback mix format";
            return false;
        }

        AVChannelLayout in_layout;
        av_channel_layout_default(&in_layout, mix_format_->nChannels);
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, kTargetChannels);
        if (swr_alloc_set_opts2(
                &resampler_,
                &out_layout,
                AV_SAMPLE_FMT_S16,
                kTargetSampleRate,
                &in_layout,
                input_sample_format_,
                mix_format_->nSamplesPerSec,
                0,
                nullptr) < 0) {
            av_channel_layout_uninit(&in_layout);
            av_channel_layout_uninit(&out_layout);
            if (error) *error = "swr_alloc_set_opts2 failed";
            return false;
        }
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        if (swr_init(resampler_) < 0) {
            if (error) *error = "swr_init failed";
            return false;
        }

        hr = client_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            kSharedBufferDuration,
            0,
            mix_format_,
            nullptr);
        if (FAILED(hr)) {
            if (error) *error = format_hresult("IAudioClient::Initialize(loopback)", hr);
            return false;
        }

        hr = client_->GetService(IID_PPV_ARGS(&capture_));
        if (FAILED(hr)) {
            if (error) *error = format_hresult("GetService(IAudioCaptureClient)", hr);
            return false;
        }

        hr = client_->Start();
        if (FAILED(hr)) {
            if (error) *error = format_hresult("IAudioClient::Start(loopback)", hr);
            return false;
        }
        started_ = true;
        return true;
    }

    void stop()
    {
        if (started_ && client_) {
            client_->Stop();
        }
        started_ = false;
        capture_.Reset();
        client_.Reset();
        device_.Reset();
        if (mix_format_) {
            CoTaskMemFree(mix_format_);
            mix_format_ = nullptr;
        }
        if (resampler_) {
            swr_free(&resampler_);
        }
    }

    bool read_frame(
        std::string* payload,
        std::atomic_bool& running,
        const std::atomic_uintptr_t* active_client = nullptr,
        uintptr_t expected_client = 0)
    {
        if (!started_ || !payload) {
            return false;
        }

        for (;;) {
            if (!running) {
                return false;
            }
            if (active_client && active_client->load() != expected_client) {
                return false;
            }
            UINT32 packet_frames = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) {
                return false;
            }
            if (packet_frames == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                return false;
            }

            const size_t input_bytes = static_cast<size_t>(frames) * mix_format_->nBlockAlign;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                silent_input_.assign(input_bytes, 0);
                data = silent_input_.data();
            }

            const int out_capacity = static_cast<int>(av_rescale_rnd(
                swr_get_delay(resampler_, mix_format_->nSamplesPerSec) + frames,
                kTargetSampleRate,
                mix_format_->nSamplesPerSec,
                AV_ROUND_UP));
            converted_.resize(static_cast<size_t>(out_capacity) * kTargetFrameBytes);
            uint8_t* out_data[] = {converted_.data()};
            const uint8_t* in_data[] = {data};
            const int out_frames = swr_convert(
                resampler_,
                out_data,
                out_capacity,
                in_data,
                static_cast<int>(frames));

            capture_->ReleaseBuffer(frames);

            if (out_frames <= 0) {
                continue;
            }

            payload->assign(
                reinterpret_cast<const char*>(converted_.data()),
                static_cast<size_t>(out_frames) * kTargetFrameBytes);
            return true;
        }
    }

private:
    ComScope com_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    WAVEFORMATEX* mix_format_ = nullptr;
    SwrContext* resampler_ = nullptr;
    AVSampleFormat input_sample_format_ = AV_SAMPLE_FMT_NONE;
    bool started_ = false;
    std::vector<uint8_t> silent_input_;
    std::vector<uint8_t> converted_;
};

class WasapiPlayer {
public:
    ~WasapiPlayer()
    {
        stop();
    }

    bool start(std::string* error)
    {
        if (!com_.ok()) {
            if (error) *error = "CoInitializeEx failed";
            return false;
        }

        if (!create_default_device(eRender, &device_, error)) {
            return false;
        }

        HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            if (error) *error = format_hresult("Activate(IAudioClient)", hr);
            return false;
        }

        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = kTargetChannels;
        format.nSamplesPerSec = kTargetSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = kTargetFrameBytes;
        format.nAvgBytesPerSec = kTargetSampleRate * kTargetFrameBytes;

        hr = client_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            kSharedBufferDuration,
            0,
            &format,
            nullptr);
        if (FAILED(hr)) {
            if (error) *error = format_hresult("IAudioClient::Initialize(render)", hr);
            return false;
        }

        hr = client_->GetBufferSize(&buffer_frames_);
        if (FAILED(hr)) {
            if (error) *error = format_hresult("GetBufferSize", hr);
            return false;
        }

        hr = client_->GetService(IID_PPV_ARGS(&render_));
        if (FAILED(hr)) {
            if (error) *error = format_hresult("GetService(IAudioRenderClient)", hr);
            return false;
        }

        hr = client_->Start();
        if (FAILED(hr)) {
            if (error) *error = format_hresult("IAudioClient::Start(render)", hr);
            return false;
        }
        started_ = true;
        return true;
    }

    void stop()
    {
        if (started_ && client_) {
            client_->Stop();
        }
        started_ = false;
        render_.Reset();
        client_.Reset();
        device_.Reset();
    }

    bool play(const std::string& payload, std::atomic_bool& running)
    {
        if (!started_ || payload.empty()) {
            return false;
        }

        const uint8_t* data = reinterpret_cast<const uint8_t*>(payload.data());
        size_t offset = 0;
        size_t remaining_frames = payload.size() / kTargetFrameBytes;
        while (remaining_frames > 0) {
            if (!running) {
                return false;
            }

            UINT32 padding = 0;
            if (FAILED(client_->GetCurrentPadding(&padding))) {
                return false;
            }
            const UINT32 available = buffer_frames_ > padding ? buffer_frames_ - padding : 0;
            if (available == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }

            const UINT32 write_frames = static_cast<UINT32>(std::min<size_t>(remaining_frames, available));
            BYTE* out = nullptr;
            if (FAILED(render_->GetBuffer(write_frames, &out))) {
                return false;
            }
            const size_t write_bytes = static_cast<size_t>(write_frames) * kTargetFrameBytes;
            std::memcpy(out, data + offset, write_bytes);
            if (FAILED(render_->ReleaseBuffer(write_frames, 0))) {
                return false;
            }

            offset += write_bytes;
            remaining_frames -= write_frames;
        }

        return true;
    }

private:
    ComScope com_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    UINT32 buffer_frames_ = 0;
    bool started_ = false;
};

uintptr_t listen_tcp(uint16_t port, std::atomic_bool& running, std::atomic_uintptr_t& server_socket)
{
    if (!ensure_wsa()) {
        return 0;
    }

    SOCKET server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        return 0;
    }

    BOOL reuse = TRUE;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(server, 1) == SOCKET_ERROR) {
        closesocket(server);
        return 0;
    }

    server_socket = static_cast<uintptr_t>(server);
    SOCKET client = ::accept(server, nullptr, nullptr);
    const uintptr_t published = server_socket.exchange(0);
    if (published) {
        uu::close_socket(published);
    }

    if (!running || client == INVALID_SOCKET) {
        if (client != INVALID_SOCKET) {
            uu::close_socket(static_cast<uintptr_t>(client));
        }
        return 0;
    }

    return static_cast<uintptr_t>(client);
}

} // namespace

class HostAudioStreamer::Impl {
public:
    ~Impl()
    {
        stop();
    }

    bool start(uint16_t port, std::string*)
    {
        if (running_) {
            return true;
        }
        running_ = true;
        worker_ = std::thread([this, port] { run(port ? port : kDefaultAudioPort); });
        return true;
    }

    void stop()
    {
        running_ = false;
        const uintptr_t server = server_socket_.exchange(0);
        if (server) {
            uu::close_socket(server);
        }
        reset_client();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void reset_client()
    {
        const uintptr_t client = client_socket_.exchange(0);
        if (client) {
            uu::close_socket(client);
        }
    }

private:
    void run(uint16_t port)
    {
        while (running_) {
            const uintptr_t client = listen_tcp(port, running_, server_socket_);
            if (!client || !running_) {
                continue;
            }
            client_socket_ = client;

            LoopbackCapture capture;
            std::string error;
            if (capture.start(&error)) {
                std::string payload;
                while (running_ && client_socket_.load() == client &&
                       capture.read_frame(&payload, running_, &client_socket_, client)) {
                    if (!uu::send_message(client, payload)) {
                        break;
                    }
                }
            }

            const uintptr_t current = client_socket_.exchange(0);
            if (current) {
                uu::close_socket(current);
            }
        }
    }

    std::atomic_bool running_ = false;
    std::atomic_uintptr_t server_socket_ = 0;
    std::atomic_uintptr_t client_socket_ = 0;
    std::thread worker_;
};

HostAudioStreamer::HostAudioStreamer()
    : impl_(std::make_unique<Impl>())
{
}

HostAudioStreamer::~HostAudioStreamer() = default;

bool HostAudioStreamer::start(uint16_t port, std::string* error)
{
    return impl_ && impl_->start(port, error);
}

void HostAudioStreamer::stop()
{
    if (impl_) {
        impl_->stop();
    }
}

void HostAudioStreamer::resetClient()
{
    if (impl_) {
        impl_->reset_client();
    }
}

class ViewerAudioPlayer::Impl {
public:
    ~Impl()
    {
        stop();
    }

    bool start(const std::string& host_ip, uint16_t port, std::string*)
    {
        if (running_) {
            return true;
        }
        running_ = true;
        worker_ = std::thread([this, host_ip, port] { run(host_ip, port ? port : kDefaultAudioPort); });
        return true;
    }

    void stop()
    {
        running_ = false;
        const uintptr_t socket = socket_.exchange(0);
        if (socket) {
            uu::close_socket(socket);
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void run(const std::string& host_ip, uint16_t port)
    {
        while (running_) {
            uintptr_t socket = 0;
            while (running_ && !socket) {
                std::string error;
                socket = uu::connect_tcp(host_ip, port, &error);
                if (socket || !running_) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (!socket || !running_) {
                if (socket) {
                    uu::close_socket(socket);
                }
                return;
            }
            socket_ = socket;

            WasapiPlayer player;
            std::string player_error;
            if (player.start(&player_error)) {
                std::string payload;
                while (running_ && uu::recv_message(socket, &payload)) {
                    if (!player.play(payload, running_)) {
                        break;
                    }
                }
            }

            const uintptr_t current = socket_.exchange(0);
            if (current) {
                uu::close_socket(current);
            }
            if (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }

    std::atomic_bool running_ = false;
    std::atomic_uintptr_t socket_ = 0;
    std::thread worker_;
};

ViewerAudioPlayer::ViewerAudioPlayer()
    : impl_(std::make_unique<Impl>())
{
}

ViewerAudioPlayer::~ViewerAudioPlayer() = default;

bool ViewerAudioPlayer::start(const std::string& host_ip, uint16_t port, std::string* error)
{
    return impl_ && impl_->start(host_ip, port, error);
}

void ViewerAudioPlayer::stop()
{
    if (impl_) {
        impl_->stop();
    }
}

} // namespace uu
