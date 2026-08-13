## ADDED Requirements

### Requirement: Isolated POC build
The repository SHALL provide a standalone Windows POC source folder and build target that does not modify or link the production FSRemote executable, stream DLL, installer, updater, startup entry, or network ports.

#### Scenario: Normal production build remains unchanged
- **WHEN** the POC folder is not explicitly configured and built
- **THEN** the existing FSRemote build and runtime behavior remain unchanged

### Requirement: Explicit service lifecycle
The POC SHALL expose explicit administrator commands to install, start, stop, query, and uninstall its Windows service, and SHALL NOT mutate the Service Control Manager during an ordinary launch or probe.

#### Scenario: Launch without a service command
- **WHEN** the executable is started without an install, start, stop, or uninstall command
- **THEN** it does not create, delete, start, or stop a Windows service

#### Scenario: Start waits for the session agent
- **WHEN** an administrator starts the POC service
- **THEN** the command reports success only after the active-session agent has created its protected capture and mouse-test events, or reports that the service is running but the agent did not become ready

### Requirement: Active-session SYSTEM agent
The POC service SHALL identify the active console session and launch one agent in that session using a duplicated primary token from that session's `winlogon.exe`.

#### Scenario: Active console session changes
- **WHEN** the active console session ID changes or the current agent exits
- **THEN** the service stops its owned agent and launches a replacement for the new active session

### Requirement: Desktop transition diagnostics
The agent SHALL record its process identity, SYSTEM status, session ID, window station, selected thread desktop, current input desktop, and every detected input-desktop change.

#### Scenario: Windows locks or unlocks
- **WHEN** the current input desktop name changes
- **THEN** the agent selects the new input desktop on its dedicated worker and records the old and new names

### Requirement: Diagnostic screenshot
The agent SHALL support an explicit screenshot request, attempt one-frame DXGI Desktop Duplication first, record the selected adapter/output and HRESULT, and fall back to a GDI capture when DXGI does not produce a frame.

#### Scenario: DXGI capture succeeds
- **WHEN** `DuplicateOutput` and `AcquireNextFrame` succeed on the selected input desktop
- **THEN** the agent writes a timestamped BMP and records that DXGI produced it

#### Scenario: DXGI capture fails
- **WHEN** DXGI initialization or frame acquisition fails
- **THEN** the agent records the exact HRESULT and attempts a timestamped GDI BMP capture

### Requirement: Explicit harmless input test
The agent SHALL perform input testing only after an administrator signals the dedicated request event and SHALL limit the test to a one-pixel relative mouse movement followed by the inverse movement.

#### Scenario: Mouse test requested
- **WHEN** an administrator signals the mouse-test event
- **THEN** the agent reports the independent FakerInputBridge and `SendInput` results without generating keyboard, button, wheel, SAS, clipboard, or credential input

### Requirement: Protected local control channel
The POC SHALL restrict its global capture, mouse-test, and stop control objects to LocalSystem and the local Administrators group.

#### Scenario: Non-administrator attempts to signal a command
- **WHEN** a process without an allowed token attempts to open a POC command event
- **THEN** Windows denies the requested event access

### Requirement: Durable isolated output
The service and agent SHALL write logs and screenshots under a POC-specific machine data directory and SHALL fall back to an explicitly supplied probe output directory for non-service validation.

#### Scenario: Service restarts or target reboots
- **WHEN** the POC process exits unexpectedly after writing a diagnostic line or completed BMP
- **THEN** previously flushed POC evidence remains available without relying on the FSRemote production log

### Requirement: Safe cleanup
The POC service SHALL stop only the agent process it created, and the uninstall command SHALL remove only the POC service registration.

#### Scenario: POC is uninstalled
- **WHEN** an administrator stops and uninstalls the POC
- **THEN** production FSRemote processes, services, files, drivers, settings, and startup entries are not modified
