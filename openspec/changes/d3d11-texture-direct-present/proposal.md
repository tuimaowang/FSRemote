## Why

Remote desktop viewing currently copies every decoded frame from GPU memory back to CPU BGRA, copies it again into a QImage, then repaints it with QWidget/QPainter. Full-screen remote-control sessions update nearly every frame, so dirty-region rendering does not remove the dominant cost: per-frame GPU readback, CPU copy, and raster drawing.

## What Changes

- Add a GPU-present path for viewer video frames using the decoder's D3D11 texture/SRV output.
- Keep the existing BGRA callback and QPainter display path as a fallback when GPU present is unavailable.
- Add a viewer-side texture callback and Qt rendering surface that can consume the latest decoded D3D11 frame without forcing CPU readback.
- Preserve existing remote input, connection status, bitrate reporting, and window chrome behavior.

## Capabilities

### New Capabilities
- `remote-desktop-gpu-present`: Viewer can present remote desktop frames from GPU textures with BGRA fallback.

### Modified Capabilities

## Impact

- Affected native stream API: `include/FsRemoteStreamApi.h`, `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp`, `native_webrtc_runtime.*`, and `uu_codec_factory.*`.
- Affected Qt UI: `src/stream/StreamRuntime.*`, `src/ui/RemoteDesktopWindow.*`, and a new D3D11-backed frame presenter widget or helper.
- Build impact: FSRemote will need Windows D3D11/DXGI linkage on the Qt application target.
- Runtime impact: Viewer should reduce CPU memory bandwidth and QPainter pressure during full-frame motion while keeping the current BGRA path for compatibility.
