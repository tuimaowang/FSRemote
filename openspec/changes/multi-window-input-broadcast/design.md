## Context

`RemoteDesktopWindow` currently owns title-bar painting and hit testing, converts local pointer positions to the existing 0..65535 remote coordinate space, captures Qt/native keyboard transitions, and writes serialized input directly to its own viewer handle through `StreamRuntime::sendInput`. `DeviceGrid` creates both ordinary and tiled remote windows and already owns cross-window services whose lifetime exceeds the windows.

The new behavior is cross-window but remains entirely within one controller process. It must preserve current one-window input behavior when synchronization is off, must not broadcast non-input messages such as clipboard payloads, and must handle windows being created, disconnected, updated, or destroyed during an active group.

## Goals / Non-Goals

**Goals:**

- Provide one explicit master and zero or more dynamically eligible followers.
- Preserve the exact existing input path for the master while routing semantic copies to followers.
- Map absolute and relative pointer data per target and preserve keyboard/button ordering.
- Prevent stuck input during master transfer, disable, disconnect, update, or teardown.
- Keep fan-out synchronous, bounded, and isolated on the Qt UI thread for the supported remote-window count.

**Non-Goals:**

- Synchronizing clipboard contents, remote quality, update actions, window geometry, or title-bar interactions.
- Coordinating separate controller computers or changing Host admission/control rules.
- Persisting the selected master across application restarts.
- Replaying historical events to a newly connected follower.
- Adding a new network protocol or third-party dependency.

## Decisions

### 1. Add a shared `RemoteInputBroadcastCoordinator`

`DeviceGrid` will own one coordinator with a lifetime longer than every `RemoteDesktopWindow`, pass it to both ordinary and tiled window constructors, and ensure every window registers/unregisters. The coordinator stores guarded `QPointer<RemoteDesktopWindow>` membership, the current master, a monotonically increasing master generation, and broadcast-held input state.

This central owner makes the single-master invariant and dynamic membership testable. A static global inside `RemoteDesktopWindow` was rejected because it obscures ownership, complicates teardown, and couples tests to process-global state. Reusing `RemoteViewerLifecycleManager` was rejected because viewer startup/cleanup scheduling and UI input routing have unrelated responsibilities.

### 2. Route semantic input, not arbitrary protocol strings

Introduce a small internal `RemoteInputEvent` representation for absolute move, relative move, button down/up, wheel, key down/up, and capture release. Existing event handlers will construct this representation only after current local filtering and shortcut handling. A window-level dispatch method will always deliver the event to the origin session, then ask the coordinator to deliver transformed copies only when the origin and captured generation still identify the master.

Clipboard (`cb`), quality messages, connection control, and other protocol strings stay on their existing direct path. Broadcasting arbitrary `QByteArray` messages was rejected because it would accidentally widen scope and makes per-target relative-coordinate transformation unsafe.

### 3. Preserve normalized absolute coordinates and scale relative deltas

Absolute move, button, and wheel events already use a normalized 0..65535 point, so that canonical point is retained and serialized identically for every target. It is independent of local window size, aspect ratio, and letterboxing.

For relative movement, the event stores delta as a fraction of the master's remote frame width/height (with a safe fallback when frame dimensions are unavailable). Each target converts that fraction to its own remote frame pixels and clamps it to the protocol's safe bounds before serialization. This preserves comparable motion across different remote resolutions better than copying source pixel deltas.

### 4. Model three UI states with one toggle action

Every non-full-screen title bar receives an icon-sized hit target positioned before the existing clipboard button; existing quality/update buttons shift left through the same rectangle chain. The visual states are off, master, and follower. Clicking off selects that window, clicking follower transfers selection, and clicking master disables synchronization. Tooltips state the next action.

The synchronization role remains active in full-screen mode even though local title controls are hidden. Entering or leaving full screen therefore does not silently alter the group.

### 5. Define eligibility separately from membership

Registration makes a window a group member; event delivery additionally checks that the viewer is connected, the handle belongs to the current generation, the window accepts input, and it is not closing or running a remote update. Newly eligible followers receive future events automatically. A failed send is recorded for diagnostics but does not stop the fan-out loop or synchronously retry.

Input generated in a follower window continues to use its existing direct route only. Merely focusing or operating a follower never changes the master; only its title-bar button does.

### 6. Use generation checks and explicit release barriers

Every enable, transfer, disable, or forced master loss advances the coordinator generation. Queued input carries the generation observed at capture and is dropped if stale.

The coordinator tracks broadcast key and mouse-button downs per participating target. Before changing master or disabling, it sends matching releases to reachable targets, requests existing per-window release helpers where applicable, clears all held state, and only then publishes the new role. A follower removal clears its tracked state after best-effort releases; it does not stop the group. Losing master eligibility disables the group rather than choosing a replacement implicitly.

### 7. Keep dispatch on the UI thread

Qt input capture, window eligibility changes, registry updates, and `StreamRuntime::sendInput` calls will remain serialized on the UI thread. With the existing practical limit of roughly twenty viewer windows, one bounded O(number of eligible windows) fan-out per input event is simpler and safer than cross-thread queues. Mouse moves can reuse the existing event cadence; coalescing or background dispatch is deferred unless profiling shows UI-thread pressure.

## Risks / Trade-offs

- [High-rate pointer fan-out can increase UI-thread and data-channel load] → Keep one bounded pass over registered windows, avoid retries/logging every move, and include a 20-window stress test and profiling checkpoint.
- [Targets can display different content despite matching coordinates] → Define synchronization as coordinate/input replication; do not attempt visual/content alignment.
- [Different relative-mouse behavior across hosts can feel inconsistent] → Scale by remote frame dimensions, propagate capture release, clamp deltas, and test mixed resolutions.
- [A disconnect can prevent a release from reaching one target] → Send best-effort release before removal and rely on the existing Host session teardown release behavior as the final safeguard.
- [Small title bars can become crowded] → Use an icon-sized control, include it in blank-area exclusion, and chain neighboring control rectangles from right to left.
- [Native keyboard-hook events could bypass the new route] → Make the existing forwarding helper construct the same semantic key event and add coverage for both Qt and native-hook paths.

## Migration Plan

1. Add the coordinator and semantic input model behind a default-off state; existing one-window behavior remains unchanged.
2. Register both ordinary and tiled windows and expose eligibility/lifecycle notifications.
3. Refactor each current keyboard/mouse send site through the semantic dispatch path while leaving clipboard and other messages direct.
4. Add and enable the title-bar control after coordinator and release tests pass.
5. Validate mixed software/texture presentation, normal/full-screen windows, mixed resolutions, reconnect/update, and 20-window fan-out.

Rollback consists of removing the title-bar entry and coordinator routing; the Host protocol and stored settings require no migration.

## Open Questions

None blocking. The initial implementation keeps synchronization enabled when the master is the only remaining window so that later windows can join automatically; it is never persisted across application restart.
