## Why

Device 99 proves that a LocalSystem session agent can detect and capture the Windows Winlogon desktop, but ToDesk locks the workstation when its session ends and the production FSRemote host then loses its normal desktop stream. Because the managed test devices use local accounts without unlock passwords, an isolated one-key POC is needed to determine whether a virtual HID Enter press can dismiss the lock and return the session to the Default desktop.

## What Changes

- Add a new standalone Windows-only POC under `experiments/windows_lock_enter_poc`; do not modify or link production FSRemote targets.
- Add a manually installed LocalSystem service that launches one SYSTEM agent in the active console session.
- Forward real `WTS_SESSION_LOCK` and `WTS_SESSION_UNLOCK` notifications to the agent through protected global events.
- After a lock notification, require the input desktop to be `Winlogon`, wait briefly for it to stabilize, and send exactly one HID Enter press/release through the existing FakerInputBridge.
- Prevent repeated Enter injection until the session returns to the Default desktop, and log whether Windows reports an unlock before timeout.
- Provide explicit install, start, query, stop, and uninstall commands; ordinary launch and build do not mutate SCM or inject input.

## Capabilities

### New Capabilities

- `windows-lock-enter-poc`: Defines the isolated service, active-session agent, lock/unlock event forwarding, one-shot Enter injection, safety gates, logging, readiness, and cleanup behavior.

### Modified Capabilities

None.

## Impact

- Adds a standalone CMake target and source folder for the test executable.
- Reuses the repository's header-only FakerInputBridge client and its existing local named-pipe protocol.
- Uses Windows SCM, WTS session notifications, Winlogon token duplication, protected global events, input-desktop inspection, and virtual HID keyboard reports.
- Does not change `FSRemote.exe`, WebRTC, DXGI/NVENC, VDD lifecycle, installer, updater, startup behavior, credentials, or network listeners.
