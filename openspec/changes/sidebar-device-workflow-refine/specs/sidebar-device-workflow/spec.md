## ADDED Requirements

### Requirement: Sidebar list layout is compact and aligned
The system SHALL align the sidebar device/group list scrollbar in the gutter between row content and the sidebar border, expand the bottom blank list area to the settings divider when there is available space, shrink the settings entry height, keep settings icon/text vertically centered, move the collapse button under the status capsule area, and remove the bottom status/name capsule display.

#### Scenario: Sidebar list with short content
- **WHEN** the visible device/group list does not fill the sidebar above the settings divider
- **THEN** the blank area extends to the top of the settings divider
- **AND** the scrollbar gutter is centered in the gap between list rows and the left-sidebar border

#### Scenario: Settings entry renders compactly
- **WHEN** the sidebar settings entry is visible
- **THEN** its row height is reduced from the previous tall footer
- **AND** the settings icon, label, and collapse affordance remain vertically centered in their respective controls

### Requirement: Device status accent remains online-colored for occupied devices
The system SHALL use the online accent color for the left-side device accent when a device is either online or occupied, while preserving other status indications elsewhere.

#### Scenario: Occupied device row
- **WHEN** a device presence state is occupied or busy
- **THEN** the left-side device accent uses the same color as an online device

### Requirement: Group names have bounded display and edit width
The system SHALL display group names in an elided region equivalent to ten characters and SHALL align the group rename editor to the same ten-character visual width.

#### Scenario: Long group name display
- **WHEN** a group name is longer than the ten-character display region
- **THEN** the visible group row text is elided within that region

#### Scenario: Group rename editor
- **WHEN** a group is renamed inline
- **THEN** the editor appears aligned with the displayed group text
- **AND** the editor width matches the ten-character display region

### Requirement: Titlebar wordmark launches selected remote devices
The system SHALL treat the top-left wordmark/title area as a clickable launch button that opens remote windows for selected devices.

#### Scenario: Launch single selected online device
- **WHEN** one online device is selected and the user clicks the titlebar wordmark button
- **THEN** the selected device remote desktop window opens or activates

#### Scenario: Launch multiple selected devices
- **WHEN** multiple devices are selected and the user clicks the titlebar wordmark button
- **THEN** remote windows are opened for each selected device using the persisted JSON geometry/size behavior

#### Scenario: Launch powered-off selected device
- **WHEN** a selected device is powered off and the user clicks the titlebar wordmark button
- **THEN** the system starts the wake flow for that device
- **AND** a remote window is opened or queued immediately for that device

### Requirement: Device double-click starts inline rename
The system SHALL use double-click on a device row to start inline device rename instead of opening a remote desktop window.

#### Scenario: Double-click device row
- **WHEN** the user double-clicks a device row
- **THEN** an inline rename editor appears for that device
- **AND** no remote desktop window is opened by the double-click itself

### Requirement: Groups can be dragged to reorder
The system SHALL allow group rows to be dragged to a new group position and SHALL preserve all member devices with their group when the group moves.

#### Scenario: Drag group row
- **WHEN** the user drags a group row above or below another group row
- **THEN** the group order changes to the drop position
- **AND** devices whose group name matches the moved group remain in that group

### Requirement: Devices are sorted by leading character within each scope
The system SHALL sort root devices and devices inside each group independently by display name leading character.

#### Scenario: Root and grouped devices render sorted
- **WHEN** the device list is rendered
- **THEN** ungrouped devices are sorted by display name leading character
- **AND** each group's devices are sorted independently by display name leading character

### Requirement: Settings access moves to icon-only titlebar control
The system SHALL expose Settings as an icon-only titlebar button positioned to the left of the refresh button, with no settings text in the sidebar footer.

#### Scenario: Click titlebar settings icon
- **WHEN** the user clicks the titlebar settings icon
- **THEN** the Settings page is selected
- **AND** the sidebar footer no longer provides the text-based Settings entry
