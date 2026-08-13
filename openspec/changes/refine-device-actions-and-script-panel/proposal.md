## Why

Device and group operations currently mix incomplete menu entries with active actions, and script execution is limited to a single run button without a visible script browser. The device detail, settings, and collapsed-sidebar layouts also need consistent safe drawing boundaries so controls do not overlap.

## What Changes

- Hide unfinished context menu entries by commenting out menu display for group tiling and device rename/delete actions.
- Add group-level system actions with a separated submenu containing batch wake, shutdown, restart, and terminal operations.
- Support right-clicking multiple selected devices to run batch wake, shutdown, restart, and terminal actions.
- Add a script tree to the Script Log tab so scripts can be browsed from the configured shared script path and selected freely before execution.
- Replace the single script action button with separate Execute and Stop buttons.
- Make Script Log output selectable and copyable with standard mouse, keyboard, and context-menu interactions.
- Restyle the Config File tab editor to use a black background with white text.
- Add drag drop-zone feedback under the title bar:
  - dragging a group shows a red translucent gradient target labeled "解散分组";
  - dragging devices shows a matching target labeled "移出分组";
  - dropping a group on the target dissolves the group using the same data-safety model as moving devices out of a group.
- Align device detail and settings page drawing boundaries so expanded and collapsed sidebar states use the same content edge and bottom safety area.

## Capabilities

### New Capabilities
- `device-context-actions`: Device and group context menus, multi-selection batch system actions, and drag-to-remove/dissolve interactions.
- `device-detail-script-panel`: Script log/config detail tabs, script tree selection, execute/stop controls, and editor visual styling.
- `remote-shell-layout-consistency`: Shared layout boundaries for device detail and settings pages across expanded/collapsed sidebar states.

### Modified Capabilities
- None.

## Impact

- Affected UI code: `src/ui/DeviceGrid.cpp`, `src/ui/DeviceGrid.h`.
- Affected services: existing device command, wake-on-LAN, SSH terminal/script execution flows may be reused for batch actions.
- Affected resources: no new image assets are required; gradient/drop-zone drawing can be implemented with QPainter.
- No public API or external dependency changes are expected.
