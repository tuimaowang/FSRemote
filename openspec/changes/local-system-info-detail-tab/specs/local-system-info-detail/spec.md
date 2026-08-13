## ADDED Requirements

### Requirement: Local tab appears before script tab and is selected by default
The device detail navigation SHALL display a local-system tab immediately to the left of the script tab, SHALL use it as the initial selected detail tab, and SHALL show the local-system page when selected.

#### Scenario: Application opens the device detail area
- **WHEN** the application starts with at least one configured device and displays the device detail area
- **THEN** the local-system tab is selected and its information page begins loading

#### Scenario: User opens local page
- **WHEN** the user clicks the local-system tab
- **THEN** the right detail area switches from the remote-device content to the local-system information page

### Requirement: Local page displays system information
The local-system page SHALL display the computer name, operating system, CPU model or architecture fallback, logical processor count, total physical memory, system-disk usage, IPv4 address, MAC address, and default gateway using read-only local data.

#### Scenario: System information is available
- **WHEN** the local-system page is opened and the operating system returns the requested values
- **THEN** the page displays the current values in labeled information fields

#### Scenario: A system field is unavailable
- **WHEN** one system query returns no value
- **THEN** that field displays an unavailable placeholder without hiding the remaining information

#### Scenario: System disk information is available
- **WHEN** the local-system page opens and the operating system reports a valid root storage volume
- **THEN** the CPU card footer displays the system-disk root, used capacity, total capacity, and usage percentage instead of a generic load description

### Requirement: Local page displays CPU usage
On Windows, the local-system page SHALL sample whole-system CPU usage using the Processor Information `_Total` Processor Utility performance-counter definition used by Task Manager, and SHALL display a percentage and ring-shaped visual progress indicator limited to 0 through 100.

#### Scenario: First CPU sample establishes baseline
- **WHEN** the user first enters the local-system page and no previous sample exists
- **THEN** the CPU area displays a sampling state until a second sample is available

#### Scenario: CPU sample is refreshed
- **WHEN** the local-system page remains selected for at least one sampling interval
- **THEN** the displayed CPU percentage counts up or down and the ring-shaped progress indicator sweeps smoothly from the current displayed value to the latest sample

### Requirement: Local page displays GPU and memory usage rings
The local-system page SHALL display CPU, GPU, and physical-memory usage as three horizontally arranged ring indicators with independent percentages and synchronized animations. On Windows, whole-GPU usage SHALL sum all process instances belonging to the same physical GPU engine and then select the busiest physical engine, rather than selecting the largest individual process instance.

#### Scenario: GPU and memory samples are available
- **WHEN** the local-system page remains selected and Windows returns valid GPU-engine and physical-memory load data
- **THEN** the GPU and memory percentages and their ring indicators animate independently to the latest values

#### Scenario: Multiple processes use the same GPU engine
- **WHEN** two or more GPU Engine counter instances share the same adapter LUID, physical index, and engine index
- **THEN** their valid utilization values are added for that physical engine before the busiest engine is selected and the final result is limited to 100 percent

#### Scenario: GPU sampling is unavailable
- **WHEN** the operating system or graphics driver does not expose a valid GPU Engine performance counter
- **THEN** the GPU column displays an unavailable or sampling placeholder while CPU and memory continue updating

### Requirement: CPU sampling follows page visibility
The main controller SHALL start periodic CPU, GPU, and memory sampling when the local-system page is selected and SHALL stop periodic sampling and release GPU counter resources after the user leaves that page.

#### Scenario: User leaves local page
- **WHEN** the user switches from the local-system tab to another detail or application page
- **THEN** periodic CPU sampling stops and no hidden local-page repaint is requested
