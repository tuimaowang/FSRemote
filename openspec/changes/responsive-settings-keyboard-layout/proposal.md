## Why

The main control window is still largely fixed-coordinate and several header/settings details remain visually noisy. This change makes the shell more usable at different window sizes, removes redundant status/header chrome, and adds a dedicated keyboard-shortcut settings view for remote-window workflow controls.

## What Changes

- Remove the device-detail status capsule and adjacent device-name header display.
- Remove the oversized Settings page title.
- Make the Settings page respond to sidebar collapse by shifting content to the left and widening like the device detail area.
- Allow the main window to be resized, with content stretching while the left-sidebar proportion remains stable.
- Redesign batch device add input to accept multiple IP/subnet patterns.
- Refresh Settings option icons with a new consistent icon treatment.
- Add a Keyboard settings page with configurable shortcut rows for remote window fullscreen, tiling, closing the topmost remote window, and closing all remote windows.
- Add titlebar local-device name and local IPv4 display to the left of the Settings icon.
- Convert the device-detail lower content into tabs for configuration files and script logs, with responsive collapsed-sidebar behavior.

## Capabilities

### New Capabilities
- `responsive-remote-control-shell`: Main shell layout, responsive settings/device detail behavior, keyboard shortcut settings, titlebar local identity, and remote-window shortcut actions.

## Impact

- Affected Qt UI code: `src/ui/DeviceGrid.cpp`, `src/ui/DeviceGrid.h`, and possibly `src/ui/RemoteDesktopWindow.*`.
- Affected persistence: shortcut settings may need new `AppSettings` keys; batch-add input may store multi-pattern text.
- Affected resources: Settings icons may use new generated/hand-coded drawing helpers or new SVGs under existing resource paths.
- No new external dependencies are planned.
