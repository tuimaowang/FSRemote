## ADDED Requirements

### Requirement: Bounded video handoff
The controller SHALL keep at most one pending BGRA frame and one pending texture descriptor per remote window, and SHALL discard or overwrite stale frames instead of growing the Qt event queue.

#### Scenario: Decoder outpaces UI
- **WHEN** a viewer decodes frames faster than its remote window can present them
- **THEN** the window SHALL present the newest safely accepted frame, SHALL discard frames that cannot acquire bounded ownership, and SHALL keep its pending-frame memory and queued presentation work bounded

#### Scenario: Shared texture slot is occupied
- **WHEN** the window already owns one pending keyed-mutex texture and another decoded texture arrives
- **THEN** the new texture SHALL be reported as a controlled drop, SHALL return immediately to the producer without BGRA readback, and SHALL NOT overwrite a descriptor whose consumer key still requires release

#### Scenario: Twenty 60 FPS viewers
- **WHEN** twenty viewers each deliver texture frames at 60 FPS
- **THEN** each window SHALL have at most one scheduled texture-drain task and stale frames SHALL NOT accumulate as individual Qt tasks

#### Scenario: New texture is not ready or cannot be presented
- **WHEN** a window has already presented a valid remote frame and the next shared texture is not ready, stale, or fails a transient presentation operation
- **THEN** the window SHALL keep displaying the last successfully presented frame until a newer texture or software frame is successfully committed, and SHALL NOT expose the black background as a transition frame

#### Scenario: Interactive window resize
- **WHEN** the user drags a remote window edge through multiple temporary sizes
- **THEN** the presenter SHALL preserve the last successful content, SHALL continue presenting newest accepted frames, SHALL throttle `ResizeBuffers` to a bounded live cadence instead of every temporary geometry, and SHALL apply the exact final buffer size together with the next successful frame presentation

#### Scenario: Interactive window move
- **WHEN** the user drags a remote window title bar
- **THEN** the window SHALL prefer the platform system move loop, SHALL suspend parent-widget backing-store painting while native video presentation continues, SHALL avoid a persistent Qt window Region on platforms with native compositor corners, SHALL temporarily remove a fallback Region during movement, and SHALL update overlay/snap composition only when state changes

### Requirement: Viewer generation isolation
The controller SHALL associate asynchronous frames, status updates, and stop completion with the viewer generation that created them.

#### Scenario: Reconnect while old callbacks remain
- **WHEN** a remote window replaces a viewer connection while callbacks from the old viewer can still arrive
- **THEN** the window SHALL ignore old-generation callbacks and SHALL NOT present them into the new session

#### Scenario: Close with pending frames
- **WHEN** a remote window closes with pending decoder or UI work
- **THEN** pending work SHALL be invalidated before the window is destroyed and SHALL NOT access released presentation resources

### Requirement: Failure containment
The controller SHALL contain a viewer connection, decoder, audio, allocation, or presentation failure to the affected remote window.

#### Scenario: One decoder fails
- **WHEN** one of twenty viewers reports a decoder exception or unrecoverable decode error
- **THEN** that window SHALL enter a recoverable or failed state and the other nineteen viewers SHALL remain connected and usable

#### Scenario: D3D11 device removal
- **WHEN** a presentation device is removed or reset
- **THEN** the controller SHALL keep the remote sessions connected, recreate affected presentation resources, and avoid terminating the application

#### Scenario: Texture path falls back to software presentation
- **WHEN** repeated texture presentation failures require bounded BGRA fallback
- **THEN** the controller SHALL paint the first valid BGRA fallback frame behind the existing texture presenter before hiding it so the transition preserves the last valid content

#### Scenario: Keyed mutex wait stalls
- **WHEN** a consumer keyed mutex cannot be acquired within the bounded presentation interval
- **THEN** the UI thread SHALL return without an unbounded wait, SHALL NOT release a key it did not acquire, SHALL preserve the last successful frame, and SHALL record a low-frequency diagnostic under the application `data` directory

#### Scenario: Keyed mutex ownership is abandoned
- **WHEN** either producer or consumer receives an abandoned keyed-mutex result
- **THEN** that side SHALL treat the shared-texture group as device-lost, SHALL stop reusing uncertain ownership, and SHALL enter the existing recreation or fallback path without blocking unrelated viewers

### Requirement: Bounded startup and shutdown
The controller SHALL pace expensive viewer initialization without limiting the number of active viewers and SHALL use joinable, cancellable lifecycle work for stop and shutdown.

#### Scenario: Open twenty devices at once
- **WHEN** the user requests twenty remote windows simultaneously
- **THEN** all windows SHALL appear immediately, initialization SHALL run with a bounded concurrency limit, and successful viewers SHALL remain active while later viewers initialize

#### Scenario: Close all windows
- **WHEN** the user closes all remote windows or exits the application
- **THEN** the controller SHALL stop accepting new lifecycle work, invalidate callbacks, close connections, join managed work, and release resources without a deadlock or process crash

### Requirement: Twenty-window stability verification
The project SHALL provide repeatable verification for twenty simultaneous remote windows and SHALL record enough diagnostics to identify viewer-scoped failures.

#### Scenario: Soak and churn
- **WHEN** twenty viewers run through sustained streaming, minimize/restore, disconnect/reconnect, close/reopen, and application-exit tests
- **THEN** the process SHALL remain alive, queue depth SHALL remain bounded, and memory, thread, and handle counts SHALL not grow without recovery

#### Scenario: Diagnostic volume under sustained streaming
- **WHEN** multiple viewers stream normally for an extended period
- **THEN** diagnostics SHALL record only state changes and rate-limited failures under the application `data` directory and SHALL NOT write one log entry per frame
