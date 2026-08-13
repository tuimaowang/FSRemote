## 1. Context Menus and Batch Actions

- [x] 1.1 Comment out the group "设备平铺" menu action creation and its selected-action handler so it is not displayed or callable.
- [x] 1.2 Comment out the device "重命名" and "删除设备" menu action creation and their selected-action handlers so they are not displayed or callable.
- [x] 1.3 Add helpers to collect valid target device indexes from a clicked group and from the current selected-device set.
- [x] 1.4 Add shared batch helpers for power-on, shutdown, restart, and terminal actions using the existing single-device command paths and availability checks.
- [x] 1.5 Add the group system submenu with a separator above it and actions for "批量开机", "批量关机", "批量重启", and "终端".
- [x] 1.6 Update device right-click behavior so right-clicking an already selected device preserves multi-selection, while right-clicking an unselected device targets only that device.
- [x] 1.7 Add device multi-selection system actions for power-on, shutdown, restart, and terminal, and route them through the shared batch helpers.

## 2. Script Log and Config Panels

- [x] 2.1 Add script tree widget/state members for the Script Log tab and keep them hidden outside the active Script Log page.
- [x] 2.2 Populate the script tree from the configured shared script path and store the selected valid script path for the active detail device.
- [x] 2.3 Update Script Log panel geometry so the tree occupies the left column and terminal output occupies the remaining area inside the shared detail boundary.
- [x] 2.4 Replace the single script action button rectangle/drawing with separate Execute and Stop button rectangles/drawing.
- [x] 2.5 Wire Execute to run the selected script path for the active detail device and Stop to stop the active device's running script.
- [x] 2.6 Restyle the Config File editor and painted panel so the editor text area uses a black background and white foreground text while preserving save behavior.
- [x] 2.7 Replace the painted Script Log body with a read-only selectable text control supporting Ctrl+C, context-menu copy, live updates, and selection preservation.

## 3. Drag Zones and Layout Boundaries

- [x] 3.1 Add a shared top drop-zone rectangle and painter helper for the red translucent gradient area under the title bar.
- [x] 3.2 Show the top drop zone with "移出分组" while dragging devices and "解散分组" while dragging groups.
- [x] 3.3 Update device drag release handling so dropping on the top zone clears group assignment for all dragged devices and saves the device list.
- [x] 3.4 Update group drag release handling so dropping on the top zone dissolves the group, clears member device group assignments, updates expanded/rename state, saves, and repaints.
- [x] 3.5 Keep normal group reordering behavior when a dragged group is not dropped on the dissolve zone.
- [x] 3.6 Ensure device detail, Script Log, Config File, settings general, and settings keyboard controls all derive geometry from the shared content boundary helpers in expanded and collapsed states.

## 4. Verification

- [x] 4.1 Perform a static review of affected menu construction and selected-action branches to confirm hidden actions cannot be invoked.
- [x] 4.2 Perform a static review of script tree, Execute/Stop hit testing, and child-widget visibility so inactive pages do not leave controls behind.
- [x] 4.3 Perform a static review of drag-zone painting and release logic for device move-out, group dissolve, and normal group reorder paths.
- [x] 4.4 Run `openspec status --change "refine-device-actions-and-script-panel"` and do not compile unless requested separately.
