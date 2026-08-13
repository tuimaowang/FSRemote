## ADDED Requirements

### Requirement: Tests are disabled for normal configuration
The CMake project SHALL provide a top-level test-build option that defaults to disabled, and SHALL create project test targets only when that option and the corresponding test-category option are enabled.

#### Scenario: Existing build directory is reconfigured
- **WHEN** a build directory whose individual test-category cache values are enabled receives the new top-level option with its default value
- **THEN** project test targets are not created because the top-level test-build option is disabled

#### Scenario: Developer enables project tests
- **WHEN** the developer explicitly enables the top-level test-build option
- **THEN** test targets are created according to the existing individual category options

### Requirement: Test executables are excluded from all
Every project test executable SHALL be excluded from the default `all` build target while remaining available as an explicit build target.

#### Scenario: User builds the default project target
- **WHEN** Qt Creator or CMake builds `all`
- **THEN** no project `*_tests` executable is compiled solely because it was registered as a test

#### Scenario: User builds one test target
- **WHEN** tests are enabled and the user explicitly selects a specific test executable target
- **THEN** that test target and its required dependencies are compiled normally

### Requirement: Production targets remain unchanged
The default-build optimization SHALL NOT remove or disable the FSRemote application, updater, stream DLL, session protocol library, or their required production dependencies.

#### Scenario: Tests are disabled
- **WHEN** the project is configured with the default test-build option
- **THEN** all production targets required by FSRemote remain available for normal build
