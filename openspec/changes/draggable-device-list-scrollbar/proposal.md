## Why

The left device/group list already displays a scrollbar for long content, but the thumb is visual only and users must repeatedly use the mouse wheel to reach distant rows. Direct thumb dragging makes long lists and newly created groups much faster to navigate.

## What Changes

- Make the existing device-list scrollbar thumb respond to left-button press, drag, and release.
- Map thumb travel proportionally to the full device-list scroll range so dragging to the bottom reaches the end immediately.
- Keep drag coordinates clamped when the pointer moves above or below the track.
- Provide hover and active cursor feedback without interfering with device/group dragging, renaming, or wheel scrolling.

## Capabilities

### New Capabilities
- `device-list-scrollbar-drag`: Direct mouse dragging for the hand-painted device/group list scrollbar.

### Modified Capabilities

None.

## Impact

- Affects scrollbar geometry, painting, and mouse state handling in `src/ui/DeviceGrid.cpp` and `src/ui/DeviceGrid.h`.
- Does not change persisted device/group data, protocols, or external dependencies.
