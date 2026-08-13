## 1. Shared scrollbar geometry

- [x] 1.1 Extract reusable device-list scrollbar track and thumb geometry helpers and update painting to use them.
- [x] 1.2 Add a clamped proportional mapping from dragged thumb position to the complete list scroll range.

## 2. Mouse interaction

- [x] 2.1 Add scrollbar drag state and detect left-button presses using an accessible thumb hit area before row drag handling.
- [x] 2.2 Update the list offset continuously during dragging while preserving the pointer grab offset and handling lost overflow safely.
- [x] 2.3 End dragging on release, keep cursor feedback correct, and leave wheel scrolling and row/group interactions unchanged.

## 3. Verification

- [x] 3.1 Build FSRemote, run relevant automated regression tests, and check the diff for formatting errors.
- [x] 3.2 Validate the OpenSpec change and confirm no WJY change-log artifact was created.
