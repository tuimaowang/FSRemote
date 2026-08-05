#pragma once

#include "common.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

namespace lsp {

struct DecodedFrame {
    Size size;
    std::vector<uint8_t> bgra;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    void* shared_handle = nullptr;
    int shared_texture_index = -1; // wjy: 记录本帧占用的共享纹理槽，回调返回后按接管或丢弃结果释放对应keyed mutex。
    bool shared_texture_locked = false; // wjy: true表示解码器仍持有生产者key 0，尚未把完整帧交给控制端。
    bool shared_texture_handed_off = false; // wjy: true表示key 1已经在调用应用回调前交给消费者，拒绝帧时必须显式取回再归还key 0。
};

// =====wjy====
enum class DecodeStatus {
    Success, // wjy: 已完成码流解码并生成一张可交付画面。
    NeedMoreInput, // wjy: FFmpeg暂未输出画面，属于正常低延迟解码节奏，不得重置参考链。
    OutputTextureBusy, // wjy: 码流已被解码，但共享输出槽暂时都由Presenter持有，只丢弃本次显示输出。
    CorruptBitstream, // wjy: FFmpeg明确报告当前压缩数据损坏，上层可进入受控关键帧恢复。
    DeviceLost, // wjy: D3D11设备被移除、重置或挂起，应交给Viewer设备恢复路径处理。
    FatalError, // wjy: 初始化、资源创建或不满足解码契约的不可继续错误。
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::FatalError;

    bool producedFrame() const
    {
        return status == DecodeStatus::Success; // wjy: 兼容现有if判断，同时保留上层读取精确失败类别的能力。
    }

    explicit operator bool() const
    {
        return producedFrame();
    }
};
// ===end====

class H264Decoder {
public:
    ~H264Decoder();
    bool initialize_d3d11(ID3D11Device* device, ID3D11DeviceContext* context, std::string* error);
    DecodeResult decode(const uint8_t* h264, std::size_t h264Size, DecodedFrame* frame, std::string* error); // wjy: 直接接收压缩包并在解码器内部补齐FFmpeg padding，避免上层重复复制。
    DecodeResult decode(const std::vector<uint8_t>& h264, DecodedFrame* frame, std::string* error); // wjy: 返回结构化状态，显示背压不再伪装成码流错误。
    bool handoff_shared_texture(DecodedFrame* frame); // wjy: 在调用异步消费者回调前完成key 0到key 1的交接，消除RenderWorker抢先读取竞态。
    bool reclaim_shared_texture(DecodedFrame* frame); // wjy: 软件回退前重新取得已交接的纹理所有权，保证GPU/CPU读回仍在keyed mutex保护内。
    bool release_shared_texture(DecodedFrame* frame); // wjy: 丢帧、回退结束或异常退出时统一归还生产者key 0，保证输出池槽位可复用。
    void reset();

private:
    bool ensure(std::string* error);
    DecodeResult receive(DecodedFrame* frame, std::string* error);
    DecodeResult convert_d3d11_frame(DecodedFrame* decoded, std::string* error);
    bool ensure_video_processor(int width, int height, std::string* error);
    int acquire_output_texture(DecodeStatus* status, std::string* error); // wjy: 无可写槽返回OutputTextureBusy，真实同步或设备错误保留独立类别。
    DecodeStatus current_device_failure_status() const; // wjy: 资源调用失败后检查D3D11设备代际，区分DeviceLost与普通致命错误。
    void retire_output_textures(); // wjy: 分辨率切换时保留仍可能排队的旧共享纹理，使裸句柄在Qt消费前继续有效。
    void collect_retired_output_textures(); // wjy: 仅当旧纹理全部回到生产者key后释放整组资源，限制质量切换期间的GPU占用。
    void shutdown();

    static constexpr int kOutputTextureCount = 3; // wjy: 一个正在呈现、一个待呈现、一个供生产端受控丢帧后立即复用，避免双缓冲读写相撞。

    struct RetiredOutputTextures {
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, kOutputTextureCount> textures;
        std::array<Microsoft::WRL::ComPtr<IDXGIKeyedMutex>, kOutputTextureCount> keyed_mutexes;
        std::array<void*, kOutputTextureCount> shared_handles = {};
    };

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> processor_enum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, kOutputTextureCount> output_textures_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, kOutputTextureCount> output_srvs_;
    std::array<Microsoft::WRL::ComPtr<IDXGIKeyedMutex>, kOutputTextureCount> output_keyed_mutexes_;
    std::array<void*, kOutputTextureCount> output_shared_handles_ = {};
    std::vector<RetiredOutputTextures> retired_output_textures_;
    int output_index_ = -1;
    int processor_width_ = 0;
    int processor_height_ = 0;

    AVCodecContext* codec_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::vector<uint8_t> packet_input_buffer_; // wjy: 复用带padding的FFmpeg输入内存，避免每个压缩包重新分配。
    AVBufferRef* hw_device_ = nullptr;
    SwsContext* sws_ = nullptr;
    int sws_width_ = 0;
    int sws_height_ = 0;
    int sws_format_ = -1;
};

} // namespace lsp
