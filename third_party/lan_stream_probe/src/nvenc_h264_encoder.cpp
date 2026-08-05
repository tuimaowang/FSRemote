#include "nvenc_h264_encoder.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>
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

NV_ENC_BUFFER_FORMAT nvenc_buffer_format(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return NV_ENC_BUFFER_FORMAT_ARGB; // wjy: D3D11 BGRA字节布局对应NVENC的ARGB命名，保持现有色彩顺序。
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return NV_ENC_BUFFER_FORMAT_ABGR;
    case DXGI_FORMAT_NV12:
        return NV_ENC_BUFFER_FORMAT_NV12; // wjy: GPU视频处理器可直接输出NV12，上传量和颜色转换成本显著低于四通道纹理。
    default:
        return NV_ENC_BUFFER_FORMAT_UNDEFINED;
    }
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

    // =====wjy====
    query_input_formats(); // wjy: 会话建立后读取HEVC真实输入格式，旧代显卡不再靠型号名单猜测BGRA/NV12兼容性。
    int temporal_aq_capability = 0;
    temporal_aq_supported_ = query_capability(NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ, &temporal_aq_capability)
        && temporal_aq_capability > 0; // wjy: 只有驱动能力查询明确支持时才启用Temporal AQ，避免旧驱动接受初始化却在首帧拒绝参数组合。
    std::ostringstream diagnostics;
    diagnostics << "api_version=" << api_version_
                << " input_formats_known=" << (input_formats_known_ ? 1 : 0)
                << " nv12=" << (supports_nv12_ ? 1 : 0)
                << " argb=" << (supports_argb_ ? 1 : 0)
                << " temporal_aq=" << (temporal_aq_supported_ ? 1 : 0);
    diagnostics_ = diagnostics.str(); // wjy: 初始化成功日志记录能力快照，下一次目标端复现可直接确认实际分支。
    // ===end====

    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = nvenc_struct_version(api_version_, 5, true);
    preset.presetCfg.version = nvenc_struct_version(api_version_, 9, true);
    if (!check(fn_.nvEncGetEncodePresetConfigEx(encoder_, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P3_GUID, // wjy: 恢复低延迟P3预设，避免原始分辨率60 FPS下因P5编码耗时增加而产生输出节奏波动。
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
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR; // wjy: 恢复稳定版本的低延迟 CBR，避免 VBR 在周期关键帧和静止桌面之间反复回收预算，表现为先糊后清晰。
    config.rcParams.averageBitRate = bitrate;
    // =====wjy====
    config.rcParams.maxBitRate = bitrate; // wjy: CBR 的平均值和峰值保持一致，编码输出不再短时超过 WebRTC 已分配带宽。
    config.rcParams.vbvBufferSize = std::max<uint32_t>(bitrate * 6 / safe_fps, 1); // wjy: 保留约六帧 VBV 吸收关键帧复杂度，但不允许 VBR 长周期压低后续桌面细节。
    config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
    config.rcParams.enableAQ = 1;
    config.rcParams.enableTemporalAQ = temporal_aq_supported_ ? 1u : 0u; // wjy: Temporal AQ按NVENC能力开关，旧代驱动不支持时自动保持空间AQ。
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
    init.presetGUID = NV_ENC_PRESET_P3_GUID; // wjy: 初始化参数与预设查询统一恢复P3；清晰度继续由提高后的码率下限、AQ和关键帧缓冲保障。
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
    nextConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR; // wjy: 在线调档继续使用 CBR，不让驱动从旧配置继承 VBR 后再次出现糊清波动。
    nextConfig.rcParams.maxBitRate = bitrate; // wjy: 重新配置后的编码峰值与 WebRTC 新分配码率完全一致。
    nextConfig.rcParams.vbvBufferSize = std::max<uint32_t>(bitrate * 6 / safeFps, 1); // wjy: FPS变化后按六帧重新计算缓冲时长，关键帧仍有稳定预算。
    // ===end====
    nextConfig.rcParams.vbvInitialDelay = nextConfig.rcParams.vbvBufferSize;
    nextConfig.encodeCodecConfig.hevcConfig.idrPeriod = nextConfig.gopLength;

    NV_ENC_INITIALIZE_PARAMS nextInit = init_;
    nextInit.frameRateNum = safeFps;
    nextInit.frameRateDen = 1;
    nextInit.encodeConfig = &nextConfig;
    NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
    // =====wjy====
    reconfigure.version = nvenc_struct_version(api_version_, 2, true); // wjy: NVIDIA SDK 要求 RECONFIGURE_PARAMS 使用结构版本2并带扩展位；旧版本会在 GTX 10/16 系列驱动上返回非法参数8。
    // ===end====
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
    // =====wjy====
    pic.inputPitch = desc.Width; // wjy: D3D11纹理无法直接取得字节步长时按SDK要求至少填写输入宽度，禁止保留0导致旧显卡驱动拒绝首帧。
    pic.frameIdx = frame_id; // wjy: 把调用方仅在编码成功后递增的帧号交给NVENC，保证失败重试和驱动内部帧序一致。
    // ===end====
    pic.outputBitstream = bitstream_;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.pictureType = NV_ENC_PIC_TYPE_UNKNOWN; // wjy: enablePTD已开启，由驱动根据GOP和FORCEIDR标志决定帧型，避免手工指定P帧形成不完整参考链。
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
    if (encoder_) {
        for (const auto& entry : registered_textures_) {
            if (entry.registered) fn_.nvEncUnregisterResource(encoder_, entry.registered); // wjy: 会话关闭时一次性注销纹理环全部缓存资源。
        }
    }
    registered_textures_.clear();
    registered_ = nullptr;
    registered_texture_ = nullptr;
    registered_format_ = NV_ENC_BUFFER_FORMAT_UNDEFINED;
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
    input_formats_known_ = false;
    supports_nv12_ = false;
    supports_argb_ = false;
    temporal_aq_supported_ = false;
    diagnostics_.clear();
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

// =====wjy====
bool NvencH264Encoder::query_input_formats()
{
    input_formats_known_ = false;
    supports_nv12_ = false;
    supports_argb_ = false;
    if (!encoder_ || !fn_.nvEncGetInputFormatCount || !fn_.nvEncGetInputFormats) {
        return false; // wjy: 很旧的API缺少能力函数时保留原有注册尝试，由具体NVENC调用返回结果。
    }

    uint32_t count = 0;
    if (fn_.nvEncGetInputFormatCount(encoder_, NV_ENC_CODEC_HEVC_GUID, &count) != NV_ENC_SUCCESS || count == 0) {
        return false;
    }
    std::vector<NV_ENC_BUFFER_FORMAT> formats(count, NV_ENC_BUFFER_FORMAT_UNDEFINED);
    uint32_t returned_count = 0;
    if (fn_.nvEncGetInputFormats(encoder_, NV_ENC_CODEC_HEVC_GUID, formats.data(), count, &returned_count) != NV_ENC_SUCCESS) {
        return false;
    }
    formats.resize(std::min(count, returned_count)); // wjy: 只遍历驱动实际写回的格式数量，忽略预分配数组中未使用的尾部。
    for (const NV_ENC_BUFFER_FORMAT format : formats) {
        supports_nv12_ = supports_nv12_ || format == NV_ENC_BUFFER_FORMAT_NV12; // wjy: NV12是统一GPU转换路径的首选输入。
        supports_argb_ = supports_argb_ || format == NV_ENC_BUFFER_FORMAT_ARGB; // wjy: ARGB仅作为VideoProcessor或CPU上传失败后的兼容回退。
    }
    input_formats_known_ = true;
    return true;
}

bool NvencH264Encoder::query_capability(NV_ENC_CAPS capability, int* value) const
{
    if (!encoder_ || !value || !fn_.nvEncGetEncodeCaps) return false;
    NV_ENC_CAPS_PARAM params = {};
    params.version = nvenc_struct_version(api_version_, 1);
    params.capsToQuery = capability;
    return fn_.nvEncGetEncodeCaps(encoder_, NV_ENC_CODEC_HEVC_GUID, &params, value) == NV_ENC_SUCCESS; // wjy: 能力查询失败不阻断编码，只关闭对应可选特性。
}
// ===end====

bool NvencH264Encoder::register_texture(ID3D11Texture2D* texture, std::string* error)
{
    for (const auto& entry : registered_textures_) {
        if (entry.texture.Get() == texture) {
            registered_ = entry.registered;
            registered_texture_ = texture;
            registered_format_ = entry.format;
            return true; // wjy: 捕获环再次轮到同一纹理时直接复用NVENC句柄，消除每帧驱动注册开销。
        }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    const NV_ENC_BUFFER_FORMAT buffer_format = nvenc_buffer_format(desc.Format);
    if (buffer_format == NV_ENC_BUFFER_FORMAT_UNDEFINED) {
        if (error) *error = "unsupported NVENC D3D11 texture format";
        return false; // wjy: 未知格式交给上层I420兼容回退，禁止NVENC按ARGB误读导致花屏或色偏。
    }
    // =====wjy====
    const bool format_supported = buffer_format == NV_ENC_BUFFER_FORMAT_NV12
        ? supports_nv12_
        : buffer_format == NV_ENC_BUFFER_FORMAT_ARGB && supports_argb_;
    if (input_formats_known_ && !format_supported) {
        if (error) {
            *error = "NVENC HEVC input format is not advertised by driver: "
                + std::to_string(static_cast<uint32_t>(buffer_format));
        }
        return false; // wjy: 驱动能力明确不含该格式时提前给出可读错误，不把不兼容纹理送进首帧编码。
    }
    // ===end====
    NV_ENC_REGISTER_RESOURCE registered = {};
    registered.version = nvenc_struct_version(api_version_, 5);
    registered.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    registered.width = desc.Width;
    registered.height = desc.Height;
    registered.resourceToRegister = texture;
    registered.bufferFormat = buffer_format;
    registered.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (!check(fn_.nvEncRegisterResource(encoder_, &registered), "NvEncRegisterResource", error)) {
        return false;
    }
    registered_ = registered.registeredResource;
    registered_texture_ = texture;
    registered_format_ = buffer_format;
    RegisteredTextureEntry entry;
    entry.texture = texture;
    entry.registered = registered_;
    entry.format = registered_format_;
    registered_textures_.push_back(std::move(entry)); // wjy: ComPtr持有纹理生命周期，直到NVENC会话统一注销缓存。
    return true;
}

bool NvencH264Encoder::check(NVENCSTATUS status, const char* call, std::string* error) const
{
    if (status == NV_ENC_SUCCESS) {
        return true;
    }
    if (error) {
        *error = std::string(call) + " failed: " + std::to_string(static_cast<int>(status));
        // =====wjy====
        if (encoder_ && fn_.nvEncGetLastErrorString) {
            const char* detail = fn_.nvEncGetLastErrorString(encoder_);
            if (detail && *detail) {
                *error += " detail=";
                *error += detail; // wjy: 保留驱动提供的具体参数说明，错误码8不再只能靠反复发布猜测。
            }
        }
        // ===end====
    }
    return false;
}

} // namespace lsp
