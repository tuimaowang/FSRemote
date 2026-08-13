## 1. Shared input coordination

- [x] 1.1 Add a semantic `RemoteInputEvent` model and a `RemoteInputBroadcastCoordinator` module, register the new sources in CMake, and keep the coordinator free of Host protocol changes.
- [x] 1.2 Implement guarded window registration, three-state role queries, unique-master selection, monotonically increasing master generations, and state-change notifications.
- [x] 1.3 Implement dynamic follower eligibility and isolated O(n) fan-out so one failed or destroyed target cannot interrupt delivery to other targets.
- [x] 1.4 Track broadcast-held keyboard keys and mouse buttons per target and implement release barriers for disable, master transfer, follower removal, master loss, and coordinator teardown.

## 2. Remote window integration

- [x] 2.1 Make `DeviceGrid` own one coordinator and pass it to both individually opened and tiled `RemoteDesktopWindow` instances; register and unregister every window safely.
- [x] 2.2 Expose the minimum window adapter needed by the coordinator: stable identity, current remote frame size, input eligibility, direct semantic-event serialization, and best-effort release.
- [x] 2.3 Refactor absolute move, relative move, button, wheel, Qt key, and native-hook key paths to create semantic events after existing local filtering and deliver the origin event exactly once.
- [x] 2.4 Transform normalized absolute coordinates and resolution-relative deltas for each eligible target while preserving button masks, wheel direction, event order, and protocol clamping.
- [x] 2.5 Keep clipboard, quality, update, connection-control, window dragging/resizing, and consumed local shortcuts on their existing non-broadcast paths.
- [x] 2.6 Notify the coordinator when connection/input eligibility changes so new followers join future events, follower loss is isolated, and master loss disables the group.

## 3. Title-bar interaction

- [x] 3.1 Add an icon-sized synchronization button rectangle before the clipboard control, shift the existing quality/update rectangle chain without overlap, and exclude the new rectangle from title dragging and double-click handling.
- [x] 3.2 Paint distinct off, master, follower, hover, and pressed visuals and provide action-oriented tooltips for each role while preserving the current full-screen title-bar behavior.
- [x] 3.3 Wire button clicks so off selects the window, follower transfers the master after a release barrier, and master disables the group; repaint all registered title bars after every role change.

## 4. Verification

- [x] 4.1 Add coordinator unit tests for enable, disable, unique-master transfer, late-generation rejection, dynamic membership, follower failure isolation, and the one-window-remains behavior.
- [x] 4.2 Add input-routing tests proving that master events reach the origin and every eligible follower once, follower-originated input stays local, and excluded message types never broadcast.
- [x] 4.3 Add coordinate tests for letterboxed windows, mixed local window sizes, mixed remote resolutions, relative movement scaling, bounds clamping, button positions, and wheel events.
- [x] 4.4 Add safety tests for held key/button release during master transfer, disable, disconnect, remote update, follower removal, and destruction, including unreachable-target fallback behavior.
- [ ] 4.5 Manually verify ordinary and tiled windows, software and D3D11 texture presentation, normal/maximized/full-screen states, native keyboard shortcuts, reconnect/update flows, and a 20-window high-rate mouse stress case.
- [x] 4.6 Build the affected targets and run the complete automated test suite; confirm no Host protocol, persisted setting, or WJY change-log artifact is introduced by this change.
