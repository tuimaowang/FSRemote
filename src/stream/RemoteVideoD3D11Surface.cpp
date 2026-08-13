#include "stream/RemoteVideoD3D11Surface.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <utility>

namespace stream {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT64 kProducerKey = 0;
constexpr UINT64 kConsumerKey = 1;
constexpr DWORD kAcquireTimeoutMs = 1; // wjy: RenderWorker最多等待1毫秒，资源忙时直接保留上一帧并服务其它窗口。

bool isDeviceLost(HRESULT result)
{
    return result == DXGI_ERROR_DEVICE_REMOVED
        || result == DXGI_ERROR_DEVICE_RESET
        || result == DXGI_ERROR_DEVICE_HUNG
        || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

RECT letterboxRect(int sourceWidth, int sourceHeight, int outputWidth, int outputHeight)
{
    RECT result{0, 0, outputWidth, outputHeight};
    if (sourceWidth <= 0 || sourceHeight <= 0 || outputWidth <= 0 || outputHeight <= 0) {
        return result;
    }
    const double scale = std::min(
        static_cast<double>(outputWidth) / sourceWidth,
        static_cast<double>(outputHeight) / sourceHeight);
    const int width = std::max(1, static_cast<int>(sourceWidth * scale + 0.5));
    const int height = std::max(1, static_cast<int>(sourceHeight * scale + 0.5));
    result.left = (outputWidth - width) / 2;
    result.top = (outputHeight - height) / 2;
    result.right = result.left + width;
    result.bottom = result.top + height;
    return result;
}

class KeyedMutexGuard final {
public:
    HRESULT acquire(ID3D11Texture2D* texture, ID3D11DeviceContext* context)
    {
        context_ = context;
        HRESULT result = texture ? texture->QueryInterface(IID_PPV_ARGS(&mutex_)) : E_INVALIDARG;
        if (FAILED(result) || !mutex_) {
            return FAILED(result) ? result : E_NOINTERFACE;
        }
        result = mutex_->AcquireSync(kConsumerKey, kAcquireTimeoutMs);
        acquired_ = result == S_OK;
        return result;
    }

    ~KeyedMutexGuard()
    {
        if (!acquired_ || !mutex_) {
            return;
        }
        if (context_) {
            context_->Flush();
        }
        mutex_->ReleaseSync(kProducerKey); // wjy: Blt命令提交后归还生产者key，解码器输出池可立即复用该槽位。
    }

private:
    ComPtr<IDXGIKeyedMutex> mutex_;
    ID3D11DeviceContext* context_ = nullptr;
    bool acquired_ = false;
};

} // namespace

struct RemoteVideoD3D11Adapter::Impl {
    explicit Impl(std::uint32_t index) : adapterIndex(index) {}

    bool ensureDevice()
    {
        if (device && context && videoDevice && videoContext) {
            return true;
        }
        reset();
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected = {};
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            &device,
            &selected,
            &context);
        if (FAILED(result)
            || FAILED(device.As(&videoDevice))
            || FAILED(context.As(&videoContext))) {
            lastError = FAILED(result) ? result : E_NOINTERFACE;
            reset();
            return false;
        }
        ++deviceGeneration;
        lastError = S_OK;
        return true;
    }

    void reset() noexcept
    {
        videoContext.Reset();
        videoDevice.Reset();
        context.Reset();
        device.Reset();
    }

    std::uint32_t adapterIndex = 0;
    std::uint64_t deviceGeneration = 0;
    HRESULT lastError = S_OK;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
};

class RemoteVideoD3D11Surface final : public RemoteVideoRenderSurface {
public:
    explicit RemoteVideoD3D11Surface(std::shared_ptr<RemoteVideoD3D11Adapter> adapter)
        : adapter_(std::move(adapter))
    {
    }

    void applyState(const RemoteVideoSurfaceState& state) override
    {
        const bool targetChanged = state_.nativeWindow != state.nativeWindow;
        const bool sizeChanged = state_.width != state.width || state_.height != state.height;
        const bool generationChanged = state_.generation != state.generation;
        state_ = state;
        if (targetChanged || sizeChanged) {
            resetSwapChain();
        }
        if (generationChanged) {
            resetImportedTextures(); // wjy: Viewer代际切换时一次性释放三槽导入缓存，旧共享句柄不会跨会话复用。
        }
    }

    RemoteVideoRenderResult render(NativeVideoFrame& frame) override
    {
        auto* impl = adapter_ ? adapter_->impl_.get() : nullptr;
        if (!impl || !impl->ensureDevice()) {
            return RemoteVideoRenderResult::DeviceLost;
        }
        if (deviceGeneration_ != impl->deviceGeneration) {
            clear();
            deviceGeneration_ = impl->deviceGeneration;
        }
        if (!ensureImportedTexture(*impl, frame)) {
            return failureResult(*impl);
        }

        KeyedMutexGuard guard;
        const HRESULT acquire = guard.acquire(
            activeImportedTexture_ ? activeImportedTexture_->texture.Get() : nullptr,
            impl->context.Get()); // wjy: 当前帧只使用匹配共享句柄的缓存槽，不再依赖单一全局导入纹理。
        if (acquire == WAIT_TIMEOUT || acquire == DXGI_ERROR_WAS_STILL_DRAWING) {
            return RemoteVideoRenderResult::RetrySync; // wjy: 尚未取得key 1时帧仍属于消费者交接中，RenderWorker必须保留原帧稍后重试。
        }
        if (acquire == WAIT_ABANDONED || FAILED(acquire)) {
            impl->lastError = acquire == WAIT_ABANDONED ? DXGI_ERROR_DEVICE_RESET : acquire;
            return failureResult(*impl);
        }
        if (!state_.nativeWindow || !state_.visible || state_.minimized
            || state_.width == 0 || state_.height == 0
            || frame.viewerGeneration != state_.generation) {
            return RemoteVideoRenderResult::DroppedStale; // wjy: 即使窗口状态已失效，也在guard析构时把当前帧真实归还key 0。
        }
        if (!ensureSwapChain(*impl)) {
            return failureResult(*impl); // wjy: SwapChain创建失败仍由已取得的guard归还共享纹理，不占死解码槽。
        }
        if (frameLatencyWaitableObject_
            && ::WaitForSingleObject(frameLatencyWaitableObject_, 0) == WAIT_TIMEOUT) {
            return RemoteVideoRenderResult::DroppedSyncBusy; // wjy: 已取得纹理后跳过本次Present，guard立即归还key 0并保留上一帧画面。
        }
        if (!ensureProcessorAndViews(*impl, frame.width, frame.height)) {
            return failureResult(*impl);
        }

        const int outputWidth = static_cast<int>(std::max<std::uint32_t>(1, state_.width));
        const int outputHeight = static_cast<int>(std::max<std::uint32_t>(1, state_.height));
        RECT sourceRect{0, 0, frame.width, frame.height};
        RECT outputRect{0, 0, outputWidth, outputHeight};
        const RECT destination = letterboxRect(frame.width, frame.height, outputWidth, outputHeight);
        D3D11_VIDEO_COLOR background{};
        background.RGBA.A = 1.0f;
        impl->videoContext->VideoProcessorSetOutputBackgroundColor(processor_.Get(), FALSE, &background);
        impl->videoContext->VideoProcessorSetStreamFrameFormat(
            processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        impl->videoContext->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &sourceRect);
        impl->videoContext->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &destination);
        impl->videoContext->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &outputRect);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = activeImportedTexture_ ? activeImportedTexture_->inputView.Get() : nullptr; // wjy: 三张轮转纹理分别复用自己的VideoProcessor输入视图。
        HRESULT result = impl->videoContext->VideoProcessorBlt(
            processor_.Get(), outputView_.Get(), 0, 1, &stream);
        if (FAILED(result)) {
            impl->lastError = result;
            return failureResult(*impl);
        }
        result = swapChain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
            return RemoteVideoRenderResult::DroppedSyncBusy;
        }
        if (FAILED(result)) {
            impl->lastError = result;
            return failureResult(*impl);
        }
        impl->lastError = S_OK;
        return RemoteVideoRenderResult::Presented;
    }

    RemoteVideoRenderResult discard(NativeVideoFrame& frame) override
    {
        auto* impl = adapter_ ? adapter_->impl_.get() : nullptr;
        if (!impl || !frame.sharedHandle || !impl->ensureDevice()) {
            return RemoteVideoRenderResult::Failed;
        }
        if (deviceGeneration_ != impl->deviceGeneration) {
            clear();
            deviceGeneration_ = impl->deviceGeneration;
        }
        if (!ensureImportedTexture(*impl, frame)) {
            return failureResult(*impl);
        }
        KeyedMutexGuard guard;
        const HRESULT acquire = guard.acquire(
            activeImportedTexture_ ? activeImportedTexture_->texture.Get() : nullptr,
            impl->context.Get()); // wjy: discard同样从当前缓存槽取得key 1并归还key 0。
        if (acquire == WAIT_TIMEOUT || acquire == DXGI_ERROR_WAS_STILL_DRAWING) {
            return RemoteVideoRenderResult::RetrySync; // wjy: discard不能把“还没拿到key”当成已丢弃，交给RenderWorker保留lease后重试。
        }
        if (acquire == WAIT_ABANDONED || FAILED(acquire)) {
            impl->lastError = acquire == WAIT_ABANDONED ? DXGI_ERROR_DEVICE_RESET : acquire;
            return failureResult(*impl);
        }
        return RemoteVideoRenderResult::DroppedStale; // wjy: 不执行Blt/Present，仅完成消费者key到生产者key的归还。
    }

    void clear() override
    {
        resetImportedTextures(); // wjy: Surface清理时释放全部三槽纹理和输入视图，避免设备换代保留旧COM资源。
        resetSwapChain();
        processor_.Reset();
        processorEnum_.Reset();
        processorSourceWidth_ = 0;
        processorSourceHeight_ = 0;
        processorOutputWidth_ = 0;
        processorOutputHeight_ = 0;
    }

    RemoteVideoSurfaceMetrics diagnostics() const noexcept override
    {
        RemoteVideoSurfaceMetrics metrics;
        metrics.textureCacheHits = textureCacheHits_;
        metrics.textureCacheMisses = textureCacheMisses_;
        metrics.viewCacheHits = viewCacheHits_;
        metrics.viewCacheMisses = viewCacheMisses_;
        metrics.deviceGeneration = deviceGeneration_; // wjy: 低频汇总读取累计值，不触碰D3D对象或执行任何GPU调用。
        return metrics;
    }

private:
    // =====wjy====
    static constexpr std::size_t kImportedTextureCacheCapacity = 3; // wjy: 与解码器三张输出纹理严格对应，稳定流只产生三次OpenSharedResource。

    struct ImportedTextureEntry {
        void* sharedHandle = nullptr;
        std::uint64_t viewerGeneration = 0;
        int width = 0;
        int height = 0;
        std::uint32_t format = 0;
        std::uint64_t lastUsed = 0;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11VideoProcessorInputView> inputView;

        bool matches(const NativeVideoFrame& frame) const noexcept
        {
            return texture
                && sharedHandle == frame.sharedHandle
                && viewerGeneration == frame.viewerGeneration
                && width == frame.width
                && height == frame.height
                && format == frame.format; // wjy: 资源身份、代际、格式和尺寸全部一致才允许命中，禁止误复用旧输出组。
        }

        void reset() noexcept
        {
            inputView.Reset(); // wjy: 输入视图引用纹理和Processor枚举，必须先释放视图再释放纹理。
            texture.Reset();
            sharedHandle = nullptr;
            viewerGeneration = 0;
            width = 0;
            height = 0;
            format = 0;
            lastUsed = 0;
        }
    };
    // ===end====

    bool ensureSwapChain(RemoteVideoD3D11Adapter::Impl& impl)
    {
        if (swapChain_) {
            return true;
        }
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(impl.device.As(&dxgiDevice))
            || FAILED(dxgiDevice->GetAdapter(&adapter))
            || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            impl.lastError = E_NOINTERFACE;
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = state_.width;
        desc.Height = state_.height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        HRESULT result = factory->CreateSwapChainForHwnd(
            impl.device.Get(),
            static_cast<HWND>(state_.nativeWindow),
            &desc,
            nullptr,
            nullptr,
            &swapChain_);
        if (FAILED(result)) {
            impl.lastError = result;
            return false;
        }
        ComPtr<IDXGISwapChain2> swapChain2;
        if (SUCCEEDED(swapChain_.As(&swapChain2))) {
            swapChain2->SetMaximumFrameLatency(1); // wjy: SwapChain最多排队一帧，旧桌面画面不会在GPU队列中积压。
            frameLatencyWaitableObject_ = swapChain2->GetFrameLatencyWaitableObject();
        }
        factory->MakeWindowAssociation(static_cast<HWND>(state_.nativeWindow), DXGI_MWA_NO_ALT_ENTER);
        return true;
    }

    bool ensureImportedTexture(RemoteVideoD3D11Adapter::Impl& impl, const NativeVideoFrame& frame)
    {
        activeImportedTexture_ = nullptr; // wjy: 每帧先清空活动指针，失败路径不会误用上一帧缓存槽。
        for (ImportedTextureEntry& entry : importedTextures_) {
            if (!entry.matches(frame)) {
                continue;
            }
            entry.lastUsed = ++textureCacheUseCounter_; // wjy: 命中后更新轻量LRU序号，异常出现第四个句柄时优先淘汰最久未用槽。
            activeImportedTexture_ = &entry;
            ++textureCacheHits_; // wjy: 三张纹理轮转后应持续命中，不再逐帧OpenSharedResource。
            return true;
        }

        ++textureCacheMisses_;
        ImportedTextureEntry* replacement = nullptr;
        for (ImportedTextureEntry& entry : importedTextures_) {
            if (!entry.texture) {
                replacement = &entry; // wjy: 初始化阶段优先填充空槽，前三张输出纹理各导入一次。
                break;
            }
            if (!replacement || entry.lastUsed < replacement->lastUsed) {
                replacement = &entry; // wjy: 同代际异常出现更多句柄时保持缓存容量恒三，禁止无界COM资源增长。
            }
        }
        if (!replacement) {
            impl.lastError = E_FAIL;
            return false;
        }
        replacement->reset();
        HRESULT result = impl.device->OpenSharedResource(
            static_cast<HANDLE>(frame.sharedHandle),
            IID_PPV_ARGS(&replacement->texture));
        if (FAILED(result) || !replacement->texture) {
            impl.lastError = FAILED(result) ? result : E_FAIL;
            replacement->reset(); // wjy: 导入失败不留下半初始化缓存键，下一帧可重新尝试或进入设备恢复。
            return false;
        }
        replacement->sharedHandle = frame.sharedHandle;
        replacement->viewerGeneration = frame.viewerGeneration;
        replacement->width = frame.width;
        replacement->height = frame.height;
        replacement->format = frame.format;
        replacement->lastUsed = ++textureCacheUseCounter_;
        activeImportedTexture_ = replacement;
        return true;
    }

    bool ensureProcessorAndViews(RemoteVideoD3D11Adapter::Impl& impl, int sourceWidth, int sourceHeight)
    {
        const bool reusable = processor_ && processorEnum_ && activeImportedTexture_
            && activeImportedTexture_->inputView && backBuffer_ && outputView_
            && processorSourceWidth_ == sourceWidth
            && processorSourceHeight_ == sourceHeight
            && processorOutputWidth_ == static_cast<int>(state_.width)
            && processorOutputHeight_ == static_cast<int>(state_.height);
        if (reusable) {
            ++viewCacheHits_; // wjy: Processor输入/输出视图与BackBuffer全部兼容时直接进入Blt，不重建任何视图。
            return true;
        }
        ++viewCacheMisses_;
        if (!processor_
            || processorSourceWidth_ != sourceWidth
            || processorSourceHeight_ != sourceHeight
            || processorOutputWidth_ != static_cast<int>(state_.width)
            || processorOutputHeight_ != static_cast<int>(state_.height)) {
            processor_.Reset();
            processorEnum_.Reset();
            resetImportedInputViews(); // wjy: Processor枚举重建后全部三槽输入视图失效，但导入纹理本身仍可继续复用。
            outputView_.Reset();
            backBuffer_.Reset();
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
            content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputWidth = static_cast<UINT>(sourceWidth);
            content.InputHeight = static_cast<UINT>(sourceHeight);
            content.OutputWidth = state_.width;
            content.OutputHeight = state_.height;
            content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            HRESULT result = impl.videoDevice->CreateVideoProcessorEnumerator(&content, &processorEnum_);
            if (FAILED(result)
                || FAILED(impl.videoDevice->CreateVideoProcessor(processorEnum_.Get(), 0, &processor_))) {
                impl.lastError = FAILED(result) ? result : E_FAIL;
                return false;
            }
            processorSourceWidth_ = sourceWidth;
            processorSourceHeight_ = sourceHeight;
            processorOutputWidth_ = static_cast<int>(state_.width);
            processorOutputHeight_ = static_cast<int>(state_.height);
        }

        if (!activeImportedTexture_ || !activeImportedTexture_->texture) {
            impl.lastError = E_FAIL;
            return false;
        }
        if (!activeImportedTexture_->inputView) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            HRESULT result = impl.videoDevice->CreateVideoProcessorInputView(
                activeImportedTexture_->texture.Get(),
                processorEnum_.Get(),
                &desc,
                &activeImportedTexture_->inputView); // wjy: 每个共享句柄只创建一次输入视图，后续轮转直接命中对应槽。
            if (FAILED(result)) {
                impl.lastError = result;
                return false;
            }
        }
        if (!backBuffer_ || !outputView_) {
            HRESULT result = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_));
            if (FAILED(result)) {
                impl.lastError = result;
                return false;
            }
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            result = impl.videoDevice->CreateVideoProcessorOutputView(
                backBuffer_.Get(), processorEnum_.Get(), &desc, &outputView_);
            if (FAILED(result)) {
                impl.lastError = result;
                return false;
            }
        }
        return true;
    }

    RemoteVideoRenderResult failureResult(RemoteVideoD3D11Adapter::Impl& impl)
    {
        HRESULT error = impl.lastError;
        if (impl.device) {
            const HRESULT removed = impl.device->GetDeviceRemovedReason();
            if (FAILED(removed)) {
                error = removed;
            }
        }
        if (isDeviceLost(error)) {
            impl.lastError = error;
            impl.reset();
            clear();
            return RemoteVideoRenderResult::DeviceLost;
        }
        return RemoteVideoRenderResult::Failed;
    }

    void resetImportedInputViews()
    {
        for (ImportedTextureEntry& entry : importedTextures_) {
            entry.inputView.Reset(); // wjy: Surface尺寸或Processor枚举变化只重建视图，不重复OpenSharedResource。
        }
    }

    void resetImportedTextures()
    {
        activeImportedTexture_ = nullptr;
        for (ImportedTextureEntry& entry : importedTextures_) {
            entry.reset(); // wjy: 代际、设备或Surface销毁时统一清空固定三槽缓存。
        }
        textureCacheUseCounter_ = 0;
    }

    void resetSwapChain()
    {
        outputView_.Reset();
        backBuffer_.Reset();
        swapChain_.Reset();
        frameLatencyWaitableObject_ = nullptr;
    }

    std::shared_ptr<RemoteVideoD3D11Adapter> adapter_;
    RemoteVideoSurfaceState state_;
    std::uint64_t deviceGeneration_ = 0;
    ComPtr<IDXGISwapChain1> swapChain_;
    HANDLE frameLatencyWaitableObject_ = nullptr;
    ComPtr<ID3D11VideoProcessorEnumerator> processorEnum_;
    ComPtr<ID3D11VideoProcessor> processor_;
    std::array<ImportedTextureEntry, kImportedTextureCacheCapacity> importedTextures_;
    ImportedTextureEntry* activeImportedTexture_ = nullptr; // wjy: 仅在RenderWorker当前调用栈使用，数组地址稳定且不会跨线程发布。
    ComPtr<ID3D11Texture2D> backBuffer_;
    ComPtr<ID3D11VideoProcessorOutputView> outputView_;
    std::uint64_t textureCacheUseCounter_ = 0;
    int processorSourceWidth_ = 0;
    int processorSourceHeight_ = 0;
    int processorOutputWidth_ = 0;
    int processorOutputHeight_ = 0;
    std::uint64_t textureCacheHits_ = 0;
    std::uint64_t textureCacheMisses_ = 0;
    std::uint64_t viewCacheHits_ = 0;
    std::uint64_t viewCacheMisses_ = 0;
};

// =====wjy====
RemoteVideoD3D11Adapter::RemoteVideoD3D11Adapter(std::uint32_t adapterIndex)
    : impl_(std::make_unique<Impl>(adapterIndex))
{
}

RemoteVideoD3D11Adapter::~RemoteVideoD3D11Adapter() = default;

std::shared_ptr<RemoteVideoRenderSurface> RemoteVideoD3D11Adapter::createSurface()
{
    return std::make_shared<RemoteVideoD3D11Surface>(shared_from_this());
}

std::uint32_t RemoteVideoD3D11Adapter::adapterIndex() const noexcept
{
    return impl_ ? impl_->adapterIndex : 0;
}

long RemoteVideoD3D11Adapter::lastDeviceError() const noexcept
{
    return impl_ ? static_cast<long>(impl_->lastError) : static_cast<long>(E_FAIL);
}

void RemoteVideoD3D11Adapter::resetDevice() noexcept
{
    if (impl_) {
        impl_->reset();
    }
}
// ===end====

} // namespace stream
