## Context

`RemoteDesktopWindow` uses a custom 28-pixel title bar. It currently defines, paints, excludes from blank-area hit testing, and handles three window buttons: minimize, maximize, and close. A separate double-click handler already toggles maximized/normal state through `toggleMaximizedState()`.

## Goals / Non-Goals

**Goals:**

- Remove every visible and interactive maximize-button path.
- Preserve title-bar double-click maximize/restore.
- Reflow the remaining right-side controls into a compact, predictable chain without gaps or overlap.
- Preserve full-screen, resize margins, drag handling, tooltips, and remote input exclusion.

**Non-Goals:**

- Removing the maximized window state itself.
- Removing maximize/restore through title-bar double-click.
- Changing full-screen shortcuts, snapping, geometry persistence, or the native window flags.
- Deleting the shared SVG asset from disk when it is no longer referenced by this window.

## Decisions

1. Delete `maximizeRect()`, its paint call, its blank-area exclusion, and its mouse-release action branch. Keep `toggleMaximizedState()` because double-click remains the single maximize/restore entry point.

2. Keep the close rectangle anchored at the current right resize margin. Move the 36-pixel minimize rectangle into the former maximize slot immediately left of close. This avoids changing close behavior or the right-edge resize safety gap.

3. Derive the 28-pixel clipboard rectangle from the new minimize rectangle with a four-pixel gap. Input sync already derives from clipboard with the same gap, while quality and update derive from input sync, so the entire action chain reflows automatically and consistently.

4. Leave the maximize SVG resource entry untouched. It may be shared or useful for future UI, and retaining an unused asset has no runtime interaction cost.

## Risks / Trade-offs

- [A stale maximize hit area still blocks double-click or drag] → Remove all `maximizeRect()` references and verify by repository search.
- [Reflowed controls overlap at narrow widths] → Preserve existing chain derivation and spacing while only changing its right-hand anchor.
- [Double-click stops toggling state after helper removal] → Retain `toggleMaximizedState()` and test compilation plus direct code-path inspection.
- [Full-screen top pixels become local controls] → Continue gating all title-bar control handling and painting through the existing title-bar/full-screen rules.
