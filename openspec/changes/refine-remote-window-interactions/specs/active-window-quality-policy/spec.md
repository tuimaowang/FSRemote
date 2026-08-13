## ADDED Requirements

### Requirement: Visible quality follows control focus and full-screen state
The controller SHALL request high quality for every full-screen remote window and for the active visible remote window, and SHALL request smooth quality for other visible remote windows.

#### Scenario: User activates a normal remote window
- **WHEN** the active remote window changes and no safety profile applies
- **THEN** the newly active window requests high quality and other visible non-full-screen windows request smooth quality

#### Scenario: Remote window enters full screen
- **WHEN** a visible remote window enters full-screen state
- **THEN** it requests high quality regardless of which normal remote window was most recently active

### Requirement: Safety profiles override focus quality
The controller SHALL retain minimized/hidden and software-fallback safety profiles above active-window and full-screen quality selection.

#### Scenario: Active window is minimized
- **WHEN** the current active remote window becomes minimized or hidden
- **THEN** it uses the existing background safety profile instead of high quality

