## 1. Realtime state model and protocol

- [x] 1.1 Add device snapshot, host session, script tri-state, controller target, and reduced remote-state data structures.
- [x] 1.2 Implement bounded JSON encode/decode validation with protocol version, device ID, boot ID, sequence, TTL, and source-IP checks.
- [x] 1.3 Register the realtime state source files and Qt Network dependencies in the project build configuration.

## 2. Broadcast transport and local message queue

- [x] 2.1 Implement UDP 49104 binding, per-interface directed broadcast discovery, datagram receive handling, and shutdown cleanup.
- [x] 2.2 Implement the bounded/coalescing inbound event queue and single-thread state reducer with boot/sequence deduplication.
- [x] 2.3 Implement immediate state-change broadcasts, short retransmission, adaptive active/idle heartbeat, jitter, and TTL expiry events.

## 3. Authoritative local state providers

- [x] 3.1 Expose verified local script runtime state for reuse by the realtime snapshot provider.
- [x] 3.2 Connect the existing host session count/controller details to the realtime service and preserve ICE/reconnect stale-session cleanup.
- [x] 3.3 Publish locally controlled target sessions from remote-window lifecycle changes without using them as target controller counts.

## 4. Device UI integration

- [x] 4.1 Connect DeviceGrid to reduced realtime state notifications for Online, Busy, Offline, controller count, and controller tooltip data.
- [x] 4.2 Apply realtime script Unknown/Idle/Running state to per-device script UI and running Logo behavior.
- [x] 4.3 Stop startup and periodic all-device TCP status polling while retaining the titlebar manual refresh as one-time calibration.
- [x] 4.4 Remove or neutralize the obsolete automatic status refresh setting so the UI does not promise periodic polling.

## 5. Static verification and compatibility review

- [x] 5.1 Review packet-size limits, malformed input rejection, queue bounds, multi-interface filtering, TTL cleanup, and QObject lifetimes.
- [x] 5.2 Review that UDP state cannot execute commands and that unconfigured source IPs cannot create or mutate device entries.
- [x] 5.3 Run OpenSpec status and static diff checks without compiling, as explicitly requested by the user.

## 6. Routed subnet delivery correction

- [x] 6.1 Add bounded UDP subscription messages and subscriber leases for configured devices outside all local IPv4 subnets.
- [x] 6.2 Unicast complete state-change, retransmission, and heartbeat snapshots to active cross-subnet subscribers while retaining same-subnet directed broadcast.
- [x] 6.3 Allow an explicit remote-update request to probe TCP 49102 even when a legacy or cross-subnet device is currently displayed Offline/Unknown.
- [x] 6.4 Verify the `192.168.2.9` controller to `192.168.1.116` target path, packet validation, lease cleanup, and static diff checks without compiling.
