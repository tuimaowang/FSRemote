## Why

Qt Creator 当前默认构建 `all` 时会同时生成主程序、更新器和大量测试可执行文件，导致普通代码修改后的等待时间明显增加。日常开发应默认只构建产品目标，同时保留按需启用和单独构建测试的能力。

## What Changes

- 新增统一的 `FSREMOTE_BUILD_TESTS` CMake 总开关，默认关闭全部项目测试目标。
- 现有各测试分类开关继续保留，只有总开关和对应分类开关同时开启时才创建测试目标。
- 所有测试 `add_executable` 目标标记为 `EXCLUDE_FROM_ALL`，即使启用测试，Qt Creator 的普通 `all` 构建也不再自动编译测试程序。
- 用户需要验证时仍可在 Qt Creator 中选择具体测试目标单独构建，或显式构建测试目标。
- 不删除测试源码，不改变 `FSRemote`、`FSRemoteUpdater`、`fsremote_stream` 或 WebRTC 生产库的构建依赖。

## Capabilities

### New Capabilities
- `fast-default-build`: 定义日常默认构建排除测试程序、测试按需启用及单独构建的行为。

### Modified Capabilities

无。

## Impact

- 仅修改根目录 `CMakeLists.txt` 和对应 OpenSpec 文件。
- 现有 Qt Creator 构建目录重新运行 CMake 后会新增关闭状态的总开关；即使旧分类缓存仍为 ON，也不会生成测试目标。
- 不修改业务代码、设备数据、网络协议、运行时功能或测试源码。
