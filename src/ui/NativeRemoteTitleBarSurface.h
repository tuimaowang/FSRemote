#pragma once

#include <QImage>
#include <QRect>
#include <QtGlobal>

#include <memory>

namespace ui {

// =====wjy====
class NativeRemoteTitleBarSurface final {
public:
    NativeRemoteTitleBarSurface();
    ~NativeRemoteTitleBarSurface();

    NativeRemoteTitleBarSurface(const NativeRemoteTitleBarSurface&) = delete;
    NativeRemoteTitleBarSurface& operator=(const NativeRemoteTitleBarSurface&) = delete;

    bool create(WId parentWindowId); // wjy: 在Qt顶层远控HWND下创建独立标题栏子窗口，优先由DXGI SwapChain持有可见前台帧，GDI仅作失败回退。
    bool commitIdentityBand(const QImage& image); // wjy: 提交左侧背景与身份信息位图，完整合成后一次上传到DXGI前台缓冲。
    bool commitButtonGroup(const QImage& image); // wjy: 提交右侧按钮组位图，只在按钮视觉或可见集合变化时重建完整前台帧。
    bool setButtonGroupOrigin(int logicalX, qreal devicePixelRatio); // wjy: 缩放期间唯一的操作；只在合成缓冲里平移按钮段，不移动任何HWND。
    void setLogicalGeometry(const QRect& geometry, qreal devicePixelRatio); // wjy: Qt逻辑坐标转换为Win32物理子窗口矩形，宽度固定按虚拟屏预留因此缩放时不产生SetWindowPos。
    void setVisible(bool visible);
    void refresh(); // wjy: 重新Present当前完整前台缓冲；D3D不可用时同步BitBlt持久DIB。
    void raise();
    bool isCreated() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
// ===end====

} // namespace ui
