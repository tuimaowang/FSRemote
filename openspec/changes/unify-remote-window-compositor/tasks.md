## 1. Baseline and rollout control

- [x] 1.1 Record the current remote-window rendering path, title-bar surface, performance overlay, D3D presenter, software fallback, and resize entry points.
- [x] 1.2 Add a runtime feature switch that selects the new compositor without changing stream protocol or persisted device settings.
- [x] 1.3 Add a focused resize regression harness covering move, edge resize, maximize/restore, DPI change, reconnect, and device-loss fallback.

## 2. Compositor and surface ownership

- [x] 2.1 Create the unified remote-window compositor interface and explicit presentation states: Idle, InteractiveResize, FinalizeResize, HardwareFallback, and DeviceRecovery.
- [x] 2.2 Create a single retained layout snapshot containing physical output geometry, content rectangle, title-bar layout, DPI, and input mapping data.
- [x] 2.3 Implement one visible presentation surface that owns remote content, letterboxing, title-bar pixels, performance information, connection overlays, and update masks.
- [x] 2.4 Ensure local UI layers are cached and recomposed atomically without independent visible overlay HWNDs.

## 3. Hardware and software presentation

- [x] 3.1 Route accepted D3D11 shared textures through the unified compositor while preserving keyed-mutex ownership and zero-copy presentation.
- [x] 3.2 Route BGRA software fallback frames through the same layout and visible-surface owner used by hardware presentation.
- [x] 3.3 Implement atomic hardware-to-software fallback and software-to-hardware recovery without exposing an uninitialized or black intermediate surface.
- [x] 3.4 Preserve existing H265 decoder, encoded bitrate, quality policy, remote input, and viewer lifecycle contracts.

## 4. Stable interactive resize

- [x] 4.1 Enter InteractiveResize by retaining the last valid source frame and local UI layer cache without hiding the visible surface.
- [x] 4.2 Apply intermediate resize geometries through a stable output transform or viewport without destroying/recreating multiple visible surfaces.
- [x] 4.3 Defer destructive SwapChain/resource resizing until FinalizeResize and present the retained latest frame at the final geometry.
- [x] 4.4 Keep Qt geometry, native output geometry, DPI conversion, title-bar hit testing, and remote input mapping derived from one committed snapshot.
- [x] 4.5 Restore normal presentation state only after final geometry, mask, input regions, and performance-layer placement are committed.

## 5. Input, title bar, and overlay migration

- [x] 5.1 Migrate title-bar rendering and button state from the independent native title-bar surface into the unified compositor.
- [x] 5.2 Migrate performance text and status/update overlays into cached compositor layers.
- [x] 5.3 Preserve title-bar hit testing, tooltips, double-click behavior, window move/resize, snapping, full-screen behavior, and remote input exclusion.
- [ ] 5.4 Remove runtime dependencies on the old independent title-bar and performance overlay HWNDs after the new path passes integration checks.

## 6. Visible-result diagnostics

- [x] 6.1 Add low-cost compositor state, frame identifier, geometry, source/output size, Present result, and fallback counters to normal diagnostics.
- [x] 6.2 Add an opt-in resize pixel-sampling or short PNG capture probe that records actual visible-region black-pixel statistics and sample timestamps.
- [x] 6.3 Correlate visible-result samples with resize state transitions and frame commits so a visual blank frame can be distinguished from ordinary WM_SIZE activity.

## 7. Verification and migration completion

- [ ] 7.1 Verify normal 60 FPS hardware presentation at high quality without title-bar, content, or overlay flicker.
- [ ] 7.2 Verify slow and fast edge resizing in all directions, including window growth and shrink, without blank frames or stale input geometry.
- [ ] 7.3 Verify multiple remote windows, active-window policy, DPI changes, maximize/restore, full-screen, reconnect, and device-loss recovery.
- [ ] 7.4 Compare the new compositor against the baseline using the visible-result diagnostics and document any remaining driver-specific limitations.
- [ ] 7.5 Enable the new compositor by default only after all acceptance checks pass; retain a rollback switch for one release cycle before deleting the legacy path.
