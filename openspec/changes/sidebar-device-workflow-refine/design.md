## Context

`DeviceGrid` owns a hand-painted frameless main window. The left sidebar currently draws device rows, group rows, the settings entry, the titlebar wordmark, and window controls in one paint path, and it handles mouse interactions for selection, double-click open, drag/drop, settings toggles, and device/group rename editors. Several recent visual tweaks already live in this file, so this change should remain narrow and reuse the existing painting/helper approach instead of adding a new widget hierarchy.

## Goals / Non-Goals

**Goals:**
- Make the sidebar visual layout more compact and aligned: scrollbar gutter, settings row, titlebar settings icon, collapse button, blank area, and status capsules.
- Move remote launching to the top-left titlebar button and make double-click consistently mean rename for devices and groups.
- Preserve existing remote desktop opening and wake-on-LAN behavior while adding multi-selection launch support.
- Add group reordering through drag/drop while preserving group membership.
- Sort root devices and each group's devices by display name leading character.

**Non-Goals:**
- Do not replace the hand-painted sidebar with Qt item views.
- Do not redesign `RemoteDesktopWindow` rendering, streaming, or geometry persistence.
- Do not change the `devices.json` file shape unless a small helper is needed for ordering.
- Do not add new external dependencies.

## Decisions

1. **Keep layout constants local to `DeviceGrid.cpp`.**
   - Rationale: The existing UI is coordinate-driven. Centralizing new constants for settings divider, settings row height, titlebar settings icon, and sidebar scroll gutter keeps drawing and hit testing synchronized.
   - Alternative considered: Convert controls to child widgets. Rejected because it would be a larger rewrite than the requested visual adjustments.

2. **Use helper functions for sorted visible rows.**
   - Rationale: Sorting should be applied consistently for paint, hit testing, drag/drop, and double-click. The existing `visibleDeviceRows()` is the right single source for row order.
   - Alternative considered: Sort only during painting. Rejected because hit testing would no longer match the visible order.

3. **Represent group dragging with a separate drag mode.**
   - Rationale: Device dragging already has candidate/active state and drop logic. Group dragging needs a parallel target type because it reorders groups rather than changing device membership.
   - Alternative considered: Treat group rows as devices. Rejected because group movement must carry member devices by preserving group names and changing group order only.

4. **Use inline rename editors for both device and group rename.**
   - Rationale: Group rename already has an in-place `QLineEdit`. Device rename should reuse the same editing pattern so double-click behavior feels consistent.
   - Alternative considered: Keep the existing modal device rename dialog. Rejected because the request explicitly asks to reference group-name renaming.

5. **Launch selected devices from the titlebar wordmark button.**
   - Rationale: This replaces double-click open with a visible launch affordance. The action can iterate selected devices and reuse existing open/wake logic.
   - Alternative considered: Add a new toolbar button. Rejected because the request names the top-left wordmark as the button.

## Risks / Trade-offs

- **Coordinate regressions in hand-painted UI** -> Use constants for shared Y positions and keep hit rectangles close to draw rectangles.
- **Sorted order can change expected drag indexes** -> Always use `DeviceListRow` values from `visibleDeviceRows()` instead of row indexes as data indexes.
- **Launching multiple remote windows may cascade poorly** -> Reuse existing saved remote window geometry and tile only when multiple windows are opened together.
- **Wake plus launch for offline devices may show a connecting window before the device is reachable** -> Reuse existing wake visual and remote window creation so behavior is immediate and visible.
- **Inline device rename can conflict with selection changes** -> Commit/cancel active rename before starting other sidebar actions.
