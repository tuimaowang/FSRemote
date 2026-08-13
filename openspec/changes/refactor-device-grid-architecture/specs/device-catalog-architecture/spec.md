## ADDED Requirements

### Requirement: Single authoritative device catalog
The system SHALL maintain device records, group records, stable identifiers, snapshot metadata, tombstones, and local expansion state in one authoritative catalog rather than anonymous UI globals.

#### Scenario: Multiple consumers read the same device data
- **WHEN** the device grid, synchronization layer, or another UI consumer requests device information
- **THEN** each consumer receives data derived from the same catalog state

### Requirement: Stable identity operations
The system MUST use stable device and group identifiers for cross-event, cross-thread, persistence, and synchronization operations; numeric indexes MAY only be used as transient presentation positions.

#### Scenario: Device ordering changes during an asynchronous operation
- **WHEN** a device list reorder or synchronized snapshot changes vector positions before an asynchronous result returns
- **THEN** the result is applied to the originally targeted stable device and not the device currently occupying the old index

### Requirement: Compatibility-preserving snapshot flow
The catalog persistence boundary SHALL preserve the existing normalized device-list schema, revision, updated metadata, tombstones, stable IDs, group IDs, and local-only group expansion behavior.

#### Scenario: Existing device JSON is loaded and saved
- **WHEN** an existing compatible device snapshot is loaded and written without a user-visible data change
- **THEN** normalization preserves all compatibility-relevant entities and metadata semantics

### Requirement: Synchronized updates remain single-source
The system SHALL apply synchronized snapshots through the catalog boundary and SHALL prevent remote snapshot application from recursively submitting an equivalent local change.

#### Scenario: A newer shared snapshot arrives
- **WHEN** `DeviceListSyncService` emits a newer normalized snapshot
- **THEN** the catalog updates once, preserves local-only expansion state, refreshes consumers, and does not create a synchronization loop
