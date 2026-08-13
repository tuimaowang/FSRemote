## ADDED Requirements

### Requirement: Remote title bar omits maximize control
The remote-window title bar SHALL display minimize and close window controls and MUST NOT display or expose a clickable maximize/restore button.

#### Scenario: Paint a normal remote window
- **WHEN** a remote window is visible outside full-screen mode
- **THEN** its title bar paints minimize and close controls
- **AND** it does not paint a maximize or restore control

#### Scenario: Click the former maximize area
- **WHEN** the user clicks a title-bar position that was previously occupied only by the maximize control
- **THEN** the application does not toggle maximized state through a button action

### Requirement: Title-bar double-click remains the maximize gesture
The application SHALL toggle a remote window between normal and maximized states when the user double-clicks a non-control title-bar area.

#### Scenario: Double-click a normal window title bar
- **WHEN** the user double-clicks a blank title-bar area on a normal remote window
- **THEN** the window becomes maximized

#### Scenario: Double-click a maximized window title bar
- **WHEN** the user double-clicks a blank title-bar area on a maximized remote window
- **THEN** the window returns to normal state

#### Scenario: Double-click another title control
- **WHEN** the user double-clicks an update, quality, input sync, clipboard, minimize, or close control
- **THEN** the double-click maximize gesture is not activated

### Requirement: Remaining title controls are reflowed without gaps
The application SHALL place close at the existing right resize margin, SHALL place minimize directly to its left, and SHALL arrange clipboard, input sync, quality, and update controls leftward with their established consistent spacing and no overlap.

#### Scenario: Lay out the title-bar control chain
- **WHEN** the remote title bar calculates its control rectangles
- **THEN** minimize is immediately adjacent to close
- **AND** clipboard is separated from minimize by four pixels
- **AND** input sync, quality, and update remain ordered and non-overlapping to the left

#### Scenario: Enter full-screen mode
- **WHEN** the remote window is full-screen
- **THEN** local title-bar controls remain hidden and the remote image continues to receive top-edge input
