## Why

当前远控画质协调器把窗口数量、焦点、性能采样、Presenter 丢帧和恢复滞回混在一个周期性决策函数中，实际测试中出现“全窗口45 FPS、切档后更卡”的结果。需要删除这套复杂策略，改成稳定、可解释、只在角色变化时更新的质量控制层，让视频管线不再被周期性质量请求反复打断。

## What Changes

- **BREAKING** 重写控制端 `RemoteQualityCoordinator` 的策略实现，移除跨窗口性能压力降档、FPS滞回和动态后台梯度。
- 新增单窗口和多窗口两组静态质量档案，档案包含分辨率、目标FPS、码率和后台档位。
- 单窗口可见且未最小化时固定使用单窗口前台档；多窗口只给真实焦点窗口前台档，其余可见窗口使用静态后台档。
- 最小化、隐藏、软件回退继续使用独立安全档，不参与前台质量档案竞争。
- 质量决策不再读取接收FPS、解码耗时、网络丢包或Presenter丢帧来改写请求；性能数据只保留给诊断和性能覆盖层。
- 保留现有版本化画质协议和不断流下发接口，只减少请求触发次数和决策状态数量。
- 删除旧协调器内部滞回状态、后台数量梯度和无效的固定模式适配代码，补充稳定角色切换测试。

## Capabilities

### New Capabilities

- `stable-remote-quality-control`: 为单窗口、多窗口焦点、后台、最小化和软件回退定义稳定的静态画质角色，以及仅在角色变化时下发质量请求。

### Modified Capabilities

- 无。当前 `openspec/specs` 没有已发布的远控画质能力规范。

## Impact

- `src/ui/RemoteQualityCoordinator.cpp/.h`：策略主体重写，保留调用接口以降低其它窗口代码改动范围。
- `src/ui/DeviceGrid.cpp`：继续负责焦点和窗口快照，不再依赖协调器的性能滞回状态。
- `src/ui/RemoteDesktopWindow.cpp`：继续使用现有在线质量协议，仅接收稳定决策变化。
- `tests/remote_quality_coordinator_tests.cpp`：删除旧压力/梯度断言，新增角色切换、最小化、回退和请求稳定性覆盖。
- `WJY_CODE_CHANGE_LOG.md`：记录本次重构的删除、替换和验证结果。
