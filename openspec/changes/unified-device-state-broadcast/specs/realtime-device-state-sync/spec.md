## ADDED Requirements

### Requirement: Unified device state snapshot
The system SHALL publish one complete realtime state snapshot per local device that includes device identity, process boot identity, monotonic sequence, host remote-control sessions, verified script runtime state, and locally controlled targets.

#### Scenario: Device publishes a complete snapshot
- **WHEN** the application starts or any included local state changes
- **THEN** the system broadcasts a complete snapshot rather than an incremental count or start/stop delta

#### Scenario: One device acts as controller and host
- **WHEN** a device is simultaneously controlling another device and being controlled
- **THEN** both roles are represented in the same device snapshot without adding controller targets to the host session count

### Requirement: Event-driven broadcast with adaptive heartbeat
The system SHALL broadcast immediately when realtime state changes and SHALL send adaptive heartbeat snapshots so peers can detect silent failure.

#### Scenario: State changes while device is active
- **WHEN** a remote-control session or script state changes
- **THEN** the updated complete snapshot is broadcast immediately and retransmitted within a short bounded interval

#### Scenario: Active device heartbeat
- **WHEN** the device has a remote-control session, a running script, or a locally controlled target
- **THEN** the system sends heartbeat snapshots at approximately one-second intervals with an active TTL

#### Scenario: Idle device heartbeat
- **WHEN** the device has no active session, running script, or controlled target
- **THEN** the system sends heartbeat snapshots at a lower frequency with a longer idle TTL

### Requirement: Realtime online and offline state
The system SHALL derive device presence from the receipt time and advertised TTL of valid realtime snapshots.

#### Scenario: Valid snapshot received
- **WHEN** a valid snapshot is received from a configured device IP before its TTL expires
- **THEN** the device is displayed as Online or Busy without waiting for a TCP polling cycle

#### Scenario: Device silently disappears
- **WHEN** no valid snapshot is received before the device TTL expires
- **THEN** the device is displayed as Offline, its remote session count is cleared, and its script state becomes Unknown

#### Scenario: Device has not been observed
- **WHEN** the application has not received a realtime snapshot or manual status result for a device
- **THEN** the device remains Unknown rather than being assumed Online

### Requirement: Authoritative remote-control session state
The system SHALL calculate a device's controlled state and controller count only from the host session list published by that device.

#### Scenario: Host publishes active sessions
- **WHEN** a target device publishes two unique active host sessions
- **THEN** the device displays a controller count of two and lists the two controller identities

#### Scenario: Controller publishes a target lease
- **WHEN** a controller publishes that it is controlling a target but the target does not publish the corresponding host session
- **THEN** the target lease is retained for diagnostics but does not increase the target controller count

#### Scenario: Same controller reconnects
- **WHEN** the host replaces an older session from the same authorized controller with a new session
- **THEN** only the new active session is included in the next snapshot and the displayed count does not temporarily remain duplicated

### Requirement: Verified script runtime state
The system SHALL publish script state as Unknown, Idle, or Running based on the target device's local manifest and process validation.

#### Scenario: Script controller process is verified
- **WHEN** the active script manifest matches a live validated controller process
- **THEN** the device publishes Running with its run ID and work metadata and peers display the script-running indicator

#### Scenario: Target confirms no active script
- **WHEN** the target validates that no active or uncertain script marker remains
- **THEN** the device publishes Idle and peers remove the script-running indicator

#### Scenario: Script state cannot be verified
- **WHEN** the device is offline, the manifest is unreadable, or process validation cannot determine liveness
- **THEN** the script state is Unknown and peers do not overwrite it as confirmed Idle

### Requirement: Ordered and self-healing state reconciliation
The system SHALL reject stale snapshots and SHALL replace prior process state when a device restarts.

#### Scenario: Out-of-order snapshot arrives
- **WHEN** a snapshot has the same device ID and boot ID but a sequence not greater than the last accepted sequence
- **THEN** the snapshot is ignored

#### Scenario: Device process restarts
- **WHEN** a known device ID publishes a new boot ID
- **THEN** prior sessions, script state, controller targets, and sequence history for that device are cleared before applying the new snapshot

#### Scenario: A state-change datagram is lost
- **WHEN** an immediate state-change broadcast is lost
- **THEN** a later complete heartbeat snapshot restores the correct state without requiring an inverse event

### Requirement: Serialized local event processing
The system SHALL normalize network, timeout, host-session, script-runtime, and controller-target events through a bounded local queue and a single state reducer before notifying the UI.

#### Scenario: Network event arrives outside UI update logic
- **WHEN** the UDP receiver obtains a valid snapshot
- **THEN** it enqueues a normalized event and does not directly mutate DeviceGrid state

#### Scenario: Multiple snapshots queue for one device
- **WHEN** multiple unprocessed snapshots for the same device arrive
- **THEN** the queue may coalesce them to the newest valid snapshot while preserving bounded memory use

#### Scenario: Reduced state changes
- **WHEN** the reducer accepts a new or expired device state
- **THEN** it emits a final device-state notification that the UI consumes on its owning thread

### Requirement: Manual status calibration without periodic polling
The system SHALL use realtime broadcast as the automatic state source and SHALL retain explicit TCP status queries only for user-requested calibration and diagnostics.

#### Scenario: Application runs normally
- **WHEN** no user requests a manual refresh
- **THEN** the system does not perform periodic all-device TCP status polling

#### Scenario: User requests refresh
- **WHEN** the user clicks the refresh action
- **THEN** the system performs a one-time TCP status query and reconciles the returned state with the realtime state store

#### Scenario: User requests update for a device currently displayed Offline or Unknown
- **WHEN** the user explicitly chooses the remote update action
- **THEN** the controller attempts the existing TCP 49102 update command and uses the connection result instead of blocking solely on cached display presence

### Requirement: LAN broadcast scope and validation
The system SHALL send state datagrams on a dedicated UDP port to valid IPv4 directed-broadcast addresses, SHALL provide routed-private-subnet delivery through bounded UDP subscriptions, and SHALL validate received datagrams before state application.

#### Scenario: Device has multiple valid LAN interfaces
- **WHEN** the application publishes a snapshot
- **THEN** it sends the datagram to each eligible interface broadcast address without using loopback interfaces

#### Scenario: Configured device is in another routed subnet
- **WHEN** a configured target IP is outside every local IPv4 subnet
- **THEN** the controller renews a bounded UDP subscription and the target unicasts complete state snapshots to that subscriber without TCP polling

#### Scenario: Cross-subnet controller exits unexpectedly
- **WHEN** a subscriber stops renewing its lease
- **THEN** the target removes that subscriber after the bounded lease expires and stops sending directed snapshots to it

#### Scenario: Datagram is malformed or oversized
- **WHEN** a datagram exceeds the size limit, has an unsupported version, lacks required identifiers, or contains invalid fields
- **THEN** it is discarded without changing device state

#### Scenario: Datagram source is not a configured device
- **WHEN** a valid-looking snapshot arrives from an IP that cannot be mapped to a configured device
- **THEN** it does not create an unsolicited device-list entry or change an existing device's UI state

#### Scenario: Subscription arrives from a controller not listed as a managed device
- **WHEN** a valid private-IPv4 subscription arrives with a source-matching advertised IP
- **THEN** the target may retain only a bounded outbound subscriber lease and SHALL NOT create or mutate a DeviceGrid entry
