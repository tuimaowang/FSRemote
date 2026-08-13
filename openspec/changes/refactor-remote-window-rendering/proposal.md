## Why

The remote desktop window currently mixes a Qt-painted parent surface, a native D3D11 child window, and separate overlay windows. During interactive resize these surfaces are committed independently, allowing the complete custom title bar to disappear temporarily even though the remote content and black bars remain stable; repeated repaint and snapshot patches have not removed the architectural race.

## What Changes

- Replace the parent-`QWidget` title-bar painting path with a dedicated retained native title-bar surface that owns its visible pixels independently of the parent Qt backing store.
- Keep the D3D11 remote-content presenter as a separate native surface while making the top-level remote window a geometry, state, and input coordinator rather than a visible mixed renderer.
- Extract title-bar rendering into a reusable renderer that produces an opaque cached image from the existing title-bar state and layout snapshot.
- Preserve the previous valid title-bar image throughout interactive resize and regenerate it only when visible state or final geometry requires a new frame.
- Retain the legacy parent-painted title-bar implementation temporarily as disabled reference code during migration, then remove it after the native surface passes resize, DPI, full-screen, input, and fallback verification.
- Remove the experimental synchronous repaint and Qt `QLabel` snapshot workarounds once the retained native title-bar surface is active.
- Preserve existing title-bar appearance, controls, tooltips, hit testing, double-click behavior, remote input exclusion, snapping, full-screen behavior, D3D11 presentation, and software-frame fallback.

## Capabilities

### New Capabilities

- `remote-window-render-surface`: Defines stable native title-bar and remote-content surface ownership, retained title-bar presentation during resize, and migration compatibility for existing remote-window interactions.

### Modified Capabilities


## Impact

- Affected UI code: `src/ui/RemoteDesktopWindow.*`, `src/ui/D3D11FramePresenter.*`, `src/ui/RemoteTitleBarLayout.h`, and new native title-bar renderer/surface files.
- The remote-window top-level HWND, child-window z-order, DPI handling, resize lifecycle, and paint ownership will change internally.
- Viewer protocol, Host behavior, stream APIs, remote input protocol, device persistence, quality policy, and update workflow remain unchanged.
- The migration must avoid permanently keeping large commented duplicate implementations; legacy code is retained only as a temporary, clearly bounded rollback reference until verification is complete.
