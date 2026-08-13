## ADDED Requirements

### Requirement: Native frame ownership
The video pipeline SHALL represent every decoded native frame with session identity, viewer generation, frame identity, timing metadata, native texture ownership, and an explicit release lease before the frame can cross a thread boundary.

#### Scenario: Native frame accepted
- **WHEN** a decoder produces a valid native texture for the current viewer generation
- **THEN** the pipeline creates a lease-backed frame envelope and submits it without invoking Qt or a UI presenter from the decoder thread

#### Scenario: Native frame rejected
- **WHEN** a frame is stale, invalid, or cannot enter the bounded handoff
- **THEN** the pipeline records a typed drop reason and returns the producer lease without blocking the decoder thread

### Requirement: Bounded latest-frame delivery
Each session SHALL keep at most one in-flight frame and one pending newest frame, and replacing a pending frame SHALL release the replaced frame's lease before it can be reused by the producer.

#### Scenario: New frame replaces pending frame
- **WHEN** a new frame arrives while an older frame is pending and not yet rendered
- **THEN** the older frame is released as stale and the new frame becomes the pending frame

#### Scenario: Render worker is delayed
- **WHEN** the render worker is delayed by another window or a surface operation
- **THEN** the session does not accumulate an unbounded queue and does not replay frames older than the newest pending frame

### Requirement: Dedicated adapter render worker
Each active GPU adapter SHALL have a render worker that owns its D3D11 presentation context, imported resources, processor state, SwapChains, and Present calls; the UI thread SHALL not perform per-frame GPU presentation.

#### Scenario: Frame presentation
- **WHEN** a session has an eligible pending frame and its presentation deadline is due
- **THEN** the adapter render worker performs synchronization, processing, and Present for that session

#### Scenario: UI event loop is busy
- **WHEN** the Qt UI thread is processing input, resize, or title-bar work
- **THEN** video rendering continues through the adapter render worker without posting one UI task per decoded frame

### Requirement: Reused presentation resources
The pipeline SHALL reuse imported textures, processor views, and SwapChain resources until a resource identity, format, source size, surface size, or device generation requires recreation.

#### Scenario: Consecutive frames use the same output group
- **WHEN** consecutive frames use the same session generation and compatible texture group
- **THEN** the worker reuses cached resources and does not recreate all presentation views for every frame

#### Scenario: Resource group changes
- **WHEN** the decoder generation, source format, source size, or device generation changes
- **THEN** the worker retires incompatible resources and creates a new cache entry before presenting the new group

### Requirement: Focused and background profiles
The scheduler SHALL reserve the focused visible window for the selected high-quality profile with a local target of 60 FPS, and SHALL assign visible background windows a fixed 1280x720/30 FPS profile during normal operation.

#### Scenario: One visible window
- **WHEN** exactly one connected remote window is visible and focused
- **THEN** it uses the focused high-quality profile without a background cap

#### Scenario: Multiple visible windows
- **WHEN** two or more connected remote windows are visible
- **THEN** the focused window keeps the focused profile and every other visible window uses the fixed background 1280x720/30 FPS profile

#### Scenario: Minimized window
- **WHEN** a remote window is minimized or otherwise not renderable
- **THEN** it uses the minimized safety profile and does not consume the focused window's presentation budget

### Requirement: Pressure-aware emergency degradation
The scheduler SHALL enter emergency degradation only after sustained evidence of resource pressure and SHALL reduce background service before reducing the focused window's resolution or frame rate.

#### Scenario: Background pressure
- **WHEN** at least two pressure indicators remain true for approximately one second
- **THEN** background windows move through the configured emergency profile before the focused window is downgraded

#### Scenario: Healthy recovery
- **WHEN** pressure indicators remain healthy for approximately three seconds
- **THEN** the scheduler restores one profile step at a time with a cooldown that prevents oscillation

### Requirement: Last successful frame retention
The pipeline SHALL retain the last successfully presented frame when a new frame fails synchronization, processing, Present, resize, reconnect, or transient resource creation.

#### Scenario: Transient frame failure
- **WHEN** a pending frame fails without device loss
- **THEN** the worker releases that frame, leaves the last successful surface visible, and waits for the next newest frame

#### Scenario: Device loss
- **WHEN** the adapter reports a device-removed or device-reset condition
- **THEN** the affected adapter resources are rebuilt without terminating unrelated sessions, and the last surface remains visible until recovery or explicit session close

### Requirement: Deterministic session teardown
Closing a session SHALL stop new frame admission, release pending and in-flight frame leases through the render worker, stop decoder resources, and then destroy the UI façade.

#### Scenario: Window closes during rendering
- **WHEN** a window closes while a frame is queued or being rendered
- **THEN** the generation becomes stale, pending work is released safely, and no later decoder callback accesses the destroyed window
