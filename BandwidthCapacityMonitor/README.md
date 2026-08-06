# BandwidthCapacityMonitor

这是一个独立的 Windows 被动监控程序，用于在 FSRemote 正在远控多台设备时观察控制电脑的局域网带宽占用和本机资源压力。它不会模拟设备、不会发送压测流量，也不会修改 FSRemote。

## 它能显示什么

- 每张活动物理网卡当前接收和发送速率（Mbps）。
- Windows 报告的接收/发送协商链路速率。
- 当前利用率和“理论余量 = 容量 - 当前速率”。
- 每个采样周期新增的网卡丢弃包和错误计数。
- 整机 CPU，以及 `FSRemote.exe` 的 CPU、工作集、私有内存和线程数。
- 可选 UTF-8 CSV 记录，便于卡死或卡顿后回看最后几秒数据。

## 余量数字的边界

默认容量来自 Windows 网卡协商速率。例如千兆有线网卡会显示约 `1000 Mbps`，当前接收 `420 Mbps` 时理论接收余量约为 `580 Mbps`。

这个数字只代表控制电脑本机网卡口径，无法被动获知以下环节的真实上限：

- Wi-Fi 空口共享和信号质量。
- 交换机背板、上联口、VLAN 或路由器限速。
- 某一台被控设备自己的百兆/千兆端口。
- 网卡驱动、UDP 接收队列、解码、GPU 呈现等非带宽瓶颈。

如果已经通过可靠方法测得这条链路实际最多只能稳定传输 `760 Mbps`，运行时加入 `--capacity-mbps 760`，余量和利用率会按 `760 Mbps` 计算，而不是按协商速率计算。

## 构建

需要 Windows、CMake 3.24 或更高版本，以及 Visual Studio 2022 C++ 工具链。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

生成的程序位于：

```text
build/Release/BandwidthCapacityMonitor.exe
```

程序静态链接 MSVC 运行库，不依赖 Qt，也不需要放在 FSRemote 目录中。

## 使用

直接运行会每秒显示全部活动物理网卡，按 `Ctrl+C` 停止：

```powershell
.\build\Release\BandwidthCapacityMonitor.exe
```

先查看网卡索引：

```powershell
.\build\Release\BandwidthCapacityMonitor.exe --list
```

只观察指定网卡，并把结果保存到 CSV：

```powershell
.\build\Release\BandwidthCapacityMonitor.exe --interface 12 --csv monitor.csv
```

使用 `940 Mbps` 的实测有效容量代替千兆协商速率：

```powershell
.\build\Release\BandwidthCapacityMonitor.exe --interface 12 --capacity-mbps 940 --csv monitor.csv
```

监控两分钟后自动退出：

```powershell
.\build\Release\BandwidthCapacityMonitor.exe --duration 120
```

## 怎样判断是不是带宽不足

1. 启动监控器和 CSV 记录。
2. 正常使用 FSRemote，逐步打开到会发生卡顿的设备数量。
3. 在卡顿时观察接收方向，因为多台被控设备的视频主要进入控制电脑。
4. 结合以下现象判断：

| 现象 | 更可能的原因 |
| --- | --- |
| 接收利用率持续高于 85%，理论余量很小，网卡丢弃计数增长 | 本机链路或接收队列接近瓶颈，带宽怀疑成立 |
| 接收利用率不高，但 FSRemote 或整机 CPU 接近满载 | 解码、线程调度或 CPU 压力，不像纯带宽不足 |
| 带宽和 CPU 都不高，但画面仍卡或程序无响应 | 更应检查 GPU、D3D11 呈现、锁竞争或程序内部队列 |
| Wi-Fi 协商速率很高，但实际很早就卡顿 | 协商速率高估了共享空口的稳定吞吐，建议填入实测有效容量或改用有线网络 |

风险文字采用以下阈值：低于 `70%` 为正常，`70%-85%` 为关注，`85%-95%` 为高风险，`95%` 以上为接近饱和。瞬时峰值不等于故障，建议重点看卡顿前后是否持续数秒，以及丢弃/错误计数是否同步增长。
