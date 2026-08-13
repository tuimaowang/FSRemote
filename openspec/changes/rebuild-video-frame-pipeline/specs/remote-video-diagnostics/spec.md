## ADDED Requirements

### Requirement: Correlated structured events
The video diagnostics system SHALL emit structured events with run ID, session ID, window ID, viewer generation, thread ID, adapter identity, event name, result, reason, and frame/render identifiers where applicable.

#### Scenario: Trace one frame
- **WHEN** a frame is decoded, queued, rendered, dropped, or released
- **THEN** the events share enough identity and monotonic timing fields to reconstruct the frame's cross-thread path

#### Scenario: Session generation changes
- **WHEN** a session reconnects or closes
- **THEN** later events carry the new generation or are explicitly marked stale and cannot be confused with the previous session

### Requirement: Low-overhead normal telemetry
Normal diagnostics SHALL write lifecycle/error events and periodic per-session and per-adapter aggregates without synchronous file IO in decoder or render hot paths.

#### Scenario: Healthy one-window stream
- **WHEN** a stream runs normally
- **THEN** logs contain periodic decoded/submitted/presented FPS, drop reasons, frame-age percentiles, decode/render timing, profile, and focus/visibility state

#### Scenario: Multiple active windows
- **WHEN** multiple windows share an adapter render worker
- **THEN** logs contain worker utilization, active session count, service time, cache hit/miss counts, and per-window presentation statistics

### Requirement: In-memory frame trace and anomaly dump
The diagnostics system SHALL retain a bounded in-memory frame-event ring and dump the recent history plus a short future window when configured latency, drop, synchronization, Present, device, or fallback thresholds are crossed.

#### Scenario: Latency anomaly
- **WHEN** frame-age p95 or drop ratio exceeds the configured threshold for the configured hold time
- **THEN** the diagnostic dump contains events before the anomaly, the triggering event, and recovery events after it

#### Scenario: Repeated synchronization failure
- **WHEN** synchronization repeatedly times out or reports abandoned ownership
- **THEN** the dump includes the session generation, resource identity, worker state, last successful frame, and recovery decision

### Requirement: Typed drop and recovery reasons
The logger SHALL record typed reasons for queue replacement, stale generation, background cap, minimized state, synchronization busy/timeout, resource import failure, device loss, Present failure, fallback, and clean shutdown.

#### Scenario: Background profile cap
- **WHEN** a background window does not present a frame because its 30 FPS deadline has not arrived
- **THEN** the event is identified as a profile/deadline decision rather than an unexplained drop

#### Scenario: Frame processing failure
- **WHEN** a frame cannot be synchronized, processed, or presented
- **THEN** the event includes the exact stage, result code, typed reason, and whether the last successful frame was retained

### Requirement: Bounded durable logs
Durable video logs SHALL be written under `data/video_pipeline/`, rotated by size/count, and capped in total storage; the logger SHALL remain usable when a log file cannot be opened.

#### Scenario: Log rotation
- **WHEN** a video log reaches its configured size
- **THEN** it is rotated without blocking the decoder or render worker and the total retained size remains bounded

#### Scenario: File IO failure
- **WHEN** the log directory or file is unavailable
- **THEN** counters and in-memory trace continue, the video pipeline continues, and a low-frequency diagnostic reports the logging failure

### Requirement: Diagnostic snapshot
The system SHALL support a snapshot that captures current pipeline state without changing video scheduling behavior.

#### Scenario: User requests a snapshot
- **WHEN** a diagnostic snapshot is requested for a window or the whole adapter
- **THEN** the system writes current profiles, queue state, timing aggregates, resource cache state, device status, and recent trace events
