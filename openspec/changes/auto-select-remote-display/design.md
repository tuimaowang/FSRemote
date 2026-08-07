## Context

`HostMediaPipeline` 目前在首个媒体订阅出现时直接创建 `ParsecVddSession`，然后仅在 VDD 成功时尝试原生 DXGI 捕获；VDD 不可用时才使用 `DesktopCapturer` 的首个屏幕源。该顺序把虚拟显示器当成默认目标，也使 `source_refresh_hz()`、绝对鼠标主屏语义和诊断日志都隐含依赖 VDD。

底层 `DxgiCapture` 已支持按 `\\.\DISPLAYx` 精确选择输出，也支持在未指定名称时枚举活动输出。Windows 侧可通过 `EnumDisplayDevicesW`、`DISPLAY_DEVICE_PRIMARY_DEVICE`、`EnumDisplaySettingsW` 和 `HMONITOR` 找到当前主屏，因此不需要改变 Viewer、WebRTC 信令或 PeerConnection 架构。

## Goals / Non-Goals

**Goals:**

- 首个已认证远控会话建立时优先使用可用的非 Parsec Windows 主屏。
- 对同一真实主屏按“原生 DXGI、DesktopCapturer”顺序回退，两者都不可用时才创建 VDD。
- VDD 路径继续优先使用原生 DXGI，并保留 DesktopCapturer 兼容回退。
- 现有多会话共享捕获源和最后订阅者释放资源的生命周期保持不变。
- 捕获目标刷新率、诊断信息和清理行为同时适用于真实屏与虚拟屏。

**Non-Goals:**

- 不支持活跃会话期间因显示器热插拔而无缝切换媒体源。
- 不增加远控端显示器选择 UI，也不支持自动捕获非主屏。
- 不修改 Viewer API、媒体协议、输入协议或多客户端准入规则。
- 不新增外部依赖。

## Decisions

### 1. 在 Host 媒体管线内完成显示目标决策

显示选择发生在 `HostMediaPipeline::SharedState::start_locked()`，与现有首订阅惰性启动边界一致。新增纯策略结构描述 Windows 显示候选项，并只选择同时满足活动、主屏、有效分辨率、非 Parsec、非镜像和非远程标记的候选项。

替代方案是在 Qt 主程序启动时预先决定。该方案无法反映远控真正建立时的显示拓扑，也会让 UI 层承担原生捕获细节，因此不采用。

### 2. 真实主屏使用两级捕获回退

发现真实主屏后，管线先用其设备名启动 `DxgiVideoSource`。如果 DXGI 初始化失败，再用该主屏对应的 `HMONITOR` 和设备名启动 `DesktopVideoSource`。物理目标的 DesktopCapturer 必须精确匹配目标源，不能静默选择 Parsec 屏或列表首项。

只有真实主屏不存在，或真实主屏的两种捕获器都无法初始化/选择目标时，才进入现有 VDD 创建路径。

### 3. 捕获目标在一轮共享管线生命周期内保持稳定

首个订阅者确定目标后，后续订阅者复用同一个 `VideoTrackSource`。直到最后一个订阅者释放前不重新枚举或切换目标；所有订阅者离开后清空目标状态，下次首订阅重新判断。

替代方案是运行中热切换。当前每个会话的 track 持有同一 source，热切换需要新增可替换 source 或逐会话替换 track，并同步输入坐标，因此留作后续能力。

### 4. 主屏坐标语义保持不变

真实屏路径只选择 Windows 当前主屏，因此现有不带 `MOUSEEVENTF_VIRTUALDESK` 的绝对鼠标注入仍与画面一致。VDD 路径继续把新建显示器设为主屏。

### 5. 统一保存活动目标身份

共享状态保存最终模式、设备名和监视器 ID。`source_refresh_hz()` 从活动设备名读取刷新率，不再通过 `virtual_display` 是否存在推断目标类型。启动、回退和停止日志均写明模式与设备名。

### 6. VDD 只清理自身持有的显示实例

删除创建前枚举并移除所有 Parsec 显示器的行为。`ParsecVddSession` 只记录 `VddAddDisplay` 返回的索引，并在自身引用归零时移除该索引。这样不会把其他进程创建的 Parsec 显示器纳入默认清理范围。

## Risks / Trade-offs

- [Windows 报告主屏活动但实际捕获异常] → DXGI 初始化失败后尝试精确目标的 DesktopCapturer；两者失败再创建 VDD，并记录每一级错误。
- [DesktopCapturer 的源 ID 与 HMONITOR 映射在不同 WebRTC 版本中变化] → 同时保留 ID、display ID 和名称匹配；物理目标无法精确匹配时视为失败，禁止误抓其他屏幕。
- [移除全局 Parsec 清理后旧崩溃进程可能留下显示器] → 优先保证跨进程安全；本变更只清理当前会话明确持有的索引，旧实例由显式维护流程处理。
- [显示器在活跃会话中拔出] → 保留当前 DXGI 恢复行为；本版本不承诺自动切换 VDD，完全断开后的下一次连接会重新决策。
- [真实主屏路径改变性能特征] → 优先使用现有 D3D11 原生纹理路径，仅在初始化失败时进入 CPU 回退。

## Migration Plan

1. 增加纯显示选择策略和单元测试。
2. 将真实主屏 DXGI/CPU 回退接入共享媒体管线，并统一活动目标状态。
3. 收紧 DesktopCapturer 的精确目标选择，避免物理路径误选其他屏幕。
4. 移除 VDD 启动前的跨进程全局显示清理。
5. 运行策略测试、媒体管线测试和目标构建；如需回滚，可恢复原有 VDD 优先的 `start_locked()` 顺序，不涉及数据迁移或协议兼容。

## Open Questions

无。首版固定采用自动策略，不暴露“始终虚拟屏/禁止虚拟屏”设置。
