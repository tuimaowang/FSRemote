## ADDED Requirements

### Requirement: Persistent global quality defaults
The main-window settings UI SHALL provide a persistent controller-wide default quality mode and SHALL explain the fixed visible presets plus the common minimized profile without exposing the retired strategy parameters.

#### Scenario: Change global settings
- **WHEN** the user changes a global remote-quality setting
- **THEN** the selected default mode SHALL be saved and immediately applied to every open window that follows global settings

#### Scenario: Open a new window
- **WHEN** a remote window is created
- **THEN** it SHALL restore the last mode saved for that device, or begin in automatic when the device has no saved mode

### Requirement: Per-device remembered window mode
Each remote window SHALL provide a title-bar quality menu with user-facing custom, automatic, high-quality, balanced, and smooth options. The custom option SHALL retain the legacy follow-global behavior, and every title-bar selection SHALL be saved automatically for that target device.

#### Scenario: Override one window
- **WHEN** the user selects high-quality in one remote window
- **THEN** only that window SHALL use the override and other windows SHALL continue using their own effective policies

#### Scenario: Global setting changes after override
- **WHEN** global settings change while one window has a local override
- **THEN** custom/follow-global windows SHALL update and the overridden window SHALL retain its current local policy

#### Scenario: Reopen overridden device
- **WHEN** a locally overridden window is closed and later reopened
- **THEN** the new window SHALL restore that device's last selected mode

#### Scenario: First remote session for a device
- **WHEN** a device has no saved window-quality mode
- **THEN** its new remote window SHALL use automatic and SHALL not inherit another device's selection

### Requirement: Effective-quality feedback
The remote-window title bar and quality menu SHALL distinguish requested mode from the host-applied resolution, FPS, bitrate, degradation reason, minimized state, and unsupported-host state.

#### Scenario: Automatic degradation
- **WHEN** the controller lowers FPS or enters a bounded minimized/software-fallback profile to preserve stability
- **THEN** the window SHALL remain connected and SHALL show the current applied values and degradation reason

#### Scenario: High-quality indicator
- **WHEN** a window uses a high-quality local override
- **THEN** its title bar SHALL show a persistent high-quality indicator until the override is cleared or the window closes

### Requirement: Controller-only performance overlay
Each remote window SHALL render a fixed, mouse-transparent performance overlay locally at the bottom-right of the displayed remote image without including it in the remote stream.

#### Scenario: Remote video is visible
- **WHEN** a remote window is displaying video and is not minimized
- **THEN** the controller SHALL show the effective quality mode, actual displayed FPS, actual receive bitrate, current pressure state, average decode time, and dropped-frame ratio as separate lines and SHALL refresh them no more frequently than once per second

#### Scenario: Actual FPS remains below the selected preset
- **WHEN** receiver statistics are valid, no direct pipeline pressure is reported, and actual displayed FPS remains materially below the current requested FPS
- **THEN** the overlay SHALL report that the target is not reached instead of labeling the session healthy, without using that label to degrade a fixed preset

#### Scenario: Texture or software presentation
- **WHEN** the window switches between D3D11 shared-texture presentation and BGRA software presentation
- **THEN** the same local overlay SHALL remain above the displayed image at its fixed bottom-right position and SHALL NOT intercept remote input

#### Scenario: No visible remote video
- **WHEN** the window is minimized, waiting for its first video frame, closed, or covered by the remote-update wait state
- **THEN** the local performance overlay SHALL be hidden

#### Scenario: Periodic statistics refresh
- **WHEN** the one-second statistics sample changes text without changing the overlay position, visibility, or state color
- **THEN** the controller SHALL repaint only the changed text and SHALL NOT recreate, resize, show, or reorder the native overlay window, so the overlay remains visually stable above D3D11 video

#### Scenario: Overlay covers remote content
- **WHEN** the local performance overlay is visible above a valid remote frame
- **THEN** its background SHALL remain highly translucent so the last presented remote content stays clearly visible below it, while unchanged text, geometry, and native-window order SHALL remain cached to prevent flicker

#### Scenario: Numeric statistics change
- **WHEN** a displayed value changes between samples, such as from 59 FPS to 60 FPS
- **THEN** the overlay SHALL clear its previous transparent pixel surface and commit the complete new panel atomically, so old glyphs SHALL NOT remain underneath the new text and the remote video SHALL NOT be cleared

#### Scenario: Translucent overlay above D3D11 video
- **WHEN** Windows composes the local overlay above a native D3D11 presenter
- **THEN** the overlay SHALL use an owner-bound layered surface that preserves fully visible text independently from its highly translucent background, follows owner movement, and hides with the owner
