## 1. Protocol and Configuration Foundations

- [x] 1.1 Define versioned admission, ownership-control, status, rejection, and scoped audio-token message types with bounded serialization and parsing.
- [x] 1.2 Add host configuration for maximum concurrent sessions, aggregate video bitrate, handshake timeout, and ownership policy, initially forcing the effective session limit to one.
- [x] 1.3 Add stable native status codes and C/Qt wrapper mappings for admission, view-only, control granted, request pending, revoked, authorization failure, and capacity failure.
- [x] 1.4 Add focused parser tests for malformed lengths, missing fields, unsupported versions, oversized messages, and unknown forward-compatible fields.

## 2. Authenticated Session Admission

- [x] 2.1 Expose safe helpers for reading the local OpenSSH client public key, signing a context-bound challenge, and checking an exact public key against the controlled device trust store.
- [x] 2.2 Implement the host-side handshake state machine with client and host nonces, version and capability negotiation, requested role, deadline enforcement, and structured rejection reasons.
- [x] 2.3 Implement the viewer-side handshake before SDP processing and surface successful identity, role, capability, and session-ID results to `ViewerInstance`.
- [x] 2.4 Verify challenge signatures are bound to both nonces, host identity, negotiated version, requested role, and client instance ID, and reject unknown keys or replayed/invalid signatures.
- [x] 2.5 Issue short-lived single-use audio tokens scoped to an admitted session and add token expiry and consumption tracking.
- [x] 2.6 Verify incompatible and unauthorized clients are rejected before creating a PeerConnection, desktop subscriber, encoder, or audio subscriber.

## 3. Host Session Manager Refactor

- [x] 3.1 Extract a `HostClientSession` context that owns one client socket, send mutex, cancellation state, admission data, `WebrtcSession`, and joinable worker lifecycle.
- [x] 3.2 Refactor `HostInstance` into a manager that owns one persistent listener, one shared `NativeWebrtcRuntime`, and a synchronized session map while retaining an effective one-session limit.
- [x] 3.3 Keep the listener accepting while a session is active, reject over-limit connections explicitly, and reap completed workers without detaching threads or joining a worker from itself.
- [x] 3.4 Implement deterministic shutdown that closes the listener and every client socket, revokes control, joins all workers, and destroys sessions before shared media and WebRTC runtime teardown.
- [x] 3.5 Run a one-controller regression covering connect, video, audio, keyboard, absolute and relative mouse, disconnect, reconnect, host exit, and viewer exit before enabling multi-session admission.

## 4. Shared Desktop Media and Capacity Control

- [x] 4.1 Extract Parsec virtual-display ownership and `DesktopVideoSource` startup from `WebrtcSession` into a manager-owned `HostMediaPipeline`.
- [x] 4.2 Change host `WebrtcSession` construction to consume a shared ref-counted video source while retaining a unique video track, PeerConnection, sender, and encoder per client.
- [x] 4.3 Add subscriber reference tracking and last-subscriber idle cleanup so one session disconnect cannot stop media used by another session.
- [x] 4.4 Enable the configured number of concurrent video sessions and verify each session receives media from the same capture source with isolated failure handling.
- [ ] 4.5 Implement aggregate video bitrate allocation across active senders and recalculate bounded per-session targets when sessions join or leave.
- [ ] 4.6 Detect encoder creation or hardware session-limit failures, reject or close only the affected session with a capacity result, and leave existing sessions running.
- [ ] 4.7 Add per-session and aggregate diagnostics for connected duration, assigned bitrate, transport state, encoder failure, and media subscriber count.

## 5. Shared Control Authorization and Input Safety

- [x] 5.1 Replace global mouse-mode state and direct input injection with one serialized `InputDispatcher` that tracks pressed keys, pressed buttons, cursor mode, and source session.
- [x] 5.2 Add shared-control policy state that grants every authenticated `control` requester independently and permits one session to disconnect or be revoked without affecting others.
- [x] 5.3 Make shared control the default ownership policy and keep view-only or capability-ineligible sessions unable to send input.
- [x] 5.4 Gate every keyboard, mouse, wheel, capture, and relative-mouse message by authenticated per-session permission and serialize accepted messages in one deterministic host-side order.
- [x] 5.5 Use shared holder semantics for keys and buttons, and on disconnect, timeout, revocation, or shutdown release only state attributed to the affected session.
- [ ] 5.6 Report shared-control-granted and per-session revoked states without exposing authentication credentials or interrupting other controllers.
- [ ] 5.7 Add deterministic dispatcher tests for concurrent controllers, same-key holders, disconnect while holding input, rejected unauthorized input, and shutdown cleanup.

## 6. Multi-Subscriber Audio

- [ ] 6.1 Refactor `HostAudioStreamer` into `HostAudioHub` with one loopback capture source and independently removable subscribers.
- [ ] 6.2 Require the session-scoped single-use audio token before attaching a subscriber and reject invalid, expired, reused, or foreign tokens without sending audio.
- [ ] 6.3 Add bounded per-subscriber audio queues and drop or disconnect a slow subscriber without blocking capture or other subscribers.
- [ ] 6.4 Update `ViewerAudioPlayer` to authenticate its audio connection with the admitted session credentials and report audio-specific connection failures.
- [ ] 6.5 Verify audio continues for remaining viewers when one audio socket disconnects and capture stops only after the final subscriber leaves.

## 7. Qt Shared-Control UI and Device Status

- [ ] 7.1 Update `RemoteDesktopWindow` to track admitted, view-only, shared-control, and revoked states from native status callbacks.
- [ ] 7.2 Forward local input for admitted shared-control sessions, suppress it for view-only or revoked sessions, and release locally tracked keys when permission is lost.
- [ ] 7.3 Add visible shared-control and rejection feedback without closing a view-only media session.
- [ ] 7.4 Add a controlled-device administrative surface for inspecting connected controllers and revoking an individual session.
- [ ] 7.5 Extend `DeviceStatusService` with versioned session count, viewer count, controller count, and capacity fields while preserving the leading `online` and `busy` tokens.
- [ ] 7.6 Define `busy` as at least one active shared controller and update device-list and detail text for view-only and collaboratively controlled states.

## 8. Enablement, Verification, and Documentation

- [ ] 8.1 Add automated admission tests for valid authorization, unknown keys, bad signatures, replayed challenges, unsupported versions, handshake timeout, and capacity rejection.
- [ ] 8.2 Run a three-client scenario where all clients receive video and every authenticated controller injects serialized input simultaneously without stuck keys or session interruption.
- [ ] 8.3 Run repeated connect, abrupt disconnect, reconnect, application exit, and controlled-device restart churn while checking for deadlocks, leaked workers, callback-after-free, and port reuse failures.
- [ ] 8.4 Measure aggregate bandwidth, frame rate, latency, CPU, GPU, and NVENC session behavior for one, two, and three viewers at supported resolutions, and record safe default limits.
- [ ] 8.5 Verify a slow or failed signaling, video, audio, or input client does not block admission, media delivery, shared-control messages, or shutdown for healthy clients.
- [ ] 8.6 Complete a Release x64 build and the existing single-controller/multi-target workflow regression before changing the default effective session limit from one to the validated value, initially no greater than three.
- [ ] 8.7 Update the WJY Markdown change log with every implementation edit, before/after snippets and line numbers, rationale, ownership markers, and verification results.
