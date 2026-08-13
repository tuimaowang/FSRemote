# 当前远控窗口渲染基线

## 可见表面

- `src/ui/RemoteDesktopWindow.cpp`：顶层 Qt `QWidget`，负责连接提示、BGRA 软件帧、更新遮罩和旧父窗口标题栏绘制块。
- `src/ui/D3D11FramePresenter.cpp`：`WA_NativeWindow` D3D11 子窗口，创建独立 `IDXGISwapChain`，负责 H265/D3D11 共享纹理、letterbox、Present 和交互缩放期间的缓存帧。
- `src/ui/NativeRemoteTitleBarSurface.cpp`：独立原生标题栏子 HWND，内部持有 DIB 合成的身份段和按钮段。
- `RemotePerformanceOverlayLabel`（定义在 `RemoteDesktopWindow.cpp`）：独立透明工具窗口，用于显示本机 FPS、码率、解码压力和丢帧信息。

## 主要入口

- 首帧路径：`enqueueRemoteFrame` / `enqueueRemoteTextureFrame` → `flushPendingRemoteFrame` / `drainPendingRemoteTextureFrame`。
- D3D 路径：`D3D11FramePresenter::presentSharedTexture` → `blitAndPresent` → `IDXGISwapChain::Present`。
- 软件回退：`RemoteDesktopWindow::setRemoteFrame`，先把 BGRA 画入 Qt backing store，再隐藏 D3D 子窗口。
- 几何同步：`RemoteDesktopWindow::resizeEvent` → `updateTexturePresenterGeometry`、`updateNativeTitleBarSurface`、`updatePerformanceOverlayGeometry`。
- 缩放入口：标题栏/边缘命中后调用 `startSystemWindowResize`，失败时由 `mouseMoveEvent` 逐次 `setGeometry`；结束时 `finishInteractiveWindowOperation` 调用 `setInteractiveResize(false)`。

## 已确认的闪烁风险

1. 顶层 Qt backing store、D3D 子 HWND、标题栏 HWND 和性能浮层 HWND 在同一 resize 手势中分别收到几何/绘制消息。
2. 这些表面虽然各自日志显示 Present/WM 消息成功，但没有共同的“最终可见像素”提交点。
3. 当前变更先建立统一状态与几何快照，再逐步迁移可见层；旧路径通过运行时开关保留回滚能力。
