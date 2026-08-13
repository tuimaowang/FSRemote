## ADDED Requirements

### Requirement: Every device target can be assigned to a group
The device context menu SHALL expose the same group-assignment submenu for a normalized single-device target and a normalized multi-device target.

#### Scenario: Single device is right-clicked
- **WHEN** the context menu targets exactly one valid device
- **THEN** it shows actions for creating a group or assigning the device to an existing group

### Requirement: Device context menus can stop scripts
The device context menu SHALL expose a stop-script action for both single-device and multi-device target sets and SHALL aggregate no-running-script feedback once per action.

#### Scenario: Multiple selected devices contain running scripts
- **WHEN** the user activates stop script from their shared context menu
- **THEN** the controller requests a stop for every targeted device with a running script and leaves unrelated devices unchanged

#### Scenario: No targeted device has a running script
- **WHEN** the user activates stop script and none of the normalized targets has a running script
- **THEN** the controller shows one informational message

