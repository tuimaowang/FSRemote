## Context

`DeviceGrid` currently owns device/group storage, snapshot serialization, shared synchronization, real-time status reduction, settings widgets, script state, remote commands, remote-window collections, global shortcuts, painting, hit testing, drag/drop, timers, and joinable background threads. `RemoteDesktopWindow` is also large, but it already delegates selected concerns to lifecycle, quality, input-broadcast, and D3D11 presentation helpers. The safest high-value starting point is therefore to turn `DeviceGrid` into a presentation coordinator without changing user-visible behavior.

The repository contains multiple active changes that touch remote-window quality, input broadcast, and multi-controller hosting. This refactor must preserve those implementations, avoid unrelated cleanup, and migrate responsibilities through narrow adapters rather than rewriting active subsystems.

The persisted device-list schema already includes stable device/group IDs, revision metadata, local-only expansion state, and tombstones. Those semantics are compatibility requirements. The project also requires source-level WJY ownership markers and detailed Chinese comments; the user explicitly opted out of the Markdown WJY change log for this refactor.

## Goals / Non-Goals

**Goals:**

- Establish one authoritative in-memory catalog for devices, groups, stable IDs, snapshot metadata, and local expansion state.
- Remove anonymous-namespace global mutable device/group containers from UI business logic.
- Move persistence and synchronization translation behind a focused repository boundary.
- Move remote-window collections, activation order, tiling state, and window lifecycle routing behind a focused coordinator.
- Move action-target normalization and repeated single/batch routing behind focused helpers/controllers.
- Replace duplicated current-script member fields plus per-device copies with one authoritative per-device script-state store.
- Centralize repeated layout/visibility/background-task patterns where this safely reduces production code.
- Keep changes incremental, buildable, testable, and behavior-preserving after every migration slice.

**Non-Goals:**

- Do not redesign the application UI or replace the hand-painted device list during the initial refactor.
- Do not change the device JSON schema, remote protocol, DLL exports, ports, command formats, or update format.
- Do not alter active multi-window quality, input broadcast, or multi-controller semantics except for dependency routing needed by extraction.
- Do not compress code using macros, opaque templates, or shortened naming merely to reduce line count.
- Do not create or append `WJY_CODE_CHANGE_LOG.md` for this change.

## Decisions

1. **Migrate through a `DeviceCatalog` façade before changing call sites broadly.**
   - The catalog owns device/group records and snapshot metadata, offers stable-ID/IP lookup, and emits no UI behavior by itself.
   - Existing `DeviceGrid` code will first consume catalog accessors while behavior remains unchanged; only then will mutation paths move.
   - Alternative considered: immediately convert the sidebar to `QAbstractItemModel`. Rejected because it combines architecture migration with a high-risk interaction rewrite.

2. **Separate authoritative domain state from transient presentation state.**
   - Device/group records, revision metadata, and tombstones belong to the catalog/repository.
   - Selected rows, hover, drag candidates, animation progress, and scroll offsets remain in `DeviceGrid` or later presentation helpers.
   - Alternative considered: place all state in a single application store. Rejected because transient paint state would unnecessarily couple unrelated consumers.

3. **Preserve snapshot normalization and synchronization algorithms.**
   - `DeviceListSyncModel` remains the authority for schema normalization and merge semantics.
   - A repository translates between catalog records and normalized snapshots and coordinates local atomic persistence plus `DeviceListSyncService` submission.
   - Alternative considered: replace JSON snapshots with a new database. Rejected because it changes deployment and compatibility without being required for maintainability.

4. **Extract remote-window orchestration without replacing existing coordinators.**
   - A new remote-window coordinator owns window maps, tiled collections, activation order, restore geometry, and topmost-window operations.
   - `RemoteViewerLifecycleManager`, `RemoteQualityCoordinator`, and `RemoteInputBroadcastCoordinator` retain their specialized responsibilities.
   - Alternative considered: merge all remote helpers into one manager. Rejected because it would recreate a monolith under a different name.

5. **Use one per-device script state as the source of truth.**
   - The currently selected device exposes a reference/view into the per-device state map instead of copying fields into a second set of members.
   - Async callbacks resolve the intended stable device/IP state before applying results.
   - Alternative considered: keep the duplicated visible fields and only move methods to another file. Rejected because that would not reduce state or eliminate synchronization bugs.

6. **Reduce repeated action routing through typed operations, not a generic callback maze.**
   - Common target validation, stable-ID resolution, online-state filtering, and result aggregation are shared.
   - Action-specific eligibility and side effects remain explicit.
   - Alternative considered: one large enum/switch for every device command. Rejected because it hides important differences such as offline wake eligibility.

7. **Keep background work bounded and UI-safe.**
   - Extracted controllers must not touch widgets from worker threads, must use guarded UI delivery, and must participate in deterministic shutdown.
   - Existing joinable-task and lifecycle-manager patterns remain until a replacement is proven by tests.

8. **Treat code-size reduction as a secondary measurable result.**
   - Production duplication, mirrored state, repeated validation, and obsolete commented implementations may be removed.
   - Interfaces and tests may increase total repository lines; success is measured by lower production complexity and clearer ownership, not a minimum line count.

## Risks / Trade-offs

- **Large migration can conflict with active remote-control changes** → Extract one responsibility at a time and preserve existing specialized coordinators rather than rewriting them.
- **Replacing indexes with stable IDs can expose hidden index assumptions** → Add lookup/selection tests and keep indexes local to rendering only.
- **Catalog/repository split can accidentally change JSON ordering or metadata** → Round-trip normalized snapshots in tests and reuse `DeviceListSyncModel` unchanged.
- **Script-state de-duplication can route late async output to the wrong device** → Key all callbacks by stable device IP/run ID and test device switching during active work.
- **Moving window ownership can change destruction order** → Define explicit shutdown order and verify all viewer stop tasks join before coordinator destruction.
- **Physical extraction may initially increase code size** → Complete each slice by deleting migrated duplicate implementations before measuring production reduction.

## Migration Plan

1. Add characterization tests and a catalog model with snapshot round-trip coverage.
2. Route `DeviceGrid` reads and mutations through the catalog while retaining the existing UI and save/sync entry points.
3. Move persistence and synchronization conversion into a repository and remove global device/group containers.
4. Extract remote-window orchestration and verify normal, tiled, close-all, activation, quality, and input-broadcast behavior.
5. Extract action target normalization and shared batch routing without changing action-specific eligibility.
6. Replace duplicated script presentation members with one per-device state store, then move script-panel behavior behind a focused controller/panel.
7. Consolidate repeated settings/layout/background-task code and remove proven-dead commented implementations.
8. Build Release x64, run focused tests plus the existing suite, and perform static lifecycle review after each slice.

Rollback is per slice: newly introduced façades retain compatibility adapters until all callers migrate, so a failing slice can revert without changing stored data or protocols.

## Open Questions

- Whether the final hand-painted device list should later migrate to `QTreeView` is intentionally deferred until responsibility separation is stable.
- Whether `DeviceCatalog` should ultimately be owned by `MainWindow` or an application context can be decided after the first migration; the initial implementation may remain owned by `DeviceGrid` to preserve destruction order.
- `RemoteDesktopWindow` decomposition will follow the same standards after the `DeviceGrid` refactor establishes tested extraction patterns.
