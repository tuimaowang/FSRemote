## Context

`DeviceGrid` owns the affected interaction surface: left-sidebar device/group rows, right-click menus, drag/drop state, the device-detail tabs, and the hand-painted script/config panels. Recent changes already moved the device detail and settings pages onto shared helper rectangles such as `deviceDetailBottomBoundaryTop()`, `scriptTerminalPanelRect()`, `scriptFileEditorRect()`, and `settingsScrollViewportRect()`. This change should continue that pattern and keep painting, hit testing, and real child widgets synchronized from the same geometry helpers.

The existing code already has single-device system actions, group script execution/stop actions, group reordering drag state, multi-device selection state, and script execution state keyed per device IP. The requested behavior is mostly a UI and routing refinement around those existing flows rather than a new service layer.

## Goals / Non-Goals

**Goals:**
- Remove unfinished context-menu entries from the callable UI by commenting out group tiling and device rename/delete menu display and handlers.
- Add group and multi-device system submenus for wake, shutdown, restart, and terminal actions, with a separator above the group system submenu.
- Preserve multi-selection when right-clicking an already selected device and apply batch actions to that selection.
- Add a script tree on the Script Log tab for browsing the shared script path and selecting a script folder to execute.
- Split the script action into explicit Execute and Stop buttons.
- Present script output in a read-only selectable text control so users can copy logs without allowing edits.
- Restyle the Config File editor area as black background with white text.
- Add a red translucent drop target below the title bar that says "解散分组" for group dragging and "移出分组" for device dragging.
- Keep device detail and settings content boundaries identical before and after sidebar collapse.

**Non-Goals:**
- Do not introduce a new model/view sidebar or replace the existing hand-painted UI.
- Do not change the device JSON schema or script state persistence shape unless a transient UI state member is needed.
- Do not redesign the remote desktop window, terminal transport, or script execution protocol.
- Do not add new third-party dependencies.

## Decisions

1. **Keep context-menu hiding as commented-out menu code.**
   - Rationale: The user explicitly asked to comment out the visible entries so they cannot be invoked. The implementation should comment out `addAction` calls and their matching `selectedAction` branches for group "设备平铺" and device "重命名"/"删除设备".
   - Alternative considered: Leave disabled menu entries. Rejected because disabled entries would still display, while the request is to comment out menu display.

2. **Route system actions through reusable batch helpers.**
   - Rationale: Single-device actions and multi-device actions should share the same wake/shutdown/restart/terminal behavior and status checks. A helper that accepts a `QSet<int>` or `QVector<int>` of device indexes keeps group menus and multi-select device menus consistent.
   - Alternative considered: Duplicate action handling in each menu branch. Rejected because the online/offline filtering and future fixes would diverge.

3. **Preserve selection on right-click when the clicked device is already selected.**
   - Rationale: Multi-select right-click must act on the current selection. If the clicked row is outside the selection, the existing single-select right-click behavior remains appropriate.
   - Alternative considered: Always use all selected devices regardless of where the user right-clicks. Rejected because right-clicking an unselected row should naturally target that row.

4. **Build the Script Log tree as a lightweight child widget synchronized to painted panel geometry.**
   - Rationale: Script paths are hierarchical and interactive, so a `QTreeWidget` or `QTreeView` is more reliable than hand-drawing expandable rows. It can sit inside the Script Log panel's left column and be shown/hidden together with existing `QTextEdit`/button controls.
   - Alternative considered: Render the tree entirely with `QPainter`. Rejected because selection, expansion, keyboard focus, and scrolling would be reimplemented manually.

5. **Split script controls into two explicit buttons.**
   - Rationale: The user asked for one Execute button and one Stop button. Separate hit rectangles avoid the current mode-switch button ambiguity and make it possible to stop a running script without changing the Execute affordance.
   - Alternative considered: Keep the current toggle button text. Rejected because it does not match the requested UI.

6. **Use a shared top drop-zone helper for both device and group drags.**
   - Rationale: The same red translucent region under the title bar is used for "移出分组" and "解散分组"; only the label and release behavior differ. A shared rectangle and draw helper keeps the visual feedback and hit test aligned.
   - Alternative considered: Reuse the existing root blank drop zone only. Rejected because the requested target is specifically below the title bar and should appear while dragging.

7. **Keep layout boundary helpers as the single source of truth.**
   - Rationale: Device detail, settings, script log, and config panels must align before and after collapse. All affected panel/widget rectangles should derive from `contentLeft()`, `contentWidth()`, `kDetailScriptPanelTop`, and `deviceDetailBottomBoundaryTop()`.
   - Alternative considered: Patch individual rectangles. Rejected because independent constants caused the overlap issues this change is meant to avoid.

8. **Use a read-only text widget for Script Log output.**
   - Rationale: `QTextEdit` supplies reliable mouse selection, keyboard selection, Ctrl+C, a standard copy context menu, and scrolling while remaining read-only.
   - Alternative considered: Implement selection ranges and clipboard handling in the hand-painted terminal. Rejected because it would duplicate mature Qt text interaction behavior and be harder to keep correct during live log updates.

## Risks / Trade-offs

- **Batch actions can accidentally include offline devices** -> Normalize targets per action: wake can include offline devices, terminal/shutdown/restart should skip or ignore targets that cannot accept remote commands.
- **Right-click can unexpectedly collapse selection** -> Only replace selection when the clicked row is not already selected.
- **Script tree may block paint-only layout assumptions** -> Treat the tree as a managed child widget like the config editor controls and update visibility/geometry during paint and resize-sensitive paths.
- **Network script path may be unavailable** -> Show an empty tree or loading/error placeholder without blocking the detail page; execution still requires selecting a valid folder.
- **Drag-to-dissolve group can be destructive** -> Reuse the delete-group data-safety flow: clear matching device `group` fields, remove the group name/expanded state, adjust rename indices, save, and repaint.

## Migration Plan

1. Add UI state and helpers for batch targets, script tree selection, two script buttons, and the shared top drop-zone.
2. Update context-menu construction and action routing without changing persisted device data.
3. Update Script Log and Config File panel geometry/style while preserving existing per-device script state.
4. Update drag release behavior so dropping a group on the top zone dissolves it and dropping devices on the same zone moves them to root.
5. Verify with static review and OpenSpec status only; do not compile unless requested separately.

## Open Questions

- For batch terminal, the expected behavior is to open one terminal per selected device because that matches the existing single-device terminal affordance.
- For the script tree, the initial root is the existing configured shared script path used by `populateScriptFolderMenu()`.
