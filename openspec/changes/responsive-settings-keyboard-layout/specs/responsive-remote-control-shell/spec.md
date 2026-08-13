## ADDED Requirements

### Requirement: Detail header removes redundant capsule display
The system SHALL remove the device-detail status capsule, status icon, and adjacent device-name header text from the right-side detail area.

#### Scenario: Device detail selected
- **WHEN** a device is selected in the sidebar
- **THEN** the detail header does not render the previous status capsule
- **AND** the detail header does not render the previous adjacent device-name text

### Requirement: Settings page uses compact responsive content
The system SHALL remove the large Settings title and SHALL lay out Settings content from the same responsive content edge used by the device page when the sidebar is collapsed or expanded.

#### Scenario: Settings page expanded sidebar
- **WHEN** the sidebar is expanded and Settings is shown
- **THEN** Settings content starts to the right of the sidebar
- **AND** Settings content stretches to the current window width

#### Scenario: Settings page collapsed sidebar
- **WHEN** the sidebar is collapsed and Settings is shown
- **THEN** Settings content moves left like the device page content
- **AND** Settings content stretches across the newly available width

### Requirement: Main shell can be resized
The system SHALL allow the main shell window to be resized while preserving the left-sidebar width ratio/visual width and stretching content with the window.

#### Scenario: User resizes the shell
- **WHEN** the user changes the main window size
- **THEN** titlebar controls remain aligned to the right edge
- **AND** Settings and detail content stretch with the window
- **AND** the expanded sidebar keeps its established width

### Requirement: Batch add accepts multiple subnet patterns
The Settings batch-add input SHALL accept multiple IP wildcard subnet patterns by default and SHALL scan de-duplicated IPs from all valid patterns.

#### Scenario: Multiple wildcard subnets entered
- **WHEN** the user enters more than one wildcard subnet separated by whitespace, comma, semicolon, or newline
- **THEN** batch add scans all unique IPs expanded from those subnets

#### Scenario: Invalid subnet entered
- **WHEN** any entered subnet pattern is invalid
- **THEN** the system keeps the batch-add input focused for correction
- **AND** the scan does not start

### Requirement: Settings option icons are redesigned
The system SHALL render Settings option icons with a consistent new icon style independent of the old SVG option icons.

#### Scenario: Settings general tab displayed
- **WHEN** Settings general options are visible
- **THEN** each option row displays the new icon treatment aligned with its row text

### Requirement: Keyboard settings shows remote-window shortcuts
The Settings Keyboard tab SHALL show remote-window shortcut rows for fullscreen toggle, tile/restore toggle, close topmost remote window, and close all remote windows.

#### Scenario: Keyboard tab selected
- **WHEN** the user clicks the Keyboard tab in Settings
- **THEN** the Settings content switches to the keyboard shortcut view
- **AND** the shortcut rows include Ctrl+D, Ctrl+P, Ctrl+W, and Ctrl+F4 with their remote-window actions

### Requirement: Remote-window shortcuts execute window actions
The system SHALL execute remote-window shortcut actions from the shell or focused remote windows.

#### Scenario: Toggle fullscreen
- **WHEN** the user presses Ctrl+D while a remote-control window is active
- **THEN** the active remote-control window toggles between fullscreen and normal window mode

#### Scenario: Toggle tile layout
- **WHEN** the user presses Ctrl+P while remote-control windows are open
- **THEN** the current remote-control windows are tiled
- **AND** pressing Ctrl+P again restores their previous positions

#### Scenario: Close topmost remote window
- **WHEN** the user presses Ctrl+W while one or more remote-control windows are open and no custom shortcut has replaced the default
- **THEN** the topmost remote-control window closes

#### Scenario: Close all remote windows
- **WHEN** the user presses Ctrl+F4 while remote-control windows are open
- **THEN** all opened remote-control windows close

#### Scenario: Modifier release crosses remote-window activation
- **WHEN** a registered shortcut activates, rearranges, switches, or closes remote-control windows while Ctrl, Shift, Alt, or Win is still physically held
- **THEN** the keyboard hook suppresses only a key release whose matching key press was suppressed by the same active hook target
- **AND** a release whose press occurred before that hook target became active continues to the local Windows input chain
- **AND** the controlled device receives release coverage for generic and left/right modifier virtual-key variants without interrupting the stream

### Requirement: Titlebar displays local identity
The titlebar SHALL display the local IPv4 address and local device name to the left of the Settings icon.

#### Scenario: Local identity available
- **WHEN** local device info has been refreshed
- **THEN** the titlebar shows the local IP to the left of the local device name
- **AND** both labels remain left of the Settings icon without overlapping titlebar controls

### Requirement: Device detail uses configuration and log tabs
The device-detail lower content SHALL expose tabs named Configuration File and Script Log, and SHALL switch the visible lower panel according to the active tab while respecting sidebar collapse and window resize.

#### Scenario: Configuration tab selected
- **WHEN** the Configuration File tab is selected
- **THEN** the script file editor panel is shown in the responsive detail content area
- **AND** the script log panel is hidden

#### Scenario: Script log tab selected
- **WHEN** the Script Log tab is selected
- **THEN** the script log panel is shown in the responsive detail content area
- **AND** the script file editor panel is hidden
