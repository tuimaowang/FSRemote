## Context

The controller currently creates one `RemoteDesktopWindow` and one native `ViewerInstance` for each target device. Every viewer owns a WebRTC runtime, connection worker, decoder, optional audio worker, and D3D11 presentation path. BGRA frames already use a one-frame pending slot, but texture frames post one queued Qt functor per decoded frame. At twenty 60 FPS windows this can post roughly 1,200 UI tasks per second while also creating dozens of WebRTC threads and D3D11 devices.

The product requirement is not to cap simultaneous devices. Twenty windows must remain connected and controllable; bounded queues and fallbacks protect the process, while automatic mode may reduce FPS under pressure instead of terminating the application. Each visible quality mode keeps a fixed clarity tier so encoder dimensions do not oscillate, while minimized and software-fallback profiles use separate bounded resolution/FPS pairs.

## Goals / Non-Goals

**Goals:**

- Keep controller memory, frame queues, and lifecycle work bounded for twenty simultaneous viewers.
- Isolate connection, decoder, presenter, and device failures to the affected viewer while preserving other sessions.
- Apply controller-wide quality defaults and per-device remembered window modes without reconnecting.
- Apply explicit visible resolution/FPS presets, allow bounded FPS adaptation only in automatic mode, and use a common lower background profile while minimized.
- Pace expensive connection initialization without limiting the final number of connected devices.
- Produce diagnostics and stress verification that make future multi-window failures actionable.

**Non-Goals:**

- Guarantee twenty native-resolution 60 FPS streams on hardware or networks that cannot sustain them.
- Disconnect, pause, or reject healthy viewers merely to enforce a local quality budget.
- Implement single-encode RTP fan-out for multiple controllers of one host in this change.
- Synchronize per-device quality preferences across different controller computers.

## Decisions

### 1. Use a single latest-texture slot per window

`RemoteDesktopWindow` stores one pending texture descriptor protected by its existing frame mutex plus a `presentationScheduled` flag. A decoder callback accepts a texture only while that pending slot is empty and posts a Qt task only when no task is already scheduled. If a pending keyed-mutex texture is still waiting for consumption, the new frame is reported as a controlled drop and its producer slot is immediately reclaimed without BGRA readback.

The descriptor carries a viewer generation token. Closing or reconnecting increments the generation, clears the slot, and prevents tasks from an older viewer from presenting into the new session. Qt receiver-context cancellation and `QPointer` checks remain defensive protections, but correctness does not depend on an unbounded event queue.

Shared texture ownership uses a three-slot keyed-mutex pool. The decoder acquires producer key `0`, writes the BGRA texture, and releases consumer key `1` only when Qt accepts the descriptor. The presenter acquires key `1` before reading and releases key `0` after its GPU commands are submitted. Resolution changes retain old texture groups until every queued consumer key has returned, preventing raw shared handles from becoming stale during live quality transitions.

Alternative considered: retain one queued functor per frame and rely on Qt to discard tasks after object destruction. Rejected because it bounds object lifetime but not queue depth, memory, or latency while the object remains alive.

The texture path uses commit-on-success presentation semantics. The decoder submits its producer commands and transfers keyed-mutex ownership before the presenter reads the shared handle. The presenter opens a new handle into a candidate texture and only replaces the last successful texture after `VideoProcessorBlt` and `Present` both succeed. A transient failure drops that frame while retaining the existing swap-chain content; repeated failures paint the first software fallback frame behind the still-visible texture child before hiding it. This prevents quality-mode or resolution transitions from exposing the window's valid black background as an accidental flash.

Interactive window resizing preserves the last successful SwapChain content but no longer freezes one buffer size for the full gesture. Incoming frames keep rendering, while `ResizeBuffers` is permitted at a bounded cadence of roughly 30 updates per second and is immediately followed by `VideoProcessorBlt` and `Present`. Temporary sizes between those commits may use short DWM scaling, but the video does not remain a stretched static surface until release. The rounded top-level mask is removed once at resize start and restored once at the final geometry.

### 2. Separate connection admission pacing from active-session capacity

A controller-side startup coordinator limits concurrent expensive viewer initializations, initially to four, while immediately creating all requested windows. A completed or failed initialization releases a slot for the next window. Once connected, viewers are not counted against the initialization limit.

Alternative considered: cap the number of open windows. Rejected because simultaneous multi-device control is a core requirement.

### 3. Make quality policy layered and live

Global settings and the last selected mode for each target device are persisted by `AppSettings`. A device without a saved mode starts in automatic rather than implicitly following the global mode; later windows for that device restore its most recent selection. The title-bar menu exposes the legacy `FollowGlobal` behavior with the user-facing label "Custom", plus automatic, high-quality, balanced, and smooth choices. The controller quality coordinator resolves the effective preset from the saved mode and visibility state; measured FPS, receiver WebRTC statistics, and presenter drops affect only automatic mode.

Quality changes travel through a typed C ABI into `ViewerInstance`, then across the existing control data channel using a versioned internal message. The host acknowledges the applied values. Unsupported or failed updates leave the existing stream running and report a non-fatal status.

Alternative considered: restart the viewer to renegotiate quality. Rejected because it interrupts input, media, authentication, and session identity.

### 4. Superseded approach: adapt FPS on every visible mode

This section records the formerly implemented policy and is superseded by Decision 8. It remains here so later optimization can compare results without reconstructing the earlier thresholds from source history.

Visible windows target 60 FPS by default. High-quality and automatic modes keep the original source resolution, balanced keeps 1080p, and smooth keeps 720p. The native viewer periodically exposes cumulative inbound WebRTC statistics through an optional versioned C ABI snapshot: decoded/received/dropped frames, decode and processing time, freezes, jitter-buffer delay, packet loss, RTT, and candidate-pair receive bandwidth where available. The controller combines deltas from those counters with local presenter drops and recent user interaction.

Observed receive FPS is a result signal, not an overload cause. A low value alone can mean a static desktop or low-motion source and therefore never starts degradation. Normal degradation requires both a sustained target-FPS shortfall and a direct decoder, network, freeze, jitter-buffer, or presenter-pressure signal. Severe freeze/drop/decode/network thresholds can enter an emergency tier without waiting for multiple weak signals.

High-quality keeps native resolution and requests 60 FPS in healthy operation. Correlated ordinary pressure may move it through 50 and 45, while 30 is reserved for severe sustained pressure; 24/15 remain background or software-fallback safety tiers. Automatic, balanced, and smooth modes use the same cause-aware signals with their configured visible floor. Minimized windows immediately use the configured background profile, initially 540p at 15 FPS, and persistent software fallback remains a hard safety exception.

Recovery raises FPS toward the selected mode's target without changing its visible resolution. Ordinary visible degradation uses a longer evidence window, while recovery uses a shorter healthy window plus a post-transition cooldown. Fresh keyboard or pointer activity accelerates recovery when no severe pressure remains, preventing an interactive session from staying trapped at a previously selected low tier.

High-quality locked windows receive the highest priority and are degraded last, but they do not bypass WebRTC congestion control and cannot cause unbounded allocation or application termination.

Each remote window renders a controller-only performance overlay as a mouse-transparent local child surface above either the D3D11 presenter or BGRA paint path. The overlay is fixed to the displayed image's bottom-right corner, refreshes from the existing one-second receiver sample, hides while minimized or before video is available, and reports only actual FPS, receive bitrate, pressure state, decode time, and dropped-frame ratio. It is never encoded or sent to the controlled device.

Because translucent native child windows are not composed reliably above a D3D11 SwapChain on Windows, the overlay uses a frameless, input-transparent layered tool window owned by its remote window. It rebuilds a highly translucent ARGB offscreen surface from a transparent baseline whenever visible text changes, then replaces its layered-window pixels in one composition operation so old digits cannot accumulate while opaque text remains visible. Its screen-space anchor follows owner movement and resizing, it hides whenever the owner is hidden or minimized, and it raises only after a real owner/presenter-layer transition.

### 5. Contain failures and bound fallbacks

Thread entry points catch standard allocation and runtime exceptions and convert them into viewer-scoped status. D3D11 device removal or presentation failure first triggers presenter recreation. Persistent failure enables a bounded software fallback with reduced resolution/FPS instead of allowing twenty full-resolution BGRA paths.

Stop and reconnect operations become managed joinable tasks rather than per-window detached threads. Application shutdown cancels new work, invalidates callbacks, clears pending frames, closes sockets, joins viewer work, and then destroys UI objects.

### 6. Move process-wide resources toward shared ownership

WebRTC SSL initialization and the PeerConnectionFactory thread set become process-wide, reference-counted resources shared by viewer sessions. D3D11 presentation uses one recoverable device manager per adapter and per-window swap chains. This reduces thread, stack, driver, and device-object growth while accepting that shared resources need explicit device-loss recovery.

The implementation is staged: bounded queues and lifecycle guards land first because they remove immediate crash multipliers; shared runtime/device ownership follows behind compatibility tests.

### 7. Record low-allocation crash context and resource telemetry

Diagnostics record active viewers, queued-frame counts, thread count, process memory, last viewer lifecycle state, D3D11 device removal reason, applied quality, and recent status transitions. Unhandled exceptions produce a minidump or compact crash record without depending on large allocations.

### 8. Replace multi-signal adaptation on fixed modes with explicit presets

The visible policy is simplified to four directly explainable presets: high quality requests original resolution at 60 FPS, automatic keeps original resolution and alone may move through 60/45/30 FPS, balanced requests 1080p at 45 FPS, and smooth requests 720p at 60 FPS. All four modes use 540p at 15 FPS while minimized. The bounded software fallback remains 540p at 24 FPS because it protects the controller from an unbounded full-resolution BGRA path rather than acting as a normal quality choice.

High quality, balanced, and smooth no longer consume receiver FPS, decoder time, network loss, presenter drops, aggregate receive budget, or recovery hysteresis to rewrite their visible request. WebRTC congestion control and Host encoder limits may still produce lower actual delivery; the local status UI reports actual FPS and bitrate independently from the unchanged preset request. Returning from minimized or software fallback immediately reapplies the selected visible preset so a temporary safety profile does not leave a fixed mode at a stale lower FPS.

Automatic retains bounded cause-aware adaptation because receive FPS alone cannot distinguish overload from a static desktop. Sustained low receive FPS must still correlate with decoder, network, freeze, jitter-buffer, or presenter pressure before automatic moves down one of its three FPS tiers; healthy evidence restores it through the same tiers. Aggregate receive-budget redistribution and recent-interaction fast recovery are removed from the decision path to keep the runtime behavior explainable.

#### Retired strategy effectiveness record

The previous visible policy kept each mode's resolution fixed but applied receiver-side hysteresis to every mode. It combined receive FPS, decode/processing time, WebRTC loss and jitter, freeze counters, local presenter drops, recent interaction, and aggregate receive budget. High quality normally requested 60 FPS, could degrade through 50 and 45 FPS under correlated pressure, and could reach 30 FPS under severe pressure.

One representative high-quality test reported 39 FPS, 12 Mbps, status “健康”, 1.3 ms decode time, and 0.0% dropped frames. Despite healthy receiver counters, the user observed obvious stutter and judged both clarity and frame rate below the high-quality target. This shows that receiver health means only that the receiving pipeline is not overloaded; it does not prove that the sender actually achieved the requested 60 FPS. Allowing a complex controller policy to rewrite fixed-mode requests also made that shortfall harder to diagnose.

The implementation therefore returns to explicit presets and confines adaptation to automatic mode. A separate sender-side performance risk remains recorded for later optimization: the current path can perform `desktop ARGB -> CPU I420 -> encoder CPU ARGB -> GPU upload -> NVENC`, causing two full-resolution color conversions before hardware encoding. This change does not expand into a zero-copy media-pipeline rewrite.

### 9. Use native window interaction and state-driven title-bar painting

Remote-window movement and edge resizing prefer `QWindow::startSystemMove` and `QWindow::startSystemResize`, with the existing manual geometry calculations retained only as a fallback. The Windows move loop supplies `WM_MOVING` rectangles for the existing cursor-priority snap calculation, while `WM_EXITSIZEMOVE` drives one shared cleanup path for snap application, geometry persistence, mask restoration, and overlay recovery.

Title-bar painting is state-driven rather than pointer- or frame-driven. Remote texture presentation no longer requests a title-bar repaint for every successful frame and D3D child mouse movement only repaints when the cursor crosses a real title-button boundary. Because Windows can still invalidate a frameless top-level QWidget backing store during native movement, the parent widget disables updates for the move duration and re-enables them only after final geometry and snap application; the native D3D child continues presenting independently. On supported Windows versions, rounded corners use DWM attributes and the Qt `QRegion` mask is removed entirely. Older systems temporarily clear the fallback mask during movement and restore it after release. The performance overlay is hidden once at interaction start and restored once at the final geometry. Snap previews change geometry and z-order only when the candidate set changes.

### 10. Reserve the focused visible window and decouple audio

The controller selects the visible, non-minimized remote window that Qt reports as the actual active/focused top-level window. That one window alone receives the native-resolution/60 FPS/high-priority request. Window size, full-screen state, and activation history do not select the video reservation; when no remote window has focus, no window receives the high-quality reservation.

Every other visible window keeps the current background resolution/FPS policy and low priority. Minimized or hidden windows keep the existing configured minimized resolution/FPS profile. Focus changes trigger one coalesced quality evaluation so the high-quality role moves without reconnecting any session.

Audio is independent from this video reservation. Each remote window starts muted and its title-bar speaker button controls only that window's audio state through the optional online audio ABI; quality evaluation never grants or revokes audio ownership.

### 11. Keep texture ownership thread-affine and synchronization waits finite

If the one pending texture descriptor is occupied, the decoder callback rejects the newly decoded descriptor and returns a controlled-drop result. The producer then reclaims its own keyed-mutex slot on its own D3D device and context. The decoder callback never calls QWidget, presenter, or consumer-device cleanup methods, eliminating cross-thread use of a shared D3D immediate context.

Every presenter keyed-mutex acquisition uses a finite wait. Timeout returns a presentation failure without pretending to own or release the consumer key; abandoned synchronization is treated as device reset so the existing presenter-recreation/software-fallback path can converge. The producer applies the same abandoned-object rule and retires the affected shared-texture group instead of reusing uncertain ownership. Timeout, abandoned, audio-owner, quality-role, and device-reset transitions are logged at low frequency to files under `<applicationDir>/data`; per-frame success logging remains disabled.

## Risks / Trade-offs

- Shared WebRTC or D3D11 resources become wider failure domains → use reference-counted lifetime, generation tokens, device-loss recreation, and viewer-scoped callbacks.
- Automatic quality can oscillate → limit adaptation to 60/45/30, retain cause-aware evidence and hold intervals, and keep fixed modes outside the transition loop.
- Static desktop content can report fewer frames than the requested cap → treat receive FPS as a result signal and require correlated pipeline evidence before lowering the requested FPS.
- A raw shared texture handle can become stale while queued → retain retired keyed-mutex texture groups until all consumer keys return, and validate the viewer generation before presentation.
- Cross-device texture production can race presentation → pair producer key `0` and consumer key `1`, return controlled drops directly to the producer, and preserve the last successful frame across transient failures.
- Interactive resize can discard SwapChain content repeatedly → throttle live buffer resizing to a bounded cadence, immediately render into each accepted new buffer, temporarily use a rectangular window region, and apply the exact final size on the next presented frame.
- Frameless manual movement can repaint independently composed layers out of phase → prefer the native system move loop, suppress unchanged title-bar painting, and hide owner-bound overlay windows until the move completes.
- Native movement can still invalidate the Qt backing store even without explicit `update()` calls → suspend parent-widget updates for the move duration and restore one final title-bar surface after geometry settles.
- A persistent rounded `QRegion` can keep the top-level window on a region-based composition path → prefer DWM corner attributes and remove the fallback Region for the duration of movement.
- Software fallback can still be expensive → enforce fallback resolution/FPS ceilings and keep only one frame.
- Startup pacing delays the last of twenty connections slightly → show every window immediately and expose connecting state while protecting process stability.
- Fixed presets can oversubscribe resources → keep queues bounded, preserve WebRTC/Host hard limits, and report the actual applied result without silently changing the selected preset.
- Focus can change rapidly between remote windows → serialize viewer audio start/stop and send only state transitions, while quality evaluation remains coalesced.
- A keyed mutex can be stalled by a driver or failed peer → use finite waits, never release a key that was not acquired, classify abandoned ownership as device loss, and preserve the last successful frame.

## Migration Plan

1. Add bounded latest-texture handoff, viewer generations, queue diagnostics, and close/reconnect invalidation without changing quality behavior.
2. Add startup pacing and deterministic managed stop work; verify existing one-window and group-tile flows.
3. Add quality data structures, global settings, title-bar override UI, typed ABI, versioned messages, and non-fatal acknowledgements.
4. Add fixed per-mode resolution, FPS-only adaptive performance logic, and bounded software fallback.
5. Share WebRTC runtime resources and D3D11 presentation devices after regression tests pass.
6. Run twenty-window churn and soak tests before enabling aggressive automatic quality recovery by default.

Rollback is configuration and feature based: disable live quality adaptation while retaining bounded frame queues and lifecycle protections. The stability protections are not rolled back unless they introduce a verified regression.

## Open Questions

- Whether a future automatic-only policy should reintroduce a visible aggregate receive-bandwidth limit after sender throughput is optimized and measured.
- Whether minimized high-quality-locked windows always follow the global minimized FPS or receive a separate opt-out.
- Which supported GPU/driver combinations require per-adapter rather than process-wide D3D11 device pools.
