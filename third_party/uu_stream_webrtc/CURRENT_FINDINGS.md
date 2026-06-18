# Current Findings

This directory is the native WebRTC line for reproducing the verified UU/GameViewer video path.

## Verified Today

- The old malformed RTP problem was from the non-native RTP path, not from WebRTC itself.
- `uu_stream_webrtc` now uses native `PeerConnection`.
- Codec preferences are strict and pinned to the captured UU payload layout where capabilities exist:
  - H265 `96`
  - RTX for H265 `97 apt=96`
  - H264 `98`
  - RTX for H264 `99 apt=98`
  - RED `100`
  - RTX for RED `101 apt=100`
  - ULPFEC `102`
  - FlexFEC `35`
- Runtime now refuses to start when the WebRTC factory cannot provide the required UU capabilities.

## Smoke Test Result

With the current `C:\webrtc-checkout\src\out\uu_release` build:

- GN reports `rtc_use_h265=true`.
- WebRTC RTP/SDP code contains H265 support.
- The builtin video encoder/decoder factories still do not advertise H265.
- Host sender capabilities also do not advertise FlexFEC.

Therefore the builtin factory path cannot produce the same UU profile. Continuing to tune bitrate, pacing, or window rendering on this path would be misleading.

## Required Next Step

Implement custom native WebRTC codec factories:

- `VideoEncoderFactory` advertising H265/H264 in the UU order.
- `VideoEncoder` backed by the existing NVENC HEVC encoder path.
- `VideoDecoderFactory` advertising H265/H264 in the UU order.
- `VideoDecoder` backed by D3D11/NVDEC or the existing D3D11VA HEVC decode path.

Only after those factories are installed in `NativeWebrtcRuntime` should quality and stall testing be considered comparable to UU.
