#include "nvenc_h264_encoder.h"

#include <algorithm>
#include <cstring>
#include <windows.h>

namespace lsp {
namespace {

uint32_t nvenc_struct_version(uint32_t api_version, uint32_t struct_version, bool extended = false)
{
    uint32_t version = api_version | (struct_version << 16) | (0x7u << 28);
    if (extended) {
        version |= (1u << 31);
    }
    return version;
}

} // namespace

NvencH264Encoder::~NvencH264Encoder()
{
    shutdown();
    if (dll_) {
        FreeLibrary(dll_);
    }
}

bool NvencH264Encoder::initialize(ID3D11Device* device, Size size, uint32_t bitrate_kbps, uint32_t fps, std::string* error)
{
    shutdown();
    if (!device || !size.valid()) {
        if (error) *error = "invalid D3D11 device or frame size";
        return false;
    }
    if (!load_api(error)) {
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session = {};
    session.version = nvenc_struct_version(api_version_, 1);
    session.device = device;
    session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session.apiVersion = api_version_;
    NVENCSTATUS status = fn_.nvEncOpenEncodeSessionEx(&session, &encoder_);
    if (status == NV_ENC_ERR_INVALID_VERSION && fn_.nvEncOpenEncodeSession) {
        encoder_ = nullptr;
        status = fn_.nvEncOpenEncodeSession(device, NV_ENC_DEVICE_TYPE_DIRECTX, &encoder_);
    }
    if (!check(status, "NvEncOpenEncodeSession", error)) {
        return false;
    }

    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = nvenc_struct_version(api_version_, 5, true);
    preset.presetCfg.version = nvenc_struct_version(api_version_, 9, true);
    if (!check(fn_.nvEncGetEncodePresetConfigEx(encoder_, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P5_GUID,
                                                NV_ENC_TUNING_INFO_LOW_LATENCY, &preset),
               "NvEncGetEncodePresetConfigEx", error)) {
        shutdown();
        return false;
    }

    const uint32_t safe_fps = std::max(1u, fps);
    const uint32_t bitrate = std::max(10u, bitrate_kbps) * 1000u;
    NV_ENC_CONFIG config = preset.presetCfg;
    config.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    config.gopLength = safe_fps * 5;
    config.frameIntervalP = 1;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    config.rcParams.averageBitRate = bitrate;
    // =====wjy====
    config.rcParams.maxBitRate = bitrate + bitrate / 4; // wjy: 复杂运动帧允许最多25%受控峰值，减少雪地、树枝和文字边缘瞬时糊化；WebRTC发送上限仍是最终硬边界。
    config.rcParams.vbvBufferSize = std::max<uint32_t>(config.rcParams.maxBitRate * 2 / safe_fps, 1); // wjy: 使用约两帧峰值缓冲吸收画面复杂度突变，不引入B帧重排或长lookahead延迟。
    config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
    config.rcParams.enableAQ = 1;
    config.rcParams.enableTemporalAQ = 1; // wjy: 时间AQ把码率优先分配给连续帧中的运动与高频细节，提升高质量模式下视频和滚动画面观感。
    config.rcParams.aqStrength = 10; // wjy: 在默认中值基础上适度增强AQ，避免过强量化造成大面积平坦区域噪声或码率失控。
    // ===end====
    config.rcParams.zeroReorderDelay = 1;
    config.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    config.rcParams.enableLookahead = 0;
    config.rcParams.disableIadapt = 1;
    config.rcParams.disableBadapt = 1;
    config.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
    config.encodeCodecConfig.hevcConfig.idrPeriod = config.gopLength;
    config.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB = 1;
    config.encodeCodecConfig.hevcConfig.level = NV_ENC_LEVEL_AUTOSELECT;
    config.encodeCodecConfig.hevcConfig.tier = NV_ENC_TIER_HEVC_HIGH;

    NV_ENC_INITIALIZE_PARAMS init = {};
    init.version = nvenc_struct_version(api_version_, 7, true);
    init.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
    init.presetGUID = NV_ENC_PRESET_P5_GUID;
    init.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init.encodeWidth = size.width;
    init.encodeHeight = size.height;
    init.darWidth = size.width;
    init.darHeight = size.height;
    init.frameRateNum = safe_fps;
    init.frameRateDen = 1;
    init.enableEncodeAsync = 0;
    init.enablePTD = 1;
    init.encodeConfig = &config;
    if (!check(fn_.nvEncInitializeEncoder(encoder_, &init), "NvEncInitializeEncoder", error)) {
        shutdown();
        return false;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstream = {};
    bitstream.version = nvenc_struct_version(api_version_, 1);
    if (!check(fn_.nvEncCreateBitstreamBuffer(encoder_, &bitstream), "NvEncCreateBitstreamBuffer", error)) {
        shutdown();
        return false;
    }

    bitstream_ = bitstream.bitstreamBuffer;
    size_ = size;
    config_ = config;
    init_ = init;
    init_.encodeConfig = &config_; // wjy: 修正从栈上init复制后的指针，保存结构始终引用成员config_。
    fps_ = safe_fps;
    return true;
}

bool NvencH264Encoder::reconfigure(uint32_t bitrate_kbps, uint32_t fps, std::string* error)
{
    if (!encoder_ || !bitstream_ || !fn_.nvEncReconfigureEncoder) {
        if (error) *error = "NVENC reconfigure is unavailable";
        return false;
    }
    const uint32_t safeFps = std::max(1u, fps);
    const uint32_t bitrate = std::max(10u, bitrate_kbps) * 1000u;
    NV_ENC_CONFIG nextConfig = config_;
    nextConfig.gopLength = safeFps * 5;
    nextConfig.rcParams.averageBitRate = bitrate;
    // =====wjy====
    nextConfig.rcParams.maxBitRate = bitrate + bitrate / 4; // wjy: 在线切换画质后同步保留25%复杂帧峰值，参数变化不重建编码器也不强制IDR。
    nextConfig.rcParams.vbvBufferSize = std::max<uint32_t>(nextConfig.rcParams.maxBitRate * 2 / safeFps, 1); // wjy: FPS变化时重新按两帧峰值计算VBV，避免低FPS档缓冲时长意外扩大。
    // ===end====
    nextConfig.rcParams.vbvInitialDelay = nextConfig.rcParams.vbvBufferSize;
    nextConfig.encodeCodecConfig.hevcConfig.idrPeriod = nextConfig.gopLength;

    NV_ENC_INITIALIZE_PARAMS nextInit = init_;
    nextInit.frameRateNum = safeFps;
    nextInit.frameRateDen = 1;
    nextInit.encodeConfig = &nextConfig;
    NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
    reconfigure.version = nvenc_struct_version(api_version_, 1);
    reconfigure.reInitEncodeParams = nextInit;
    reconfigure.resetEncoder = 0; // wjy: 只更新速率控制参数，不重置位流队列或注册输入资源。
    reconfigure.forceIDR = 0; // wjy: 码率/FPS在线调节不改变画面尺寸，无需周期性强制IDR，避免流畅模式频繁调参放大解码纹理切换和闪黑。
    if (!check(fn_.nvEncReconfigureEncoder(encoder_, &reconfigure), "NvEncReconfigureEncoder", error)) {
        return false; // wjy: 驱动拒绝时保留旧编码器完整可用，调用方继续不断流编码。
    }

    config_ = nextConfig;
    init_ = nextInit;
    init_.encodeConfig = &config_;
    fps_ = safeFps;
    return true;
}

bool NvencH264Encoder::encode(ID3D11Texture2D* texture, uint32_t frame_id, bool force_keyframe,
                              std::vector<uint8_t>* output, bool* keyframe, std::string* error)
{
    if (!encoder_ || !bitstream_ || !texture) {
        if (error) *error = "NVENC is not initialized";
        return false;
    }
    if (!register_texture(texture, error)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);

    NV_ENC_MAP_INPUT_RESOURCE mapped = {};
    mapped.version = nvenc_struct_version(api_version_, 4);
    mapped.registeredResource = registered_;
    if (!check(fn_.nvEncMapInputResource(encoder_, &mapped), "NvEncMapInputResource", error)) {
        return false;
    }

    const bool force_idr = force_keyframe;
    NV_ENC_PIC_PARAMS pic = {};
    pic.version = nvenc_struct_version(api_version_, 7, true);
    pic.inputBuffer = mapped.mappedResource;
    pic.bufferFmt = mapped.mappedBufferFmt;
    pic.inputWidth = desc.Width;
    pic.inputHeight = desc.Height;
    pic.outputBitstream = bitstream_;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.pictureType = force_idr ? NV_ENC_PIC_TYPE_IDR : NV_ENC_PIC_TYPE_P;
    if (force_idr) {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }
    pic.codecPicParams.hevcPicParams.sliceMode = 0;
    pic.codecPicParams.hevcPicParams.sliceModeData = 0;

    const NVENCSTATUS enc_status = fn_.nvEncEncodePicture(encoder_, &pic);
    fn_.nvEncUnmapInputResource(encoder_, mapped.mappedResource);
    if (!check(enc_status, "NvEncEncodePicture", error)) {
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = nvenc_struct_version(api_version_, 2, true);
    lock.outputBitstream = bitstream_;
    lock.doNotWait = 0;
    if (!check(fn_.nvEncLockBitstream(encoder_, &lock), "NvEncLockBitstream", error)) {
        return false;
    }

    output->resize(lock.bitstreamSizeInBytes);
    if (lock.bitstreamSizeInBytes && lock.bitstreamBufferPtr) {
        std::memcpy(output->data(), lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
    }
    if (keyframe) {
        *keyframe = force_idr || lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
    }
    fn_.nvEncUnlockBitstream(encoder_, bitstream_);
    return !output->empty();
}

void NvencH264Encoder::shutdown()
{
    if (encoder_ && registered_) {
        fn_.nvEncUnregisterResource(encoder_, registered_);
    }
    registered_ = nullptr;
    registered_texture_ = nullptr;
    if (encoder_ && bitstream_) {
        fn_.nvEncDestroyBitstreamBuffer(encoder_, bitstream_);
    }
    bitstream_ = nullptr;
    if (encoder_) {
        fn_.nvEncDestroyEncoder(encoder_);
    }
    encoder_ = nullptr;
    size_ = {};
    config_ = {};
    init_ = {};
}

bool NvencH264Encoder::load_api(std::string* error)
{
    if (!dll_) {
        dll_ = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (!dll_) {
            if (error) *error = "failed to load nvEncodeAPI64.dll";
            return false;
        }
    }

    using GetMaxSupportedVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    auto get_max = reinterpret_cast<GetMaxSupportedVersion>(GetProcAddress(dll_, "NvEncodeAPIGetMaxSupportedVersion"));
    if (get_max) {
        uint32_t max_supported = 0;
        if (get_max(&max_supported) == NV_ENC_SUCCESS && max_supported) {
            api_version_ = std::min(max_supported, static_cast<uint32_t>(NVENCAPI_VERSION));
            if ((api_version_ & 0xFFu) >= 13) {
                api_version_ = 12;
            }
        }
    }

    using CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto create_instance = reinterpret_cast<CreateInstance>(GetProcAddress(dll_, "NvEncodeAPICreateInstance"));
    if (!create_instance) {
        if (error) *error = "NvEncodeAPICreateInstance not found";
        return false;
    }

    fn_ = {};
    fn_.version = nvenc_struct_version(api_version_, 2);
    return check(create_instance(&fn_), "NvEncodeAPICreateInstance", error);
}

bool NvencH264Encoder::register_texture(ID3D11Texture2D* texture, std::string* error)
{
    if (registered_ && registered_texture_ == texture) {
        return true;
    }
    if (registered_) {
        fn_.nvEncUnregisterResource(encoder_, registered_);
        registered_ = nullptr;
        registered_texture_ = nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    NV_ENC_REGISTER_RESOURCE registered = {};
    registered.version = nvenc_struct_version(api_version_, 5);
    registered.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    registered.width = desc.Width;
    registered.height = desc.Height;
    registered.resourceToRegister = texture;
    registered.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    registered.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (!check(fn_.nvEncRegisterResource(encoder_, &registered), "NvEncRegisterResource", error)) {
        return false;
    }
    registered_ = registered.registeredResource;
    registered_texture_ = texture;
    return true;
}

bool NvencH264Encoder::check(NVENCSTATUS status, const char* call, std::string* error) const
{
    if (status == NV_ENC_SUCCESS) {
        return true;
    }
    if (error) {
        *error = std::string(call) + " failed: " + std::to_string(static_cast<int>(status));
    }
    return false;
}

} // namespace lsp
