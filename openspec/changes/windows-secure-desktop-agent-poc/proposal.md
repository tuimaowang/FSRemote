## Why

FSRemote currently runs its host, capture, and input stack inside the logged-in user's `FSRemote.exe`. When another remote-control product locks Windows, DXGI loses the current desktop and `DuplicateOutput` returns `E_ACCESSDENIED`, so restarting the same user process cannot capture the Winlogon desktop. A small isolated proof of concept is needed before changing the production remote-control architecture.

## What Changes

- Add an isolated Windows-only POC under a new repository folder; do not link it into `FSRemote.exe` or `fsremote_stream.dll`.
- Add a LocalSystem-capable Windows service executable that supervises an agent in the active console session.
- Add a session agent mode that reports its token identity, session, window station, thread desktop, and current input desktop.
- Add explicit commands to capture a single diagnostic screenshot from the current input desktop and to perform an opt-in harmless mouse-move test.
- Add lock/unlock and desktop-transition diagnostics suitable for testing on device 99.
- Add explicit service install, start, stop, and uninstall commands; no service mutation occurs during a normal POC launch.
- Keep all generated binaries, logs, and screenshots separate from the production FSRemote runtime.

## Capabilities

### New Capabilities

- `secure-desktop-agent-poc`: Defines the isolated service, session-agent launch, current-input-desktop inspection, screenshot, input test, logging, and cleanup behavior.

### Modified Capabilities

None.

## Impact

- Adds a new standalone CMake target and source folder for the POC.
- Uses Windows SCM, WTS, token, process, window-station, desktop, GDI, and input APIs.
- Does not change existing ports, remote-session protocols, VDD ownership, WebRTC, NVENC, Qt UI, installer, updater, or startup behavior.
- Real lock-screen validation requires an administrator-approved service install and an interactive test on device 99.
