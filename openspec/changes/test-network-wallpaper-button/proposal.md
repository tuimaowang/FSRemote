## Why

The manual shared-wallpaper test has validated the end-to-end Windows wallpaper path. The Settings action now needs to become an opt-in automatic rotation feature so the current computer can cycle through the shared images without repeated manual clicks.

## What Changes

- Replace the General Settings test button with a persisted switch that is off by default.
- Add a persisted rotation interval in whole minutes, defaulting to one minute and editable while rotation is enabled.
- When enabled, immediately apply the next usable image from `\\192.168.1.100\广告部工具\远程软件_桌面`, then continue on the configured timer.
- Scan supported image files in deterministic filename order and advance to the next decodable image, wrapping at the end.
- Copy and convert the selected image to a local BMP cache before calling the Windows wallpaper API, so the desktop does not depend on a live network file handle.
- Flatten the current target machine's Windows device name into the upper-right image pixels before writing the local BMP, using a high-contrast outlined style while leaving the shared source file unchanged.
- Run network discovery and image application away from the UI thread, prevent overlapping rotations, and retry transient failures on the next interval.
- Keep remote-device wallpaper distribution out of this change.

## Capabilities

### New Capabilities

- `shared-wallpaper-test`: Opt-in automatic rotation of the current machine's Windows desktop wallpaper from a shared folder.

### Modified Capabilities

None.

## Impact

- Adds a small Windows-specific platform service for shared image discovery, local cache conversion, and wallpaper application.
- Replaces the existing button with a hand-painted switch and a real minute input in the Settings > General page.
- Persists enablement and interval through the existing application settings service.
- Adds targeted tests for deterministic next-image selection and wraparound; no new third-party dependency is required.
- Adds image-composition coverage confirming that the supplied target device name is rendered only into the upper-right area, different names produce different cached pixels, and the source image remains unchanged.
