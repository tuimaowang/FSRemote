## Context

`RemoteDesktopWindow` already calculates local resize edges and displays Qt resize cursors for its own frameless border. Inside the streamed image it falls back to the local arrow because the native Host sends video and input-mode/clipboard/quality control messages but never publishes the Windows `HCURSOR` selected by the controlled application. The Host already owns an auxiliary polling worker and each authorized session already has a reliable ordered WebRTC control channel back to its Viewer.

## Goals / Non-Goals

**Goals:**

- Mirror standard Windows cursor categories in the remote image with low latency and negligible bandwidth.
- Cover all four resize directions required for application-window resizing.
- Preserve local window chrome cursor priority and relative/game-mode hiding.
- Support multiple authorized Viewer sessions safely without holding dispatcher locks during WebRTC sends.
- Keep malformed or unknown cursor messages harmless and forward-compatible.

**Non-Goals:**

- Transmitting arbitrary custom cursor bitmap pixels, animation frames, hotspot coordinates, or cursor scale.
- Changing mouse input coordinate mapping or resize behavior itself.
- Making old Host DLLs synthesize cursor shape; they continue showing the arrow inside the remote image.

## Decisions

1. Extend the native session protocol with a versioned `__fsremote_cursor_v1 <shape>` message and a fixed enum/token whitelist. Serialization and parsing live beside the existing session protocol so malformed/unknown shapes are testable and never reach input injection.
2. Map the Host's current `CURSORINFO.hCursor` against standard cursors loaded with `LoadCursorW`. When that result is arrow, hidden, or unknown, ask the top-level window under the pointer for `WM_NCHITTEST` through `SendMessageTimeoutW` and accept only the eight standard resize-edge results. Failure or timeout falls back to arrow.
3. Poll the Host cursor every 25 ms in the existing auxiliary state worker, while retaining the current clipboard cadence. Skip classification entirely when no Viewer subscribes, and only fan out a changed enum to registered authorized sessions.
4. Copy callbacks while holding the input dispatcher mutex, then send control messages outside it. This follows the existing mouse-mode and clipboard pattern and prevents a WebRTC callback from stalling serialized `SendInput` processing.
5. ViewerInstance parses the cursor control message and reports it through a new `FSREMOTE_STATUS_CURSOR_SHAPE` status code. The public callback ABI does not change, so only the updated DLL and Qt application need to agree on the new optional status.
6. `RemoteDesktopWindow` caches the latest remote shape and applies an equivalent Qt cursor only while the pointer is inside `remoteImageRect()`. Its own resize edges and title-bar actions remain higher priority. Both the parent window and D3D presenter receive the cursor because either can own the mouse event.
7. Relative/game mouse mode remains the highest priority and keeps `BlankCursor`. Returning to desktop mode re-evaluates the pointer position and restores the cached remote cursor immediately.

## Risks / Trade-offs

- [Custom application cursors do not equal a standard system handle] → Fall back to arrow; pixel cursor transport remains a separate future capability.
- [Polling adds Host work] → `GetCursorInfo` and handle comparisons are lightweight, run at 40 Hz, and send no network data while unchanged.
- [Cursor state can race with pointer movement by one poll] → A 25 ms interval bounds the visible delay while preserving the ordered control channel.
- [Multiple controllers observe one global Windows cursor] → Broadcast the same Host state to all authorized sessions, matching the shared desktop and shared input model.
- [New Viewer with old Host receives no status] → Keep arrow fallback and all existing input/video behavior unchanged.

## Migration Plan

The control DataChannel reference is synchronized across initialization, WebRTC callbacks, sending, and teardown. Absolute injected coordinates target the Windows virtual desktop so the queried cursor position remains aligned on multi-monitor Hosts.

Deploy the updated Qt application and native stream DLL together. The message is optional and version-prefixed, so removing the new parser/publisher or rolling either side back leaves video and input functional with the prior arrow-only behavior.

## Open Questions

Whether future versions need exact custom cursor bitmap/hotspot transport will be decided after standard Windows cursor coverage is tested in real applications.
