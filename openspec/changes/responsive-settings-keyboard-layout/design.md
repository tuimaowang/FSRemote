## Context

The main Qt shell is hand-painted in `DeviceGrid` and still uses fixed 920x680-era rectangles for titlebar, Settings, and device detail content. Recent sidebar work moved Settings to the titlebar and removed the sidebar footer, so this change can focus on making the right-side content stretch, simplifying noisy headers, and adding remote-window keyboard controls.

## Goals / Non-Goals

**Goals:**
- Keep the current hand-painted Qt architecture while making the main window resizable.
- Share the same content-left/content-width calculation between Settings and device detail so collapsed sidebar behavior is consistent.
- Replace the Settings page title with compact tab navigation and add a Keyboard view for remote-window shortcuts.
- Keep batch-add behavior local to the Settings page while accepting multiple wildcard subnets in one input.
- Add lightweight remote-window shortcut actions without changing the streaming protocol.

**Non-Goals:**
- No new external UI framework or rendering dependency.
- No compile/build step in this change.
- No persistence migration beyond using existing remote-window geometry storage for restore/tile behavior.

## Decisions

- Use helper layout functions around `DeviceGrid::width()` and `height()` instead of rewriting all UI into Qt layouts. This keeps the change scoped and compatible with the existing painter/control code.
- Keep the left sidebar at its established visual width when expanded. Resizing stretches the content region to the right and bottom, while collapsed mode shifts content to the left.
- Render new Settings option icons with small `QPainter` primitives. This avoids adding resource churn and lets the icons align to the new responsive row rectangles.
- Treat remote-window shortcuts as shell-level commands that can be invoked from `DeviceGrid` and forwarded from each `RemoteDesktopWindow`. This keeps window management centralized in the grid while allowing focused remote windows to respond.
- Use Ctrl+W as the close-topmost default only when no persisted shortcut exists; an explicit user customization remains authoritative across upgrades.
- Track physical hook state separately from key-down events actually consumed by the active hook target. A key-up is suppressed only when its matching key-down belongs to that target; unmatched releases continue through `CallNextHookEx` so activating a remote window mid-chord cannot leave a local modifier logically held.
- Keep the existing injected-key isolation unchanged in this focused fix because passing an injected event onward can reach the remote window's Qt fallback and be forwarded again. Shortcut cleanup emits generic and left/right modifier releases so the controlled-side per-session key tracker can release the exact virtual-key code originally forwarded.
- Store tile/restore geometry in memory for the current session. Persisted JSON geometry remains the source for normal window opening.

## Risks / Trade-offs

- Fixed-coordinate remnants may remain in older device-card drawing paths -> Mitigation: constrain this change to visible Settings, detail tabs, and control hit rectangles, and run static searches after editing.
- Remote-window focus order can be imperfect if the OS activates windows outside the app -> Mitigation: update ordering on open/raise and on remote-window activation events.
- Multi-subnet batch scans can become large -> Mitigation: parse only wildcard subnet patterns already supported by existing scan code and de-duplicate IPs before scanning.
