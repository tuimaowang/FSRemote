## Why

Realtime release gossip currently makes update discovery depend on peer IP allowlists, subscription direction, and publisher reachability. Update availability should instead be derived only from the authoritative shared `FSRemote.version` marker, with a short bounded polling interval that remains responsive without coupling release discovery to device-state packets.

## What Changes

- Check the authoritative shared version at startup and every 20 seconds.
- Remove peer `knownReleaseVersion` publication and peer-triggered shared-version checks.
- Keep realtime publication of each device's own installed version and runtime-repair requirement so controllers can evaluate remote-device update buttons against their locally confirmed shared version.
- Preserve manual refresh, post-publish local state refresh, asynchronous SMB probing, bounded background reads, and explicit update transaction polling.
- Ensure update-button visibility never depends on recognizing the publisher IP or receiving a release notification packet.

## Capabilities

### New Capabilities
- `shared-update-discovery`: Defines authoritative shared-folder update discovery, the 20-second check cadence, and separation between shared release knowledge and per-device installed-version telemetry.

### Modified Capabilities

## Impact

- `UpdateService` periodic scheduling and cached shared-version state.
- Realtime update metadata encoded by `DeviceRealtimeStateService`.
- `main.cpp` local realtime update-state publication.
- `DeviceGrid` peer update-state handling and remote update-button derivation.
- Realtime protocol compatibility remains at protocol version 1; removed fields must continue to decode safely from mixed-version peers during rollout.
