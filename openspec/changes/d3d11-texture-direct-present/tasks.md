## 1. Native Texture Frame Pipeline

- [x] 1.1 Add exported texture-frame callback types and viewer start API while keeping the existing BGRA API.
- [x] 1.2 Add runtime/decoder texture callback plumbing scoped to a single viewer instance.
- [x] 1.3 Publish shared D3D11 texture frames from the HEVC decoder when a decoded SRV is available.
- [x] 1.4 Keep BGRA callback fallback behavior when texture publishing is unavailable.

## 2. Qt D3D11 Presenter

- [x] 2.1 Add a D3D11 presenter helper/widget that owns a child HWND, D3D11 device, swap chain, and frame rendering resources.
- [x] 2.2 Implement shared-handle frame import, latest-frame retention, resize handling, and safe cleanup.
- [x] 2.3 Render the imported frame into the remote image rectangle without QImage/QPainter for video pixels.

## 3. RemoteDesktopWindow Integration

- [x] 3.1 Load the texture-capable viewer export through StreamRuntime with fallback to the existing viewer export.
- [x] 3.2 Route texture frames into the D3D11 presenter and BGRA frames into the existing QImage fallback.
- [x] 3.3 Keep input coordinate mapping, connection status, bitrate reporting, and window close behavior intact.
- [x] 3.4 Hide or disable the D3D11 presenter when fallback QPainter rendering is active or no remote frame is available.

## 4. Build and Validation

- [x] 4.1 Update CMake linkage/sources for the D3D11 presenter and new APIs.
- [x] 4.2 Validate the OpenSpec change and build the project.
- [x] 4.3 Record any remaining runtime limitations or follow-up work.
