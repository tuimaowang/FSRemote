## Why

Remote update buttons are currently refreshed by opening a TCP `update_status` connection to every online target during device refreshes and every ten seconds while remote windows are open. FSRemote already has a self-healing realtime UDP snapshot channel, so update discovery can reuse that channel and avoid device-count-dependent background polling while retaining authoritative shared-storage validation before installation.

## What Changes

- Extend realtime device snapshots with the installed FSRemote version, whether same-version runtime dependency repair is required, and the latest shared release version the sender has confirmed.
- Trigger an immediate realtime snapshot whenever local update availability or the confirmed shared release version changes.
- Derive remote update-button visibility from received realtime snapshot data and the controller's confirmed shared release version instead of periodic per-device `update_status` queries.
- Treat a higher release version observed in a peer snapshot as a hint that schedules an authoritative shared-storage version check; the hint never directly authorizes installation.
- Retain TCP `update` and `update_status` only for a user-initiated update transaction, including preparation failure reporting, target restart, and reconnection tracking.
- Retain startup shared-version validation and a low-frequency fallback check so missed UDP packets, offline publishers, or blocked broadcasts cannot permanently hide an update.

## Capabilities

### New Capabilities
- `realtime-update-discovery`: Publishes and reconciles update discovery state over the existing realtime device snapshot channel while preserving shared-storage authority and transactional update status handling.

### Modified Capabilities

None.

## Impact

- Realtime protocol structures and JSON validation in `DeviceRealtimeStateService`.
- Local update-state publication in `main.cpp` and `UpdateService`.
- Remote update-button state and device-state consumption in `DeviceGrid` and `RemoteDesktopWindow`.
- Removal of normal-operation per-device `update_status` polling; the command protocol remains compatible for explicit update transactions.
- No new external dependencies, ports, services, or update package formats.
