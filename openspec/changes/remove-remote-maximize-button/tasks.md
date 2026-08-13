## 1. Remove maximize-button interaction

- [x] 1.1 Remove the maximize rectangle declaration/definition, paint call, blank-area exclusion, and mouse-release click branch.
- [x] 1.2 Keep `toggleMaximizedState()` exclusively for title-bar double-click and update comments to describe the remaining gesture.

## 2. Reflow title-bar controls

- [x] 2.1 Move minimize directly beside close while preserving the right resize margin.
- [x] 2.2 Derive clipboard from minimize with a four-pixel gap and verify input sync, quality, and update continue leftward without overlap.

## 3. Verification

- [x] 3.1 Build affected targets, run relevant automated regression tests, and confirm no production `maximizeRect` or maximize-icon paint references remain.
- [x] 3.2 Run diff and strict OpenSpec validation checks and confirm no WJY change-log artifact was created.
