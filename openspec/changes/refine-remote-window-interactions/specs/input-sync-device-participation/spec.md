## ADDED Requirements

### Requirement: Followers can disable only their own synchronization
The controller SHALL allow a follower endpoint to leave the active input synchronization group without disabling synchronization for the master or other followers.

#### Scenario: Follower button is clicked once
- **WHEN** the user clicks the synchronization control of a follower window
- **THEN** held synchronized input is released for that endpoint, the endpoint stops receiving broadcasts, and the current master remains unchanged

#### Scenario: Excluded endpoint is clicked again
- **WHEN** the user clicks the synchronization control of a locally excluded eligible window
- **THEN** held group input is safely released and that window becomes the unique master

### Requirement: Synchronization roles are visually explicit
The remote title bar SHALL distinguish global off, master, follower, and locally excluded states and SHALL describe the next click action in each state.

#### Scenario: Synchronization role changes
- **WHEN** an endpoint changes synchronization role
- **THEN** its title-bar control updates its label, color, icon, and tooltip without requiring the remote window to reopen

