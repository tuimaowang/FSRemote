## Why

`DeviceGrid` and `RemoteDesktopWindow` have accumulated UI rendering, mutable device data, persistence, synchronization, remote commands, script execution, window coordination, timers, and thread-lifecycle responsibilities in a small number of very large classes. The current structure makes behavior changes difficult to review, increases the risk of stale indexes and duplicated state, and forces unrelated features to modify the same files.

## What Changes

- Introduce a single device catalog boundary for devices, groups, stable identifiers, selection-safe lookup, snapshot conversion, and change notifications.
- Move device JSON persistence and shared snapshot synchronization behind a repository/service boundary while preserving the existing schema, revision, tombstone, and backward-compatibility behavior.
- Extract remote-window ownership and orchestration from `DeviceGrid`, while retaining the existing viewer lifecycle, quality, and input-broadcast coordinators.
- Extract device actions and batch-target normalization so wake, shutdown, restart, update, terminal, authorization, and related operations no longer depend on hand-painted UI state.
- Extract script panel state and operations, eliminating the duplicated visible-state/member-state representation currently copied when switching devices.
- Centralize repeated layout, visibility, status-refresh, and bounded background-task patterns where doing so reduces production code without changing behavior.
- Keep `DeviceGrid` as the presentation coordinator and preserve all existing visual behavior, device-list interactions, remote-control flows, compatibility fallbacks, and data formats.
- Add focused tests for extracted pure logic and lifecycle boundaries.
- Do not create or append a WJY Markdown change log for this user-requested refactor; source edits will still use WJY ownership markers and detailed Chinese comments.

## Capabilities

### New Capabilities

- `device-catalog-architecture`: Defines the single-source-of-truth device/group catalog, stable-ID operations, persistence boundary, and compatibility-preserving snapshot flow.
- `device-grid-responsibility-separation`: Defines the separation of device actions, remote-window orchestration, script-panel state, layout helpers, and background work from the main `DeviceGrid` presentation class.
- `behavior-preserving-code-reduction`: Defines measurable reduction of duplicated production state and logic while preserving UI, protocol, persistence, and lifecycle behavior.

### Modified Capabilities

- None.

## Impact

- Primary UI impact: `src/ui/DeviceGrid.cpp`, `src/ui/DeviceGrid.h`, and new focused UI/controller modules.
- Secondary UI impact: selected responsibilities currently coordinated with `src/ui/RemoteDesktopWindow.*`, `RemoteViewerLifecycleManager`, `RemoteQualityCoordinator`, and `RemoteInputBroadcastCoordinator`.
- Data/synchronization impact: `src/system/DeviceListSyncModel.*`, `DeviceListSyncService.*`, and new catalog/repository modules; persisted schema and public behavior remain compatible.
- Build/test impact: `CMakeLists.txt` gains new source files and focused tests.
- No intentional public API, remote protocol, DLL export, user-visible workflow, or external dependency changes.
