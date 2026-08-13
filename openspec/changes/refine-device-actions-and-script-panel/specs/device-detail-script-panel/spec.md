## ADDED Requirements

### Requirement: Script Log tab provides a selectable script tree
The system SHALL display a tree on the left side of the Script Log tab that represents script paths from the configured shared script root, and the user SHALL be able to select a script path for execution.

#### Scenario: Script tree renders shared script paths
- **WHEN** the device detail page is open on the Script Log tab
- **THEN** the left side of the script panel displays a tree rooted at the configured shared script location
- **AND** child folders or script path nodes are displayed hierarchically when available

#### Scenario: User selects script from tree
- **WHEN** the user selects a valid script path in the tree
- **THEN** the selected script path becomes the current execution target for the active detail device
- **AND** the script log header reflects the selected script target

### Requirement: Script Log output is selectable and copyable
The Script Log output SHALL be displayed as read-only text that supports standard text selection and clipboard copying without allowing the log to be edited.

#### Scenario: Copy part of a script log
- **WHEN** the user selects text in the Script Log output and invokes Ctrl+C or the standard context-menu copy action
- **THEN** the selected plain text SHALL be placed on the clipboard
- **AND** live log output SHALL remain read-only

#### Scenario: Live output updates during selection
- **WHEN** new script output arrives while the user has selected existing log text
- **THEN** the UI SHALL preserve the selection when its positions remain within the retained log text
- **AND** it SHALL NOT force the view to the newest line while the user is reviewing older output

### Requirement: Script Log tab has separate Execute and Stop buttons
The system SHALL replace the single run/stop toggle with two distinct controls: one Execute button and one Stop button.

#### Scenario: Execute selected script
- **WHEN** a valid script path is selected in the script tree
- **AND** the user clicks Execute
- **THEN** the system starts script execution for the active detail device using the selected script path
- **AND** the script log shows the running state for that device

#### Scenario: Stop running script
- **WHEN** a script is running for the active detail device
- **AND** the user clicks Stop
- **THEN** the system stops the running script for that device
- **AND** the script log updates to show that execution is no longer running

#### Scenario: Execute without a valid script selection
- **WHEN** no valid script path is selected
- **AND** the user clicks Execute
- **THEN** the system does not start a script execution
- **AND** the UI remains on the Script Log tab without changing device selection

### Requirement: Config File editor uses dark editor styling
The system SHALL render the Config File editor with a black background and white text while preserving the existing file title, loading state, save affordance, and editable text behavior.

#### Scenario: Config editor visual style
- **WHEN** the device detail page is open on the Config File tab
- **THEN** the file editor text area uses a black background
- **AND** editable file text uses white foreground text

#### Scenario: Config save remains available
- **WHEN** a config file is loaded and edited
- **THEN** the save control remains visible and callable
- **AND** saving uses the existing config-file save flow

### Requirement: Script controls and tree are scoped to the active detail device
The system SHALL keep script selection, script output, running state, and editor state isolated per active detail device where existing per-device script UI state already applies.

#### Scenario: Switch active device
- **WHEN** the user changes the active detail device
- **THEN** the Script Log tab loads the script output and selected script target associated with the new device when available
- **AND** script state from the previous device is not displayed as the new device's state

#### Scenario: Return to previous device
- **WHEN** the user returns to a device that had script UI state
- **THEN** the Script Log tab restores that device's script output and selected script target where available
