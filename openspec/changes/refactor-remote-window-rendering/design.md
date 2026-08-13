## Context

`RemoteDesktopWindow` currently owns a Qt raster backing store for its custom title bar and fallback content, while `D3D11FramePresenter` owns a native child HWND and SwapChain for the live remote image and black bars. A separate native tool window renders performance metrics. Interactive resize changes the top-level HWND and the D3D child geometry before the parent Qt backing store is guaranteed to flush, so the D3D content remains visible while the complete title-bar surface can temporarily reveal the desktop.

The attempted mitigations—native system resize, manual `setGeometry`, synchronous title-bar repaint, and a native `QLabel` snapshot—did not eliminate the failure because every title-bar variant still depended on a Qt widget backing store. The refactor must therefore change title-bar surface ownership rather than add another repaint request.

The project is Windows-specific in this area and already depends on Win32, DWM, D3D11, DXGI, native child windows, and custom frameless hit testing. Existing title-bar layout and hit-test behavior must remain compatible with `RemoteTitleBarLayoutSnapshot`, input synchronization, clipboard, quality, update, minimize, close, drag, double-click, and tooltip behavior.

## Goals / Non-Goals

**Goals:**

- Give the title bar a retained native surface whose last complete frame remains visible across every temporary resize geometry.
- Make the top-level remote window a coordinator for geometry, state, input, and child surfaces instead of the owner of title-bar pixels.
- Reuse the existing title-bar layout, visual state, icons, text, and hit-test rules.
- Keep live D3D11 content and black-bar presentation unchanged except for explicit sibling z-order coordination.
- Preserve normal, maximized, tiled, full-screen, multi-monitor, and mixed-DPI behavior.
- Retain a bounded rollback reference to the old parent-painted implementation during migration and remove it after verification.

**Non-Goals:**

- Rewriting viewer lifecycle, transport, decoding, remote input, clipboard, update, quality policy, snapping, or geometry persistence.
- Moving the entire application UI to Direct2D, QML, OpenGL, or a new framework.
- Merging video, title bar, update overlays, and performance metrics into a single D3D SwapChain in this change.
- Changing user-visible title-bar controls or interaction semantics.

## Decisions

### 1. Use a retained Win32 title-bar child surface instead of another Qt widget surface

Create `NativeRemoteTitleBarSurface` as a dedicated `WS_CHILD` HWND owned by the remote window. It maintains a top-down 32-bit DIB section and memory DC containing the last fully rendered title-bar frame. `WM_PAINT` performs a direct `BitBlt` from the retained DIB, and `WM_ERASEBKGND` reports handled because every visible pixel comes from the retained buffer.

The child returns `HTTRANSPARENT` from `WM_NCHITTEST`, allowing the existing parent-window Qt mouse path to continue handling resize borders, title-bar dragging, buttons, tooltips, context menus, and double-click maximize/restore.

Alternative considered: a native `QLabel` or child `QWidget` containing a snapshot. Rejected because it still owns a Qt backing store and reproduced the same surface-loss timing during resize.

Alternative considered: render the title bar into the D3D content SwapChain. Rejected for this change because it would also require redesigning software fallback, connecting states, update overlays, DPI text rasterization, and all content composition at once.

### 2. Extract title-bar drawing into an opaque image renderer

Move title-bar drawing out of `RemoteDesktopWindow::paintEvent` into a renderer that accepts an immutable state snapshot, logical size, and device-pixel ratio and returns an opaque premultiplied image. The renderer reuses `RemoteTitleBarLayoutSnapshot` so drawing, visibility, and parent hit testing continue to share one layout source.

The native surface updates its retained DIB only after a complete new image has been rendered. If rendering is delayed or fails, the previous valid DIB remains visible.

### 3. Keep the retained buffer allocated independently of temporary width changes

Allocate DIB capacity for the maximum relevant monitor width at the current device-pixel ratio, with a fixed title-bar height. Interactive resize changes only the child HWND's visible width. This avoids reallocating or discarding the retained title-bar pixels on every mouse movement.

When the current width changes, the renderer may synchronously produce a new frame because the title bar is small. Until that frame is committed, the previous valid frame remains in the DIB; newly exposed capacity is filled with the title-bar background color rather than transparency.

### 4. Make surface ownership explicit

The normal-window child order is:

```text
RemoteDesktopWindow top-level HWND
├─ NativeRemoteTitleBarSurface: y=0, native retained DIB
├─ D3D11FramePresenter: y=titleBarHeight, native SwapChain
└─ optional performance overlay: independent tool HWND
```

`RemoteDesktopWindow::paintEvent` no longer paints title-bar pixels. It may continue painting connection, update, or CPU fallback content below the title-bar boundary. `WS_CLIPCHILDREN` prevents parent painting from overwriting either visible native child surface.

### 5. Update title-bar content by state version, not frame cadence

Introduce a title-bar visual-state snapshot/version. Device identity, connection state, quality status, input-sync state, clipboard state, hover/pressed state, update availability, width, DPI, and full-screen transitions invalidate the title-bar renderer. Remote video frames and bitrate samples do not invalidate it unless a displayed title-bar state actually changes.

During resize, width/DPI invalidations can render new retained frames, but the last complete frame remains visible until replacement is ready.

### 6. Treat full-screen as an explicit surface state

Entering full-screen hides the native title-bar HWND and expands the content presenter to the complete client area. Leaving full-screen restores the title surface, regenerates it for the final DPI and width, and then restores the content rectangle below it.

### 7. Retain legacy code only as a temporary migration guard

The existing parent-painted title-bar block and experimental repaint/snapshot paths are disabled behind one clearly named compile-time migration block, default off. They are not interleaved with the new runtime path. After native-surface verification succeeds, the legacy block and temporary migration flag are removed in the final cleanup task.

This honors rollback needs without leaving permanent duplicated commented code throughout the 5,000-line window implementation.

## Risks / Trade-offs

- [Native child z-order can cover the D3D surface or local controls incorrectly] → Restrict the title HWND to the exact title-bar rectangle, use explicit sibling ordering only on show/state transitions, and test D3D recovery and full-screen transitions.
- [Returning `HTTRANSPARENT` may behave differently across child/parent message paths] → Add focused native hit-test verification for resize borders, draggable blank areas, and every title-bar control before removing the legacy path.
- [Mixed-DPI movement can blur or mis-size the retained image] → Render in physical pixels using the window's current device-pixel ratio and recreate DIB capacity after DPI changes outside active buffer use.
- [Large legacy title-bar code extraction can change visuals] → Capture representative reference images and keep layout calculation unchanged; move drawing mechanically before simplifying it.
- [The retained buffer can show stale button state during a busy UI interval] → Stale but complete UI is preferable to transparency; state changes queue a replacement frame and commit atomically when complete.
- [Keeping a temporary legacy block increases code size] → Keep it in one bounded region with a removal task and do not add new features to it.

## Migration Plan

1. Remove the unsuccessful `QLabel` snapshot and synchronous repaint experiments, restoring a clean single legacy title-bar path.
2. Extract the existing title-bar drawing into a pure renderer without changing visible output.
3. Add the retained native title-bar HWND and DIB implementation behind a compile-time migration flag.
4. Route normal-window title rendering to the native surface while retaining existing parent hit testing.
5. Disable the parent-painted title-bar block and verify all resize directions, DPI modes, state changes, and full-screen transitions.
6. Verify D3D11 texture presentation, black bars, CPU fallback, connecting/update states, overlays, snapping, and window closure.
7. Remove the legacy block and migration flag after user hardware verification confirms the title bar never reveals the desktop.

Rollback during steps 3–6 consists of switching the migration flag back to the legacy parent-painted path. No persisted data or protocol migration is required.

## Open Questions

- Whether the first implementation should render directly into the DIB with Win32/DirectWrite or continue using `QPainter` on a `QImage` and copy the finished pixels into the DIB. The latter minimizes visual regression and is the recommended starting point.
- Whether the separate performance overlay should later be folded into the content presenter; this is not required to solve title-bar resize flicker.
