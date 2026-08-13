## ADDED Requirements

### Requirement: Refactor preserves externally observable behavior
The refactor MUST preserve existing user-visible workflows, visual states, remote protocols, persisted data compatibility, DLL fallback behavior, and device-command semantics unless a separate change explicitly modifies them.

#### Scenario: Existing workflow is exercised after extraction
- **WHEN** a user performs device management, remote control, batch commands, settings, script execution, synchronization, or application shutdown after the refactor
- **THEN** the workflow produces the same externally observable result as before the refactor

### Requirement: Production duplication is removed
Each completed migration slice SHALL remove the superseded production implementation, mirrored state, repeated validation, or obsolete commented implementation that the new boundary replaces.

#### Scenario: A responsibility is extracted
- **WHEN** all callers of a migrated responsibility use the new collaborator
- **THEN** the former duplicate implementation and unused state are deleted rather than retained as parallel code

### Requirement: Code reduction does not rely on obscurity
Production-code reduction MUST NOT be achieved through behavior-hiding macros, overly generic callback frameworks, compressed naming, or removal of safety checks and compatibility handling.

#### Scenario: Common action logic is consolidated
- **WHEN** repeated validation or routing is replaced by a shared implementation
- **THEN** action-specific eligibility, error handling, lifecycle ownership, and compatibility behavior remain explicit and reviewable

### Requirement: Extracted logic is verified
Pure catalog, snapshot, target-selection, and state-transition logic SHALL have focused automated tests, and each implementation slice SHALL build successfully before the next high-risk slice proceeds.

#### Scenario: A refactor slice completes
- **WHEN** a catalog, repository, action, window, script, layout, or task-lifecycle slice is marked complete
- **THEN** its focused tests and the relevant existing tests pass, or the task remains incomplete with the failure documented
