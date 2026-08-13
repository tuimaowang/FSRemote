## 1. Standalone POC Setup

- [x] 1.1 Create the isolated `experiments/windows_lock_enter_poc` folder, standalone CMake target, and administrator usage documentation
- [x] 1.2 Add shared Win32 helpers for UTF-8 logging, protected named events, identity/session/desktop diagnostics, and RAII handles

## 2. LocalSystem Service and Agent

- [x] 2.1 Implement explicit install, start, query, stop, and uninstall SCM commands with a protected Program Files service copy
- [x] 2.2 Implement the LocalSystem service control loop, active-session `winlogon.exe` token duplication, and owned-agent supervision
- [x] 2.3 Forward matching WTS lock/unlock notifications through protected events and expose AgentReady diagnostics

## 3. One-Shot Enter Test

- [x] 3.1 Implement the agent lock-cycle state machine and require both a lock event and Winlogon input desktop
- [x] 3.2 Connect and ping FakerInputBridge, send exactly one HID Enter key-down/key-up snapshot, and release all state
- [x] 3.3 Log unlock success or timeout and suppress repeated input until a real unlock returns the desktop to Default

## 4. Verification and Handoff

- [x] 4.1 Configure and compile the standalone Release POC without changing production build targets
- [x] 4.2 Validate help/query behavior, run static diff checks, and inspect the final binary metadata
- [x] 4.3 Document device-99 install, controlled lock test, evidence collection, and cleanup steps
