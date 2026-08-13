## ADDED Requirements

### Requirement: Settings exposes automatic shared-wallpaper rotation
The system SHALL display a wallpaper rotation switch and an editable whole-minute interval on the General Settings page, SHALL identify the fixed shared image directory, and SHALL persist both settings.

#### Scenario: User opens General Settings
- **WHEN** the user opens the General tab of Settings
- **THEN** the system displays a wallpaper rotation card for `\\192.168.1.100\广告部工具\远程软件_桌面`
- **AND** the switch is off by default when no saved setting exists
- **AND** the interval defaults to one minute when no valid saved interval exists

#### Scenario: User enables rotation
- **WHEN** the user turns on wallpaper rotation
- **THEN** the system persists the enabled state
- **AND** immediately starts one wallpaper rotation attempt
- **AND** schedules later attempts at the configured interval

#### Scenario: User disables rotation
- **WHEN** the user turns off wallpaper rotation
- **THEN** the system persists the disabled state
- **AND** stops scheduling later wallpaper changes

#### Scenario: User changes the interval
- **WHEN** the user enters a valid minute interval
- **THEN** the system persists the value
- **AND** an enabled rotation timer restarts from the time of the edit using the new interval

#### Scenario: User leaves General Settings
- **WHEN** the user switches to another Settings tab or leaves Settings
- **THEN** the real interval editor is hidden and cannot intercept input over other pages

### Requirement: Rotation deterministically selects the next usable image
The system SHALL inspect supported top-level image files in case-insensitive filename order and SHALL select the next decodable file after the last successfully applied source, wrapping to the beginning when required.

#### Scenario: Multiple supported images exist
- **WHEN** rotation has no previously applied source and the shared directory contains multiple decodable supported image files
- **THEN** the system selects the first image in case-insensitive filename order

#### Scenario: A previous image was applied
- **WHEN** another decodable image follows the last successfully applied source
- **THEN** the system selects that following image

#### Scenario: Rotation reaches the end
- **WHEN** the last successfully applied source is the final decodable candidate
- **THEN** the system wraps and selects the first decodable candidate

#### Scenario: An earlier candidate is damaged
- **WHEN** the next supported-extension file cannot be decoded and a later candidate can be decoded
- **THEN** the system skips the damaged file and selects the later valid image

### Requirement: Selected image is applied through a stable local cache
The system SHALL flatten the current target machine's Windows device name into the selected image's upper-right pixels, convert the composed image to a local BMP cache, and ask Windows to set that BMP as the current user's desktop wallpaper.

#### Scenario: Wallpaper application succeeds
- **WHEN** a valid shared image is decoded, cached, and accepted by the Windows wallpaper API
- **THEN** the current Windows desktop uses the cached image and the system reports the selected source filename

#### Scenario: Device-name overlay is composed
- **WHEN** a valid shared image is selected for wallpaper application
- **THEN** the cached image contains the current target machine's device name in its upper-right area
- **AND** the label uses a light fill with a dark outline so it remains visible over light and dark image content
- **AND** an overlong device name is reduced or elided so it remains inside the available image width
- **AND** the label is part of the cached image pixels rather than a separate desktop window or overlay

#### Scenario: Shared source remains unchanged
- **WHEN** the system composes the target device name into the wallpaper cache
- **THEN** it does not modify or overwrite the selected shared image file

### Requirement: Failures are safe and visible
The system SHALL report a user-visible failure and SHALL NOT call the Windows wallpaper API when the directory is inaccessible, no usable image exists, decoding fails for every candidate, or local cache creation fails.

#### Scenario: Shared directory is empty
- **WHEN** a rotation attempt runs while the shared directory contains no usable image
- **THEN** the system explains that no supported image was found and leaves the current wallpaper unchanged

#### Scenario: Shared directory is unavailable
- **WHEN** a rotation attempt runs while the shared directory cannot be accessed
- **THEN** the system reports the inaccessible directory and leaves the current wallpaper unchanged

#### Scenario: Local cache cannot be written
- **WHEN** a valid source image exists but the local BMP cache cannot be created
- **THEN** the system reports the cache failure and does not request a wallpaper change

#### Scenario: A timed attempt fails
- **WHEN** an automatic rotation attempt fails
- **THEN** the system keeps rotation enabled
- **AND** retries at the next configured interval

### Requirement: Automatic rotation remains responsive
The system SHALL perform shared-directory access, decoding, caching, and wallpaper application outside the UI thread and SHALL allow at most one rotation attempt at a time.

#### Scenario: A shared-directory attempt is slow
- **WHEN** an automatic rotation attempt is still running when another timer event occurs
- **THEN** the settings UI remains responsive
- **AND** the overlapping timer event does not start another attempt
