## ADDED Requirements

### Requirement: Authenticated sessions receive shared control by default
The controlled device SHALL grant keyboard and mouse authority to every authenticated admitted session that requests the `control` role and negotiates the `control` capability, up to the configured session limit.

#### Scenario: First controller connects
- **WHEN** an authenticated control-capable session is admitted
- **THEN** the host grants shared control and the remote window enables input forwarding

#### Scenario: Another controller connects while control is active
- **WHEN** another authenticated control-capable session is admitted while one or more controllers are connected
- **THEN** the host also grants shared control without revoking, pausing, or disconnecting existing controllers

### Requirement: Shared control authorization remains session scoped
The system SHALL retain the control permission independently for each admitted session and SHALL support revoking or ending one session without changing another controller's permission.

#### Scenario: One controller disconnects
- **WHEN** one of several shared controllers disconnects
- **THEN** the host removes only that session's authorization and input state while the remaining controllers continue operating

#### Scenario: Controlled-device administrator revokes one controller
- **WHEN** the controlled-device administrator revokes a selected session
- **THEN** the host disables and cleans up that session without revoking other admitted controllers

### Requirement: Unauthorized input is never injected
The controlled device MUST inject keyboard, mouse, wheel, and relative-mouse messages only when they originate from an authenticated admitted session with the negotiated control capability, and accepted input MUST be serialized through one input dispatcher.

#### Scenario: Viewer sends an input message
- **WHEN** a connected view-only, unauthenticated, or revoked session sends a keyboard or mouse message
- **THEN** the host rejects the message, does not call operating-system input injection, and returns a permission status to that session

#### Scenario: Multiple controllers send input concurrently
- **WHEN** two or more authorized controllers send input concurrently
- **THEN** the host establishes one deterministic dispatcher order and invokes operating-system input injection serially in that order

### Requirement: Per-session cleanup prevents stuck or prematurely released input
The controlled device SHALL track pressed keys and mouse buttons per authorized controller and SHALL release only that session's contribution when its permission or connection ends.

#### Scenario: Controller disconnects while holding unique input
- **WHEN** a controller disconnects with keys or mouse buttons that no other session holds
- **THEN** the host emits the corresponding releases and clears that session's recorded state

#### Scenario: Two controllers hold the same key
- **WHEN** two controllers hold the same key or button and one controller releases or disconnects
- **THEN** the host keeps the operating-system input pressed until the final holding controller releases or disconnects

#### Scenario: Host shuts down with active controllers
- **WHEN** the host begins shutdown while one or more sessions hold input
- **THEN** all session-attributed input cleanup completes before the input dispatcher and session manager are destroyed

### Requirement: Viewer UI reflects shared control permission
An updated remote window SHALL clearly distinguish view-only, shared-control, and revoked states and SHALL suppress local input forwarding whenever its session lacks control permission.

#### Scenario: Control is granted to a viewer
- **WHEN** an admitted remote window receives a shared-control-granted event
- **THEN** it enables input forwarding and displays that simultaneous control is enabled

#### Scenario: Control is revoked
- **WHEN** a remote window receives a control-revoked event
- **THEN** it releases locally tracked keys, disables further input forwarding, and remains connected for viewing
