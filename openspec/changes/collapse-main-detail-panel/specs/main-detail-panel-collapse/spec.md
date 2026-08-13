## ADDED Requirements

### Requirement: Collapse button hides the detail panel
The main controller SHALL keep the device sidebar visible when the user presses `<<`, SHALL hide the detail panel, and SHALL change the button label to `>>`.

#### Scenario: User collapses the detail panel
- **WHEN** the main window is expanded and the user clicks `<<`
- **THEN** the device sidebar remains visible and interactive, the detail content is hidden, and the button displays `>>`

#### Scenario: User expands the detail panel
- **WHEN** the detail panel is collapsed and the user clicks `>>`
- **THEN** the detail panel is restored with the previously selected page and the button displays `<<`

### Requirement: Main window follows the detail-panel state
The main window SHALL shrink to a fixed compact width containing the full device sidebar and a dedicated toggle strip when the detail panel is collapsed, and SHALL restore the previously expanded width when the detail panel is opened.

#### Scenario: Window shrinks after collapse
- **WHEN** the user collapses the detail panel from a normally sized main window
- **THEN** the window keeps its current top-left position and height and reduces its width to the compact device-sidebar width

#### Scenario: Window restores after expansion
- **WHEN** the user expands the detail panel after moving or using the compact window
- **THEN** the window keeps its current top-left position and height and restores the saved expanded width of at least the normal minimum width

### Requirement: Toggle button does not overlap devices
The collapse/expand button SHALL be drawn and hit-tested in a dedicated strip to the right of the 240-pixel device sidebar and SHALL NOT overlap device rows, group rows, or the device scrollbar.

#### Scenario: Compact sidebar contains devices
- **WHEN** one or more device or group rows extend near the bottom of the compact window
- **THEN** the `>>` button remains outside the device-list viewport and every visible device row keeps its normal interaction area

### Requirement: Compact title bar remains usable
The compact title bar SHALL display a non-overlapping application title and the minimize and close controls, SHALL hide detail-related title content that cannot fit, and SHALL remove hidden controls from mouse hover and click handling.

#### Scenario: Detail panel is collapsed
- **WHEN** the window enters compact mode
- **THEN** local computer identity, update, settings, and refresh content are hidden while the title, minimize, and close controls remain visible and non-overlapping

#### Scenario: Detail panel is expanded
- **WHEN** the window leaves compact mode
- **THEN** the full title-bar content and its normal mouse interactions are restored

### Requirement: Hidden detail controls stop participating
The application SHALL hide all real Qt controls belonging to the detail panel and SHALL stop detail-only local resource monitoring while the detail panel is collapsed.

#### Scenario: Collapse while a detail editor is visible
- **WHEN** the user collapses the detail panel while settings, script, configuration, search, or local-system controls are visible
- **THEN** those controls become hidden and disabled without changing their logical page state

#### Scenario: Expand back to the same page
- **WHEN** the user expands the detail panel again
- **THEN** controls for the previously selected page are restored and local resource monitoring resumes only if the local-system page is selected
