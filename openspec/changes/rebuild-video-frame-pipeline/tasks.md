## 1. Baseline and module boundary

- [x] 1.1 Capture the current `Fsremote` video path, thread ownership, frame counters, and build file references without modifying `Fsremote2`.
- [x] 1.2 Add a temporary baseline diagnostic snapshot/test helper for one-window and multi-window frame age, presented FPS, drops, and UI-thread presentation calls.
- [x] 1.3 Enumerate the old application video contract and mark the final removal set: texture callback, Qt drain, presenter, texture slot, and old fallback bridge.

## 2. Native frame contract

- [x] 2.1 Define the native frame envelope with session ID, viewer generation, frame ID, timestamps, source metadata, native texture reference, and release lease.
- [x] 2.2 Implement lease transitions for accept, pending replacement, explicit drop, render completion, stale generation, and shutdown.
- [x] 2.3 Add focused unit tests for lease ownership, double release prevention, generation invalidation, and pending-frame replacement.
- [x] 2.4 Implement the bounded per-session `FrameInbox` with one in-flight frame and one newest pending frame.
- [x] 2.5 Add unit tests proving the inbox never grows beyond its bounded ownership and always releases replaced frames.

## 3. WebRTC/native decode integration

- [x] 3.1 Add the native decoded-frame adapter at the correct WebRTC receive boundary and preserve RTP/render timestamps.
- [x] 3.2 Verify native frames reach the new sink without unexpected `ToI420` conversion; record native/software path counters.
- [x] 3.3 Replace the old application-facing texture callback result contract with the native frame submission interface.
- [x] 3.4 Keep software conversion as an explicit fallback with typed reasons and session-scoped recovery behavior.
- [x] 3.5 Add a focused integration test or diagnostic probe for decoder generation changes, stale callbacks, and output-texture retirement.

## 4. RenderWorker and D3D11 surfaces

- [x] 4.1 Add the adapter-level RenderWorker command queue, wakeup mechanism, lifecycle state, and session registry.
- [x] 4.2 Add RenderSurface state commands for HWND/visual identity, geometry, visibility, focus, minimized state, and profile.
- [x] 4.3 Move D3D11 presentation device/context ownership into RenderWorker and forbid per-frame UI-thread GPU calls.
- [x] 4.4 Implement the fixed three-slot shared-texture import cache keyed by session generation, resource identity, format, and source dimensions.
- [x] 4.5 Implement reusable VideoProcessor input/output views and SwapChain back-buffer resources.
- [ ] 4.6 Implement bounded Fence synchronization with keyed-mutex fallback and typed synchronization outcomes.
- [x] 4.7 Add waitable SwapChain/frame-latency control and latest-frame Present behavior.
- [x] 4.8 Add RenderWorker unit tests with fake surfaces for command ordering, deadline handling, stale session rejection, and last-frame retention.

## 5. Multi-window scheduling and profiles

- [x] 5.1 Add focused/background/minimized role resolution from connection, visibility, focus, and window-count state.
- [x] 5.2 Add the fixed profiles: focused selected quality/60 FPS, visible background 1280x720/30 FPS, minimized 640x360/1-5 FPS.
- [x] 5.3 Add weighted deadline scheduling that protects the focused window while fairly servicing visible background windows.
- [x] 5.4 Add pressure indicators, two-signal entry hold time, emergency background tiers, and recovery hysteresis.
- [x] 5.5 Debounce remote encoder profile changes on focus transitions while applying local presentation priority immediately.
- [x] 5.6 Add tests for one-window, two-to-six-window, seven-plus-window, focus switching, minimized restore, overload, and recovery behavior.

## 6. Structured video diagnostics

- [x] 6.1 Add the asynchronous structured video logger and common correlation fields for run, session, window, generation, frame, job, thread, and adapter.
- [x] 6.2 Add typed frame-stage events for decode, queue, synchronization, processing, Present, release, profile, fallback, and recovery decisions.
- [x] 6.3 Add periodic per-session and per-adapter summaries with frame-age, timing, drops, queue, worker, and cache metrics.
- [x] 6.4 Add an in-memory frame trace ring and anomaly-triggered before/after dumps.
- [x] 6.5 Add bounded JSONL/text rotation under `data/video_pipeline/` and make logger file failure non-fatal.
- [x] 6.6 Add diagnostic snapshot export for a selected window, adapter, or all active sessions.
- [x] 6.7 Add tests for correlation, rate limiting, anomaly capture, rotation, and no synchronous file IO on decode/render threads.

## 7. UI/lifecycle integration

- [ ] 7.1 Reduce `RemoteDesktopWindow` to a video-surface façade that sends state commands and consumes low-frequency telemetry.
- [ ] 7.2 Remove the per-frame Qt timer/drain path and all decoder-thread calls into QWidget or presenter objects.
- [x] 7.3 Integrate resize, focus, minimize, reconnect, close, and title-bar state with RenderSurface commands.
- [ ] 7.4 Implement deterministic session teardown and adapter device-loss recovery without terminating unrelated windows.
- [ ] 7.5 Preserve input, clipboard, audio, connection status, and software fallback UI behavior around the new pipeline.

## 8. Verification and old-module removal

- [ ] 8.1 Run one-window 60 FPS, resize, reconnect, software fallback, and device-loss verification with diagnostic snapshots.
- [ ] 8.2 Run multi-window visible-background 720p/30 verification, focus rotation, minimize/restore, and sustained pressure tests.
- [ ] 8.3 Verify UI thread no longer performs per-frame D3D11 work and that frame age/queue bounds remain within the specification.
- [ ] 8.4 Remove the temporary migration switch and delete the old texture callback, Qt drain, presenter, texture slot, and obsolete fallback contract.
- [ ] 8.5 Update CMake/source inventories and add focused tests to the default test/build targets.
- [x] 8.6 Append the complete WJY code change record with before/after snippets, final line numbers, and verification results.
