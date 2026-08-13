# FakerInputBridge

FakerInputBridge 是一个与 FSRemote 完全独立的 Windows x64 本机桥接程序。它把受限的本机命名管道消息转换成 FakerInput HID 报告。

## 安全设计

- 不监听 TCP 或 UDP，不接受网络连接；
- 命名管道使用 `PIPE_REJECT_REMOTE_CLIENTS`；
- 管道 ACL 仅允许启动服务端的 Windows 用户和 SYSTEM；
- 协议只接受固定长度的键盘、相对鼠标、绝对鼠标和释放报告；
- 不接受脚本、命令行或任意可执行内容；
- 客户端断开、Ctrl+C 或服务端退出时自动释放全部键鼠状态；
- 不修改 FakerInput 驱动、VID/PID、设备名称或签名；
- 不包含反作弊绕过逻辑。

## 构建

```powershell
cd third_party\faker_input_bridge
.\build.ps1
```

输出：

```text
build\Release\FakerInputBridge.exe
```

Release 使用静态 MSVC 运行库（`/MT`）。

## 被控端测试

把 `FakerInputBridge.exe` 复制到被控端。打开第一个 PowerShell 窗口：

```powershell
.\FakerInputBridge.exe --server
```

保持窗口运行，再打开第二个 PowerShell 窗口：

```powershell
.\FakerInputBridge.exe --ping
```

预期看到 `Driver ready: yes`。普通桌面先确认后，可使用下面的命令验证完整桥接点击链路：

```powershell
.\FakerInputBridge.exe --click-test
```

命令倒计时 5 秒，然后在鼠标当前位置发送一次左键按下/抬起，不移动鼠标。必须避开购买、删除、交易、确认等不可撤销操作。

紧急释放：

```powershell
.\FakerInputBridge.exe --release-all
```

停止服务端：在服务端窗口按 `Ctrl+C`。

## 本机协议 v1

协议定义位于 `src/bridge_protocol.h`。消息头固定为 16 字节，所有载荷固定长度且在服务端严格校验。当前命令支持：

- `ping`
- `get_status`
- `keyboard`
- `relative_mouse`
- `absolute_mouse`
- `release_all`

后续与 FSRemote 联动时只需复用 `bridge_client.h/.cpp`，驱动访问仍保留在独立服务端进程中。

客户端应保持管道连接，不要为每个输入报告重新连接。服务端会把客户端断开视为故障保护信号，并立即发送 `release_all`，以避免断线后按键或鼠标按钮保持按下。

输入载荷采用“当前状态快照”语义：

- 键盘包含 1 字节 USB HID modifier 和最多 6 个 USB HID usage；
- 相对鼠标按钮低 5 位对应左、右、中、后退、前进，位移为有符号 16 位；
- 垂直/水平滚轮为有符号 8 位；
- 绝对坐标范围为 `0..32767`；
- 每个请求必须读取并验证同序号响应后再发送下一个请求。
