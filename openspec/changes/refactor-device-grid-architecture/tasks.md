## 1. Baseline and Characterization

- [x] 1.1 Record the current production source sizes and identify the exact `DeviceGrid` global/device-state boundaries without modifying behavior.
- [x] 1.2 Add focused characterization tests for normalized device/group snapshot round trips, stable IDs, group expansion preservation, revision metadata, and tombstones.
- [x] 1.3 Confirm the existing targeted test suite and Release x64 build baseline before high-risk migration.

## 2. Device Catalog Extraction

- [x] 2.1 Add shared device/group domain types with stable IDs and WJY ownership comments.
- [x] 2.2 Implement a `DeviceCatalog` that owns devices, groups, expansion state, revision metadata, and stable lookup by ID/IP.
- [x] 2.3 Add catalog tests covering lookup, rename, grouping, removal, selection-safe identity, and snapshot application.
- [x] 2.4 Route `DeviceGrid` read paths through the catalog while preserving the current rendering order and indexes as transient presentation positions.
- [x] 2.5 Route device/group mutation paths through the catalog and remove superseded anonymous-namespace global containers.

## 3. Persistence and Synchronization Boundary

- [x] 3.1 Implement a catalog repository that converts catalog state to/from normalized snapshots and performs atomic local persistence.
- [x] 3.2 Route `DeviceListSyncService` submission and incoming snapshots through the repository without recursive resubmission.
- [x] 3.3 Preserve local-only group expansion state during synchronized snapshot application and add regression tests.
- [x] 3.4 Remove the superseded `DeviceGrid` snapshot serialization, loading, saving, and sync-application implementation.

## 4. Device Actions and Status Coordination

- [x] 4.1 Introduce stable device target resolution and shared validation for single, selected, and group actions.
- [x] 4.2 Consolidate repeated batch wake, shutdown, restart, update, and terminal routing while keeping action-specific eligibility explicit.
- [x] 4.3 Extract status-refresh result collection and UI-safe application from `DeviceGrid` without changing real-time state precedence.
- [x] 4.4 Add focused tests for target normalization, offline/online filtering, stale identity rejection, and busy-session fallback behavior.
- [x] 4.5 Remove duplicate action/status validation and migrated production code from `DeviceGrid`.

## 5. Remote Window Orchestration

- [x] 5.1 Add a focused remote-window coordinator for normal-window deduplication, tiled collections, activation order, restore geometry, and close-all behavior.
- [x] 5.2 Migrate remote-window creation and lifecycle routing while preserving viewer admission, input broadcast, quality coordination, update availability, and controller-target leases.
- [x] 5.3 Migrate global shortcut window operations to the remote-window coordinator.
- [x] 5.4 Add focused lifecycle tests or bounded non-network tests for window identity, activation order, tiling state, and shutdown ordering where practical.
- [x] 5.5 Remove superseded remote-window collections and orchestration methods from `DeviceGrid`.

## 6. Script State and Panel Separation

- [x] 6.1 Replace duplicated current-script members with one authoritative per-device `ScriptUiState` store.
- [x] 6.2 Key asynchronous script/editor completion by stable device IP and run identity so device switching cannot misroute results.
- [x] 6.3 Extract script tree, output, editor visibility, and state-transition logic into a focused panel/controller boundary.
- [x] 6.4 Preserve execute, stop, group execution, recovery, copyable output, and editor behavior with focused state tests.
- [x] 6.5 Remove superseded script state copies and migrated methods from `DeviceGrid`.

## 7. Settings, Layout, and Input Code Reduction

- [x] 7.1 Introduce shared layout snapshots/helpers used by painting, hit testing, and real child-widget geometry.
- [x] 7.2 Consolidate repeated settings control visibility, geometry, enabled-state, and persistence updates without hiding option-specific behavior.
- [x] 7.3 Consolidate guarded background-task delivery patterns while preserving bounded threads and deterministic shutdown.
- [x] 7.4 Remove proven-dead commented implementations and unused helpers after confirming no references remain.
- [x] 7.5 Separate focused renderer/interaction helpers where this reduces `DeviceGrid` responsibility without changing the hand-painted UI.

## 8. RemoteDesktopWindow Follow-up Decomposition

- [x] 8.1 Inventory `RemoteDesktopWindow` responsibilities and protect current frame, input, clipboard, cursor, reconnect, sizing, and title-bar behavior with focused tests.
- [x] 8.2 Extract remaining connection/status state transitions that do not belong to the widget itself.
- [x] 8.3 Extract clipboard/input/title-bar helpers where ownership is clear and remove migrated duplicates.
- [x] 8.4 Verify D3D11 presentation, CPU fallback, resize/cursor synchronization, quality updates, and shutdown behavior.

## 9. Verification and Completion

- [x] 9.1 Build Release x64 after each completed migration group and fix all compilation/link errors before continuing.
- [x] 9.2 Run all focused and existing CTest targets relevant to device sync, status, remote windows, input, quality, cursor, and update behavior.
- [x] 9.3 Compare final production source sizes and document which reductions came from deleted duplication versus responsibility extraction.
- [x] 9.4 Perform a final static lifecycle review for worker/UI boundaries, stable identity, persistence compatibility, and shutdown order.
- [x] 9.5 Confirm all edited source blocks contain appropriate WJY markers and detailed Chinese comments while leaving `WJY_CODE_CHANGE_LOG.md` unchanged.

> Progress note (2026-07-20): DeviceCatalog ownership and the repository persistence boundary are implemented. `DeviceGrid` still retains temporary mutable-reference adapters for rendering and legacy action paths; those are intentionally left until the remaining mutation call sites migrate to stable-ID APIs.

> Progress note (2026-07-20): `ScriptPanelVisibility` now owns the pure page-visibility decision for the script tree, terminal output, and file editor, with a focused regression test. The asynchronous editor/output state transitions and legacy projection fields remain in `DeviceGrid` until they can be migrated without weakening request-id and device-switch guards.

> Progress note (2026-07-20): `RemoteWindowRegistry` now owns normal-window identity, tiled collections, activation order, restore geometry, and deduplicated shutdown snapshots. Its non-network test uses ordinary `QObject` instances, while `RemoteWindowCoordinator` retains real `RemoteDesktopWindow` close/closing-state behavior.

> Progress note (2026-07-20): The current script-panel projection fields have moved out of `DeviceGrid` into the private `ScriptPanelController` boundary, without changing existing field access or asynchronous request-id guards. The remaining migration is limited to moving the state projection methods themselves and then deleting their `DeviceGrid` wrappers.

> Completion note (2026-07-20): `ScriptPanelController` now owns the current-page projection fields and the save/apply transitions between visible controls and `ScriptUiState`. The superseded `DeviceGrid` fields plus `saveVisibleScriptUiFields` / `applyVisibleScriptUiState` declarations and implementations have been removed; `ScriptUiStateStore` remains the authoritative per-device state owner.
