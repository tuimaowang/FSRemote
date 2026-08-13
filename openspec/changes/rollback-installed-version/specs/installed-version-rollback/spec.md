## ADDED Requirements

### Requirement: Historical rollback versions are discoverable
The system SHALL enumerate immutable shared releases and present only normalized semantic versions that are strictly lower than the currently installed version and have the required release identity and entry files.

#### Scenario: Valid historical releases exist
- **WHEN** the user opens General Settings and the shared release directory contains valid lower versions
- **THEN** the rollback selector lists those versions from newest to oldest

#### Scenario: No valid historical release exists
- **WHEN** no lower complete release is available or the shared directory cannot be read
- **THEN** the selector shows that no rollback version is available and rollback cannot be started

### Requirement: User explicitly selects and confirms rollback
The system SHALL require a selected historical version and an explicit confirmation before preparing a rollback.

#### Scenario: User confirms the selected version
- **WHEN** the user selects a historical version, clicks rollback, and confirms the warning
- **THEN** the system begins preparing that exact version

#### Scenario: User cancels confirmation
- **WHEN** the user cancels the rollback confirmation
- **THEN** the installed application remains running and unchanged

### Requirement: Rollback uses verified transactional installation
The system SHALL stage and validate the complete selected runtime payload before exiting the main application and SHALL use the independent updater to transactionally replace the installation.

#### Scenario: Selected release is valid
- **WHEN** all required files for the selected version are copied and validated and the updater task starts successfully
- **THEN** the main application exits so the updater can install the selected version

#### Scenario: Preparation fails
- **WHEN** the selected version disappears, is incomplete, cannot be copied, or the updater cannot be started
- **THEN** the current application remains running and its installed files are not modified

#### Scenario: Replacement fails after exit
- **WHEN** the updater cannot finish replacing the installed files
- **THEN** the updater restores the previous installation according to the existing transactional recovery behavior

### Requirement: Application restarts after rollback
The updater SHALL restart FSRemote after successfully installing the selected historical version and SHALL restart the restored current installation if rollback installation fails and recovery succeeds.

#### Scenario: Rollback installation succeeds
- **WHEN** the updater finishes installing the selected historical version
- **THEN** it starts the installed `FSRemote.exe` automatically

#### Scenario: Rollback installation is recovered
- **WHEN** installation fails and the updater restores the prior installation
- **THEN** it starts the restored `FSRemote.exe` with the existing failure result argument

### Requirement: Existing update behavior remains separate
The system SHALL keep automatic update checks, manual upgrades, publishing, remote protocols, and user data behavior unchanged by the rollback selector.

#### Scenario: A newer shared version is available
- **WHEN** the normal update service checks the remote latest-version marker
- **THEN** it continues to offer upgrades only under the existing strict newer-version rule
