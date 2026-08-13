## 1. Realtime Protocol

- [x] 1.1 Add update metadata structures to local, snapshot, and reduced realtime state models.
- [x] 1.2 Encode, validate, decode, reduce, expire, and compare optional update metadata in protocol-version-1 snapshots.

## 2. Local Update Publication

- [x] 2.1 Expose the confirmed shared release version and change the periodic shared-version check to a ten-minute fallback.
- [x] 2.2 Publish update metadata from `main.cpp` and trigger immediate realtime snapshots after confirmed update-state changes and successful publication.
- [x] 2.3 Treat higher peer-known release versions as hints that schedule authoritative shared-storage checks.

## 3. Event-Driven Remote Buttons

- [x] 3.1 Cache target realtime update metadata and derive remote update-button visibility from the controller-confirmed release.
- [x] 3.2 Remove the ten-second remote-window `update_status` timer and automatic open-window queries while retaining explicit transaction polling and manual refresh.
- [x] 3.3 Clear obsolete stored preparation failures when the target no longer requires an update or repair.
- [x] 3.4 Show confirmed controller updates without waiting for target acknowledgement and carry the expected version through the explicit update request.

## 4. Verification

- [x] 4.1 Review protocol compatibility, update-state transitions, and all removed polling references with static repository checks.
