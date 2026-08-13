## 1. Authoritative Shared Discovery

- [x] 1.1 Change the periodic shared-version check interval to 20 seconds while preserving the immediate startup check.
- [x] 1.2 Remove peer release-knowledge triggers so only shared-folder checks update the confirmed latest release.

## 2. Realtime Update Telemetry

- [x] 2.1 Remove `knownReleaseVersion` from locally published realtime update state while retaining installed version and runtime-repair state.
- [x] 2.2 Keep mixed-version snapshot decoding compatible and ensure remote update buttons continue comparing target telemetry with the controller-confirmed shared release.

## 3. Verification

- [x] 3.1 Run static reference checks confirming no normal-operation peer version trigger remains and the periodic interval is 20 seconds.
- [x] 3.2 Run the relevant configured tests or build-time checks available for update and realtime state behavior.
