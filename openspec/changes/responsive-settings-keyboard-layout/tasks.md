## 1. Responsive Shell Layout

- [x] 1.1 Replace the fixed main shell size with resizable minimum/initial sizing.
- [x] 1.2 Add responsive content/titlebar helpers and keep titlebar controls aligned to the right edge.
- [x] 1.3 Shift Settings content left on sidebar collapse and stretch Settings/detail content with window size.

## 2. Settings Page

- [x] 2.1 Remove the large Settings page title and redraw compact Settings tabs.
- [x] 2.2 Replace old Settings option SVG icons with new painter-drawn option icons.
- [x] 2.3 Refactor the batch-add input to accept multiple wildcard subnet patterns and de-duplicate scan IPs.
- [x] 2.4 Add the Keyboard tab view with Ctrl+D, Ctrl+P, Ctrl+W, and Ctrl+F4 shortcut rows.
- [x] 2.5 Change the close-topmost default to Ctrl+W while preserving persisted user customizations.

## 3. Device Detail

- [x] 3.1 Remove the device-detail status capsule and adjacent device name display.
- [x] 3.2 Add Configuration File and Script Log tabs in device detail and route editor/log visibility through the active tab.
- [x] 3.3 Make the detail tab panels stretch and shift consistently with sidebar collapse.

## 4. Remote Window Commands

- [x] 4.1 Add remote-window ordering and close topmost/all helpers.
- [x] 4.2 Add fullscreen and tile/restore shortcut actions for opened remote windows.
- [x] 4.3 Forward remote-window shortcut keys from focused remote windows to the shell actions.
- [x] 4.4 Prevent local or controlled-side modifier keys from remaining held when shortcut handling changes the active keyboard-hook target.

## 5. Verification

- [x] 5.1 Run OpenSpec apply/status checks and static code checks without compiling.
- [x] 5.2 Review modifier ownership transitions and run focused static checks without compiling.
