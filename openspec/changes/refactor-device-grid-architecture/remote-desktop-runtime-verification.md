# RemoteDesktopWindow 运行时核查记录

日期：2026-07-20

本记录对应任务 8.4。由于 `RemoteDesktopWindow` 依赖真实 Qt 窗口、D3D11 设备、原生 Viewer 和远端会话，未构造不真实的测试替身；改用源代码生命周期核查，并结合现有可执行专项测试确认纯状态边界。

## D3D11 presentation

- `D3D11FramePresenter::presentSharedTexture()` 在共享句柄、源尺寸和目标窗口尺寸无效时直接拒绝。
- 新纹理先放入局部候选对象，只有完整完成输入视图、输出视图、`VideoProcessorBlt` 和 `Present` 后才替换上一帧资源。
- 普通资源失败不会清空最后一帧；`DXGI_ERROR_DEVICE_REMOVED`、`RESET`、`HUNG` 和驱动内部错误会使共享设备失效，下一次尝试重新创建设备。
- `resizeEvent()` 清理 SwapChain/Processor 尺寸缓存，下一帧按新的 QWidget 尺寸重建或调整资源。

## CPU fallback

- `RemoteDesktopWindow::enqueueRemoteTextureFrame()` 在纹理呈现失败后进入有界的软件回退状态，并保留最后一张有效纹理（若仍然有效）。
- 软件回退通过 `m_softwareFallbackActive` 限制画质协调目标，恢复前保持低分辨率/低 FPS 上限，避免 BGRA CPU 路径在高分辨率下失控。
- 后续 BGRA 帧成功到达后关闭纹理覆盖并退出回退；真实设备移除会清除纹理活动状态，避免继续使用失效资源。

## resize / cursor synchronization

- `RemoteDesktopWindow::resizeEvent()` 同时更新父窗口掩码、D3D presenter 几何和远程质量输入，保证窗口尺寸、纹理目标矩形与质量协调器使用同一视口。
- `setWindowAndPresenterCursor()` / `unsetWindowAndPresenterCursor()` 同步父窗口和 D3D 子窗口光标，远程光标形状变更不会被子窗口覆盖。

## quality updates

- `remoteQualityInputsChanged` 只在可见性、尺寸、FPS、编码码率或回退状态变化时触发，避免每帧请求。
- `RemoteQualityCoordinator` 保留窗口级请求、全局预算、最小化策略、软件回退上限和恢复保持时间；现有专项测试覆盖这些纯状态决策。
- 已通过 `fsremote_remote_quality_policy_tests`、`fsremote_remote_quality_coordinator_tests`，并在本轮全量 CTest 中再次通过。

## shutdown ordering

- `shutdownForApplicationExit()` 先使 Viewer 回调代际失效，再提交异步 stop；`RemoteViewerLifecycleManager` 在 stop 完成后释放启动名额。
- `RemoteWindowCoordinator::shutdownWindows()` 提供包含 closing 窗口的稳定快照，应用退出阶段不会遗漏正在关闭的窗口。
- `RemoteDesktopWindow` 析构路径最终调用 presenter reset、定时器停止和回调上下文释放，避免后台回调解引用已销毁 QWidget。

## Verification

- Release x64 主程序构建：已成功。
- 全量 CTest：21/21 通过（2026-07-20）。
- `git diff --check`：通过。
- 本记录未改变远控协议、DLL API、端口或窗口行为；仅记录现有实现的核查结论。
