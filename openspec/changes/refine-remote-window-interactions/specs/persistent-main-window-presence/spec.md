## ADDED Requirements

### Requirement: Tray left click toggles main-window minimization
The application SHALL use a single left click on the tray icon to toggle the main window between restored and minimized states and SHALL NOT require a double click to minimize it.

#### Scenario: Main window is visible
- **WHEN** the user single-clicks the tray icon while the main window is restored
- **THEN** the main window is minimized

#### Scenario: Main window is minimized
- **WHEN** the user single-clicks the tray icon while the main window is minimized
- **THEN** the main window is restored, raised, and activated

### Requirement: Close and tray minimization retain the taskbar entry
The application SHALL keep its Windows taskbar entry when the user minimizes through the tray or closes the main window without explicitly quitting.

#### Scenario: User clicks the main-window close control
- **WHEN** the tray is available and the application is not executing an explicit quit
- **THEN** the close request minimizes the main window and the taskbar entry remains visible

### Requirement: A lone main window toggles from the taskbar
The application SHALL allow a single click on the Windows taskbar button to minimize or restore the main window when no other ordinary application window is visible.

#### Scenario: Lone restored main window is clicked on the taskbar
- **WHEN** the restored main window is the only visible ordinary application window and the user single-clicks its taskbar button
- **THEN** the main window is minimized and its taskbar entry remains visible

#### Scenario: Lone minimized main window is clicked on the taskbar
- **WHEN** the minimized main window has no other visible ordinary application window and the user single-clicks its taskbar button
- **THEN** the main window is restored, raised, and activated

#### Scenario: Another ordinary application window is visible
- **WHEN** a remote window or dialog is visible alongside the main window
- **THEN** taskbar activation retains the native Windows grouped-window behavior
