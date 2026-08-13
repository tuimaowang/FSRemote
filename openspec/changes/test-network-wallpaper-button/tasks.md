## 1. Wallpaper platform service

- [x] 1.1 Add deterministic top-level image candidate discovery for the fixed shared directory, with supported-extension filtering and decodability checks.
- [x] 1.2 Convert the selected shared image into a stable local BMP cache and apply it through the Windows wallpaper API with structured success and error results.
- [x] 1.3 Register the new service in the application build and keep unsupported-platform behavior explicit.

## 2. Settings test interface

- [x] 2.1 Add a dedicated wallpaper test card and scroll-aware button layout to Settings > General, reflowing the following cards without overlap.
- [x] 2.2 Connect the button to the wallpaper service, prevent repeated clicks during processing, and show selected-image success or actionable failure feedback.
- [x] 2.3 Ensure the button is visible only in the General Settings viewport and remains correctly positioned during resize and scroll updates.

## 3. Verification

- [x] 3.1 Add targeted automated tests for candidate ordering, extension filtering, empty directories, and damaged-image skipping without mutating the real desktop.
- [x] 3.2 Build the application and test target, run relevant regression tests, and check the diff for formatting errors.
- [x] 3.3 Run strict OpenSpec validation and confirm no WJY change-log artifact was created.

## 4. Automatic wallpaper rotation

- [x] 4.1 Persist a default-off wallpaper rotation switch and a validated one-minute default interval in application settings.
- [x] 4.2 Extend the wallpaper service with deterministic next-image selection, damaged-image skipping, and wraparound while preserving the stable local BMP path.
- [x] 4.3 Replace the manual test button with a scroll-aware switch and minute editor that follow the existing General Settings visual language.
- [x] 4.4 Run rotation attempts in the background, apply one image immediately when enabled, prevent overlapping work, and restart scheduling after interval edits.
- [x] 4.5 Add next-image and wraparound tests, build the application and wallpaper tests, and run strict OpenSpec validation without creating a WJY Markdown log.

## 5. Fixed wallpaper number overlay

- [x] 5.1 Add a reusable image-composition function that draws a fixed `99` in the upper-right with proportional sizing, margin, and high-contrast outline.
- [x] 5.2 Apply the composed pixels before writing `current.bmp`, without modifying the shared source image or changing rotation order.
- [x] 5.3 Add focused composition tests, rebuild the Release application and wallpaper test target, and run strict OpenSpec validation without modifying the WJY Markdown log.

## 6. Target device-name wallpaper overlay

- [x] 6.1 Replace the fixed-number composition API with a supplied device-name API that trims input, scales or elides long names, and preserves the outlined upper-right layout.
- [x] 6.2 Read the current target machine name through `DeviceInfoService` on the background rotation task and pass it into local BMP composition without duplicating shared source images.
- [x] 6.3 Update focused tests for name-dependent pixels and source preservation, rebuild Release targets, and run strict OpenSpec validation without modifying the WJY Markdown log.
