#include "lan_stream_api.h"

#include "dxgi_capture.h"
#include "ffmpeg_decoder.h"
#include "nvenc_h264_encoder.h"
#include "protocol.h"
#include "udp_socket.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <mmsystem.h>
#include <ws2tcpip.h>

namespace {

class TimerResolution {
public:
    TimerResolution() { timeBeginPeriod(1); }
    ~TimerResolution() { timeEndPeriod(1); }
};

void wait_until_precise(std::chrono::steady_clock::time_point target)
{
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= target) return;
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

template <typename SendFn>
void send_packets_paced(size_t count, std::chrono::steady_clock::time_point deadline, SendFn&& send_one)
{
    if (!count) return;
    const auto start = std::chrono::steady_clock::now();
    if (deadline <= start || count < 8) {
        for (size_t i = 0; i < count; ++i) send_one(i);
        return;
    }
    constexpr size_t batch_size = 4;
    const auto budget = deadline - start;
    for (size_t i = 0; i < count; ++i) {
        send_one(i);
        if ((i + 1) % batch_size == 0 && i + 1 < count) {
            wait_until_precise(start + budget * (i + 1) / count);
        }
    }
}

uint64_t now_us()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

struct CachedFramePackets {
    uint32_t frame_id = 0;
    std::vector<std::vector<uint8_t>> packets;
};

struct EncodedFrame {
    uint32_t frame_id = 0;
    std::shared_ptr<std::vector<uint8_t>> data;
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

class HostCore {
public:
    ~HostCore() { stop(); }

    int start(const char* ip, uint16_t port, uint32_t bitrate, uint32_t fps)
    {
        stop();
        if (!ip || !*ip) {
            set_error("viewer ip is empty");
            return 0;
        }
        viewer_ip_ = ip;
        port_ = port;
        bitrate_ = bitrate ? bitrate : 120000;
        fps_ = fps ? fps : 60;
        running_ = true;
        worker_ = std::thread([this] { run(); });
        return 1;
    }

    void stop()
    {
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }

    void get_stats(LspStats* out)
    {
        if (!out) return;
        std::lock_guard lock(mutex_);
        *out = stats_;
    }

    const char* last_error()
    {
        std::lock_guard lock(mutex_);
        return last_error_.c_str();
    }

private:
    void set_error(const std::string& error)
    {
        std::lock_guard lock(mutex_);
        last_error_ = error;
    }

    void run()
    {
        TimerResolution timer;
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        lsp::WsaRuntime wsa;
        if (!wsa.ok()) {
            set_error("WSAStartup failed");
            return;
        }

        std::string error;
        lsp::UdpSocket socket;
        if (!socket.open(0, &error)) {
            set_error(error);
            return;
        }
        socket.set_send_buffer(256 * 1024 * 1024, nullptr);
        socket.set_recv_buffer(4 * 1024 * 1024, nullptr);
        socket.set_nonblocking(true, nullptr);

        sockaddr_in viewer_addr = {};
        viewer_addr.sin_family = AF_INET;
        viewer_addr.sin_port = htons(port_);
        if (::inet_pton(AF_INET, viewer_ip_.c_str(), &viewer_addr.sin_addr) != 1) {
            set_error("invalid viewer ip: " + viewer_ip_);
            return;
        }

        lsp::DxgiCapture capture;
        if (!capture.initialize(&error)) {
            set_error("DXGI initialize failed: " + error);
            return;
        }

        lsp::NvencH264Encoder encoder;
        lsp::Size encoder_size{};
        lsp::CapturedFrame last_frame;
        std::deque<CachedFramePackets> packet_cache;
        constexpr size_t kPacketCacheFrames = 90;
        bool pending_keyframe = false;
        uint32_t frame_id = 1;
        const auto interval = std::chrono::microseconds(1000000 / std::max<uint32_t>(fps_, 1));
        auto next_frame = std::chrono::steady_clock::now();

        while (running_) {
            const auto loop_now = std::chrono::steady_clock::now();
            next_frame += interval;
            const auto loop_start = loop_now;
            if (next_frame < loop_start - interval) {
                next_frame = loop_start + interval;
            }

            lsp::CapturedFrame frame;
            error.clear();
            if (!capture.capture(&frame, &error)) {
                if (error == "timeout" && last_frame.texture) {
                    frame = last_frame;
                } else if (error == "timeout") {
                    wait_until_precise(next_frame);
                    continue;
                } else {
                    encoder.shutdown();
                    encoder_size = {};
                    last_frame = {};
                    capture.reset();
                    if (!capture.initialize(&error)) {
                        set_error("capture reinit failed: " + error);
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                    wait_until_precise(next_frame);
                    continue;
                }
            } else {
                last_frame = frame;
            }

            if (!encoder.ready() || encoder_size.width != frame.size.width || encoder_size.height != frame.size.height) {
                encoder.shutdown();
                encoder_size = frame.size;
                if (!encoder.initialize(capture.device(), frame.size, bitrate_, fps_, &error)) {
                    set_error("NVENC initialize failed: " + error);
                    return;
                }
                std::lock_guard lock(mutex_);
                stats_.width = frame.size.width;
                stats_.height = frame.size.height;
            }

            uint8_t fb_buf[256] = {};
            sockaddr_in fb_from = {};
            for (;;) {
                const int received = socket.recv(fb_buf, sizeof(fb_buf), &fb_from);
                if (received <= 0) break;
                lsp::FeedbackHeader fb = {};
                if (!lsp::parse_feedback(fb_buf, static_cast<size_t>(received), &fb)) continue;
                if (fb.type == lsp::kFeedbackRequestKeyFrame) {
                    pending_keyframe = true;
                } else if (fb.type == lsp::kFeedbackNackFragment) {
                    bool resent = false;
                    for (const auto& cached : packet_cache) {
                        if (cached.frame_id == fb.frame_id &&
                            fb.fragment_index < cached.packets.size() &&
                            !cached.packets[fb.fragment_index].empty()) {
                            if (socket.send_to(viewer_addr, cached.packets[fb.fragment_index])) {
                                std::lock_guard lock(mutex_);
                                ++stats_.rtx_sent;
                            }
                            resent = true;
                            break;
                        }
                    }
                    if (!resent) {
                        pending_keyframe = true;
                    }
                }
            }

            std::vector<uint8_t> encoded;
            bool keyframe = false;
            const bool force_keyframe = frame_id == 1 || (frame_id % (std::max<uint32_t>(fps_, 1) * 5)) == 0 ||
                                        pending_keyframe;
            if (!encoder.encode(frame.texture.Get(), frame_id, force_keyframe, &encoded, &keyframe, &error)) {
                std::lock_guard lock(mutex_);
                ++stats_.encode_errors;
                last_error_ = error;
                encoder.shutdown();
                encoder_size = {};
                wait_until_precise(next_frame);
                continue;
            }
            if (!encoded.empty()) {
                if (keyframe) {
                    pending_keyframe = false;
                }
                auto packets = lsp::fragment_frame(encoded, frame_id, now_us(), frame.size, keyframe);
                packet_cache.push_back(make_retransmit_cache(frame_id, packets));
                while (packet_cache.size() > kPacketCacheFrames) packet_cache.pop_front();
                const auto deadline = next_frame - std::chrono::microseconds(500);
                uint64_t sent_packets = 0;
                uint64_t sent_bytes = 0;
                uint64_t failed_sends = 0;
                send_packets_paced(packets.size(), deadline, [&](size_t i) {
                    if (!socket.send_to(viewer_addr, packets[i])) {
                        ++failed_sends;
                    } else {
                        ++sent_packets;
                        sent_bytes += packets[i].size();
                    }
                });
                std::lock_guard lock(mutex_);
                stats_.send_errors += failed_sends;
                stats_.packets += sent_packets;
                stats_.bytes += sent_bytes;
                stats_.frame_id = frame_id;
                ++stats_.frames;
                ++frame_id;
            }
            wait_until_precise(next_frame);
        }
    }

    std::atomic_bool running_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::string last_error_;
    LspStats stats_{};
    std::string viewer_ip_;
    uint16_t port_ = 49000;
    uint32_t bitrate_ = 120000;
    uint32_t fps_ = 60;
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
    uint16_t received_count = 0;
    bool initialized = false;
    bool keyframe = false;
    bool pli_sent = false;
    std::chrono::steady_clock::time_point first_seen{};
    std::chrono::steady_clock::time_point last_nack{};

    void reset(const lsp::PacketHeader& h)
    {
        initialized = true;
        keyframe = (h.flags & lsp::kFlagKeyFrame) != 0;
        frame_id = h.frame_id;
        fragment_count = h.fragment_count;
        frame_size = h.frame_size;
        size = {h.width, h.height};
        data.assign(frame_size, 0);
        received.assign(fragment_count, 0);
        payload_sizes.assign(fragment_count, 0);
        fec_groups.clear();
        received_count = 0;
        first_seen = std::chrono::steady_clock::now();
        last_nack = {};
    }

    uint16_t expected_payload_size(uint16_t index) const
    {
        const size_t offset = size_t(index) * lsp::kMaxPayload;
        if (offset >= frame_size) return 0;
        return static_cast<uint16_t>(std::min<size_t>(lsp::kMaxPayload, frame_size - offset));
    }

    bool add(const lsp::PacketView& p)
    {
        const auto& h = p.header;
        if (h.fragment_index >= fragment_count || h.fragment_count != fragment_count || h.frame_size != frame_size) return false;
        if (h.flags & lsp::kFlagFec) {
            fec_groups[h.fragment_index].assign(p.payload, p.payload + h.payload_size);
            return complete();
        }
        if (!received[h.fragment_index]) {
            const size_t offset = size_t(h.fragment_index) * lsp::kMaxPayload;
            if (offset + h.payload_size > data.size()) return false;
            std::memcpy(data.data() + offset, p.payload, h.payload_size);
            payload_sizes[h.fragment_index] = h.payload_size;
            received[h.fragment_index] = 1;
            ++received_count;
        }
        return complete();
    }

    uint32_t recover_fec()
    {
        uint32_t recovered = 0;
        for (const auto& [group_start, parity] : fec_groups) {
            int missing = -1;
            int missing_count = 0;
            const uint16_t end = std::min<uint16_t>(fragment_count, group_start + lsp::kFecGroupSize);
            for (uint16_t i = group_start; i < end; ++i) {
                if (!received[i]) {
                    missing = i;
                    ++missing_count;
                }
            }
            if (missing_count != 1) continue;
            const uint16_t restored_size = expected_payload_size(static_cast<uint16_t>(missing));
            if (!restored_size) continue;
            std::vector<uint8_t> restored = parity;
            restored.resize(std::max<size_t>(restored.size(), restored_size), 0);
            for (uint16_t i = group_start; i < end; ++i) {
                if (i == missing || !received[i]) continue;
                const size_t offset = size_t(i) * lsp::kMaxPayload;
                const uint16_t size = payload_sizes[i] ? payload_sizes[i] : expected_payload_size(i);
                for (uint16_t j = 0; j < size && j < restored.size(); ++j) restored[j] ^= data[offset + j];
            }
            const size_t offset = size_t(missing) * lsp::kMaxPayload;
            if (offset + restored_size > data.size()) continue;
            std::memcpy(data.data() + offset, restored.data(), restored_size);
            payload_sizes[missing] = restored_size;
            received[missing] = 1;
            ++received_count;
            ++recovered;
        }
        return recovered;
    }

    bool complete() const { return initialized && received_count == fragment_count; }

    std::vector<uint16_t> missing_indices(uint16_t limit) const
    {
        std::vector<uint16_t> missing;
        for (uint16_t i = 0; i < fragment_count && missing.size() < limit; ++i) {
            if (!received[i]) missing.push_back(i);
        }
        return missing;
    }
};

class ViewerCore {
public:
    ~ViewerCore() { stop(); }

    int start(uint16_t port, ID3D11Device* device, ID3D11DeviceContext* context)
    {
        stop();
        if (!device || !context) {
            set_error("invalid D3D11 device/context");
            return 0;
        }
        port_ = port;
        device_ = device;
        context_ = context;
        device_->AddRef();
        context_->AddRef();
        running_ = true;
        receiver_ = std::thread([this] { receive_loop(); });
        decoder_ = std::thread([this] { decode_loop(); });
        return 1;
    }

    void stop()
    {
        running_ = false;
        encoded_cv_.notify_all();
        if (receiver_.joinable()) receiver_.join();
        if (decoder_.joinable()) decoder_.join();
        latest_frame_.reset();
        if (context_) {
            context_->Release();
            context_ = nullptr;
        }
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
    }

    void get_stats(LspStats* out)
    {
        if (!out) return;
        std::lock_guard lock(mutex_);
        *out = stats_;
        if (latest_frame_) {
            out->width = latest_frame_->size.width;
            out->height = latest_frame_->size.height;
        }
    }

    int get_latest_frame(ID3D11ShaderResourceView** srv, uint32_t* width, uint32_t* height, uint32_t* frame_id)
    {
        if (!srv) return 0;
        std::lock_guard lock(mutex_);
        if (!latest_frame_ || !latest_frame_->srv) return 0;
        *srv = latest_frame_->srv.Get();
        (*srv)->AddRef();
        if (width) *width = latest_frame_->size.width;
        if (height) *height = latest_frame_->size.height;
        if (frame_id) *frame_id = stats_.frame_id;
        return 1;
    }

    const char* last_error()
    {
        std::lock_guard lock(mutex_);
        return last_error_.c_str();
    }

private:
    void set_error(const std::string& error)
    {
        std::lock_guard lock(mutex_);
        last_error_ = error;
    }

    void submit_encoded(uint32_t frame_id, std::shared_ptr<std::vector<uint8_t>> encoded)
    {
        {
            std::lock_guard lock(encoded_mutex_);
            constexpr size_t kMaxDecodeQueue = 8;
            if (encoded_queue_.size() >= kMaxDecodeQueue) {
                encoded_queue_.clear();
                decode_chain_broken_ = true;
                std::lock_guard stats_lock(mutex_);
                ++stats_.dropped;
            }
            encoded_queue_.push_back({frame_id, std::move(encoded)});
        }
        encoded_cv_.notify_one();
    }

    void receive_loop()
    {
        lsp::WsaRuntime wsa;
        if (!wsa.ok()) {
            set_error("WSAStartup failed");
            return;
        }
        std::string error;
        lsp::UdpSocket socket;
        if (!socket.open(port_, &error)) {
            set_error(error);
            return;
        }
        socket.set_recv_buffer(256 * 1024 * 1024, nullptr);
        socket.set_nonblocking(true, nullptr);

        std::unordered_map<uint32_t, FrameAssembly> assemblies;
        uint32_t highest = 0;
        uint32_t submitted = 0;
        bool waiting_for_keyframe = true;
        auto last_keyframe_request = std::chrono::steady_clock::time_point{};
        std::vector<uint8_t> buffer(65536);
        uint64_t local_packets = 0;
        uint64_t local_bytes = 0;
        uint32_t empty_polls = 0;
        auto flush_packet_stats = [&] {
            if (!local_packets) return;
            std::lock_guard lock(mutex_);
            stats_.packets += local_packets;
            stats_.bytes += local_bytes;
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
            std::lock_guard lock(mutex_);
            ++stats_.pli_sent;
        };

        while (running_) {
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
                std::lock_guard lock(mutex_);
                ++stats_.bad_packets;
                continue;
            }

            if (decode_chain_broken_.exchange(false)) {
                request_keyframe(from, packet.header.frame_id, std::chrono::steady_clock::now(), true);
            }

            const auto now = std::chrono::steady_clock::now();
            if (packet.header.frame_id > highest) {
                highest = packet.header.frame_id;
                for (auto& [id, pending] : assemblies) {
                    if (id < highest && pending.initialized && !pending.complete() &&
                        (pending.last_nack.time_since_epoch().count() == 0 || now - pending.last_nack >= std::chrono::milliseconds(15))) {
                        const auto missing = pending.missing_indices(128);
                        for (uint16_t index : missing) socket.send_to(from, lsp::make_nack_feedback(id, index));
                        pending.last_nack = now;
                        std::lock_guard lock(mutex_);
                        ++stats_.incomplete;
                        stats_.nack_sent += missing.size();
                    }
                }
            }

            if (packet.header.frame_id + 8 < highest || packet.header.frame_id <= submitted) continue;

            auto& assembly = assemblies[packet.header.frame_id];
            if (!assembly.initialized) assembly.reset(packet.header);
            assembly.add(packet);
            const uint32_t recovered = assembly.recover_fec();
            if (recovered) {
                std::lock_guard lock(mutex_);
                stats_.fec_recovered += recovered;
            }

            auto try_submit = [&](uint32_t id) -> bool {
                auto it = assemblies.find(id);
                if (it == assemblies.end() || !it->second.complete()) return false;
                submit_encoded(id, std::make_shared<std::vector<uint8_t>>(it->second.data));
                submitted = id;
                return true;
            };

            if (waiting_for_keyframe) {
                uint32_t best = 0;
                for (const auto& [id, ready] : assemblies) {
                    if (ready.complete() && ready.keyframe && id > best) best = id;
                }
                if (best && try_submit(best)) {
                    waiting_for_keyframe = false;
                } else {
                    request_keyframe(from, highest, now, false);
                }
            } else {
                while (try_submit(submitted + 1)) {}
                const uint32_t next_id = submitted + 1;
                auto next = assemblies.find(next_id);
                const bool next_stalled = next != assemblies.end() && next->second.initialized && !next->second.complete() &&
                                          now - next->second.first_seen >= std::chrono::milliseconds(120);
                if (next_stalled || highest > submitted + 8) {
                    request_keyframe(from, submitted + 1, now, false);
                }
            }

            for (auto it = assemblies.begin(); it != assemblies.end();) {
                if (it->first + 8 < highest || it->first <= submitted) it = assemblies.erase(it);
                else ++it;
            }
        }
        flush_packet_stats();
    }

    void decode_loop()
    {
        lsp::H264Decoder decoder;
        std::string error;
        if (!decoder.initialize_d3d11(device_, context_, &error)) {
            set_error(error);
            return;
        }

        while (running_) {
            std::shared_ptr<std::vector<uint8_t>> encoded;
            uint32_t frame_id = 0;
            {
                std::unique_lock lock(encoded_mutex_);
                encoded_cv_.wait_for(lock, std::chrono::milliseconds(20), [&] {
                    return !running_ || !encoded_queue_.empty();
                });
                if (!running_) break;
                if (encoded_queue_.empty()) continue;
                EncodedFrame next = std::move(encoded_queue_.front());
                encoded_queue_.pop_front();
                encoded = std::move(next.data);
                frame_id = next.frame_id;
            }

            lsp::DecodedFrame decoded;
            const lsp::DecodeResult decodeResult = decoder.decode(*encoded, &decoded, &error);
            if (decodeResult.producedFrame()) {
                auto frame = std::make_shared<lsp::DecodedFrame>(std::move(decoded));
                std::lock_guard lock(mutex_);
                latest_frame_ = std::move(frame);
                stats_.frame_id = frame_id;
                ++stats_.frames;
                last_error_.clear();
            } else if (decodeResult.status != lsp::DecodeStatus::NeedMoreInput
                && decodeResult.status != lsp::DecodeStatus::OutputTextureBusy
                && !error.empty()) {
                {
                    std::lock_guard lock(encoded_mutex_);
                    encoded_queue_.clear();
                }
                decode_chain_broken_ = true;
                set_error(error);
            }
        }
    }

    std::atomic_bool running_{false};
    std::thread receiver_;
    std::thread decoder_;
    std::mutex mutex_;
    std::string last_error_;
    LspStats stats_{};
    std::shared_ptr<lsp::DecodedFrame> latest_frame_;
    std::mutex encoded_mutex_;
    std::condition_variable encoded_cv_;
    std::deque<EncodedFrame> encoded_queue_;
    std::atomic_bool decode_chain_broken_{false};
    uint16_t port_ = 49000;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
};

template <typename T>
T* cast(void* handle)
{
    return reinterpret_cast<T*>(handle);
}

} // namespace

LSP_API LspHostHandle lsp_host_create() { return new HostCore(); }
LSP_API void lsp_host_destroy(LspHostHandle h) { delete cast<HostCore>(h); }
LSP_API int lsp_host_start(LspHostHandle h, const char* ip, uint16_t port, uint32_t bitrate, uint32_t fps) { return h ? cast<HostCore>(h)->start(ip, port, bitrate, fps) : 0; }
LSP_API void lsp_host_stop(LspHostHandle h) { if (h) cast<HostCore>(h)->stop(); }
LSP_API void lsp_host_get_stats(LspHostHandle h, LspStats* stats) { if (h) cast<HostCore>(h)->get_stats(stats); }
LSP_API const char* lsp_host_last_error(LspHostHandle h) { return h ? cast<HostCore>(h)->last_error() : "invalid handle"; }

LSP_API LspViewerHandle lsp_viewer_create() { return new ViewerCore(); }
LSP_API void lsp_viewer_destroy(LspViewerHandle h) { delete cast<ViewerCore>(h); }
LSP_API int lsp_viewer_start(LspViewerHandle h, uint16_t port, ID3D11Device* device, ID3D11DeviceContext* context) { return h ? cast<ViewerCore>(h)->start(port, device, context) : 0; }
LSP_API void lsp_viewer_stop(LspViewerHandle h) { if (h) cast<ViewerCore>(h)->stop(); }
LSP_API void lsp_viewer_get_stats(LspViewerHandle h, LspStats* stats) { if (h) cast<ViewerCore>(h)->get_stats(stats); }
LSP_API int lsp_viewer_get_latest_frame(LspViewerHandle h, ID3D11ShaderResourceView** srv, uint32_t* width, uint32_t* height, uint32_t* frame_id)
{
    return h ? cast<ViewerCore>(h)->get_latest_frame(srv, width, height, frame_id) : 0;
}
LSP_API const char* lsp_viewer_last_error(LspViewerHandle h) { return h ? cast<ViewerCore>(h)->last_error() : "invalid handle"; }
