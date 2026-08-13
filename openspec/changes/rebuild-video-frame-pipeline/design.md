## Context

The current viewer path crosses several ownership domains: WebRTC decode callbacks produce D3D11 shared textures, `RemoteDesktopWindow` stores pending descriptors and schedules Qt work, `D3D11FramePresenter` opens and synchronizes textures, and the quality coordinator changes remote profiles while the same UI thread presents frames. The result is a large, partially overlapping implementation with per-frame GPU work on the Qt thread, ambiguous frame lifetime, and insufficient correlation between decoder, queue, renderer, and window state.

The replacement is intentionally scoped to the application-layer video receive/presentation module. Existing signaling, WebRTC session lifecycle, input, clipboard, audio, and the underlying FFmpeg/H265 decode capability remain available. The implementation target is this repository; any external comparison checkout remains read-only reference context.

## Goals / Non-Goals

**Goals:**

- Replace the old texture callback, Qt drain, presenter, and BGRA presentation contract with an explicit native-frame pipeline.
- Keep decoded frame ownership valid until the render worker finishes or drops the frame.
- Remove per-frame D3D11 work from the Qt UI thread.
- Make one render worker per GPU adapter responsible for D3D11 context, imported textures, processor views, SwapChains, and Present.
- Bound every session to one in-flight frame and one pending newest frame.
- Reserve a focused window profile and use a fixed visible-background profile of 1280x720 at 30 FPS.
- Make emergency degradation, recovery hysteresis, device loss, resize, reconnect, and fallback deterministic.
- Produce structured, asynchronous, frame-correlated diagnostics without disk IO in the decode or render hot path.
- Keep the final source tree free of the old application video module after migration verification.

**Non-Goals:**

- Rewriting the WebRTC signaling protocol, remote input protocol, clipboard protocol, or audio transport.
- Replacing FFmpeg/H265 decode before the native-frame output contract is proven.
- Making every visible window high-quality simultaneously under an unbounded GPU or network budget.
- Adding a general-purpose graphics abstraction for non-Windows platforms.
- Persisting per-frame trace data indefinitely.

## Decisions

### 1. Clean-room application video boundary

The old application-facing texture callback and presenter path is treated as a replaceable module. New code owns the interface from decoded native frame to visible window surface. During migration a compile-time/runtime switch may route a test session to the new path, but the switch and the old implementation are removed after acceptance; no permanent dual pipeline remains.

Alternative: incrementally patch `D3D11FramePresenter` and `LatestTextureFrameSlot`. Rejected because the current ownership, Qt scheduling, and fallback responsibilities are already interleaved, making it difficult to prove that old synchronization behavior is gone.

### 2. Native frame envelope and lease

The decoder produces a `NativeVideoFrame` containing session identity, viewer generation, frame ID, RTP/render timestamps, source dimensions/format, the native D3D11 texture reference, and a lease object. The lease is the only authority that returns producer ownership after render completion or an explicit drop. A raw shared handle is never queued without a lifetime object.

The decoder attempts to deliver the frame through the WebRTC decoded-frame path. A native sink/adapter consumes the native buffer without `ToI420`; software conversion is an explicit compatibility fallback and is instrumented separately.

Alternative: keep a callback that returns an integer accepted/dropped status and let the decoder manage all ownership. Rejected because replacing an already accepted pending frame then requires hidden cross-thread keyed-mutex recovery and makes stale-frame disposal difficult to reason about.

### 3. One RenderWorker per GPU adapter

Each active adapter has one `RenderWorker` thread. The worker owns its D3D11 immediate context, presentation device, imported resource cache, VideoProcessor state, per-window SwapChains, and Present calls. `RemoteDesktopWindow` owns only a UI façade and sends surface-state commands to the worker.

The worker uses a bounded command queue and an event/semaphore wakeup. It selects eligible windows by deadline and priority rather than running one Qt timer per window. The focused window receives the highest weight; visible background windows use fair round-robin service; minimized windows receive only their safety cadence.

Alternative: one D3D11 worker thread per window. Rejected because it multiplies contexts, GPU resources, and synchronization points and makes adapter-wide fairness harder to enforce.

### 4. Latest-frame handoff with bounded ownership

Each session has one `FrameInbox` with an in-flight frame and one pending frame. A newer pending frame replaces an older pending frame by releasing the older lease through the same explicit ownership mechanism. The decoder never waits for the UI or render worker; if the inbox or output pool cannot accept a frame, the frame is dropped with a typed reason and the producer lease is returned.

The worker never renders a frame whose viewer generation, session state, or surface identity is stale. The last successfully presented surface remains visible when a new frame fails.

### 5. Resource reuse and bounded synchronization

Imported textures and D3D11 processor views are cached by session/generation/resource identity and recreated only for a new resource group, format, source size, device generation, or SwapChain back-buffer change. The hot path uses a non-blocking or very short synchronization attempt; a busy resource drops the current frame instead of blocking the UI or starving other windows.

The primary synchronization implementation uses a cross-device fence when available; a keyed-mutex adapter remains as a bounded fallback. A waitable SwapChain/frame-latency limit prevents the presentation queue from accumulating stale frames.

### 6. Fixed normal profiles with emergency pressure handling

Normal profile selection is separate from render mechanics:

- one focused visible window: the selected high-quality/native profile, local target 60 FPS;
- visible background windows: fixed 1280x720, target 30 FPS;
- minimized windows: 640x360, target 1-5 FPS;
- emergency pressure: background first moves through 15/10/5/3/1 FPS or a lower resolution, then focused resolution, and only finally focused FPS.

Role selection uses connected state, visibility, minimized/cloaked state, actual focus, and visible-window count. Pressure enters only after at least two indicators remain true for approximately one second (worker backlog, present drops, frame age, synchronization busy ratio, render utilization, decode queue age, or device/present failure). Recovery requires all normal indicators for approximately three seconds and restores one step at a time.

Remote encoder profile changes are debounced on focus transitions. Local presentation priority changes immediately; remote resolution/FPS changes are not sent repeatedly during rapid focus changes.

### 7. Asynchronous structured diagnostics

All video-pipeline events go through one process-level asynchronous logger. Every event carries run ID, session ID, window ID, viewer generation, frame ID where applicable, render job ID, thread ID, adapter, monotonic timestamp, event name, result, reason, and typed fields.

Normal operation writes lifecycle/error events and one periodic aggregate per active session. Frame-level events stay in a bounded in-memory ring. The ring and the next few seconds are dumped when p95 frame age, drop ratio, worker backlog, synchronization failure, Present failure, device loss, or fallback thresholds are crossed. Files rotate under `data/video_pipeline/` with a bounded total size.

Alternative: retain the existing per-component append-to-file helpers. Rejected because they serialize unrelated threads on file locks, lack common correlation IDs, and cannot reconstruct a frame's cross-thread timeline.

### 8. Explicit lifecycle and device recovery

Session close first stops new frame admission, then releases pending/in-flight leases through the render worker, then stops decode resources, and finally destroys the UI façade. Device removal invalidates only the affected adapter resources, keeps unrelated sessions alive, and reimports surfaces after the worker rebuilds its presentation device.

## Risks / Trade-offs

- [The WebRTC receive path may force a software conversion for a generic sink] → Add a native-sink spike before removing the old path; if required, place the native adapter at the last point before any `ToI420` conversion and keep conversion counters visible.
- [Fence support differs across older Windows/GPU combinations] → Detect capability once per adapter, use bounded keyed-mutex fallback, and log the selected synchronization mode.
- [A single adapter worker can become a bottleneck with many 720p/30 background windows] → Use weighted deadlines, drop stale frames, cap background profiles, and expose worker utilization and per-window service time.
- [Remote profile changes can trigger keyframe and bandwidth bursts] → Debounce role changes, apply local cadence immediately, and require sustained role changes before requesting a remote resolution change.
- [Replacing a pending frame can fail to return its producer lease] → Make the lease object the sole release authority and add tests for replacement, cancellation, stale generation, and shutdown races.
- [Diagnostic capture can create disk pressure] → Use an in-memory ring, anomaly-triggered dumps, asynchronous flushing, rotation, and a total-size cap.
- [The old module is currently untracked user code in the target worktree] → Never reset or clean the worktree; edit only the files named by this change and preserve unrelated untracked files.

## Migration Plan

1. Capture a baseline with the new diagnostic event schema implemented as testable helpers; do not change behavior yet.
2. Add native frame envelope/lease, FrameInbox, and RenderWorker unit tests with synthetic frame descriptors.
3. Add the native WebRTC sink adapter and a per-session new-pipeline switch; verify no unexpected `ToI420` calls on the native path.
4. Implement resource import/view caches, waitable SwapChains, bounded sync, and one-window presentation.
5. Add multi-window scheduling, fixed background 720p/30, minimized profile, emergency pressure handling, and recovery hysteresis.
6. Integrate structured logger, periodic summaries, trace ring, anomaly dumps, device-loss snapshots, and WJY change-log entries.
7. Run single-window, resize, reconnect, device-loss, and multi-window stress verification against the acceptance criteria.
8. Remove the old texture callback, Qt drain, presenter, texture slot, and old fallback contract; remove the migration switch.

Rollback during steps 2-7 is limited to routing the test switch back to the existing path. After step 8 the old module is intentionally gone; rollback is through source control rather than a permanent runtime branch.

## Open Questions

- Which exact WebRTC sink insertion point preserves native buffers for this bundled WebRTC revision without invoking `ToI420`? This is a required early spike, not a reason to continue the old design.
- What is the minimum supported Windows/D3D feature level for the target product? The worker will still provide a keyed-mutex fallback, but the accepted feature matrix must be recorded before release.
- Should minimized sessions continue receiving 1 FPS or be paused after a long idle interval? The initial implementation uses 1 FPS for fast restore and avoids reconnect churn.
