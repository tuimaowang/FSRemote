## ADDED Requirements

### Requirement: Remote title bar uses a retained native surface
The controller SHALL render every non-full-screen remote-window title bar through a dedicated retained native surface that does not depend on the top-level Qt backing store for its visible pixels.

#### Scenario: Remote window is shown normally
- **WHEN** a remote window is visible outside full-screen mode
- **THEN** the native title-bar surface displays the complete current title-bar background, identity, status, and visible controls
- **AND** the parent window does not paint a second visible title bar beneath or above it

### Requirement: Interactive resize preserves the last complete title-bar frame
The controller SHALL retain the last fully rendered title-bar image throughout interactive resizing and MUST NOT reveal the desktop, transparency, uninitialized pixels, or remote-content black bars in the title-bar region.

#### Scenario: User resizes from any edge or corner
- **WHEN** the user drags any of the four remote-window edges or four corners through temporary sizes
- **THEN** the title-bar region continuously displays either the last complete retained frame or a newer complete frame
- **AND** it never disappears while the D3D11 content surface remains visible

#### Scenario: UI rendering is delayed during resize
- **WHEN** D3D11 resize, frame presentation, or other UI-thread work delays generation of a new title-bar frame
- **THEN** the previous valid title-bar frame remains visible until the replacement is fully committed

### Requirement: Title-bar rendering and hit testing remain consistent
The native title-bar renderer SHALL use the same width-dependent layout snapshot as parent-window hit testing and SHALL preserve all existing visible-control priority and exclusion rules.

#### Scenario: Window becomes too narrow for every control
- **WHEN** a remote window is resized below the width required for all title-bar controls
- **THEN** only controls marked visible by the shared layout snapshot are rendered
- **AND** hidden controls cannot receive hover, press, release, tooltip, drag exclusion, or context-menu behavior

#### Scenario: User interacts with a visible title-bar control
- **WHEN** the user hovers, presses, or releases a visible title-bar control
- **THEN** the parent remote window receives the interaction through the existing behavior path
- **AND** the retained title-bar surface displays the resulting visual state

### Requirement: Native title-bar surface preserves window interactions
The new surface SHALL preserve remote-window resize borders, blank-area dragging, blank-area context menus, double-click maximize or restore, minimize, close, clipboard, input synchronization, quality, mouse-backend, and update actions.

#### Scenario: User drags a blank title-bar area
- **WHEN** the user presses and drags a title-bar location that is not occupied by a visible control
- **THEN** the remote window follows the existing drag, restore, snap-preview, and geometry-persistence behavior

#### Scenario: User double-clicks a blank title-bar area
- **WHEN** the user double-clicks a visible blank title-bar location
- **THEN** the window toggles between normal and maximized state using the existing behavior

### Requirement: Full-screen mode owns the complete client area
The controller SHALL hide the native title-bar surface in full-screen mode and SHALL restore a complete correctly scaled title bar before normal-window interaction resumes.

#### Scenario: User enters full-screen mode
- **WHEN** a remote window enters full-screen mode
- **THEN** the native title-bar surface is hidden
- **AND** the remote content surface occupies the full client area without a reserved title-bar strip

#### Scenario: User exits full-screen mode
- **WHEN** a remote window returns from full-screen to a normal, maximized, or tiled geometry
- **THEN** the title-bar surface is regenerated for the current width and device-pixel ratio
- **AND** the content surface is restored below the title bar

### Requirement: Title-bar surface handles DPI and lifecycle changes safely
The controller SHALL render the retained title-bar image at the current device-pixel ratio and SHALL release all native title-bar resources when the remote window is destroyed.

#### Scenario: Window moves between monitors with different DPI
- **WHEN** the remote window changes to a monitor with a different device-pixel ratio
- **THEN** the title-bar retained buffer and visible image are recreated at the new physical size
- **AND** title text, icons, controls, and hit rectangles remain aligned in logical coordinates

#### Scenario: Remote window closes during or after resize
- **WHEN** the remote window closes while native title-bar resources exist
- **THEN** its child HWND, DIB section, memory DC, and cached image state are released without affecting other remote windows

### Requirement: Migration removes obsolete flicker workarounds
After the native title-bar surface passes verification, the controller SHALL remove the legacy parent-painted title-bar path and the experimental repaint and Qt snapshot workarounds.

#### Scenario: Native title-bar verification is complete
- **WHEN** all required resize, DPI, full-screen, interaction, D3D11, and fallback checks pass
- **THEN** the legacy migration block is deleted
- **AND** only one production title-bar rendering implementation remains
