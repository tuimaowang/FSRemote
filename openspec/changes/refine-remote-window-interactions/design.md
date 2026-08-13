## Context

The controller uses hand-painted Qt title bars and menus, a shared input-broadcast coordinator, a top-level main window with `QSystemTrayIcon`, and a live remote-quality coordinator. Most requested behaviors already have reusable lower-level operations; the change primarily aligns state transitions, precedence rules, and UI hit testing without changing the Host protocol.

## Goals / Non-Goals

**Goals:**

- Make snap target selection deterministic when cursor and window-edge signals disagree.
- Keep painted title-bar state and clickable state identical at every window width.
- Support per-device input-sync participation without weakening held-input release barriers.
- Preserve both tray access and the Windows taskbar entry while the main window is minimized.
- Reuse stable device target resolution for single-device and multi-device menu actions.
- Apply quality automatically from window activity and visibility without reconnecting streams.

**Non-Goals:**

- Change the remote input, script execution, update, or live-quality wire protocols.
- Add synchronization between different controller processes.
- Build a new native title bar or replace the existing hand-painted UI framework.
- Disconnect background remote sessions to enforce the quality policy.

## Decisions

### 1. Represent snap trigger provenance explicitly

Each nearby-window snap candidate will carry whether it was selected by cursor-edge proximity or moving-window-edge proximity. Cursor candidates are evaluated first and always outrank window-edge candidates; distance and center proximity only rank candidates from the same trigger source. Both sources feed the existing aspect-ratio, group-layout, preview, and commit path.

### 2. Add a device-excluded input-sync state

The input coordinator will distinguish global-off, master, follower, and locally excluded states. A follower click excludes only that endpoint; the next click promotes it to master after the existing release barrier. Disabling the master clears the whole group and endpoint exclusions so the next session starts predictably.

### 3. Derive painting and hit testing from one title-bar layout snapshot

Remote title-bar controls will be assigned visibility by priority and available width. Only fully visible controls will be painted, hovered, pressed, included in tooltips, or excluded from title-bar drag handling. Close and minimize remain the highest-priority controls.

### 4. Minimize instead of hiding the main window

Single left-click tray activation will toggle between restored and minimized states. Normal close requests will be converted into minimization while explicit quit continues through the existing shutdown path. This retains the taskbar entry because the top-level `Qt::Window` remains present.

The frameless main window will retain native system-menu and minimize capabilities through `Qt::WindowSystemMenuHint` and `Qt::WindowMinimizeButtonHint`. Windows can then minimize a restored main window from its taskbar button and restore a minimized one without custom system-command interception. Native grouped-taskbar behavior remains responsible when remote windows are also open.

### 5. Keep device context actions target-set based

The shared device menu will expose group assignment and script stopping for every normalized non-empty target set. Single-device and multi-device actions will use the same stable-ID normalization and aggregate user feedback once per action.

### 6. Layer automatic quality above existing safety profiles

The quality coordinator will receive active and full-screen metrics. Minimized/hidden and software-fallback profiles retain safety precedence; otherwise full-screen and active windows request high quality while other visible windows request smooth quality. The existing live request/acknowledgement path remains unchanged, and the title-bar quality control becomes a status presentation rather than a manual mode selector.

### 7. Require hover intent before expanding the controlled-device bubble

The collapsed controlled-device badge will start a 150 ms single-shot hover-intent timer instead of expanding from one 16 ms cursor sample. Expansion occurs only if the cursor remains inside the stable collapsed-badge geometry when the timer expires. Leaving the badge, hiding the overlay, starting an animation, or entering click/drag handling cancels the pending expansion. Once expanded, the existing full-card leave detection continues to collapse the bubble normally. The first controller session still displays the complete bubble immediately.

### 8. Paint the main window on a transparent rounded surface

The frameless `MainWindow` will enable a translucent backing surface. `DeviceGrid` will clear that surface to transparent using source composition, clip all shell background painting to one antialiased rounded path, and then draw the existing rounded border inside the same shape. This removes the opaque rectangular corner fill while preserving the current title bar, sidebar, content colors, taskbar behavior, and custom window controls.

## Risks / Trade-offs

- [Cursor proximity could select an unintended adjacent edge] → Require the same snap-distance threshold and orthogonal projection checks used by window-edge snapping.
- [Rapid focus switching could cause repeated resolution changes] → Coalesce activation changes through the existing queued quality evaluation and add a short bounded stability delay if runtime testing shows churn.
- [Narrow-window controls could disappear earlier than before] → Use explicit priority and preserve close/minimize controls first; hidden controls have no invisible hit targets.
- [Per-device sync exclusion could retain held input] → Reuse the coordinator release barrier before exclusion or promotion.
- [Minimization behavior may differ across Windows shell versions] → Keep `Qt::Window`, avoid `hide()`, and verify tray, taskbar, restore, and explicit quit paths manually when builds are allowed.
