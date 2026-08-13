## Context

The viewer currently decodes H265 through FFmpeg D3D11VA and receives a `DecodedFrame` with an `ID3D11ShaderResourceView`. The code then calls `copy_srv_to_bgra`, maps a staging texture to CPU memory, sends BGRA through `FsRemoteFrameCallback`, copies it into `QImage`, and paints it with `QPainter`.

Remote-control sessions often update the full frame continuously, so dirty-rectangle detection is not the primary optimization. The higher-impact change is to avoid the GPU-to-CPU readback and QWidget raster paint path when the decoded D3D11 texture is already available.

## Goals / Non-Goals

**Goals:**
- Present decoded remote desktop frames from D3D11 textures on the viewer when possible.
- Keep current BGRA frame callback and QPainter rendering as a fallback.
- Preserve existing input forwarding, connection status, close behavior, and stream lifecycle.
- Avoid changing Host-side WebRTC capture or encoder behavior.

**Non-Goals:**
- Do not add dirty-rectangle/tile detection as the first optimization.
- Do not replace WebRTC transport, signaling, or codec negotiation.
- Do not remove existing BGRA diagnostics and fallback rendering.
- Do not implement multi-monitor virtual-screen routing in this change.

## Decisions

1. **Add a texture callback beside the existing BGRA callback.**
   - Rationale: The decoder already has `DecodedFrame.srv`. A new callback lets the UI consume that GPU resource before CPU readback.
   - Alternative considered: Replace the BGRA callback entirely. Rejected because fallback is valuable for unsupported GPUs, device mismatch, and early rollout.

2. **Expose a D3D11 shared texture handle to the Qt process instead of passing raw decoder SRV ownership directly.**
   - Rationale: The decoder runs in `fsremote_stream.dll`; Qt UI code lives in the host process but should not depend on the decoder's private device/context lifetime. A shared handle allows the UI renderer to open the texture on its own D3D11 device.
   - Alternative considered: Pass `ID3D11ShaderResourceView*` directly. Rejected because device/thread/lifetime coupling is harder to make safe across the DLL/UI boundary.

3. **Use a dedicated D3D11 child-window presenter for the remote image region.**
   - Rationale: It avoids relying on Qt's raster `QPainter::drawImage` for the video surface, while the existing frameless Qt window can keep titlebar chrome and input handling.
   - Alternative considered: QOpenGLWidget texture upload. Rejected as the main path because it still uploads BGRA from CPU every frame.

4. **Keep the Qt `RemoteDesktopWindow` as the owner of connection, input, overlay, and fallback state.**
   - Rationale: The current UI logic is already wired to the stream lifecycle. The presenter should be a narrow rendering helper, not a new window controller.

5. **Throttle presenter updates to latest-frame semantics.**
   - Rationale: The current UI intentionally keeps only the newest frame. The GPU presenter should keep this low-latency behavior and drop stale frames if rendering lags.

## Risks / Trade-offs

- **D3D11 shared texture support may vary by device/driver** -> Keep BGRA fallback and allow the decoder to call both callbacks when needed.
- **Decoder and presenter devices may not be directly compatible** -> Use shared handle open on the presenter's device and recreate resources on size/device changes.
- **Window layering with a child presenter can conflict with Qt painting** -> Restrict the child presenter to the remote image rectangle and keep Qt chrome above/outside it where practical; fall back to QPainter if creation fails.
- **Shared handle lifetime is delicate** -> Duplicate/open the shared handle on the UI side and close handles deterministically after the presenter owns the resource.
- **Stats overlay currently drawn by QPainter over the image** -> Keep overlay in Qt fallback initially, and favor fast video present over overlay richness for the GPU path.

## Migration Plan

1. Add native texture callback structs and loader support without changing current behavior.
2. Add decoder-side shared texture export and callback emission; leave BGRA callback intact.
3. Add a D3D11 presenter helper owned by `RemoteDesktopWindow`.
4. Use the presenter when texture frames arrive; otherwise continue with existing `QImage` fallback.
5. Validate build and run-time fallback by ensuring a viewer can still receive BGRA frames if presenter creation or shared handle opening fails.

## Open Questions

- Whether the current FFmpeg D3D11VA output texture can always be opened with the desired share flags, or whether the existing post-processor output texture must be recreated with shared flags.
- Whether the stats overlay should be redrawn on top of the D3D child presenter in a later change.
