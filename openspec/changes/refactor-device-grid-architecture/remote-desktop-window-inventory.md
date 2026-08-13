## RemoteDesktopWindow Responsibility Inventory

This inventory records the behavior that must remain stable while `RemoteDesktopWindow` is decomposed.

### Widget-owned responsibilities

- Window geometry, minimum size, fullscreen state, resize edges, title-bar hit testing, painting, and local cursor feedback.
- Presentation of connection status, session duration, update overlay, remote-control ownership, and quality status.
- Composition of the D3D11 presenter and CPU-painted frame fallback.

### Existing focused collaborators

- `RemoteViewerLifecycleManager`: bounded viewer initialization, stop submission, application-shutdown admission, and join ordering.
- `D3D11FramePresenter`: shared-texture presentation, swap-chain resize, device-loss handling, and presentation reset.
- `LatestTextureFrameSlot`: one-slot latest-frame handoff and single-drain scheduling.
- `RemoteInputBroadcastCoordinator`: single-controller role, synchronized fan-out, coordinate mapping, and release safety.
- `RemoteQualityCoordinator`: global resource-pressure decisions, minimized behavior, hysteresis, and quality recovery.
- `RemoteCursorShape`: validated protocol-to-Qt cursor mapping.
- `RemoteControllerOverlay`: rendering of the lower-right controlled-by status UI.

### Remaining extraction candidates

- Connection/reconnect/update-wait state transitions that only transform state and schedule existing lifecycle operations.
- Clipboard payload normalization, local-change suppression, and polling decisions.
- Title-bar button layout, hit testing, labels, and context-menu action selection.
- Local input translation and remote event construction that do not require direct widget ownership.

### Protected behavior and focused coverage

- Latest frame replacement and drain ownership: `fsremote_latest_texture_frame_slot_tests`.
- Quality normalization and multi-window decisions: `fsremote_remote_quality_policy_tests` and `fsremote_remote_quality_coordinator_tests`.
- Input role, fan-out, coordinate mapping, stale generation, and release behavior: `fsremote_remote_input_broadcast_tests`.
- Cursor validation and mapping: `fsremote_remote_cursor_shape_tests`.
- Viewer/host protocol and frame pipeline behavior: `uu_session_protocol_tests`, `uu_viewer_quality_protocol_tests`, `uu_host_session_manager_tests`, and `uu_host_media_pipeline_tests`.

Clipboard polling, title-bar painting, D3D11 runtime presentation, CPU fallback activation, live resize synchronization, reconnect UI, and final widget shutdown still require bounded integration verification during tasks 8.2 through 8.4.
