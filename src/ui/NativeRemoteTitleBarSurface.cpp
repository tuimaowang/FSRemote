#include "ui/NativeRemoteTitleBarSurface.h"

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

#include <QColor>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>

namespace ui {

#if defined(Q_OS_WIN)
namespace {

using Microsoft::WRL::ComPtr;

struct TitleBarPresentationDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::atomic_bool invalidated = false;
};

std::mutex g_titleBarDeviceMutex;
std::weak_ptr<TitleBarPresentationDevice> g_titleBarDevice;

std::shared_ptr<TitleBarPresentationDevice> acquireTitleBarPresentationDevice()
{
    std::lock_guard lock(g_titleBarDeviceMutex);
    if (std::shared_ptr<TitleBarPresentationDevice> existing = g_titleBarDevice.lock()) {
        if (!existing->invalidated.load(std::memory_order_acquire)) {
            return existing;
        }
    }

    auto shared = std::make_shared<TitleBarPresentationDevice>();
    const D3D_FEATURE_LEVEL preferredLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL actualLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT result = ::D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        preferredLevels,
        2,
        D3D11_SDK_VERSION,
        &shared->device,
        &actualLevel,
        &shared->context);
    if (result == E_INVALIDARG) {
        const D3D_FEATURE_LEVEL fallbackLevel[] = {D3D_FEATURE_LEVEL_11_0};
        result = ::D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            fallbackLevel,
            1,
            D3D11_SDK_VERSION,
            &shared->device,
            &actualLevel,
            &shared->context);
    }
    if (FAILED(result) || !shared->device || !shared->context) {
        return {};
    }
    g_titleBarDevice = shared;
    return shared;
}

bool isDeviceLoss(HRESULT result)
{
    return result == DXGI_ERROR_DEVICE_REMOVED
        || result == DXGI_ERROR_DEVICE_RESET
        || result == DXGI_ERROR_DEVICE_HUNG
        || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

void invalidateTitleBarPresentationDevice(
    const std::shared_ptr<TitleBarPresentationDevice>& shared)
{
    if (!shared) return;
    shared->invalidated.store(true, std::memory_order_release);
    std::lock_guard lock(g_titleBarDeviceMutex);
    if (g_titleBarDevice.lock() == shared) {
        g_titleBarDevice.reset();
    }
}

} // namespace
#endif

struct NativeRemoteTitleBarSurface::Impl {
#if defined(Q_OS_WIN)
    // =====wjy====
    // wjy: 一个层就是一张顶向下32位DIB加它自己的内存DC；只有composite层参与WM_PAINT。
    struct Layer {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previousBitmap = nullptr;
        void* pixels = nullptr;
        int capacityWidth = 0;
        int capacityHeight = 0;
        int contentWidth = 0;
        int contentHeight = 0; // wjy: 记录真实提交尺寸，合成时只复制这一部分而不是整块容量。

        bool ensure(int requiredWidth, int requiredHeight)
        {
            requiredWidth = std::max(1, requiredWidth);
            requiredHeight = std::max(1, requiredHeight);
            if (bitmap && capacityWidth >= requiredWidth && capacityHeight >= requiredHeight) return true;

            const int nextWidth = std::max(requiredWidth, capacityWidth);
            const int nextHeight = std::max(requiredHeight, capacityHeight);
            BITMAPINFO info = {};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = nextWidth;
            info.bmiHeader.biHeight = -nextHeight; // wjy: 顶向下DIB使QImage每行可直接复制而无需翻转。
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            HDC screenDc = ::GetDC(nullptr);
            void* nextPixels = nullptr;
            HBITMAP nextBitmap = ::CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &nextPixels, nullptr, 0);
            ::ReleaseDC(nullptr, screenDc);
            if (!nextBitmap || !nextPixels) {
                if (nextBitmap) ::DeleteObject(nextBitmap);
                return false;
            }
            if (!dc) dc = ::CreateCompatibleDC(nullptr);
            if (!dc) {
                ::DeleteObject(nextBitmap);
                return false;
            }
            if (bitmap) {
                ::SelectObject(dc, previousBitmap);
                ::DeleteObject(bitmap);
            }
            bitmap = nextBitmap;
            pixels = nextPixels;
            capacityWidth = nextWidth;
            capacityHeight = nextHeight;
            previousBitmap = ::SelectObject(dc, bitmap);
            fill();
            return true;
        }

        void fill() const
        {
            if (!pixels || capacityWidth <= 0 || capacityHeight <= 0) return;
            const quint32 pixel = 0xFFF2EEE9u; // wjy: 内存按BGRA排列，对应不透明标题栏颜色#E9EEF2。
            auto* output = static_cast<quint32*>(pixels);
            std::fill(output, output + static_cast<size_t>(capacityWidth) * capacityHeight, pixel);
        }

        bool store(const QImage& image)
        {
            if (image.isNull() || !ensure(image.width(), image.height())) return false;
            const int stride = capacityWidth * 4;
            for (int row = 0; row < image.height(); ++row) {
                std::memcpy(
                    static_cast<uchar*>(pixels) + static_cast<size_t>(row) * stride,
                    image.constScanLine(row),
                    static_cast<size_t>(image.width()) * 4);
            }
            contentWidth = image.width();
            contentHeight = image.height();
            return true;
        }

        void release()
        {
            if (dc && bitmap) ::SelectObject(dc, previousBitmap);
            if (bitmap) ::DeleteObject(bitmap);
            if (dc) ::DeleteDC(dc);
            dc = nullptr;
            bitmap = nullptr;
            previousBitmap = nullptr;
            pixels = nullptr;
            capacityWidth = 0;
            capacityHeight = 0;
            contentWidth = 0;
            contentHeight = 0;
        }
    };

    HWND window = nullptr;
    std::shared_ptr<TitleBarPresentationDevice> presentationDevice;
    ComPtr<IDXGISwapChain> swapChain;
    int swapWidth = 0;
    int swapHeight = 0;
    bool d3dUnavailable = false;
    Layer identity; // wjy: 背景与左侧身份信息，虚拟屏宽度，缩放时不重新提交。
    Layer buttons; // wjy: 右侧按钮组，宽度只取决于可见按钮集合。
    Layer composite; // wjy: DXGI上传与GDI回退共用的完整像素源，任何可见提交都只读取这一张合成完成的帧。
    int buttonOriginX = -1; // wjy: 按钮段在合成缓冲中的当前物理x；-1表示尚未放置。
    int appliedX = std::numeric_limits<int>::min();
    int appliedY = std::numeric_limits<int>::min();
    int appliedWidth = 0;
    int appliedHeight = 0;
    bool visible = false; // wjy: 缓存真实显隐状态，普通状态刷新不得重复调用ShowWindow扰动DWM子窗口合成。
    bool raised = false; // wjy: 标题栏只在首次显示或全屏恢复时建立一次层级，交互缩放期间禁止重复SetWindowPos(HWND_TOP)。

    static constexpr COLORREF backgroundColor = RGB(233, 238, 242);

    void releasePresentation()
    {
        swapChain.Reset();
        presentationDevice.reset();
        swapWidth = 0;
        swapHeight = 0;
    }

    void handlePresentationFailure(HRESULT result)
    {
        if (isDeviceLoss(result)) {
            invalidateTitleBarPresentationDevice(presentationDevice);
        } else {
            d3dUnavailable = true; // wjy: 当前系统不接受子HWND SwapChain时固定退回GDI，不在每次标题栏刷新中重复创建设备。
        }
        releasePresentation();
    }

    bool ensureSwapChain()
    {
        if (d3dUnavailable || !window || !composite.pixels
            || composite.contentWidth <= 0 || composite.contentHeight <= 0) {
            return false;
        }
        if (!presentationDevice
            || presentationDevice->invalidated.load(std::memory_order_acquire)) {
            releasePresentation();
            presentationDevice = acquireTitleBarPresentationDevice();
            if (!presentationDevice) {
                d3dUnavailable = true;
                return false;
            }
        }

        const int targetWidth = composite.contentWidth;
        const int targetHeight = composite.contentHeight;
        if (swapChain && swapWidth == targetWidth && swapHeight == targetHeight) {
            return true;
        }
        if (swapChain) {
            presentationDevice->context->Flush(); // wjy: ResizeBuffers前提交对旧backbuffer的上传命令，确保DXGI不因仍有排队引用而拒绝调整。
            const HRESULT resizeResult = swapChain->ResizeBuffers(
                0,
                static_cast<UINT>(targetWidth),
                static_cast<UINT>(targetHeight),
                DXGI_FORMAT_B8G8R8A8_UNORM,
                0);
            if (SUCCEEDED(resizeResult)) {
                swapWidth = targetWidth;
                swapHeight = targetHeight;
                return true;
            }
            handlePresentationFailure(resizeResult);
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        if (FAILED(presentationDevice->device.As(&dxgiDevice))
            || FAILED(dxgiDevice->GetAdapter(&adapter))
            || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            d3dUnavailable = true;
            releasePresentation();
            return false;
        }

        DXGI_SWAP_CHAIN_DESC description = {};
        description.BufferDesc.Width = static_cast<UINT>(targetWidth);
        description.BufferDesc.Height = static_cast<UINT>(targetHeight);
        description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.BufferDesc.RefreshRate.Numerator = 60;
        description.BufferDesc.RefreshRate.Denominator = 1;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.OutputWindow = window;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const HRESULT createResult = factory->CreateSwapChain(
            presentationDevice->device.Get(), &description, &swapChain);
        if (FAILED(createResult) || !swapChain) {
            handlePresentationFailure(FAILED(createResult) ? createResult : E_FAIL);
            return false;
        }
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        swapWidth = targetWidth;
        swapHeight = targetHeight;
        return true;
    }

    bool presentD3D()
    {
        if (!ensureSwapChain()) return false;

        ComPtr<ID3D11Texture2D> backBuffer;
        const HRESULT bufferResult = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(bufferResult) || !backBuffer) {
            handlePresentationFailure(FAILED(bufferResult) ? bufferResult : E_FAIL);
            return false;
        }
        presentationDevice->context->UpdateSubresource(
            backBuffer.Get(),
            0,
            nullptr,
            composite.pixels,
            static_cast<UINT>(composite.capacityWidth * 4),
            0); // wjy: 每次把完整不透明合成帧上传到前台缓冲，DWM永远不会看到只更新了一半的标题栏。
        const HRESULT presentResult = swapChain->Present(0, 0);
        if (FAILED(presentResult)) {
            handlePresentationFailure(presentResult);
            return false;
        }
        return true;
    }

    // wjy: 把identity整幅铺入合成缓冲，再把buttons贴到指定位置；两次内存BitBlt完成后才允许DXGI上传或GDI回退，
    // 因此任何可见提交都看不到半成品，也完全不需要移动或改变任何子HWND。
    void recomposite()
    {
        if (!composite.dc || !identity.dc) return;
        if (identity.contentWidth > 0 && identity.contentHeight > 0) {
            ::BitBlt(composite.dc, 0, 0, identity.contentWidth, identity.contentHeight,
                identity.dc, 0, 0, SRCCOPY);
            composite.contentWidth = identity.contentWidth;
            composite.contentHeight = identity.contentHeight;
        }
        if (buttons.dc && buttons.contentWidth > 0 && buttons.contentHeight > 0 && buttonOriginX >= 0) {
            const int clippedWidth = std::min(buttons.contentWidth, composite.contentWidth - buttonOriginX);
            if (clippedWidth > 0) {
                ::BitBlt(composite.dc, buttonOriginX, 0, clippedWidth, buttons.contentHeight,
                    buttons.dc, 0, 0, SRCCOPY); // wjy: 按钮段直接覆盖在身份层之上，与旧实现按钮不与身份区重叠的规则一致。
            }
        }
    }

    bool ensureComposite()
    {
        return composite.ensure(
            std::max(identity.contentWidth, 1),
            std::max(identity.contentHeight, 1));
    }

    void present()
    {
        if (!window || !visible) return;
        if (presentD3D()) {
            return; // wjy: DXGI前台缓冲由DWM持续持有，父窗口逐像素缩放不会让标题栏退回Qt backing store或普通GDI屏幕位图。
        }
        ::RedrawWindow(window, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_NOCHILDREN); // wjy: D3D不可用时保留原生DIB BitBlt回退，标题栏功能不因显卡初始化失败而消失。
    }

    static const wchar_t* className()
    {
        return L"FSRemoteNativeTitleBarSurface";
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Impl* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create ? create->lpCreateParams : nullptr);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT; // wjy: 鼠标命中继续落到父远控窗口，保留现有Qt标题栏按钮、拖动、双击和缩放逻辑。
        case WM_ERASEBKGND:
            return 1; // wjy: 合成缓冲覆盖完整客户区，不允许Windows先擦除为透明或默认背景。
        case WM_PAINT:
            if (self) return self->paint();
            break;
        case WM_PRINTCLIENT:
            if (self) {
                self->blit(reinterpret_cast<HDC>(wParam));
                return 0;
            }
            break;
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static bool ensureWindowClass()
    {
        static const ATOM atom = [] {
            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.style = 0; // wjy: 持久合成缓冲自行覆盖完整标题栏，禁止尺寸变化让Windows自动把整个子窗口标记为失效。
            windowClass.lpfnWndProc = &Impl::windowProc;
            windowClass.hInstance = ::GetModuleHandleW(nullptr);
            windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            windowClass.hbrBackground = nullptr;
            windowClass.lpszClassName = className();
            const ATOM registered = ::RegisterClassExW(&windowClass);
            if (registered) return registered;
            return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS
                ? static_cast<ATOM>(1)
                : static_cast<ATOM>(0); // wjy: 只把“类已注册”视为成功，其它Win32注册错误必须阻止创建无效子窗口。
        }();
        return atom != 0;
    }

    void blit(HDC destination) const
    {
        if (!destination || !window) return;
        RECT client = {};
        ::GetClientRect(window, &client);
        const int width = std::max(0L, client.right - client.left);
        const int height = std::max(0L, client.bottom - client.top);
        if (composite.dc && composite.bitmap && width > 0 && height > 0 && composite.contentWidth > 0) {
            const int blitWidth = std::min(width, composite.contentWidth);
            const int blitHeight = std::min(height, composite.contentHeight);
            if (blitWidth > 0 && blitHeight > 0) {
                ::BitBlt(destination, 0, 0, blitWidth, blitHeight,
                    composite.dc, 0, 0, SRCCOPY); // wjy: WM_PAINT只做一次合成缓冲到客户区的复制，不参与任何排版计算。
            }
        } else {
            HBRUSH brush = ::CreateSolidBrush(backgroundColor);
            ::FillRect(destination, &client, brush);
            ::DeleteObject(brush);
        }
    }

    LRESULT paint()
    {
        PAINTSTRUCT paintStruct = {};
        HDC destination = ::BeginPaint(window, &paintStruct);
        if (!presentD3D()) {
            blit(destination);
        }
        ::EndPaint(window, &paintStruct);
        return 0;
    }

    void releaseGraphics()
    {
        releasePresentation();
        identity.release();
        buttons.release();
        composite.release();
        buttonOriginX = -1;
    }
    // ===end====
#endif
};

NativeRemoteTitleBarSurface::NativeRemoteTitleBarSurface()
    : m_impl(std::make_unique<Impl>())
{
}

NativeRemoteTitleBarSurface::~NativeRemoteTitleBarSurface()
{
#if defined(Q_OS_WIN)
    m_impl->releasePresentation(); // wjy: SwapChain必须在所属HWND销毁前释放，避免DXGI仍引用已经失效的输出窗口。
    if (m_impl->window) {
        ::DestroyWindow(m_impl->window);
        m_impl->window = nullptr;
    }
    m_impl->releaseGraphics();
#endif
}

bool NativeRemoteTitleBarSurface::create(WId parentWindowId)
{
#if defined(Q_OS_WIN)
    if (m_impl->window) return true;
    if (!parentWindowId || !Impl::ensureWindowClass()) return false;
    m_impl->window = ::CreateWindowExW(
        WS_EX_NOACTIVATE,
        Impl::className(), L"", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 1, 1,
        reinterpret_cast<HWND>(parentWindowId), nullptr, ::GetModuleHandleW(nullptr), m_impl.get());
    return m_impl->window != nullptr;
#else
    Q_UNUSED(parentWindowId)
    return false;
#endif
}

// =====wjy====
bool NativeRemoteTitleBarSurface::commitIdentityBand(const QImage& image)
{
#if defined(Q_OS_WIN)
    if (!m_impl->window || image.isNull()) return false;
    const QImage source = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (!m_impl->identity.store(source)) return false;
    if (!m_impl->ensureComposite()) return false;
    m_impl->recomposite();
    m_impl->present();
    return true;
#else
    Q_UNUSED(image)
    return false;
#endif
}

bool NativeRemoteTitleBarSurface::commitButtonGroup(const QImage& image)
{
#if defined(Q_OS_WIN)
    if (!m_impl->window || image.isNull()) return false;
    const QImage source = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (!m_impl->buttons.store(source)) return false;
    if (!m_impl->ensureComposite()) return false;
    m_impl->recomposite();
    m_impl->present();
    return true;
#else
    Q_UNUSED(image)
    return false;
#endif
}

bool NativeRemoteTitleBarSurface::setButtonGroupOrigin(int logicalX, qreal devicePixelRatio)
{
#if defined(Q_OS_WIN)
    if (!m_impl->window || !m_impl->composite.dc) return false;
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatio);
    const int physicalX = std::max(0, qRound(logicalX * dpr));
    if (m_impl->buttonOriginX == physicalX) return true; // wjy: 同一物理位置不重复合成，底边缩放不会产生任何标题栏工作。

    m_impl->buttonOriginX = physicalX;
    m_impl->recomposite(); // wjy: 缩放期间唯一的操作：两次内存BitBlt重建合成缓冲，不移动也不改变任何HWND。
    m_impl->present();
    return true;
#else
    Q_UNUSED(logicalX)
    Q_UNUSED(devicePixelRatio)
    return false;
#endif
}
// ===end====

void NativeRemoteTitleBarSurface::setLogicalGeometry(const QRect& geometry, qreal devicePixelRatio)
{
#if defined(Q_OS_WIN)
    if (!m_impl->window) return;
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatio);
    const int x = qRound(geometry.x() * dpr);
    const int y = qRound(geometry.y() * dpr);
    const int width = std::max(
        std::max(1, qRound(geometry.width() * dpr)),
        ::GetSystemMetrics(SM_CXVIRTUALSCREEN)); // wjy: 子标题栏固定覆盖整个虚拟屏幕宽度，由父客户区裁剪；窗口宽度变化因此永远不产生SetWindowPos。
    const int height = std::max(1, qRound(geometry.height() * dpr));
    if (m_impl->appliedX == x && m_impl->appliedY == y
        && m_impl->appliedWidth == width && m_impl->appliedHeight == height) {
        return; // wjy: 全部缩放路径都命中这里直接返回，持久前台表面保持原位。
    }
    ::SetWindowPos(m_impl->window, nullptr, x, y, width, height,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER); // wjy: 几何同步不改变Z序也不丢弃现有客户区像素，上一帧合成缓冲继续作为可见表面。
    m_impl->appliedX = x;
    m_impl->appliedY = y;
    m_impl->appliedWidth = width;
    m_impl->appliedHeight = height;
    m_impl->present(); // wjy: 物理尺寸或DPI真正改变时立即把完整帧提交给同一个SwapChain；普通缩放因固定虚拟屏宽度不会命中这里。
#else
    Q_UNUSED(geometry)
    Q_UNUSED(devicePixelRatio)
#endif
}

void NativeRemoteTitleBarSurface::setVisible(bool visible)
{
#if defined(Q_OS_WIN)
    if (!m_impl->window) return;
    const bool systemVisible = ::IsWindowVisible(m_impl->window) != FALSE; // wjy: Win32在父窗口合成变化后可能与本地缓存不一致，显隐判断必须复核真实HWND状态。
    if (m_impl->visible == visible && systemVisible == visible) return;
    ::ShowWindow(m_impl->window, visible ? SW_SHOWNOACTIVATE : SW_HIDE); // wjy: 缓存或真实状态任一不一致时恢复标题栏，避免“逻辑可见、系统实际隐藏”永久跳过修复。
    m_impl->visible = visible;
    if (!visible) {
        m_impl->raised = false; // wjy: 隐藏后清除层级缓存，下一次退出全屏时允许重新建立一次正确的兄弟窗口顺序。
    } else {
        m_impl->present(); // wjy: 首次显示或退出全屏时先提交完整DXGI前台缓冲，避免依赖随后异步到达的WM_PAINT。
    }
#else
    Q_UNUSED(visible)
#endif
}

// =====wjy====
void NativeRemoteTitleBarSurface::refresh()
{
#if defined(Q_OS_WIN)
    if (!m_impl->window || !m_impl->visible) return;
    if (::IsWindowVisible(m_impl->window) == FALSE) {
        ::ShowWindow(m_impl->window, SW_SHOWNOACTIVATE); // wjy: 缩放期间若系统意外隐藏子HWND，立即恢复但不激活或抢走远控窗口焦点。
        m_impl->raised = false; // wjy: 重新显示后要求收尾阶段重新核对兄弟窗口层级，不能沿用隐藏前的置顶缓存。
    }
    m_impl->present(); // wjy: 优先重新Present同一完整DXGI前台缓冲；只有D3D不可用时才同步BitBlt持久DIB。
#endif
}
// ===end====

void NativeRemoteTitleBarSurface::raise()
{
#if defined(Q_OS_WIN)
    if (!m_impl->window || !m_impl->visible) return;
    if (::GetWindow(m_impl->window, GW_HWNDFIRST) == m_impl->window) {
        m_impl->raised = true; // wjy: 每次交互收尾按真实兄弟HWND顺序复核，仍在顶部时不产生多余SetWindowPos。
        return;
    }
    if (::SetWindowPos(m_impl->window, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        m_impl->raised = true; // wjy: Qt或D3D子窗口在缩放中改变过层级时恢复标题栏到顶部，避免本地布尔缓存误判后永久透明。
    }
#endif
}

bool NativeRemoteTitleBarSurface::isCreated() const
{
#if defined(Q_OS_WIN)
    return m_impl->window != nullptr;
#else
    return false;
#endif
}

} // namespace ui
