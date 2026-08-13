## Why

Moving several selected devices into a group currently requires repeated drag operations or other indirect UI actions. A group submenu in the existing multi-device context menu makes this common organization workflow immediate while keeping the user oriented in the device list.

## What Changes

- Add an `添加分组` submenu directly below `系统设置` when the context menu targets multiple selected devices.
- Put `新建分组` on the first submenu row and list every current group below it.
- Create a group with a default name, move all targeted devices into it, expand and scroll to it, then select the default name in the inline group-name editor.
- Move all targeted devices into a selected existing group, expand that group, and scroll the device list to its header.
- Preserve the existing single-device and remote-window context-menu behavior.

## Capabilities

### New Capabilities
- `multi-device-group-assignment`: Assign a multi-device context-menu selection to a newly created or existing group and reveal the destination group in the sidebar.

### Modified Capabilities

None.

## Impact

- Affects the device context-menu construction and group navigation/editing helpers in `src/ui/DeviceGrid.cpp` and `src/ui/DeviceGrid.h`.
- Reuses the existing device/group persistence and synchronization format; no protocol or settings schema change is required.
- Adds focused tests for destination-group assignment and navigation calculations where practical.
