## ADDED Requirements

### Requirement: Host publishes standard Windows cursor changes
The controlled Host SHALL classify the current Windows cursor into a versioned standard shape and SHALL notify each active authorized Viewer when that shape changes.

#### Scenario: Pointer enters a horizontal resize edge
- **WHEN** Windows selects its horizontal resize cursor on the controlled desktop
- **THEN** the Host sends the horizontal-resize shape to active authorized Viewers

#### Scenario: Pointer enters a vertical resize edge
- **WHEN** Windows selects its vertical resize cursor on the controlled desktop
- **THEN** the Host sends the vertical-resize shape to active authorized Viewers

#### Scenario: Pointer enters either diagonal resize corner
- **WHEN** Windows selects either standard diagonal resize cursor on the controlled desktop
- **THEN** the Host sends the corresponding diagonal direction without interchanging the two diagonals

#### Scenario: Cursor remains unchanged
- **WHEN** consecutive Host samples classify to the same shape
- **THEN** the Host sends no duplicate cursor-shape message

#### Scenario: Cursor handle is hidden, custom, or remains arrow on a resize border
- **WHEN** the top-level window under the pointer reports a standard resize edge through `WM_NCHITTEST` but `GetCursorInfo` does not expose the matching resize handle
- **THEN** the Host publishes the resize shape derived from the bounded hit-test result

#### Scenario: No Viewer subscribes to cursor state
- **WHEN** the Host has no active authorized Viewer cursor subscriptions
- **THEN** the Host performs no cross-process window hit-test work

### Requirement: Cursor protocol is bounded and forward-compatible
The cursor-shape control protocol SHALL accept only its versioned prefix and known shape tokens and SHALL reject malformed or unknown values without affecting video, input, or connection state.

#### Scenario: Valid cursor shape round trip
- **WHEN** a supported cursor enum is serialized and parsed
- **THEN** the parser returns the same cursor enum

#### Scenario: Unknown cursor token arrives
- **WHEN** a Viewer receives a cursor message with an unsupported token or incorrect version prefix
- **THEN** the message is ignored and the previously displayed cursor remains active

### Requirement: Viewer displays the remote cursor within the streamed image
The remote desktop window SHALL map received standard cursor shapes to equivalent Qt cursors on both software and D3D presentation paths while the pointer is inside the remote image.

#### Scenario: User resizes an application inside the remote desktop
- **WHEN** the pointer crosses horizontal, vertical, or diagonal borders of a remote application window
- **THEN** the local pointer changes to the corresponding Windows-style resize cursor

#### Scenario: Pointer leaves the remote image for local chrome
- **WHEN** the pointer moves from the remote image onto the remote window's local border or title-bar controls
- **THEN** the local chrome cursor takes priority over the cached remote shape

#### Scenario: Viewer enters relative mouse mode
- **WHEN** the Host switches the Viewer into relative/game mouse mode
- **THEN** the Viewer hides the pointer regardless of subsequent remote cursor-shape notifications

#### Scenario: Viewer returns to desktop mouse mode
- **WHEN** relative/game mouse mode ends
- **THEN** the Viewer immediately re-evaluates pointer position and restores the latest remote shape inside the image

### Requirement: Missing cursor support degrades safely
The Viewer SHALL retain arrow behavior when the Host does not publish cursor-shape messages or when a shape cannot be classified.

#### Scenario: Viewer connects to an older Host
- **WHEN** video and input connect successfully but no cursor-shape status is received
- **THEN** remote interaction continues with the normal arrow cursor

#### Scenario: Viewer starts a replacement session
- **WHEN** a remote desktop window reconnects or replaces its current Viewer session
- **THEN** the cached remote cursor resets to arrow until the new Host publishes a valid shape
