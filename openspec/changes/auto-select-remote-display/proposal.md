## Why

当前 Host 在第一个已认证远控会话建立时总是创建 Parsec 虚拟显示器，即使 Windows 已有可正常捕获的真实主屏。这会造成不必要的显示拓扑切换、主屏变更和虚拟显示器创建开销，也让每次完全断开后的重新远控都重复执行同一流程。

## What Changes

- 在共享 Host 媒体管线启动时自动检测当前 Windows 主屏，并优先使用可捕获的非 Parsec 主屏。
- 对真实主屏依次尝试原生 DXGI 纹理捕获和现有 DesktopCapturer 兼容捕获；只有真实主屏不存在或两种捕获路径都不可用时才创建 Parsec 虚拟显示器。
- 保留现有“首个订阅者启动、多个会话共享、最后一个订阅者停止”的媒体生命周期，不在活跃会话期间动态热切换显示源。
- 统一记录最终捕获模式、目标设备、真实屏回退原因和虚拟屏创建原因，便于实机诊断。
- 虚拟显示器停止时只释放本次 FSRemote 会话实际持有的显示实例，不把其他进程可能拥有的 Parsec 显示器当作默认清理目标。
- 增加显示选择策略和共享媒体生命周期的自动化测试。

## Capabilities

### New Capabilities

- `automatic-host-display-selection`: 定义 Host 在真实主屏、兼容捕获和 Parsec 虚拟显示器之间自动选择捕获目标及回退路径的行为。

### Modified Capabilities

无。

## Impact

- 主要影响 `third_party/uu_stream_webrtc/src/host_media_pipeline.*`、`third_party/lan_stream_probe/src/dxgi_capture.*` 和 `third_party/uu_stream_webrtc/src/parsec_vdd_session.*`。
- 需要增加可独立测试的显示选择策略，并扩展 `third_party/uu_stream_webrtc/tests` 下的回归测试。
- 不修改 Viewer API、WebRTC 信令协议、PeerConnection 所有权、多客户端准入协议或 Qt 远控窗口交互。
