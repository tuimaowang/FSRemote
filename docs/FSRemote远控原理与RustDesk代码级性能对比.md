# FSRemote 远控原理与 RustDesk 代码级性能对比

> 本文回答的是“从代码结构判断远控性能”，不是实机跑分报告。
> 结论基于当前工作区源码：FSRemote 主仓库提交 `c93f0f44c2fb4f882e0b253a6228833bc4b6253d`，RustDesk 子目录提交 `807e05ea9a7e298ed2deb438195faaafce19cdd2`（本地快照标记为 `nightly-43-g807e05ea9`）。

## 1. 结论先行

| 场景 | 代码级判断 | 主要原因 |
|---|---|---|
| Windows、同一局域网、单个 Viewer、NVIDIA 硬件链路命中 | **FSRemote 更有机会低延迟、低 CPU** | 原生 DXGI/D3D11 纹理可以直接进入自定义 NVENC；Viewer 端可直接接收共享纹理并 Present。 |
| 一台 Host 同时服务多个 Viewer | **RustDesk 更节省 Host GPU/编码资源** | RustDesk 一个显示器只捕获并编码一次，再把同一个 `Arc<Message>` 广播给订阅者；FSRemote 每个 PeerConnection 都有独立 sender/编码器。 |
| 公网、NAT、IPv6、跨网络 | **RustDesk 明显更强** | RustDesk 并行尝试 TCP、UDP/KCP、IPv6，失败后走 hbbr relay；FSRemote 当前是 IPv4 直连，WebRTC 配置清空 STUN/TURN。 |
| Windows Viewer 的 GPU 呈现 | **FSRemote 正常路径更激进** | D3D11VA HEVC 解码后以 keyed-mutex 共享纹理交给 Presenter；RustDesk 也支持 VRAM/texture feature，但运行时仍保留 RGBA/软件路径。 |
| 音频 | **RustDesk 明显更优** | RustDesk 使用 Opus LowDelay；FSRemote 当前为 48 kHz、双声道、16-bit 原始 PCM TCP，理论净负载约 1.536 Mbps。 |
| 键鼠控制单向延迟 | **FSRemote 路径更短；RustDesk 适应性更强** | FSRemote 控制 DataChannel 直接承载文本命令；RustDesk 使用 protobuf 消息并复用统一 `Stream`，协议层更完整，但媒体与控制共享逻辑连接。 |
| 连接成功率与故障恢复 | **RustDesk 更强** | RustDesk 有 rendezvous、打洞、直连竞争和 relay；FSRemote 的直接 IP 设计减少中间层，但无法覆盖复杂网络。 |

一句话总结：

> **FSRemote 是“Windows 局域网原生 GPU 优化的专用远控链路”；RustDesk 是“连接成功率、跨平台和多订阅者扩展优先的通用远控链路”。**

因此不能用一个总分判断谁更快：FSRemote 更可能在“已连通的单路 Windows 高画质”中占优，RustDesk 更可能在“多客户端、弱网、公网和跨平台”中占优。

## 2. 分析边界

### 2.1 代码范围

- FSRemote：`src/`、`include/`、`third_party/uu_stream_webrtc/src/`，并参考 `tests/` 与 `third_party/uu_stream_webrtc/tests/`。
- RustDesk：`rustdesk/rustdesk/src/`，同时查看 Flutter 侧调用结构。
- RustDesk 的 `libs/hbb_common` 当前是未初始化子模块（工作区状态前缀为 `-`），因此无法从本地快照完整展开所有 protobuf schema、`Stream` 内部 framing 和部分公共网络实现。文中对这些位置只做源码能证明的判断，不把缺失部分猜成确定事实。

### 2.2 证据等级

本文把结论分为三类：

1. **代码事实**：能直接对应到函数、字段或控制流。
2. **复杂度推断**：根据每帧/每会话的操作次数推导资源随 Viewer 数量的增长趋势。
3. **待实测项**：必须在相同分辨率、编码器、GPU、网络和 Viewer 数量下做基准，源码本身不能给出毫秒级答案。

当前没有公平的 FSRemote/RustDesk 双机基准，因此本文不会声称某个项目在所有情况下具有固定 FPS、固定端到端延迟或固定 CPU 百分比。

### 2.3 规模参考（不是性能指标）

按当前工作区物理行数粗略统计：FSRemote 核心 C/C++ 约 132 个文件、50,439 行；RustDesk `src/` Rust 约 148 个文件、119,683 行，Flutter/Dart 约 124 个文件、72,059 行。这个数字只说明 RustDesk 的平台、协议和产品覆盖面更大，不能直接等价为“更慢”或“更快”。

## 3. 两套远控的底层数据流

### 3.1 FSRemote

```text
Controller
  └─ TCP/49100：准入、challenge、SDP/ICE 信令
       └─ WebRTC PeerConnection（当前不配置 STUN/TURN）
            ├─ 视频 RTP：H.265/H.264，SRTP/UDP
            │    └─ Host：共享 VDD/DXGI 捕获
            │         └─ 每个 Session 独立 Track/Sender/NVENC 编码器
            └─ 可靠有序 DataChannel：键鼠、剪贴板、质量请求、状态

Host 音频（独立于 WebRTC）
  WASAPI loopback ──> 48 kHz stereo S16 PCM ──TCP/49105──> Viewer 播放

Viewer 视频
  H.265 ──> FFmpeg D3D11VA ──> 共享 D3D11 texture/keyed mutex
       ├─正常：D3D11 Presenter / DirectComposition
       └─失败：BGRA 回读 ──> Qt 软件绘制
```

关键源码证据：

- `third_party/uu_stream_webrtc/src/webrtc_session.cpp:663-666`：Unified Plan，`rtc_config.servers.clear()`。
- `third_party/uu_stream_webrtc/src/webrtc_session.cpp:691-748`：创建控制 DataChannel、绑定 Host video source、创建 Track/Sender、设置码率。
- `third_party/uu_stream_webrtc/src/host_media_pipeline.cpp:578-613`：优先 Parsec VDD + DXGI，失败后回退 DesktopCapturer。
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp:2240-2266`：监听器持续接收客户端，并为每个客户端创建 worker/session。
- `third_party/uu_stream_webrtc/src/system_audio_stream.cpp:40-45,495-560`：独立 49105 TCP 音频服务，单客户端接收循环。

### 3.2 RustDesk

```text
被控端 ──注册/保活──> hbbs rendezvous
控制端 ──请求打洞──> hbbs
       ├─并行 TCP / UDP(KCP) / IPv6 直连
       └─失败后请求 hbbr relay

已建立的逻辑连接
  Stream + protobuf Message
       ├─VideoFrame
       ├─AudioFrame（Opus）
       ├─键鼠、剪贴板、文件、终端等
       └─其他控制消息

Host 每个显示器
  Capturer ──> Encoder（一次）──> Message/Arc
                                  ├─订阅者 A
                                  ├─订阅者 B
                                  └─订阅者 N

Viewer
  有界视频队列 ──> 独立视频解码线程 ──> RGBA 或 VRAM texture 回调
```

关键源码证据：

- `rustdesk/rustdesk/src/client.rs:371-631`：连接 rendezvous、发送 punch-hole 请求并处理直连/relay 响应。
- `rustdesk/rustdesk/src/client.rs:695-737`：并行 TCP、UDP、IPv6 连接竞争，失败后请求 relay。
- `rustdesk/rustdesk/src/rendezvous_mediator.rs:650-745`：Host 侧 TCP/UDP 打洞和 relay 决策。
- `rustdesk/rustdesk/src/server/video_service.rs:536-630`：每个显示器创建 capturer 和 encoder。
- `rustdesk/rustdesk/src/server/video_service.rs:1137-1205`：一次编码，再通过 `send_video_frame` 广播给所有订阅连接。
- `rustdesk/rustdesk/src/server/service.rs:224-243`：以 `Arc<Message>` 共享同一编码结果。

## 4. Host 侧性能：捕获、转换、编码

### 4.1 FSRemote 的正常高性能路径

FSRemote 的优势不是“用了 WebRTC”这么简单，而是把 Windows GPU 资源一路保留到自定义编码器：

1. `HostMediaPipeline` 的第一个订阅者启动 VDD/DXGI，后续订阅者复用同一个 `VideoTrackSource`。
2. `DxgiVideoSource` 产生 `D3D11NativeFrameBuffer`。
3. `NvencHevcEncoder::encodeFrameSync()` 先检查 `AsD3D11NativeFrameBuffer()`。
4. 如果没有裁剪或缩放，`D3D11FrameTransformer::transform()` 直接返回原纹理，不创建额外输出纹理。
5. 纹理直接交给 NVENC，编码后的 H.265 再进入 WebRTC RTP。

对应代码：

- `third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:263-285`
- `third_party/uu_stream_webrtc/src/d3d11_frame_transformer.cpp:9-23`
- `third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:515-530`

这条路径的性能含义是：高质量原始分辨率下，可以避免“GPU 纹理回读到 CPU 再转 I420”的全帧复制。对 4K、60 FPS 这类像素吞吐很高的场景，避免一次 CPU 回读通常比微优化协议字段更重要。

### 4.2 FSRemote 的缩放和回退代价

当目标分辨率与源纹理不一致，或裁剪区域发生变化时，`D3D11FrameTransformer` 使用 D3D11 VideoProcessor 做 GPU 内部的缩放/裁剪，并优先尝试 NV12 输出；驱动不支持时退到 BGRA 输出。

对应代码：

- `third_party/uu_stream_webrtc/src/d3d11_frame_transformer.cpp:25-65`
- `third_party/uu_stream_webrtc/src/d3d11_frame_transformer.cpp:67-189`

真正昂贵的是兼容回退：

```text
D3D11 texture
  └─CopyResource 到 staging texture
      └─Map CPU
          └─ARGBToI420
              └─I420ToARGB
                  └─UpdateSubresource 回 GPU
                      └─NVENC
```

代码中的 `D3D11NativeFrameBuffer::ToI420()` 明确执行 `CopyResource`、`Map` 和 `ARGBToI420`；NVENC 回退分支随后又执行 `I420ToARGB` 和 `UpdateSubresource`：

- `third_party/uu_stream_webrtc/src/d3d11_native_frame_buffer.cpp:84-135`
- `third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:293-325`

因此，FSRemote 的源码级性能结论必须带条件：

> **原生 DXGI + 原生 D3D11 frame buffer + NVENC 命中时，复制开销很有竞争力；一旦进入 CPU/I420 回退，性能会从“GPU 管线问题”退化为“全帧内存带宽和颜色转换问题”。**

### 4.3 RustDesk 的一次编码、多路广播

RustDesk `VideoService::run()` 为一个显示器创建一次捕获器和一次 encoder。每个循环中：

1. `c.frame(spf)` 捕获一帧。
2. `frame.to(encoder.yuvfmt(), &mut yuv, &mut mid_data)` 完成编码器需要的格式转换。
3. `handle_one_frame()` 调用 `encoder.encode_to_message()` 一次。
4. `sp.send_video_frame(msg)` 把一个 `Message` 发给所有订阅者。

对应代码：

- `rustdesk/rustdesk/src/server/video_service.rs:636-655`
- `rustdesk/rustdesk/src/server/video_service.rs:720-789`
- `rustdesk/rustdesk/src/server/video_service.rs:1158-1173`
- `rustdesk/rustdesk/src/server/service.rs:231-243`

这使 RustDesk 的 Host 侧视频编码复杂度近似为：

```text
FSRemote：capture + N × encode + N × RTP/transport
RustDesk：capture + 1 × encode + N × message/transport
```

这里的 `N` 是同时观看同一显示器的客户端数量。对单个 Viewer，FSRemote 的原生 D3D11 路径可能更快；对多个 Viewer，RustDesk 避免 N 次 NVENC session 和 N 次编码，通常更省 GPU 编码资源。

RustDesk 的代价是编码结果和质量策略更共享：不同 Viewer 的解码能力、分辨率和网络状况不容易完全独立。FSRemote 每个 Session 独立 sender/encoder，牺牲 Host 资源换来更细的 per-viewer 控制。

## 5. 多客户端扩展性：谁先撞到瓶颈

### 5.1 FSRemote 的资源增长

FSRemote 已经共享了捕获源，但没有共享编码结果：

- Host 层共享 VDD/DXGI/`VideoTrackSource`。
- 每个 `WebrtcSession` 创建自己的 PeerConnection、Track、Sender。
- 每个自定义 H.265 编码器都有自己的 worker、D3D11/NVENC 状态和码率控制。

这意味着 Viewer 数量增加时，下面几项近似线性增长：

- NVENC session 数量。
- 编码器输入/输出缓冲区。
- PeerConnection、RTP/SRTP、网络发送队列。
- 每个客户端的目标码率和出站带宽。

FSRemote 当前还有一个需要特别注意的实现偏差：

- 配置字段名是 `max_aggregate_video_kbps`，头文件注释也称其为“所有发送会话共享的视频码率预算”。
- 但 `fsremote_stream_api.cpp:2585` 当前把它直接赋给每个 Session 的 `target_bitrate_kbps`；同一字段在 `:2680` 又作为每个 Viewer 的质量请求上限使用。
- 在当前源码中没有看到按活动 Session 数量动态分摊该值的 allocator。

因此，不能把这个字段当成已经实现的全局总码率限制。多 Viewer 时，实际 Host 出站带宽和 NVENC 压力可能接近“每路都拿到该上限”，除非 WebRTC 自身拥塞控制把实际码率压低。

### 5.2 RustDesk 的资源增长

RustDesk 对同一个显示器只保留一个捕获循环和一个编码循环；每个订阅者主要增加：

- 一个连接的发送/加密/队列开销。
- 一个 `Arc<Message>` 引用和发送队列引用。
- Viewer 侧独立解码线程和呈现资源。

从 Host GPU 角度，RustDesk 在 N 路订阅下通常优于 FSRemote；从 Host 出站带宽角度，两者都仍然要发送 N 份网络数据，RustDesk 并不会神奇地把 N 路网络带宽变成 1 路。

### 5.3 RustDesk 的慢客户端背压

RustDesk 没有无限制地把帧堆到 Host 内存里。`VideoFrameController` 会记录本帧发给了哪些连接，并等待 `notify_video_frame_fetched()`；每次等待最多 300 ms，整个当前帧的等待窗口最长 3 秒：

- `rustdesk/rustdesk/src/server/video_service.rs:119-175`
- `rustdesk/rustdesk/src/server/video_service.rs:875-887`

这是一种有界背压策略，优点是不会对慢客户端无限排队；缺点是某个慢客户端可能延迟整个显示器的下一轮捕获/发送。它更像“保护 Host 稳定性”，而不是“每个订阅者完全独立的实时管线”。

### 5.4 FSRemote 的最新帧策略

FSRemote 的 Viewer 纹理回调采用单槽 latest-frame 语义：

- 槽为空时接管共享纹理并只投递一个 Qt drain task。
- 槽已占用时直接返回 `FSREMOTE_TEXTURE_FRAME_DROPPED`，生产端归还 keyed mutex，不做 BGRA 回读。
- drain 时取最新帧，旧帧不补播。

对应代码：

- `src/ui/RemoteDesktopWindow.cpp:4182-4199`
- `src/ui/RemoteDesktopWindow.cpp:4211-4328`
- `include/FsRemoteStreamApi.h` 中 `FSREMOTE_TEXTURE_FRAME_FALLBACK/ACCEPTED/DROPPED` 三态回调协议。

这对交互延迟非常重要：在渲染跟不上时，FSRemote 优先丢弃旧画面，而不是把“几百毫秒前的桌面”排队播放。它不能提升编码器的原始吞吐，但能限制排队延迟和 UI 事件增长。

## 6. Viewer 侧解码与呈现

### 6.1 FSRemote

FSRemote 的正常 Viewer 路径是：

```text
H.265 bitstream
  └─FFmpeg D3D11VA
      └─shared D3D11 texture
          └─keyed mutex handoff
              └─D3D11 Presenter / DirectComposition
```

`HevcD3d11Decoder::Decode()` 在有共享 handle 时调用 texture callback；只有 texture 路径不可用时才申请 I420/BGRA 回退缓冲：

- `third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:734-873`
- `third_party/uu_stream_webrtc/src/uu_codec_factory.cpp:968-1010`

这条路径减少了“解码 GPU → CPU BGRA → Qt QImage → GPU”往返。代价是 D3D11 device、共享 handle、keyed mutex 和窗口生命周期的耦合更复杂，设备丢失时需要单窗口恢复。

### 6.2 RustDesk

RustDesk 为每个显示器创建有界 `ArrayQueue` 和独立视频线程：

- `rustdesk/rustdesk/src/client/io_loop.rs:1320-1353`：收到 VideoFrame 后写入队列，关键帧优先，队列满时触发刷新/覆盖策略。
- `rustdesk/rustdesk/src/client/io_loop.rs:2425-2461`：创建每显示器的视频线程。
- `rustdesk/rustdesk/src/client.rs:2873-3022`：解码线程处理队列并回调 RGBA 或 texture。

RustDesk 的优势是跨平台统一抽象、队列和解码线程职责清楚；FSRemote 的优势是 Windows D3D11 纹理 ownership 更直接，正常路径可以减少 CPU 拷贝。

不能把 RustDesk 简化为“纯 CPU 解码”：源码支持 VP8/VP9/AV1/H.264/H.265，以及 Windows `vram`/硬件路径；但它同时保留更广泛的 RGBA 和软件回退，因此在不同编译 feature、GPU 驱动和平台上实际路径差异较大。

## 7. 网络连接和持续传输

### 7.1 FSRemote：建立快，但网络覆盖窄

FSRemote Viewer 接口要求一个 IPv4 字符串和端口，正式窗口固定连接 `49100`。WebRTC 配置清空 `servers`，因此当前没有从源码看到 STUN/TURN 服务器配置。

优点：

- 同网直连时，信令路径短，不经过 hbbs/hbbr。
- 视频使用 WebRTC RTP/RTCP 的拥塞控制、关键帧和丢包恢复机制。
- 控制 DataChannel 与视频 RTP 分离，视频压力通常不会直接把键鼠文本混成同一个应用层消息队列。

缺点：

- 没有 NAT 打洞、中继、IPv6 或 DNS 解析路径。
- 远端地址必须能以 IPv4 直连到 `49100`。
- 独立音频 TCP 还需要额外打开 `49105`。

### 7.2 RustDesk：连接层更重，但成功率更高

RustDesk 的连接流程在 `client.rs` 中是异步并行竞争：

1. 连接 rendezvous 服务器并发送 punch-hole 请求。
2. 根据返回地址并行尝试 TCP、UDP/KCP 和 IPv6。
3. 直连失败后请求 relay，再连接 hbbr。
4. 建立连接后通过 `Stream`/protobuf 传输视频、音频、输入和其他消息。

这会增加建连握手和控制面复杂度，但显著扩大可用网络范围。直连成功后，hbbs/hbbr 不一定承载媒体；relay 只在直连失败或策略强制时介入。

性能上的真实取舍是：

- **可达性**：RustDesk 胜出。
- **同网最短路径**：FSRemote 更简单。
- **relay 场景**：RustDesk 多一个中继跳，RTT 和带宽成本上升，但至少能连通。

## 8. 音频带宽和处理链

### FSRemote

`system_audio_stream.cpp` 将 WASAPI loopback 统一为 48 kHz、双声道、16-bit PCM，然后通过 TCP 逐帧发送。理论原始数据率：

```text
48,000 samples/s × 2 channels × 16 bits = 1,536,000 bits/s ≈ 1.536 Mbps
```

这还没有计入 TCP/应用层 framing。原始 PCM 的优点是编码延迟和实现复杂度低；缺点是带宽固定、没有 Opus 的丢包适应，也没有与 WebRTC DTLS-SRTP 共享保护。

当前实现还只允许一个音频客户端，`issue_audio_token/consume_audio_token` 的 token 消费路径在本地源码中没有实际调用；Viewer 启动音频时也没有传 token。因此，49105 不能按“已经认证并加密的音频通道”描述。

### RustDesk

`src/server/audio_service.rs` 使用 `magnum_opus::Encoder` 的 `LowDelay` 模式，并以 10 ms、48 kHz、双声道帧为基本处理单位。Opus 的实际码率由编码器和网络策略决定，通常远低于 1.536 Mbps，且能在弱网下保持可听性。

音频性能结论很明确：**RustDesk 的音频编码和传输效率高于当前 FSRemote 的原始 PCM TCP 方案。**

## 9. 输入、剪贴板和控制延迟

### FSRemote

控制 DataChannel 可靠有序，输入文本协议在 `fsremote_stream_api.cpp:1629-1731` 解析，典型命令包括：

```text
m x y buttons       绝对鼠标移动
r dx dy buttons     相对鼠标移动
d/u button x y      鼠标按钮按下/释放
w delta x y         滚轮
k vk down           键盘按键
cb <base64>         剪贴板
```

Host 侧由 `InputDispatcher` 串行化，再落到 Windows `SendInput` 或可选 FakerInputBridge/虚拟 HID。输入消息本身短、路径直接，局域网下应用层开销小。

FSRemote 仍有两个性能/体验取舍：

- 可靠有序 DataChannel 适合键鼠状态一致性，但大量输入消息可能在同一条有序队列中等待。
- 光标和剪贴板状态由 Host 25 ms 轮询，剪贴板约 350 ms 轮询；这不等同于键鼠事件延迟，但会影响状态同步新鲜度。

### RustDesk

RustDesk 使用 protobuf 的 MouseEvent、PointerDeviceEvent、KeyEvent 等消息，并在平台层接入 Enigo、rdev、uinput/RDP input 等后端。协议字段更多，跨平台能力更强，也更适合文件、终端和多种输入设备扩展。

持续媒体和输入都在统一 `Stream`/Message 体系中。由于 `hbb_common` 子模块未初始化，本文不对其底层是否为单一有序字节流、是否有独立优先级队列做更强断言；但从 `client.rs` 和 `io_loop.rs` 可以确认，视频、音频、输入都经过同一套应用消息分发框架。

因此：

- 同网单路键鼠：FSRemote 的控制面更短，理论延迟更低。
- 弱网、多平台、复杂输入：RustDesk 的协议完整性和平台适配更强。

## 10. 质量控制与队列策略

### FSRemote

FSRemote 支持通过 C ABI 和 DataChannel 在线修改 Viewer 质量：分辨率、FPS、码率、优先级都有版本化结构和确认状态。当前固定模式倾向于“请求保持可解释”，WebRTC 的拥塞控制负责实际发送结果；自动模式才根据解码、网络、presenter 压力调 FPS。

性能优点：

- 不需要重连就能更新质量。
- 高质量、平衡、流畅模式的分辨率/FPS 语义明确。
- 纹理回调采用单槽 latest-frame，避免 UI 队列积压。

性能限制：

- 每个 Viewer 仍然有自己的发送器和编码器。
- `max_aggregate_video_kbps` 当前没有真正完成跨 Session 分摊。
- 分辨率切换会触发纹理组和 NVENC 尺寸重建，短时间内需要关键帧和资源切换。

### RustDesk

RustDesk 的 `VideoQoS` 维护用户 RTT/网络延迟、FPS 和质量 ratio，并在服务级别调整显示器的编码节奏。源码注释明确写出“真实 FPS 取所有用户的最低需求/网络结果”这一类全局策略：

- `rustdesk/rustdesk/src/server/video_qos.rs:8-28`
- `rustdesk/rustdesk/src/server/video_qos.rs:183-238`
- `rustdesk/rustdesk/src/server/video_qos.rs:246-260`

这让共享编码更容易保持 Host 资源稳定，但也意味着一个慢订阅者可能影响同一显示器的全局 FPS/质量。RustDesk 通过客户端有界队列丢旧帧缓解显示端积压，却没有把共享编码变成每个客户端完全独立的质量层。

## 11. 安全与性能的关系

安全不是纯性能问题，但会影响握手成本和长期可用性。

### FSRemote 当前模型

- Viewer 用 OpenSSH 公钥签署 Host challenge，Host 检查 authorized key：`src/stream/StreamRuntime.cpp:21-65`、`fsremote_stream_api.cpp:538-759`。
- WebRTC 媒体和 DataChannel 自带 DTLS-SRTP/SCTP 保护。
- 当前 Viewer 没有对 Host 身份做等价验证，签名 challenge 也没有和 SDP/DTLS fingerprint 做绑定。
- 信令 TCP 本身未加密。
- 49105 音频是独立原始 PCM TCP，当前未发现有效 token 消费调用。

所以不能把当前 FSRemote 写成“完整双向认证、所有通道端到端绑定”。额外的信令/认证步骤很少，局域网建连路径短；但安全覆盖范围比 RustDesk 的会话密钥协商更窄。

### RustDesk 当前模型

RustDesk 在建立连接时交换签名设备 ID/公钥，并协商临时 box key/secretbox 对称密钥：

- `rustdesk/rustdesk/src/client.rs:759-835`
- `rustdesk/rustdesk/src/server.rs:196-255`

这会增加握手计算和消息，但通常不改变稳定传输阶段的主要 CPU 成本。源码仍保留签名密钥缺失/不匹配时的兼容路径，因此不能写成“任何配置下都强制安全”；从身份与会话绑定完整度看，RustDesk 更成熟。

## 12. FSRemote C API（与性能相关的入口）

头文件：`include/FsRemoteStreamApi.h`。

| API | 作用 | 性能/资源含义 |
|---|---|---|
| `fsremote_stream_start_host` | 启动默认 Host | 默认端口和默认会话上限，适合兼容旧调用。 |
| `fsremote_stream_start_host_with_config` | 按配置启动 Host | 可设置 `max_sessions`、`max_aggregate_video_kbps`、握手超时和 ownership；当前 aggregate 字段仍需注意实现偏差。 |
| `fsremote_stream_set_identity_callbacks` | 注入公钥读取、签名、授权和验签 | 把 OpenSSH 身份能力放在 Qt/应用侧，DLL 不直接依赖密钥路径。 |
| `fsremote_stream_start_viewer` | BGRA Viewer | 兼容性最好，但正常帧可能需要 CPU/BGRA 回调和 Qt 侧拷贝。 |
| `fsremote_stream_start_viewer_with_status` | BGRA + 状态回调 | 可获得建连、画面、网络和质量状态。 |
| `fsremote_stream_start_viewer_with_texture` | BGRA + 共享纹理回调 | Windows GPU 正常路径首选；回调返回 fallback/accepted/dropped 三态。 |
| `fsremote_stream_stop` | 停止并销毁 handle | 删除后 handle 不可继续使用；必须等待异步生命周期收尾。 |
| `fsremote_stream_send_input` | 发送文本输入命令 | 控制 DataChannel 的应用层入口。 |
| `fsremote_stream_set_viewer_quality` | 在线请求质量 | 不重连更新分辨率/FPS/码率/优先级。 |
| `fsremote_stream_set_viewer_audio_enabled` | 开关本地音频播放 | 只影响 Viewer 本地音频 worker，不重建视频会话。 |
| `fsremote_stream_get_viewer_quality_status` | 读取 Host 确认的质量 | 区分“请求值”和“实际应用值”。 |
| `fsremote_stream_get_viewer_performance_stats` | 读取累计接收/解码/丢帧/RTT/码率 | 可用于区分网络压力、解码压力和呈现丢帧。 |
| `fsremote_stream_is_busy` / `fsremote_stream_active_session_count` | 查询 Host 会话状态 | 供 UI 做容量和控制状态显示。 |
| `fsremote_stream_active_controller_names/details` | 查询当前控制端 | 对共享控制和诊断有用，不直接改变媒体吞吐。 |
| `fsremote_stream_last_error` | 获取最近错误 | 便于把失败定位到连接、编码、设备或协议阶段。 |

ABI 的 `struct_size + version` 约定是性能迭代的重要基础：后续可以增加统计字段而不破坏旧 Viewer；纹理三态协议则让“丢旧帧”不必触发昂贵的 BGRA 回读。

## 13. 场景化评判

### 场景 A：同一局域网，单 Viewer，1080p/60 FPS

更可能是 FSRemote 占优。

理由：

- 不需要 rendezvous/relay。
- DXGI texture → NVENC 可避免 CPU 回读。
- Viewer 端 keyed-mutex texture → Presenter，避免每帧 Qt 软件图像路径。
- 控制 DataChannel 与视频 RTP 分离。

前提是 VDD、DXGI、D3D11VA、NVENC 都命中；任一环节回退到 CPU，优势会明显收窄。

### 场景 B：同一台 Host 被 3 个 Viewer 观看

更可能是 RustDesk 占优。

FSRemote 共享捕获，但默认仍然创建 3 个 PeerConnection/encoder。RustDesk 对同一显示器只编码一次，再把同一个编码结果发送给 3 个订阅者。FSRemote 的优势只剩“每个 Viewer 可以独立质量/码率”，代价是 GPU session、显存和编码时间随 N 增长。

### 场景 C：公网、对称 NAT 或无法开放入站端口

RustDesk 占优，而且是“能否工作”的差别，不只是几个百分点。

FSRemote 当前没有 STUN/TURN/relay；RustDesk 会在直连失败后请求 hbbr。若 relay 路径成立，RustDesk 可能增加 RTT 和中继带宽，但仍然比 FSRemote 直接失败更实用。

### 场景 D：弱网、丢包、慢 Viewer

结论分裂：

- **连接和恢复**：RustDesk 更强，路径更多。
- **旧帧延迟控制**：FSRemote 的单槽 latest-frame 更直接。
- **Host 共享编码的全局稳定性**：RustDesk 更省 GPU，但 `VideoFrameController` 的等待和全局 QoS 可能让慢客户端影响同显示器其他客户端。

### 场景 E：需要声音、文件、终端和跨平台输入

RustDesk 更强。FSRemote 当前音频是原始 PCM TCP，输入协议是简化文本命令，覆盖面和带宽效率都不如 RustDesk 的 Opus + protobuf 消息体系。

## 14. 代码级改进优先级

### FSRemote 优先级

1. **先修正 aggregate bitrate 的语义**：按活动 Session 数量、最小可用码率和编码器容量真正分配预算，不能把总预算直接赋给每路。
2. **加入 ICE/STUN/TURN 或可选 relay**：保持局域网直连快速，同时覆盖公网/NAT。
3. **音频迁移到 Opus + WebRTC 音频轨道或至少加密/认证的独立通道**：这是带宽和安全的双重收益。
4. **保留原生 D3D11 路径，并把 CPU 回退指标单独暴露**：统计 `native_d3d11`、transform time、NVENC time、ToI420 次数，不要只看接收端 FPS。
5. **为相同质量的多个 Viewer 增加可选编码复用策略**：可以先做“相同分辨率/FPS/码率时共享编码结果”的实验，再处理每个 RTP 会话的序号、关键帧和拥塞控制隔离。
6. **继续保持单槽 latest-frame 和窗口级故障隔离**：这是 FSRemote 当前最有价值的交互延迟保护之一。

### RustDesk 优先级

1. **把共享编码和每订阅者质量解耦**：可考虑 SVC、simulcast 或分层编码，让慢客户端不必牵制所有订阅者。
2. **把慢客户端背压从显示器全局循环中隔离**：当前最多 3 秒的 `try_wait_next` 应逐连接丢帧/降级，而不是让捕获循环等待。
3. **对控制消息设置更明确的优先级/独立队列**：需结合 `hbb_common` 完整源码确认当前 Stream 是否已经提供足够隔离。
4. **在 Windows 高频桌面场景持续扩大硬件 texture/HEVC/AV1 命中率**：降低 `frame.to()` 和软件 RGBA 回退的比例。
5. **把 relay、直连、KCP、解码和队列指标统一暴露**：否则用户只能看到最终 FPS，难以判断瓶颈在网络还是编码/呈现。

## 15. 最终判断

从代码层面，最稳妥的排序是：

```text
单路 Windows 局域网原生 GPU 低延迟：FSRemote > RustDesk
多路观看的 Host 编码效率：RustDesk > FSRemote
公网/NAT/IPv6/跨平台连接成功率：RustDesk >> FSRemote
Windows 纹理呈现的复制开销：FSRemote > RustDesk（前提是正常 D3D11 路径）
音频带宽效率：RustDesk >> FSRemote
控制协议的简单直接：FSRemote > RustDesk
控制/文件/终端/平台能力的完整度：RustDesk >> FSRemote
```

所以，如果目标是“受控 Windows 机器、可信局域网、少量 Viewer、优先高画质和低复制开销”，FSRemote 的架构方向是合理的；如果目标是“像 RustDesk 一样在复杂网络中稳定连通，并让一台 Host 高效服务多个 Viewer”，FSRemote 当前最大的短板不是 Qt 绘制，而是 **连接层覆盖、每 Viewer 重复编码、aggregate bitrate 尚未真正分配、以及独立 PCM 音频**。

## 16. 关键源码索引

### FSRemote

- `include/FsRemoteStreamApi.h`
- `src/main.cpp`
- `src/stream/StreamRuntime.cpp`
- `src/ui/RemoteDesktopWindow.cpp`
- `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp`
- `third_party/uu_stream_webrtc/src/webrtc_session.cpp`
- `third_party/uu_stream_webrtc/src/host_media_pipeline.cpp`
- `third_party/uu_stream_webrtc/src/uu_codec_factory.cpp`
- `third_party/uu_stream_webrtc/src/d3d11_frame_transformer.cpp`
- `third_party/uu_stream_webrtc/src/d3d11_native_frame_buffer.cpp`
- `third_party/uu_stream_webrtc/src/system_audio_stream.cpp`

### RustDesk

- `rustdesk/rustdesk/src/client.rs`
- `rustdesk/rustdesk/src/rendezvous_mediator.rs`
- `rustdesk/rustdesk/src/server/video_service.rs`
- `rustdesk/rustdesk/src/server/video_qos.rs`
- `rustdesk/rustdesk/src/server/audio_service.rs`
- `rustdesk/rustdesk/src/server/service.rs`
- `rustdesk/rustdesk/src/client/io_loop.rs`
- `rustdesk/rustdesk/src/server.rs`

## 17. 验证记录

- 本地 FSRemote 构建目录登记了 28 个 CTest；本次只验证了与远控链路直接相关的 9 个轻量目标，全部通过。
- 这些测试覆盖协议、Host session manager、共享媒体管线、最新帧槽、输入状态和窗口合成策略；它们不等于双机吞吐、GPU 占用或公网延迟 benchmark。
- RustDesk 当前没有可比较的本地构建产物，且 `libs/hbb_common` 子模块未初始化；因此未做二进制包体或同场景实机对比。

