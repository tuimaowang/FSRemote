## Context

`DeviceGrid` already builds one context menu for a device-list selection, stores groups with stable IDs, persists each device's group, paints an expandable scrollable sidebar, and provides inline group renaming. The new flow should reuse those mechanisms and must not affect the single-device menu opened from a remote-window title bar.

## Goals / Non-Goals

**Goals:**

- Add a discoverable group-assignment submenu only when more than one valid device is targeted.
- Make new-group creation, assignment, expansion, scrolling, and inline rename feel like one atomic interaction.
- Make assignment to an existing group idempotent and reveal the destination even when no persisted data changes.
- Save and synchronize the resulting device/group state through the current persistence path.

**Non-Goals:**

- Changing the group JSON schema, device synchronization protocol, or group ordering rules.
- Adding the submenu to single-device or group-header context menus.
- Removing drag-and-drop grouping or the blank-area new-group action.

## Decisions

1. `showDeviceContextMenuForIndexes` will create `添加分组` immediately after `系统设置` only when the validated target set contains multiple devices. The submenu will contain `新建分组`, a separator, and one action per current group. This preserves all existing single-target menus.

2. Group actions will be tracked by their `QAction*` and group index instead of storing the index in `QAction::data`. Script leaf actions already use `data`, so pointer-based dispatch prevents a group action from being mistaken for a script action.

3. Small `DeviceGrid` helpers will own the workflow: generate a unique `默认分组N` name, create the group with a stable UUID, assign valid target devices, expand the destination, align its header within the scroll viewport, and optionally start inline rename. Reusing helpers also lets the existing blank-area `新建分组` action keep the same naming rules.

4. Assignment writes both the compatibility group name and stable group ID, then calls the existing `saveDevices` path once if data changed. Selecting a group that already contains every target skips the write but still expands and reveals the group.

5. Reveal logic will open the sidebar and destination group before recomputing visual rows, clamp the scroll offset to its valid range, repaint, and only then position the inline editor. This keeps editor geometry correct for groups that were previously collapsed or off-screen.

## Risks / Trade-offs

- [A group action index could become stale while the modal menu is open] → Group mutations occur on the UI thread; validate the index again after `exec` before assigning.
- [Moving devices changes the number and order of visible rows] → Expand first, recompute the destination visual row after assignment, and clamp the final scroll offset.
- [A save/sync can rebuild group storage] → Complete the synchronous local save before resolving the destination row; use stable group IDs when writing device membership.
- [The last group cannot always align exactly to the viewport top] → Clamp to the maximum scroll offset while guaranteeing the header remains visible.
