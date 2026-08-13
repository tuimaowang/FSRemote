## Why

The remote viewer forwards pointer movement but never receives the cursor shape selected by the controlled Windows desktop. As a result, resizing applications inside the remote image provides no horizontal, vertical, or diagonal resize feedback even though resizing itself works.

## What Changes

- Detect the controlled Windows desktop's current standard cursor shape while a remote control session is active, including a bounded window-edge hit-test fallback when the global cursor handle stays hidden, custom, or arrow-shaped.
- Send a versioned cursor-shape control message only when the shape changes.
- Map Windows default cursor types to equivalent Qt cursors in every remote desktop window, including horizontal, vertical, both diagonal resize directions, move, text, hand, busy, forbidden, crosshair, and arrow fallback.
- Preserve higher-priority local chrome cursors for the remote window's own resize border and title-bar controls.
- Preserve the existing blank cursor while relative/game mouse capture is active and restore the latest remote shape when returning to desktop mode.

## Capabilities

### New Capabilities

- `remote-cursor-shape-sync`: Propagation and display of standard Windows cursor shapes inside remote desktop images.

### Modified Capabilities

None.

## Impact

- Extends the native Host-to-Viewer control protocol and status code list without changing callback signatures.
- Adds lightweight Host cursor polling to the existing auxiliary state worker and broadcasts only changes.
- Updates `RemoteDesktopWindow` cursor precedence and both software/D3D presentation paths.
- Adds protocol and mapping regression coverage; no new dependency is required.
- Corrects virtual-desktop absolute mouse injection and protects concurrent control-channel access uncovered by real-device regression testing.
