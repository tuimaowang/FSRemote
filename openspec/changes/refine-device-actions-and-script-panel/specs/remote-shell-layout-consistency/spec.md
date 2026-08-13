## ADDED Requirements

### Requirement: Detail and settings pages share content boundaries
The system SHALL use the same right-content left edge, width, top edge, and bottom safety boundary for the device detail page and the settings page in both expanded-sidebar and collapsed-sidebar states.

#### Scenario: Expanded sidebar boundaries match
- **WHEN** the left sidebar is expanded
- **AND** the user switches between device detail and settings pages
- **THEN** the main content panels align to the same left edge
- **AND** the main content panels use the same bottom safety boundary

#### Scenario: Collapsed sidebar boundaries match
- **WHEN** the left sidebar is collapsed
- **AND** the user switches between device detail and settings pages
- **THEN** the main content panels align to the same left edge
- **AND** the main content panels use the same bottom safety boundary

### Requirement: Detail content avoids collapse controls
The system SHALL prevent device detail content from drawing into the bottom area reserved for the sidebar collapse control and related lower controls.

#### Scenario: Device detail expanded state
- **WHEN** the device detail page is rendered with the sidebar expanded
- **THEN** script, config, and detail content remain above the shared bottom boundary
- **AND** the collapse control does not overlap the content

#### Scenario: Device detail collapsed state
- **WHEN** the device detail page is rendered with the sidebar collapsed
- **THEN** script, config, and detail content remain above the shared bottom boundary
- **AND** the collapse control does not overlap the content

### Requirement: Settings UI follows device-detail panel styling
The system SHALL render the settings page using panel geometry and spacing that visually matches the device detail page instead of using a separate lower or wider content area.

#### Scenario: Settings general tab layout
- **WHEN** the user opens the settings page
- **THEN** the settings content starts at the same vertical position as the device detail lower panel area
- **AND** the visible settings panel height stops at the same bottom boundary as the device detail page

#### Scenario: Settings keyboard tab layout
- **WHEN** the user opens the settings keyboard tab
- **THEN** the keyboard shortcut controls remain inside the shared settings viewport
- **AND** no control overlaps the collapse button or lower reserved area

### Requirement: Child widgets follow shared panel rectangles
The system SHALL update child widget geometry for config editing, script controls, settings controls, and shortcut editors from the same shared panel rectangles used by painting and hit testing.

#### Scenario: Resize or collapse updates child widgets
- **WHEN** the window size changes or the sidebar collapse state changes
- **THEN** visible child widgets move to the same rectangles as the painted panels
- **AND** hidden page widgets remain hidden outside their active page
