## ADDED Requirements

### Requirement: Strict update version ordering
The system SHALL offer an update only when the normalized remote semantic version is strictly greater than the local semantic version.

#### Scenario: Remote version is newer
- **WHEN** the local version is `1.2.3` and the remote version is `1.2.4`
- **THEN** the system reports an available update

#### Scenario: Remote version is older
- **WHEN** the local version is `1.2.4` and the remote version is `1.2.3`
- **THEN** the system does not offer or install the remote version

### Requirement: Safe local staging
The main application SHALL copy the complete runtime payload to a version-specific local staging directory and validate required files before changing the installed application.

#### Scenario: Staging copy fails
- **WHEN** a shared payload file cannot be copied or a required file is absent
- **THEN** the system aborts the update without modifying the installed application

### Requirement: Out-of-process installation
The system SHALL execute file replacement from an updater process that does not load Qt or other runtime files from the target installation directory.

#### Scenario: Main process is still running
- **WHEN** the updater starts while the specified FSRemote process is active
- **THEN** the updater waits for that process to exit before replacing any installed file

### Requirement: Transactional rollback
The updater SHALL back up every existing target file before replacement and SHALL restore the previous installation if installation does not complete successfully.

#### Scenario: A file replacement fails
- **WHEN** replacement of any task file fails after earlier files were installed
- **THEN** the updater restores backed-up files and removes newly introduced files from the failed update

### Requirement: Automatic restart
The updater SHALL start FSRemote after a successful installation and SHALL restart the previous installation after a successful rollback.

#### Scenario: Installation succeeds
- **WHEN** all task files are installed and validated
- **THEN** the updater starts the updated `FSRemote.exe` with the previous and current version arguments

#### Scenario: Installation is rolled back
- **WHEN** installation fails and rollback completes
- **THEN** the updater starts the restored `FSRemote.exe` with a rollback result argument

### Requirement: Stable updater protocol
The updater SHALL accept only a supported versioned task schema and SHALL reject unsafe or malformed paths before modifying the installation.

#### Scenario: Unsupported task schema
- **WHEN** the task schema version is not supported
- **THEN** the updater exits without modifying the installation and records an error

### Requirement: Manual checks remain available
The system SHALL allow an explicit manual update check even when periodic automatic checks are disabled.

#### Scenario: Automatic checking is disabled
- **WHEN** the user clicks check for updates while automatic checking is disabled
- **THEN** the system performs a one-time remote version check
