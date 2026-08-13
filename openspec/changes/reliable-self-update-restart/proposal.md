## Why

FSRemote 当前在主程序仍运行时直接覆盖 EXE 和已加载 DLL，容易因 Windows 文件占用导致更新失败或产生新旧文件混合。需要把更新改为可验证的暂存下载、主程序退出后的事务替换，并在成功或回滚后自动重启。

## What Changes

- 新增轻量、无 Qt 运行时依赖的独立 `FSRemoteUpdater.exe`，从临时副本运行。
- 主程序先将共享目录运行载荷暂存到本机并验证完整性，再生成版本化更新任务。
- 更新器等待主程序退出，备份现有安装、替换文件、验证结果，并在失败时回滚。
- 更新完成后自动重启 FSRemote，并向新进程传递更新结果。
- 将更新判断改为严格的语义版本升序比较，避免误降级。
- 将手动检查与自动检查开关解耦，并抑制同一版本的重复自动提示。
- 发布时生成完整载荷后最后提交版本标记，保持现有局域网共享目录兼容。

## Capabilities

### New Capabilities
- `reliable-self-update`: 定义共享目录发布、暂存校验、退出后事务替换、失败回滚和自动重启行为。

### Modified Capabilities

无。

## Impact

- 主要影响 `src/system/UpdateService.*`、`src/main.cpp`、设置页更新入口及根 `CMakeLists.txt`。
- 新增独立 Windows 更新器目标和更新任务文件格式。
- 继续使用现有 UNC 共享目录，不引入 HTTP 服务或 Qt 更新器依赖。
