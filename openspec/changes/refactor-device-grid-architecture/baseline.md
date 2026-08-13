## Production Source Baseline

Recorded before the first business-code refactor on 2026-07-20.

| File | Bytes | Lines |
|---|---:|---:|
| `src/ui/DeviceGrid.cpp` | 499201 | 10189 |
| `src/ui/DeviceGrid.h` | 27421 | 430 |
| `src/ui/RemoteDesktopWindow.cpp` | 190045 | 4119 |
| `src/ui/RemoteDesktopWindow.h` | 20806 | 306 |

## DeviceGrid Authoritative-State Boundary

The following mutable objects were located in the anonymous namespace at the start of the refactor:

- `g_devices`
- `g_deviceGroupNames`
- `g_deviceGroupExpandedStates`
- `g_deviceGroupIds`
- `g_deviceSnapshotRevision`
- `g_deviceSnapshotUpdatedAt`
- `g_deviceSnapshotUpdatedBy`
- `g_deviceSnapshotTombstones`
- `g_deviceSyncStarted`
- `g_deviceSyncApplyingRemote`

The migration must separate authoritative device/group/snapshot data from the two synchronization lifecycle flags, which are repository/service state rather than catalog entities.

## Baseline Verification

- Release build directory: `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Release`
- Generator: `NMake Makefiles JOM`
- `FSRemote` target: passed
- `fsremote_device_sync_tests` target: passed
- `ctest -R fsremote_device_sync_tests --output-on-failure`: passed (1/1)

## Refactor Progress (2026-07-20)

The first catalog/repository migration reduced `DeviceGrid.cpp` from 499,201 bytes / 10,189 lines to 490,765 bytes / 9,034 lines. The removed code was duplicate snapshot assembly, legacy JSON loading, and direct `QSaveFile` persistence; those responsibilities now live in `DeviceCatalog` and `DeviceCatalogRepository`.

- `FSRemote`: Release x64 build passed after repository migration.
- `fsremote_device_sync_tests`: passed.
- `fsremote_device_catalog_tests`: passed, including repository round-trip coverage.
- `WJY_CODE_CHANGE_LOG.md`: intentionally unchanged per user instruction.

## Refactor Progress (continued)

The second migration slice moved all `DeviceGrid` device/group writes behind stable-ID catalog methods and made the UI views read-only. It also moved sync lifecycle/submission suppression into `DeviceCatalogRepository` and added `DeviceActionTargetResolver` for single, multi-select, and group targets.

- `DeviceGrid.cpp`: 490,163 bytes / 8,961 lines.
- `fsremote_device_action_target_tests`: passed.
- Focused tests: 3/3 passed (`device_sync`, `device_catalog`, `device_action_target`).

## Refactor Progress (action routing slice)

Batch device actions now share stable-ID target normalization and an explicit action eligibility policy. Wake, shutdown, restart, update, and terminal entry points retain their prior offline/unknown/busy semantics while no longer duplicating target filtering loops.

- `DeviceActionTargetResolver`: filters stale IDs, deduplicates targets, and restores current indexes after reorder.
- `DeviceActionPolicy`: centralizes action-specific presence-state rules.
- `FSRemote` compilation reached the final link step; the only failure was `LNK1104` because the existing `FSRemote.exe` process (PID 72804) held the output file open.
- Focused tests remained 3/3 passing after the policy integration.

## Refactor Progress (status result slice)

The status worker now returns one `DeviceStatusRefreshResult` containing complete per-device status records plus update availability. The UI still owns the final application policy, but no longer receives or maintains nine parallel worker result maps; compile-time verification passed through the `DeviceGrid.cpp` compile stage and all three focused tests passed.

Current production size after this slice: `DeviceGrid.cpp` is 489,041 bytes / 8,954 lines. Overall OpenSpec progress is 15/41 tasks; status-result UI application remains intentionally pending for the next extraction slice.
