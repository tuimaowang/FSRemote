## 1. Bounded Frame Delivery and Lifecycle Safety

- [x] 1.1 Replace one-Qt-task-per-texture-frame delivery with a one-slot latest-texture queue and one scheduled drain per remote window.
- [x] 1.2 Add viewer-generation invalidation for texture, BGRA, status, reconnect, and close paths so stale callbacks cannot affect a replacement session.
- [x] 1.3 Add focused tests or a testable helper proving texture queue depth remains one while producers overwrite stale frames.
- [x] 1.4 Replace per-window detached stop work with managed joinable lifecycle tasks and deterministic application shutdown ordering.
- [x] 1.5 Add bounded-concurrency viewer startup pacing that immediately creates all requested windows but limits simultaneous expensive initialization.
- [x] 1.6 Preserve the last successfully presented frame across shared-texture synchronization, quality changes, transient D3D11 failures, and D3D-to-BGRA fallback transitions.

## 2. Global and Per-Window Quality Settings

- [x] 2.1 Add shared quality-mode/configuration types and persistent global defaults for target/minimum FPS, minimized FPS/resolution, and recovery behavior.
- [x] 2.2 Add a main-window Remote Control settings tab for controller-wide quality defaults and resource policy.
- [x] 2.3 Add a remote-window title-bar quality button/menu with custom/follow-global compatibility and session-only automatic as the new-window default.
- [x] 2.4 Add effective/applied quality status fields and visible title-bar feedback for high-quality lock, degradation, minimized mode, and unsupported hosts.
- [x] 2.5 Persist every title-bar quality selection by target device, restore it on reopen, and use automatic for devices without a saved mode.
- [x] 2.6 Remove the painted light outer stroke from the frameless remote window while preserving its resize hit area and rounded mask.
- [x] 2.7 Add a controller-only, mouse-transparent performance overlay fixed to the remote image bottom-right that reports actual FPS, receive bitrate, pressure, decode time, and dropped-frame ratio for both D3D11 and BGRA presentation.
- [x] 2.8 Eliminate performance-overlay flicker by caching text/style, suppressing unchanged native-window geometry and z-order work, and using stable opaque background painting.
- [x] 2.9 Restore a translucent performance-overlay background without changing the cached text/geometry or stable native-window z-order strategy.
- [x] 2.10 Repaint changed statistics through a fully cleared highly translucent ARGB surface so previous digits cannot accumulate beneath new values.
- [x] 2.11 Move the ARGB overlay to an owner-bound layered tool window so opaque text remains visible above D3D11 while its background stays highly translucent.

## 3. Live Quality Protocol and Native Application

- [x] 3.1 Extend the stable C ABI and `StreamRuntime` loader with versioned viewer-quality configuration and applied-quality status.
- [x] 3.2 Store the newest pending quality request in `ViewerInstance` and send it over the existing control data channel without reconnecting.
- [x] 3.3 Parse, validate, and acknowledge versioned quality messages per `HostClientSession` without routing them through input injection.
- [x] 3.4 Apply per-sender bitrate and FPS changes live and replace NVENC shutdown/recreate rate changes with safe reconfiguration where supported.
- [x] 3.5 Apply fixed resolution tiers per session, preferring GPU scaling and preserving the shared host capture source.

## 4. Fixed-Resolution Controller Coordination

- [x] 4.1 Add a controller-wide quality coordinator that resolves global defaults, local overrides, visibility, minimized state, viewport size, and resource priority.
- [x] 4.2 Implement fixed degradation tiers that lower visible resolution before FPS and lower minimized-window FPS immediately without disconnecting.
- [x] 4.3 Implement recovery hysteresis that restores FPS before resolution and prevents rapid tier oscillation during resize or transient load.
- [x] 4.4 Enforce a bounded software-fallback profile and retry D3D11 presentation recovery without converting all failed windows to full-resolution BGRA.
- [x] 4.5 Keep each visible quality mode at a fixed resolution, adapt sustained pressure through FPS tiers only, align high-quality bitrate with the Host ceiling, and force a clean keyframe handoff for unavoidable size changes.
- [x] 4.6 Replace receive-FPS-only degradation with cause-aware pressure that combines WebRTC receiver deltas, local presenter drops, and recent interaction while treating static desktop FPS as healthy content demand.
- [x] 4.7 Keep high quality at native/60 during healthy operation, limit ordinary correlated degradation to 45 FPS, reserve 30 FPS for severe pressure, and use faster healthy recovery with transition cooldown.
- [x] 4.8 Replace receiver-driven adaptation on fixed modes with explicit high-quality native/60, balanced 1080p/45, and smooth 720p/60 presets; keep 60/45/30 adaptation only for automatic and restore presets immediately after background safety profiles.
- [x] 4.9 (superseded by 4.11) Derive foreground ownership from the actual focused remote window, keep visible full-screen windows high performance, and force every other window to 360p/15 with low priority.
- [x] 4.10 Add an optional online viewer-audio C ABI and enforce one focused, visible, non-minimized audio owner without reconnecting video sessions.
- [x] 4.11 (policy revision) Reserve the currently focused visible window for the native/60 FPS request and keep every other window on the current background FPS policy; audio remains independent.

## 5. Shared Resources and Failure Containment

- [x] 5.1 Move WebRTC SSL and factory thread ownership to a process-wide reference-counted viewer runtime while keeping per-viewer callbacks isolated.
- [x] 5.2 Add a recoverable per-adapter D3D11 presentation device manager with per-window swap chains and device-removal recovery.
- [x] 5.3 Catch allocation/runtime failures at viewer thread boundaries and surface a viewer-scoped recoverable error without terminating other windows.
- [x] 5.4 Record bounded crash/resource diagnostics including active viewers, queue state, lifecycle state, memory, threads, applied quality, and D3D11 removal reason.
- [x] 5.5 Initial anti-flash implementation: freeze per-window SwapChain dimensions during interactive edge resizing and avoid rebuilding the rounded window mask for every mouse move. Superseded by 5.6 after user feedback about stretched static content.
- [x] 5.6 Supersede full-gesture buffer freezing with native move/resize loops, state-driven title-bar painting, stable snap/overlay composition, and throttled live SwapChain resizing that keeps newest frames visible.
- [x] 5.7 Suspend the frameless parent QWidget backing-store updates during title-bar movement while allowing the native D3D presenter to continue independently, then restore one final title-bar paint after release.
- [x] 5.8 Replace the persistent Windows Qt rounded Region with DWM native corner attributes where supported, and temporarily clear the fallback Region during movement on older systems.
- [x] 5.9 Reject new shared-texture descriptors while the pending slot is occupied and remove decoder-thread calls into the Qt/D3D presenter.
- [x] 5.10 Bound keyed-mutex waits, treat abandoned ownership as device loss, and write rate-limited presenter diagnostics under the application `data` directory.
## 6. Verification and Documentation

- [x] 6.1 Add unit coverage for quality policy precedence, fixed tiers, minimized behavior, hysteresis, malformed messages, and old-host compatibility.
- [ ] 6.2 Run one-window regressions for video, input, clipboard, audio, reconnect, close, update wait, and application exit.
- [ ] 6.3 Run twenty-window connect, eight-hour soak, minimize/restore, random disconnect/reconnect, close/reopen, and exit stress scenarios while checking bounded memory, threads, handles, and queues.
- [x] 6.4 Complete a Release x64 build and record verification results and every edited code area in `WJY_CODE_CHANGE_LOG.md`.
- [x] 6.5 Add focused tests for static-content immunity, correlated pressure, severe high-quality protection, interaction recovery, and performance-stat delta handling.
- [x] 6.6 Update focused policy coverage for fixed visible presets, automatic-only 60/45/30 transitions, common minimized behavior, and immediate safety-profile recovery without running a build.
- [ ] 6.7 Add focused coverage for largest-visible-window native/60 reservation, count/pressure-driven 15/10/5/3/1 background tiers, independent audio toggling, pending-texture rejection, and finite keyed-mutex failure handling; complete targeted tests/builds and update `WJY_CODE_CHANGE_LOG.md`.
