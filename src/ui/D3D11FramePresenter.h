#pragma once

#include <cstdint>
#include <functional>

#include <QByteArray>
#include <QImage>
#include <QPoint>
#include <QString>
#include <QWidget>

class QRect;
class QMouseEvent;

struct ID3D11Texture2D; // wjy: 只在私有实现里使用具体D3D类型，头文件保持前向声明不拉入d3d11.h。

namespace ui {

struct D3D11CompositorTelemetry {
    std::uint64_t commitCount = 0;
    std::uint64_t commitFailureCount = 0;
    double averageCommitMs = 0.0;
};

class D3D11FramePresenter final : public QWidget {
public:
    using MouseMoveCallback = std::function<void(const QPoint& parentPosition, Qt::MouseButtons buttons)>;
    using MouseButtonCallback = std::function<void(
        const QPoint& parentPosition,
        Qt::MouseButton button,
        Qt::MouseButtons buttons,
        Qt::KeyboardModifiers modifiers)>;

    explicit D3D11FramePresenter(QWidget* parent = nullptr);
    ~D3D11FramePresenter() override;

    bool presentSharedTexture(void* sharedHandle, int width, int height);
    void discardSharedTexture(void* sharedHandle); // wjy: Qt取消尚未呈现的已接受帧时只完成keyed mutex交接，不执行Blt或Present。
    void setCompositorHostWindow(void* hostWindow, bool enabled); // wjy: 新路径把SwapChain挂到顶层窗口的DirectComposition目标，旧路径继续使用本机子HWND。
    bool usesCompositorSurface() const; // wjy: 父窗口据此决定是否隐藏/移动旧D3D子控件，避免新路径再次产生第二个可见表面。
    bool hasVisiblePresentation() const; // wjy: 统一返回子HWND或DirectComposition视觉树的当前可见状态。
    bool hasCompositorOverlay() const; // wjy: 叠加SwapChain创建成功后才隐藏旧标题栏/性能层，创建失败保留可用回退表面。
    void setPresentationVisible(bool visible); // wjy: 统一控制旧子HWND或DComp视频视觉的可见性，更新遮罩不会露出旧帧。
    bool setCompositorOutputRect(const QRect& rect); // wjy: 提交统一内容矩形并返回结果；失败时实现会恢复上一份可见几何。
    bool presentCompositorOverlay(const QImage& image); // 仅在候选叠加表面完成上传、Present和Commit后返回成功。
    void setInteractiveResize(bool active); // wjy: 拖拽期间冻结SwapChain尺寸但继续呈现新帧，松手后只按最终客户区调整一次并立即补画缓存帧。
    void reset();
    long lastDeviceRemovalReason() const; // wjy: 返回最近一次D3D11失败HRESULT，供窗口级诊断记录，不触发进程级异常。
    bool hasPresentedFrame() const; // wjy: 新纹理失败时由父窗口判断是否可以继续保留最后一次成功画面，避免露出黑色背景。
    bool lastFailureWasDeviceLost() const; // wjy: 区分普通单帧资源竞争和真实Device Removed，只有设备级故障才需要重建设备代际。
    D3D11CompositorTelemetry compositorTelemetry() const;
    QString resizeDebugSnapshot() const; // wjy: 返回当前子HWND、SwapChain和交互补帧状态，供父窗口一次性写出缩放手势时序诊断。
    void setMouseMoveCallback(MouseMoveCallback callback);
    void setMouseButtonCallbacks(MouseButtonCallback pressCallback, MouseButtonCallback releaseCallback);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override; // wjy: 仅记录原生子HWND的尺寸/绘制消息，不改变Windows默认处理结果。
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Impl;
    bool ensureDevice();
    bool ensureSwapChain();
    bool ensureCompositionTarget(); // wjy: 创建一次顶层DirectComposition目标和视频视觉，避免每帧重建视觉树。
    bool ensureCompositionOverlaySurface(int width, int height); // wjy: 只在窗口物理尺寸改变时重建本地UI叠加SwapChain。
    void resetCompositionResources(); // wjy: 设备丢失或窗口关闭时按依赖顺序释放视觉树和目标。
    bool commitCompositionVisual(); // 几何或SwapChain变化只有在DComp Commit成功后才视为可见提交。
    bool ensureVideoProcessor(int sourceWidth, int sourceHeight, int outputWidth, int outputHeight);
    bool blitAndPresent(ID3D11Texture2D* sourceTexture, int frameWidth, int frameHeight); // wjy: 共享纹理和缩放缓存副本共用同一条Blt/Present路径，避免两处letterbox规则漂移。
    void cacheFrameForResize(ID3D11Texture2D* sourceTexture, int frameWidth, int frameHeight); // wjy: 仅在交互缩放期间复制一份私有纹理，常态呈现不增加GPU拷贝。
    bool presentCachedFrameForResize(); // wjy: 子HWND尺寸变化后立即重呈现缓存帧，但严格复用现有BackBuffer并禁止进入ResizeBuffers路径。
    bool handleDeviceFailure(long result); // wjy: Device Removed/Reset时废弃共享设备代际，下一次重试自动创建新设备。
    void releaseFrameResources();

    Impl* m_impl = nullptr;
    MouseMoveCallback m_mouseMoveCallback;
    MouseButtonCallback m_mousePressCallback;
    MouseButtonCallback m_mouseReleaseCallback;
};

} // namespace ui
