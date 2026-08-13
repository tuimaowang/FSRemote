## Why

The left sidebar mixes device listing, grouping, settings access, and remote-window entry points, but several details are visually misaligned or tied to older interactions. This change makes the sidebar behave more like a compact device manager: predictable sorting, inline renaming, group dragging, clearer launch behavior, and tighter chrome controls.

## What Changes

- Refine left sidebar layout: scrollbar placement, expanded blank drop area, smaller settings row, settings icon relocation, collapse-button placement, and removal of the bottom status/name capsule.
- Keep device status accent color online-style for both online and occupied devices.
- Constrain group display/edit widths to a ten-character visual width, with the rename editor aligned to the displayed group text.
- Convert the top-left wordmark area into a remote-launch button for selected devices.
- Change device double-click behavior from opening remote desktop to inline device renaming.
- Add group dragging/reordering, carrying group member devices with the group.
- Sort devices by leading character by default, independently at root and inside each group.
- Move Settings to an icon-only titlebar control beside refresh.

## Capabilities

### New Capabilities
- `sidebar-device-workflow`: Sidebar device/group presentation, remote launch entry points, sorting, renaming, dragging, and compact settings access.

### Modified Capabilities

## Impact

- Affected Qt UI code: `src/ui/DeviceGrid.cpp` and `src/ui/DeviceGrid.h`.
- Affected resources: titlebar/settings SVG usage and existing titlebar layout constants.
- Affected persisted data behavior: device/group display order will be derived from sorted names, while group membership continues to persist in `devices.json`.
- Runtime behavior impact: double-click no longer opens remote desktop; remote launch moves to the top-left titlebar button.
