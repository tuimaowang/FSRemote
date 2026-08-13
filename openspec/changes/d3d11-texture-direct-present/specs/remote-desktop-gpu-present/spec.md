## ADDED Requirements

### Requirement: Viewer can receive GPU texture frames
The viewer stream pipeline SHALL provide a decoded-frame path that delivers D3D11 texture metadata for remote desktop frames when GPU presentation resources are available.

#### Scenario: Decoder produces a presentable texture
- **WHEN** the H265 decoder completes a frame with a valid D3D11 shader resource view
- **THEN** the viewer pipeline SHALL publish width, height, frame id, and a D3D11 shared texture handle for that frame

#### Scenario: Texture path is unavailable
- **WHEN** the decoder cannot create or publish a shared D3D11 texture handle
- **THEN** the viewer pipeline SHALL continue to publish BGRA frames through the existing callback path

### Requirement: Viewer can present latest texture frame
The remote desktop window SHALL present the latest received GPU texture frame in the remote image area without using QImage/QPainter for the video pixels.

#### Scenario: Texture frame arrives
- **WHEN** the remote desktop window receives a valid GPU texture frame
- **THEN** it SHALL update the GPU presenter with the newest frame and request presentation of that frame

#### Scenario: Presenter cannot start
- **WHEN** the GPU presenter cannot create a D3D11 device, swap chain, or open a shared texture
- **THEN** the remote desktop window SHALL fall back to the existing BGRA/QPainter rendering path

### Requirement: Existing remote-control behavior remains intact
The GPU presentation path SHALL preserve the existing remote-control connection lifecycle and input forwarding behavior.

#### Scenario: User sends input while GPU present is active
- **WHEN** the user moves the mouse, clicks, scrolls, or presses a key over the remote image
- **THEN** the system SHALL send the same control messages as the existing BGRA rendering path

#### Scenario: Viewer closes while GPU present is active
- **WHEN** the remote desktop window is closed
- **THEN** the system SHALL stop the stream and release GPU presentation resources without blocking the Qt UI thread indefinitely
