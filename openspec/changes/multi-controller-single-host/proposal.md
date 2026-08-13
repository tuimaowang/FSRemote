## Why

FSRemote currently allows one controller to open remote sessions to multiple devices, but each controlled device accepts only one remote desktop session at a time. Supporting multiple trusted devices connecting to one controlled device is needed for shared observation, handoff, and resilient remote assistance without duplicating unsafe input or host capture state.

## What Changes

- Allow a controlled device to accept a bounded number of concurrent remote desktop sessions while keeping each signaling and WebRTC lifecycle isolated.
- Share host-level virtual display, desktop capture, audio capture, and WebRTC runtime resources across connected sessions instead of recreating them per client.
- Permit all admitted control-capable sessions to receive remote media and use keyboard and mouse control simultaneously by default.
- Add shared-control authorization state and per-session disconnect or administrative-revocation cleanup without interrupting other controllers.
- Serialize accepted input and release session-attributed keys and mouse buttons whenever that controller disconnects or loses permission.
- Add a versioned admission handshake with stable client identity, requested mode, capability negotiation, authorization proof, connection limits, and explicit rejection reasons.
- Extend device and remote-window status reporting with viewer count, control ownership, and local permission while retaining compatibility with existing online/busy consumers.
- Add configurable concurrent-session and total media-budget limits, with graceful rejection when host, network, or encoder capacity is exhausted.

## Capabilities

### New Capabilities

- `multi-client-remote-session`: Concurrent remote-session admission, isolated lifecycle, shared media delivery, capacity limits, and multi-client status behavior.
- `remote-control-ownership`: Shared control authorization, input gating, serialized multi-controller injection, and per-session cleanup.
- `remote-session-admission`: Versioned client identification, authorization, capability negotiation, compatibility handling, and explicit admission failures.

### Modified Capabilities

None. The repository has no main OpenSpec capabilities covering remote-session concurrency or ownership.

## Impact

- Native stream host and public wrapper: `third_party/uu_stream_webrtc/src/fsremote_stream_api.cpp`, `webrtc_session.*`, `native_webrtc_runtime.*`, `system_audio_stream.*`, signaling helpers, and `include/FsRemoteStreamApi.h`.
- Qt integration and status surfaces: `src/stream/StreamRuntime.*`, `src/ui/RemoteDesktopWindow.*`, `src/ui/DeviceGrid.*`, `src/system/DeviceStatusService.*`, and startup host ownership in `src/main.cpp`.
- Runtime behavior: persistent multi-client listeners, shared capture resources, multiple PeerConnections and NVENC senders, aggregate bitrate limits, authenticated session identity, and deterministic shutdown of all client workers.
- Compatibility: existing single-controller behavior remains valid; old status readers continue to recognize online/busy prefixes, while incompatible or unauthorized stream clients receive an explicit rejection instead of entering WebRTC negotiation.
