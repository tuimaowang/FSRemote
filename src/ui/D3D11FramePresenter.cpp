#include "ui/D3D11FramePresenter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h> // wjy: 读取Overlay翻转模型当前BackBuffer索引，黑闪诊断不修改SwapChain行为。
#include <wrl/client.h>

#include <QMouseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QResizeEvent>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QTextStream>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

namespace ui {

namespace {

using Microsoft::WRL::ComPtr;

// =====wjy====
RECT letterbox_rect(int sourceWidth, int sourceHeight, int outputWidth, int outputHeight)
{
    RECT result = {0, 0, outputWidth, outputHeight};
    if (sourceWidth <= 0 || sourceHeight <= 0 || outputWidth <= 0 || outputHeight <= 0) {
        return result;
    }

    const QSize scaled = QSize(sourceWidth, sourceHeight).scaled(
        QSize(outputWidth, outputHeight),
        Qt::KeepAspectRatio); // wjy: 与RemoteDesktopWindow::remoteImageRect使用同一Qt取整规则，输入命中、浮层锚点和D3D画面边缘保持逐像素一致。
    result.left = (outputWidth - scaled.width()) / 2; // wjy: 水平方向居中，左右黑边宽度最多只相差一个取整像素。
    result.top = (outputHeight - scaled.height()) / 2; // wjy: 垂直方向居中，上下黑边与Qt软件绘制位置一致。
    result.right = result.left + scaled.width(); // wjy: 右边界使用左边界加缩放宽度，避免再次除法产生不同取整。
    result.bottom = result.top + scaled.height(); // wjy: 四周剩余区域与视频共用同一backbuffer，不再依赖Qt父窗口补画黑边。
    return result;
}

constexpr UINT64 kSharedTextureProducerKey = 0; // wjy: Presenter读取结束后归还给解码生产端的key。
constexpr UINT64 kSharedTextureConsumerKey = 1; // wjy: 解码器完成Blt后交给Presenter读取的key。
constexpr DWORD kSharedTextureAcquireTimeoutMs = 16; // wjy: UI线程最多等待一个60 FPS帧周期，驱动异常不能无限冻结全部远控窗口。

std::mutex g_presenterDiagnosticMutex;
std::atomic_uint64_t g_lastPresenterTimeoutLogMs = 0;

void appendPresenterSyncDiagnostic(const char* operation, HRESULT result, void* sharedHandle)
{
    const std::uint64_t nowMs = ::GetTickCount64();
    if (result == WAIT_TIMEOUT) {
        std::uint64_t previous = g_lastPresenterTimeoutLogMs.load(std::memory_order_acquire);
        if (nowMs - previous < 2000
            || !g_lastPresenterTimeoutLogMs.compare_exchange_strong(
                previous, nowMs, std::memory_order_acq_rel)) {
            return; // wjy: 多窗口同时背压时全进程最多每2秒记录一次超时，日志IO不能反过来放大卡顿。
        }
    }

    std::lock_guard lock(g_presenterDiagnosticMutex);
    const QString dataDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
    QDir().mkpath(dataDir); // wjy: 发布目录首次运行也保证诊断文件落在程序data文件夹。
    QFile file(QDir(dataDir).filePath(QStringLiteral("remote_presenter_diagnostic.log")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return; // wjy: 诊断失败绝不改变D3D恢复和远控会话状态。
    }
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
           << QStringLiteral(" operation=") << QString::fromLatin1(operation ? operation : "unknown")
           << QStringLiteral(" result=0x") << QString::number(static_cast<qulonglong>(static_cast<unsigned long>(result)), 16)
           << QStringLiteral(" handle=0x") << QString::number(reinterpret_cast<quintptr>(sharedHandle), 16)
           << QStringLiteral(" thread=") << static_cast<qulonglong>(::GetCurrentThreadId())
           << QStringLiteral(" wait_ms=") << kSharedTextureAcquireTimeoutMs
           << Qt::endl; // wjy: 每条异常立即刷盘，程序被驱动故障终止时仍保留最后同步节点。
}

struct SharedTextureReadGuard {
    ComPtr<IDXGIKeyedMutex> keyedMutex;
    ComPtr<ID3D11DeviceContext> context;
    bool acquired = false;

    HRESULT acquire(ID3D11Texture2D* texture, ID3D11DeviceContext* deviceContext)
    {
        if (!texture || !deviceContext) {
            return E_INVALIDARG;
        }
        HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(&keyedMutex));
        if (FAILED(hr) || !keyedMutex) {
            return FAILED(hr) ? hr : E_NOINTERFACE;
        }
        context = deviceContext;
        hr = keyedMutex->AcquireSync(kSharedTextureConsumerKey, kSharedTextureAcquireTimeoutMs); // wjy: 正常交接应立即可用；异常时一个帧周期内返回，绝不永久阻塞Qt线程。
        acquired = hr == S_OK; // wjy: WAIT_ABANDONED表示所有权不可信，不能读取、更不能在析构时伪造ReleaseSync。
        return hr;
    }

    ComPtr<IDXGIKeyedMutex> retain()
    {
        if (!acquired || !keyedMutex) {
            return {};
        }
        if (context) {
            context->Flush(); // wjy: 新前台帧继续由Presenter持有消费者key，先提交读取命令再转移互斥量所有权。
        }
        acquired = false; // wjy: 所有权已经转交给Impl，禁止析构函数立即把最后一帧归还并允许生产端覆盖。
        context.Reset();
        return std::move(keyedMutex);
    }

    ~SharedTextureReadGuard()
    {
        if (!acquired || !keyedMutex) {
            return;
        }
        if (context) {
            context->Flush(); // wjy: 将VideoProcessor读取命令提交后再把纹理归还生产者，禁止下一帧提前覆盖。
        }
        keyedMutex->ReleaseSync(kSharedTextureProducerKey);
    }
};
// ===end====

// =====wjy====
struct SharedPresentationDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    std::atomic_bool invalidated = false; // wjy: 任一窗口检测到设备移除后立即使全部Presenter在下一帧切换到新代际。
    std::atomic_long removalReason = S_OK;
    uint64_t generation = 0;
};

std::mutex g_presentationDeviceMutex;
std::weak_ptr<SharedPresentationDevice> g_presentationDevice;
uint64_t g_presentationDeviceGeneration = 0;

std::shared_ptr<SharedPresentationDevice> acquirePresentationDevice()
{
    std::lock_guard lock(g_presentationDeviceMutex);
    if (std::shared_ptr<SharedPresentationDevice> existing = g_presentationDevice.lock()) {
        if (!existing->invalidated.load(std::memory_order_acquire)) {
            return existing; // wjy: 20个窗口共享默认显卡上的Device/Context，每个窗口只单独持有SwapChain和视频处理器视图。
        }
    }

    auto shared = std::make_shared<SharedPresentationDevice>();
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL featureLevel = {};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        2,
        D3D11_SDK_VERSION,
        &shared->device,
        &featureLevel,
        &shared->context);
    if (FAILED(result)
        || FAILED(shared->device.As(&shared->videoDevice))
        || FAILED(shared->context.As(&shared->videoContext))) {
        return {};
    }
    shared->generation = ++g_presentationDeviceGeneration;
    g_presentationDevice = shared;
    return shared;
}

void invalidatePresentationDevice(const std::shared_ptr<SharedPresentationDevice>& shared, HRESULT reason)
{
    if (!shared) {
        return;
    }
    shared->removalReason.store(reason, std::memory_order_release);
    shared->invalidated.store(true, std::memory_order_release); // wjy: 旧设备仍由正在返回的调用安全持有，后续Presenter不会再复用它。
    std::lock_guard lock(g_presentationDeviceMutex);
    if (g_presentationDevice.lock() == shared) {
        g_presentationDevice.reset(); // wjy: 下一次ensureDevice创建新代际，不在失败调用栈内同步影响其它窗口对象。
    }
}
// ===end====

} // namespace

struct D3D11FramePresenter::Impl {
    std::shared_ptr<SharedPresentationDevice> sharedDevice; // wjy: Device/Context进程级共享，SwapChain仍严格属于当前HWND。
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<IDCompositionDevice> compositionDevice; // wjy: 顶层窗口唯一的DirectComposition设备，旧路径为空。
    ComPtr<IDCompositionTarget> compositionTarget; // wjy: 把根视觉树绑定到RemoteDesktopWindow顶层HWND。
    ComPtr<IDCompositionVisual> compositionRoot;
    ComPtr<IDCompositionVisual> compositionVideo;
    ComPtr<IDCompositionVisual> compositionOverlay;
    ComPtr<IDXGISwapChain> compositionOverlaySwapChain;
    ComPtr<IDXGISwapChain> compositionOverlayCandidateSwapChain;
    int compositionOverlayWidth = 0;
    int compositionOverlayHeight = 0;
    int compositionOverlayCandidateWidth = 0;
    int compositionOverlayCandidateHeight = 0;
    int compositionOverlayTargetWidth = 0;
    int compositionOverlayTargetHeight = 0; // wjy: 保存调用方本次生成叠加图的真实物理尺寸，DComp不再依赖Win32/Qt是否启用DPI虚拟化来猜测目标大小。
    int compositionOverlayFullPresentsRemaining = 0; // wjy: 双缓冲新表面先完整写入两个BackBuffer，之后才允许局部标题栏上传。
    HWND compositorHostWindow = nullptr;
    QRect compositorOutputRect;
    bool compositorMode = false;
    bool compositorVisible = true;
    ComPtr<ID3D11VideoProcessorEnumerator> processorEnum;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11Texture2D> currentTexture;
    ComPtr<IDXGIKeyedMutex> currentKeyedMutex; // wjy: 持有最后成功帧的消费者key，直到下一帧成功替换或Presenter重置。
    void* currentHandle = nullptr;
    int currentTextureWidth = 0;
    int currentTextureHeight = 0; // wjy: 拖拽按下时直接按最后成功源尺寸建立私有缓存，不等待下一张网络帧。
    int swapWidth = 0;
    int swapHeight = 0; // wjy: 当前BackBuffer物理尺寸；普通状态精确匹配客户区，交互拖拽期间暂时保留手势开始时的稳定尺寸。
    int processorSourceWidth = 0;
    int processorSourceHeight = 0;
    int processorOutputWidth = 0;
    int processorOutputHeight = 0;
    HRESULT lastDeviceRemovalReason = S_OK;
    bool hasPresentedFrame = false; // wjy: 只有Present成功后才置位，失败路径据此保留SwapChain中的最后有效画面。
    bool lastFailureWasDeviceLost = false; // wjy: 普通共享句柄瞬时失败不清空最后画面，真实设备移除才触发完整恢复。
    HRESULT lastPresentResult = S_OK; // wjy: 记录最近一次Blt/Present结果，缩放诊断在松手后读取而不在高频路径写日志。
    bool interactiveResize = false; // wjy: 鼠标拖拽窗口边缘时保持旧BackBuffer有效，由DWM把持续呈现的新帧缩放到实时变化的子HWND。
    bool resizePending = false; // wjy: 记录交互期间客户区已经变化，松手后只按最终尺寸执行一次ResizeBuffers和缓存帧补画。
    quint64 resizeEventCount = 0; // wjy: 当前Presenter在本次及后续手势中收到的子窗口resizeEvent累计次数。
    quint64 resizePresentCount = 0; // wjy: 统计交互尺寸变化触发的缓存帧补Present次数，和网络帧Present分开观察。
    quint64 nativeEventCount = 0; // wjy: 统计当前缩放手势中原生子HWND收到的关键Windows消息总数。
    quint64 nativeWindowPosChangingCount = 0; // wjy: 记录子HWND开始调整位置/尺寸的消息次数。
    quint64 nativeWindowPosChangedCount = 0; // wjy: 记录子HWND完成位置/尺寸提交的消息次数。
    quint64 nativeSizeCount = 0; // wjy: 记录子HWND收到WM_SIZE的次数。
    quint64 nativePaintCount = 0; // wjy: 记录子HWND收到WM_PAINT的次数，判断是否发生原生重绘空档。
    quint64 nativeEraseBackgroundCount = 0; // wjy: 记录子HWND收到WM_ERASEBKGND的次数，区分擦除造成的空白。
    quint64 nativeShowWindowCount = 0; // wjy: 记录子HWND显隐切换次数，确认闪烁期间是否被Windows短暂隐藏。
    quint64 nativePaintHandledCount = 0; // wjy: 记录本Presenter实际拦截并验证的WM_PAINT次数。
    quint64 nativeEraseHandledCount = 0; // wjy: 记录本Presenter实际拦截的WM_ERASEBKGND次数。
    quint64 nativeNoRedrawWindowPosCount = 0; // wjy: 记录交互缩放期间成功给Qt提交的WINDOWPOS注入SWP_NOREDRAW次数。
    quint64 interactivePresentCount = 0; // wjy: 记录本次缩放期间真正成功提交到SwapChain的网络帧数量。
    std::uint64_t compositionCommitCount = 0;
    std::uint64_t compositionCommitFailureCount = 0;
    std::uint64_t compositionCommitTimeUs = 0;
    std::uint64_t compositionOverlayFullPresentCount = 0;
    std::uint64_t compositionOverlayPartialPresentCount = 0;
    std::uint64_t compositionOverlayPresentFailureCount = 0;
    std::uint64_t compositionOverlayUploadedBytes = 0;
    // =====wjy====
    std::uint64_t compositionOverlayPresentSequence = 0; // wjy: 记录实际进入Overlay Present的顺序，供黑闪时间线关联视频帧。
    QString lastCompositionOverlayMode = QStringLiteral("none"); // wjy: 记录最近一次Overlay走完整提交还是脏区提交。
    QRect lastCompositionOverlayDirtyRect; // wjy: 记录最近一次Overlay使用的物理脏区。
    int lastCompositionOverlayBackBufferIndex = -1; // wjy: 记录Present前的BackBuffer索引，检查双缓冲初始内容是否完整。
    HRESULT lastCompositionOverlayPresentResult = S_OK; // wjy: 记录最近一次Overlay Present HRESULT。
    HRESULT lastCompositionCommitResult = S_OK; // wjy: 记录最近一次DComp Commit HRESULT。
    std::uint64_t lastCompositionCommitTimeUs = 0; // wjy: 记录最近一次DComp Commit耗时。
    // ===end====
    QString nativeLastMessage = QStringLiteral("none"); // wjy: 保存最近一次关键原生消息名称，和父窗口resize时序绑定输出。
    ComPtr<ID3D11Texture2D> resizeCacheTexture; // wjy: 交互缩放期间独占的最后一帧副本，不受共享纹理keyed mutex归还影响。
    int resizeCacheWidth = 0;
    int resizeCacheHeight = 0; // wjy: 缓存副本自身的源分辨率，缩放期间letterbox按它计算而不是窗口尺寸。
};

D3D11FramePresenter::D3D11FramePresenter(QWidget* parent)
    : QWidget(parent)
    , m_impl(new Impl)
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setUpdatesEnabled(false);
    hide();
}

void D3D11FramePresenter::setCompositorHostWindow(void* hostWindow, bool enabled)
{
    if (!m_impl) {
        return;
    }
    const HWND requestedWindow = static_cast<HWND>(hostWindow);
    if (m_impl->compositorMode == enabled
        && m_impl->compositorHostWindow == requestedWindow) {
        return;
    }
    m_impl->compositorMode = enabled;
    m_impl->compositorHostWindow = enabled ? requestedWindow : nullptr;
    m_impl->compositorOutputRect = QRect();
    m_impl->compositorVisible = true;
    resetCompositionResources(); // wjy: 切换输出所有权时先断开旧视觉树，避免同一SwapChain同时挂到两个窗口目标。
    m_impl->swapChain.Reset();
    m_impl->swapWidth = 0;
    m_impl->swapHeight = 0;
}

bool D3D11FramePresenter::usesCompositorSurface() const
{
    return m_impl && m_impl->compositorMode;
}

bool D3D11FramePresenter::hasVisiblePresentation() const
{
    if (!m_impl) {
        return false;
    }
    return m_impl->compositorMode
        ? (m_impl->hasPresentedFrame && m_impl->compositorVisible)
        : isVisible();
}

bool D3D11FramePresenter::hasCompositorOverlay() const
{
    return m_impl
        && m_impl->compositorMode
        && m_impl->compositionOverlaySwapChain;
}

void D3D11FramePresenter::setPresentationVisible(bool visible)
{
    if (!m_impl) {
        return;
    }
    if (!m_impl->compositorMode) {
        if (visible == !isHidden()) {
            return; // wjy: 使用显式隐藏状态去重；父窗口暂时隐藏时仍能正确保存子窗口下次显示意图。
        }
        if (visible) {
            show();
        } else {
            hide();
        }
        return;
    }
    if (m_impl->compositorVisible == visible) {
        return; // wjy: 正常视频帧会反复请求visible=true；状态未变化时禁止重复DComp Commit。
    }
    m_impl->compositorVisible = visible;
    if (!visible && isVisible()) {
        hide(); // wjy: DComp模式只隐藏备用控件本身，真正视频由视觉树的透明度控制。
    }
    commitCompositionVisual(); // wjy: 仅可见状态真正变化时提交视觉树，视频SwapChain帧不再触发这里。
}

bool D3D11FramePresenter::setCompositorOutputRect(const QRect& rect)
{
    if (!m_impl || !m_impl->compositorMode) {
        return false;
    }
    if (m_impl->compositorOutputRect == rect) {
        return true;
    }
    const QRect previousRect = m_impl->compositorOutputRect;
    m_impl->compositorOutputRect = rect;
    if (!m_impl->compositionDevice || !m_impl->compositionVideo || !m_impl->compositionOverlay
        || !m_impl->swapChain || m_impl->swapWidth <= 0 || m_impl->swapHeight <= 0) {
        return true; // 首帧前只保存目标；视频SwapChain创建后会与内容在同一次Commit中生效。
    }
    if (commitCompositionVisual()) {
        return true;
    }
    m_impl->compositorOutputRect = previousRect;
    if (m_impl->compositionDevice && m_impl->compositionVideo && m_impl->compositionOverlay) {
        commitCompositionVisual(); // 普通提交失败时尽力恢复上一份可见几何；设备丢失路径已经释放视觉树。
    }
    return false;
}

bool D3D11FramePresenter::ensureCompositionTarget()
{
    if (!m_impl || !m_impl->compositorMode || !m_impl->compositorHostWindow
        || !m_impl->sharedDevice || !m_impl->sharedDevice->device) {
        return false;
    }
    if (m_impl->compositionDevice && m_impl->compositionTarget
        && m_impl->compositionRoot && m_impl->compositionVideo
        && m_impl->compositionOverlay) {
        return true;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_impl->sharedDevice->device.As(&dxgiDevice))) {
        return false;
    }
    if (FAILED(DCompositionCreateDevice(
            dxgiDevice.Get(), IID_PPV_ARGS(&m_impl->compositionDevice)))) {
        return false;
    }
    if (FAILED(m_impl->compositionDevice->CreateTargetForHwnd(
            m_impl->compositorHostWindow, TRUE, &m_impl->compositionTarget))
        || FAILED(m_impl->compositionDevice->CreateVisual(&m_impl->compositionRoot))
        || FAILED(m_impl->compositionDevice->CreateVisual(&m_impl->compositionVideo))
        || FAILED(m_impl->compositionDevice->CreateVisual(&m_impl->compositionOverlay))
        || FAILED(m_impl->compositionRoot->AddVisual(m_impl->compositionVideo.Get(), FALSE, nullptr))
        || FAILED(m_impl->compositionRoot->AddVisual(m_impl->compositionOverlay.Get(), TRUE, nullptr))
        || FAILED(m_impl->compositionTarget->SetRoot(m_impl->compositionRoot.Get()))) {
        resetCompositionResources();
        return false;
    }
    if (!commitCompositionVisual()) {
        resetCompositionResources();
        return false;
    }
    return true; // 首次创建后立即提交根视觉树，避免SwapChain已Present但顶层目标尚未显示。
}

bool D3D11FramePresenter::ensureCompositionOverlaySurface(int width, int height)
{
    if (!m_impl || !m_impl->compositorMode || !m_impl->compositionDevice
        || !m_impl->compositionOverlay || width <= 0 || height <= 0) {
        return false;
    }
    if (m_impl->compositionOverlaySwapChain
        && m_impl->compositionOverlayWidth == width
        && m_impl->compositionOverlayHeight == height) {
        m_impl->compositionOverlayCandidateSwapChain.Reset();
        m_impl->compositionOverlayCandidateWidth = 0;
        m_impl->compositionOverlayCandidateHeight = 0;
        return true;
    }
    if (m_impl->compositionOverlayCandidateSwapChain
        && m_impl->compositionOverlayCandidateWidth == width
        && m_impl->compositionOverlayCandidateHeight == height) {
        return true;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory2;
    if (FAILED(m_impl->sharedDevice->device.As(&dxgiDevice))
        || FAILED(dxgiDevice->GetAdapter(&adapter))
        || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory2)))) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    ComPtr<IDXGISwapChain1> overlaySwapChain;
    const HRESULT createResult = factory2->CreateSwapChainForComposition(
        m_impl->sharedDevice->device.Get(),
        &desc,
        nullptr,
        &overlaySwapChain);
    ComPtr<IDXGISwapChain> candidateSwapChain;
    if (FAILED(createResult)
        || FAILED(overlaySwapChain.As(&candidateSwapChain))) {
        return false;
    }
    m_impl->compositionOverlayCandidateSwapChain = std::move(candidateSwapChain);
    m_impl->compositionOverlayCandidateWidth = width;
    m_impl->compositionOverlayCandidateHeight = height; // 候选表面尚未进入视觉树，失败时旧叠加层仍完整可见。
    return true;
}

bool D3D11FramePresenter::presentCompositorOverlay(
    const QImage& image,
    const QRect& dirtyPhysicalRect)
{
    if (!m_impl || !m_impl->compositorMode || image.isNull()) {
        return false;
    }
    m_impl->lastCompositionOverlayPresentResult = E_PENDING; // wjy: 在本次Overlay尚未真正Present前标记待定，避免时间线误读上一次成功HRESULT。
    const int requestedTargetWidth = image.width();
    const int requestedTargetHeight = image.height();
    const bool overlayTargetSizeChanged
        = m_impl->compositionOverlayTargetWidth != requestedTargetWidth
        || m_impl->compositionOverlayTargetHeight != requestedTargetHeight;
    if (!ensureDevice() || !ensureCompositionTarget()) {
        return false;
    }
    QImage converted = image.format() == QImage::Format_ARGB32_Premultiplied
        ? image
        : image.convertToFormat(QImage::Format_ARGB32_Premultiplied); // wjy: 缓存Overlay已经是目标格式时复用隐式共享像素，标题栏秒级更新不再复制整窗QImage。
    if (converted.isNull()) {
        return false;
    }
    bool reuseCurrentDuringResize = false;
    if (m_impl->interactiveResize
        && m_impl->compositionOverlaySwapChain
        && m_impl->compositionOverlayWidth > 0
        && m_impl->compositionOverlayHeight > 0
        && (converted.width() != m_impl->compositionOverlayWidth
            || converted.height() != m_impl->compositionOverlayHeight)) {
        converted = converted.scaled(
            QSize(m_impl->compositionOverlayWidth, m_impl->compositionOverlayHeight),
            Qt::IgnoreAspectRatio,
            Qt::FastTransformation); // wjy: 缩放手势内只更新Alpha内容，不重建SwapChain资源。
        reuseCurrentDuringResize = true;
    } else if (!ensureCompositionOverlaySurface(converted.width(), converted.height())) {
        return false;
    }

    const bool useCandidate = !reuseCurrentDuringResize
        && m_impl->compositionOverlayCandidateSwapChain
        && m_impl->compositionOverlayCandidateWidth == converted.width()
        && m_impl->compositionOverlayCandidateHeight == converted.height();
    IDXGISwapChain* targetSwapChain = useCandidate
        ? m_impl->compositionOverlayCandidateSwapChain.Get()
        : m_impl->compositionOverlaySwapChain.Get();

    const QRect surfaceBounds(0, 0, converted.width(), converted.height());
    const QRect requestedDirtyRect = dirtyPhysicalRect.intersected(surfaceBounds);
    const bool requestedPartialUpdate = !requestedDirtyRect.isEmpty()
        && requestedDirtyRect != surfaceBounds;
    ComPtr<IDXGISwapChain1> targetSwapChain1;
    bool partialPresent = !useCandidate
        && !reuseCurrentDuringResize
        && !m_impl->interactiveResize
        && m_impl->compositionOverlayFullPresentsRemaining == 0
        && requestedPartialUpdate
        && targetSwapChain
        && SUCCEEDED(targetSwapChain->QueryInterface(IID_PPV_ARGS(&targetSwapChain1))); // wjy: 只有稳定且两个BackBuffer都完整初始化后才允许局部Present。

    // =====wjy====
    m_impl->lastCompositionOverlayMode = partialPresent ? QStringLiteral("partial") : QStringLiteral("full"); // wjy: 保存实际呈现模式，而不是仅根据调用方传入的脏区猜测。
    m_impl->lastCompositionOverlayDirtyRect = requestedDirtyRect; // wjy: 记录经过表面边界裁剪后的真实物理脏区。
    m_impl->lastCompositionOverlayBackBufferIndex = -1; // wjy: 每次Present前重新查询，查询失败时保留明确的未知值。
    // ===end====
    ComPtr<ID3D11Texture2D> backBuffer;
    if (!targetSwapChain
        || FAILED(targetSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC backBufferDesc = {};
    backBuffer->GetDesc(&backBufferDesc);
    if (backBufferDesc.Width != static_cast<UINT>(converted.width())
        || backBufferDesc.Height != static_cast<UINT>(converted.height())) {
        return false;
    }
    D3D11_BOX uploadBox = {};
    const D3D11_BOX* uploadBoxPointer = nullptr;
    const uchar* sourceBits = converted.constBits();
    std::uint64_t uploadedBytes = static_cast<std::uint64_t>(converted.width())
        * static_cast<std::uint64_t>(converted.height()) * 4;
    if (partialPresent) {
        uploadBox.left = static_cast<UINT>(requestedDirtyRect.left());
        uploadBox.top = static_cast<UINT>(requestedDirtyRect.top());
        uploadBox.front = 0;
        uploadBox.right = static_cast<UINT>(requestedDirtyRect.right() + 1);
        uploadBox.bottom = static_cast<UINT>(requestedDirtyRect.bottom() + 1);
        uploadBox.back = 1;
        uploadBoxPointer = &uploadBox;
        sourceBits += requestedDirtyRect.top() * converted.bytesPerLine()
            + requestedDirtyRect.left() * 4;
        uploadedBytes = static_cast<std::uint64_t>(requestedDirtyRect.width())
            * static_cast<std::uint64_t>(requestedDirtyRect.height()) * 4; // wjy: 正常9窗口每秒只复制约42像素高标题栏，不再触碰透明视频孔和黑边。
    }
    m_impl->sharedDevice->context->UpdateSubresource(
        backBuffer.Get(),
        0,
        uploadBoxPointer,
        sourceBits,
        static_cast<UINT>(converted.bytesPerLine()),
        0);

    HRESULT presentResult = S_OK;
    // =====wjy====
    ++m_impl->compositionOverlayPresentSequence; // wjy: 只在即将调用DXGI Present时递增，避免资源准备失败伪装成可见提交。
    ComPtr<IDXGISwapChain3> targetSwapChain3;
    if (targetSwapChain && SUCCEEDED(targetSwapChain->QueryInterface(IID_PPV_ARGS(&targetSwapChain3)))) {
        m_impl->lastCompositionOverlayBackBufferIndex = static_cast<int>(targetSwapChain3->GetCurrentBackBufferIndex()); // wjy: 记录双缓冲当前索引，定位黑闪是否落在未补齐的BackBuffer。
    }
    // ===end====
    if (partialPresent) {
        RECT dirtyRect = {
            requestedDirtyRect.left(),
            requestedDirtyRect.top(),
            requestedDirtyRect.right() + 1,
            requestedDirtyRect.bottom() + 1};
        DXGI_PRESENT_PARAMETERS parameters = {};
        parameters.DirtyRectsCount = 1;
        parameters.pDirtyRects = &dirtyRect;
        presentResult = targetSwapChain1->Present1(0, 0, &parameters);
    } else {
        presentResult = targetSwapChain->Present(0, 0);
    }
    m_impl->lastCompositionOverlayPresentResult = presentResult; // wjy: 成功与失败都写入最近一次HRESULT，供data时间线和现有汇总共同读取。
    if (FAILED(presentResult)) {
        ++m_impl->compositionOverlayPresentFailureCount;
        if (useCandidate) {
            m_impl->compositionOverlayCandidateSwapChain.Reset();
            m_impl->compositionOverlayCandidateWidth = 0;
            m_impl->compositionOverlayCandidateHeight = 0;
        }
        handleDeviceFailure(presentResult);
        return false;
    }
    m_impl->compositionOverlayUploadedBytes += uploadedBytes;
    if (partialPresent) {
        ++m_impl->compositionOverlayPartialPresentCount;
    } else {
        ++m_impl->compositionOverlayFullPresentCount;
        if (!useCandidate && requestedPartialUpdate
            && m_impl->compositionOverlayFullPresentsRemaining > 0) {
            --m_impl->compositionOverlayFullPresentsRemaining; // wjy: 标题栏请求被迫完整写入第二个BackBuffer后，两份静态内容才重新一致。
        } else if (!useCandidate && !requestedPartialUpdate) {
            m_impl->compositionOverlayFullPresentsRemaining = 1; // wjy: 连接遮罩、软件帧或几何完整变化只写了当前BackBuffer，另一份必须在开放局部更新前补齐。
        }
    }

    const int previousTargetWidth = m_impl->compositionOverlayTargetWidth;
    const int previousTargetHeight = m_impl->compositionOverlayTargetHeight;
    m_impl->compositionOverlayTargetWidth = requestedTargetWidth;
    m_impl->compositionOverlayTargetHeight = requestedTargetHeight;
    if (useCandidate) {
        const ComPtr<IDXGISwapChain> previousSwapChain = m_impl->compositionOverlaySwapChain;
        const int previousOverlayWidth = m_impl->compositionOverlayWidth;
        const int previousOverlayHeight = m_impl->compositionOverlayHeight;
        const int previousFullPresentsRemaining = m_impl->compositionOverlayFullPresentsRemaining;
        m_impl->compositionOverlayWidth = m_impl->compositionOverlayCandidateWidth;
        m_impl->compositionOverlayHeight = m_impl->compositionOverlayCandidateHeight;
        if (FAILED(m_impl->compositionOverlay->SetContent(targetSwapChain))
            || !commitCompositionVisual()) {
            m_impl->compositionOverlayTargetWidth = previousTargetWidth;
            m_impl->compositionOverlayTargetHeight = previousTargetHeight;
            m_impl->compositionOverlayWidth = previousOverlayWidth;
            m_impl->compositionOverlayHeight = previousOverlayHeight;
            m_impl->compositionOverlayFullPresentsRemaining = previousFullPresentsRemaining;
            if (m_impl->compositionOverlay && m_impl->compositionDevice) {
                m_impl->compositionOverlay->SetContent(previousSwapChain.Get());
                commitCompositionVisual();
            }
            m_impl->compositionOverlayCandidateSwapChain.Reset();
            m_impl->compositionOverlayCandidateWidth = 0;
            m_impl->compositionOverlayCandidateHeight = 0;
            return false;
        }
        m_impl->compositionOverlaySwapChain = m_impl->compositionOverlayCandidateSwapChain;
        m_impl->compositionOverlayCandidateSwapChain.Reset();
        m_impl->compositionOverlayCandidateWidth = 0;
        m_impl->compositionOverlayCandidateHeight = 0;
        m_impl->compositionOverlayFullPresentsRemaining = 1; // wjy: 候选表面首个BackBuffer已经完整Present，下一次再完整写入另一个BackBuffer后开放局部更新。
    } else if (overlayTargetSizeChanged && !commitCompositionVisual()) {
        m_impl->compositionOverlayTargetWidth = previousTargetWidth;
        m_impl->compositionOverlayTargetHeight = previousTargetHeight;
        commitCompositionVisual();
        return false;
    }
    return true;
}

void D3D11FramePresenter::resetCompositionResources()
{
    if (m_impl && m_impl->compositionTarget) {
        m_impl->compositionTarget->SetRoot(nullptr);
    }
    if (m_impl && m_impl->compositionDevice) {
        m_impl->compositionDevice->Commit();
    }
    if (m_impl) {
        m_impl->compositionOverlaySwapChain.Reset();
        m_impl->compositionOverlayCandidateSwapChain.Reset();
        m_impl->compositionOverlayWidth = 0;
        m_impl->compositionOverlayHeight = 0;
        m_impl->compositionOverlayCandidateWidth = 0;
        m_impl->compositionOverlayCandidateHeight = 0;
        m_impl->compositionOverlayTargetWidth = 0;
        m_impl->compositionOverlayTargetHeight = 0;
        m_impl->compositionOverlayFullPresentsRemaining = 0;
        m_impl->compositionOverlay.Reset();
        m_impl->compositionVideo.Reset();
        m_impl->compositionRoot.Reset();
        m_impl->compositionTarget.Reset();
        m_impl->compositionDevice.Reset();
    }
}

bool D3D11FramePresenter::commitCompositionVisual()
{
    if (!m_impl || !m_impl->compositorMode || !m_impl->compositionDevice
        || !m_impl->compositionVideo || !m_impl->compositionOverlay) {
        return false;
    }
    const QRect& outputRect = m_impl->compositorOutputRect;
    const bool hasOutputGeometry = outputRect.width() > 0 && outputRect.height() > 0;
    const bool hasSwapGeometry = m_impl->swapWidth > 0 && m_impl->swapHeight > 0;
    const int sourceWidth = m_impl->currentTextureWidth > 0 && m_impl->currentTextureHeight > 0
        ? m_impl->currentTextureWidth
        : m_impl->swapWidth;
    const int sourceHeight = m_impl->currentTextureWidth > 0 && m_impl->currentTextureHeight > 0
        ? m_impl->currentTextureHeight
        : m_impl->swapHeight;
    const RECT sourceClipRect = letterbox_rect(
        sourceWidth,
        sourceHeight,
        m_impl->swapWidth,
        m_impl->swapHeight); // wjy: 计算旧SwapChain中真正的视频像素区域，裁掉已经写入BackBuffer的旧黑边。
    const RECT targetImageRect = letterbox_rect(
        sourceWidth,
        sourceHeight,
        outputRect.width(),
        outputRect.height()); // wjy: 按当前窗口尺寸重新计算目标视频区域，黑边不再继承旧SwapChain的尺寸。
    const float sourceClipWidth = static_cast<float>(sourceClipRect.right - sourceClipRect.left);
    const float sourceClipHeight = static_cast<float>(sourceClipRect.bottom - sourceClipRect.top);
    const float targetImageWidth = static_cast<float>(targetImageRect.right - targetImageRect.left);
    const float targetImageHeight = static_cast<float>(targetImageRect.bottom - targetImageRect.top);
    float uniformScale = 1.0f;
    if (hasOutputGeometry && hasSwapGeometry
        && sourceClipWidth > 0.0f && sourceClipHeight > 0.0f
        && targetImageWidth > 0.0f && targetImageHeight > 0.0f) {
        const float scaleX = targetImageWidth / sourceClipWidth;
        const float scaleY = targetImageHeight / sourceClipHeight;
        uniformScale = scaleX < scaleY ? scaleX : scaleY; // wjy: 视频内容只按实际图像区域等比缩放，避免黑边参与比例计算。
    }
    const float scaledVideoWidth = sourceClipWidth * uniformScale;
    const float scaledVideoHeight = sourceClipHeight * uniformScale;
    const float centeredOffsetX = static_cast<float>(outputRect.left() + targetImageRect.left)
        + (targetImageWidth - scaledVideoWidth) * 0.5f
        - static_cast<float>(sourceClipRect.left) * uniformScale; // wjy: 把旧BackBuffer中的视频区域对齐到当前窗口的等比画面区域。
    const float centeredOffsetY = static_cast<float>(outputRect.top() + targetImageRect.top)
        + (targetImageHeight - scaledVideoHeight) * 0.5f
        - static_cast<float>(sourceClipRect.top) * uniformScale; // wjy: 垂直方向使用同一套映射，拖拽时黑边不会随旧缓冲一起移动。
    const D2D_RECT_F videoClip = {
        static_cast<float>(sourceClipRect.left),
        static_cast<float>(sourceClipRect.top),
        static_cast<float>(sourceClipRect.right),
        static_cast<float>(sourceClipRect.bottom)};
    HRESULT visualResult = m_impl->compositionVideo->SetClip(videoClip);
    if (SUCCEEDED(visualResult)) visualResult = m_impl->compositionVideo->SetOffsetX(centeredOffsetX);
    if (SUCCEEDED(visualResult)) visualResult = m_impl->compositionVideo->SetOffsetY(centeredOffsetY);
    D2D_MATRIX_3X2_F transform = {};
    transform._11 = uniformScale; // wjy: X轴和Y轴共用同一个比例，窗口变窄或变高时不再把远端内容压扁或拉长。
    transform._22 = uniformScale; // wjy: 与X轴保持一致，缩放期间只改变整体尺寸，不改变远端像素的几何比例。
    if (SUCCEEDED(visualResult)) visualResult = m_impl->compositionVideo->SetTransform(transform);
    if (SUCCEEDED(visualResult)) {
        visualResult = m_impl->compositionVideo->SetContent(
            m_impl->compositorVisible ? m_impl->swapChain.Get() : nullptr); // wjy: IDCompositionVisual没有SetOpacity，使用内容绑定/解绑实现原子显示与隐藏，避免引入Visual3运行时依赖。
    }
    D2D_MATRIX_3X2_F overlayTransform = {};
    overlayTransform._11 = 1.0f;
    overlayTransform._22 = 1.0f;
    if (m_impl->compositionOverlayWidth > 0 && m_impl->compositionOverlayHeight > 0
        && m_impl->compositionOverlayTargetWidth > 0
        && m_impl->compositionOverlayTargetHeight > 0) {
        overlayTransform._11 = static_cast<float>(m_impl->compositionOverlayTargetWidth)
            / static_cast<float>(m_impl->compositionOverlayWidth);
        overlayTransform._22 = static_cast<float>(m_impl->compositionOverlayTargetHeight)
            / static_cast<float>(m_impl->compositionOverlayHeight); // wjy: 源与目标都使用QImage真实物理像素，避免GetClientRect在不同DPI感知模式下返回逻辑或物理坐标造成半屏。
    }
    if (SUCCEEDED(visualResult)) visualResult = m_impl->compositionOverlay->SetOffsetX(0.0f);
    if (SUCCEEDED(visualResult)) visualResult = m_impl->compositionOverlay->SetOffsetY(0.0f);
    if (SUCCEEDED(visualResult)) visualResult = m_impl->compositionOverlay->SetTransform(overlayTransform);
    if (FAILED(visualResult)) {
        handleDeviceFailure(visualResult);
        return false;
    }
    const auto commitStarted = std::chrono::steady_clock::now();
    const HRESULT commitResult = m_impl->compositionDevice->Commit();
    // =====wjy====
    const std::uint64_t commitTimeUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - commitStarted).count()); // wjy: 单次计算同时服务累计平均值和最近一次时间线，避免重复读取时钟。
    m_impl->compositionCommitTimeUs += commitTimeUs;
    m_impl->lastCompositionCommitTimeUs = commitTimeUs; // wjy: 保存最近一次Commit耗时，黑闪现场可检查是否出现突发阻塞。
    m_impl->lastCompositionCommitResult = commitResult; // wjy: Commit成功和失败都进入时间线，不再只依赖累计失败计数。
    // ===end====
    ++m_impl->compositionCommitCount;
    if (FAILED(commitResult)) {
        ++m_impl->compositionCommitFailureCount;
        handleDeviceFailure(commitResult);
        return false;
    }
    return true; // 位置与SwapChain内容在同一个Commit中原子提交，避免视觉树只更新一半。
}

D3D11FramePresenter::~D3D11FramePresenter()
{
    reset();
    delete m_impl;
    m_impl = nullptr;
}

bool D3D11FramePresenter::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#if defined(Q_OS_WIN)
    Q_UNUSED(eventType)
    const auto* nativeMessage = static_cast<const MSG*>(message);
    if (m_impl && nativeMessage) {
        quint64* counter = nullptr;
        const char* messageName = nullptr;
        switch (nativeMessage->message) {
        case WM_WINDOWPOSCHANGING:
            counter = &m_impl->nativeWindowPosChangingCount;
            messageName = "WM_WINDOWPOSCHANGING";
            break;
        case WM_WINDOWPOSCHANGED:
            counter = &m_impl->nativeWindowPosChangedCount;
            messageName = "WM_WINDOWPOSCHANGED";
            break;
        case WM_SIZE:
            counter = &m_impl->nativeSizeCount;
            messageName = "WM_SIZE";
            break;
        case WM_PAINT:
            counter = &m_impl->nativePaintCount;
            messageName = "WM_PAINT";
            break;
        case WM_ERASEBKGND:
            counter = &m_impl->nativeEraseBackgroundCount;
            messageName = "WM_ERASEBKGND";
            break;
        case WM_SHOWWINDOW:
            counter = &m_impl->nativeShowWindowCount;
            messageName = "WM_SHOWWINDOW";
            break;
        default:
            break;
        }
        if (counter && messageName) {
            ++m_impl->nativeEventCount;
            ++(*counter);
            m_impl->nativeLastMessage = QString::fromLatin1(messageName);
        }

        if (nativeMessage->message == WM_WINDOWPOSCHANGING
            && m_impl->interactiveResize
            && nativeMessage->lParam) {
            auto* windowPos = reinterpret_cast<WINDOWPOS*>(nativeMessage->lParam);
            if (windowPos && !(windowPos->flags & SWP_NOREDRAW)) {
                windowPos->flags |= SWP_NOREDRAW; // wjy: 保留Qt对geometry和resizeEvent的完整跟踪，同时禁止Windows在原生尺寸提交阶段重绘D3D子表面。
                ++m_impl->nativeNoRedrawWindowPosCount; // wjy: 记录本次WINDOWPOS确实注入了无重绘标志，便于和闪烁测试结果对照。
            }
        }

        if (nativeMessage->message == WM_ERASEBKGND) {
            ++m_impl->nativeEraseHandledCount;
            if (result) *result = 1; // wjy: D3D SwapChain负责内容表面，禁止Windows先擦除原生子窗口背景造成黑帧。
            return true; // wjy: 不再把WM_ERASEBKGND交给Qt/DefWindowProc，避免每次resize先清空子HWND。
        }
        if (nativeMessage->message == WM_PAINT) {
            ++m_impl->nativePaintHandledCount;
            PAINTSTRUCT paintStruct = {};
            if (nativeMessage->hwnd) {
                HDC deviceContext = ::BeginPaint(nativeMessage->hwnd, &paintStruct);
                if (deviceContext) {
                    ::EndPaint(nativeMessage->hwnd, &paintStruct); // wjy: 只验证并清除无效区域，不绘制任何背景像素。
                }
            }
            if (result) *result = 0;
            return true; // wjy: 原生D3D内容由Present提交，禁止Qt默认WM_PAINT路径再次擦除/重绘子窗口。
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

bool D3D11FramePresenter::ensureDevice()
{
    if (m_impl->sharedDevice
        && !m_impl->sharedDevice->invalidated.load(std::memory_order_acquire)) {
        return true;
    }

    releaseFrameResources();
    m_impl->processor.Reset();
    m_impl->processorEnum.Reset();
    resetCompositionResources(); // wjy: 设备代际变化时视觉树必须先解除旧Device上的SwapChain内容。
    m_impl->swapChain.Reset(); // wjy: SwapChain与创建它的Device绑定，设备代际变化时当前窗口必须单独重建。
    m_impl->sharedDevice = acquirePresentationDevice();
    m_impl->swapWidth = 0;
    m_impl->swapHeight = 0;
    return static_cast<bool>(m_impl->sharedDevice);
}

bool D3D11FramePresenter::ensureSwapChain()
{
    if (!ensureDevice()) {
        return false;
    }

    // =====wjy====
    // wjy: D3D的backbuffer和client area一律使用物理像素；Qt的width()/height()是逻辑像素。
    // 高DPI下混用会让SetSourceSize的源区域与真实client area不等，画面被放大且letterbox溢出裁切。
    int outputWidth = qMax(1, width());
    int outputHeight = qMax(1, height());
    RECT clientRect = {};
    const HWND outputWindow = m_impl->compositorMode
        ? m_impl->compositorHostWindow
        : reinterpret_cast<HWND>(winId());
    if (outputWindow && ::GetClientRect(outputWindow, &clientRect)) {
        if (m_impl->compositorMode && m_impl->compositorOutputRect.isValid()) {
            outputWidth = qMax(1, m_impl->compositorOutputRect.width());
            outputHeight = qMax(1, m_impl->compositorOutputRect.height());
        } else {
            outputWidth = qMax(1, static_cast<int>(clientRect.right - clientRect.left));
            outputHeight = qMax(1, static_cast<int>(clientRect.bottom - clientRect.top)); // wjy: 直接取Windows权威物理客户区，不用DPR二次推算。
        }
    }
    // ===end====
    if (m_impl->swapChain && m_impl->swapWidth == outputWidth && m_impl->swapHeight == outputHeight) {
        if (!m_impl->interactiveResize) {
            m_impl->resizePending = false; // wjy: 仅在非拖拽状态确认缓冲已精确匹配；手势期间保留最终收尾标记，避免瞬时尺寸读取把它提前清掉。
        }
        return true;
    }

    // =====wjy====
    // wjy: 拖拽期间子HWND仍实时跟随父窗口，但禁止按每个像素尺寸销毁并重建BackBuffer。
    // 新网络帧继续按旧BackBuffer尺寸Blt/Present，再由DWM缩放到当前子窗口，避免ResizeBuffers与Present之间露出空白。
    if (m_impl->swapChain && m_impl->interactiveResize) {
        m_impl->resizePending = true; // wjy: 只登记最终需要精确调整，当前调用继续复用有效SwapChain呈现最新帧。
        return true;
    }

    m_impl->processor.Reset();
    m_impl->processorEnum.Reset();
    if (m_impl->swapChain) {
        const HRESULT hr = m_impl->swapChain->ResizeBuffers(
            0, static_cast<UINT>(outputWidth), static_cast<UINT>(outputHeight), DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (FAILED(hr)) {
            return handleDeviceFailure(hr); // wjy: ResizeBuffers失败时先保留旧SwapChain前台画面；仅真实Device Removed才释放并换代。
        }
    }

    if (!m_impl->swapChain) {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        if (FAILED(m_impl->sharedDevice->device.As(&dxgiDevice)) ||
            FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            return false;
        }

        if (m_impl->compositorMode && !ensureCompositionTarget()) {
            return false; // 发布运行不再切换到旧子HWND视频路径，上层在同一窗口进入软件保活。
        }
        if (m_impl->compositorMode) {
            ComPtr<IDXGIFactory2> factory2;
            if (FAILED(factory.As(&factory2))) {
                return false;
            }
            DXGI_SWAP_CHAIN_DESC1 desc = {};
            desc.Width = static_cast<UINT>(outputWidth);
            desc.Height = static_cast<UINT>(outputHeight);
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            ComPtr<IDXGISwapChain1> compositionSwapChain;
            const HRESULT createResult = factory2->CreateSwapChainForComposition(
                m_impl->sharedDevice->device.Get(),
                &desc,
                nullptr,
                &compositionSwapChain);
            if (FAILED(createResult) || FAILED(compositionSwapChain.As(&m_impl->swapChain))) {
                return handleDeviceFailure(FAILED(createResult) ? createResult : E_FAIL);
            }
        } else {
            DXGI_SWAP_CHAIN_DESC desc = {};
            desc.BufferDesc.Width = static_cast<UINT>(outputWidth);
            desc.BufferDesc.Height = static_cast<UINT>(outputHeight);
            desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.BufferDesc.RefreshRate.Numerator = 60;
            desc.BufferDesc.RefreshRate.Denominator = 1;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.OutputWindow = reinterpret_cast<HWND>(winId());
            desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            const HRESULT createResult = factory->CreateSwapChain(m_impl->sharedDevice->device.Get(), &desc, &m_impl->swapChain);
            if (FAILED(createResult)) {
                return handleDeviceFailure(createResult);
            }
            factory->MakeWindowAssociation(reinterpret_cast<HWND>(winId()), DXGI_MWA_NO_ALT_ENTER);
        }
    }
    // ===end====

    m_impl->swapWidth = outputWidth;
    m_impl->swapHeight = outputHeight;
    m_impl->resizePending = false;
    if (m_impl->compositorMode) {
        if (!commitCompositionVisual()) {
            return false;
        }
    }
    return true;
}

bool D3D11FramePresenter::ensureVideoProcessor(int sourceWidth, int sourceHeight, int outputWidth, int outputHeight)
{
    if (m_impl->processor &&
        m_impl->processorSourceWidth == sourceWidth &&
        m_impl->processorSourceHeight == sourceHeight &&
        m_impl->processorOutputWidth == outputWidth &&
        m_impl->processorOutputHeight == outputHeight) {
        return true;
    }

    m_impl->processor.Reset();
    m_impl->processorEnum.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = static_cast<UINT>(sourceWidth);
    content.InputHeight = static_cast<UINT>(sourceHeight);
    content.OutputWidth = static_cast<UINT>(outputWidth);
    content.OutputHeight = static_cast<UINT>(outputHeight);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    const HRESULT enumResult = m_impl->sharedDevice->videoDevice->CreateVideoProcessorEnumerator(&content, &m_impl->processorEnum);
    if (FAILED(enumResult)) {
        return handleDeviceFailure(enumResult);
    }
    const HRESULT processorResult = m_impl->sharedDevice->videoDevice->CreateVideoProcessor(m_impl->processorEnum.Get(), 0, &m_impl->processor);
    if (FAILED(processorResult)) {
        m_impl->processorEnum.Reset();
        return handleDeviceFailure(processorResult);
    }

    m_impl->processorSourceWidth = sourceWidth;
    m_impl->processorSourceHeight = sourceHeight;
    m_impl->processorOutputWidth = outputWidth;
    m_impl->processorOutputHeight = outputHeight;
    return true;
}

bool D3D11FramePresenter::presentSharedTexture(void* sharedHandle, int frameWidth, int frameHeight)
{
    const bool outputAvailable = m_impl && m_impl->compositorMode
        ? m_impl->compositorHostWindow != nullptr
        : width() > 0 && height() > 0;
    if (!sharedHandle || frameWidth <= 0 || frameHeight <= 0 || !outputAvailable) {
        return false;
    }
    if (!ensureDevice()) {
        return false;
    }

    // =====wjy====
    ComPtr<ID3D11Texture2D> candidateTexture;
    const HRESULT openResult = m_impl->sharedDevice->device->OpenSharedResource(
        static_cast<HANDLE>(sharedHandle), IID_PPV_ARGS(&candidateTexture));
    if (FAILED(openResult) || !candidateTexture) {
        return handleDeviceFailure(FAILED(openResult) ? openResult : E_FAIL); // wjy: 当前成功帧仍被锁定保留，新候选必须独立打开，失败不能破坏旧画面。
    }
    SharedTextureReadGuard textureReadGuard; // wjy: guard覆盖后续全部成功和失败出口，确保每张已接受纹理最终归还生产者key。
    const HRESULT acquireResult = textureReadGuard.acquire(candidateTexture.Get(), m_impl->sharedDevice->context.Get());
    if (acquireResult == WAIT_ABANDONED) {
        appendPresenterSyncDiagnostic("present_abandoned", acquireResult, sharedHandle);
        return handleDeviceFailure(DXGI_ERROR_DEVICE_RESET); // wjy: 放弃的消费者所有权使共享设备代际失效，按设备重置进入现有重建/软件保活路径。
    }
    if (acquireResult != S_OK) {
        appendPresenterSyncDiagnostic("present_timeout_or_error", acquireResult, sharedHandle);
        return handleDeviceFailure(acquireResult);
    }
    // ===end====

    cacheFrameForResize(candidateTexture.Get(), frameWidth, frameHeight); // wjy: 先在仍持有消费者key时复制私有副本，让缩放期间第一张帧就能解除ensureSwapChain的等待并精确调整缓冲。

    if (!ensureSwapChain()) {
        return false; // wjy: 已取得消费者key后再准备SwapChain，失败出口由guard归还纹理，不会锁死生产端槽位。
    }

    if (!blitAndPresent(candidateTexture.Get(), frameWidth, frameHeight)) {
        return false; // wjy: 失败原因已在blitAndPresent内部记录，guard仍会归还消费者key。
    }

    // =====wjy====
    ComPtr<IDXGIKeyedMutex> candidateKeyedMutex = textureReadGuard.retain(); // wjy: Present成功后保留本帧读取权，缩放第一下即可安全复制。
    if (!candidateKeyedMutex) {
        return false;
    }
    releaseFrameResources(); // wjy: 新帧已经完整提交后才归还旧槽位，始终有且只有一张可安全复用的最后帧。
    m_impl->currentTexture = std::move(candidateTexture);
    m_impl->currentKeyedMutex = std::move(candidateKeyedMutex);
    m_impl->currentHandle = sharedHandle;
    m_impl->currentTextureWidth = frameWidth;
    m_impl->currentTextureHeight = frameHeight;
    m_impl->lastDeviceRemovalReason = S_OK;
    m_impl->lastFailureWasDeviceLost = false;
    m_impl->hasPresentedFrame = true;
    // ===end====
    return true;
}

// =====wjy====
bool D3D11FramePresenter::blitAndPresent(ID3D11Texture2D* sourceTexture, int frameWidth, int frameHeight)
{
    if (!sourceTexture || frameWidth <= 0 || frameHeight <= 0) {
        return false;
    }

    const int outputWidth = qMax(1, m_impl->swapWidth);
    const int outputHeight = qMax(1, m_impl->swapHeight); // wjy: 始终按真实BackBuffer坐标绘制；拖拽期间客户区变化由DWM缩放该完整表面，松手后再恢复精确尺寸。
    if (!ensureVideoProcessor(frameWidth, frameHeight, outputWidth, outputHeight)) {
        return false;
    }

    ComPtr<ID3D11VideoProcessorInputView> inputView;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.MipSlice = 0;
    const HRESULT inputResult = m_impl->sharedDevice->videoDevice->CreateVideoProcessorInputView(
        sourceTexture, m_impl->processorEnum.Get(), &inputDesc, &inputView);
    if (FAILED(inputResult)) {
        return handleDeviceFailure(inputResult);
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    const HRESULT bufferResult = m_impl->swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(bufferResult)) {
        return handleDeviceFailure(bufferResult);
    }

    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    const HRESULT outputResult = m_impl->sharedDevice->videoDevice->CreateVideoProcessorOutputView(
        backBuffer.Get(), m_impl->processorEnum.Get(), &outputDesc, &outputView);
    if (FAILED(outputResult)) {
        return handleDeviceFailure(outputResult);
    }

    RECT sourceRect = {0, 0, frameWidth, frameHeight};
    RECT outputRect = {0, 0, outputWidth, outputHeight};
    const RECT streamDestRect = letterbox_rect(
        frameWidth,
        frameHeight,
        outputWidth,
        outputHeight); // wjy: 真实视频目标矩形保持宽高比，完整输出矩形继续覆盖标题栏下的全部内容区。
    D3D11_VIDEO_COLOR backgroundColor = {};
    backgroundColor.RGBA.A = 1.0f;
    m_impl->sharedDevice->videoContext->VideoProcessorSetOutputBackgroundColor(
        m_impl->processor.Get(),
        FALSE,
        &backgroundColor); // wjy: 每次Blt都明确把视频外区域写成纯黑，Resize或backbuffer复用不会留下旧尺寸残影。
    m_impl->sharedDevice->videoContext->VideoProcessorSetStreamFrameFormat(m_impl->processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_impl->sharedDevice->videoContext->VideoProcessorSetStreamSourceRect(m_impl->processor.Get(), 0, TRUE, &sourceRect);
    m_impl->sharedDevice->videoContext->VideoProcessorSetStreamDestRect(m_impl->processor.Get(), 0, TRUE, &streamDestRect);
    m_impl->sharedDevice->videoContext->VideoProcessorSetOutputTargetRect(m_impl->processor.Get(), TRUE, &outputRect);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    const HRESULT bltResult = m_impl->sharedDevice->videoContext->VideoProcessorBlt(
        m_impl->processor.Get(), outputView.Get(), 0, 1, &stream);
    if (FAILED(bltResult)) {
        return handleDeviceFailure(bltResult);
    }
    const HRESULT presentResult = m_impl->swapChain->Present(0, 0);
    m_impl->lastPresentResult = presentResult; // wjy: 无论成功失败都保留HRESULT，父窗口诊断可区分空档与呈现错误。
    if (FAILED(presentResult)) {
        return handleDeviceFailure(presentResult); // wjy: Present失败不抛异常；当前窗口转软件回退并由定时器稍后重试共享设备。
    }
    // wjy: 视频SwapChain的Present会自行通知DirectComposition消费新缓冲；视觉属性未变化时禁止额外Commit。
    if (m_impl->interactiveResize) {
        ++m_impl->interactivePresentCount; // wjy: 只统计缩放期间真正完成的网络帧Present，区分“无新帧”和“新帧提交后仍闪烁”。
    }
    return true;
}

void D3D11FramePresenter::cacheFrameForResize(ID3D11Texture2D* sourceTexture, int frameWidth, int frameHeight)
{
    if (!m_impl->interactiveResize || !sourceTexture || frameWidth <= 0 || frameHeight <= 0) {
        return; // wjy: 非缩放状态不保留私有副本，20路常态呈现不增加任何GPU拷贝和显存占用。
    }

    if (!m_impl->resizeCacheTexture
        || m_impl->resizeCacheWidth != frameWidth
        || m_impl->resizeCacheHeight != frameHeight) {
        m_impl->resizeCacheTexture.Reset();
        D3D11_TEXTURE2D_DESC sourceDesc = {};
        sourceTexture->GetDesc(&sourceDesc);
        D3D11_TEXTURE2D_DESC cacheDesc = sourceDesc;
        cacheDesc.Usage = D3D11_USAGE_DEFAULT;
        cacheDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cacheDesc.CPUAccessFlags = 0;
        cacheDesc.MiscFlags = 0; // wjy: 私有副本不共享也不加keyed mutex，缩放期间可随时读取而无需与解码端同步。
        if (FAILED(m_impl->sharedDevice->device->CreateTexture2D(&cacheDesc, nullptr, &m_impl->resizeCacheTexture))) {
            m_impl->resizeCacheTexture.Reset();
            return; // wjy: 缓存分配失败只失去缩放期重呈现优化，正常远端帧路径不受影响。
        }
        m_impl->resizeCacheWidth = frameWidth;
        m_impl->resizeCacheHeight = frameHeight;
    }

    m_impl->sharedDevice->context->CopyResource(m_impl->resizeCacheTexture.Get(), sourceTexture); // wjy: 调用方仍持有消费者key，此处复制不会与生产端写入竞争。
}

bool D3D11FramePresenter::presentCachedFrameForResize()
{
    if (!m_impl
        || !m_impl->interactiveResize
        || !m_impl->resizeCacheTexture
        || !m_impl->swapChain
        || !m_impl->sharedDevice
        || m_impl->sharedDevice->invalidated.load(std::memory_order_acquire)) {
        return false; // wjy: 缓存、现有SwapChain或共享设备任一不可用时保留当前前台画面，等待下一张正常网络帧恢复。
    }

    ++m_impl->resizePresentCount; // wjy: 只对尺寸手势缓存补帧计数，不把正常网络帧混入缩放诊断。
    return blitAndPresent(
        m_impl->resizeCacheTexture.Get(),
        m_impl->resizeCacheWidth,
        m_impl->resizeCacheHeight); // wjy: 只向手势开始时的稳定BackBuffer重画并Present，由DWM映射到当前子HWND，整个调用绝不调整缓冲尺寸。
}

void D3D11FramePresenter::discardSharedTexture(void* sharedHandle)
{
    if (!m_impl || !sharedHandle || !ensureDevice()) {
        return;
    }

    ComPtr<ID3D11Texture2D> texture;
    const HRESULT openResult = m_impl->sharedDevice->device->OpenSharedResource(
        static_cast<HANDLE>(sharedHandle), IID_PPV_ARGS(&texture));
    if (FAILED(openResult) || !texture) {
        handleDeviceFailure(FAILED(openResult) ? openResult : E_FAIL);
        return; // wjy: 待取消帧不可能等于仍由Presenter锁定的前台槽，始终按自己的共享句柄完成交接。
    }

    SharedTextureReadGuard textureReadGuard;
    const HRESULT acquireResult = textureReadGuard.acquire(texture.Get(), m_impl->sharedDevice->context.Get());
    if (acquireResult == WAIT_ABANDONED) {
        appendPresenterSyncDiagnostic("discard_abandoned", acquireResult, sharedHandle);
        handleDeviceFailure(DXGI_ERROR_DEVICE_RESET); // wjy: 取消路径同样不能把abandoned当成功，否则会释放一个从未可靠取得的key。
    } else if (acquireResult != S_OK) {
        appendPresenterSyncDiagnostic("discard_timeout_or_error", acquireResult, sharedHandle);
        handleDeviceFailure(acquireResult); // wjy: 超时不释放未获得的key；连接停止或解码器销毁负责最终回收该资源组。
    }
}
// ===end====

// =====wjy====
void D3D11FramePresenter::setInteractiveResize(bool active)
{
    if (!m_impl || m_impl->interactiveResize == active) {
        return;
    }

    m_impl->interactiveResize = active;
    if (active) {
        m_impl->resizeEventCount = 0; // wjy: 每次新手势从零开始统计子HWND尺寸事件，输出只对应当前一次拖拽。
        m_impl->resizePresentCount = 0; // wjy: 每次新手势从零开始统计缓存补Present，避免历史会话干扰判断。
        m_impl->nativeEventCount = 0; // wjy: 原生消息计数从当前手势重新开始，避免历史窗口创建事件干扰判断。
        m_impl->nativeWindowPosChangingCount = 0; // wjy: 清零本次手势开始前的WM_WINDOWPOSCHANGING计数。
        m_impl->nativeWindowPosChangedCount = 0; // wjy: 清零本次手势开始前的WM_WINDOWPOSCHANGED计数。
        m_impl->nativeSizeCount = 0; // wjy: 清零本次手势开始前的WM_SIZE计数。
        m_impl->nativePaintCount = 0; // wjy: 清零本次手势开始前的WM_PAINT计数。
        m_impl->nativeEraseBackgroundCount = 0; // wjy: 清零本次手势开始前的WM_ERASEBKGND计数。
        m_impl->nativeShowWindowCount = 0; // wjy: 清零本次手势开始前的WM_SHOWWINDOW计数。
        m_impl->nativePaintHandledCount = 0; // wjy: 清零本次手势开始前实际拦截的WM_PAINT计数。
        m_impl->nativeEraseHandledCount = 0; // wjy: 清零本次手势开始前实际拦截的WM_ERASEBKGND计数。
        m_impl->nativeNoRedrawWindowPosCount = 0; // wjy: 清零本次手势开始前注入SWP_NOREDRAW的WINDOWPOS计数。
        m_impl->interactivePresentCount = 0; // wjy: 清零本次手势开始前缩放期间的成功Present计数。
        m_impl->nativeLastMessage = QStringLiteral("none"); // wjy: 新手势从无关键原生消息状态开始记录。
        m_impl->lastPresentResult = S_OK; // wjy: 清除上一次手势的HRESULT，当前日志只报告本次拖拽结果。
        m_impl->resizePending = false; // wjy: 新手势从当前精确BackBuffer尺寸开始，只有真实子窗口变化才登记最终调整。
        cacheFrameForResize(
            m_impl->currentTexture.Get(),
            m_impl->currentTextureWidth,
            m_impl->currentTextureHeight); // wjy: 最后成功帧的消费者key仍在本Presenter手中，按下边缘即可同步复制私有缓存且不会等待解码线程。
        return;
    }

    if (m_impl->resizePending && m_impl->resizeCacheTexture) {
        const int cacheWidth = m_impl->resizeCacheWidth;
        const int cacheHeight = m_impl->resizeCacheHeight;
        ComPtr<ID3D11Texture2D> finalFrame = m_impl->resizeCacheTexture; // wjy: 先取出引用，确保精确ResizeBuffers后仍能补呈现最终尺寸的一帧。
        m_impl->resizeCacheTexture.Reset();
        m_impl->resizeCacheWidth = 0;
        m_impl->resizeCacheHeight = 0;
        if (ensureSwapChain()) {
            blitAndPresent(finalFrame.Get(), cacheWidth, cacheHeight); // wjy: 整个拖拽手势只在这里调整一次最终BackBuffer，并立即补帧覆盖调整后的未定义内容。
        }
    } else if (m_impl->resizeCacheTexture) {
        m_impl->resizeCacheTexture.Reset(); // wjy: 仅按下但未改变尺寸时直接释放缓存，不重复Present同一画面。
        m_impl->resizeCacheWidth = 0;
        m_impl->resizeCacheHeight = 0;
    }
}
// ===end====

bool D3D11FramePresenter::handleDeviceFailure(long result)
{
    HRESULT removalReason = static_cast<HRESULT>(result);
    if (m_impl->sharedDevice && m_impl->sharedDevice->device) {
        const HRESULT queriedReason = m_impl->sharedDevice->device->GetDeviceRemovedReason();
        if (FAILED(queriedReason)) {
            removalReason = queriedReason; // wjy: 优先记录驱动返回的真实Device Removed原因，而不是外层操作的通用失败码。
        }
    }
    m_impl->lastDeviceRemovalReason = removalReason;
    const bool deviceLost = removalReason == DXGI_ERROR_DEVICE_REMOVED
        || removalReason == DXGI_ERROR_DEVICE_RESET
        || removalReason == DXGI_ERROR_DEVICE_HUNG
        || removalReason == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    m_impl->lastFailureWasDeviceLost = deviceLost; // wjy: 普通句柄/视图瞬时失败只丢当前帧，不把可继续显示的SwapChain误判为设备死亡。
    if (deviceLost) {
        invalidatePresentationDevice(m_impl->sharedDevice, removalReason); // wjy: GPU设备级失败通知全部窗口换代，普通资源失败只重建当前Presenter。
        releaseFrameResources(); // wjy: 设备代际已失效时旧纹理不能继续复用，下一次ensureDevice会取得新共享设备。
        m_impl->processor.Reset();
        m_impl->processorEnum.Reset();
        resetCompositionResources(); // wjy: 设备移除后先解除顶层视觉对旧SwapChain的引用，再等待新Device恢复。
        m_impl->swapChain.Reset();
        m_impl->hasPresentedFrame = false;
    }
    return false;
}

long D3D11FramePresenter::lastDeviceRemovalReason() const
{
    return m_impl ? static_cast<long>(m_impl->lastDeviceRemovalReason) : static_cast<long>(S_OK);
}

bool D3D11FramePresenter::hasPresentedFrame() const
{
    return m_impl && m_impl->hasPresentedFrame; // wjy: 该状态只由Qt呈现线程读写，不增加跨线程锁和20窗口额外开销。
}

bool D3D11FramePresenter::lastFailureWasDeviceLost() const
{
    return m_impl && m_impl->lastFailureWasDeviceLost; // wjy: 父窗口据此决定立即设备恢复还是继续沿用最后成功画面。
}

D3D11CompositorTelemetry D3D11FramePresenter::compositorTelemetry() const
{
    D3D11CompositorTelemetry telemetry;
    if (!m_impl) return telemetry;
    telemetry.commitCount = m_impl->compositionCommitCount;
    telemetry.commitFailureCount = m_impl->compositionCommitFailureCount;
    telemetry.averageCommitMs = m_impl->compositionCommitCount > 0
        ? static_cast<double>(m_impl->compositionCommitTimeUs)
            / static_cast<double>(m_impl->compositionCommitCount) / 1000.0
        : 0.0;
    telemetry.overlayFullPresentCount = m_impl->compositionOverlayFullPresentCount;
    telemetry.overlayPartialPresentCount = m_impl->compositionOverlayPartialPresentCount;
    telemetry.overlayPresentFailureCount = m_impl->compositionOverlayPresentFailureCount;
    telemetry.overlayUploadedBytes = m_impl->compositionOverlayUploadedBytes;
    // =====wjy====
    telemetry.overlayPresentSequence = m_impl->compositionOverlayPresentSequence; // wjy: 向窗口级data时间线公开实际Overlay提交序号。
    telemetry.lastOverlayMode = m_impl->lastCompositionOverlayMode; // wjy: 区分完整Present与Present1脏区提交。
    telemetry.lastOverlayDirtyRect = m_impl->lastCompositionOverlayDirtyRect; // wjy: 暴露实际裁剪后的物理脏区。
    telemetry.lastOverlayBackBufferIndex = m_impl->lastCompositionOverlayBackBufferIndex; // wjy: 暴露最近一次双缓冲索引。
    telemetry.lastOverlayPresentResult = static_cast<long>(m_impl->lastCompositionOverlayPresentResult); // wjy: 保留最近一次Overlay HRESULT。
    telemetry.lastCompositionCommitResult = static_cast<long>(m_impl->lastCompositionCommitResult); // wjy: 保留最近一次DComp Commit HRESULT。
    telemetry.lastCompositionCommitTimeUs = m_impl->lastCompositionCommitTimeUs; // wjy: 保留最近一次Commit耗时。
    // ===end====
    return telemetry;
}

QString D3D11FramePresenter::resizeDebugSnapshot() const
{
    if (!m_impl) {
        return QStringLiteral("d3d{presenter=null}");
    }

    int clientWidth = width();
    int clientHeight = height();
#if defined(Q_OS_WIN)
    RECT clientRect = {};
    const HWND debugWindow = m_impl->compositorMode
        ? m_impl->compositorHostWindow
        : reinterpret_cast<HWND>(winId());
    if (debugWindow && ::GetClientRect(debugWindow, &clientRect)) {
        clientWidth = clientRect.right - clientRect.left;
        clientHeight = clientRect.bottom - clientRect.top;
    }
#endif
    return QStringLiteral(
        "d3d{mode=%1 host=0x%2 hwnd=0x%3 visible=%4 geom=%5,%6 %7x%8 client=%9x%10 swap=%11x%12 output=%13,%14 %15x%16 interactive=%17 pending=%18 cache=%19 resize_events=%20 resize_presents=%21 interactive_presents=%22 native_events=%23 native_pos_changing=%24 native_pos_changed=%25 native_size=%26 native_paint=%27 native_erase=%28 native_paint_handled=%29 native_erase_handled=%30 native_no_redraw=%31 native_show=%32 native_last=%33 last_present=0x%34}")
        .arg(m_impl->compositorMode ? QStringLiteral("dcomp") : QStringLiteral("child"))
        .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(m_impl->compositorHostWindow)), 0, 16)
        .arg(static_cast<qulonglong>(winId()), 0, 16) // wjy: Qt 6.11的WId本身就是quintptr整数句柄，直接数值转换即可，不能再次reinterpret_cast为指针。
        .arg(hasVisiblePresentation() ? 1 : 0)
        .arg(geometry().x())
        .arg(geometry().y())
        .arg(geometry().width())
        .arg(geometry().height())
        .arg(clientWidth)
        .arg(clientHeight)
        .arg(m_impl->swapWidth)
        .arg(m_impl->swapHeight)
        .arg(m_impl->compositorOutputRect.x())
        .arg(m_impl->compositorOutputRect.y())
        .arg(m_impl->compositorOutputRect.width())
        .arg(m_impl->compositorOutputRect.height())
        .arg(m_impl->interactiveResize ? 1 : 0)
        .arg(m_impl->resizePending ? 1 : 0)
        .arg(m_impl->resizeCacheTexture ? 1 : 0)
        .arg(m_impl->resizeEventCount)
        .arg(m_impl->resizePresentCount)
        .arg(m_impl->interactivePresentCount)
        .arg(m_impl->nativeEventCount)
        .arg(m_impl->nativeWindowPosChangingCount)
        .arg(m_impl->nativeWindowPosChangedCount)
        .arg(m_impl->nativeSizeCount)
        .arg(m_impl->nativePaintCount)
        .arg(m_impl->nativeEraseBackgroundCount)
        .arg(m_impl->nativePaintHandledCount)
        .arg(m_impl->nativeEraseHandledCount)
        .arg(m_impl->nativeNoRedrawWindowPosCount)
        .arg(m_impl->nativeShowWindowCount)
        .arg(m_impl->nativeLastMessage)
        .arg(static_cast<qulonglong>(static_cast<unsigned long>(m_impl->lastPresentResult)), 0, 16); // wjy: 一行快照同时绑定输出模式、顶层目标、视觉矩形、原生消息和HRESULT。
}

void D3D11FramePresenter::releaseFrameResources()
{
    if (m_impl->currentKeyedMutex) {
        if (m_impl->sharedDevice && m_impl->sharedDevice->context) {
            m_impl->sharedDevice->context->Flush(); // wjy: 确保最后一帧的全部GPU读取结束后再把共享槽归还生产端。
        }
        m_impl->currentKeyedMutex->ReleaseSync(kSharedTextureProducerKey);
        m_impl->currentKeyedMutex.Reset();
    }
    m_impl->currentTexture.Reset();
    m_impl->currentHandle = nullptr;
    m_impl->currentTextureWidth = 0;
    m_impl->currentTextureHeight = 0;
}

void D3D11FramePresenter::setMouseMoveCallback(MouseMoveCallback callback)
{
    m_mouseMoveCallback = std::move(callback);
}

// =====wjy====
// The previous generated block is disabled because its comments were merged into code.
#if 0
void D3D11FramePresenter::setMouseButtonCallbacks(
    MouseButtonCallback pressCallback,
    MouseButtonCallback releaseCallback)
{
    m_mousePressCallback = std::move(pressCallback); // wjy: 保存按下回调，让父窗口继续处理边缘缩放和远端按键按下。
    m_mouseReleaseCallback = std::move(releaseCallback); // wjy: 保存释放回调，保证边缘缩放和远端鼠标按钮成对结束。
}

    m_mousePressCallback = std::move(pressCallback);
    m_mouseReleaseCallback = std::move(releaseCallback);
}
#endif

void D3D11FramePresenter::setMouseButtonCallbacks(
    MouseButtonCallback pressCallback,
    MouseButtonCallback releaseCallback)
{
    m_mousePressCallback = std::move(pressCallback); // wjy: Store the press callback for parent-window hit testing.
    m_mouseReleaseCallback = std::move(releaseCallback); // wjy: Store the release callback for parent-window cleanup.
}
// ===end====

void D3D11FramePresenter::mousePressEvent(QMouseEvent* event)
{
    if (m_mousePressCallback && event) {
        m_mousePressCallback(
            mapToParent(event->pos()),
            event->button(),
            event->buttons(),
            event->modifiers()); // wjy: 子控件覆盖远控画面时，将按下位置转换为父窗口坐标后交回统一命中逻辑。
        event->accept();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void D3D11FramePresenter::mouseMoveEvent(QMouseEvent* event)
{
    if (m_mouseMoveCallback && event) {
        m_mouseMoveCallback(mapToParent(event->pos()), event->buttons());
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void D3D11FramePresenter::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_mouseReleaseCallback && event) {
        m_mouseReleaseCallback(
            mapToParent(event->pos()),
            event->button(),
            event->buttons(),
            event->modifiers()); // wjy: 子控件释放鼠标时继续复用父窗口的缩放提交、远端抬起和按钮命中处理。
        event->accept();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void D3D11FramePresenter::reset()
{
    if (!m_impl) {
        return;
    }
    releaseFrameResources();
    m_impl->processor.Reset();
    m_impl->processorEnum.Reset();
    resetCompositionResources(); // wjy: reset前先提交空根节点，保证窗口销毁时DComp不再持有Presenter资源。
    m_impl->swapChain.Reset();
    m_impl->sharedDevice.reset(); // wjy: 只释放当前窗口对共享Device的引用，不影响仍在显示的其它远控窗口。
    m_impl->swapWidth = 0;
    m_impl->swapHeight = 0;
    m_impl->processorSourceWidth = 0;
    m_impl->processorSourceHeight = 0;
    m_impl->processorOutputWidth = 0;
    m_impl->processorOutputHeight = 0;
    m_impl->hasPresentedFrame = false; // wjy: 主动reset后SwapChain已释放，不能再宣称存在可保留的最后画面。
    m_impl->lastFailureWasDeviceLost = false;
    m_impl->interactiveResize = false;
    m_impl->resizePending = false;
    m_impl->resizeCacheTexture.Reset();
    m_impl->resizeCacheWidth = 0;
    m_impl->resizeCacheHeight = 0; // wjy: Presenter重建后从普通呈现状态重新开始，旧缩放缓存不得影响新SwapChain。
}

void D3D11FramePresenter::resizeEvent(QResizeEvent* event)
{
    // =====wjy====
    if (m_impl && m_impl->interactiveResize) {
        ++m_impl->resizeEventCount; // wjy: 记录子HWND实际收到的尺寸事件数量，和父窗口resize次数对照定位跨窗口时序。
        m_impl->resizePending = true; // wjy: 继续记录松手后需要按最终客户区精确调整一次BackBuffer。
        // wjy: 拖拽期间不再对每个子HWND尺寸事件重复Present；网络新帧仍走正常呈现路径，松手时再统一按最终尺寸补画一次，避免DWM反复切换原生子表面。
    }
    // ===end====
    QWidget::resizeEvent(event);
}

} // namespace ui
