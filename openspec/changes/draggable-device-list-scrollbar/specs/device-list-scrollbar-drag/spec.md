## ADDED Requirements

### Requirement: Device-list scrollbar thumb can be dragged
When the expanded device/group list overflows its viewport, the application SHALL allow the user to press and drag the visible scrollbar thumb with the left mouse button.

#### Scenario: Start dragging the thumb
- **WHEN** the list has a positive maximum scroll offset and the user presses the scrollbar thumb
- **THEN** the application enters scrollbar dragging state
- **AND** it consumes the gesture instead of starting device, group, or window dragging

#### Scenario: Preserve the pointer grab position
- **WHEN** the user presses below the top edge of the thumb and begins dragging
- **THEN** the thumb keeps that relative pointer position without jumping its top edge to the pointer

### Requirement: Thumb travel maps to the complete list range
The application SHALL map the draggable thumb travel proportionally to the current device-list scroll range and MUST clamp the result within that range.

#### Scenario: Drag to the bottom
- **WHEN** the user drags the thumb to or below the final track position
- **THEN** the device-list scroll offset equals its maximum value
- **AND** the last list content is immediately reachable

#### Scenario: Drag above the track
- **WHEN** the user drags the thumb above its first track position
- **THEN** the device-list scroll offset equals zero

#### Scenario: Drag within the track
- **WHEN** the thumb is dragged to an intermediate track position
- **THEN** the device-list scroll offset changes proportionally and remains clamped

### Requirement: Scrollbar drag lifecycle is isolated and safe
The application SHALL stop scrollbar dragging on left-button release, SHALL retain wheel scrolling behavior, and SHALL safely cancel if the list no longer overflows.

#### Scenario: Release the mouse button
- **WHEN** the user releases the left mouse button after dragging the thumb
- **THEN** the application exits scrollbar dragging state and restores normal cursor handling

#### Scenario: Overflow disappears during dragging
- **WHEN** layout or group changes reduce the maximum device-list scroll offset to zero during a drag
- **THEN** the application exits dragging state and clamps the scroll offset without division by zero

#### Scenario: Use the mouse wheel
- **WHEN** the pointer is over the device-list viewport and the user rotates the mouse wheel
- **THEN** the existing wheel-based scrolling behavior remains available
