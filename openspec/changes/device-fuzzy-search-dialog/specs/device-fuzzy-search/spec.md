## ADDED Requirements

### Requirement: Search page opens from tab or Ctrl+F
The main controller SHALL show an embedded device search page to the right of the configuration tab when the user clicks the search tab or presses Ctrl+F while the main window or one of its child controls has focus.

#### Scenario: User invokes search
- **WHEN** the user clicks the search tab or presses Ctrl+F in the main controller
- **THEN** the right detail area shows device name, group name, MAC, four IP segment inputs, and a result list without opening a separate dialog

### Requirement: Device fields support fuzzy filtering
The embedded search page SHALL filter the current device snapshot in real time by case-insensitive partial device name, group name, and MAC text, and SHALL combine non-empty field conditions with logical AND.

#### Scenario: Partial name and group are entered
- **WHEN** the user enters partial device and group text
- **THEN** the result list contains only devices whose corresponding fields contain both values

### Requirement: IP search supports positional and remembered segment input
The search page SHALL provide four IPv4 segment inputs limited to values from 0 through 255 and SHALL match either the filled positional segments or the ordered sequence of non-empty remembered segments.

#### Scenario: User remembers the last two segments
- **WHEN** the user enters `2` and `9` as consecutive remembered IP segments
- **THEN** a device with IP `192.168.2.9` is included in the result list

#### Scenario: User enters one broad segment
- **WHEN** the user enters only `2`
- **THEN** every otherwise matching device whose IP contains that segment remains visible as a candidate

### Requirement: IP Enter navigation advances focus
The search page SHALL move focus to the next IP segment when Enter is pressed in the first three segment inputs and SHALL execute filtering when Enter is pressed in the fourth segment.

#### Scenario: User enters IP segments by keyboard
- **WHEN** the user types a segment and presses Enter
- **THEN** focus advances to the following segment until the final segment triggers search

### Requirement: Search results populate complete information
The search page SHALL populate all search inputs with complete device information when exactly one candidate remains or when the user selects a candidate row.

#### Scenario: Filtering leaves one device
- **WHEN** the current criteria produce exactly one result
- **THEN** the device name, group, MAC, and all four IP fields display that device's complete information

#### Scenario: User edits an auto-populated field
- **WHEN** one device has already been auto-populated and the user deletes or changes any search field while that same device remains the only result
- **THEN** the user's edit remains unchanged and the search page does not immediately populate the deleted value again

#### Scenario: User selects one of multiple results
- **WHEN** the user clicks a result row
- **THEN** all fields display the selected device's complete information while the candidate list remains available

### Requirement: Activated result locates the main device
The main controller SHALL resolve an activated result by stable device ID, expand its group if necessary, select it in the device list, and display its detail page.

#### Scenario: User double-clicks a result
- **WHEN** the result still exists and the user double-clicks it
- **THEN** the main controller selects and reveals that device and switches from the search page to the configuration page
