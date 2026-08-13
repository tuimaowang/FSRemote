## Context

FSRemote already publishes complete, ordered device snapshots over UDP 49104. A local state change sends the current snapshot immediately and again after 100 ms and 300 ms, while adaptive heartbeats provide eventual repair after packet loss. Remote update-button discovery currently bypasses this channel: manual device refreshes and every open remote window periodically connect to TCP 49102 and request `update_status`.

The shared `FSRemote.version` file and immutable `releases/<version>` directory must remain authoritative. UDP state is unauthenticated display metadata and cannot authorize an installation. The implementation must remain compatible with peers that still publish protocol-version-1 snapshots without update fields.

## Goals / Non-Goals

**Goals:**

- Publish installed version, runtime repair need, and the sender's confirmed shared release version in the existing complete realtime snapshot.
- Update remote-window buttons from realtime state rather than normal-operation per-device TCP polling.
- Propagate a newly published release quickly across broadcast domains by treating a peer's higher confirmed release version as a hint to recheck shared storage.
- Preserve startup discovery, missed-packet recovery, user-triggered update status tracking, and shared-storage authority.
- Fix stale preparation failures so an already-current target cannot indefinitely advertise a retry state.

**Non-Goals:**

- Do not send update payloads, credentials, commands, or detailed installer errors over UDP.
- Do not automatically install an update when a version hint is received.
- Do not remove TCP `update` or the short-lived `update_status` state machine used after a user starts an update.
- Do not introduce a new port, service, dependency, or incompatible realtime protocol version.

## Decisions

### 1. Add an optional update object to protocol-version-1 snapshots

The snapshot gains an `update` object containing `installedVersion`, `runtimeRepairRequired`, and `knownReleaseVersion`. New receivers validate semantic-version strings before accepting them. Missing update fields decode to an unknown/default update state, allowing old and new binaries to coexist without losing presence, session, or script synchronization.

Incrementing the protocol version was rejected because current receivers reject unknown versions completely, which would temporarily remove mixed-version devices from the realtime state bus during rollout.

### 2. Keep local update metadata in the existing local-state provider

`main.cpp` maintains a small cached realtime update state. The installed version and repair requirement are initialized from the running directory. `UpdateService::updateAvailabilityChanged` refreshes the confirmed release version and repair state, then asks `DeviceRealtimeStateService` to re-read the provider. This produces the existing immediate/100 ms/300 ms publication without polling files every 250 ms.

### 3. Compare target state only against a locally confirmed shared release

`DeviceGrid` stores each target's realtime update state. A remote update button is visible only when the controller has successfully confirmed a shared release version and either:

- the confirmed release is newer than the target installed version; or
- the versions are equal and the target reports missing runtime dependencies.

A peer's `knownReleaseVersion` is not used directly to show an update button. If it is higher than the controller's cached confirmed release, it only calls `UpdateService::checkNow()`, which performs the existing TCP-445 gate and bounded SMB read.

The target's own `knownReleaseVersion` is not a prerequisite for showing the button. Requiring it would hide a real update whenever the target misses the release-hint datagrams and would postpone discovery until the long fallback check.

### 4. Remove normal-operation update-status polling

The ten-second remote-window timer, its background query function, and open-window immediate queries are removed. Manual device refresh may continue to issue a one-time `update_status` query as an explicit diagnostic/calibration action.

After the user sends `update`, the remote window retains its existing one-second transaction polling. This is necessary because the target application intentionally exits while the independent updater owns installation, so no realtime publisher exists during part of the transaction.

### 5. Use gossip-style release hints with a long fallback check

After a successful publish, `UpdateService` publishes the newly committed version through its existing availability signal. The local realtime snapshot then carries that version on every retransmission and heartbeat. Peers that observe a higher version schedule an authoritative check and, after confirming it, advertise it onward.

The normal shared-version interval changes from 60 seconds to 10 minutes. Startup still performs an immediate check. Ten minutes is only a missed-notification fallback; ordinary discovery is event-driven through existing realtime traffic.

### 6. Clear stale remote preparation failures when no update remains

`update_status` checks current cached availability before returning a stored preparation failure. If the target is now current and complete, it clears the stale failure and returns `complete`. This prevents manual refresh from restoring an update button solely because of an obsolete error.

### 7. Carry the controller-confirmed version in an explicit update request

The optional second field of the TCP `update` command contains the release version confirmed by the controller. A target whose local update cache is stale may accept the request when that version is newer than its installed version or when it identifies a same-version repair. The background update preparation still reads and validates shared storage and never installs from the controller-provided value.

## Risks / Trade-offs

- [UDP update fields are absent on older peers] → Treat the state as unknown and retain explicit update actions/manual refresh during rollout.
- [A forged peer advertises a high release version] → Use the value only to trigger the existing authoritative shared-storage check; never install or display a confirmed update from the hint alone.
- [All release-hint datagrams are lost] → Startup checks and the ten-minute fallback eventually discover the release.
- [Shared storage is temporarily unavailable when a hint arrives] → Existing cooldown prevents SMB pressure and the long fallback retries later.
- [Realtime state expires while a device is updating] → Clear the normal button state; the explicit remote-update transaction state machine continues independently.
- [Controller confirms no shared version during an outage] → Hide derived update buttons until authority is restored rather than presenting stale install actions.

## Migration Plan

1. Deploy the extended snapshot parser and publisher while retaining protocol version 1.
2. New clients begin deriving update buttons from peers that publish update metadata.
3. Old clients continue normal realtime presence behavior and remain updateable through the existing explicit command path.
4. If rollback is required, restore the ten-second query timer; the TCP command protocol remains unchanged.

## Open Questions

None for the initial implementation.
