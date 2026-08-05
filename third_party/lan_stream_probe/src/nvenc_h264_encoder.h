#pragma once

#include "common.h"

#include <d3d11.h>
#include <nvEncodeAPI.h>
#include <wrl/client.h>

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
    const std::string& diagnostics() const { return diagnostics_; } // wjy: 把本次会话查询到的输入格式和Temporal AQ能力交给目标端诊断日志。

private:
    struct RegisteredTextureEntry {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR registered = nullptr;
        NV_ENC_BUFFER_FORMAT format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
    };

    bool load_api(std::string* error);
    bool query_input_formats();
    bool query_capability(NV_ENC_CAPS capability, int* value) const;
    bool register_texture(ID3D11Texture2D* texture, std::string* error);
    bool check(NVENCSTATUS status, const char* call, std::string* error) const;

    HMODULE dll_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST fn_ = {};
    void* encoder_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
    NV_ENC_REGISTERED_PTR registered_ = nullptr;
    ID3D11Texture2D* registered_texture_ = nullptr;
    NV_ENC_BUFFER_FORMAT registered_format_ = NV_ENC_BUFFER_FORMAT_UNDEFINED; // wjy: 记录BGRA/NV12真实输入格式，避免纹理重注册时沿用错误像素解释。
    std::vector<RegisteredTextureEntry> registered_textures_; // wjy: 缓存四槽采集环和转换纹理的NVENC注册，运动画面逐帧换槽时不再反复注册/注销。
    Size size_;
    NV_ENC_CONFIG config_ = {}; // wjy: 保存成功初始化配置，后续ReconfigureEncoder基于同一编码会话安全修改速率参数。
    NV_ENC_INITIALIZE_PARAMS init_ = {};
    bool input_formats_known_ = false; // wjy: 能力查询成功时严格拒绝驱动未声明支持的BGRA/NV12格式，查询失败则保持旧版兼容行为。
    bool supports_nv12_ = false;
    bool supports_argb_ = false;
    bool temporal_aq_supported_ = false;
    std::string diagnostics_; // wjy: 保存API版本、输入格式和Temporal AQ能力，首帧失败时无需再次猜测目标显卡特性。
    uint32_t fps_ = 60;
    uint32_t api_version_ = NVENCAPI_VERSION;
};

} // namespace lsp
