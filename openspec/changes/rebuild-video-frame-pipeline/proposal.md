## Why

The current application-layer remote-video path mixes WebRTC decode callbacks, Qt event delivery, D3D11 shared-texture synchronization, VideoProcessor work, SwapChain presentation, fallback rendering, and quality policy across several large classes. That coupling makes single-window stalls and multi-window contention difficult to diagnose and prevents the focused window from receiving a stable rendering budget.

This change replaces the current video receive/presentation module with an explicit native-frame pipeline, GPU-owned render workers, deterministic multi-window profiles, and structured diagnostics while preserving the existing remote-control session, input, clipboard, audio, and signaling behavior.

## What Changes

- **BREAKING** Remove the current application-facing texture callback, Qt timer-driven texture drain, `D3D11FramePresenter`, `LatestTextureFrameSlot`, and the old BGRA/shared-texture presentation contract.
- Add a native video-frame envelope with timestamps, viewer generations, frame IDs, and an explicit texture lifetime lease.
- Route decoded native frames through a bounded per-session latest-frame handoff into a dedicated GPU render worker.
- Use one render worker per active GPU adapter; keep D3D11 context, VideoProcessor, SwapChain, and presentation views owned by that worker.
- Cache imported shared textures and presentation views; keep the UI thread out of per-frame GPU work.
- Add deterministic profiles: focused windows use the selected high-quality profile, visible background windows use fixed 1280x720/30 FPS, and minimized windows use a low-rate safety profile.
- Add pressure-based emergency degradation, recovery hysteresis, and focused-window reservation without making normal background policy oscillate.
- Add one asynchronous structured video-pipeline logger with frame correlation, periodic summaries, in-memory trace buffering, anomaly dumps, and bounded log rotation.
- Preserve the last successful frame across transient synchronization, resize, reconnect, and presentation failures; isolate device loss and fallback to the affected session/adapter.
- Keep the underlying WebRTC signaling/session, input, clipboard, audio, and FFmpeg decode capability outside the replacement boundary.

## Capabilities

### New Capabilities

- `remote-video-pipeline`: Native-frame ownership, bounded latest-frame delivery, render-worker presentation, multi-window scheduling, fixed quality profiles, recovery, and lifecycle requirements.
- `remote-video-diagnostics`: Structured frame tracing, render-worker telemetry, pressure events, anomaly capture, log rotation, and diagnostic snapshot requirements.

### Modified Capabilities

<!-- No existing main specs are present; this change introduces the two capabilities above. -->

## Impact

- Application UI/video integration: `src/ui/RemoteDesktopWindow.*`, current D3D11 presenter and texture-slot files, and new render-worker/surface/diagnostic files.
- Native WebRTC media bridge: `third_party/uu_stream_webrtc/src/uu_codec_factory.*`, native frame ownership, decoder output callbacks, and stream API integration.
- D3D11/DXGI resources: shared-texture import, synchronization, VideoProcessor, SwapChain, adapter/device recovery, and frame-latency control.
- Build and tests: `CMakeLists.txt`, focused frame-queue/render-worker/diagnostic tests, and the repository `WJY_CODE_CHANGE_LOG.md`.
- The implementation is scoped to this repository; any external comparison checkout remains read-only reference context.
