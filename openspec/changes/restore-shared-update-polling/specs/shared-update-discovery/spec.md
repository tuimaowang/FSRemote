## ADDED Requirements

### Requirement: Shared marker is the latest-release authority
The system SHALL determine the latest available FSRemote release from the shared `FSRemote.version` marker and SHALL NOT treat a peer-advertised version as authoritative update availability.

#### Scenario: Shared version is newer
- **WHEN** a device confirms that the shared version is newer than its installed version
- **THEN** the device displays its local update entry

#### Scenario: Peer reports a newer version
- **WHEN** a realtime peer reports an installed version newer than the local installed version
- **THEN** the system does not display a local update entry solely from that peer report

### Requirement: Shared version is checked every twenty seconds
The system SHALL schedule an asynchronous shared-version check at startup and every 20 seconds while update checking is running.

#### Scenario: Program starts
- **WHEN** FSRemote starts update checking
- **THEN** it schedules an immediate authoritative shared-version check

#### Scenario: Program remains running
- **WHEN** 20 seconds elapse after the periodic timer starts
- **THEN** the system requests another asynchronous authoritative shared-version check

### Requirement: Realtime snapshots carry only sender update facts
The realtime device snapshot SHALL publish the sender's installed version and runtime-repair requirement, and SHALL NOT use peer release knowledge to trigger shared-version checks.

#### Scenario: Target publishes its installed version
- **WHEN** a target device publishes a realtime snapshot
- **THEN** controllers can compare its installed version with their locally confirmed shared release

#### Scenario: Older peer includes release knowledge
- **WHEN** a mixed-version peer sends a snapshot containing a legacy `knownReleaseVersion` field
- **THEN** the receiver preserves normal snapshot compatibility but does not trigger an update check from that field

### Requirement: Remote update buttons use locally confirmed release state
The system SHALL show a remote-device update entry only when the controller has confirmed a shared release and that release is newer than the target installed version, or when the versions are equal and the target reports a runtime-repair requirement.

#### Scenario: Target is behind confirmed release
- **WHEN** the controller has confirmed version `1.1.120` and a target reports installed version `1.1.119`
- **THEN** the target's remote update entry is visible

#### Scenario: Target version is unknown
- **WHEN** the controller has a confirmed shared release but the target has not reported a valid installed version
- **THEN** the target's remote update entry remains hidden
