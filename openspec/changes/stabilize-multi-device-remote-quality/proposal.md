## Why

FSRemote must support one controller viewing and controlling up to twenty remote devices without the process exiting, unbounded memory growth, or stale-frame latency. The current per-window WebRTC/D3D11 ownership and one-queued-task-per-texture-frame path multiply threads, GPU resources, and UI events until a large multi-device session can become unstable.

## What Changes

- Bound every remote-window video handoff to latest-frame semantics so decoder throughput cannot grow the Qt event queue or retain stale frames.
- Preserve the last presented D3D11 content during interactive window resizing while continuing to present newest frames, throttle SwapChain size changes to a bounded live cadence, and apply the exact final geometry after release.
- Route remote-window movement and resizing through the native system loop where available, suppress title-bar redraws that do not represent a visual state change, and keep snap/overlay windows out of the per-pixel composition path.
- Add deterministic viewer lifecycle and failure containment so one connection, decoder, presenter, or device failure cannot terminate unrelated remote windows or the application.
- Add controller-wide remote-quality defaults and a per-device remembered window mode selected from the remote-window title bar.
- Add a controller-only, fixed bottom-right performance overlay for each remote window that shows actual FPS, receive bitrate, pressure state, decode time, and dropped-frame ratio without entering the remote video stream.
- Keep every admitted remote session connected while applying bitrate, fixed per-mode resolution, and FPS changes live; high-quality, balanced, and smooth use explicit visible presets, while only automatic adapts FPS from correlated decoder, network, or presenter pressure.
- Reserve the currently focused visible remote window for native resolution at a fixed 60 FPS/high-priority request, lower every other visible window through the current background FPS policy, keep minimized windows on their existing profile, and keep audio as an independent per-window title-bar control.
- Remove decoder-thread access to Qt/D3D presentation objects, bound keyed-mutex waits, and write low-frequency ownership/device diagnostics under the application `data` directory.
- Coordinate twenty-device startup and resource pressure through bounded initialization, quality degradation, and recovery rather than rejecting or disconnecting healthy sessions.
- Add diagnostics and repeatable stress verification for twenty-window connect, run, minimize/restore, disconnect/reconnect, and close/reopen scenarios.

## Capabilities

### New Capabilities

- `multi-device-viewer-stability`: Bounded frame queues, isolated viewer failures, deterministic shutdown, startup pacing, GPU recovery, and twenty-window stability requirements.
- `live-remote-quality-control`: Versioned per-session quality requests and acknowledgements that change bitrate, resolution, FPS, and priority without restarting the remote session.
- `remote-quality-settings`: Persistent controller-wide quality defaults plus per-device remembered remote-window modes and title-bar status feedback.

### Modified Capabilities

None. No main OpenSpec capability currently defines controller-side multi-device stability or live viewer-quality behavior.

## Impact

- Qt viewer lifecycle and UI: `src/ui/RemoteDesktopWindow.*`, `src/ui/DeviceGrid.*`, and related title-bar/settings rendering.
- Controller settings and coordination: `src/system/AppSettings.*` and a controller-wide quality/resource coordinator.
- Stream ABI and runtime bridge: `include/FsRemoteStreamApi.h`, `src/stream/StreamRuntime.*`, and viewer status handling.
- Native WebRTC and media pipeline: `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp`, `webrtc_session.*`, decoder/encoder factories, and D3D11 presentation resources.
- Verification and diagnostics: twenty-window stress tests, bounded-queue tests, lifecycle tests, crash/minidump context, and the WJY change log.
