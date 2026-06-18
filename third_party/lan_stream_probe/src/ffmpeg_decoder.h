#pragma once

#include "common.h"

#include <d3d11.h>
#include <wrl/client.h>

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
};

class H264Decoder {
public:
    ~H264Decoder();
    bool initialize_d3d11(ID3D11Device* device, ID3D11DeviceContext* context, std::string* error);
    bool decode(const std::vector<uint8_t>& h264, DecodedFrame* frame, std::string* error);
    void reset();

private:
    bool ensure(std::string* error);
    bool receive(DecodedFrame* frame, std::string* error);
    bool convert_d3d11_frame(DecodedFrame* decoded, std::string* error);
    bool ensure_video_processor(int width, int height, std::string* error);
    void shutdown();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> processor_enum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_textures_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srvs_[2];
    int output_index_ = 0;
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
