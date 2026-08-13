## ADDED Requirements

### Requirement: Realtime update metadata
The system SHALL publish the installed FSRemote version, local runtime dependency repair requirement, and latest authoritatively confirmed shared release version as optional fields in the existing complete realtime device snapshot.

#### Scenario: Local update metadata changes
- **WHEN** the local installed version, repair requirement, or confirmed shared release version changes
- **THEN** the system publishes a new complete realtime snapshot using the existing immediate retransmission behavior

#### Scenario: Older peer omits update metadata
- **WHEN** a valid protocol-version-1 snapshot contains no update object
- **THEN** the receiver accepts its presence, session, and script state while treating its update metadata as unknown

### Requirement: Authoritative remote update-button derivation
The controller SHALL derive a target's normal update-button visibility from realtime target metadata and a shared release version that the controller itself has confirmed.

#### Scenario: Target version is behind
- **WHEN** the controller has confirmed release `1.2.4` and a target publishes installed version `1.2.3`
- **THEN** the controller displays the target update button without querying `update_status`

#### Scenario: Target requires same-version repair
- **WHEN** the confirmed release and target installed version are equal and the target publishes that runtime dependency repair is required
- **THEN** the controller displays the target update button

#### Scenario: Target is current and complete
- **WHEN** the confirmed release and target installed version are equal and the target publishes that runtime dependency repair is not required
- **THEN** the controller hides the target update button

#### Scenario: Controller has no confirmed release
- **WHEN** shared release confirmation is unavailable or empty
- **THEN** the controller does not infer an installable update from peer metadata alone

#### Scenario: Target has not confirmed the new release yet
- **WHEN** the controller has confirmed a newer shared release and the target publishes an older installed version but an older known release version
- **THEN** the controller still displays the target update button

### Requirement: Peer release hints require shared confirmation
The system SHALL treat a higher shared release version observed in a peer snapshot only as a trigger for the existing authoritative shared-storage check.

#### Scenario: Peer advertises a higher known release
- **WHEN** a valid configured peer publishes a known release version higher than the receiver's cached confirmed release
- **THEN** the receiver schedules an asynchronous shared-version check

#### Scenario: Shared confirmation fails
- **WHEN** the receiver cannot confirm the hinted version from shared storage
- **THEN** it does not mark the hinted release installable and does not start an update

### Requirement: Event-driven normal update discovery
The system SHALL NOT perform periodic per-device `update_status` queries solely to maintain normal remote update-button visibility.

#### Scenario: Remote windows remain open
- **WHEN** one or more remote windows are open and no update transaction is active
- **THEN** update-button state is maintained from realtime snapshots without a ten-second TCP query loop

#### Scenario: User requests manual refresh
- **WHEN** the user explicitly requests device refresh
- **THEN** the system may issue a one-time `update_status` query for calibration and diagnostics

### Requirement: Reliable fallback discovery
The system SHALL perform an immediate authoritative shared-version check at application startup and SHALL retain a low-frequency fallback check for missed realtime notifications.

#### Scenario: Release notification is received
- **WHEN** a higher peer release hint arrives while shared storage is available
- **THEN** the receiver confirms and publishes the new known release without waiting for the fallback interval

#### Scenario: All release notifications are missed
- **WHEN** realtime release hints are lost or blocked
- **THEN** the fallback shared-version check eventually discovers the release

### Requirement: Explicit update transaction status remains reliable
The system SHALL retain the existing TCP update request and transaction status tracking after a user explicitly starts an update.

#### Scenario: Target exits during installation
- **WHEN** the target application exits and cannot publish realtime snapshots while the updater replaces files
- **THEN** the controller continues the explicit update state machine and reconnects after the new target process starts

#### Scenario: Stored failure is obsolete
- **WHEN** a target has a stored preparation failure but no update or runtime repair is currently required
- **THEN** `update_status` clears the obsolete failure and returns `complete`

#### Scenario: Target update cache is stale
- **WHEN** the controller sends an explicit update request with a confirmed version newer than the target installed version but the target has not yet refreshed its local update cache
- **THEN** the target accepts the request and the background preparation performs the normal authoritative shared-storage validation
