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
#include "stream_capture_diagnostics.h"

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
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

uintptr_t create_audio_listener(uint16_t port, std::atomic_uintptr_t& server_socket)
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
        ::listen(server, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(server);
        return 0;
    }

    u_long nonBlocking = 1;
    if (::ioctlsocket(server, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
        closesocket(server);
        return 0;
    }
    server_socket = static_cast<uintptr_t>(server);
    return static_cast<uintptr_t>(server);
}

bool send_all_until(
    SOCKET socket,
    const uint8_t* data,
    size_t size,
    std::chrono::steady_clock::time_point deadline)
{
    while (size > 0) {
        const int sent = ::send(socket, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
        if (sent > 0) {
            data += sent;
            size -= static_cast<size_t>(sent);
            continue;
        }

        const int error = ::WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEINVAL) {
            return false;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return false;
        }

        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(socket, &writable);
        timeval timeout = {};
        timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
        timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
        const int selected = ::select(0, nullptr, &writable, nullptr, &timeout);
        if (selected <= 0 || !FD_ISSET(socket, &writable)) {
            return false;
        }
    }
    return true;
}

bool send_audio_message_bounded(uintptr_t socket_value, const std::string& message)
{
    constexpr auto kSendDeadline = std::chrono::milliseconds(100);
    const SOCKET socket = static_cast<SOCKET>(socket_value);
    const uint32_t size = htonl(static_cast<uint32_t>(message.size()));
    const auto deadline = std::chrono::steady_clock::now() + kSendDeadline;
    return send_all_until(
               socket,
               reinterpret_cast<const uint8_t*>(&size),
               sizeof(size),
               deadline)
        && send_all_until(
               socket,
               reinterpret_cast<const uint8_t*>(message.data()),
               message.size(),
               deadline);
}

} // namespace

class HostAudioStreamer::Impl {
public:
    struct Client final {
        uintptr_t socket = 0;
        std::mutex mutex;
        std::condition_variable condition;
        std::string pending_payload;
        bool has_payload = false;
        bool running = true;
        bool failed = false;
        std::thread worker;
    };

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
        capture_worker_ = std::thread([this] { capture_loop(); });
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
        if (capture_worker_.joinable()) {
            capture_worker_.join();
        }
    }

    void reset_client()
    {
        std::vector<std::shared_ptr<Client>> clients;
        {
            std::lock_guard lock(clients_mutex_);
            clients.reserve(clients_.size());
            for (auto& [socket, client] : clients_) {
                (void)socket;
                clients.push_back(std::move(client));
            }
            clients_.clear();
        }
        clients_condition_.notify_all();
        for (const auto& client : clients) {
            stop_client(client);
        }
        for (const auto& client : clients) {
            join_client(client);
        }
    }

    bool add_client(uintptr_t socket)
    {
        if (!socket || !running_) {
            if (socket) uu::close_socket(socket);
            return false;
        }
        u_long nonBlocking = 1;
        if (::ioctlsocket(static_cast<SOCKET>(socket), FIONBIO, &nonBlocking) == SOCKET_ERROR) {
            uu::close_socket(socket);
            return false;
        }

        auto client = std::make_shared<Client>();
        client->socket = socket;
        client->worker = std::thread([this, client] { client_loop(client); });
        {
            std::lock_guard lock(clients_mutex_);
            if (!running_) {
                stop_client(client);
                join_client(client);
                return false;
            }
            clients_[socket] = client;
        }
        clients_condition_.notify_all();
        lsp::append_stream_capture_diagnostic_log(
            "audio",
            "client accepted socket=" + std::to_string(socket));
        return true;
    }

    void remove_client(uintptr_t socket)
    {
        if (!socket) return;
        std::shared_ptr<Client> client;
        {
            std::lock_guard lock(clients_mutex_);
            const auto iterator = clients_.find(socket);
            if (iterator == clients_.end()) return;
            client = std::move(iterator->second);
            clients_.erase(iterator);
        }
        clients_condition_.notify_all();
        stop_client(client);
        join_client(client);
        lsp::append_stream_capture_diagnostic_log(
            "audio",
            "client removed socket=" + std::to_string(socket));
    }

private:
    static void stop_client(const std::shared_ptr<Client>& client)
    {
        if (!client) return;
        {
            std::lock_guard lock(client->mutex);
            client->running = false;
            client->has_payload = false;
            client->pending_payload.clear();
        }
        client->condition.notify_all();
        if (client->socket) {
            uu::close_socket(client->socket);
        }
    }

    static void join_client(const std::shared_ptr<Client>& client)
    {
        if (!client || !client->worker.joinable()
            || client->worker.get_id() == std::this_thread::get_id()) {
            return;
        }
        client->worker.join();
    }

    static bool client_is_failed(const std::shared_ptr<Client>& client)
    {
        std::lock_guard lock(client->mutex);
        return client->failed;
    }

    bool has_clients() const
    {
        std::lock_guard lock(clients_mutex_);
        return !clients_.empty();
    }

    void reap_failed_clients()
    {
        std::vector<std::shared_ptr<Client>> failed;
        {
            std::lock_guard lock(clients_mutex_);
            for (auto iterator = clients_.begin(); iterator != clients_.end();) {
                if (!client_is_failed(iterator->second)) {
                    ++iterator;
                    continue;
                }
                failed.push_back(std::move(iterator->second));
                iterator = clients_.erase(iterator);
            }
        }
        for (const auto& client : failed) {
            stop_client(client);
            join_client(client);
            lsp::append_stream_capture_diagnostic_log_rate_limited(
                "audio",
                "client send failed socket=" + std::to_string(client->socket),
                1000);
        }
    }

    void client_loop(const std::shared_ptr<Client>& client)
    {
        for (;;) {
            std::string payload;
            {
                std::unique_lock lock(client->mutex);
                client->condition.wait(lock, [&] {
                    return !client->running || client->has_payload || !running_;
                });
                if (!client->running || !running_) {
                    break;
                }
                payload.swap(client->pending_payload);
                client->has_payload = false;
            }

            if (!send_audio_message_bounded(client->socket, payload)) {
                std::lock_guard lock(client->mutex);
                client->failed = true;
                client->running = false;
                break;
            }
        }
    }

    void run(uint16_t port)
    {
        const uintptr_t listener = create_audio_listener(port, server_socket_);
        if (!listener) {
            lsp::append_stream_capture_diagnostic_log(
                "audio",
                "listener create failed port=" + std::to_string(port));
            return;
        }
        lsp::append_stream_capture_diagnostic_log(
            "audio",
            "listener ready port=" + std::to_string(port));
        while (running_) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(static_cast<SOCKET>(listener), &readable);
            timeval timeout = {};
            timeout.tv_usec = 100000;
            const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
            if (!running_) break;
            if (ready == SOCKET_ERROR) break;
            if (ready == 0 || !FD_ISSET(static_cast<SOCKET>(listener), &readable)) {
                reap_failed_clients();
                continue;
            }

            for (;;) {
                const SOCKET client = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
                if (client == INVALID_SOCKET) {
                    const int error = ::WSAGetLastError();
                    if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) {
                        lsp::append_stream_capture_diagnostic_log_rate_limited(
                            "audio",
                            "accept failed error=" + std::to_string(error),
                            1000);
                    }
                    break;
                }
                add_client(static_cast<uintptr_t>(client));
            }
            reap_failed_clients();
        }
        const uintptr_t currentListener = server_socket_.exchange(0);
        if (currentListener) uu::close_socket(currentListener);
    }

    void capture_loop()
    {
        while (running_) {
            {
                std::unique_lock lock(clients_mutex_);
                clients_condition_.wait(lock, [&] {
                    return !running_ || !clients_.empty();
                });
            }
            if (!running_) break;

            LoopbackCapture capture;
            std::string error;
            if (!capture.start(&error)) {
                lsp::append_stream_capture_diagnostic_log_rate_limited(
                    "audio",
                    "loopback capture start failed error=" + error,
                    1000);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            std::string payload;
            while (running_ && has_clients() && capture.read_frame(&payload, running_)) {
                broadcast(payload);
            }
        }
    }

    void broadcast(const std::string& payload)
    {
        std::vector<std::shared_ptr<Client>> clients;
        {
            std::lock_guard lock(clients_mutex_);
            clients.reserve(clients_.size());
            for (const auto& [socket, client] : clients_) {
                (void)socket;
                clients.push_back(client);
            }
        }
        for (const auto& client : clients) {
            {
                std::lock_guard lock(client->mutex);
                if (!client->running) continue;
                client->pending_payload = payload;
                client->has_payload = true;
            }
            client->condition.notify_one();
        }
        reap_failed_clients();
    }

    std::atomic_bool running_ = false;
    std::atomic_uintptr_t server_socket_ = 0;
    mutable std::mutex clients_mutex_;
    std::condition_variable clients_condition_;
    std::unordered_map<uintptr_t, std::shared_ptr<Client>> clients_;
    std::thread worker_;
    std::thread capture_worker_;
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

bool HostAudioStreamer::addClient(uintptr_t socket)
{
    return impl_ && impl_->add_client(socket);
}

void HostAudioStreamer::removeClient(uintptr_t socket)
{
    if (impl_) impl_->remove_client(socket);
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
    // =====wjy====
    uintptr_t connectInterruptibly(const std::string& host_ip, uint16_t port, std::string* error)
    {
        if (!ensure_wsa()) {
            if (error) *error = "WSAStartup failed";
            return 0;
        }
        SOCKET connectingSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connectingSocket == INVALID_SOCKET) {
            if (error) *error = "audio socket failed";
            return 0;
        }

        uintptr_t expectedSocket = 0;
        if (!socket_.compare_exchange_strong(
                expectedSocket, static_cast<uintptr_t>(connectingSocket), std::memory_order_acq_rel)) {
            ::closesocket(connectingSocket);
            if (error) *error = "audio socket already active";
            return 0; // wjy: 单播放器任意时刻只允许一个连接中或已连接socket，焦点抖动不会并行拨号。
        }

        auto closeIfStillOwned = [this, connectingSocket] {
            uintptr_t owned = static_cast<uintptr_t>(connectingSocket);
            if (socket_.compare_exchange_strong(owned, 0, std::memory_order_acq_rel)) {
                ::shutdown(connectingSocket, SD_BOTH);
                ::closesocket(connectingSocket); // wjy: 仅创建线程仍拥有句柄时关闭，stop已交换走的句柄绝不二次关闭。
            }
        };

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host_ip.c_str(), &addr.sin_addr) != 1) {
            if (error) *error = "invalid audio host";
            closeIfStillOwned();
            return 0;
        }

        u_long nonBlocking = 1;
        if (::ioctlsocket(connectingSocket, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
            if (error) *error = "audio nonblocking setup failed";
            closeIfStillOwned();
            return 0;
        }

        const int connectResult = ::connect(
            connectingSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (connectResult == SOCKET_ERROR) {
            const int connectError = ::WSAGetLastError();
            if (connectError != WSAEWOULDBLOCK
                && connectError != WSAEINPROGRESS
                && connectError != WSAEINVAL) {
                if (error) *error = "audio connect failed";
                closeIfStillOwned();
                return 0;
            }

            bool connected = false;
            while (running_.load(std::memory_order_acquire)
                && socket_.load(std::memory_order_acquire) == static_cast<uintptr_t>(connectingSocket)) {
                fd_set writeSet;
                fd_set errorSet;
                FD_ZERO(&writeSet);
                FD_ZERO(&errorSet);
                FD_SET(connectingSocket, &writeSet);
                FD_SET(connectingSocket, &errorSet);
                timeval timeout = {};
                timeout.tv_usec = 100000; // wjy: 每100ms重新观察停止标志，焦点离开不会等待系统默认TCP超时。
                const int selected = ::select(0, nullptr, &writeSet, &errorSet, &timeout);
                if (selected == SOCKET_ERROR) {
                    break; // wjy: stop从另一线程关闭连接中socket时select立即失败并退出。
                }
                if (selected == 0) {
                    continue;
                }
                int socketError = 0;
                int socketErrorSize = sizeof(socketError);
                if (::getsockopt(
                        connectingSocket,
                        SOL_SOCKET,
                        SO_ERROR,
                        reinterpret_cast<char*>(&socketError),
                        &socketErrorSize) == 0
                    && socketError == 0
                    && FD_ISSET(connectingSocket, &writeSet)) {
                    connected = true;
                }
                break;
            }
            if (!connected) {
                if (error && running_.load(std::memory_order_acquire)) *error = "audio connect failed";
                closeIfStillOwned();
                return 0;
            }
        }

        if (!running_.load(std::memory_order_acquire)
            || socket_.load(std::memory_order_acquire) != static_cast<uintptr_t>(connectingSocket)) {
            closeIfStillOwned();
            return 0; // wjy: 连接成功瞬间若焦点已离开，禁止把已被stop取消的socket重新交给播放循环。
        }
        u_long blocking = 0;
        if (::ioctlsocket(connectingSocket, FIONBIO, &blocking) == SOCKET_ERROR) {
            if (error) *error = "audio blocking restore failed";
            closeIfStillOwned();
            return 0;
        }
        return static_cast<uintptr_t>(connectingSocket);
    }
    // ===end====

    void run(const std::string& host_ip, uint16_t port)
    {
        while (running_) {
            uintptr_t socket = 0;
            while (running_ && !socket) {
                std::string error;
                socket = connectInterruptibly(host_ip, port, &error); // wjy: 连接中句柄立即登记到socket_，stop可从焦点线程取消而不会等待系统TCP超时。
                if (socket || !running_) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50)); // wjy: 连接失败重试间隔压到50ms，焦点离开时stop等待不会被旧200ms睡眠放大。
            }
            if (!socket || !running_) {
                if (socket) {
                    uu::close_socket(socket);
                }
                return;
            }
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
