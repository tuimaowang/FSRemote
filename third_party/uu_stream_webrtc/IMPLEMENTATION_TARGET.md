# UU-Quality WebRTC Target

This project is intentionally scoped to reproduce the verified UU/GameViewer video path, not to build another ad-hoc LAN streaming demo.

## Hard Requirements

- Native WebRTC media pipeline, not libdatachannel-only RTP and not custom raw UDP.
- DXGI/D3D11 capture.
- NVIDIA hardware encode first, HEVC payload `96` preferred, H264 fallback payload `98`.
- Hardware decode on receiver.
- WebRTC pacing and jitter/packet buffer behavior.
- RTCP feedback:
  - `goog-remb`
  - `transport-cc`
  - `ccm fir`
  - `nack`
  - `nack pli`
  - `rrtr`
- Repair payloads:
  - RTX for HEVC: payload `97`, `apt=96`
  - RTX for H264: payload `99`, `apt=98`
  - RED: payload `100`
  - RTX for RED: payload `101`, `apt=100`
  - ULPFEC: payload `102`
  - FlexFEC: payload `35`, `repair-window=10000000`
- RTP header extensions matching the captured UU SDP where supported by the WebRTC build:
  - `abs-send-time`
  - `transport-wide-cc-extensions-01`
  - `video-timing`
  - `sdes:mid`
  - `sdes:rtp-stream-id`
  - `sdes:repaired-rtp-stream-id`
  - video content/capture/feedback extensions where available.

## Non-Goals

- No fake SDP advertisement without matching sender/receiver implementation.
- No private fallback transport that bypasses WebRTC recovery.
- No software decode path for final quality testing.

## Current Dependency State

The repository currently has FFmpeg, NVENC headers, ImGui, and libdatachannel. It does not contain native WebRTC. Configure with:

```powershell
cmake -S uu_stream_webrtc -B uu_stream_webrtc/build -DWEBRTC_ROOT=C:\path\to\native-webrtc
cmake --build uu_stream_webrtc/build --config Release --parallel
```

If `WEBRTC_ROOT` is absent, configuration fails by design.

## Important Finding

The current native WebRTC checkout can compile H265 RTP/SDP support, but the builtin video codec factories do not advertise H265 on Windows in this build. The probe therefore rejects that path instead of silently falling back to H264. Matching UU requires custom WebRTC `VideoEncoderFactory`/`VideoDecoderFactory` backed by NVENC and D3D11/NVDEC-style decode.
