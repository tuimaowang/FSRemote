## ADDED Requirements

### Requirement: Remote windows expose input synchronization state
The system SHALL show an input synchronization button in the title bar of every non-full-screen remote desktop window. The button SHALL represent exactly one of three states: synchronization off, synchronization master, or synchronization follower. The active master SHALL be visually distinct, and every state SHALL have a tooltip that describes the click action.

#### Scenario: No synchronization master exists
- **WHEN** multiple remote desktop windows are open and input synchronization is disabled
- **THEN** every window shows the synchronization-off state

#### Scenario: Synchronization is active
- **WHEN** one remote desktop window is the synchronization master
- **THEN** that window shows the master state and every other open remote desktop window shows the follower state

#### Scenario: Full-screen window hides local title controls
- **WHEN** a remote desktop window enters full-screen mode
- **THEN** its local synchronization button is not displayed while its synchronization role remains unchanged

### Requirement: User can select or clear the unique master
The system SHALL maintain at most one synchronization master across all remote desktop windows. Clicking an off-state button SHALL enable synchronization with that window as master. Clicking a follower button SHALL atomically transfer the master role to that window. Clicking the current master's button SHALL disable synchronization for all windows.

#### Scenario: Enable synchronization
- **WHEN** the user clicks the synchronization button of a window while synchronization is off
- **THEN** that window becomes the unique master and all other windows become followers

#### Scenario: Transfer synchronization master
- **WHEN** the user clicks the synchronization button of a follower window
- **THEN** held broadcast input is safely released and that window becomes the unique master without an interval containing two masters

#### Scenario: Disable synchronization from master
- **WHEN** the user clicks the current master's synchronization button
- **THEN** held broadcast input is safely released and all windows return to the synchronization-off state

### Requirement: Master input is broadcast to eligible followers
The system SHALL send keyboard and mouse input produced inside the master's remote image to the master's own remote session and to every eligible follower session. A follower is eligible only while its window is alive, its viewer session is connected, it accepts remote input, and it is not closing or performing a remote update. Input originating from a non-master window SHALL remain local to that window.

#### Scenario: Broadcast to all connected followers
- **WHEN** the user produces a keyboard or mouse event in the master's remote image and two followers are eligible
- **THEN** the same semantic event is sent once to the master session and once to each eligible follower session

#### Scenario: Ignore ineligible follower
- **WHEN** a master event occurs while one follower is disconnected, closing, or updating
- **THEN** the event is sent to the master and all other eligible followers without being sent to the ineligible follower

#### Scenario: Operate a follower directly
- **WHEN** the user produces input in a follower window without clicking its synchronization button
- **THEN** the input is sent only to that follower and the existing master remains unchanged

#### Scenario: Title-bar action is local only
- **WHEN** the user clicks or drags any local title-bar control or window border
- **THEN** the action is not serialized as remote input and is not broadcast

### Requirement: Mouse input is mapped consistently per target
The system SHALL represent absolute pointer positions in the normalized remote coordinate space and SHALL map them independently to every target session. Relative movement SHALL be scaled from the master's remote frame dimensions to each target's remote frame dimensions. Mouse button and wheel events SHALL preserve their logical button, direction, and normalized pointer location.

#### Scenario: Followers have different window sizes
- **WHEN** the master pointer is at a normalized location and followers render their remote images at different local sizes
- **THEN** every target receives the same normalized remote location independent of local window geometry and letterboxing

#### Scenario: Followers have different remote resolutions
- **WHEN** relative mouse movement is broadcast to followers whose remote frame dimensions differ from the master's
- **THEN** each follower receives a proportional relative delta based on its own remote frame dimensions

#### Scenario: Pointer is outside master image
- **WHEN** the pointer moves over the master's title bar, border, or letterbox area outside the remote image
- **THEN** no absolute remote pointer event is broadcast

### Requirement: Keyboard semantics are preserved
The system SHALL broadcast native virtual-key down and up transitions from the master, including transitions captured by the existing native keyboard forwarding path. Auto-repeat SHALL preserve the current single-window behavior, and local application shortcuts already consumed by the master window SHALL not be introduced into the broadcast path.

#### Scenario: Normal key transition
- **WHEN** the master receives a valid native key-down followed by key-up
- **THEN** every eligible target receives the corresponding ordered down and up transitions

#### Scenario: Local shortcut is consumed
- **WHEN** the master handles an application shortcut locally instead of forwarding it to its own remote session
- **THEN** that shortcut is not sent to any follower

### Requirement: Broadcast state is released safely
The system SHALL track keys and mouse buttons held through synchronization. Before disabling synchronization, transferring the master, removing a participating target, or losing the master, the system SHALL emit the required release transitions to every reachable session that may hold broadcast state and SHALL clear its local held-state record. Late events from a previous master generation SHALL be ignored.

#### Scenario: Switch master while key is held
- **WHEN** the user transfers the master while a broadcast key or mouse button is held
- **THEN** reachable participating sessions receive releases before input from the new master is accepted

#### Scenario: Master becomes unavailable
- **WHEN** the master closes, disconnects, starts a remote update, or otherwise becomes unable to accept input
- **THEN** synchronization is disabled, held broadcast state is released where possible, and all remaining windows show the off state

#### Scenario: Follower becomes unavailable
- **WHEN** a follower closes or becomes unable to accept input
- **THEN** that follower is removed from the eligible target set while synchronization continues for the master and remaining followers

#### Scenario: Late event from old master
- **WHEN** an input event queued before a master transfer is delivered after the transfer
- **THEN** the event is rejected by generation and is not sent to the new synchronization group

### Requirement: Membership follows remote window lifecycle
The system SHALL register both individually opened and tiled remote desktop windows with one shared synchronization coordinator. While synchronization is active, a newly registered window SHALL become a follower and SHALL begin receiving subsequent events once eligible; historical input events SHALL not be replayed.

#### Scenario: New window connects during synchronization
- **WHEN** a new remote desktop window becomes connected while another window is master
- **THEN** the new window displays follower state and receives only subsequent master input events

#### Scenario: Only one window remains
- **WHEN** all follower windows are removed and the master remains connected
- **THEN** synchronization remains enabled on the master so a later connected window can join automatically

### Requirement: Target failures are isolated
The system SHALL fan out each event without allowing a failure from one follower to suppress delivery to the master or other followers. Dispatch SHALL occur on the UI thread using stable guarded window references, and synchronization SHALL not create a second network or input protocol.

#### Scenario: One follower send fails
- **WHEN** sending an event to one eligible follower returns failure
- **THEN** delivery is still attempted for every other target and the UI remains responsive

#### Scenario: Existing protocol is reused
- **WHEN** a synchronized event is delivered to a target
- **THEN** it is serialized through that target window's existing viewer handle and `fsremote_stream_send_input` path
