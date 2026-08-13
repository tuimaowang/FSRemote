## ADDED Requirements

### Requirement: Unfinished context menu actions are hidden
The system SHALL remove unfinished context menu entries from the visible callable UI by commenting out the group "设备平铺" menu display and handler, and by commenting out the device "重命名" and "删除设备" menu display and handlers.

#### Scenario: Group context menu hides tiling
- **WHEN** the user right-clicks a group row
- **THEN** the context menu does not display "设备平铺"
- **AND** there is no selectable action path that calls the group tiling handler from that menu

#### Scenario: Device context menu hides rename and delete
- **WHEN** the user right-clicks a device row
- **THEN** the device system submenu does not display "重命名"
- **AND** the device system submenu does not display "删除设备"
- **AND** there is no selectable action path that calls rename or delete from that menu

### Requirement: Group context menu exposes batch system actions
The system SHALL add a separated group system submenu below the script actions, with a horizontal separator above it, and the submenu SHALL contain batch power-on, shutdown, restart, and terminal actions for devices in that group.

#### Scenario: Group menu system submenu layout
- **WHEN** the user right-clicks a group row
- **THEN** the context menu displays script-related actions first
- **AND** a horizontal separator appears above the system submenu
- **AND** the system submenu contains "批量开机", "批量关机", "批量重启", and "终端"

#### Scenario: Group batch system action targets member devices
- **WHEN** the user chooses a system action from a group context menu
- **THEN** the action applies only to devices whose group matches the clicked group
- **AND** devices outside that group are not targeted

### Requirement: Multi-selected device context menu supports batch system actions
The system SHALL preserve an existing multi-selection when right-clicking one of the selected devices and SHALL expose power-on, shutdown, restart, and terminal actions that apply to the selected devices.

#### Scenario: Right-click selected device keeps multi-selection
- **WHEN** multiple devices are selected
- **AND** the user right-clicks one of those selected devices
- **THEN** the selected-device set remains unchanged
- **AND** the context menu actions target all selected devices

#### Scenario: Right-click unselected device targets only that device
- **WHEN** multiple devices are selected
- **AND** the user right-clicks a device outside the selection
- **THEN** the selection changes to only the clicked device
- **AND** the context menu actions target only the clicked device

#### Scenario: Batch terminal action
- **WHEN** the user chooses the terminal action for multiple selected devices
- **THEN** the system opens or activates a terminal flow for each target device that can accept terminal connections

### Requirement: Batch power actions reuse device command safety
The system SHALL route batch power-on, shutdown, and restart actions through the same safety checks and command paths used by existing single-device actions.

#### Scenario: Batch power-on
- **WHEN** the user chooses "批量开机" for a group or selected devices
- **THEN** the system starts the wake flow for each target device that has enough wake information

#### Scenario: Batch shutdown or restart skips unavailable command targets
- **WHEN** the user chooses "批量关机" or "批量重启"
- **THEN** the system sends the command to each target device that can accept remote commands
- **AND** unavailable targets are not used for command execution

### Requirement: Top drag zone removes devices or dissolves groups
The system SHALL show a red translucent gradient drop zone below the title bar while dragging devices or groups, and dropping on that zone SHALL either move devices out of a group or dissolve the dragged group.

#### Scenario: Dragging devices shows remove-from-group zone
- **WHEN** the user drags one or more devices
- **THEN** a red translucent gradient zone appears below the title bar
- **AND** the zone label is "移出分组"

#### Scenario: Dropping devices on top zone clears group assignment
- **WHEN** the user drops dragged devices on the "移出分组" zone
- **THEN** each dragged device has its group assignment cleared
- **AND** the device list is saved and repainted

#### Scenario: Dragging group shows dissolve zone
- **WHEN** the user drags a group row
- **THEN** a red translucent gradient zone appears below the title bar
- **AND** the zone label is "解散分组"

#### Scenario: Dropping group on top zone dissolves group
- **WHEN** the user drops a dragged group on the "解散分组" zone
- **THEN** the group name is removed from the group list
- **AND** devices that belonged to that group have their group assignment cleared
- **AND** group expanded state, inline rename state, persisted data, and repaint state are updated consistently
