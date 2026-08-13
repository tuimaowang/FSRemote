## 1. Standalone POC Setup

- [x] 1.1 Create the isolated `experiments/windows_secure_desktop_agent_poc` folder, standalone CMake target, and usage documentation
- [x] 1.2 Add shared Win32 helpers for durable logging, output paths, error formatting, RAII handles, and protected global events

## 2. Service and Session Agent

- [x] 2.1 Implement explicit SCM install, query, start, stop, and uninstall console commands
- [x] 2.2 Implement the LocalSystem service control loop and session-change diagnostics
- [x] 2.3 Find `winlogon.exe` in the active console session, duplicate its token, and supervise one SYSTEM agent process

## 3. Secure Desktop Diagnostics

- [x] 3.1 Implement agent identity, window-station, thread-desktop, input-desktop, and desktop-transition logging
- [x] 3.2 Implement one-frame primary-output DXGI Desktop Duplication capture with exact HRESULT diagnostics
- [x] 3.3 Implement timestamped GDI BMP fallback capture from the selected input desktop
- [x] 3.4 Implement protected capture, mouse-test, and stop events with explicit console request commands
- [x] 3.5 Implement harmless FakerInputBridge and `SendInput` relative mouse movement tests

## 4. Verification and Handoff

- [x] 4.1 Add a non-mutating `--probe` mode and focused helper tests where practical
- [x] 4.2 Configure and compile the standalone POC without building or changing production FSRemote targets
- [x] 4.3 Run probe/help validation, inspect logs and BMP output, and run static diff checks
- [x] 4.4 Document administrator commands for device-99 lock, capture, input, unlock, cleanup, and evidence collection
- [x] 4.5 Wait for active-session agent readiness after service start, report readiness during query, and avoid overwriting a running installed binary
