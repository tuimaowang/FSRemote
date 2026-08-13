## ADDED Requirements

### Requirement: DeviceGrid remains a presentation coordinator
`DeviceGrid` SHALL coordinate presentation state, route user intent, and compose focused collaborators; it SHALL NOT remain the authoritative owner of device persistence, remote-window collections, device command execution, and duplicated script domain state.

#### Scenario: User invokes a device action
- **WHEN** the user selects one or more devices and invokes wake, shutdown, restart, update, terminal, script, or remote-control behavior
- **THEN** `DeviceGrid` resolves presentation selection and delegates the business operation to a focused collaborator

### Requirement: Remote-window orchestration has focused ownership
The system SHALL have one focused owner for ordinary and tiled remote-window collections, activation order, restore geometry, close-all behavior, and window-level lifecycle routing.

#### Scenario: User opens and retakes focus of a normal remote window
- **WHEN** the same target device is opened more than once through the normal remote-control entry point
- **THEN** the existing window is activated instead of creating an unintended duplicate

#### Scenario: Application exits with multiple remote windows
- **WHEN** application shutdown begins while ordinary or tiled viewer windows remain open
- **THEN** all windows unregister specialized coordinators, submit bounded viewer stops, and complete shutdown without callbacks reaching destroyed UI objects

### Requirement: Script state has one authoritative representation
The system SHALL maintain one per-device script-panel state representation and SHALL NOT require mirrored current-device member fields to be manually copied during selection changes.

#### Scenario: User switches devices while a script is active
- **WHEN** the visible device changes while another device has running or recoverable script state
- **THEN** each device retains isolated output, editor, run identity, and cancellation state without copying unrelated state into the newly selected device

### Requirement: Worker results are delivered safely
Extracted controllers and helpers MUST perform blocking work outside the UI thread and MUST guard UI delivery against application or widget destruction.

#### Scenario: Window is destroyed before background work completes
- **WHEN** a network, file, SSH, or status task completes after its UI owner has begun destruction
- **THEN** the result is discarded or delivered to a still-valid owner without dereferencing a destroyed object
