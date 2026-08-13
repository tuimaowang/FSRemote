## ADDED Requirements

### Requirement: Multi-device context menu exposes group assignment
When a device-list context menu targets more than one valid selected device, the application SHALL show an `添加分组` submenu immediately below `系统设置`. The submenu SHALL show `新建分组` as its first action and SHALL list all current group names below it.

#### Scenario: Multi-selection opens the menu
- **WHEN** the user right-clicks a device that belongs to the current multi-device selection
- **THEN** the context menu shows `系统设置` followed by `添加分组`
- **AND** hovering `添加分组` reveals `新建分组` first and every current group name after it

#### Scenario: Single-device menu remains unchanged
- **WHEN** the context menu targets only one valid device, including a menu opened from a remote-window title bar
- **THEN** the application does not show the `添加分组` submenu

### Requirement: New group assignment enters inline rename
The application SHALL create a uniquely named group when `新建分组` is activated, SHALL assign every targeted device to that group, SHALL expand and reveal it, and SHALL place its default name into the focused inline group-name editor with all text selected.

#### Scenario: Create a group for selected devices
- **WHEN** the user activates `新建分组` for a multi-device selection
- **THEN** the application creates the next available `默认分组N` group with a stable group ID
- **AND** every targeted device is assigned to that group
- **AND** the sidebar and new group are expanded and scrolled into view
- **AND** the inline name editor is focused with the complete default name selected

### Requirement: Existing group assignment reveals the destination
The application SHALL assign every targeted device to the chosen existing group, expand that group, and scroll its header into view. If every targeted device already belongs to that group, the application SHALL leave membership unchanged and still perform the reveal operation.

#### Scenario: Move selected devices into another group
- **WHEN** the user activates an existing group name and at least one targeted device is outside that group
- **THEN** all targeted devices are persisted as members of the chosen group
- **AND** the chosen group is expanded and its header is visible

#### Scenario: Choose the current group
- **WHEN** the user activates an existing group name and all targeted devices already belong to it
- **THEN** the application does not rewrite device membership unnecessarily
- **AND** it expands and scrolls to the chosen group

### Requirement: Group assignment is scoped to the menu targets
The application MUST change membership only for valid device indexes captured for the opened context menu and MUST preserve other devices and the current multi-selection.

#### Scenario: Selection contains stale or duplicate indexes
- **WHEN** group assignment receives duplicate or no-longer-valid target indexes
- **THEN** each valid target device is assigned at most once
- **AND** invalid targets and all non-target devices remain unchanged
