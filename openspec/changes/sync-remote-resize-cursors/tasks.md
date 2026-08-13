## 1. Cursor control protocol

- [x] 1.1 Add a versioned standard cursor-shape enum, token mapping, serializer, and strict parser to the native control protocol.
- [x] 1.2 Add public cursor status code plumbing from ViewerInstance to the Qt status callback without changing the callback ABI.
- [x] 1.3 Extend native protocol tests for every supported shape plus malformed prefix, unknown token, and extra-field rejection.

## 2. Host cursor publication

- [x] 2.1 Classify Windows standard `HCURSOR` handles into protocol shapes with arrow fallback.
- [x] 2.2 Extend the shared Host state dispatcher to poll at low latency, deduplicate unchanged shapes, and copy callbacks before sending.
- [x] 2.3 Register and release per-session cursor callbacks with the same lifetime safety as mouse-mode and clipboard callbacks.

## 3. Remote window cursor display

- [x] 3.1 Cache incoming cursor status in `RemoteDesktopWindow` and map all supported protocol shapes to equivalent Qt cursors.
- [x] 3.2 Apply the cursor to both parent and D3D presenter while preserving local border/title controls and remote-image bounds precedence.
- [x] 3.3 Preserve blank relative-mode behavior and restore the cached desktop cursor when capture ends.

## 4. Verification

- [x] 4.1 Build the native stream DLL, main application, and affected tests, then run relevant regression tests.
- [x] 4.2 Check the diff, verify strict OpenSpec validation, and confirm no WJY change-log artifact was created.

## 5. Real-device regression fix

- [x] 5.1 Verify the tested Release process loaded the newly built executable and matching native stream DLL rather than stale local binaries.
- [x] 5.2 Add bounded standard resize-edge hit-test classification with no-subscriber short-circuiting and unit-testable hit-code mapping.
- [x] 5.3 Synchronize control DataChannel access and correct absolute mouse injection for the Windows virtual desktop.
- [x] 5.4 Reset remote cursor state for replacement Viewer sessions and make cursor mapping assertions effective in Release builds.
- [x] 5.5 Build affected targets, run regression tests, validate OpenSpec strictly, and confirm no WJY Markdown log was modified.
