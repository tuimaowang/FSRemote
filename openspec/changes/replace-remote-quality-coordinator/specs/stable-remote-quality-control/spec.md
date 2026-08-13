## ADDED Requirements

### Requirement: Static quality roles
The controller SHALL resolve each registered remote window to exactly one static role: single-window foreground, multi-window foreground, multi-window background, minimized/hidden safety, or software-fallback safety.

#### Scenario: One visible remote window
- **WHEN** exactly one remote window is registered, visible, and not minimized
- **THEN** the coordinator SHALL return the single-window foreground preset without requiring Qt focus

#### Scenario: Multiple visible remote windows
- **WHEN** two or more remote windows are registered and one visible non-minimized window is focused
- **THEN** the coordinator SHALL return the multi-window foreground preset only for that focused window and the multi-window background preset for other visible windows

### Requirement: Stable foreground FPS
The coordinator SHALL keep the target FPS of a foreground role fixed at its configured preset and SHALL NOT rewrite it from receive FPS, decoder timing, packet loss, jitter, or presenter-drop statistics.

#### Scenario: Healthy foreground statistics
- **WHEN** a foreground window reports healthy or low receive FPS statistics
- **THEN** the coordinator SHALL keep the configured foreground target FPS unchanged

#### Scenario: Pressure statistics
- **WHEN** a foreground window reports decoder, network, freeze, or presenter pressure
- **THEN** the coordinator SHALL keep the same quality request and leave adaptive response to WebRTC/Host safety controls

### Requirement: Background and safety separation
The coordinator SHALL apply background and safety presets independently from the foreground presets.

#### Scenario: Background window remains visible
- **WHEN** a non-focused window remains visible and not minimized
- **THEN** the coordinator SHALL keep its configured background resolution, FPS, bitrate, and low priority unchanged until its role changes

#### Scenario: Window is minimized or hidden
- **WHEN** a window becomes minimized or hidden
- **THEN** the coordinator SHALL apply the configured minimized resolution and FPS and SHALL mark the decision as minimized

#### Scenario: Software presenter fallback
- **WHEN** a window reports software fallback active
- **THEN** the coordinator SHALL apply the bounded software-fallback resolution and FPS regardless of foreground role

### Requirement: Event-stable quality requests
The quality coordinator SHALL produce identical decisions for identical window-role snapshots and SHALL NOT maintain per-window degradation or recovery timers.

#### Scenario: Repeated evaluation without role changes
- **WHEN** the same window snapshot is evaluated repeatedly
- **THEN** the returned effective quality fields SHALL remain identical and SHALL not advance an internal FPS state

#### Scenario: Focus changes
- **WHEN** focus moves from one visible remote window to another
- **THEN** exactly the affected windows SHALL receive new foreground/background role decisions and all unchanged windows SHALL retain their previous decisions

### Requirement: Protocol compatibility
The controller SHALL continue using the existing versioned viewer-quality request and acknowledgement path without reconnecting the remote session.

#### Scenario: Static quality change is applied
- **WHEN** a role decision changes resolution, FPS, bitrate, or priority
- **THEN** the existing online quality request SHALL be sent and the video/input session SHALL remain connected
