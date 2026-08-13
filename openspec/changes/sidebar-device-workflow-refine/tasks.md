## 1. Sidebar Layout

- [x] 1.1 Move the device-list scrollbar into the gutter beside row content and keep the expanded blank area aligned to the settings divider.
- [x] 1.2 Shrink the settings entry footprint, move Settings to an icon-only titlebar button, and keep hit testing aligned.
- [x] 1.3 Move the sidebar collapse button below the status capsule area and remove the bottom device capsule/name rendering.

## 2. Row Rendering

- [x] 2.1 Keep occupied devices using the online-colored left accent.
- [x] 2.2 Constrain group-name display to a ten-character visual width and align the group rename editor to the same width.
- [x] 2.3 Sort root devices and each group's devices by display name leading character.

## 3. Rename and Launch Interactions

- [x] 3.1 Convert the titlebar wordmark into a launch button for selected devices, including multi-selection and offline wake handling.
- [x] 3.2 Change device double-click to start inline rename instead of opening a remote desktop window.
- [x] 3.3 Reuse group-style inline rename flow for devices and keep selection/hit testing stable.

## 4. Group Reordering

- [x] 4.1 Add group drag candidate/active state and drag feedback.
- [x] 4.2 Reorder groups on drop while keeping grouped devices attached to the moved group.

## 5. Verification

- [x] 5.1 Run OpenSpec status/apply checks and perform a static code review of affected hit rectangles and paint rectangles.
