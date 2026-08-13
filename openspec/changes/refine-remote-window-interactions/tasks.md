## 1. Cursor-Prioritized Window Snapping

- [x] 1.1 Add explicit cursor-edge and moving-window-edge trigger sources to nearby snap candidate selection.
- [x] 1.2 Make cursor-triggered candidates outrank simultaneous moving-window-edge candidates while preserving existing group layout, preview, and screen-edge fallback behavior.
- [x] 1.3 Perform a static review of all snap directions, thresholds, and fallback paths without building.

## 2. Input Synchronization Participation and UI

- [x] 2.1 Add a locally excluded endpoint state and safe follower-to-excluded-to-master transitions to the input broadcast coordinator.
- [x] 2.2 Update routing, registration, eligibility, teardown, and held-input release behavior for excluded endpoints.
- [x] 2.3 Replace the compact synchronization glyph with distinct off, master, follower, and excluded status visuals and action tooltips.
- [x] 2.4 Update focused coordinator tests for the new state transitions without running a build.

## 3. Responsive Remote Title-Bar Hit Testing

- [x] 3.1 Create one width-dependent remote title-bar layout snapshot with explicit control visibility priority.
- [x] 3.2 Use the snapshot for painting, hover, tooltip, press, release, context-menu exclusion, and title-bar dragging.
- [x] 3.3 Add focused layout coverage for minimum, narrow, and normal window widths without running a build.

## 4. Single-Click Tray Toggle

- [x] 4.1 Change tray left-click activation to toggle restored and minimized main-window states.
- [x] 4.2 Remove double-click-only close behavior while preserving the tray context menu and explicit quit action.

## 5. Persistent Taskbar Presence

- [x] 5.1 Convert normal main-window close requests and tray minimization from hiding to system minimization.
- [x] 5.2 Statically review restore, foreground activation, explicit quit, and restart paths without building.
- [x] 5.3 Advertise native Windows system-menu and minimize capability while retaining the frameless custom title bar.
- [x] 5.4 Remove custom system-command interception and rely on native taskbar and grouped-window behavior.

## 6. Single-Device Group Assignment

- [x] 6.1 Show the existing group-assignment submenu for every normalized non-empty device target set.
- [x] 6.2 Reuse existing group creation, assignment, persistence, reveal, and inline-rename paths for a single device.

## 7. Device Script Stop Actions

- [x] 7.1 Add stop-script to the shared single-device and multi-device context menu.
- [x] 7.2 Aggregate stop dispatch and no-running-script feedback once per normalized target set.

## 8. Active-Window Quality Policy

- [x] 8.1 Add active and full-screen state to quality evaluation inputs and trigger queued reevaluation on activation/state changes.
- [x] 8.2 Apply minimized/hidden and software fallback first, high quality to active or full-screen windows, and smooth quality to other visible windows.
- [x] 8.3 Convert the title-bar quality selector to a read-only effective-status presentation and retire per-window manual switching from the active UI.
- [x] 8.4 Update focused quality coordinator coverage for active, full-screen, smooth-background, minimized, and fallback precedence without running a build.

## 9. Controlled-Device Bubble Hover Intent

- [x] 9.1 Add a 150 ms single-shot hover-intent timer that expands only while the cursor remains over the stable collapsed badge.
- [x] 9.2 Cancel pending expansion across cursor leave, animation, hide, click forwarding, and drag transitions without changing initial connection display.

## 10. Transparent Rounded Main Window

- [x] 10.1 Enable an alpha-backed translucent surface for the frameless main window without changing its native minimize/taskbar capabilities.
- [x] 10.2 Clear and clip `DeviceGrid` shell painting to the rounded window path so all four corner pixels remain transparent.
