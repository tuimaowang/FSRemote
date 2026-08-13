## ADDED Requirements

### Requirement: Local page uses one resource card
The local-system page SHALL display CPU, GPU, memory, and disks inside one continuous card and SHALL NOT display a separate local identity, operating-system, or network-information card.

#### Scenario: User opens local page
- **WHEN** the local-system page is visible
- **THEN** one card contains the CPU, GPU, memory, and disk sections in vertical order
- **AND** computer name, Windows version, IPv4, MAC, subnet mask, and default gateway are absent

### Requirement: CPU and GPU rows display hardware summaries
The resource card SHALL display the CPU model, logical processor count, architecture, preferred GPU model, and dedicated GPU memory when those values are available, while retaining the existing live CPU and whole-GPU percentage rings.

#### Scenario: Hardware summaries are available
- **WHEN** Windows returns CPU and hardware GPU information
- **THEN** the CPU row displays model, logical processor count, and architecture
- **AND** the GPU row displays adapter model and dedicated video-memory capacity

#### Scenario: GPU hardware summary is unavailable
- **WHEN** no hardware adapter can be enumerated
- **THEN** the GPU row displays an unavailable placeholder while its live usage sampler continues independently

### Requirement: Memory row displays module and slot details
The memory row SHALL display live memory usage, used and total capacity, installed module count, module-capacity composition, used and total slot counts when known, memory type, and configured transfer rate when available.

#### Scenario: Matching memory modules are installed
- **WHEN** two installed modules each report 16 GB, DDR5, 5600 MT/s and the physical array reports four slots
- **THEN** the memory row displays `2 × 16 GB`, `已使用 2 / 4 个插槽`, and `DDR5 · 5600 MT/s`

#### Scenario: Mixed memory modules are installed
- **WHEN** installed modules report different capacities, types, or configured rates
- **THEN** the memory row preserves the individual capacity composition and marks differing type or rate information as mixed rather than inventing one common value

#### Scenario: Total slot count is unavailable
- **WHEN** installed modules are readable but the physical array does not report a valid slot count
- **THEN** the memory row displays the installed module count without a fabricated total-slot value

#### Scenario: Memory hardware query fails
- **WHEN** WMI is unavailable, times out, or returns no usable module objects
- **THEN** the memory row continues to display live usage and total physical memory with unavailable placeholders for module-specific fields

### Requirement: Disk section follows file-manager capacity presentation
The disk section SHALL display every readable local fixed or removable volume as an independent row containing its root label, proportional capacity bar, used and total capacity, and available capacity.

#### Scenario: Multiple disks are available
- **WHEN** Windows reports readable C:, D:, and E: volumes
- **THEN** the card displays three disk rows in stable root-path order

#### Scenario: Disk usage reaches warning levels
- **WHEN** a disk reaches the existing warning or critical usage threshold
- **THEN** its capacity bar uses the corresponding warning or critical color while retaining readable capacity text
