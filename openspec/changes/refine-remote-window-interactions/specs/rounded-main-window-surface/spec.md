## ADDED Requirements

### Requirement: Rounded main-window corners are transparent
The frameless main window SHALL render its shell inside an antialiased rounded boundary and SHALL leave pixels outside that boundary transparent.

#### Scenario: Main window is normally displayed
- **WHEN** the main window paints its title bar, sidebar, content background, and border
- **THEN** the four outer corners show the desktop or underlying window instead of an opaque rectangular fill

#### Scenario: Main window is minimized and restored
- **WHEN** the user minimizes and restores the main window through the taskbar or tray
- **THEN** the transparent rounded corners and existing taskbar behavior remain unchanged
