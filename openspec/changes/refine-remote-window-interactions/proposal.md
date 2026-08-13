## Why

Remote-window interaction behavior is currently inconsistent at several high-frequency touch points: snap selection does not treat the cursor as an independent priority signal, compact title bars retain invisible hit targets, tray actions use different click semantics, device menus expose different actions by selection count, and quality selection requires manual per-window switching. These inconsistencies make multi-device control less predictable and less efficient.

## What Changes

- Add cursor-edge snap detection and give cursor-selected snap targets priority over moving-window-edge targets.
- Refine remote input synchronization participation so a follower can disable synchronization for only itself before a later click promotes it to master, with clearer role visuals.
- Make compact remote title bars share one visibility and hit-test policy so covered controls cannot be clicked.
- Change tray activation, taskbar activation, close, and minimize behavior to use single-click toggling while retaining the main-window taskbar icon.
- Expose group assignment and script stopping from both single-device and multi-device context menus.
- Automatically assign high quality to active or full-screen remote windows and smooth quality to other visible remote windows.
- Require deliberate hover intent before expanding the controlled-device status bubble so rapid cursor passes do not leave it open.
- Make the frameless main-window corners genuinely transparent instead of painting rectangular fill behind the rounded border.

## Capabilities

### New Capabilities
- `remote-window-snap-and-titlebar`: Cursor-prioritized snapping and responsive title-bar control visibility/hit testing.
- `input-sync-device-participation`: Per-device participation states and clear synchronization role controls.
- `persistent-main-window-presence`: Single-click tray toggling and taskbar-preserving minimize/close behavior.
- `device-context-workflows`: Consistent grouping and script-stop actions for single and multiple device targets.
- `active-window-quality-policy`: Automatic quality assignment based on active, full-screen, background, and minimized window state.
- `controlled-overlay-hover-intent`: Delayed hover expansion and cancellation for the controlled-device status bubble.
- `rounded-main-window-surface`: Alpha-backed rounded main-window painting without opaque corner fill.

### Modified Capabilities

None. The repository currently has no promoted main specs for these interaction contracts.

## Impact

- Remote-window geometry, title-bar painting, and mouse hit testing in `src/ui/RemoteDesktopWindow.*`.
- Input broadcast state management in `src/ui/RemoteInputBroadcastCoordinator.*`.
- Tray and main-window lifecycle behavior in `src/ui/MainWindow.*`.
- Device context-menu dispatch in `src/ui/DeviceGrid.*`.
- Remote quality coordination and related settings/status UI in `src/ui/RemoteQualityCoordinator.*`, `src/ui/RemoteWindowCoordinator.*`, and `src/system/AppSettings.*`.
- Controlled-device status bubble hover timing in `src/ui/RemoteControllerOverlay.*`.
- Main-window transparency and shell painting in `src/ui/MainWindow.*` and `src/ui/DeviceGrid.*`.
- Focused unit tests for snap selection, synchronization state transitions, title-bar layout, device action targets, and quality decisions.
