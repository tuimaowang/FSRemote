## Context

FSRemote currently has two distinct update inputs: the shared `FSRemote.version` file, which is authoritative, and realtime device snapshots, which publish peer update metadata. The recent realtime discovery design also gossips `knownReleaseVersion`, but snapshot delivery is intentionally restricted by configured device IPs and cross-subnet subscription leases. A publisher that is not itself in the shared device catalog therefore cannot reliably notify targets, even though the shared release is valid.

Online, session, script, and installed-version telemetry remain valid realtime device facts. Only discovery of the authoritative latest release needs to return to shared-folder checks.

## Goals / Non-Goals

**Goals:**

- Use the shared version marker as the only source of truth for the latest release.
- Check that marker asynchronously at startup and every 20 seconds.
- Keep target installed version and runtime-repair state in realtime snapshots for remote-button evaluation.
- Remove publisher-IP, peer-gossip, and reverse-subscription dependencies from local update discovery.
- Preserve mixed-version snapshot compatibility and existing explicit update transaction handling.

**Non-Goals:**

- Do not add a release notification packet, port, publisher identity, or signing system.
- Do not return to per-window or per-device `update_status` polling during normal operation.
- Do not rewrite payload staging, verification, rollback, updater restart, or driver installation.

## Decisions

### 1. Poll the authoritative shared marker every 20 seconds

`UpdateService` will retain the existing asynchronous TCP-445 gate and bounded background UNC read, but its periodic interval will be 20 seconds. Startup still schedules an immediate check. This produces a predictable maximum discovery delay without trusting peer metadata.

A shorter interval was chosen over the previous ten-minute fallback because release gossip is being removed. With roughly 54 clients, a 20-second cadence averages fewer than three small marker checks per second and remains materially cheaper than per-device status polling.

### 2. Realtime snapshots publish only per-device update facts

`DeviceRealtimeUpdateState` will retain `installedVersion` and `runtimeRepairRequired`. `knownReleaseVersion` will be removed from local publication and ignored/removed from peer-trigger logic.

Installed version belongs in the realtime snapshot because it is an authoritative fact about the sender. The latest shared release does not belong there because it is global shared-storage state and creates publisher topology dependencies.

### 3. Local and remote buttons use the same locally confirmed shared release

The main-window update button compares the locally installed version and repair state with `UpdateService`'s confirmed shared version. Remote-device buttons compare that same confirmed shared version with the target's realtime installed version and repair state.

No remote button waits for the target to acknowledge or confirm the release. No local button is derived from a peer version.

### 4. Normal-operation target polling remains removed

The existing realtime installed-version telemetry remains the source for target version state. Explicit `update_status` polling remains limited to manual calibration and an active update transaction, where the target process can intentionally exit and stop publishing realtime snapshots.

## Risks / Trade-offs

- [A release can take up to 20 seconds to appear on another device] → The interval is deterministic, startup checks immediately, and manual refresh remains available.
- [Many devices read the same small marker] → Keep the TCP gate, bounded background read, cooldown, and single in-flight check per process.
- [An older peer still sends `knownReleaseVersion`] → Protocol version remains 1 and receivers tolerate the extra JSON field while no longer acting on it.
- [A target has not yet published its installed version] → Hide the remote update button until valid target telemetry arrives rather than falling back to continuous TCP polling.

## Migration Plan

1. Deploy clients that stop acting on peer release knowledge and check the shared marker every 20 seconds.
2. Keep decoding optional update objects from older snapshots for mixed-version presence compatibility.
3. After rollout, older clients may continue sending `knownReleaseVersion`; new clients ignore it.
4. Rollback by restoring the previous snapshot field and ten-minute fallback only if shared-folder polling causes unacceptable load.

## Open Questions

None.
