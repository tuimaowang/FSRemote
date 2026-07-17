#pragma once

#include "common.h"

#include <d3d11.h>
#include <nvEncodeAPI.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lsp {

class NvencH264Encoder {
public:
    ~NvencH264Encoder();

    bool initialize(ID3D11Device* device, Size size, uint32_t bitrate_kbps, uint32_t fps, std::string* error);
    bool reconfigure(uint32_t bitrate_kbps, uint32_t fps, std::string* error); // wjy: 在线更新码率/FPS，不销毁编码器、位流缓冲或注册纹理。
    bool encode(ID3D11Texture2D* texture, uint32_t frame_id, bool force_keyframe,
                std::vector<uint8_t>* output, bool* keyframe, std::string* error);
    void shutdown();
    bool ready() const { return encoder_ && bitstream_; }

private:
    bool load_api(std::string* error);
    bool register_texture(ID3D11Texture2D* texture, std::string* error);
    bool check(NVENCSTATUS status, const char* call, std::string* error) const;

    HMODULE dll_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST fn_ = {};
    void* encoder_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
    NV_ENC_REGISTERED_PTR registered_ = nullptr;
    ID3D11Texture2D* registered_texture_ = nullptr;
    Size size_;
    NV_ENC_CONFIG config_ = {}; // wjy: 保存成功初始化配置，后续ReconfigureEncoder基于同一编码会话安全修改速率参数。
    NV_ENC_INITIALIZE_PARAMS init_ = {};
    uint32_t fps_ = 60;
    uint32_t api_version_ = NVENCAPI_VERSION;
};

} // namespace lsp
