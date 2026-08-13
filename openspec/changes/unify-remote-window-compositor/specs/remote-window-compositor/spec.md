## ADDED Requirements

### Requirement: Single visible surface ownership

The remote window compositor SHALL own the final visible pixels for remote video, letterboxing, title-bar content, performance information, connection overlays, and update masks through one coordinated presentation surface.

#### Scenario: Normal hardware presentation
- **WHEN** a decoded D3D11 shared texture is accepted for presentation
- **THEN** the compositor SHALL combine the remote content and all currently visible local UI layers into one visible frame without exposing a separately painted parent or overlay surface.

#### Scenario: Local UI state changes
- **WHEN** title-bar state, performance text, connection state, or an update mask changes
- **THEN** the compositor SHALL update its retained local layer and present a complete frame without hiding the remote content surface.

### Requirement: Stable interactive resize

The compositor SHALL preserve a valid visible frame throughout interactive window resizing and SHALL NOT clear the visible content as a side effect of an intermediate resize event.

#### Scenario: Continuous edge drag
- **WHEN** Windows delivers a sequence of intermediate resize geometries
- **THEN** the compositor SHALL keep the last valid frame or a newer valid frame visible for every intermediate geometry and SHALL defer destructive buffer reallocation until finalization.

#### Scenario: Resize finalization
- **WHEN** the interactive resize loop ends
- **THEN** the compositor SHALL apply the final output geometry once, present the retained latest frame at that geometry, and return to the normal presentation state.

### Requirement: Geometry and coordinate consistency

The compositor SHALL use one geometry snapshot for output placement, DPI scaling, title-bar hit testing, remote input mapping, and performance-layer placement.

#### Scenario: DPI-aware resize
- **WHEN** the window moves between DPI contexts or changes size under a non-100% scale factor
- **THEN** the compositor SHALL derive the native output rectangle and input mapping from the same physical-pixel geometry snapshot.

#### Scenario: Remote input during resize
- **WHEN** a mouse or keyboard event is received while the window is being resized
- **THEN** the compositor SHALL map the event using the current committed content rectangle and SHALL NOT use a stale child-window geometry.

### Requirement: Hardware and software presentation parity

The compositor SHALL provide hardware D3D11 presentation and software BGRA fallback with the same visible layout, content rectangle, title-bar behavior, and input hit regions.

#### Scenario: Hardware presenter failure
- **WHEN** the D3D11 device, shared texture, or SwapChain cannot present a frame
- **THEN** the compositor SHALL retain the last valid visible frame when possible, transition to software fallback under the existing recovery policy, and SHALL NOT expose an uninitialized surface.

#### Scenario: Recovery to hardware
- **WHEN** a later hardware frame is successfully presented after fallback
- **THEN** the compositor SHALL switch back to the hardware surface atomically and SHALL preserve the same layout and input regions.

### Requirement: Observable visible-result diagnostics

The compositor SHALL expose test-only diagnostics that correlate presentation state with the actual visible result rather than relying only on event counts.

#### Scenario: Resize diagnostic sampling
- **WHEN** the resize diagnostic mode is enabled
- **THEN** the system SHALL record sample time, committed geometry, source/output size, frame identifier, compositor state, and a visible-region pixel statistic or captured sample.

#### Scenario: Diagnostic mode disabled
- **WHEN** the application runs without the resize diagnostic mode
- **THEN** the compositor SHALL avoid continuous screenshot capture and SHALL retain only low-cost counters and error state.

### Requirement: Controlled migration and rollback

The new compositor SHALL be guarded by a controlled rollout switch until resize, title-bar, DPI, full-screen, multi-window, reconnect, and fallback verification is complete.

#### Scenario: New compositor disabled
- **WHEN** the rollout switch is disabled
- **THEN** the existing rendering path SHALL remain available without changing stream protocol or persisted device settings.

#### Scenario: New compositor enabled
- **WHEN** the rollout switch is enabled
- **THEN** all visible layers for the remote window SHALL use the new compositor and the application SHALL expose a diagnostic identifier for the active path.
