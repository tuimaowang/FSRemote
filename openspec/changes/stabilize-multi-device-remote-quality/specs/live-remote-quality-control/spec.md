## ADDED Requirements

### Requirement: Live quality update
The controller SHALL update a viewer's requested bitrate, resolution, FPS, and priority over the existing remote session without stopping or recreating that viewer.

#### Scenario: Change quality while connected
- **WHEN** the global policy or current-window override changes during an active session
- **THEN** the viewer SHALL send the newest versioned quality request over the existing connection and input/media continuity SHALL be preserved

#### Scenario: Change quality while connecting
- **WHEN** quality changes before the control data channel is ready
- **THEN** the viewer SHALL retain only the newest pending request and SHALL send it after the session becomes ready

### Requirement: Versioned acknowledgement
The host SHALL validate quality requests and SHALL return the applied resolution, FPS, bitrate, mode, and any non-fatal limitation through a versioned acknowledgement.

#### Scenario: Supported request
- **WHEN** a host supports the requested live-quality protocol
- **THEN** it SHALL apply bounded sender/encoder parameters and acknowledge the effective result

#### Scenario: Unsupported request
- **WHEN** a host does not support live quality updates or rejects an invalid value
- **THEN** the existing stream SHALL continue and the controller SHALL report that the requested change was not applied

### Requirement: Explicit visible quality presets
Visible windows SHALL use one explicit resolution/FPS policy for their effective quality mode, while minimized windows SHALL use the common background profile and remain streamed and connected. When multiple visible windows are evaluated together, the largest-visible-window resource policy takes precedence over a window's saved mode for deciding which stream receives the native/60 reservation and which streams enter the background tiers.

#### Scenario: High-quality window is visible
- **WHEN** a visible window uses high-quality mode
- **THEN** the controller SHALL request original resolution and 60 FPS and SHALL NOT actively lower that request from receiver performance or aggregate-budget policy

#### Scenario: Automatic window is visible
- **WHEN** a visible window uses automatic mode
- **THEN** the controller SHALL preserve original resolution and MAY adapt only its requested FPS through 60, 45, and 30 FPS

#### Scenario: Balanced window is visible
- **WHEN** a visible window uses balanced mode
- **THEN** the controller SHALL request 1080p and 45 FPS without receiver-driven FPS transitions

#### Scenario: Smooth window is visible
- **WHEN** a visible window uses smooth mode
- **THEN** the controller SHALL request 720p and 60 FPS without receiver-driven FPS transitions

#### Scenario: Automatic window under sustained hardware pressure
- **WHEN** an automatic visible window remains below its current requested FPS and at least one decoder, network, freeze, jitter-buffer, or presenter-pressure signal persists for the degradation interval
- **THEN** the controller SHALL preserve original resolution and lower the requested FPS by one step through 60, 45, and 30 FPS

#### Scenario: Static or low-motion desktop
- **WHEN** the observed receive/display FPS is below the requested limit but decoder, network, freeze, jitter-buffer, and presenter-pressure signals remain healthy
- **THEN** the controller SHALL treat the low FPS as content demand rather than overload and SHALL NOT lower the requested FPS

#### Scenario: Quality modes select fixed clarity tiers
- **WHEN** a visible window uses high-quality, automatic, balanced, or smooth mode
- **THEN** its requested resolution SHALL remain original, original, 1080p, or 720p respectively until the mode, minimized state, or software-fallback state changes

#### Scenario: Minimized window
- **WHEN** a remote window becomes minimized
- **THEN** the controller SHALL request 540p at 15 FPS and SHALL continue receiving video and control connectivity

#### Scenario: Automatic resources recover
- **WHEN** a degraded automatic viewer remains stable for the recovery interval
- **THEN** the controller SHALL restore requested FPS toward 60 through the same fixed tiers without changing original resolution

#### Scenario: Window returns from a background or fallback profile
- **WHEN** a window leaves minimized or software-fallback state
- **THEN** the controller SHALL immediately return to the visible preset for its effective mode, with later automatic-only adaptation evaluated from fresh evidence

### Requirement: Fixed requests remain bounded by the transport
A fixed-preset window SHALL keep its controller request unchanged while remaining subject to WebRTC congestion control, Host encoder limits, and bounded software-fallback behavior.

#### Scenario: Fixed requests exceed available capacity
- **WHEN** one or more fixed-preset requests exceed available network, GPU, CPU, decoder, or Host encoder capacity
- **THEN** the transport or Host MAY apply a lower actual result, the controller SHALL report that actual result, and bounded queues SHALL prevent process instability without rewriting the selected visible preset

### Requirement: Focused-window resource policy
The controller SHALL select the visible, non-minimized remote window that Qt reports as the actual focused top-level window. That window SHALL receive native resolution, 60 FPS, and the highest priority; focus, not viewport size or full-screen state, SHALL select the reservation.

#### Scenario: Focus moves between remote windows
- **WHEN** focus leaves one visible remote window and enters another
- **THEN** the high-quality/native/60 request SHALL move to the newly focused window without reconnecting either session

#### Scenario: No remote window has focus
- **WHEN** the controller or another application owns focus
- **THEN** no remote window SHALL receive the focused high-quality reservation and visible windows SHALL use the current background policy

#### Scenario: Other visible windows
- **WHEN** a visible window is not the focused remote window
- **THEN** it SHALL keep the current low-priority background resolution/FPS policy

#### Scenario: Minimized or hidden window
- **WHEN** a remote window is minimized or hidden
- **THEN** it SHALL keep the configured minimized resolution/FPS profile and remain connected

### Requirement: Single online viewer-audio owner
The controller SHALL allow audio playback for at most one visible, non-minimized remote viewer, and that viewer SHALL be the truly focused remote window.

#### Scenario: Focus moves between remote windows
- **WHEN** focus leaves one remote window and enters another
- **THEN** the controller SHALL stop the old viewer audio and start the new viewer audio online without reconnecting either video session

#### Scenario: Controller loses remote-window focus
- **WHEN** the user focuses a non-remote application or no eligible remote window is active
- **THEN** every remote viewer SHALL have local audio playback disabled while video and control sessions remain connected

#### Scenario: Audio request arrives before admission
- **WHEN** the controller selects an audio owner before that viewer has completed capability negotiation
- **THEN** the viewer SHALL retain only the latest requested audio state and apply it after audio capability becomes available
