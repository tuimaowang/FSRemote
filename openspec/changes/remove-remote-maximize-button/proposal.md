## Why

The remote window offers both a title-bar maximize button and title-bar double-click for the same operation, making the compact custom title bar unnecessarily crowded. Keeping double-click as the only maximize/restore gesture creates more space and a cleaner control layout.

## What Changes

- Remove the maximize/restore icon and click target from remote-window title bars.
- Preserve double-clicking the non-control title-bar area to toggle maximized and normal states.
- Move the minimize button directly beside the close button.
- Reflow clipboard sync, input sync, quality, and update controls into a continuous right-aligned chain with consistent spacing.
- Keep full-screen behavior, title dragging, resizing, and window geometry persistence unchanged.

## Capabilities

### New Capabilities
- `remote-titlebar-window-controls`: Defines the simplified remote-window title bar with minimize and close buttons plus double-click maximize/restore.

### Modified Capabilities

None.

## Impact

- Affects title-bar rectangles, painting, hit testing, and double-click comments in `src/ui/RemoteDesktopWindow.cpp` and `src/ui/RemoteDesktopWindow.h`.
- The existing maximize SVG may remain as an unused resource; no protocol, persisted setting, or dependency changes are required.
