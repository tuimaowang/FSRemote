## 1. Replace the coordinator policy

- [ ] 1.1 Replace `RemoteQualityCoordinator.cpp` role resolution with static single-window and multi-window foreground/background presets.
- [ ] 1.2 Remove internal FPS hysteresis, pressure timers, presenter-pressure degradation, and background count梯度 while preserving minimized and software-fallback safety branches.
- [ ] 1.3 Keep `RemotePerformanceSignalSampler` as a diagnostic-only helper and remove its use from quality decisions.
- [ ] 1.4 Ensure bitrate, resolution, FPS, priority, role, and degradation reason are deterministic for identical window snapshots.

## 2. Integrate stable role changes

- [ ] 2.1 Verify `DeviceGrid` focus snapshots map exactly one visible non-minimized window to the multi-window foreground role.
- [ ] 2.2 Verify window close, hide, minimize, restore, and software-fallback transitions immediately produce the corresponding safety or foreground/background preset.
- [ ] 2.3 Confirm the existing online quality request deduplication sends only changed role decisions without reconnecting the viewer.

## 3. Tests and documentation

- [ ] 3.1 Rewrite coordinator tests for one-window 60 FPS, multi-window foreground 60 FPS, static background FPS, focus handoff, minimized/hidden safety, and software fallback.
- [ ] 3.2 Add regression coverage proving repeated evaluations with changing performance counters do not alter a fixed role decision.
- [ ] 3.3 Build and run the focused coordinator tests, then complete a Release x64 compile/link when the running `FSRemote.exe` is closed.
- [ ] 3.4 Append the deletion/replacement details, final line numbers, and verification results to `WJY_CODE_CHANGE_LOG.md`.
