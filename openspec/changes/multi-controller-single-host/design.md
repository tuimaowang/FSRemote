## Context

The current native host owns one listening socket, accepts one signaling client, closes the listener, and runs one `WebrtcSession` until that client disconnects. The session also owns the Parsec virtual display, desktop capturer, video source, control data channel, and related mouse-mode state. Audio is transported by a separate TCP listener that retains one client socket. This ownership model is safe only while the controlled device has one remote session.

The controller UI already creates independent viewer handles for different target devices, so most multi-host controller behavior can remain. The controlled-device side must become a bounded multi-session service without multiplying virtual displays and desktop capture loops or permitting concurrent input races. WebRTC media remains encrypted, while the existing signaling and separate audio transport require an authenticated admission layer before they are exposed to additional clients.

The initial supported operating point is three concurrent remote sessions per controlled device. All admitted sessions can view media, and every authenticated session admitted with the `control` capability can inject keyboard and mouse input simultaneously by default.

## Goals / Non-Goals

**Goals:**

- Accept and independently manage up to a configurable number of authenticated remote sessions.
- Capture the virtual desktop and system audio once and distribute them to all admitted sessions.
- Maintain one PeerConnection, signaling channel, control data channel, and encoder sender per client.
- Grant keyboard and mouse authority to every authenticated control-capable session and make concurrent input deterministic and observable.
- Bound total bitrate, encoder usage, queues, and shutdown time so one slow or failed client cannot destabilize other sessions.
- Preserve existing single-controller behavior and backward-compatible online/busy status prefixes.
- Reuse the application's existing OpenSSH Ed25519 device identities and authorized-key trust store instead of adding a new cryptographic dependency.

**Non-Goals:**

- Implementing an SFU, RTP packet relay, or single-encode media fan-out in the first version.
- Supporting untrusted Internet exposure, NAT traversal services, or a cloud rendezvous service.
- Removing the current C API or changing how a controller opens one remote window per target device.
- Guaranteeing more than the configured session limit or a fixed 120 Mbps stream for every viewer.

## Decisions

### 1. Introduce a host session manager with one shared WebRTC runtime

`HostInstance` becomes a service owner rather than a single client worker. It owns one persistent signaling listener, one initialized `NativeWebrtcRuntime`, one `HostMediaPipeline`, one `HostAudioHub`, one `ControlArbiter`, and a synchronized map of `HostClientSession` objects keyed by generated session ID.

The accept loop remains available while existing clients are connected. Each accepted socket receives a dedicated session context and joinable worker. Each context owns its socket, send mutex, handshake state, `WebrtcSession`, negotiated capabilities, client identity, requested role, and cancellation state. Completed workers are reaped by the manager; workers are never detached.

The stop sequence is: stop accepting, mark all contexts cancelled, close every client socket to unblock reads, revoke control and release injected state, join client workers, stop shared audio and desktop capture, destroy sessions, and finally shut down the WebRTC runtime. This order prevents callbacks from referencing destroyed host resources.

Alternative considered: starting one complete `HostInstance` per client. This was rejected because it would bind the same ports repeatedly and create duplicate WebRTC runtimes, virtual displays, capture loops, audio listeners, and global input state.

### 2. Share capture sources, not PeerConnections or encoder senders

`HostMediaPipeline` lazily starts one `ParsecVddSession` and one ref-counted `DesktopVideoSource` when the first session is admitted. It keeps them alive while at least one media subscriber exists and stops them after the last session leaves, optionally using a short idle grace period to avoid rapid teardown/restart.

Each `WebrtcSession` still creates its own PeerConnection and a uniquely named video track backed by the shared video source. WebRTC therefore performs congestion control and encoding independently for each client. Session destruction releases only its track and sender; it does not stop the shared source or virtual display.

Alternative considered: forwarding one encoded NVENC stream to every client. This would reduce GPU and capture cost but requires an encoded-frame fan-out layer that correctly handles independent RTP sequence numbers, keyframe requests, retransmission, congestion feedback, and SRTP contexts. It is deferred until measured fan-out requirements exceed the initial three-session target.

### 3. Enforce an aggregate media budget

The host exposes configurable `maxSessions` and `maxAggregateVideoKbps` values, with initial defaults of three sessions and an aggregate ceiling matching the current single-session 120 Mbps ceiling. A budget allocator assigns each sender a bounded target based on the active subscriber count, minimum viable bitrate, and aggregate ceiling. Admission fails with a capacity reason if a new session cannot receive a viable stream or if encoder creation reports a hardware session limit.

Encoder or transport failure in one session closes or degrades that session without stopping shared capture or other clients. A slow client's audio and signaling queues are bounded; media for that client may be dropped rather than blocking the producer.

Alternative considered: preserving 120 Mbps per viewer. This was rejected because three viewers could request 360 Mbps and three NVENC sessions without any host-level guardrail.

### 4. Replace single-client audio with a shared authenticated hub

`HostAudioHub` owns one loopback capture loop and a set of audio subscribers. The existing framed PCM transport can remain for the first version, but the audio connection must begin with the admitted remote session ID and a one-time audio token issued during signaling admission. Invalid, expired, or already-used tokens are rejected.

Captured audio frames are fanned out through bounded per-subscriber queues. Disconnecting one subscriber removes only that subscriber. Moving audio into a WebRTC audio track remains a later improvement for integrated congestion handling and transport encryption.

### 5. Use shared control authorization and a serialized input dispatcher

Every authenticated session requesting the `control` role and negotiating the `control` capability is granted shared control by default. View-only sessions and sessions without the negotiated capability remain unable to inject input. A future controlled-device administrative surface may revoke one session without changing the permissions of other controllers.

All accepted data-channel input messages reach one manager-owned `InputDispatcher`. The dispatcher serializes events from every controller on one execution path, owns host mouse mode, and records pressed keys and mouse buttons per session. Arrival at the dispatcher mutex defines the deterministic host-side order; absolute pointer movement uses the latest ordered position and relative movement is applied in ordered deltas.

Keys and buttons use shared holder semantics: the operating system receives a down event when the first session presses an item and an up event only after the last session holding it releases or disconnects. Session timeout, disconnect, revocation, and host shutdown release only state attributed to the affected session so another controller's held input is not cancelled.

Alternative considered: direct concurrent `SendInput` calls from each PeerConnection callback. This was rejected because input and mouse-mode state would race, ordering would be undefined, and disconnects could leave keys or buttons stuck.

### 6. Add a versioned challenge-response admission handshake

Before SDP exchange, a client sends a versioned hello containing a generated client instance ID, device name, requested role, supported capabilities, public key, and client nonce. The host verifies that the exact public key is in the existing authorized-key trust store, returns a host nonce, and requires a signature over both nonces, negotiated protocol version, requested role, and host identity.

Signing and verification reuse the bundled OpenSSH Ed25519 identity and tools. The handshake has a short deadline and produces either an admitted session ID with scoped media tokens or a structured rejection containing `unsupported_version`, `unauthorized`, `capacity`, `policy`, or `timeout`. WebRTC negotiation begins only after successful admission.

Legacy status queries continue to receive payloads beginning with `online` or `busy`. Legacy stream clients that cannot complete the handshake are rejected by default; an insecure compatibility bypass is not enabled.

Alternative considered: trusting source IP addresses on the LAN. This was rejected because IP addresses do not identify a controller and can be reused or spoofed.

### 7. Report session and ownership state through existing callback surfaces

The stream status callback gains stable status codes/messages for admitted, view-only, shared control granted, control revoked, capacity rejected, and authorization rejected. `RemoteDesktopWindow` forwards local input whenever its authenticated session has shared control and suppresses input for view-only or revoked sessions.

The device status payload keeps its existing leading `online` or `busy` token and appends versioned fields for session count, viewer count, active controller count, and capacity. `busy` means at least one admitted shared controller exists; view-only subscribers alone do not make the device appear controlled.

## Risks / Trade-offs

- **Multiple PeerConnections still create multiple NVENC encoders and multiply egress bandwidth** → enforce low default fan-out, aggregate bitrate allocation, explicit encoder-capacity failure, and collect per-session metrics before considering encoded fan-out.
- **The existing OpenSSH authorization command is part of the trust bootstrap** → require explicit prior device authorization, bind signatures to nonces and host identity, and never accept a public key merely because the client supplied it.
- **Separate TCP audio is less robust and less secure than WebRTC audio** → bind it to a one-time admitted-session token, bound queues, and retain migration to WebRTC audio as a documented follow-up.
- **Shared source lifetime can race session teardown** → use ref-counted media objects, manager-owned shutdown ordering, weak callback captures, and join all workers before destroying the runtime.
- **Per-client bitrate may be lower than today's single-viewer quality** → make the aggregate limit configurable and show clear capacity/quality state rather than silently oversubscribing.
- **Old stream binaries cannot perform the new handshake** → fail quickly with an explicit incompatibility result and update both host and viewer together; retain compatibility only for the separate status protocol.

## Migration Plan

1. Refactor host lifetime into `HostSessionManager`, `HostMediaPipeline`, and `InputDispatcher` while retaining `maxSessions = 1`; verify current video, audio, input, busy status, and shutdown behavior.
2. Add the versioned authenticated handshake and update the viewer before enabling more than one session.
3. Make desktop capture and virtual display shared, then enable multiple view-only sessions with bounded per-session senders.
4. Add shared-control authorization, the serialized multi-controller dispatcher, per-session input cleanup, and status extensions.
5. Replace single-client audio with `HostAudioHub`, add audio session tokens, and enable the default three-session limit.
6. Run connection churn, simultaneous-input, encoder-capacity, bandwidth, crash, and application-exit tests before changing the default from one session to three.

Rollback is configuration-based while the refactor is being stabilized: set `maxSessions = 1` and disable multi-session admission. If the new protocol must be rolled back completely, deploy the matching prior host and viewer binaries together because legacy stream clients are not handshake-compatible.

## Open Questions

- Whether the first release should expose the aggregate bitrate and maximum session settings in the UI or keep them as application configuration defaults.
- Which controlled-device UI surface should list controllers and host per-session administrative revocation.
- Whether measured NVENC limits on supported GPUs justify a two-session default instead of three for 4K60 operation.
