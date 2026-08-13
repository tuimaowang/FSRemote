## Context

The application already provides a hand-painted Settings > General page with real child controls positioned over painted cards. The requested shared directory is a UNC path and may be temporarily unavailable, and Windows wallpaper APIs are more reliable with a stable local bitmap than with a network image whose connection can disappear after the call.

## Goals / Non-Goals

**Goals:**

- Add a persisted, scroll-aware rotation switch and minute input to the General Settings page.
- Find the next decodable supported image in deterministic filename order, with wraparound.
- Materialize a stable local BMP and set it as the current user's Windows wallpaper.
- Flatten the current target machine's Windows device name into the upper-right of each cached wallpaper without modifying shared source images.
- Report actionable errors and preserve the existing wallpaper on all pre-application failures.
- Keep discovery logic independently testable without changing the real desktop during automated tests, and keep network access off the UI thread.

**Non-Goals:**

- Random ordering or sub-minute scheduling.
- Applying wallpaper to selected or remote devices.
- Recursing into subdirectories or editing images in the shared directory.
- Adding a user-editable source path in this test iteration.

## Decisions

1. Introduce a small `DesktopWallpaperService` in the platform layer. It owns the fixed UNC source, supported-file discovery, decoding, BMP cache creation, and the Windows API call. Keeping this logic outside `DeviceGrid` prevents UI code from owning filesystem and operating-system details.
2. Sort top-level candidate files by filename, case-insensitively, then begin after the last successful source and try each candidate once, wrapping as needed. This provides deterministic rotation while allowing damaged files to be skipped.
3. Decode through Qt and save a local `current.bmp` under `QStandardPaths::AppLocalDataLocation/wallpaper`. This normalizes PNG/JPEG/WebP inputs and avoids asking Windows to retain a UNC dependency.
4. Apply the BMP through `SystemParametersInfoW(SPI_SETDESKWALLPAPER, ..., SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)`. A non-Windows build returns a clear unsupported-platform result.
5. Keep the dedicated Settings card before the optional update-publishing card. Its switch is hand-painted like existing settings switches, while the minute interval uses a real validated `QLineEdit` that follows the same scroll clipping rules as other General-page controls.
6. Store `desktopWallpaperRotationEnabled` with a default of `false` and `desktopWallpaperRotationIntervalMinutes` with a default of `1` in `AppSettings`. Invalid persisted or edited intervals normalize to the supported 1-1440 minute range.
7. Own a coarse timer in `DeviceGrid`. Enabling starts the timer and launches one immediate attempt; interval edits restart the timer; disabling stops it. Each attempt runs through the existing joinable background-task mechanism and uses an in-progress guard to prevent overlap.
8. Automated tests cover candidate ordering, extension filtering, empty directories, damaged-image skipping, next-image selection, and wraparound. They do not invoke the final Windows wallpaper mutation.
9. Read the target machine name through the existing `DeviceInfoService` on the wallpaper worker thread, matching the Windows computer name already reported during device discovery. Pass that value into the image-composition function so the controller's name is never substituted for the wallpaper target.
10. Compose the trimmed device name with `QPainterPath` after decoding and before BMP serialization. Size and margin scale from the image's shorter side with safe minimum and maximum bounds; overlong names shrink and may be middle-elided to remain inside the available width. A white fill and dark rounded outline keep the text readable without introducing a separate desktop overlay process.

## Risks / Trade-offs

- [The UNC share can block during access] → Run each attempt on a joinable background thread and never start a second attempt while one is active.
- [The shared directory is currently empty] → The button reports that no usable image exists and leaves the wallpaper unchanged.
- [Windows style preferences may crop the image differently] → This iteration preserves the user's existing wallpaper style and only changes the image path.
- [A valid image can fail to cache or apply] → Return the specific failing stage, retain the last successful source, and retry at the next interval without disabling the feature.
- [The device name could disappear over varied artwork] → Draw a dark outline around light text and size it proportionally to the decoded image.
- [A long device name could extend beyond the image] → Reduce the font size down to a safe bound and middle-elide only when the full name still cannot fit.

## Migration Plan

The new settings keys are additive and have safe defaults. Rolling back leaves unused QSettings values without affecting older builds.

## Open Questions

Remote-device scope and image fit style remain future decisions; this change affects only the local current-user desktop and preserves the current Windows fit preference.
