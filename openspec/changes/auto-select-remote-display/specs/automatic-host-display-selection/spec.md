## ADDED Requirements

### Requirement: Prefer the existing Windows primary display
The Host SHALL prefer an active, attached, non-Parsec Windows primary display with a valid current mode before creating a Parsec virtual display.

#### Scenario: Existing primary display is available
- **WHEN** the first authenticated media subscriber arrives and an eligible Windows primary display exists
- **THEN** the Host attempts to capture that display without creating a Parsec virtual display

#### Scenario: Only ineligible displays are present
- **WHEN** the first authenticated media subscriber arrives and all enumerated displays are inactive, non-primary, invalid, remote, mirrored, or Parsec displays
- **THEN** the Host treats the existing display topology as unavailable and proceeds to the virtual-display fallback

### Requirement: Use ordered capture fallback for a real primary display
The Host SHALL attempt native DXGI capture for the selected real primary display first and SHALL attempt an exact-target DesktopCapturer fallback before creating a virtual display.

#### Scenario: Native capture succeeds
- **WHEN** native DXGI capture initializes for the selected real primary display
- **THEN** the Host publishes the native video source and does not create a virtual display

#### Scenario: Compatible capture succeeds
- **WHEN** native DXGI capture fails but DesktopCapturer can select the same real primary display
- **THEN** the Host publishes the compatible video source and does not create a virtual display

#### Scenario: Real display capture is unusable
- **WHEN** neither native DXGI nor DesktopCapturer can initialize and select the chosen real primary display
- **THEN** the Host stops the failed real-display resources and creates a Parsec virtual display

### Requirement: Preserve virtual-display fallback behavior
The Host SHALL create a Parsec virtual display when no eligible real primary display can be captured, SHALL prefer native DXGI capture for that display, and SHALL retain DesktopCapturer as its compatibility fallback.

#### Scenario: Headless host starts a session
- **WHEN** no eligible real primary display exists when the first media subscriber arrives
- **THEN** the Host creates one shared Parsec virtual display and starts capture from it

#### Scenario: Virtual native capture fails
- **WHEN** the Parsec display is ready but native DXGI capture cannot initialize for it
- **THEN** the Host attempts DesktopCapturer against the Parsec display identity

### Requirement: Keep one stable shared capture target per active subscriber group
The Host SHALL select a capture target only when the first subscriber starts the idle media pipeline and SHALL retain that target until the final subscriber releases it.

#### Scenario: Additional subscriber joins
- **WHEN** a second authenticated subscriber joins while the media pipeline is active
- **THEN** the Host reuses the existing video source and does not re-evaluate or recreate the display target

#### Scenario: A new subscriber group starts later
- **WHEN** all subscribers have disconnected and a later first subscriber arrives
- **THEN** the Host re-enumerates the current display topology and makes a new selection

### Requirement: Keep capture identity and input semantics consistent
The Host SHALL capture only the current Windows primary display when using an existing display and SHALL report refresh rate and diagnostics from the final selected device.

#### Scenario: Real primary display is selected
- **WHEN** the Host captures an existing display
- **THEN** the captured display remains the Windows primary display so existing absolute mouse input maps to the visible image

#### Scenario: Refresh rate is queried
- **WHEN** the media pipeline reports its source refresh rate
- **THEN** it reads the final selected device regardless of whether that device is real or virtual

### Requirement: Release only the owned virtual display
The Host SHALL remove only the Parsec display index created and owned by its current shared VDD session.

#### Scenario: Other Parsec displays already exist
- **WHEN** FSRemote needs to create a virtual display while other Parsec display indexes are present
- **THEN** FSRemote leaves those existing indexes unchanged and removes only its own index when the session stops
