## Context

The production application starts the remote host from the logged-in Qt process. That process belongs to the user's interactive desktop and cannot recreate Desktop Duplication after Windows switches to the protected Winlogon desktop. The POC must prove the Windows privilege and desktop-transition mechanics without destabilizing the current remote-control product.

The existing repository already contains the media pipeline and a FakerInput named-pipe client. The POC can reuse the bridge client for a diagnostic input attempt, but it must not link the production WebRTC, VDD, or NVENC pipeline.

## Goals / Non-Goals

**Goals:**

- Build a standalone Windows executable from a new source folder.
- Support an SCM service mode that is suitable for LocalSystem.
- Launch one SYSTEM agent in the active console session by duplicating the matching `winlogon.exe` token.
- Detect and log changes between the Default, Winlogon, and other current input desktops.
- Attempt a one-frame DXGI Desktop Duplication capture and fall back to GDI for diagnostic comparison.
- Allow explicit administrator-triggered capture and harmless mouse-move tests through protected global events.
- Save durable logs and BMP screenshots for device-99 testing.

**Non-Goals:**

- Do not modify `FSRemote.exe`, `fsremote_stream.dll`, WebRTC signaling, codecs, VDD lifecycle, or the production input dispatcher.
- Do not provide unattended credential storage or automatic password entry.
- Do not install or start the service during build or ordinary execution.
- Do not claim production security, multi-user support, or update compatibility from the POC.

## Decisions

### Use one executable with explicit modes

`FSRemoteSecureDesktopPoc.exe` will expose console commands and internal `--service` / `--agent` modes. One signed artifact simplifies service registration and ensures the agent code being launched is the same artifact that the administrator installed.

Alternative considered: separate service and agent binaries. This gives a cleaner production boundary but adds packaging and version-handshake work before the Windows mechanism is proven.

### Keep the production runtime completely disconnected

The POC lives under `experiments/windows_secure_desktop_agent_poc` with a standalone `CMakeLists.txt`. No root CMake, installer, updater, startup registry, ports, or production source files are changed.

Alternative considered: add the POC targets to the root build. This is convenient but risks accidental release packaging.

### Launch the agent with the active session's Winlogon token

The LocalSystem service finds `winlogon.exe` in the active console session, opens and duplicates its primary token, creates an environment block, and starts the same executable in agent mode. This preserves SYSTEM identity while placing the process in the interactive session that owns the input desktop.

Alternative considered: `WTSQueryUserToken`. That launches as the logged-in user and does not prove secure-desktop access.

### Use a dedicated desktop worker thread

The agent's worker opens the current input desktop, compares its name with the selected thread desktop, calls `SetThreadDesktop` before creating capture resources, destroys capture resources before a later switch, and records every transition.

Alternative considered: switching the main process thread. That becomes unreliable after windows, hooks, or other USER objects have been created.

### Compare DXGI and GDI in the same capture request

Each capture request first enumerates DXGI outputs, chooses the primary attached output, attempts `DuplicateOutput`, and saves a BMP if successful. If initialization or frame acquisition fails, the agent records the HRESULT and performs a GDI `BitBlt` capture from the selected input desktop. This distinguishes a privilege problem from a Desktop Duplication limitation.

### Keep privileged commands explicit and local

The agent creates global auto-reset events for screenshot and input-test requests. Their DACL permits only LocalSystem and the local Administrators group. Console commands merely signal these events; they do not silently install a service or inject input.

### Wait for agent readiness after service start

The service launches the active-session agent immediately on entering its control loop. The `--start-service` command waits for both protected agent command events to exist before reporting success, and `--query-service` reports agent readiness separately from the SCM running state. The service process does not create transient capture or mouse events itself, so a client cannot mistake service initialization for a ready agent.

Alternative considered: sleep for a fixed number of seconds in the test script. This hides launch failures, varies across machines, and still allows capture requests to race with the agent.

### Test FakerInput before system input

The mouse test asks the existing `FakerInputBridge` to move one pixel and return. It separately attempts the same harmless relative movement with `SendInput` and logs both results. No keyboard, button, wheel, SAS, password, or clipboard action is generated.

## Risks / Trade-offs

- [The Winlogon desktop may not render on the Parsec virtual display] → Capture and log the primary output plus GDI fallback before integrating VDD-specific selection.
- [Desktop Duplication may still return `E_ACCESSDENIED` despite SYSTEM identity] → Preserve the exact HRESULT, token/session/desktop evidence, and GDI result so the next design decision is evidence-based.
- [The current FakerInputBridge may not be available before login] → Report bridge availability independently; do not treat it as proof that desktop capture failed.
- [A SYSTEM network host would increase attack impact] → This POC contains no network listener and does not move production networking into SYSTEM.
- [Global objects can become an elevation boundary] → Apply an explicit Administrators/System-only security descriptor and keep commands fixed with no payload parsing.
- [A stuck agent could block service shutdown] → Signal a stop event, wait with a timeout, and terminate only the child process created by this service instance.
- [GPL/AGPL reference implementations cannot be copied into a closed product] → Implement against Windows APIs and the project's own code; use permissive references only for architecture understanding.

## Migration Plan

1. Build and run `--probe` as the current user to validate logging and screenshot output without service mutation.
2. On device 99, explicitly install and start the POC service from an elevated terminal.
3. Request an unlocked screenshot and mouse test.
4. Lock the device through ToDesk, request another screenshot and mouse test, and collect the log/BMP files.
5. Unlock and verify the same agent detects the desktop transition and captures again.
6. Stop and uninstall the POC service.
7. Only after the evidence is reviewed, create a separate production change for service/agent integration.

Rollback is limited to stopping and deleting the POC service and removing the standalone POC binary/output folder. Production FSRemote files are untouched.

## Open Questions

- Does the active Winlogon desktop expose the Parsec VDD as the primary output on device 99?
- Does `DuplicateOutput` succeed for the selected output when the agent is SYSTEM in the active session?
- Does the installed FakerInput bridge accept commands from the SYSTEM agent before user unlock?
- Should a production version keep capture/encode/WebRTC in the agent, or share D3D11 textures with an unprivileged network process?
