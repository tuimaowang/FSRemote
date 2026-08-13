## ADDED Requirements

### Requirement: Isolated lock-enter POC
The repository SHALL provide a standalone Windows POC that does not modify or link the production FSRemote executable, stream DLL, secure-desktop screenshot POC, installer, updater, startup behavior, drivers, or network ports.

#### Scenario: Production build remains unchanged
- **WHEN** the lock-enter POC folder is not explicitly configured and built
- **THEN** existing FSRemote build and runtime behavior remain unchanged

### Requirement: Explicit service lifecycle
The POC SHALL expose explicit administrator commands to install, start, query, stop, and uninstall a manual-start LocalSystem service, and SHALL NOT mutate SCM during an ordinary launch.

#### Scenario: Service start completes
- **WHEN** an administrator starts the installed POC service
- **THEN** the command waits until the active-session agent reports ready or returns a readiness error

### Requirement: Active-session SYSTEM agent
The service SHALL launch and supervise one SYSTEM agent in the active console session using a duplicated primary token from that session's `winlogon.exe`.

#### Scenario: Active console session changes
- **WHEN** the active console session changes or the owned agent exits
- **THEN** the service stops only its owned agent and launches a replacement for the active session

### Requirement: Genuine lock event safety gate
The service SHALL forward `WTS_SESSION_LOCK` and `WTS_SESSION_UNLOCK` only for the active agent session, and the agent MUST additionally confirm that the current input desktop is `Winlogon` before sending input.

#### Scenario: UAC secure desktop without workstation lock
- **WHEN** the input desktop is Winlogon but no matching WTS lock event was received
- **THEN** the agent does not send Enter

### Requirement: Exactly one Enter per lock cycle
After a qualifying lock event, the agent SHALL wait 500 ms and send exactly one FakerInput HID Enter key-down snapshot followed by one all-zero key-up snapshot.

#### Scenario: Passwordless device locks
- **WHEN** a matching lock event is received and the input desktop remains Winlogon after the delay
- **THEN** the agent sends HID usage `0x28` once, releases all keyboard state, and disarms further injection for that lock cycle

### Requirement: No credential or fallback input
The POC SHALL NOT store or submit credentials, send SAS or Ctrl+Alt+Del, inject mouse actions, repeatedly retry Enter, or fall back to `SendInput` after FakerInput failure.

#### Scenario: FakerInput is unavailable
- **WHEN** the SYSTEM agent cannot connect to or ping FakerInputBridge
- **THEN** it records the failure and sends no alternative input

### Requirement: Unlock outcome diagnostics
The POC SHALL record lock detection, selected desktop, bridge status, Enter key-down/key-up results, and whether an unlock event arrives within the diagnostic timeout.

#### Scenario: Enter unlocks the session
- **WHEN** Windows reports `WTS_SESSION_UNLOCK` after the one-shot Enter
- **THEN** the agent records success and rearms only after the session returns to the Default desktop

#### Scenario: Enter does not unlock the session
- **WHEN** no unlock is observed before timeout
- **THEN** the agent records timeout and remains disarmed until a later real unlock

### Requirement: Protected local events and safe cleanup
The POC SHALL restrict its named events to LocalSystem and local Administrators, and uninstall SHALL remove only the POC service registration.

#### Scenario: POC is removed
- **WHEN** an administrator stops and uninstalls the POC
- **THEN** production FSRemote, the screenshot POC, drivers, files, settings, and services remain untouched
