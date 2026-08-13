## Context

`DeviceGrid` paints its device/group list and scrollbar manually. The wheel handler changes `m_deviceListScrollOffset`, while scrollbar track and thumb geometry currently exist only as local paint variables, so mouse events cannot reuse them reliably.

## Goals / Non-Goals

**Goals:**

- Make the visible device-list thumb draggable across the entire scroll range.
- Keep painted geometry, hit testing, and scroll-value mapping identical.
- Prevent scrollbar dragging from starting device/group drag candidates or inline row actions.
- Preserve wheel scrolling and all existing group/device interactions.

**Non-Goals:**

- Replacing the hand-painted list with `QScrollArea` or `QScrollBar`.
- Adding persistence for scroll position.
- Changing the separate Settings or script-output scrolling behavior.

## Decisions

1. Extract free geometry helpers for the device-list track, thumb, and thumb-top-to-scroll-offset mapping. Painting and mouse input will call the same helpers, preventing drift between the visual and interactive rectangles.

2. Store only active drag state and the pointer-to-thumb-top grab offset in `DeviceGrid`. Preserving the grab offset avoids making the thumb jump when it is pressed away from its top edge.

3. Handle thumb press before device/group drag-candidate detection. The visual thumb is narrow, so hit testing will use a modest horizontally expanded rectangle clipped to the device-list viewport while painting remains unchanged.

4. During movement, clamp the requested thumb top to the track travel and map it proportionally with 64-bit intermediate arithmetic. Reaching the last thumb position therefore produces exactly `maxDeviceListScrollOffset()`.

5. End the drag on left-button release and restore the cursor. If overflow disappears during a drag, cancel safely and clamp the current list offset.

## Risks / Trade-offs

- [The list height changes while dragging] → Recompute track, thumb travel, and maximum offset on every move.
- [The five-pixel thumb is hard to hit] → Expand only its input hit box without changing the painted width.
- [Drag conflicts with row/group reordering] → Consume press, move, and release while the scrollbar drag flag is active.
- [Integer rounding prevents reaching the exact end] → Clamp endpoints explicitly through proportional integer mapping using the current maximum offset.
