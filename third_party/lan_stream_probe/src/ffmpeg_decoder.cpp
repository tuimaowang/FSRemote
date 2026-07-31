#include "ffmpeg_decoder.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
}

namespace lsp {
namespace {

// =====wjy====
constexpr UINT64 kSharedTextureProducerKey = 0; // wjy: 生产端只有取得key 0后才能覆盖共享纹理。
constexpr UINT64 kSharedTextureConsumerKey = 1; // wjy: 完整帧提交后使用key 1交给控制端读取。
// ===end====

AVPixelFormat get_hw_format(AVCodecContext*, const AVPixelFormat* formats)
{
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

std::string fferr(const char* call, int ret)
{
    return std::string(call) + " failed: " + std::to_string(ret);
}

DecodeStatus decode_status_from_averror(int error)
{
    return error == AVERROR_INVALIDDATA
        ? DecodeStatus::CorruptBitstream
        : DecodeStatus::FatalError; // wjy: 仅FFmpeg明确标记的无效码流归为损坏，内存/参数等错误不得触发码流恢复风暴。
}

bool is_device_lost_hresult(HRESULT result)
{
    return result == DXGI_ERROR_DEVICE_REMOVED
        || result == DXGI_ERROR_DEVICE_RESET
        || result == DXGI_ERROR_DEVICE_HUNG
        || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR; // wjy: 这些HRESULT表示当前D3D设备代际不可继续使用。
}

} // namespace

H264Decoder::~H264Decoder()
{
    shutdown();
}

bool H264Decoder::initialize_d3d11(ID3D11Device* device, ID3D11DeviceContext* context, std::string* error)
{
    shutdown();
    if (!device || !context) {
        if (error) *error = "invalid D3D11 device/context";
        return false;
    }
    device_ = device;
    context_ = context;
    device_->AddRef();
    context_->AddRef();

    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&video_device_))) ||
        FAILED(context_->QueryInterface(IID_PPV_ARGS(&video_context_)))) {
        if (error) *error = "D3D11 video interfaces are not available";
        shutdown();
        return false;
    }
    return ensure(error);
}

DecodeResult H264Decoder::decode(const std::vector<uint8_t>& h264, DecodedFrame* frame, std::string* error)
{
    return decode(h264.data(), h264.size(), frame, error); // wjy: 旧vector调用者复用新的内部padding路径，保持外部接口不变。
}

DecodeResult H264Decoder::decode(const uint8_t* h264, std::size_t h264Size, DecodedFrame* frame, std::string* error)
{
    if (error) {
        error->clear(); // wjy: NeedMoreInput和OutputTextureBusy不能携带上一帧遗留错误，避免上层误判解码链已损坏。
    }
    if (!frame) {
        if (error) *error = "decoded frame output is null";
        return {DecodeStatus::FatalError};
    }
    if (!h264 || h264Size == 0) {
        if (error) *error = "encoded frame is empty";
        return {DecodeStatus::CorruptBitstream};
    }
    if (!ensure(error)) {
        return {current_device_failure_status()};
    }
    av_packet_unref(packet_);
    // =====wjy====
    // FFmpeg可能读取压缩包末尾的padding字节；输入内存由解码器统一复用并始终补零，避免越界读取。
    constexpr std::size_t kPacketPadding = AV_INPUT_BUFFER_PADDING_SIZE;
    packet_input_buffer_.resize(h264Size + kPacketPadding);
    std::memcpy(packet_input_buffer_.data(), h264, h264Size);
    std::memset(packet_input_buffer_.data() + h264Size, 0, kPacketPadding);
    packet_->data = packet_input_buffer_.data();
    packet_->size = static_cast<int>(h264Size);
    // ===end====
    const int sent = avcodec_send_packet(codec_, packet_);
    if (sent == AVERROR(EAGAIN)) {
        return receive(frame, error);
    }
    if (sent < 0) {
        if (error) *error = fferr("avcodec_send_packet", sent);
        const DecodeStatus deviceStatus = current_device_failure_status();
        return {deviceStatus == DecodeStatus::DeviceLost
            ? deviceStatus
            : decode_status_from_averror(sent)}; // wjy: FFmpeg外部错误发生时优先检查底层D3D设备，避免把Device Removed误记为码流损坏。
    }
    return receive(frame, error);
}

void H264Decoder::reset()
{
    shutdown();
}

bool H264Decoder::ensure(std::string* error)
{
    if (codec_) {
        return true;
    }
    if (!device_ || !context_) {
        if (error) *error = "D3D11 decoder was not initialized";
        return false;
    }

    AVBufferRef* device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!device_ref) {
        if (error) {
            std::string types;
            AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
            while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
                if (!types.empty()) types += ",";
                types += av_hwdevice_get_type_name(type);
            }
            *error = "av_hwdevice_ctx_alloc(D3D11VA) failed; available_hw_types=[" + types + "]";
        }
        return false;
    }
    auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
    auto* d3d11 = reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
    d3d11->device = device_;
    d3d11->device_context = context_;
    d3d11->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    device_->AddRef();
    context_->AddRef();

    const int hw_init = av_hwdevice_ctx_init(device_ref);
    if (hw_init < 0) {
        if (error) *error = fferr("av_hwdevice_ctx_init(D3D11VA)", hw_init);
        av_buffer_unref(&device_ref);
        return false;
    }
    hw_device_ = device_ref;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) {
        if (error) *error = "HEVC decoder not found";
        shutdown();
        return false;
    }
    bool supports_d3d11 = false;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            break;
        }
        if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
            config->pix_fmt == AV_PIX_FMT_D3D11 &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            supports_d3d11 = true;
            break;
        }
    }
    if (!supports_d3d11) {
        if (error) *error = "HEVC decoder has no D3D11VA hardware config";
        shutdown();
        return false;
    }
    codec_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!codec_ || !frame_ || !packet_) {
        if (error) *error = "FFmpeg decoder allocation failed";
        shutdown();
        return false;
    }

    codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_->thread_count = 1;
    codec_->get_format = get_hw_format;
    codec_->hw_device_ctx = av_buffer_ref(hw_device_);
    if (!codec_->hw_device_ctx) {
        if (error) *error = "av_buffer_ref(hw_device) failed";
        shutdown();
        return false;
    }

    const int opened = avcodec_open2(codec_, codec, nullptr);
    if (opened < 0) {
        if (error) *error = fferr("avcodec_open2(D3D11VA)", opened);
        shutdown();
        return false;
    }
    return true;
}

DecodeResult H264Decoder::receive(DecodedFrame* decoded, std::string* error)
{
    const int ret = avcodec_receive_frame(codec_, frame_);
    if (ret == AVERROR(EAGAIN)) {
        return {DecodeStatus::NeedMoreInput}; // wjy: 解码器正常等待后续压缩数据，不重置、不请求关键帧。
    }
    if (ret < 0) {
        if (error) *error = fferr("avcodec_receive_frame", ret);
        const DecodeStatus deviceStatus = current_device_failure_status();
        return {deviceStatus == DecodeStatus::DeviceLost
            ? deviceStatus
            : decode_status_from_averror(ret)}; // wjy: 硬解码接收失败可能由设备代际失效引起，必须先进入当前Viewer设备恢复。
    }

    if (frame_->format != AV_PIX_FMT_D3D11) {
        if (error) *error = "decoder did not return a D3D11 frame";
        av_frame_unref(frame_);
        return {DecodeStatus::FatalError};
    }
    const DecodeResult result = convert_d3d11_frame(decoded, error);
    av_frame_unref(frame_);
    return result;
}

DecodeResult H264Decoder::convert_d3d11_frame(DecodedFrame* decoded, std::string* error)
{
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
    const UINT array_slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame_->data[1]));
    if (!texture) {
        if (error) *error = "D3D11 decoded texture is null";
        return {DecodeStatus::FatalError};
    }
    if (!ensure_video_processor(frame_->width, frame_->height, error)) {
        return {current_device_failure_status()};
    }

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.ArraySlice = array_slice;
    input_desc.Texture2D.MipSlice = 0;
    HRESULT hr = video_device_->CreateVideoProcessorInputView(texture, processor_enum_.Get(), &input_desc, &input_view);
    if (FAILED(hr)) {
        if (error) *error = "CreateVideoProcessorInputView failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
        return {is_device_lost_hresult(hr) ? DecodeStatus::DeviceLost : current_device_failure_status()};
    }

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc = {};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_desc.Texture2D.MipSlice = 0;
    // =====wjy====
    DecodeStatus acquireStatus = DecodeStatus::FatalError;
    const int write_index = acquire_output_texture(&acquireStatus, error); // wjy: keyed mutex保证不会覆盖Qt仍在显示或排队的纹理。
    if (write_index < 0) {
        return {acquireStatus}; // wjy: 三槽正常繁忙只丢本次显示输出，不破坏已经完成的H265参考链。
    }
    // ===end====
    hr = video_device_->CreateVideoProcessorOutputView(output_textures_[write_index].Get(), processor_enum_.Get(), &output_desc, &output_view);
    if (FAILED(hr)) {
        output_keyed_mutexes_[write_index]->ReleaseSync(kSharedTextureProducerKey); // wjy: 创建输出视图失败时立即归还生产者key。
        if (error) *error = "CreateVideoProcessorOutputView failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
        return {is_device_lost_hresult(hr) ? DecodeStatus::DeviceLost : current_device_failure_status()};
    }

    RECT rect = {0, 0, frame_->width, frame_->height};
    video_context_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    // =====wjy====
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color_space = {};
    input_color_space.YCbCr_Matrix = 1; // wjy: HEVC桌面视频按BT.709矩阵解读，避免驱动使用SD色彩矩阵导致灰阶和颜色偏差。
    input_color_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235; // wjy: NV12/P010解码面是视频limited range，显式声明后由视频处理器完成正确扩展。
    video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &input_color_space); // wjy: 不依赖不同显卡驱动的默认颜色范围，保证共享纹理和软件回退得到一致结果。
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color_space = {};
    output_color_space.RGB_Range = 1; // wjy: 输出BGRA使用桌面full range，恢复黑位、白位和细节对比度。
    output_color_space.YCbCr_Matrix = 1; // wjy: 输入输出统一使用BT.709转换语义，适配当前1080p及以上远控桌面。
    output_color_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255; // wjy: 共享BGRA纹理中的每个通道直接覆盖0到255，后续呈现不再二次扩展。
    video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &output_color_space); // wjy: 色彩修正在解码设备上只执行GPU视频处理，不增加CPU逐像素开销。
    // ===end====
    video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &rect);
    video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &rect);
    video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    hr = video_context_->VideoProcessorBlt(processor_.Get(), output_view.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        output_keyed_mutexes_[write_index]->ReleaseSync(kSharedTextureProducerKey); // wjy: Blt失败没有可交付帧，保持槽位仍归生产端。
        if (error) *error = "VideoProcessorBlt failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
        return {is_device_lost_hresult(hr) ? DecodeStatus::DeviceLost : current_device_failure_status()};
    }
    context_->Flush(); // wjy: 跨D3D11设备共享纹理前立即提交生产端Blt，避免控制端读到尚未执行完成的新纹理初始黑色内容。

    decoded->size = {static_cast<uint32_t>(frame_->width), static_cast<uint32_t>(frame_->height)};
    decoded->bgra.clear();
    output_index_ = write_index;
    decoded->srv = output_srvs_[output_index_];
    decoded->shared_handle = output_shared_handles_[output_index_];
    decoded->shared_texture_index = output_index_; // wjy: 上层根据纹理回调结果决定交给消费者key还是直接归还生产者key。
    decoded->shared_texture_locked = true; // wjy: 回调返回前生产端持续持有该槽，消费者只能在正式交接后读取。
    return {DecodeStatus::Success};
}

// =====wjy====
void H264Decoder::release_shared_texture(DecodedFrame* frame, bool consumerAccepted)
{
    if (!frame || !frame->shared_texture_locked
        || frame->shared_texture_index < 0
        || frame->shared_texture_index >= kOutputTextureCount) {
        return;
    }

    const int textureIndex = frame->shared_texture_index;
    if (output_keyed_mutexes_[textureIndex]) {
        context_->Flush(); // wjy: 将本设备对共享纹理的命令提交后再切换同步key。
        output_keyed_mutexes_[textureIndex]->ReleaseSync(
            consumerAccepted ? kSharedTextureConsumerKey : kSharedTextureProducerKey); // wjy: 接受帧交给Presenter；丢帧或BGRA回退则立即复用。
    }
    frame->shared_texture_locked = false;
    frame->shared_texture_index = -1;
}

int H264Decoder::acquire_output_texture(DecodeStatus* status, std::string* error)
{
    if (status) {
        *status = DecodeStatus::OutputTextureBusy; // wjy: 所有槽仅WAIT_TIMEOUT时属于正常显示背压。
    }
    collect_retired_output_textures(); // wjy: 每帧回收已经被控制端释放的旧分辨率纹理组。
    for (int offset = 1; offset <= kOutputTextureCount; ++offset) {
        const int candidate = (output_index_ + offset + kOutputTextureCount) % kOutputTextureCount;
        if (!output_keyed_mutexes_[candidate]) {
            continue;
        }
        const HRESULT hr = output_keyed_mutexes_[candidate]->AcquireSync(kSharedTextureProducerKey, 0);
        if (hr == S_OK || hr == WAIT_ABANDONED) {
            return candidate; // wjy: 非阻塞选择空闲槽，解码线程不等待Qt释放某一张固定纹理。
        }
        if (hr != WAIT_TIMEOUT && FAILED(hr)) {
            if (error) *error = "AcquireSync(BGRA output) failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
            if (status) {
                *status = is_device_lost_hresult(hr)
                    ? DecodeStatus::DeviceLost
                    : current_device_failure_status(); // wjy: keyed mutex API自身失败不能伪装成普通槽位繁忙。
            }
        }
    }
    if (error && error->empty()) {
        *error = "no writable keyed shared texture";
    }
    return -1;
}

DecodeStatus H264Decoder::current_device_failure_status() const
{
    if (!device_) {
        return DecodeStatus::FatalError;
    }
    const HRESULT removalReason = device_->GetDeviceRemovedReason();
    return FAILED(removalReason) || is_device_lost_hresult(removalReason)
        ? DecodeStatus::DeviceLost
        : DecodeStatus::FatalError; // wjy: 设备仍健康时保留资源/契约错误类别，避免无意义地重建设备。
}

void H264Decoder::retire_output_textures()
{
    bool hasTextures = false;
    RetiredOutputTextures retired;
    for (int i = 0; i < kOutputTextureCount; ++i) {
        hasTextures = hasTextures || output_textures_[i].Get() != nullptr;
        retired.textures[i] = std::move(output_textures_[i]); // wjy: 延长旧资源生命周期，Qt队列中的共享句柄不会在分辨率切换时悬空。
        retired.keyed_mutexes[i] = std::move(output_keyed_mutexes_[i]);
        retired.shared_handles[i] = output_shared_handles_[i];
        output_srvs_[i].Reset();
        output_shared_handles_[i] = nullptr;
    }
    if (hasTextures) {
        retired_output_textures_.push_back(std::move(retired));
    }
    output_index_ = -1;
}

void H264Decoder::collect_retired_output_textures()
{
    for (auto iterator = retired_output_textures_.begin(); iterator != retired_output_textures_.end();) {
        bool allReleased = true;
        for (const auto& keyedMutex : iterator->keyed_mutexes) {
            if (!keyedMutex) {
                continue;
            }
            const HRESULT hr = keyedMutex->AcquireSync(kSharedTextureProducerKey, 0);
            if (hr == S_OK || hr == WAIT_ABANDONED) {
                keyedMutex->ReleaseSync(kSharedTextureProducerKey); // wjy: 探测后保持生产者key不变，确认该槽没有待消费帧。
            } else {
                allReleased = false;
                break;
            }
        }
        if (allReleased) {
            iterator = retired_output_textures_.erase(iterator); // wjy: 全部槽归还后才销毁旧纹理和共享句柄。
        } else {
            ++iterator;
        }
    }
}
// ===end====

bool H264Decoder::ensure_video_processor(int width, int height, std::string* error)
{
    if (processor_ && processor_width_ == width && processor_height_ == height) {
        return true;
    }

    processor_.Reset();
    processor_enum_.Reset();
    collect_retired_output_textures(); // wjy: 创建新尺寸前清理已经完成消费的历史纹理组。
    retire_output_textures(); // wjy: 尚在Qt单槽或Presenter中的旧句柄继续有效，避免尺寸切换闪黑。

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&content, &processor_enum_);
    if (FAILED(hr)) {
        if (error) *error = "CreateVideoProcessorEnumerator failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
        return false;
    }
    hr = video_device_->CreateVideoProcessor(processor_enum_.Get(), 0, &processor_);
    if (FAILED(hr)) {
        if (error) *error = "CreateVideoProcessor failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX; // wjy: keyed mutex同时提供跨设备共享和明确的GPU读写所有权。
    for (int i = 0; i < kOutputTextureCount; ++i) {
        hr = device_->CreateTexture2D(&desc, nullptr, &output_textures_[i]);
        if (FAILED(hr)) {
            if (error) *error = "CreateTexture2D(BGRA output) failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        hr = device_->CreateShaderResourceView(output_textures_[i].Get(), &srv_desc, &output_srvs_[i]);
        if (FAILED(hr)) {
            if (error) *error = "CreateShaderResourceView(BGRA output) failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
            return false;
        }
        hr = output_textures_[i].As(&output_keyed_mutexes_[i]);
        if (FAILED(hr) || !output_keyed_mutexes_[i]) {
            if (error) *error = "QueryInterface(IDXGIKeyedMutex) failed for BGRA output texture";
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIResource> shared_resource;
        hr = output_textures_[i].As(&shared_resource);
        if (FAILED(hr) || !shared_resource) {
            if (error) *error = "QueryInterface(IDXGIResource) failed for BGRA output texture";
            return false;
        }
        HANDLE shared_handle = nullptr;
        hr = shared_resource->GetSharedHandle(&shared_handle);
        if (FAILED(hr) || !shared_handle) {
            if (error) *error = "GetSharedHandle(BGRA output) failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
            return false;
        }
        output_shared_handles_[i] = shared_handle;
    }

    output_index_ = -1; // wjy: 第一帧从0号槽开始，后续环形选择已经归还的纹理。
    processor_width_ = width;
    processor_height_ = height;
    return true;
}

void H264Decoder::shutdown()
{
    processor_.Reset();
    processor_enum_.Reset();
    for (int i = 0; i < kOutputTextureCount; ++i) {
        output_srvs_[i].Reset();
        output_keyed_mutexes_[i].Reset();
        output_textures_[i].Reset();
        output_shared_handles_[i] = nullptr;
    }
    retired_output_textures_.clear(); // wjy: Viewer停止后统一释放活动和退役共享纹理。
    output_index_ = -1;
    video_context_.Reset();
    video_device_.Reset();

    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&codec_);
    av_buffer_unref(&hw_device_);

    if (context_) {
        context_->Release();
        context_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    processor_width_ = 0;
    processor_height_ = 0;
}

} // namespace lsp
