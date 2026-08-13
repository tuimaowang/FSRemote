# Current Fsremote Video Baseline

## Application path

```text
WebRTC/FFmpeg decoder
  -> DecodedTextureCallback in third_party/uu_stream_webrtc/src/uu_codec_factory.cpp
  -> FsRemoteTextureFrameCallback in include/FsRemoteStreamApi.h
  -> RemoteDesktopWindow::enqueueRemoteTextureFrame
  -> LatestTextureFrameSlot / Qt queued drain
  -> D3D11FramePresenter::presentSharedTexture
  -> OpenSharedResource / AcquireSync / VideoProcessorBlt / Present
```

## Current ownership and scheduling observations

- `RemoteDesktopWindow` owns `D3D11FramePresenter` as a QWidget and calls it from the Qt thread.
- `RemoteDesktopWindow` uses a 16 ms Qt timer and a queued drain for software frames and texture descriptors.
- The current texture slot preserves an older pending descriptor and rejects newer descriptors while pending; it is not latest-frame replacement semantics.
- `D3D11FramePresenter::blitAndPresent` creates an input view and output view for each presentation attempt.
- The C ABI texture callback returns an accepted/dropped/fallback integer, but it does not carry an explicit cross-thread frame lease.
- The current worktree contains many untracked user files; no cleanup or reset is permitted during this change.

## Final removal set

- `src/ui/D3D11FramePresenter.*`
- `src/ui/LatestTextureFrameSlot.h`
- the texture callback and Qt drain methods in `src/ui/RemoteDesktopWindow.*`
- the old `FsRemoteTextureFrameCallback` presentation contract after the native-frame adapter is verified
- obsolete presenter-specific synchronization and fallback glue

## Initial new primitives implemented

- `src/stream/RemoteVideoFrame.h`: lease-backed native frame envelope and replacement-capable `FrameInbox`.
- `src/stream/RemoteVideoDiagnostics.*`: asynchronous JSONL diagnostics with bounded queue, trace ring, rotation, and snapshots.
- `tests/remote_video_frame_inbox_tests.cpp` and `tests/remote_video_diagnostics_tests.cpp`.
