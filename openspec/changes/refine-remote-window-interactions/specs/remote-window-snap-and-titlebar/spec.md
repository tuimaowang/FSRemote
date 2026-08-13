## ADDED Requirements

### Requirement: Cursor-selected snap targets have priority
The controller SHALL treat cursor proximity to a compatible remote-window edge as an independent snap trigger and SHALL prefer a cursor-triggered candidate over any simultaneously valid moving-window-edge candidate.

#### Scenario: Cursor and window edge select different targets
- **WHEN** a dragged remote window has a valid window-edge snap candidate while the cursor is within snap distance of a different compatible target edge
- **THEN** the snap preview and committed geometry use the cursor-selected target

#### Scenario: Cursor does not identify a target
- **WHEN** the cursor is not within snap distance of a compatible target edge and the dragged window edge is within snap distance
- **THEN** the existing moving-window-edge snap behavior is used

### Requirement: Hidden title-bar controls are not interactive
The controller SHALL derive remote title-bar painting and hit testing from the same width-dependent layout and SHALL NOT respond to a control whose full visual rectangle is covered or outside the visible title bar.

#### Scenario: Remote window is reduced to minimum width
- **WHEN** a remote window becomes too narrow to display all title-bar controls
- **THEN** lower-priority controls are hidden and their former rectangles do not respond to hover, press, release, tooltip, or menu actions

