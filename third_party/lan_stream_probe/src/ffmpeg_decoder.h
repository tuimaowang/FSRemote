#pragma once

#include "common.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
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
    bool shared_texture_locked = false; // wjy: 只有生产端成功AcquireSync的帧才允许执行一次ReleaseSync，避免异常路径重复释放。
};

class H264Decoder {
public:
    ~H264Decoder();
    bool initialize_d3d11(ID3D11Device* device, ID3D11DeviceContext* context, std::string* error);
    bool decode(const std::vector<uint8_t>& h264, DecodedFrame* frame, std::string* error);
    void release_shared_texture(DecodedFrame* frame, bool consumerAccepted); // wjy: GPU接管时交给消费者key，回退或主动丢帧时直接归还生产者key。
    void reset();

private:
    bool ensure(std::string* error);
    bool receive(DecodedFrame* frame, std::string* error);
    bool convert_d3d11_frame(DecodedFrame* decoded, std::string* error);
    bool ensure_video_processor(int width, int height, std::string* error);
    int acquire_output_texture(std::string* error); // wjy: 从三槽池选择当前可写纹理，禁止覆盖Presenter尚未释放的资源。
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
    AVBufferRef* hw_device_ = nullptr;
    SwsContext* sws_ = nullptr;
    int sws_width_ = 0;
    int sws_height_ = 0;
    int sws_format_ = -1;
};

} // namespace lsp
