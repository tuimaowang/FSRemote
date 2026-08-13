## 1. Clean Migration Baseline

- [x] 1.1 Remove the unsuccessful synchronous title-bar repaint and native `QLabel` snapshot experiments while preserving the current D3D11 content and black-bar behavior.
- [x] 1.2 Isolate the legacy parent-painted title-bar block behind one clearly named, default-off compile-time migration guard so it remains available for rollback without interleaving with the new path.
- [x] 1.3 Capture the existing title-bar state inputs, layout rectangles, visible-control priority, and interaction routes that the replacement must preserve.

## 2. Extract Retained Title-Bar Rendering

- [x] 2.1 Add an immutable remote title-bar visual-state snapshot containing identity, connection, quality, input-sync, clipboard, update, mouse-backend, hover, pressed, width, and DPI inputs.
- [x] 2.2 Extract the existing title-bar drawing into a renderer that produces an opaque premultiplied image while continuing to use `RemoteTitleBarLayoutSnapshot` for visibility and geometry.
- [x] 2.3 Add focused renderer checks for normal width, narrow width, minimum width, right-aligned controls, hover and pressed states, and DPI-scaled output.

## 3. Implement the Native Retained Surface

- [x] 3.1 Add `NativeRemoteTitleBarSurface` with a dedicated child HWND, top-down 32-bit DIB section, retained memory DC, and deterministic Win32 resource cleanup.
- [x] 3.2 Implement atomic image commit and `WM_PAINT` blitting so the last complete title-bar frame remains available until a complete replacement is ready.
- [x] 3.3 Handle `WM_ERASEBKGND`, background fill for newly exposed capacity, child-window resizing without per-mouse-event DIB reallocation, and `HTTRANSPARENT` mouse pass-through.
- [x] 3.4 Recreate physical-pixel buffer capacity safely when device-pixel ratio or monitor requirements change.

## 4. Integrate Remote Window Surface Ownership

- [x] 4.1 Create, position, show, hide, and destroy the native title-bar surface from `RemoteDesktopWindow`, keeping it above the D3D content child and restricted to the title-bar rectangle.
- [x] 4.2 Route title-bar state invalidations to the retained renderer and commit only complete images; remote video frames must not trigger title-bar regeneration unless displayed state changes.
- [x] 4.3 Stop the parent `paintEvent` from painting visible title-bar pixels while retaining connection, update, and CPU fallback content below the title-bar boundary.
- [x] 4.4 Preserve parent-window hit testing and actions for resize borders, blank-area drag, context menu, double-click maximize or restore, minimize, close, clipboard, input sync, quality, update, and mouse-backend controls.
- [x] 4.5 Hide the title surface and expand content in full-screen mode, then regenerate and restore the surface before normal-window interaction resumes.

## 5. Resize and Lifecycle Validation

- [x] 5.1 Perform source-level consistency checks for all eight resize directions, ensuring temporary geometry never removes the retained title-bar frame even when D3D11 presentation is active.
- [x] 5.2 Verify state and layout behavior for normal, maximized, tiled, restored, minimum-width, multi-window snap, and saved-geometry paths.
- [x] 5.3 Verify mixed-DPI monitor transitions, title text and icon alignment, logical hit rectangles, and physical retained-buffer dimensions.
- [x] 5.4 Verify D3D11 shared-texture presentation, black bars, software-frame fallback, connecting state, update overlay, performance overlay, device-loss recovery, and close-during-resize behavior remain compatible.
- [x] 5.5 Hand off the resulting source for the user-owned build and hardware runtime matrix, including repeated bottom, top, left, right, and corner resize capture tests.

## 6. Remove the Legacy Path

- [ ] 6.1 After user hardware validation confirms the title bar never reveals the desktop, delete the disabled parent-painted title-bar block and migration flag.
- [ ] 6.2 Remove obsolete background-erasure, repaint, and snapshot workaround code that is no longer required by either the retained title surface or the D3D content surface.
- [ ] 6.3 Confirm repository searches show only one production title-bar renderer and one native title-bar surface ownership path.
