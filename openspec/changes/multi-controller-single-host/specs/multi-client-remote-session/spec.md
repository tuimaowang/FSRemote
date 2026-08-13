## ADDED Requirements

### Requirement: Bounded concurrent remote sessions
The controlled device SHALL accept and maintain multiple authenticated remote sessions concurrently up to its configured session limit, and each session SHALL have an isolated signaling, WebRTC, status, and shutdown lifecycle.

#### Scenario: Three clients connect within the default limit
- **WHEN** three authorized clients connect to a controlled device configured for three sessions
- **THEN** all three clients are admitted and maintain independent remote sessions

#### Scenario: One client disconnects while others remain
- **WHEN** one of several admitted clients disconnects or its transport fails
- **THEN** only that session is removed and the remaining sessions continue receiving remote media

#### Scenario: Client exceeds the session limit
- **WHEN** another authorized client connects after the configured session limit is reached
- **THEN** the host rejects that client with a capacity reason without disturbing admitted sessions

### Requirement: Shared host media production
The controlled device SHALL create at most one active virtual-display session, one desktop capture source, and one system-audio capture source for all admitted remote sessions.

#### Scenario: Additional viewer joins an active host
- **WHEN** a second admitted client begins receiving remote media
- **THEN** the host attaches that client to the existing desktop and audio sources instead of starting duplicate capture sources

#### Scenario: Last viewer disconnects
- **WHEN** the final media subscriber disconnects
- **THEN** the host stops or idles the shared media pipeline only after all session references have been released

### Requirement: Independent media delivery under a host budget
The controlled device SHALL provide each admitted client with an independent PeerConnection and sender while enforcing configured aggregate bitrate, encoder-capacity, and queue limits.

#### Scenario: Aggregate bitrate is divided among active viewers
- **WHEN** multiple viewers are active and their unconstrained targets would exceed the host aggregate video budget
- **THEN** the host assigns bounded per-session targets whose sum does not exceed the configured aggregate limit

#### Scenario: One viewer cannot keep up
- **WHEN** one viewer's transport or audio queue becomes slow
- **THEN** the host drops or degrades data for that viewer without blocking media delivery to other viewers

#### Scenario: Encoder capacity is exhausted
- **WHEN** a newly admitted session cannot create a required encoder within host capacity
- **THEN** that session receives an explicit capacity failure and existing sessions continue normally

### Requirement: Multi-session status reporting
The system SHALL report active session count, viewer count, control-owner presence, and capacity to updated clients while preserving the existing leading online/busy status token for older status readers.

#### Scenario: Viewers are connected without a controller
- **WHEN** one or more view-only sessions are connected and no session owns control
- **THEN** the device reports an online-compatible prefix and appended viewer and capacity fields

#### Scenario: A controller owns input
- **WHEN** an admitted session holds control ownership
- **THEN** the device reports a busy-compatible prefix and appended ownership and session-count fields

