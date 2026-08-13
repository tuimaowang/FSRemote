## Context

The secure-desktop screenshot POC has already proven on device 99 that a LocalSystem service can launch a SYSTEM agent in the active console session, observe `Default ↔ Winlogon`, and capture the rendered lock screen. ToDesk deliberately locks the workstation on disconnect. The target account has no unlock password, so the next uncertainty is whether one virtual HID Enter report is enough to dismiss the lock and restore the Default desktop.

The repository already contains a header-only `FakerInputBridgeClient` with a keyboard command that sends complete HID keyboard snapshots. The test must remain isolated from production FSRemote and must not broaden the SYSTEM process into a network host.

## Goals / Non-Goals

**Goals:**

- Build a standalone Windows executable from a new source folder.
- Run a manually installed LocalSystem service and one supervised SYSTEM agent in the active console session.
- Trigger only from a genuine SCM `WTS_SESSION_LOCK` notification for the agent's session.
- Confirm the current input desktop is `Winlogon`, wait 500 ms, and send exactly one Enter key-down/key-up sequence through FakerInputBridge.
- Prevent another Enter until an unlock notification returns the state machine to armed.
- Log service identity, session, desktop, bridge connection, key-down, key-up, unlock, timeout, and cleanup outcomes.

**Non-Goals:**

- Do not store, enter, derive, or transmit credentials.
- Do not send Ctrl+Alt+Del, SAS, mouse buttons, clipboard data, or repeated keyboard input.
- Do not trigger from UAC merely because its secure desktop may also be named `Winlogon`.
- Do not modify production FSRemote, the screenshot POC, FakerInputBridge, drivers, network ports, installer, updater, or startup entries.
- Do not claim that one Enter will unlock password-protected, Windows Hello, domain, or logged-off sessions.

## Decisions

### Use a separate executable and service

The POC lives under `experiments/windows_lock_enter_poc`, uses service name `FSRemoteLockEnterPoc`, and installs a protected copy under `C:\Program Files\FSRemote\LockEnterPoc`. This avoids changing the completed screenshot experiment or production components.

Alternative considered: add auto-enter behavior to the secure-desktop screenshot POC. That would mix capture evidence with privileged keyboard injection and weaken the original POC's explicit no-keyboard guarantee.

### Require both a real lock notification and Winlogon desktop

The service receives `SERVICE_CONTROL_SESSIONCHANGE` and signals protected lock/unlock events only for the active agent session. The agent injects Enter only after receiving the lock event and independently confirming that `OpenInputDesktop` reports `Winlogon`.

Alternative considered: poll only for a desktop named Winlogon. UAC can use a secure desktop without the workstation being locked, so desktop name alone is not a sufficient safety gate.

### Send exactly one complete HID Enter snapshot

After a 500 ms stabilization delay, the agent connects and pings FakerInputBridge, sends HID usage `0x28` in one key-down snapshot, waits 60 ms, then sends an all-zero key-up snapshot and calls `releaseAll`. It does not fall back to `SendInput`, because a fallback could accidentally produce a second Enter and would make the result ambiguous.

### Use a one-shot state machine per lock cycle

The agent starts armed. A qualifying lock moves it to injected/waiting state. Additional lock signals are logged and ignored until a matching unlock event arrives or the input desktop returns to Default. A timeout records failure but remains disarmed until unlock, preventing retry loops on credential screens.

### Keep readiness and control objects local and protected

Service stop, agent ready, lock, and unlock events use a DACL that grants access only to LocalSystem and local Administrators. `--start-service` waits for the ready event before reporting success, and `--query-service` reports `AgentReady`.

## Risks / Trade-offs

- [One Enter may only dismiss the wallpaper and not unlock] → Record timeout without sending a second key; use the result to decide a later separately authorized experiment.
- [FakerInputBridge may not be reachable from the SYSTEM agent] → Log ping and driver flags; do not fall back to another input backend in the same test.
- [Lock notification arrives before Winlogon is ready] → Wait 500 ms and re-check the input desktop immediately before injection.
- [Duplicate session-change notifications occur] → Disarm after the first qualifying injection and ignore repeats until unlock.
- [Service starts while Windows is already locked] → Do not inject because no fresh lock event was observed; unlock and repeat the controlled test.
- [Automatic unlock removes a Windows security boundary] → Keep the POC manually installed, manual-start, isolated, and non-production.

## Migration Plan

1. Build and inspect `--help` without installing the service.
2. On device 99, stop and leave the screenshot POC independent; install and start the lock-enter POC from an elevated terminal.
3. Confirm `AgentReady=1`, then disconnect ToDesk once to generate a real lock event.
4. Reconnect and collect the POC log to determine whether one Enter caused `WTS_SESSION_UNLOCK`.
5. Stop and uninstall the test service.
6. If successful, create a separate production proposal for an opt-in managed-device auto-unlock policy and capture recovery.

Rollback stops and deletes only `FSRemoteLockEnterPoc`; installed binary and logs remain for manual evidence collection.

## Open Questions

- Does one Enter both dismiss the Windows lock wallpaper and unlock the passwordless local account on device 99?
- Does the existing FakerInputBridge accept keyboard reports from the SYSTEM session agent while Winlogon owns the input desktop?
- If one Enter only reveals the sign-in page, should a future experiment allow a separately authorized second Enter?
