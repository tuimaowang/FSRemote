## 1. Display Selection Policy

- [x] 1.1 Add a pure host display candidate model and deterministic policy that selects only an eligible non-Parsec Windows primary display.
- [x] 1.2 Add policy regression tests for physical primary, Parsec, inactive, secondary, remote, mirrored, and invalid-mode candidates, and register the test target in CMake.

## 2. Shared Media Pipeline

- [x] 2.1 Enumerate the current Windows display topology at the first-subscriber boundary and resolve the selected primary display device name and monitor identity.
- [x] 2.2 Make DesktopCapturer require an exact source match for a selected physical display while preserving the existing virtual and generic compatibility fallbacks.
- [x] 2.3 Reorder production startup to try physical DXGI, physical DesktopCapturer, virtual DXGI, and virtual DesktopCapturer in sequence without changing subscriber sharing semantics.
- [x] 2.4 Store and clear the active capture mode and device identity so refresh-rate reporting and diagnostics describe the final real or virtual target.

## 3. Virtual Display Ownership

- [x] 3.1 Remove cross-process Parsec display cleanup from VDD acquisition and retain removal of only the display index owned by the current shared session.

## 4. Verification and Documentation

- [x] 4.1 Build the affected stream library and the host display policy, host media pipeline, and DXGI policy test targets.
- [x] 4.2 Run the affected tests and inspect the final diff for unintended Viewer, signaling, or input protocol changes.
- [x] 4.3 Append the required WJY change record with verified final line numbers, before/after snippets, implementation rationale, and actual verification results.
